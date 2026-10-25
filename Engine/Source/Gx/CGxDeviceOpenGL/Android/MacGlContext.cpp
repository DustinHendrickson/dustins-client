// Android side of the OpenGL device's context (Mac/MacGlContext.h), on EGL with OpenGL ES 3. The desktop GL the device
// speaks is carried out on it by CGxDeviceOpenGL/Android/GlAndroid.cpp.
//
// The surface is on the activity's window, which comes and goes with the app in the background (AndroidSystem.h): the
// context stays, its surface is destroyed with the window, and a new one is made on the next window. Its buffers are
// the game's render size (the video options' resolution), which the display's hardware scaler stretches to the screen.

#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroid.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidContext.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Mac/MacGlContext.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Gx/CGxGammaRamp.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/Os/Mac/OsMacWindow.h"

namespace {

struct AndroidGlContext {
    EGLDisplay m_display;
    EGLConfig m_config;
    EGLContext m_context;
    EGLSurface m_surface;
    ANativeWindow* m_window;
    uint32_t m_windowGeneration;
    void* m_view;
    int32_t m_interval;
};

// The frame rate (AndroidGlSetFrameRate), held whatever the display's refresh: the display is asked for it, the swap
// waits for as many of the display's refreshes as make one frame (two at 120 Hz for 60), and where they don't divide
// (90 Hz) the swap is also held back until the frame's time is nearly up.
int32_t s_frameRate = 60;
ANativeWindow* s_frameRateWindow;
int32_t s_frameRateSent;
uint64_t s_lastSwapNs;
float s_refreshRate;
uint64_t s_refreshCheckedNs;
int32_t s_swapInterval;

AndroidGlContext* s_current;

void DestroySurface(AndroidGlContext* context) {
    if (context->m_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(context->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, context->m_context);
        eglDestroySurface(context->m_display, context->m_surface);
        context->m_surface = EGL_NO_SURFACE;
    }
}

void CreateSurface(AndroidGlContext* context) {
    if (!context->m_window) {
        return;
    }

    int32_t width;
    int32_t height;
    OsMacGetContentSize(context->m_view, &width, &height);

    EGLint visual = 0;
    eglGetConfigAttrib(context->m_display, context->m_config, EGL_NATIVE_VISUAL_ID, &visual);
    ANativeWindow_setBuffersGeometry(context->m_window, width, height, visual);

    context->m_surface = eglCreateWindowSurface(context->m_display, context->m_config, context->m_window, nullptr);

    if (context->m_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "EGL: no window surface (0x%x)\n", eglGetError());
        return;
    }

    eglMakeCurrent(context->m_display, context->m_surface, context->m_surface, context->m_context);
    eglSwapInterval(context->m_display, context->m_interval);
    s_swapInterval = context->m_interval;
}

uint64_t MonotonicNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

// Android's performance hints (ADPF, Android 13 on, so looked up rather than linked): each frame's work against the
// frame's time, so the system sizes the CPU's clocks to the game rather than guessing (cooler, steadier).
struct PerformanceHints {
    void* (*getManager)();
    void* (*createSession)(void* manager, const int32_t* threads, size_t count, int64_t targetNs);
    int (*updateTarget)(void* session, int64_t targetNs);
    int (*reportActual)(void* session, int64_t actualNs);
    void* session;
    int32_t target;
    bool tried;
};

PerformanceHints s_hints;

void ReportFrameWork(uint64_t workNs) {
    PerformanceHints& hints = s_hints;

    if (!hints.tried) {
        hints.tried = true;
        void* library = dlopen("libandroid.so", RTLD_NOW);

        if (library) {
            hints.getManager = reinterpret_cast<void* (*)()>(dlsym(library, "APerformanceHint_getManager"));
            hints.createSession = reinterpret_cast<void* (*)(void*, const int32_t*, size_t, int64_t)>(dlsym(library, "APerformanceHint_createSession"));
            hints.updateTarget = reinterpret_cast<int (*)(void*, int64_t)>(dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
            hints.reportActual = reinterpret_cast<int (*)(void*, int64_t)>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
        }

        if (hints.getManager && hints.createSession && hints.updateTarget && hints.reportActual) {
            void* manager = hints.getManager();
            int32_t thread = gettid();
            hints.target = s_frameRate;
            hints.session = manager ? hints.createSession(manager, &thread, 1, 1000000000ll / s_frameRate) : nullptr;
        }
    }

    if (!hints.session || !workNs) {
        return;
    }

    if (hints.target != s_frameRate) {
        hints.target = s_frameRate;
        hints.updateTarget(hints.session, 1000000000ll / s_frameRate);
    }

    hints.reportActual(hints.session, static_cast<int64_t>(workNs));
}

void PaceFrame(AndroidGlContext* context) {
    // The frame's work: from the last swap's return to now, before any wait for the frame's time.
    if (s_lastSwapNs) {
        ReportFrameWork(MonotonicNs() - s_lastSwapNs);
    }

    if (context->m_window && (context->m_window != s_frameRateWindow || s_frameRate != s_frameRateSent)) {
        ANativeWindow_setFrameRate(context->m_window, static_cast<float>(s_frameRate), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        s_frameRateWindow = context->m_window;
        s_frameRateSent = s_frameRate;
    }

    // The display's refresh, asked again every couple of seconds (the system changes it).
    uint64_t nowNs = MonotonicNs();

    if (!s_refreshRate || nowNs - s_refreshCheckedNs > 2000000000ull) {
        s_refreshCheckedNs = nowNs;
        s_refreshRate = AndroidGetRefreshRate();
    }

    // With the vertical sync on (the device's interval), as many refreshes a frame as fit in the frame's time.
    int32_t refreshes = static_cast<int32_t>(s_refreshRate / static_cast<float>(s_frameRate) + 0.1f);
    int32_t interval = context->m_interval > 0 ? std::max(1, refreshes) * context->m_interval : 0;

    if (interval != s_swapInterval) {
        s_swapInterval = interval;
        eglSwapInterval(context->m_display, interval);
    }

    // Faster than the frame rate even so (no vertical sync, or a refresh that doesn't divide): the swap waits for the
    // frame's time, less a couple of milliseconds so it still makes its vertical blank.
    float shown = interval > 0 ? s_refreshRate / static_cast<float>(interval) : 1000.0f;

    if (shown > static_cast<float>(s_frameRate) * 1.05f && s_lastSwapNs) {
        uint64_t due = s_lastSwapNs + 1000000000ull / static_cast<uint64_t>(s_frameRate) - 2000000ull;
        uint64_t now = MonotonicNs();

        if (now < due) {
            usleep(static_cast<useconds_t>((due - now) / 1000));
        }
    }
}

// Follows the activity's window: a new one gets a surface, a gone one loses it. True while there is one to draw to.
bool SyncWindow(AndroidGlContext* context) {
    uint32_t generation;
    ANativeWindow* window = AndroidGetWindow(&generation);

    if (generation != context->m_windowGeneration) {
        DestroySurface(context);
        context->m_window = window;
        context->m_windowGeneration = generation;
        AndroidReleaseWindow(generation);
        CreateSurface(context);
    }

    return context->m_surface != EGL_NO_SURFACE;
}

}

void* MacGlCreateContext(void* view, const CGxFormat& format) {
    // The device is made after the activity has a window (WoW/Source/Android/main.cpp waits for it).
    if (!AndroidWaitForWindow()) {
        return nullptr;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        fprintf(stderr, "EGL: no display (0x%x)\n", eglGetError());
        return nullptr;
    }

    EGLint depth = 24;
    EGLint stencil = 8;

    switch (format.m_depthFormat) {
        case CGxFormat::Fmt_Ds160:
            depth = 16;
            stencil = 0;
            break;
        case CGxFormat::Fmt_Ds24X:
            stencil = 0;
            break;
        default:
            break;
    }

    EGLint samples = format.m_multisampleCount > 1 ? static_cast<EGLint>(format.m_multisampleCount) : 0;
    EGLConfig config = nullptr;

    // The format's multisampling if a config has it, else none.
    for (int32_t attempt = 0; attempt < 2 && !config; attempt++) {
        const EGLint attributes[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, format.m_colorFormat == CGxFormat::Fmt_Argb8888 ? 8 : 0,
            EGL_DEPTH_SIZE, depth,
            EGL_STENCIL_SIZE, stencil,
            EGL_SAMPLE_BUFFERS, attempt == 0 && samples ? 1 : 0,
            EGL_SAMPLES, attempt == 0 ? samples : 0,
            EGL_NONE,
        };

        EGLint count = 0;

        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count < 1) {
            config = nullptr;
        }
    }

    if (!config) {
        fprintf(stderr, "EGL: no config for depth %d stencil %d samples %d (0x%x)\n", depth, stencil, samples, eglGetError());
        return nullptr;
    }

    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext eglContext = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);

    if (eglContext == EGL_NO_CONTEXT) {
        fprintf(stderr, "EGL: no OpenGL ES 3 context (0x%x)\n", eglGetError());
        return nullptr;
    }

    AndroidGlContext* context = new AndroidGlContext();
    context->m_display = display;
    context->m_config = config;
    context->m_context = eglContext;
    context->m_surface = EGL_NO_SURFACE;
    context->m_window = nullptr;
    context->m_windowGeneration = 0;
    context->m_view = view;
    context->m_interval = 1;
    s_current = context;

    // Surfaceless until the window's surface exists (EGL_KHR_surfaceless_context, which Android always has).
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext);
    AndroidSetWindowInUse(true);
    SyncWindow(context);

    EGLint configSamples = 0;
    EGLint configAlpha = 0;
    eglGetConfigAttrib(display, config, EGL_SAMPLES, &configSamples);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &configAlpha);
    fprintf(stderr, "OpenGL ES: %s, %s, %s; %d samples, %d alpha bits\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), configSamples, configAlpha);
    GlEsContextCreated();

    return context;
}

void MacGlDestroyContext(void* handle) {
    AndroidGlContext* context = static_cast<AndroidGlContext*>(handle);

    if (!context) {
        return;
    }

    GlEsContextDestroyed();
    DestroySurface(context);
    AndroidReleaseWindow(context->m_windowGeneration);
    AndroidSetWindowInUse(false);

    eglMakeCurrent(context->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(context->m_display, context->m_context);

    if (s_current == context) {
        s_current = nullptr;
    }

    delete context;
}

void MacGlSetSwapInterval(void* handle, int32_t interval) {
    AndroidGlContext* context = static_cast<AndroidGlContext*>(handle);

    if (context) {
        context->m_interval = interval;
        eglSwapInterval(context->m_display, interval);

        // The next frame works out its own from this one (PaceFrame).
        s_swapInterval = -1;
    }
}

void MacGlUpdate(void* handle) {
    AndroidGlContext* context = static_cast<AndroidGlContext*>(handle);

    // The render size or the window changed: a new surface at the size.
    if (context) {
        DestroySurface(context);
        context->m_windowGeneration = 0;
        SyncWindow(context);
    }
}

bool MacGlGetDisplayGamma(CGxGammaRamp& ramp) {
    return false;
}

void MacGlSetDisplayGamma(const CGxGammaRamp& ramp) {
}

void MacGlRestoreDisplayGamma() {
}

bool AndroidGlMakeCurrent(void* handle) {
    AndroidGlContext* context = static_cast<AndroidGlContext*>(handle);

    if (!context) {
        if (s_current) {
            eglMakeCurrent(s_current->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        s_current = nullptr;
        return true;
    }

    s_current = context;
    return eglMakeCurrent(context->m_display, context->m_surface, context->m_surface, context->m_context);
}

void* AndroidGlCurrentContext() {
    return s_current;
}

void AndroidGlSetFrameRate(int32_t fps) {
    s_frameRate = fps >= 60 || fps <= 0 ? 60 : fps;
}

void AndroidGlSwapBuffers() {
    if (!s_current) {
        return;
    }

    GlEsFrameDone();

    if (s_current->m_surface != EGL_NO_SURFACE) {
        PaceFrame(s_current);

        uint64_t start = GlEsNowNs();
        eglSwapBuffers(s_current->m_display, s_current->m_surface);
        s_lastSwapNs = MonotonicNs();

        if (start) {
            GlEsSwapTime(GlEsNowNs() - start);
        }
    }

    // In the background there is nothing to draw to: the game runs on (its connection stays up) at a slow tick.
    if (!SyncWindow(s_current)) {
        usleep(50 * 1000);
    }
}
