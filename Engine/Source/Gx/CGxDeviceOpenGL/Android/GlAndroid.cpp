// The OpenGL device's desktop GL entry points on OpenGL ES 3 (GlAndroid.h): fixed-function and ARB program state kept
// here for the generated programs (GlAndroidPrograms.cpp), the rest passed on to ES, and the function table behind
// wglGetProcAddress.

#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidState.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace GlEs {

State& Get() {
    static State* s_state = new State();
    return *s_state;
}

uint64_t NowNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

Stats& GetStats() {
    static Stats s_stats;
    return s_stats;
}

void CheckError(const char* where) {
    if (!GetStats().enabled) {
        return;
    }

    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        __android_log_print(ANDROID_LOG_WARN, "WoW", "GL error 0x%x at %s (draw framebuffer %d status %x)", error, where, framebuffer, glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
    }
}

bool Tracing() {
    Stats& stats = GetStats();
    return stats.enabled && stats.traceFrames > 0 && stats.frames > 2;
}

void Trace(const char* format, ...) {
    if (!Tracing()) {
        return;
    }

    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, "WoW", "trace: %s", line);
}

void TraceDraw(GLenum mode, GLsizei count) {
    if (!Tracing()) {
        return;
    }

    State& s = Get();
    GLint fbo = 0;
    GLint viewport[4] = {};
    GLint scissor[4] = {};
    GLfloat depthRange[2] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetFloatv(GL_DEPTH_RANGE, depthRange);
    GLint depthFunc = 0;
    GLint cullFace = 0;
    GLint frontFace = 0;
    GLint blendSrc = 0;
    GLint blendDst = 0;
    GLboolean depthMask = 0;
    GLboolean colorMask[4] = {};
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);
    glGetIntegerv(GL_FRONT_FACE, &frontFace);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    Trace("draw mode %x count %d fbo %d vp %d,%d %dx%d sc %s %d,%d %dx%d depth %s %x mask %d range %.2f-%.2f cull %s %x front %x blend %s %x/%x color %d%d%d%d vp#%u fp#%u lit %d fog %d alpha %d tex0 %u/%d",
          mode, count, fbo, viewport[0], viewport[1], viewport[2], viewport[3], glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off", scissor[0], scissor[1], scissor[2], scissor[3],
          glIsEnabled(GL_DEPTH_TEST) ? "on" : "off", depthFunc, depthMask, depthRange[0], depthRange[1], glIsEnabled(GL_CULL_FACE) ? "on" : "off", cullFace, frontFace,
          glIsEnabled(GL_BLEND) ? "on" : "off", blendSrc, blendDst, colorMask[0], colorMask[1], colorMask[2], colorMask[3],
          s.vertexProgram ? s.boundVertexProgram : 0, s.fragmentProgram ? s.boundFragmentProgram : 0, s.lighting, s.fog, s.alphaTest, s.units[0].bound2D, s.units[0].enabled2D);
    for (int32_t unit = 0; unit < 4; unit++) {
        GLuint name = s.units[unit].bound2D;
        auto texture = s.textures.find(name);

        if (!name || texture == s.textures.end()) {
            continue;
        }

        GLint minFilter = 0;
        GLint maxLevel = 0;
        GLint baseLevel = 0;
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &baseLevel);
        Trace("  unit %d tex %u enabled %d conversion %d format %x type %x %dx%d genmip %d compare %d min %x levels %d-%d", unit, name, s.units[unit].enabled2D, texture->second.conversion, texture->second.format, texture->second.type, texture->second.width, texture->second.height, texture->second.generateMipmap, texture->second.compare, minFilter, baseLevel, maxLevel);
    }

    glActiveTexture(GL_TEXTURE0 + s.activeUnit);

    Trace("  vertex env c28-30: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f; fragment c0-1: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f; fog %.2f-%.2f", s.vertexEnv[28][0], s.vertexEnv[28][1], s.vertexEnv[28][2], s.vertexEnv[28][3], s.vertexEnv[29][0], s.vertexEnv[29][1], s.vertexEnv[29][2], s.vertexEnv[29][3], s.vertexEnv[30][0], s.vertexEnv[30][1], s.vertexEnv[30][2], s.vertexEnv[30][3], s.fragmentEnv[0][0], s.fragmentEnv[0][1], s.fragmentEnv[0][2], s.fragmentEnv[0][3], s.fragmentEnv[1][0], s.fragmentEnv[1][1], s.fragmentEnv[1][2], s.fragmentEnv[1][3], s.fogStart, s.fogEnd);
    TraceProgramEnv();
    Trace("  vertex env c2-5: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f", s.vertexEnv[2][0], s.vertexEnv[2][1], s.vertexEnv[2][2], s.vertexEnv[2][3], s.vertexEnv[3][0], s.vertexEnv[3][1], s.vertexEnv[3][2], s.vertexEnv[3][3], s.vertexEnv[4][0], s.vertexEnv[4][1], s.vertexEnv[4][2], s.vertexEnv[4][3], s.vertexEnv[5][0], s.vertexEnv[5][1], s.vertexEnv[5][2], s.vertexEnv[5][3]);
}

void FrameDone() {
    Stats& stats = GetStats();

    if (!stats.enabled) {
        return;
    }

    if (Tracing()) {
        __android_log_print(ANDROID_LOG_INFO, "WoW", "trace: ---- end of frame");
        stats.traceFrames--;
    }

    stats.frames++;
    CheckError("frame");

    uint64_t nowNs = NowNs();

    if (stats.lastFrameNs) {
        uint64_t frameNs = nowNs - stats.lastFrameNs;
        stats.worstFrameNs = frameNs > stats.worstFrameNs ? frameNs : stats.worstFrameNs;
        stats.longFrames += frameNs > 50000000ull;
    }

    stats.lastFrameNs = nowNs;

    static time_t s_last;
    time_t now = time(nullptr);

    if (now != s_last) {
        s_last = now;
        __android_log_print(ANDROID_LOG_INFO, "WoW", "GL: %u frames, worst %.0f ms, %u over 50 ms; %u draws (%u skipped, %u ARB vertex, %u ARB fragment, %u points); %u programs built (%u cached) in %.0f ms, textures %.0f ms, draw setup %.0f ms, swap %.0f ms",
                            stats.frames, stats.worstFrameNs / 1e6, stats.longFrames, stats.draws, stats.skipped, stats.arbVertex, stats.arbFragment, stats.points, stats.programsBuilt, stats.programsCached, stats.buildNs / 1e6, stats.textureNs / 1e6, stats.prepareNs / 1e6, stats.swapNs / 1e6);
        Stats reset;
        reset.enabled = true;
        reset.traceFrames = stats.traceFrames;
        reset.frames = stats.frames > 2 ? 3 : stats.frames;
        reset.lastFrameNs = stats.lastFrameNs;
        stats = reset;
    }
}

void MatrixMultiply(const float* a, const float* b, float* result) {
    float out[16];

    for (int32_t column = 0; column < 4; column++) {
        for (int32_t row = 0; row < 4; row++) {
            out[column * 4 + row] = a[0 * 4 + row] * b[column * 4 + 0] + a[1 * 4 + row] * b[column * 4 + 1] + a[2 * 4 + row] * b[column * 4 + 2] + a[3 * 4 + row] * b[column * 4 + 3];
        }
    }

    memcpy(result, out, sizeof(out));
}

bool MatrixInverse(const float* m, float* result) {
    float inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0.0f) {
        memcpy(result, m, sizeof(inv));
        return false;
    }

    det = 1.0f / det;

    for (int32_t i = 0; i < 16; i++) {
        result[i] = inv[i] * det;
    }

    return true;
}

void MatrixTransform(const float* m, const float* v, float* result) {
    float out[4];

    for (int32_t row = 0; row < 4; row++) {
        out[row] = m[0 * 4 + row] * v[0] + m[1 * 4 + row] * v[1] + m[2 * 4 + row] * v[2] + m[3 * 4 + row] * v[3];
    }

    memcpy(result, out, sizeof(out));
}

}

using namespace GlEs;

namespace {

const GLenum kVertexProgram = 0x8620;
const GLenum kFragmentProgram = 0x8804;
const GLenum kPointSprite = 0x8861;
const GLenum kVertexProgramPointSize = 0x8642;
const GLenum kTextureRectangle = 0x84F5;
const GLenum kTextureCubeMap = 0x8513;
const GLenum kMultisample = 0x809D;
const GLenum kCoordReplace = 0x8862;
const GLenum kRgbScale = 0x8573;
const GLenum kLightModelColorControl = 0x81F8;
const GLenum kSeparateSpecularColor = 0x81FA;

std::string s_extensions;
std::string s_version;

bool IsIdentity(const float* m) {
    static const float s_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    return !memcmp(m, s_identity, sizeof(s_identity));
}

void SetBool(bool& value, bool enable, bool program) {
    if (value != enable) {
        value = enable;

        if (program) {
            Get().programDirty = true;
        }
    }
}

TexUnit& ActiveUnit() {
    return Get().units[Get().activeUnit];
}

Array* ClientArray(GLenum array) {
    State& s = Get();

    switch (array) {
        case GL_VERTEX_ARRAY:
            return &s.vertexArray;
        case GL_NORMAL_ARRAY:
            return &s.normalArray;
        case GL_COLOR_ARRAY:
            return &s.colorArray;
        case GL_TEXTURE_COORD_ARRAY:
            return &s.units[s.clientActiveUnit].texCoord;
        default:
            return nullptr;
    }
}

void SetPointer(Array& array, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid* pointer) {
    array.size = size;
    array.type = type;
    array.normalized = normalized;
    array.stride = stride;
    array.pointer = pointer;
    array.buffer = Get().arrayBuffer;
}

float* MaterialColor(GLenum pname) {
    State& s = Get();

    switch (pname) {
        case GL_AMBIENT:
            return s.matAmbient;
        case GL_DIFFUSE:
            return s.matDiffuse;
        case GL_SPECULAR:
            return s.matSpecular;
        case GL_EMISSION:
            return s.matEmission;
        default:
            return nullptr;
    }
}

int32_t TexGenCoord(GLenum coord) {
    return coord >= GL_S && coord <= GL_Q ? static_cast<int32_t>(coord - GL_S) : -1;
}

void LogError(const char* format, const char* detail) {
    __android_log_print(ANDROID_LOG_WARN, "WoW", format, detail);
}

}

void GlEsContextCreated() {
    State& s = Get();
    s = State();

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s.maxTextureSize);
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &s.maxCubeMapSize);
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, s.pointSizeRange);

    std::string extensions;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);

    for (GLint i = 0; i < count; i++) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));

        if (!name) {
            continue;
        }

        if (!strcmp(name, "GL_EXT_texture_compression_s3tc") || !strcmp(name, "GL_EXT_texture_compression_dxt1")) {
            s.s3tc = s.s3tc || !strcmp(name, "GL_EXT_texture_compression_s3tc");
        } else if (!strcmp(name, "GL_EXT_texture_filter_anisotropic")) {
            s.anisotropic = true;
        }
    }

    if (s.anisotropic) {
        glGetFloatv(0x84FF, &s.maxAnisotropy);
    }

    __android_log_print(ANDROID_LOG_INFO, "WoW", "GL: S3TC %s, anisotropy %.0f, %d extensions", s.s3tc ? "on the GPU" : "decoded", s.maxAnisotropy, count);

    // What the device looks for (GlExt.cpp BindGlExtensions). DXT is always offered: it's decoded on upload where the
    // GPU lacks it. No occlusion queries (ES 3 counts no samples) and no rectangle textures.
    s_extensions = "GL_ARB_multitexture GL_EXT_bgra GL_SGIS_texture_edge_clamp GL_ARB_texture_compression "
                   "GL_EXT_texture_compression_s3tc GL_SGIS_generate_mipmap GL_ARB_vertex_buffer_object "
                   "GL_EXT_draw_range_elements GL_ARB_texture_env_combine GL_ARB_fragment_program GL_ARB_vertex_program "
                   "GL_EXT_gpu_program_parameters GL_ARB_texture_cube_map GL_ARB_point_parameters GL_ARB_point_sprite "
                   "GL_EXT_framebuffer_object GL_EXT_blend_color";

    if (s.anisotropic) {
        s_extensions += " GL_EXT_texture_filter_anisotropic";
    }

    s_version = std::string("1.4 (") + reinterpret_cast<const char*>(glGetString(GL_VERSION)) + ")";

    char property[PROP_VALUE_MAX] = {};
    __system_property_get("debug.wow.gl", property);
    GetStats().enabled = property[0] == '1';
    __system_property_get("debug.wow.gltrace", property);
    GetStats().traceFrames = atoi(property);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GlEsTexturesReset();
    ProgramsInitialize();
}

void GlEsFrameDone() {
    FrameDone();
}

void GlEsSwapTime(uint64_t ns) {
    if (GetStats().enabled) {
        GetStats().swapNs += ns;
    }
}

uint64_t GlEsNowNs() {
    return GetStats().enabled ? NowNs() : 0;
}

void GlEsContextDestroyed() {
    ProgramsReset();
    GlEsTexturesReset();
}

void GxEs_glEnable(GLenum cap) {
    State& s = Get();

    switch (cap) {
        case GL_LIGHTING:
            SetBool(s.lighting, true, true);
            break;
        case GL_COLOR_MATERIAL:
            SetBool(s.colorMaterial, true, true);
            break;
        case GL_FOG:
            SetBool(s.fog, true, true);
            break;
        case GL_ALPHA_TEST:
            SetBool(s.alphaTest, true, true);
            break;
        case GL_NORMALIZE:
        case GL_RESCALE_NORMAL:
            SetBool(s.normalize, true, true);
            break;
        case GL_TEXTURE_2D:
        case kTextureRectangle:
            SetBool(ActiveUnit().enabled2D, true, true);
            break;
        case kTextureCubeMap:
            SetBool(ActiveUnit().enabledCube, true, true);
            break;
        case GL_TEXTURE_GEN_S:
        case GL_TEXTURE_GEN_T:
        case GL_TEXTURE_GEN_R:
        case GL_TEXTURE_GEN_Q:
            SetBool(ActiveUnit().texGenEnabled[cap - GL_TEXTURE_GEN_S], true, true);
            break;
        case kVertexProgram:
            SetBool(s.vertexProgram, true, true);
            break;
        case kFragmentProgram:
            SetBool(s.fragmentProgram, true, true);
            break;
        case kPointSprite:
            SetBool(s.pointSprite, true, true);
            break;
        case kVertexProgramPointSize:
            SetBool(s.vertexProgramPointSize, true, true);
            break;
        case GL_BLEND:
        case GL_CULL_FACE:
        case GL_DEPTH_TEST:
        case GL_DITHER:
        case GL_POLYGON_OFFSET_FILL:
        case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST:
            glEnable(cap);
            break;
        default:
            if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kLights) {
                SetBool(s.lights[cap - GL_LIGHT0].enabled, true, true);
            }

            // Clip planes (the device has none with vertex programs), multisample (always on with a multisampled
            // surface), color sum.
            break;
    }
}

void GxEs_glDisable(GLenum cap) {
    State& s = Get();

    switch (cap) {
        case GL_LIGHTING:
            SetBool(s.lighting, false, true);
            break;
        case GL_COLOR_MATERIAL:
            SetBool(s.colorMaterial, false, true);
            break;
        case GL_FOG:
            SetBool(s.fog, false, true);
            break;
        case GL_ALPHA_TEST:
            SetBool(s.alphaTest, false, true);
            break;
        case GL_NORMALIZE:
        case GL_RESCALE_NORMAL:
            SetBool(s.normalize, false, true);
            break;
        case GL_TEXTURE_2D:
        case kTextureRectangle:
            SetBool(ActiveUnit().enabled2D, false, true);
            break;
        case kTextureCubeMap:
            SetBool(ActiveUnit().enabledCube, false, true);
            break;
        case GL_TEXTURE_GEN_S:
        case GL_TEXTURE_GEN_T:
        case GL_TEXTURE_GEN_R:
        case GL_TEXTURE_GEN_Q:
            SetBool(ActiveUnit().texGenEnabled[cap - GL_TEXTURE_GEN_S], false, true);
            break;
        case kVertexProgram:
            SetBool(s.vertexProgram, false, true);
            break;
        case kFragmentProgram:
            SetBool(s.fragmentProgram, false, true);
            break;
        case kPointSprite:
            SetBool(s.pointSprite, false, true);
            break;
        case kVertexProgramPointSize:
            SetBool(s.vertexProgramPointSize, false, true);
            break;
        case GL_BLEND:
        case GL_CULL_FACE:
        case GL_DEPTH_TEST:
        case GL_DITHER:
        case GL_POLYGON_OFFSET_FILL:
        case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST:
            glDisable(cap);
            break;
        default:
            if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kLights) {
                SetBool(s.lights[cap - GL_LIGHT0].enabled, false, true);
            }

            break;
    }
}

void GxEs_glEnableClientState(GLenum array) {
    if (Array* target = ClientArray(array)) {
        target->enabled = true;
    }
}

void GxEs_glDisableClientState(GLenum array) {
    if (Array* target = ClientArray(array)) {
        target->enabled = false;
    }
}

void GxEs_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(Get().vertexArray, size, type, GL_FALSE, stride, pointer);
}

void GxEs_glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer) {
    // Integer normals are scaled to [-1, 1], as GL does for normal arrays.
    SetPointer(Get().normalArray, 3, type, type != GL_FLOAT, stride, pointer);
}

void GxEs_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(Get().colorArray, size, type, type != GL_FLOAT, stride, pointer);
}

void GxEs_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    State& s = Get();
    SetPointer(s.units[s.clientActiveUnit].texCoord, size, type, GL_FALSE, stride, pointer);
}

void GxEs_glColor4fv(const GLfloat* v) {
    memcpy(Get().currentColor, v, sizeof(float) * 4);
}

void GxEs_glMatrixMode(GLenum mode) {
    Get().matrixMode = mode;
}

void GxEs_glLoadMatrixf(const GLfloat* m) {
    State& s = Get();

    switch (s.matrixMode) {
        case GL_MODELVIEW:
            memcpy(s.modelView, m, sizeof(s.modelView));
            Touch(Group_Transform);
            break;

        case GL_PROJECTION:
            memcpy(s.projection, m, sizeof(s.projection));
            Touch(Group_Transform);
            break;

        case GL_TEXTURE: {
            TexUnit& unit = ActiveUnit();
            memcpy(unit.matrix, m, sizeof(unit.matrix));
            SetBool(unit.matrixIdentity, IsIdentity(m), true);
            Touch(Group_TexMatrix);
            break;
        }

        default:
            break;
    }
}

void GxEs_glLoadIdentity() {
    static const float s_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    GxEs_glLoadMatrixf(s_identity);
}

void GxEs_glLightfv(GLenum lightName, GLenum pname, const GLfloat* params) {
    State& s = Get();

    if (lightName < GL_LIGHT0 || lightName >= GL_LIGHT0 + kLights) {
        return;
    }

    Light& light = s.lights[lightName - GL_LIGHT0];

    switch (pname) {
        case GL_AMBIENT:
            memcpy(light.ambient, params, sizeof(light.ambient));
            break;

        case GL_DIFFUSE:
            memcpy(light.diffuse, params, sizeof(light.diffuse));
            break;

        case GL_SPECULAR:
            memcpy(light.specular, params, sizeof(light.specular));
            break;

        case GL_POSITION: {
            // Kept in eye space, as transformed by the modelview matrix at the time.
            bool directional = light.position[3] == 0.0f;
            MatrixTransform(s.modelView, params, light.position);

            if (directional != (light.position[3] == 0.0f)) {
                s.programDirty = true;
            }

            break;
        }

        case GL_CONSTANT_ATTENUATION:
        case GL_LINEAR_ATTENUATION:
        case GL_QUADRATIC_ATTENUATION:
            light.attenuation[pname - GL_CONSTANT_ATTENUATION] = params[0];
            break;

        default:
            break;
    }

    Touch(Group_Lights);
}

void GxEs_glLightf(GLenum light, GLenum pname, GLfloat param) {
    GxEs_glLightfv(light, pname, &param);
}

void GxEs_glLightModelfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_LIGHT_MODEL_AMBIENT) {
        memcpy(Get().lightModelAmbient, params, sizeof(float) * 4);
        Touch(Group_Lights);
    }
}

void GxEs_glLightModeli(GLenum pname, GLint param) {
    State& s = Get();

    if (pname == GL_LIGHT_MODEL_LOCAL_VIEWER) {
        SetBool(s.localViewer, param != 0, true);
    } else if (pname == kLightModelColorControl) {
        SetBool(s.separateSpecular, static_cast<GLenum>(param) == kSeparateSpecularColor, true);
    }
}

void GxEs_glMaterialfv(GLenum face, GLenum pname, const GLfloat* params) {
    State& s = Get();

    if (face == GL_BACK) {
        return;
    }

    if (pname == GL_AMBIENT_AND_DIFFUSE) {
        memcpy(s.matAmbient, params, sizeof(float) * 4);
        memcpy(s.matDiffuse, params, sizeof(float) * 4);
    } else if (pname == GL_SHININESS) {
        s.matShininess = params[0];
    } else if (float* color = MaterialColor(pname)) {
        memcpy(color, params, sizeof(float) * 4);
    }

    Touch(Group_Material);
}

void GxEs_glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    GxEs_glMaterialfv(face, pname, &param);
}

void GxEs_glColorMaterial(GLenum face, GLenum mode) {
    State& s = Get();

    if (s.colorMaterialMode != mode) {
        s.colorMaterialMode = mode;
        s.programDirty = true;
    }
}

void GxEs_glFogi(GLenum pname, GLint param) {
    State& s = Get();

    if (pname == GL_FOG_MODE) {
        if (s.fogMode != static_cast<GLenum>(param)) {
            s.fogMode = param;
            s.programDirty = true;
        }
    } else {
        GxEs_glFogf(pname, static_cast<GLfloat>(param));
    }
}

void GxEs_glFogf(GLenum pname, GLfloat param) {
    State& s = Get();

    switch (pname) {
        case GL_FOG_MODE:
            GxEs_glFogi(pname, static_cast<GLint>(param));
            return;
        case GL_FOG_DENSITY:
            s.fogDensity = param;
            break;
        case GL_FOG_START:
            s.fogStart = param;
            break;
        case GL_FOG_END:
            s.fogEnd = param;
            break;
        default:
            return;
    }

    Touch(Group_Fog);
}

void GxEs_glFogfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_FOG_COLOR) {
        memcpy(Get().fogColor, params, sizeof(float) * 4);
        Touch(Group_Fog);
    } else {
        GxEs_glFogf(pname, params[0]);
    }
}

void GxEs_glAlphaFunc(GLenum func, GLclampf ref) {
    State& s = Get();

    if (s.alphaFunc != func) {
        s.alphaFunc = func;
        s.programDirty = true;
    }

    s.alphaRef = ref;
    Touch(Group_Alpha);
}

void GxEs_glTexEnvi(GLenum target, GLenum pname, GLint param) {
    State& s = Get();

    if (target == kPointSprite) {
        if (pname == kCoordReplace) {
            SetBool(ActiveUnit().coordReplace, param != 0, true);
        }

        return;
    }

    if (target != GL_TEXTURE_ENV) {
        return;
    }

    TexEnv& env = ActiveUnit().env;
    GLenum value = static_cast<GLenum>(param);

    if (pname == GL_TEXTURE_ENV_MODE) {
        env.mode = value;
    } else if (pname == GL_COMBINE_RGB) {
        env.combineRgb = value;
    } else if (pname == GL_COMBINE_ALPHA) {
        env.combineAlpha = value;
    } else if (pname >= GL_SOURCE0_RGB && pname < GL_SOURCE0_RGB + 3) {
        env.sourceRgb[pname - GL_SOURCE0_RGB] = value;
    } else if (pname >= GL_SOURCE0_ALPHA && pname < GL_SOURCE0_ALPHA + 3) {
        env.sourceAlpha[pname - GL_SOURCE0_ALPHA] = value;
    } else if (pname >= GL_OPERAND0_RGB && pname < GL_OPERAND0_RGB + 3) {
        env.operandRgb[pname - GL_OPERAND0_RGB] = value;
    } else if (pname >= GL_OPERAND0_ALPHA && pname < GL_OPERAND0_ALPHA + 3) {
        env.operandAlpha[pname - GL_OPERAND0_ALPHA] = value;
    } else if (pname == kRgbScale || pname == GL_ALPHA_SCALE) {
        GxEs_glTexEnvf(target, pname, static_cast<GLfloat>(param));
        return;
    } else {
        return;
    }

    s.programDirty = true;
}

void GxEs_glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    if (target != GL_TEXTURE_ENV) {
        GxEs_glTexEnvi(target, pname, static_cast<GLint>(param));
        return;
    }

    TexEnv& env = ActiveUnit().env;

    if (pname == kRgbScale) {
        if (env.scaleRgb != param) {
            env.scaleRgb = param;
            Get().programDirty = true;
        }
    } else if (pname == GL_ALPHA_SCALE) {
        if (env.scaleAlpha != param) {
            env.scaleAlpha = param;
            Get().programDirty = true;
        }
    } else {
        GxEs_glTexEnvi(target, pname, static_cast<GLint>(param));
    }
}

void GxEs_glTexEnviv(GLenum target, GLenum pname, const GLint* params) {
    GxEs_glTexEnvi(target, pname, params[0]);
}

void GxEs_glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params) {
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR) {
        memcpy(ActiveUnit().env.color, params, sizeof(float) * 4);
        Touch(Group_TexEnv);
    } else {
        GxEs_glTexEnvf(target, pname, params[0]);
    }
}

void GxEs_glTexGeni(GLenum coord, GLenum pname, GLint param) {
    int32_t index = TexGenCoord(coord);

    if (index < 0 || pname != GL_TEXTURE_GEN_MODE) {
        return;
    }

    TexUnit& unit = ActiveUnit();

    if (unit.texGenMode[index] != static_cast<GLenum>(param)) {
        unit.texGenMode[index] = param;
        Get().programDirty = true;
    }
}

void GxEs_glTexGenfv(GLenum coord, GLenum pname, const GLfloat* params) {
    State& s = Get();
    int32_t index = TexGenCoord(coord);

    if (index < 0) {
        return;
    }

    TexUnit& unit = ActiveUnit();

    if (pname == GL_OBJECT_PLANE) {
        memcpy(unit.objectPlane[index], params, sizeof(float) * 4);
    } else if (pname == GL_EYE_PLANE) {
        // The plane times the inverse of the modelview matrix at the time (row vector).
        float inverse[16];
        MatrixInverse(s.modelView, inverse);

        for (int32_t j = 0; j < 4; j++) {
            unit.eyePlane[index][j] = params[0] * inverse[j * 4 + 0] + params[1] * inverse[j * 4 + 1] + params[2] * inverse[j * 4 + 2] + params[3] * inverse[j * 4 + 3];
        }
    } else if (pname == GL_TEXTURE_GEN_MODE) {
        GxEs_glTexGeni(coord, pname, static_cast<GLint>(params[0]));
        return;
    } else {
        return;
    }

    Touch(Group_TexGen);
}

void GxEs_glPointSize(GLfloat size) {
    Get().pointSize = size;
    Touch(Group_Point);
}

void GxEs_glClipPlane(GLenum plane, const GLdouble* equation) {
    // The device has no clip planes with vertex programs (CGxDeviceOpenGl ISetCaps), and this GL reports none.
}

void GxEs_glPolygonMode(GLenum face, GLenum mode) {
    // No wireframe on ES.
}

void GxEs_glHint(GLenum target, GLenum mode) {
}

void GxEs_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
}

void GxEs_glClear(GLbitfield mask) {
    GLint fbo = 0;

    if (GetStats().enabled) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    }

    Trace("clear %x fbo %d", mask, fbo);
    glClear(mask);
}

void GxEs_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glClearColor(red, green, blue, alpha);
}

void GxEs_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    glColorMask(red, green, blue, alpha);
}

void GxEs_glCullFace(GLenum mode) {
    glCullFace(mode);
}

void GxEs_glDepthFunc(GLenum func) {
    glDepthFunc(func);
}

void GxEs_glDepthMask(GLboolean flag) {
    glDepthMask(flag);
}

void GxEs_glDepthRange(GLclampd zNear, GLclampd zFar) {
    glDepthRangef(static_cast<float>(zNear), static_cast<float>(zFar));
}

void GxEs_glFrontFace(GLenum mode) {
    glFrontFace(mode);
}

void GxEs_glLineWidth(GLfloat width) {
    glLineWidth(width);
}

void GxEs_glPolygonOffset(GLfloat factor, GLfloat units) {
    glPolygonOffset(factor, units);
}

void GxEs_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
}

void GxEs_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
}

void GxEs_glFinish() {
    glFinish();
}

GLenum GxEs_glGetError() {
    return glGetError();
}

void GxEs_glPixelStorei(GLenum pname, GLint param) {
    State& s = Get();

    // Uploads are converted from the game's rows into tight ones (GlAndroidTextures.cpp), which read these.
    if (pname == GL_UNPACK_ROW_LENGTH) {
        s.unpackRowLength = param;
    } else if (pname == GL_UNPACK_ALIGNMENT) {
        s.unpackAlignment = param;
    } else {
        glPixelStorei(pname, param);
    }
}

void GxEs_glGetIntegerv(GLenum pname, GLint* params) {
    State& s = Get();

    switch (pname) {
        case 0x84E2:    // GL_MAX_TEXTURE_UNITS_ARB
            *params = kUnits;
            break;
        case 0x8872: {  // GL_MAX_TEXTURE_IMAGE_UNITS_ARB
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, params);
            *params = *params > 16 ? 16 : *params;
            break;
        }
        case GL_MAX_TEXTURE_SIZE:
            *params = s.maxTextureSize;
            break;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
            *params = s.maxCubeMapSize;
            break;
        case 0x84F8:    // GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB
            *params = 0;
            break;
        case GL_MAX_VERTEX_ATTRIBS:
            *params = kAttribs;
            break;
        case 0x84FF:    // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
            *params = static_cast<GLint>(s.maxAnisotropy);
            break;
        case GL_MAX_CLIP_PLANES:
            *params = 0;
            break;
        case GL_MAX_LIGHTS:
            *params = kLights;
            break;
        case 0x864B:    // GL_PROGRAM_ERROR_POSITION_ARB
            *params = s.programErrorPosition;
            break;
        case 0x8CA6:    // GL_FRAMEBUFFER_BINDING_EXT
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, params);
            break;
        default:
            glGetIntegerv(pname, params);
            break;
    }
}

void GxEs_glGetFloatv(GLenum pname, GLfloat* params) {
    State& s = Get();

    switch (pname) {
        case GL_POINT_SIZE_MAX:
            *params = s.pointSizeRange[1];
            break;
        case 0x84FD:    // GL_MAX_TEXTURE_LOD_BIAS_EXT
            *params = 0.0f;
            break;
        case 0x84FF:    // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
            *params = s.maxAnisotropy;
            break;
        default:
            glGetFloatv(pname, params);
            break;
    }
}

const GLubyte* GxEs_glGetString(GLenum name) {
    if (name == GL_EXTENSIONS) {
        return reinterpret_cast<const GLubyte*>(s_extensions.c_str());
    }

    if (name == GL_VERSION) {
        return reinterpret_cast<const GLubyte*>(s_version.c_str());
    }

    return glGetString(name);
}

void GxEs_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid* pixels) {
    if (format == GL_DEPTH_COMPONENT) {
        // ES reads no depth: report the far plane (nothing in front).
        float* depths = static_cast<float*>(pixels);

        for (GLsizei i = 0; i < width * height; i++) {
            depths[i] = 1.0f;
        }

        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (format == GL_BGRA) {
        uint8_t* bytes = static_cast<uint8_t*>(pixels);

        for (GLsizei i = 0; i < width * height; i++) {
            uint8_t red = bytes[i * 4];
            bytes[i * 4] = bytes[i * 4 + 2];
            bytes[i * 4 + 2] = red;
        }
    }
}

void GxEs_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (PrepareDraw(mode)) {
        TraceDraw(mode, count);
        glDrawArrays(mode, first, count);
        CheckError("draw");
    }
}

void GxEs_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices) {
    if (PrepareDraw(mode)) {
        TraceDraw(mode, count);
        glDrawElements(mode, count, type, indices);
        CheckError("draw");
    }
}

// Extension entry points, returned by GlEsProcAddress.

namespace {

void APIENTRY EsActiveTexture(GLenum texture) {
    State& s = Get();
    s.activeUnit = static_cast<int32_t>(texture - GL_TEXTURE0);
    glActiveTexture(texture);
}

void APIENTRY EsClientActiveTexture(GLenum texture) {
    Get().clientActiveUnit = static_cast<int32_t>(texture - GL_TEXTURE0);
}

void APIENTRY EsBindBuffer(GLenum target, GLuint buffer) {
    State& s = Get();

    if (target == GL_ARRAY_BUFFER) {
        s.arrayBuffer = buffer;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        s.elementArrayBuffer = buffer;
    }

    glBindBuffer(target, buffer);
}

void APIENTRY EsDeleteBuffers(GLsizei n, const GLuint* buffers) {
    State& s = Get();

    for (GLsizei i = 0; i < n; i++) {
        if (s.arrayBuffer == buffers[i]) {
            s.arrayBuffer = 0;
        }

        if (s.elementArrayBuffer == buffers[i]) {
            s.elementArrayBuffer = 0;
        }
    }

    glDeleteBuffers(n, buffers);
}

void APIENTRY EsGenBuffers(GLsizei n, GLuint* buffers) {
    glGenBuffers(n, buffers);
}

void APIENTRY EsBufferData(GLenum target, GLsizeiptr size, const GLvoid* data, GLenum usage) {
    glBufferData(target, size, data, usage);
}

void APIENTRY EsBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid* data) {
    glBufferSubData(target, offset, size, data);
}

GLvoid* APIENTRY EsMapBuffer(GLenum target, GLenum access) {
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    return size > 0 ? glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT) : nullptr;
}

GLboolean APIENTRY EsUnmapBuffer(GLenum target) {
    return glUnmapBuffer(target);
}

void APIENTRY EsDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid* indices) {
    if (PrepareDraw(mode)) {
        TraceDraw(mode, count);
        glDrawRangeElements(mode, start, end, count, type, indices);
        CheckError("draw");
    }
}

void APIENTRY EsVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid* pointer) {
    if (index < static_cast<GLuint>(kAttribs)) {
        SetPointer(Get().attribs[index], size, type, normalized, stride, pointer);
    }
}

void APIENTRY EsEnableVertexAttribArray(GLuint index) {
    if (index < static_cast<GLuint>(kAttribs)) {
        Get().attribs[index].enabled = true;
    }
}

void APIENTRY EsDisableVertexAttribArray(GLuint index) {
    if (index < static_cast<GLuint>(kAttribs)) {
        Get().attribs[index].enabled = false;
    }
}

void APIENTRY EsGenPrograms(GLsizei n, GLuint* programs) {
    State& s = Get();

    for (GLsizei i = 0; i < n; i++) {
        programs[i] = s.nextProgram++;
        s.programs[programs[i]];
    }
}

void APIENTRY EsDeletePrograms(GLsizei n, const GLuint* programs) {
    State& s = Get();

    for (GLsizei i = 0; i < n; i++) {
        s.programs.erase(programs[i]);

        if (s.boundVertexProgram == programs[i]) {
            s.boundVertexProgram = 0;
        }

        if (s.boundFragmentProgram == programs[i]) {
            s.boundFragmentProgram = 0;
        }
    }

    s.programDirty = true;
}

GLboolean APIENTRY EsIsProgram(GLuint program) {
    return Get().programs.count(program) ? GL_TRUE : GL_FALSE;
}

void APIENTRY EsBindProgram(GLenum target, GLuint program) {
    State& s = Get();
    GLuint& bound = target == kVertexProgram ? s.boundVertexProgram : s.boundFragmentProgram;

    if (bound != program) {
        bound = program;
        s.programDirty = true;
    }

    if (program) {
        s.programs[program].target = target;
    }
}

void APIENTRY EsProgramString(GLenum target, GLenum format, GLsizei len, const GLvoid* string) {
    State& s = Get();
    GLuint bound = target == kVertexProgram ? s.boundVertexProgram : s.boundFragmentProgram;

    if (!bound) {
        s.programErrorPosition = 0;
        return;
    }

    ArbProgram& program = s.programs[bound];
    program = ArbProgram();
    program.target = target;
    program.serial = s.nextSerial++;
    TranslateArbProgram(program, static_cast<const char*>(string), static_cast<size_t>(len));
    s.programErrorPosition = program.errorPosition;
    s.programDirty = true;

    if (!program.valid) {
        std::string text(static_cast<const char*>(string), static_cast<size_t>(len));
        LogError("ARB program not translated (%s):", program.error.c_str());
        LogError("%s", text.c_str());
    }
}

void SetEnv(GLenum target, GLuint index, GLsizei count, const GLfloat* params) {
    State& s = Get();
    bool vertex = target == kVertexProgram;
    float (*env)[4] = vertex ? s.vertexEnv : s.fragmentEnv;
    uint32_t* stamps = vertex ? s.vertexEnvStamps : s.fragmentEnvStamps;

    if (index >= static_cast<GLuint>(kEnvParams)) {
        return;
    }

    if (index + count > static_cast<GLuint>(kEnvParams)) {
        count = kEnvParams - index;
    }

    // Only parameters whose values change are stamped: the device sends whole ranges each draw.
    uint32_t stamp = ++s.envStamp;

    for (GLsizei i = 0; i < count; i++, params += 4) {
        float* value = env[index + i];

        if (memcmp(value, params, sizeof(float) * 4)) {
            memcpy(value, params, sizeof(float) * 4);
            stamps[index + i] = stamp;
        }
    }
}

void APIENTRY EsProgramEnvParameter4fv(GLenum target, GLuint index, const GLfloat* params) {
    SetEnv(target, index, 1, params);
}

void APIENTRY EsProgramEnvParameters4fv(GLenum target, GLuint index, GLsizei count, const GLfloat* params) {
    SetEnv(target, index, count, params);
}

void APIENTRY EsProgramLocalParameter4fv(GLenum target, GLuint index, const GLfloat* params) {
    // The device rewrites program.local to program.env in every program it loads (IVertexShaderPatch,
    // IPixelShaderPatch) and never sets local parameters.
}

void APIENTRY EsProgramLocalParameters4fv(GLenum target, GLuint index, GLsizei count, const GLfloat* params) {
}

void APIENTRY EsGetProgramiv(GLenum target, GLenum pname, GLint* params) {
    State& s = Get();
    GLuint bound = target == kVertexProgram ? s.boundVertexProgram : s.boundFragmentProgram;

    switch (pname) {
        case 0x88B5:    // GL_MAX_PROGRAM_ENV_PARAMETERS_ARB
        case 0x88B4:    // GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB
        case 0x88AB:    // GL_MAX_PROGRAM_NATIVE_PARAMETERS_ARB
            *params = kEnvParams;
            break;
        case 0x8677:    // GL_PROGRAM_BINDING_ARB
            *params = static_cast<GLint>(bound);
            break;
        case 0x88B6: {  // GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB
            auto found = s.programs.find(bound);
            *params = found != s.programs.end() && found->second.valid ? 1 : 0;
            break;
        }
        default:
            *params = 0;
            break;
    }
}

void APIENTRY EsPointParameterf(GLenum pname, GLfloat param) {
    State& s = Get();

    if (pname == 0x8126) {          // GL_POINT_SIZE_MIN_ARB
        s.pointSizeMin = param;
    } else if (pname == 0x8127) {   // GL_POINT_SIZE_MAX_ARB
        s.pointSizeMax = param;
    }

    Touch(Group_Point);
}

void APIENTRY EsPointParameterfv(GLenum pname, const GLfloat* params) {
    if (pname == 0x8129) {          // GL_POINT_DISTANCE_ATTENUATION_ARB
        memcpy(Get().pointAttenuation, params, sizeof(float) * 3);
        Touch(Group_Point);
    } else {
        EsPointParameterf(pname, params[0]);
    }
}

void APIENTRY EsBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glBlendColor(red, green, blue, alpha);
}

void APIENTRY EsBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    glBindRenderbuffer(target, renderbuffer);
}

void APIENTRY EsDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) {
    glDeleteRenderbuffers(n, renderbuffers);
}

void APIENTRY EsGenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
    glGenRenderbuffers(n, renderbuffers);
}

void APIENTRY EsRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    glRenderbufferStorage(target, internalformat, width, height);
}

void APIENTRY EsBindFramebuffer(GLenum target, GLuint framebuffer) {
    glBindFramebuffer(target, framebuffer);
}

void APIENTRY EsDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
    glDeleteFramebuffers(n, framebuffers);
}

void APIENTRY EsGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    glGenFramebuffers(n, framebuffers);
}

GLenum APIENTRY EsCheckFramebufferStatus(GLenum target) {
    return glCheckFramebufferStatus(target);
}

void APIENTRY EsFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    // A depth texture made for the window's depth and stencil format attaches as both.
    if (attachment == GL_DEPTH_ATTACHMENT && texture) {
        auto found = Get().textures.find(texture);

        if (found != Get().textures.end() && found->second.format == GL_DEPTH_STENCIL) {
            attachment = GL_DEPTH_STENCIL_ATTACHMENT;
        }
    }

    glFramebufferTexture2D(target, attachment, textarget == kTextureRectangle ? GL_TEXTURE_2D : textarget, texture, level);
}

void APIENTRY EsFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}

void APIENTRY EsGenQueries(GLsizei n, GLuint* ids) {
    glGenQueries(n, ids);
}

void APIENTRY EsDeleteQueries(GLsizei n, const GLuint* ids) {
    glDeleteQueries(n, ids);
}

void APIENTRY EsBeginQuery(GLenum target, GLuint id) {
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, id);
}

void APIENTRY EsEndQuery(GLenum target) {
    glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
}

void APIENTRY EsGetQueryiv(GLenum target, GLenum pname, GLint* params) {
    *params = 32;
}

void APIENTRY EsGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
    glGetQueryObjectuiv(id, pname, params);
}

void APIENTRY EsGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    GLuint value = 0;
    glGetQueryObjectuiv(id, pname, &value);
    *params = static_cast<GLint>(value);
}

}

// GlAndroidTextures.cpp.
void APIENTRY GxEs_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid* data);
void APIENTRY GxEs_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid* data);

void* GlEsProcAddress(const char* name) {
    struct Entry {
        const char* name;
        void* function;
    };

    static const Entry s_entries[] = {
        { "glActiveTextureARB", reinterpret_cast<void*>(EsActiveTexture) },
        { "glClientActiveTextureARB", reinterpret_cast<void*>(EsClientActiveTexture) },
        { "glCompressedTexImage2DARB", reinterpret_cast<void*>(GxEs_glCompressedTexImage2D) },
        { "glCompressedTexSubImage2DARB", reinterpret_cast<void*>(GxEs_glCompressedTexSubImage2D) },
        { "glBindBufferARB", reinterpret_cast<void*>(EsBindBuffer) },
        { "glDeleteBuffersARB", reinterpret_cast<void*>(EsDeleteBuffers) },
        { "glGenBuffersARB", reinterpret_cast<void*>(EsGenBuffers) },
        { "glBufferDataARB", reinterpret_cast<void*>(EsBufferData) },
        { "glBufferSubDataARB", reinterpret_cast<void*>(EsBufferSubData) },
        { "glMapBufferARB", reinterpret_cast<void*>(EsMapBuffer) },
        { "glUnmapBufferARB", reinterpret_cast<void*>(EsUnmapBuffer) },
        { "glDrawRangeElementsEXT", reinterpret_cast<void*>(EsDrawRangeElements) },
        { "glProgramStringARB", reinterpret_cast<void*>(EsProgramString) },
        { "glBindProgramARB", reinterpret_cast<void*>(EsBindProgram) },
        { "glProgramEnvParameter4fvARB", reinterpret_cast<void*>(EsProgramEnvParameter4fv) },
        { "glProgramEnvParameters4fvEXT", reinterpret_cast<void*>(EsProgramEnvParameters4fv) },
        { "glProgramLocalParameter4fvARB", reinterpret_cast<void*>(EsProgramLocalParameter4fv) },
        { "glProgramLocalParameters4fvEXT", reinterpret_cast<void*>(EsProgramLocalParameters4fv) },
        { "glDeleteProgramsARB", reinterpret_cast<void*>(EsDeletePrograms) },
        { "glGenProgramsARB", reinterpret_cast<void*>(EsGenPrograms) },
        { "glIsProgramARB", reinterpret_cast<void*>(EsIsProgram) },
        { "glGetProgramivARB", reinterpret_cast<void*>(EsGetProgramiv) },
        { "glVertexAttribPointerARB", reinterpret_cast<void*>(EsVertexAttribPointer) },
        { "glEnableVertexAttribArrayARB", reinterpret_cast<void*>(EsEnableVertexAttribArray) },
        { "glDisableVertexAttribArrayARB", reinterpret_cast<void*>(EsDisableVertexAttribArray) },
        { "glGenQueriesARB", reinterpret_cast<void*>(EsGenQueries) },
        { "glDeleteQueriesARB", reinterpret_cast<void*>(EsDeleteQueries) },
        { "glBeginQueryARB", reinterpret_cast<void*>(EsBeginQuery) },
        { "glEndQueryARB", reinterpret_cast<void*>(EsEndQuery) },
        { "glGetQueryivARB", reinterpret_cast<void*>(EsGetQueryiv) },
        { "glGetQueryObjectivARB", reinterpret_cast<void*>(EsGetQueryObjectiv) },
        { "glGetQueryObjectuivARB", reinterpret_cast<void*>(EsGetQueryObjectuiv) },
        { "glPointParameterfARB", reinterpret_cast<void*>(EsPointParameterf) },
        { "glPointParameterfvARB", reinterpret_cast<void*>(EsPointParameterfv) },
        { "glBindRenderbufferEXT", reinterpret_cast<void*>(EsBindRenderbuffer) },
        { "glDeleteRenderbuffersEXT", reinterpret_cast<void*>(EsDeleteRenderbuffers) },
        { "glGenRenderbuffersEXT", reinterpret_cast<void*>(EsGenRenderbuffers) },
        { "glRenderbufferStorageEXT", reinterpret_cast<void*>(EsRenderbufferStorage) },
        { "glBindFramebufferEXT", reinterpret_cast<void*>(EsBindFramebuffer) },
        { "glDeleteFramebuffersEXT", reinterpret_cast<void*>(EsDeleteFramebuffers) },
        { "glGenFramebuffersEXT", reinterpret_cast<void*>(EsGenFramebuffers) },
        { "glCheckFramebufferStatusEXT", reinterpret_cast<void*>(EsCheckFramebufferStatus) },
        { "glFramebufferTexture2DEXT", reinterpret_cast<void*>(EsFramebufferTexture2D) },
        { "glFramebufferRenderbufferEXT", reinterpret_cast<void*>(EsFramebufferRenderbuffer) },
        { "glBlendColorEXT", reinterpret_cast<void*>(EsBlendColor) },
    };

    for (const Entry& entry : s_entries) {
        if (!strcmp(entry.name, name)) {
            return entry.function;
        }
    }

    return nullptr;
}
