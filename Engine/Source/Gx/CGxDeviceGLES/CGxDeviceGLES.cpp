// The OpenGL ES 3 device (CGxDeviceGLES.h). This is its frame: EGL, the window, the format and caps, clearing and
// presenting, and buffers in system memory. Drawing (state, textures, the fixed-function pipeline as GLSL and the ARB
// programs translated to GLSL ES) comes next, per docs/ANDROID_PORT.md.

#include "Engine/Source/Gx/CGxDeviceGLES/CGxDeviceGLES.h"
#include <android/native_window.h>
#include <unistd.h>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {

bool HasExtension(const char* name) {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));

    if (!extensions) {
        return false;
    }

    size_t length = strlen(name);

    for (const char* found = strstr(extensions, name); found; found = strstr(found + length, name)) {
        if ((found == extensions || found[-1] == ' ') && (found[length] == ' ' || found[length] == '\0')) {
            return true;
        }
    }

    return false;
}

}

CGxDevice* CreateCGxDeviceOpenGl() {
    void* mem = SMemAlloc(sizeof(CGxDeviceGLES), __FILE__, __LINE__, 0);

    if (!mem) {
        return nullptr;
    }

    return new (mem) CGxDeviceGLES();
}

CGxDeviceGLES::CGxDeviceGLES() {
    // The game picks its OpenGL paths (shaders, texture formats) for this device.
    this->m_api = GxApi_OpenGl;
    this->m_caps.m_colorFormat = GxCF_rgba;
    this->m_caps.unk_0018 = 0;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceGLES::~CGxDeviceGLES() {
    this->IDestroyContext();
}

bool CGxDeviceGLES::ICreateContext(const CGxFormat& format) {
    this->m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    if (this->m_display == EGL_NO_DISPLAY || !eglInitialize(this->m_display, nullptr, nullptr)) {
        CGxDevice::Log("CGxDeviceGLES: no EGL display (0x%x)", eglGetError());
        return false;
    }

    // The format's depth and multisampling if a config has them, else the closest without multisampling.
    bool alpha = format.m_colorFormat == CGxFormat::Fmt_Argb8888;
    EGLint depth = format.m_depthFormat == CGxFormat::Fmt_Ds160 ? 16 : 24;
    EGLint stencil = format.m_depthFormat == CGxFormat::Fmt_Ds248 ? 8 : 0;
    EGLint samples = format.m_multisampleCount > 1 ? static_cast<EGLint>(format.m_multisampleCount) : 0;

    for (int32_t attempt = 0; attempt < 2 && !this->m_config; attempt++) {
        const EGLint attributes[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, alpha ? 8 : 0,
            EGL_DEPTH_SIZE, depth,
            EGL_STENCIL_SIZE, stencil,
            EGL_SAMPLE_BUFFERS, attempt == 0 && samples ? 1 : 0,
            EGL_SAMPLES, attempt == 0 ? samples : 0,
            EGL_NONE,
        };

        EGLint count = 0;

        if (!eglChooseConfig(this->m_display, attributes, &this->m_config, 1, &count) || count < 1) {
            this->m_config = nullptr;
        }
    }

    if (!this->m_config) {
        CGxDevice::Log("CGxDeviceGLES: no EGL config for the format (0x%x)", eglGetError());
        return false;
    }

    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    this->m_eglContext = eglCreateContext(this->m_display, this->m_config, EGL_NO_CONTEXT, contextAttributes);

    if (this->m_eglContext == EGL_NO_CONTEXT) {
        CGxDevice::Log("CGxDeviceGLES: no OpenGL ES 3 context (0x%x)", eglGetError());
        return false;
    }

    // Surfaceless until there is a window (EGL_KHR_surfaceless_context, which Android always has).
    eglMakeCurrent(this->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->m_eglContext);

    this->m_glVendor = glGetString(GL_VENDOR);
    this->m_glRenderer = glGetString(GL_RENDERER);
    this->m_glVersion = glGetString(GL_VERSION);
    CGxDevice::Log("CGxDeviceGLES: %s, %s, %s", this->m_glVendor, this->m_glRenderer, this->m_glVersion);

    AndroidSetWindowInUse(true);

    return true;
}

void CGxDeviceGLES::IDestroyContext() {
    this->IDestroySurface();

    if (this->m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(this->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (this->m_eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(this->m_display, this->m_eglContext);
        }

        eglTerminate(this->m_display);
    }

    AndroidSetWindowInUse(false);

    this->m_display = EGL_NO_DISPLAY;
    this->m_eglContext = EGL_NO_CONTEXT;
    this->m_config = nullptr;
    this->m_context = 0;
}

bool CGxDeviceGLES::ICreateSurface() {
    if (!this->m_window || this->m_eglContext == EGL_NO_CONTEXT) {
        return false;
    }

    int32_t width = this->m_format.m_size.x;
    int32_t height = this->m_format.m_size.y;

    if (width <= 0 || height <= 0) {
        AndroidGetScreenSize(&width, &height);
    }

    // The buffers are the render size; the display's hardware scaler stretches them to the screen.
    EGLint visual = 0;
    eglGetConfigAttrib(this->m_display, this->m_config, EGL_NATIVE_VISUAL_ID, &visual);
    ANativeWindow_setBuffersGeometry(this->m_window, width, height, visual);

    this->m_surface = eglCreateWindowSurface(this->m_display, this->m_config, this->m_window, nullptr);

    if (this->m_surface == EGL_NO_SURFACE) {
        CGxDevice::Log("CGxDeviceGLES: no window surface (0x%x)", eglGetError());
        return false;
    }

    eglMakeCurrent(this->m_display, this->m_surface, this->m_surface, this->m_eglContext);
    eglSwapInterval(this->m_display, this->m_format.m_vsync ? 1 : 0);

    this->ISetRenderSize(width, height);
    this->m_windowVisible = 1;

    return true;
}

void CGxDeviceGLES::IDestroySurface() {
    if (this->m_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(this->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->m_eglContext);
        eglDestroySurface(this->m_display, this->m_surface);
        this->m_surface = EGL_NO_SURFACE;
    }

    this->m_windowVisible = 0;
}

bool CGxDeviceGLES::ISyncWindow() {
    uint32_t generation;
    ANativeWindow* window = AndroidGetWindow(&generation);

    if (generation != this->m_windowGeneration) {
        this->IDestroySurface();
        this->m_window = window;
        this->m_windowGeneration = generation;
        AndroidReleaseWindow(generation);

        if (window) {
            this->ICreateSurface();
        }
    }

    return this->m_surface != EGL_NO_SURFACE;
}

void CGxDeviceGLES::ISetRenderSize(int32_t width, int32_t height) {
    AndroidInputSetRenderSize(width, height);

    CRect rect;
    rect.minX = 0.0f;
    rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    this->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
}

void CGxDeviceGLES::ISetCaps(const CGxFormat& format) {
    GLint value = 0;

    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &value);
    this->m_caps.m_numTmus = value >= 8 ? 8 : value;
    this->m_caps.m_pixelCenterOnEdge = 1;
    this->m_caps.m_texelCenterOnEdge = 1;

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    this->m_caps.m_texMaxSize[GxTex_2d] = value < 256 ? 256 : value;
    this->m_caps.m_texTarget[GxTex_2d] = 1;

    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &value);
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = value < 256 ? 256 : value;
    this->m_caps.m_texTarget[GxTex_CubeMap] = 1;
    this->m_caps.m_texMaxSize[GxTex_Rectangle] = 256;
    this->m_caps.m_texTarget[GxTex_Rectangle] = 0;
    this->m_caps.m_texNonPow2 = 1;
    this->m_caps.m_texMaxSize[GxTex_NonPow2] = this->m_caps.m_texMaxSize[GxTex_2d];
    this->m_caps.m_texTarget[GxTex_NonPow2] = 0;

    // DXT where the GPU decodes it (most desktop-derived and emulator GPUs); the others get it decoded on upload.
    int32_t s3tc = HasExtension("GL_EXT_texture_compression_s3tc") ? 1 : 0;
    this->m_caps.m_texFmt[GxTex_Dxt1] = s3tc;
    this->m_caps.m_texFmt[GxTex_Dxt3] = s3tc;
    this->m_caps.m_texFmt[GxTex_Dxt5] = s3tc;
    this->m_caps.m_texFmt[GxTex_Uv88] = 0;

    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &value);
    this->m_caps.m_numStreams = value;
    this->m_caps.m_streamOffset = 1;
    this->m_caps.m_generateMipMaps = 1;

    // No shader targets yet: the game draws through the fixed-function path.
    for (uint32_t i = 0; i < 6; i++) {
        this->m_caps.m_shaderTargets[i] = 0;
        this->m_caps.m_shaderConstants[i] = 0;
    }

    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_texFilterAnisotropic = 0;
    this->m_caps.m_maxTexAnisotropy = 1;

    if (HasExtension("GL_EXT_texture_filter_anisotropic")) {
        glGetIntegerv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &value);
        this->m_caps.m_maxTexAnisotropy = value;
        this->m_caps.m_texFilterAnisotropic = value > 1;
    }

    this->m_caps.m_colorWrite = 1;
    this->m_caps.m_maxClipPlanes = 0;
    this->m_caps.m_occlusionQuery = 0;
    this->m_caps.m_hwCursor = 0;
    this->m_caps.m_pointParameters = 0;
    this->m_caps.m_pointScaleMax = 1.0f;
    this->m_caps.m_pointSprite = 0;
    this->m_caps.m_shaderAlphaRef = 1;
    this->m_caps.unk_0134 = 1;
    memset(this->m_caps.unused_0114, 0, sizeof(this->m_caps.unused_0114));
    this->m_caps.m_stereoAvailable = 0;
    this->m_caps.unk_0138 = 1;
    this->Log(this->m_caps);
}

int32_t CGxDeviceGLES::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    if (CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceGLES::DeviceCreate(void* window, const CGxFormat& format) {
    // The device always draws into the activity's window.
    return this->DeviceCreate(static_cast<GxWindowProc>(nullptr), format);
}

void CGxDeviceGLES::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    this->ICursorDestroy();
    this->IDestroyContext();
}

int32_t CGxDeviceGLES::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceGLES::DeviceSetFormat():");
    this->Log(format);

    if (!CGxDevice::DeviceSetFormat(format)) {
        return 0;
    }

    if (this->m_eglContext == EGL_NO_CONTEXT) {
        if (!AndroidWaitForWindow() || !this->ICreateContext(this->m_format)) {
            this->IDestroyContext();
            return 0;
        }

        this->m_context = 1;
        this->ISetCaps(this->m_format);
        this->ICursorCreate(this->m_format);
        this->IRsSync(1);
    }

    // A new size takes a new surface.
    this->IDestroySurface();
    this->m_windowGeneration = 0;
    this->ISyncWindow();
    this->m_windowFocus = 1;

    return 1;
}

void* CGxDeviceGLES::DeviceWindow() {
    return this->m_window;
}

void CGxDeviceGLES::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    pixels.SetCount(0);
}

void CGxDeviceGLES::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    depths.SetCount(0);
}

int32_t CGxDeviceGLES::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
            } else {
                this->m_windowVisible = 1;
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_SetFocus:
            this->m_windowFocus = 1;
            break;

        case GxWM_KillFocus:
            this->m_windowFocus = 0;
            break;

        default:
            break;
    }

    return 0;
}

void CGxDeviceGLES::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceGLES::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceGLES::LogCrashInfo(char* buffer, uint32_t size) {
    char line[256];
    SStrPrintf(line, sizeof(line), "OpenGL ES: %s, %s, %s\r\n", this->m_glVendor ? reinterpret_cast<const char*>(this->m_glVendor) : "",
               this->m_glRenderer ? reinterpret_cast<const char*>(this->m_glRenderer) : "", this->m_glVersion ? reinterpret_cast<const char*>(this->m_glVersion) : "");
    SStrPack(buffer, line, size);
}

void CGxDeviceGLES::ScenePresent() {
    CGxDevice::ScenePresent();

    if (this->m_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(this->m_display, this->m_surface);
    }

    // In the background there is nothing to draw to: the game runs on (its connection stays up) at a slow tick.
    if (!this->ISyncWindow()) {
        usleep(50 * 1000);
    }

    this->WaitForFPSCap();
}

void CGxDeviceGLES::SceneClear(uint32_t mask, CImVector color) {
    if (this->m_surface == EGL_NO_SURFACE) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    glViewport(0, 0, static_cast<GLsizei>(window.maxX), static_cast<GLsizei>(window.maxY));
    glClearColor(color.r * (1.0f / 255.0f), color.g * (1.0f / 255.0f), color.b * (1.0f / 255.0f), color.a * (1.0f / 255.0f));
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    GLbitfield bits = 0;

    if (mask & GxBufferBit_Color) {
        bits |= GL_COLOR_BUFFER_BIT;
    }

    if (mask & GxBufferBit_Depth) {
        bits |= GL_DEPTH_BUFFER_BIT;
    }

    glClear(bits);
}

void CGxDeviceGLES::IRsSendToHw(EGxRenderState which) {
}

void CGxDeviceGLES::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (pool->m_apiSpecific) {
        SMemFree(pool->m_apiSpecific, __FILE__, __LINE__, 0);
    }

    pool->m_apiSpecific = SMemAlloc(size, __FILE__, __LINE__, 0);
    pool->Invalidate();
    pool->m_size = size;
}

void CGxDeviceGLES::PoolDestroy(CGxPool* pool) {
    if (pool) {
        pool->Invalidate();

        if (pool->m_apiSpecific) {
            SMemFree(pool->m_apiSpecific, __FILE__, __LINE__, 0);
            pool->m_apiSpecific = nullptr;
        }
    }

    CGxDevice::PoolDestroy(pool);
}

char* CGxDeviceGLES::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);

    CGxPool* pool = buf->m_pool;

    if (!pool->m_apiSpecific) {
        pool->m_apiSpecific = SMemAlloc(pool->m_size, __FILE__, __LINE__, 0);
    }

    char* mem = static_cast<char*>(pool->m_apiSpecific);
    pool->m_mem = mem;

    if (pool->m_usage == GxPoolUsage_Stream) {
        buf->m_index = 0;
        return mem;
    }

    if (pool->m_usage <= GxPoolUsage_Dynamic) {
        return mem + buf->m_index;
    }

    return nullptr;
}

int32_t CGxDeviceGLES::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    buf->m_valid = 1;
    buf->m_pool->m_mem = nullptr;
    return 1;
}

int32_t CGxDeviceGLES::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    return 0;
}

int32_t CGxDeviceGLES::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    return 0;
}

bool CGxDeviceGLES::QueryBegin(CGxQuery* query) {
    return false;
}

bool CGxDeviceGLES::QueryEnd(CGxQuery* query) {
    return false;
}

bool CGxDeviceGLES::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    data = 0;
    return false;
}

bool CGxDeviceGLES::QueryGetData(CGxQuery* query, uint32_t& data) {
    data = 0;
    return false;
}

void CGxDeviceGLES::IShaderReload(CGxShader* shader, const char* path, const char* name) {
}

void CGxDeviceGLES::IShaderCreate(CGxShader* shader) {
}

void CGxDeviceGLES::StereoSetConvergence(float convergence) {
}

float CGxDeviceGLES::StereoGetConvergence() {
    return 0.0f;
}

void CGxDeviceGLES::StereoSetSeparation(float separation) {
}

float CGxDeviceGLES::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceGLES::StereoEnabled() {
    return false;
}

void CGxDeviceGLES::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceGLES::DeviceUnk82() {
}

void CGxDeviceGLES::DeviceUnk83(uint32_t a, uint32_t b) {
}
