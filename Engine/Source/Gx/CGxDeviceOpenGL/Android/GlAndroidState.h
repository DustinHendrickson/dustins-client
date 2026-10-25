#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDSTATE_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_ANDROID_GLANDROIDSTATE_H

// The desktop GL state GlAndroid.cpp keeps for the OpenGL device (see GlAndroid.h), shared by its parts: the entry
// points (GlAndroid.cpp), the generated and translated programs (GlAndroidPrograms.cpp) and textures
// (GlAndroidTextures.cpp). Only the GL thread (the game's) touches it.

#define WOWDECOMP_GLES_IMPLEMENTATION 1
#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroid.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace GlEs {

const int32_t kUnits = 8;          // fixed-function texture units and texture coordinate sets
const int32_t kSamplers = 16;      // texture units ARB fragment programs may sample
const int32_t kLights = 8;
const int32_t kAttribs = 16;
const int32_t kEnvParams = 256;

// Fixed attribute locations: the device's own generic attribute numbers (CGxDeviceOpenGl IStateSyncVertexPtrs), which
// the generated fixed-function shaders use for the conventional arrays too.
const int32_t kLocPosition = 0;
const int32_t kLocNormal = 3;
const int32_t kLocColor = 4;
const int32_t kLocSecondaryColor = 5;
const int32_t kLocTexCoord0 = 6;

// Varyings between the stages, as bits of a mask: what the fragment shader reads, so the vertex shader writes only that.
enum Varying {
    Varying_Color0 = 1 << 0,
    Varying_Color1 = 1 << 1,
    Varying_Fog = 1 << 2,
    Varying_Tex0 = 1 << 3,      // ... Varying_Tex0 << 7
};

struct Array {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    const GLvoid* pointer = nullptr;
    GLuint buffer = 0;
};

struct Light {
    bool enabled = false;
    float ambient[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float diffuse[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float specular[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float position[4] = { 0.0f, 0.0f, 1.0f, 0.0f };   // eye space
    float attenuation[3] = { 1.0f, 0.0f, 0.0f };
};

// A texture unit's environment (GL_TEXTURE_ENV): legacy mode or ARB_texture_env_combine.
struct TexEnv {
    GLenum mode = GL_MODULATE;
    GLenum combineRgb = GL_MODULATE;
    GLenum combineAlpha = GL_MODULATE;
    GLenum sourceRgb[3] = { GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT };
    GLenum sourceAlpha[3] = { GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT };
    GLenum operandRgb[3] = { GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA };
    GLenum operandAlpha[3] = { GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA };
    float scaleRgb = 1.0f;
    float scaleAlpha = 1.0f;
    float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct TexUnit {
    bool enabled2D = false;
    bool enabledCube = false;
    GLuint bound2D = 0;
    GLuint boundCube = 0;
    TexEnv env;
    bool texGenEnabled[4] = {};
    GLenum texGenMode[4] = { GL_EYE_LINEAR, GL_EYE_LINEAR, GL_EYE_LINEAR, GL_EYE_LINEAR };
    float eyePlane[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    float objectPlane[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    float matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    bool matrixIdentity = true;
    bool coordReplace = false;
    Array texCoord;
};

// What GL keeps per texture that ES doesn't, or stores differently.
struct Texture {
    GLenum target = 0;
    GLenum format = 0;          // the ES upload format and type chosen at creation (GlAndroidTextures.cpp)
    GLenum type = 0;
    int32_t conversion = 0;     // Conversion in GlAndroidTextures.cpp
    bool generateMipmap = false;
    bool compare = false;       // depth comparison: sampled as a shadow texture
    bool depth = false;
    GLsizei width = 0;          // level 0's size, for the sizes of the others
    GLsizei height = 0;
};

// An ARB program as given to glProgramStringARB, and its translation.
struct ArbProgram {
    GLenum target = 0;
    uint32_t serial = 0;        // changes with every glProgramStringARB, for the program cache
    bool valid = false;
    int32_t errorPosition = -1;
    std::string error;          // why it wasn't translated
    std::string glsl;           // the body: declarations and main(), without the stage's interface (GlAndroidPrograms)
    uint32_t inputs = 0;        // vertex: attribute locations read; fragment: varyings read
    uint32_t outputs = 0;       // vertex: varyings written
    uint32_t samplers[kSamplers] = {};  // fragment: per unit, Sampler in GlAndroidPrograms.cpp
    int32_t envCount = 0;       // program.env parameters it reads: [0, envCount)
    bool fogLinear = false;     // fragment: OPTION ARB_fog_linear
    bool pointSize = false;     // vertex: writes result.pointsize
};

// Groups of uniforms, each with a version that changes when anything in it does, so a program uploads only what
// changed since it last drew.
enum Group {
    Group_Transform,
    Group_TexMatrix,
    Group_Lights,
    Group_Material,
    Group_Fog,
    Group_Alpha,
    Group_TexEnv,
    Group_TexGen,
    Group_Point,
    Group_Count,
};

struct State {
    // Matrices (column major, as GL takes them).
    GLenum matrixMode = 0x1700;     // GL_MODELVIEW
    float modelView[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    // Fixed-function enables.
    bool lighting = false;
    bool colorMaterial = false;
    bool fog = false;
    bool alphaTest = false;
    bool normalize = false;
    bool pointSprite = false;
    bool vertexProgram = false;
    bool fragmentProgram = false;
    bool vertexProgramPointSize = false;

    // Lighting and material.
    Light lights[kLights];
    float lightModelAmbient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    bool localViewer = false;
    bool separateSpecular = false;
    GLenum colorMaterialMode = 0x1602;  // GL_AMBIENT_AND_DIFFUSE
    float matAmbient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    float matDiffuse[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
    float matSpecular[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float matEmission[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float matShininess = 0.0f;
    float currentColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Fog, alpha test, points.
    GLenum fogMode = 0x0800;    // GL_EXP
    float fogDensity = 1.0f;
    float fogStart = 0.0f;
    float fogEnd = 1.0f;
    float fogColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    GLenum alphaFunc = GL_ALWAYS;
    float alphaRef = 0.0f;
    float pointSize = 1.0f;
    float pointSizeMin = 0.0f;
    float pointSizeMax = 64.0f;
    float pointAttenuation[3] = { 1.0f, 0.0f, 0.0f };

    // Textures.
    int32_t activeUnit = 0;
    int32_t clientActiveUnit = 0;
    TexUnit units[kUnits];
    std::unordered_map<GLuint, Texture> textures;
    GLint unpackRowLength = 0;
    GLint unpackAlignment = 4;

    // Vertex arrays: the conventional ones (position, normal, color; texture coordinates are per unit) and the generic
    // ones of vertex programs.
    Array vertexArray;
    Array normalArray;
    Array colorArray;
    Array attribs[kAttribs];
    GLuint arrayBuffer = 0;
    GLuint elementArrayBuffer = 0;

    // ARB programs and their parameters.
    std::unordered_map<GLuint, ArbProgram> programs;
    GLuint nextProgram = 1;
    uint32_t nextSerial = 1;
    GLuint boundVertexProgram = 0;
    GLuint boundFragmentProgram = 0;
    GLint programErrorPosition = -1;
    // program.env, with the stamp of each parameter's last change (from envStamp), so a program uploads only what
    // changed since it last drew.
    float vertexEnv[kEnvParams][4] = {};
    float fragmentEnv[kEnvParams][4] = {};
    uint32_t vertexEnvStamps[kEnvParams] = {};
    uint32_t fragmentEnvStamps[kEnvParams] = {};
    uint32_t envStamp = 1;

    // Changes that pick a different program, and the uniform groups' versions.
    bool programDirty = true;
    uint32_t versions[Group_Count] = {};

    // What the driver offers.
    bool s3tc = false;
    bool anisotropic = false;
    float maxAnisotropy = 1.0f;
    float pointSizeRange[2] = { 1.0f, 64.0f };
    GLint maxTextureSize = 2048;
    GLint maxCubeMapSize = 2048;
};

State& Get();

// Diagnostics, on when the system property debug.wow.gl is 1 (adb shell setprop debug.wow.gl 1): GL errors and draw
// counts, logged once a second (GlAndroid.cpp).
struct Stats {
    bool enabled = false;
    uint32_t draws = 0;
    uint32_t skipped = 0;
    uint32_t arbVertex = 0;
    uint32_t arbFragment = 0;
    uint32_t points = 0;
    uint32_t programsBuilt = 0;
    uint32_t programsCached = 0;   // of those, loaded from the program cache
    uint32_t frames = 0;
    int32_t traceFrames = 0;
    // Time, in nanoseconds since the last report: frames (the worst one and how many ran long), and the work in them
    // that can make one long: building programs, uploading textures (DXT decoding included), the per-draw setup, and the
    // swap (where the driver waits for the GPU).
    uint64_t lastFrameNs = 0;
    uint64_t worstFrameNs = 0;
    uint32_t longFrames = 0;
    uint64_t buildNs = 0;
    uint64_t textureNs = 0;
    uint64_t prepareNs = 0;
    uint64_t swapNs = 0;    // debug.wow.gltrace=N: every draw of the N frames after the first second, in detail
};

Stats& GetStats();
void CheckError(const char* where);
void FrameDone();
uint64_t NowNs();
void TraceDraw(GLenum mode, GLsizei count);
void TraceProgramEnv();
void Trace(const char* format, ...);

inline void Touch(Group group) {
    Get().versions[group]++;
}

// GlAndroidPrograms.cpp: picks, builds and binds the program for the current state and the draw's primitive, uploads
// its uniforms and sets up the vertex attributes. False if no program could be built (the draw is skipped).
bool PrepareDraw(GLenum mode);

// GlAndroidPrograms.cpp: translates an ARB program's text.
void TranslateArbProgram(ArbProgram& program, const char* text, size_t length);

// GlAndroidPrograms.cpp: creates the uniform buffers once a context exists; drops every cached program (the context
// went away).
void ProgramsInitialize();
void ProgramsReset();

// GlAndroidTextures.cpp: what the texture bound to the active unit's target is, created on first use.
Texture& BoundTexture(GLenum target);

// Math helpers (GlAndroid.cpp).
void MatrixMultiply(const float* a, const float* b, float* result);
bool MatrixInverse(const float* m, float* result);
void MatrixTransform(const float* m, const float* v, float* result);

}

#endif
