// Android side of the OpenGL device's context (Mac/MacGlContext.h), on EGL with OpenGL ES 3. The desktop GL the device
// speaks is carried out on it by CGxDeviceOpenGL/Android/GlAndroid.cpp.
//
// The surface is on the activity's window, which comes and goes with the app in the background (AndroidSystem.h): the
// context stays, its surface is destroyed with the window, and a new one is made on the next window. Its buffers are
// the game's render size (the video options' resolution), which the display's hardware scaler stretches to the screen.

#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <unistd.h>
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

void AndroidGlSwapBuffers() {
    if (!s_current) {
        return;
    }

    GlEsFrameDone();

    if (s_current->m_surface != EGL_NO_SURFACE) {
        uint64_t start = GlEsNowNs();
        eglSwapBuffers(s_current->m_display, s_current->m_surface);

        if (start) {
            GlEsSwapTime(GlEsNowNs() - start);
        }
    }

    // In the background there is nothing to draw to: the game runs on (its connection stays up) at a slow tick.
    if (!SyncWindow(s_current)) {
        usleep(50 * 1000);
    }
}
