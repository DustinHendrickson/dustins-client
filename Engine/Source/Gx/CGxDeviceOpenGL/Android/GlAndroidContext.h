#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDCONTEXT_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDCONTEXT_H

#include <cstdint>

// EGL calls behind Android's WGL stand-ins (GlAndroidWgl.cpp), kept apart from the device code so that <EGL/egl.h>
// stays out of it. The context handle is the HGLRC the device holds (MacGlCreateContext in MacGlContext.cpp).

bool AndroidGlMakeCurrent(void* context);
void* AndroidGlCurrentContext();
void AndroidGlSwapBuffers();

// The frame rate the game is held to: 60 by default (whatever the display's refresh), 30 to save the battery.
void AndroidGlSetFrameRate(int32_t fps);

#endif
