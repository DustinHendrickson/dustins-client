// Android entry point (Win64/WinMain.cpp, Mac/main.mm): the NativeActivity callbacks of WowActivity, and the game's
// thread.
//
// The game folder is WoW335 on shared storage (/sdcard/WoW335/), with the 3.3.5a Data folder in it; WTF/, Logs/,
// Interface/ and the rest are written there as next to Wow.exe. Reaching it takes All files access, which the player
// grants on a system page the game opens. Windows runs the client on an 8 MB fiber, so it runs here on a thread with an
// 8 MB stack.
//
// The activity's callbacks come on its UI thread; input comes on a thread of its own (the input queue's looper), and
// both hand what they get to the game thread (Os/Android/Input.cpp, AndroidActivity.cpp).

#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/Process/Mac/GameDirectory.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Client.h"
#include "WowBuildId.h"

// What _NSGetArgc and _NSGetArgv report (Base/Compat/Android/crt_externs.h): the game reads its command line from
// them. An activity has none.
static char s_programName[] = "wow";
static char* s_argv[] = { s_programName, nullptr };
int wowdecomp_android_argc = 1;
char** wowdecomp_android_argv = s_argv;

namespace {

const char* kLogTag = "WoW";
const char* kFolderName = "WoW335";

ANativeActivity* s_activity;
pthread_t s_gameThread;
std::atomic<bool> s_gameRunning;

// Input: a looper thread of its own reads the activity's input queue.
std::mutex s_inputLock;
AInputQueue* s_inputQueue;
ALooper* s_inputLooper;
pthread_t s_inputThread;
bool s_inputThreadStarted;

// stdout and stderr go to Logs/Console.txt in the game folder (the working directory by then), so a run leaves its
// output behind; the previous run's is kept.
void RedirectConsole() {
    mkdir("Logs", 0777);
    remove("Logs/Console-previous.txt");
    rename("Logs/Console.txt", "Logs/Console-previous.txt");

    if (freopen("Logs/Console.txt", "w", stdout)) {
        dup2(fileno(stdout), fileno(stderr));
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }

    fprintf(stderr, "World of Warcraft for Android, build %s (%s %s)\n", WOWDECOMP_BUILD_ID, __DATE__, __TIME__);
}

bool HasGameData(const std::string& folder) {
    DIR* directory = opendir((folder + "Data").c_str());

    if (!directory) {
        return false;
    }

    bool found = false;

    while (dirent* entry = readdir(directory)) {
        size_t length = strlen(entry->d_name);

        if (length > 4 && !strcasecmp(entry->d_name + length - 4, ".mpq")) {
            found = true;
            break;
        }
    }

    closedir(directory);
    return found;
}

void ShowError(const char* message) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message);
    const char* buttons[] = { "Quit" };
    AndroidShowMessage("World of Warcraft", message, buttons, 1);
}

// All files access, asked for until it's granted or the player quits.
bool GetStorageAccess() {
    while (!AndroidHasStorageAccess()) {
        if (AndroidIsFinishing()) {
            return false;
        }

        const char* buttons[] = { "Allow", "Quit" };
        int32_t choice = AndroidShowMessage("World of Warcraft",
                                            "The game reads its data from the WoW335 folder on this device's storage, "
                                            "which needs All files access.\n\nAllow it for World of Warcraft on the next "
                                            "page, then come back.",
                                            buttons, 2);

        if (choice != 0) {
            return false;
        }

        AndroidRequestStorageAccess();

        // Until the player comes back from the settings page.
        for (int32_t i = 0; i < 600 && !AndroidHasStorageAccess() && !AndroidIsFinishing(); i++) {
            usleep(500 * 1000);
        }
    }

    return true;
}

// A gxResolution the device offers (Gx/CGxDevice/Android: the screen's size and smaller ones in its shape), so the
// client doesn't fall back to its 4:3 hardware table; a new config skips the hardware detection that would pick one.
void PrepareConfig() {
    std::vector<std::string> lines;
    bool haveResolution = false;
    bool haveDetect = false;

    if (FILE* file = fopen("WTF/Config.wtf", "r")) {
        char line[1024];

        while (fgets(line, sizeof(line), file)) {
            std::string text = line;

            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }

            if (!strncasecmp(text.c_str(), "SET gxResolution ", 17)) {
                haveResolution = true;
            } else if (!strncasecmp(text.c_str(), "SET hwDetect ", 13)) {
                haveDetect = true;
            }

            lines.push_back(text);
        }

        fclose(file);
    }

    if (haveResolution && haveDetect) {
        return;
    }

    if (!haveResolution) {
        int32_t width;
        int32_t height;
        AndroidGetScreenSize(&width, &height);

        char resolution[64];
        snprintf(resolution, sizeof(resolution), "SET gxResolution \"%dx%d\"", width, height);
        lines.push_back(resolution);
    }

    if (!haveDetect) {
        lines.push_back("SET hwDetect \"0\"");
    }

    mkdir("WTF", 0777);

    if (FILE* file = fopen("WTF/Config.wtf", "w")) {
        for (const std::string& line : lines) {
            fprintf(file, "%s\n", line.c_str());
        }

        fclose(file);
    }
}

void* GameThread(void* param) {
    int result = 1;

    // The device is made for the activity's window: wait for the first one.
    if (AndroidWaitForWindow() && GetStorageAccess()) {
        std::string folder = AndroidGetStorageRoot() + "/" + kFolderName + "/";
        mkdir(folder.c_str(), 0777);

        if (HasGameData(folder)) {
            MacSetGameDirectory(folder.c_str());
            chdir(folder.c_str());
            RedirectConsole();
            PrepareConfig();

            StormRtlInitialize();
            result = CommonMain();
            fprintf(stderr, "Quit: the game has shut down (%d)\n", result);
        } else {
            std::string message = "No game data found.\n\nCopy the Data folder of a World of Warcraft 3.3.5a (12340) "
                                  "client to " + folder + "Data on this device.";
            ShowError(message.c_str());
        }
    }

    fflush(stdout);
    fflush(stderr);
    s_gameRunning = false;

    // The game has saved and shut down. Ending the process here skips static destructors that would run with the
    // audio and network threads still about; Android then closes the activity.
    if (s_activity) {
        ANativeActivity_finish(s_activity);
    }

    usleep(200 * 1000);
    _exit(result);
}

int InputCallback(int fd, int events, void* data) {
    std::lock_guard<std::mutex> lock(s_inputLock);

    if (!s_inputQueue) {
        return 1;
    }

    AInputEvent* event = nullptr;

    while (AInputQueue_getEvent(s_inputQueue, &event) >= 0) {
        // Keys go to an input method first if one is showing.
        if (AInputQueue_preDispatchEvent(s_inputQueue, event)) {
            continue;
        }

        AInputQueue_finishEvent(s_inputQueue, event, AndroidInputHandleEvent(event));
    }

    return 1;
}

void* InputThread(void* param) {
    pthread_setname_np("Input");
    ALooper* looper = ALooper_prepare(0);

    {
        std::lock_guard<std::mutex> lock(s_inputLock);
        ALooper_acquire(looper);
        s_inputLooper = looper;

        if (s_inputQueue) {
            AInputQueue_attachLooper(s_inputQueue, looper, 1, InputCallback, nullptr);
        }
    }

    while (ALooper_pollOnce(-1, nullptr, nullptr, nullptr) != ALOOPER_POLL_ERROR) {
    }

    return nullptr;
}

void OnInputQueueCreated(ANativeActivity* activity, AInputQueue* queue) {
    std::lock_guard<std::mutex> lock(s_inputLock);
    s_inputQueue = queue;

    if (s_inputLooper) {
        AInputQueue_attachLooper(queue, s_inputLooper, 1, InputCallback, nullptr);
    } else if (!s_inputThreadStarted) {
        s_inputThreadStarted = pthread_create(&s_inputThread, nullptr, InputThread, nullptr) == 0;
    }
}

void OnInputQueueDestroyed(ANativeActivity* activity, AInputQueue* queue) {
    std::lock_guard<std::mutex> lock(s_inputLock);

    if (s_inputLooper) {
        AInputQueue_detachLooper(queue);
    }

    s_inputQueue = nullptr;
}

void OnNativeWindowCreated(ANativeActivity* activity, ANativeWindow* window) {
    AndroidInputSetSurfaceSize(ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    AndroidSetWindow(window);
}

void OnNativeWindowResized(ANativeActivity* activity, ANativeWindow* window) {
    AndroidInputSetSurfaceSize(ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
}

void OnNativeWindowDestroyed(ANativeActivity* activity, ANativeWindow* window) {
    AndroidSetWindow(nullptr);
}

void OnWindowFocusChanged(ANativeActivity* activity, int hasFocus) {
    AndroidInputSetFocus(hasFocus != 0);
}

void OnDestroy(ANativeActivity* activity) {
    // The activity is going (the player swiped the app away): the game quits as when its window is closed.
    AndroidSetFinishing();

    if (s_gameRunning) {
        AndroidInputClose();
    }
}

}

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Starting");

    // Writes to a closed connection must fail with EPIPE rather than end the process.
    signal(SIGPIPE, SIG_IGN);

    s_activity = activity;
    AndroidSetActivity(activity);

    activity->callbacks->onDestroy = OnDestroy;
    activity->callbacks->onWindowFocusChanged = OnWindowFocusChanged;
    activity->callbacks->onNativeWindowCreated = OnNativeWindowCreated;
    activity->callbacks->onNativeWindowResized = OnNativeWindowResized;
    activity->callbacks->onNativeWindowRedrawNeeded = nullptr;
    activity->callbacks->onNativeWindowDestroyed = OnNativeWindowDestroyed;
    activity->callbacks->onInputQueueCreated = OnInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = OnInputQueueDestroyed;

    // One game per process: an activity started again while it runs (after the system recreated it) shares it.
    if (s_gameRunning.exchange(true)) {
        return;
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);

    if (pthread_create(&s_gameThread, &attributes, GameThread, nullptr) != 0) {
        s_gameRunning = false;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "The game thread couldn't be started");
        ANativeActivity_finish(activity);
    }

    pthread_attr_destroy(&attributes);
}
