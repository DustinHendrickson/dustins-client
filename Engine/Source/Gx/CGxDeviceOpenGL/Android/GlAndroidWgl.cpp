// WGL stand-ins for the shared OpenGL device code on Android (see Mac/GlMac.h), as on the Switch (Switch/GlSwitch.cpp):
// an HGLRC is the context from MacGlCreateContext and an HDC the window it draws to, or the framebuffer object that
// stands in for the device's pbuffer. The extension entry points come from the GL on ES 3 (GlAndroid.cpp).

#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceOpenGL/GlExt.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidContext.h"

namespace {

HDC s_currentDC;

struct AndroidPbuffer {
    GLuint m_framebuffer;
    GLuint m_color;
    GLuint m_depthStencil;
};

AndroidPbuffer* s_pbuffer;

void DeletePbuffer(AndroidPbuffer* pbuffer) {
    glDeleteFramebuffers(1, &pbuffer->m_framebuffer);
    glDeleteRenderbuffers(1, &pbuffer->m_color);
    glDeleteRenderbuffers(1, &pbuffer->m_depthStencil);
    delete pbuffer;
}

}

void* wglGetProcAddress(const char* name) {
    return name ? GlEsProcAddress(name) : nullptr;
}

HDC wglGetCurrentDC() {
    return AndroidGlCurrentContext() ? s_currentDC : nullptr;
}

HGLRC wglGetCurrentContext() {
    return AndroidGlCurrentContext();
}

HDC MacGlCreatePbuffer(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    AndroidPbuffer* pbuffer = new AndroidPbuffer();

    glGenRenderbuffers(1, &pbuffer->m_color);
    glBindRenderbuffer(GL_RENDERBUFFER, pbuffer->m_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);

    // Depth textures copied out of it are made in this format (GlAndroidTextures.cpp), so the copy can be a blit.
    glGenRenderbuffers(1, &pbuffer->m_depthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, pbuffer->m_depthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);

    glGenFramebuffers(1, &pbuffer->m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, pbuffer->m_framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, pbuffer->m_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pbuffer->m_depthStencil);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, previous);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        DeletePbuffer(pbuffer);
        return nullptr;
    }

    s_pbuffer = pbuffer;
    return reinterpret_cast<HDC>(pbuffer);
}

void MacGlDestroyPbuffer(HDC handle) {
    AndroidPbuffer* pbuffer = reinterpret_cast<AndroidPbuffer*>(handle);

    if (!pbuffer) {
        return;
    }

    GLint current = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current);

    if (static_cast<GLuint>(current) == pbuffer->m_framebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (s_pbuffer == pbuffer) {
        s_pbuffer = nullptr;
    }

    DeletePbuffer(pbuffer);
}

BOOL wglMakeCurrent(HDC dc, HGLRC context) {
    if (!context) {
        AndroidGlMakeCurrent(nullptr);
        s_currentDC = nullptr;
        return 1;
    }

    // There is one context: switching between the window and the pbuffer only rebinds the framebuffer.
    if (AndroidGlCurrentContext() != context && !AndroidGlMakeCurrent(context)) {
        return 0;
    }

    s_currentDC = dc;

    AndroidPbuffer* pbuffer = s_pbuffer && dc == reinterpret_cast<HDC>(s_pbuffer) ? s_pbuffer : nullptr;
    glBindFramebuffer(GL_FRAMEBUFFER, pbuffer ? pbuffer->m_framebuffer : 0);

    return 1;
}

BOOL wglSwapLayerBuffers(HDC dc, UINT planes) {
    AndroidGlSwapBuffers();
    return 1;
}
