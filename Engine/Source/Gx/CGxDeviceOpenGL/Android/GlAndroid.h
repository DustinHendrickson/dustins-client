#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROID_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROID_H

// Android side of GlExt.h, in place of <OpenGL/gl.h>: the OpenGL device's desktop GL on OpenGL ES 3.
//
// Android has OpenGL ES only, which has no fixed-function pipeline, ARB programs, client-side texture formats like BGRA
// or (on most GPUs) DXT textures. The device calls about 120 GL 1.x and extension entry points; GlAndroid.cpp carries
// out each of them on ES 3, so the device builds unchanged, as on the Switch:
//
// - Fixed-function state (matrices, lights, material, fog, texture environments and coordinate generation, alpha test,
//   point sizes) is tracked, and each combination in use gets a generated GLSL ES program (GlAndroidPrograms.cpp).
// - ARB vertex and fragment programs (the game's .bls shaders) are translated to GLSL ES when loaded; their program.env
//   parameters live in uniform buffers shared by all programs.
// - Textures are converted on upload where ES lacks the format (BGRA, the reversed packed formats), and DXT is decoded
//   to RGBA on GPUs without S3TC (GlAndroidTextures.cpp).
//
// The core GL functions keep their names in the device code: each is renamed here to its GxEs_ implementation. The
// extension entry points are the device's function pointers (GlMac.h), filled from wglGetProcAddress, which returns
// the GxEs_ ones.

#include <GLES3/gl3.h>
#include <cstdint>

#if !defined(APIENTRY)
#define APIENTRY
#endif

typedef double GLdouble;
typedef double GLclampd;

// Desktop GL constants ES 3 doesn't have.
#define GL_ADD 0x0104
#define GL_ALPHA_SCALE 0x0D1C
#define GL_ALPHA_TEST 0x0BC0
#define GL_AMBIENT 0x1200
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_BGRA 0x80E1
#define GL_CLAMP 0x2900
#define GL_CLIP_PLANE0 0x3000
#define GL_COLOR_ARRAY 0x8076
#define GL_COLOR_MATERIAL 0x0B57
#define GL_COLOR_SUM 0x8458
#define GL_COMBINE 0x8570
#define GL_COMBINE_ALPHA 0x8572
#define GL_COMBINE_RGB 0x8571
#define GL_CONSTANT 0x8576
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_DECAL 0x2101
#define GL_DEPTH_COMPONENT32 0x81A7
#define GL_DIFFUSE 0x1201
#define GL_DOT3_RGB 0x86AE
#define GL_DOT3_RGBA 0x86AF
#define GL_EMISSION 0x1600
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_EYE_LINEAR 0x2400
#define GL_EYE_PLANE 0x2502
#define GL_FILL 0x1B02
#define GL_FOG 0x0B60
#define GL_FOG_COLOR 0x0B66
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_START 0x0B63
#define GL_INTERPOLATE 0x8575
#define GL_LIGHT0 0x4000
#define GL_LIGHTING 0x0B50
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LINE 0x1B01
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_MAX_CLIP_PLANES 0x0D32
#define GL_MAX_LIGHTS 0x0D31
#define GL_MODELVIEW 0x1700
#define GL_MODULATE 0x2100
#define GL_NORMALIZE 0x0BA1
#define GL_NORMAL_ARRAY 0x8075
#define GL_OBJECT_LINEAR 0x2401
#define GL_OBJECT_PLANE 0x2501
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND0_RGB 0x8590
#define GL_POINT_SIZE_MAX 0x8127
#define GL_POSITION 0x1203
#define GL_PREVIOUS 0x8578
#define GL_PRIMARY_COLOR 0x8577
#define GL_PROJECTION 0x1701
#define GL_Q 0x2003
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_R 0x2002
#define GL_RESCALE_NORMAL 0x803A
#define GL_RGB5 0x8050
#define GL_S 0x2000
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#define GL_SHININESS 0x1601
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE0_RGB 0x8580
#define GL_SPECULAR 0x1202
#define GL_SPHERE_MAP 0x2402
#define GL_SPOT_CUTOFF 0x1206
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SUBTRACT 0x84E7
#define GL_ADD_SIGNED 0x8574
#define GL_T 0x2001
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_TEXTURE_GEN_Q 0x0C63
#define GL_TEXTURE_GEN_R 0x0C62
#define GL_TEXTURE_GEN_S 0x0C60
#define GL_TEXTURE_GEN_T 0x0C61
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_VERTEX_ARRAY 0x8074
#define GL_FRAMEBUFFER_BINDING_EXT 0x8CA6
#define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#define GL_DEPTH24_STENCIL8_EXT 0x88F0
#define GL_STENCIL_ATTACHMENT_EXT 0x8D20

// The GL 1.x entry points the device calls, on ES 3 (GlAndroid.cpp).
void GxEs_glAlphaFunc(GLenum func, GLclampf ref);
void GxEs_glBindTexture(GLenum target, GLuint texture);
void GxEs_glBlendFunc(GLenum sfactor, GLenum dfactor);
void GxEs_glClear(GLbitfield mask);
void GxEs_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void GxEs_glClipPlane(GLenum plane, const GLdouble* equation);
void GxEs_glColor4fv(const GLfloat* v);
void GxEs_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void GxEs_glColorMaterial(GLenum face, GLenum mode);
void GxEs_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GxEs_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
void GxEs_glCullFace(GLenum mode);
void GxEs_glDeleteTextures(GLsizei n, const GLuint* textures);
void GxEs_glDepthFunc(GLenum func);
void GxEs_glDepthMask(GLboolean flag);
void GxEs_glDepthRange(GLclampd zNear, GLclampd zFar);
void GxEs_glDisable(GLenum cap);
void GxEs_glDisableClientState(GLenum array);
void GxEs_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void GxEs_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices);
void GxEs_glEnable(GLenum cap);
void GxEs_glEnableClientState(GLenum array);
void GxEs_glFinish();
void GxEs_glFogf(GLenum pname, GLfloat param);
void GxEs_glFogfv(GLenum pname, const GLfloat* params);
void GxEs_glFogi(GLenum pname, GLint param);
void GxEs_glFrontFace(GLenum mode);
void GxEs_glGenTextures(GLsizei n, GLuint* textures);
GLenum GxEs_glGetError();
void GxEs_glGetFloatv(GLenum pname, GLfloat* params);
void GxEs_glGetIntegerv(GLenum pname, GLint* params);
const GLubyte* GxEs_glGetString(GLenum name);
void GxEs_glHint(GLenum target, GLenum mode);
void GxEs_glLightModelfv(GLenum pname, const GLfloat* params);
void GxEs_glLightModeli(GLenum pname, GLint param);
void GxEs_glLightf(GLenum light, GLenum pname, GLfloat param);
void GxEs_glLightfv(GLenum light, GLenum pname, const GLfloat* params);
void GxEs_glLineWidth(GLfloat width);
void GxEs_glLoadIdentity();
void GxEs_glLoadMatrixf(const GLfloat* m);
void GxEs_glMaterialf(GLenum face, GLenum pname, GLfloat param);
void GxEs_glMaterialfv(GLenum face, GLenum pname, const GLfloat* params);
void GxEs_glMatrixMode(GLenum mode);
void GxEs_glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer);
void GxEs_glPixelStorei(GLenum pname, GLint param);
void GxEs_glPointSize(GLfloat size);
void GxEs_glPolygonMode(GLenum face, GLenum mode);
void GxEs_glPolygonOffset(GLfloat factor, GLfloat units);
void GxEs_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid* pixels);
void GxEs_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void GxEs_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GxEs_glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void GxEs_glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params);
void GxEs_glTexEnvi(GLenum target, GLenum pname, GLint param);
void GxEs_glTexEnviv(GLenum target, GLenum pname, const GLint* params);
void GxEs_glTexGenfv(GLenum coord, GLenum pname, const GLfloat* params);
void GxEs_glTexGeni(GLenum coord, GLenum pname, GLint param);
void GxEs_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels);
void GxEs_glTexParameteri(GLenum target, GLenum pname, GLint param);
void GxEs_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels);
void GxEs_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GxEs_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

// glCopyTexImage2D, which GlMac.h sends to MacGlCopyTexImage2D, is GlAndroidTextures.cpp's.

// The extension entry points by name, for wglGetProcAddress; null for any this GL doesn't have.
void* GlEsProcAddress(const char* name);

// The context (MacGlContext.cpp): state reset and the driver's limits read when one is made current, and the objects'
// bookkeeping dropped when it goes.
void GlEsContextCreated();
void GlEsContextDestroyed();
void GlEsTexturesReset();

// After each frame, before the swap: diagnostics (GlAndroidState.h Stats).
void GlEsFrameDone();
uint64_t GlEsNowNs();
void GlEsSwapTime(uint64_t ns);

#if !defined(WOWDECOMP_GLES_IMPLEMENTATION)
#define glAlphaFunc GxEs_glAlphaFunc
#define glBindTexture GxEs_glBindTexture
#define glBlendFunc GxEs_glBlendFunc
#define glClear GxEs_glClear
#define glClearColor GxEs_glClearColor
#define glClipPlane GxEs_glClipPlane
#define glColor4fv GxEs_glColor4fv
#define glColorMask GxEs_glColorMask
#define glColorMaterial GxEs_glColorMaterial
#define glColorPointer GxEs_glColorPointer
#define glCopyTexSubImage2D GxEs_glCopyTexSubImage2D
#define glCullFace GxEs_glCullFace
#define glDeleteTextures GxEs_glDeleteTextures
#define glDepthFunc GxEs_glDepthFunc
#define glDepthMask GxEs_glDepthMask
#define glDepthRange GxEs_glDepthRange
#define glDisable GxEs_glDisable
#define glDisableClientState GxEs_glDisableClientState
#define glDrawArrays GxEs_glDrawArrays
#define glDrawElements GxEs_glDrawElements
#define glEnable GxEs_glEnable
#define glEnableClientState GxEs_glEnableClientState
#define glFinish GxEs_glFinish
#define glFogf GxEs_glFogf
#define glFogfv GxEs_glFogfv
#define glFogi GxEs_glFogi
#define glFrontFace GxEs_glFrontFace
#define glGenTextures GxEs_glGenTextures
#define glGetError GxEs_glGetError
#define glGetFloatv GxEs_glGetFloatv
#define glGetIntegerv GxEs_glGetIntegerv
#define glGetString GxEs_glGetString
#define glHint GxEs_glHint
#define glLightModelfv GxEs_glLightModelfv
#define glLightModeli GxEs_glLightModeli
#define glLightf GxEs_glLightf
#define glLightfv GxEs_glLightfv
#define glLineWidth GxEs_glLineWidth
#define glLoadIdentity GxEs_glLoadIdentity
#define glLoadMatrixf GxEs_glLoadMatrixf
#define glMaterialf GxEs_glMaterialf
#define glMaterialfv GxEs_glMaterialfv
#define glMatrixMode GxEs_glMatrixMode
#define glNormalPointer GxEs_glNormalPointer
#define glPixelStorei GxEs_glPixelStorei
#define glPointSize GxEs_glPointSize
#define glPolygonMode GxEs_glPolygonMode
#define glPolygonOffset GxEs_glPolygonOffset
#define glReadPixels GxEs_glReadPixels
#define glScissor GxEs_glScissor
#define glTexCoordPointer GxEs_glTexCoordPointer
#define glTexEnvf GxEs_glTexEnvf
#define glTexEnvfv GxEs_glTexEnvfv
#define glTexEnvi GxEs_glTexEnvi
#define glTexEnviv GxEs_glTexEnviv
#define glTexGenfv GxEs_glTexGenfv
#define glTexGeni GxEs_glTexGeni
#define glTexImage2D GxEs_glTexImage2D
#define glTexParameteri GxEs_glTexParameteri
#define glTexSubImage2D GxEs_glTexSubImage2D
#define glVertexPointer GxEs_glVertexPointer
#define glViewport GxEs_glViewport
#endif

#endif
