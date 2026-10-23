#ifndef ENGINE_SOURCE_GX_CGXDEVICEGLES_CGXDEVICEGLES_H
#define ENGINE_SOURCE_GX_CGXDEVICEGLES_CGXDEVICEGLES_H

// The OpenGL ES 3 device for Android (docs/ANDROID_PORT.md), in the OpenGL device's slot: the game asks for OpenGL and
// gets this. It draws into the activity's window through EGL, at the size of the video options' resolution, which
// Android's hardware scaler stretches to the screen.
//
// The window comes and goes with the app in the background (AndroidSystem.h): the context stays, only the surface is
// destroyed, and nothing is drawn until a window is back.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdint>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

struct ANativeWindow;

class CGxDeviceGLES : public CGxDevice {
    public:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    ANativeWindow* m_window = nullptr;
    uint32_t m_windowGeneration = 0;
    const GLubyte* m_glVendor = nullptr;
    const GLubyte* m_glRenderer = nullptr;
    const GLubyte* m_glVersion = nullptr;

    virtual void IRsSendToHw(EGxRenderState which);
    virtual ~CGxDeviceGLES();
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void LogCrashInfo(char* buffer, uint32_t size);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane);
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face);
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name);
    virtual void IShaderCreate(CGxShader* shader);
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    CGxDeviceGLES();

    // EGL: the context (kept while the app is in the background) and the surface on the current window.
    bool ICreateContext(const CGxFormat& format);
    void IDestroyContext();
    bool ICreateSurface();
    void IDestroySurface();

    // Follows the activity's window: a new one gets a surface, a gone one loses it. True while there is one to draw to.
    bool ISyncWindow();
    void ISetCaps(const CGxFormat& format);
    void ISetRenderSize(int32_t width, int32_t height);
};

CGxDevice* CreateCGxDeviceOpenGl();

#endif
