// The game's side of WowActivity (Tools/Android/java/org/wowdecomp/wow/WowActivity.java): JNI calls into the activity,
// and the window it hands over (AndroidSystem.h). Any game thread may call these; each attaches to the Java VM once.

#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <pthread.h>
#include "Engine/Source/Os/Android/AndroidSystem.h"

namespace {

std::mutex s_lock;
std::condition_variable s_windowChanged;
ANativeActivity* s_activity;
ANativeWindow* s_window;
uint32_t s_windowGeneration;
uint32_t s_releasedGeneration;
bool s_windowInUse;
bool s_finishing;

pthread_key_t s_detachKey;
pthread_once_t s_detachOnce = PTHREAD_ONCE_INIT;

// Threads attached here are detached when they end, as the VM requires.
void DetachThread(void* vm) {
    static_cast<JavaVM*>(vm)->DetachCurrentThread();
}

void MakeDetachKey() {
    pthread_key_create(&s_detachKey, DetachThread);
}

JNIEnv* Env() {
    if (!s_activity) {
        return nullptr;
    }

    JavaVM* vm = s_activity->vm;
    JNIEnv* env = nullptr;

    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }

    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }

    pthread_once(&s_detachOnce, MakeDetachKey);
    pthread_setspecific(s_detachKey, vm);

    return env;
}

// A method of the activity (WowActivity), by name and JNI signature.
jmethodID Method(JNIEnv* env, const char* name, const char* signature) {
    jclass type = env->GetObjectClass(s_activity->clazz);
    jmethodID method = env->GetMethodID(type, name, signature);
    env->DeleteLocalRef(type);

    if (!method || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }

    return method;
}

// Clears a Java exception a call threw, so the next call works; true if there was one.
bool Failed(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }

    return false;
}

std::string String(JNIEnv* env, jstring value) {
    if (!value) {
        return std::string();
    }

    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    env->ReleaseStringUTFChars(value, chars);

    return result;
}

}

void AndroidSetActivity(ANativeActivity* activity) {
    std::lock_guard<std::mutex> lock(s_lock);
    s_activity = activity;
}

void AndroidSetWindow(ANativeWindow* window) {
    std::unique_lock<std::mutex> lock(s_lock);
    s_window = window;
    uint32_t generation = ++s_windowGeneration;
    s_windowChanged.notify_all();

    // The surface on a window that is going must be gone before the activity returns: Android frees the window then.
    if (!window && s_windowInUse) {
        s_windowChanged.wait_for(lock, std::chrono::seconds(2), [generation] {
            return static_cast<int32_t>(s_releasedGeneration - generation) >= 0 || s_finishing || !s_windowInUse;
        });
    }
}

ANativeWindow* AndroidGetWindow(uint32_t* generation) {
    std::lock_guard<std::mutex> lock(s_lock);
    *generation = s_windowGeneration;
    return s_window;
}

void AndroidReleaseWindow(uint32_t generation) {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_releasedGeneration = generation;
    }

    s_windowChanged.notify_all();
}

void AndroidSetWindowInUse(bool inUse) {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_windowInUse = inUse;
    }

    s_windowChanged.notify_all();
}

ANativeWindow* AndroidWaitForWindow() {
    std::unique_lock<std::mutex> lock(s_lock);
    s_windowChanged.wait(lock, [] { return s_window || s_finishing; });

    return s_finishing ? nullptr : s_window;
}

void AndroidSetFinishing() {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_finishing = true;
    }

    s_windowChanged.notify_all();
}

bool AndroidIsFinishing() {
    std::lock_guard<std::mutex> lock(s_lock);
    return s_finishing;
}

float AndroidGetRefreshRate() {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "getRefreshRate", "()F") : nullptr;

    if (!method) {
        return 60.0f;
    }

    jfloat rate = env->CallFloatMethod(s_activity->clazz, method);
    return Failed(env) || rate <= 0.0f ? 60.0f : rate;
}

void AndroidSetGestureExclusion(const int32_t* rects, int32_t count) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "setGestureExclusion", "([I)V") : nullptr;

    if (!method) {
        return;
    }

    jintArray array = env->NewIntArray(count * 4);
    env->SetIntArrayRegion(array, 0, count * 4, reinterpret_cast<const jint*>(rects));
    env->CallVoidMethod(s_activity->clazz, method, array);
    env->DeleteLocalRef(array);
    Failed(env);
}

bool AndroidOpenURL(const char* url) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "openUrl", "(Ljava/lang/String;)Z") : nullptr;

    if (!method) {
        return false;
    }

    jstring text = env->NewStringUTF(url ? url : "");
    jboolean opened = env->CallBooleanMethod(s_activity->clazz, method, text);
    env->DeleteLocalRef(text);

    return !Failed(env) && opened;
}

void AndroidGetScreenSize(int32_t* width, int32_t* height) {
    // The display's size, asked once: the window's changes while the activity settles into landscape, and the game's
    // display modes (Gx/CGxDevice/Android) follow from this.
    static int32_t s_displayWidth;
    static int32_t s_displayHeight;

    if (!s_displayWidth) {
        JNIEnv* env = Env();
        jmethodID method = env ? Method(env, "getDisplaySize", "()[I") : nullptr;
        jintArray size = method ? static_cast<jintArray>(env->CallObjectMethod(s_activity->clazz, method)) : nullptr;

        if (method && !Failed(env) && size && env->GetArrayLength(size) >= 2) {
            jint values[2];
            env->GetIntArrayRegion(size, 0, 2, values);
            s_displayWidth = values[0] > values[1] ? values[0] : values[1];
            s_displayHeight = values[0] > values[1] ? values[1] : values[0];
        }

        if (size) {
            env->DeleteLocalRef(size);
        }
    }

    if (s_displayWidth > 0 && s_displayHeight > 0) {
        *width = s_displayWidth;
        *height = s_displayHeight;
        return;
    }

    std::lock_guard<std::mutex> lock(s_lock);

    if (s_window) {
        int32_t w = ANativeWindow_getWidth(s_window);
        int32_t h = ANativeWindow_getHeight(s_window);

        // Landscape whatever the window reports while the rotation settles.
        *width = w > h ? w : h;
        *height = w > h ? h : w;
        return;
    }

    *width = 1920;
    *height = 1080;
}

float AndroidGetDensityDpi() {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "getDensityDpi", "()F") : nullptr;

    if (!method) {
        return 160.0f;
    }

    jfloat dpi = env->CallFloatMethod(s_activity->clazz, method);
    return Failed(env) || dpi <= 0.0f ? 160.0f : dpi;
}

void AndroidGetSafeInsets(int32_t* left, int32_t* top, int32_t* right, int32_t* bottom) {
    *left = *top = *right = *bottom = 0;

    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "getSafeInsets", "()[I") : nullptr;

    if (!method) {
        return;
    }

    jintArray insets = static_cast<jintArray>(env->CallObjectMethod(s_activity->clazz, method));

    if (Failed(env) || !insets || env->GetArrayLength(insets) < 4) {
        return;
    }

    jint values[4];
    env->GetIntArrayRegion(insets, 0, 4, values);
    env->DeleteLocalRef(insets);

    *left = values[0];
    *top = values[1];
    *right = values[2];
    *bottom = values[3];
}

bool AndroidHasStorageAccess() {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "hasStorageAccess", "()Z") : nullptr;

    if (!method) {
        return false;
    }

    jboolean granted = env->CallBooleanMethod(s_activity->clazz, method);
    return !Failed(env) && granted;
}

void AndroidRequestStorageAccess() {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "requestStorageAccess", "()V") : nullptr;

    if (method) {
        env->CallVoidMethod(s_activity->clazz, method);
        Failed(env);
    }
}

std::string AndroidGetStorageRoot() {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "getStorageRoot", "()Ljava/lang/String;") : nullptr;

    if (!method) {
        return "/sdcard";
    }

    jstring root = static_cast<jstring>(env->CallObjectMethod(s_activity->clazz, method));

    if (Failed(env) || !root) {
        return "/sdcard";
    }

    std::string result = String(env, root);
    env->DeleteLocalRef(root);

    return result;
}

int32_t AndroidShowMessage(const char* title, const char* message, const char* const* buttons, int32_t buttonCount) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "showMessage", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I") : nullptr;

    if (!method) {
        return 0;
    }

    jclass stringType = env->FindClass("java/lang/String");
    jobjectArray labels = env->NewObjectArray(buttonCount, stringType, nullptr);

    for (int32_t i = 0; i < buttonCount; i++) {
        jstring label = env->NewStringUTF(buttons[i]);
        env->SetObjectArrayElement(labels, i, label);
        env->DeleteLocalRef(label);
    }

    jstring titleText = env->NewStringUTF(title ? title : "");
    jstring messageText = env->NewStringUTF(message ? message : "");
    jint choice = env->CallIntMethod(s_activity->clazz, method, titleText, messageText, labels);

    env->DeleteLocalRef(titleText);
    env->DeleteLocalRef(messageText);
    env->DeleteLocalRef(labels);
    env->DeleteLocalRef(stringType);

    return Failed(env) ? 0 : choice;
}

bool AndroidEditText(const char* title, const char* text, bool password, std::string* result) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "editText", "(Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;") : nullptr;

    if (!method) {
        return false;
    }

    jstring titleText = env->NewStringUTF(title ? title : "");
    jstring initial = env->NewStringUTF(text ? text : "");
    jstring entered = static_cast<jstring>(env->CallObjectMethod(s_activity->clazz, method, titleText, initial, static_cast<jboolean>(password)));

    env->DeleteLocalRef(titleText);
    env->DeleteLocalRef(initial);

    if (Failed(env) || !entered) {
        return false;
    }

    *result = String(env, entered);
    env->DeleteLocalRef(entered);

    return true;
}

bool AndroidGetClipboard(std::string* text) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "getClipboardText", "()Ljava/lang/String;") : nullptr;

    if (!method) {
        return false;
    }

    jstring value = static_cast<jstring>(env->CallObjectMethod(s_activity->clazz, method));

    if (Failed(env) || !value) {
        return false;
    }

    *text = String(env, value);
    env->DeleteLocalRef(value);

    return true;
}

void AndroidSetClipboard(const char* text) {
    JNIEnv* env = Env();
    jmethodID method = env ? Method(env, "setClipboardText", "(Ljava/lang/String;)V") : nullptr;

    if (!method) {
        return;
    }

    jstring value = env->NewStringUTF(text ? text : "");
    env->CallVoidMethod(s_activity->clazz, method, value);
    env->DeleteLocalRef(value);
    Failed(env);
}
