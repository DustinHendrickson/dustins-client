#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDCONTEXT_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDCONTEXT_H

// EGL calls behind Android's WGL stand-ins (GlAndroidWgl.cpp), kept apart from the device code so that <EGL/egl.h>
// stays out of it. The context handle is the HGLRC the device holds (MacGlCreateContext in MacGlContext.cpp).

bool AndroidGlMakeCurrent(void* context);
void* AndroidGlCurrentContext();
void AndroidGlSwapBuffers();

#endif
