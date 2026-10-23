// Textures for the OpenGL device on ES 3 (GlAndroid.h).
//
// The device uploads the formats Windows drivers take: BGRA bytes and the reversed packed 16-bit formats (ARGB4444 and
// ARGB1555 as the game keeps them), RGB565, and DXT1/3/5. ES 3 has RGBA bytes and the forward packed formats, so
// uploads are converted, row by row from the game's stride into a tight buffer; DXT goes to the GPU where it decodes
// S3TC, and is decoded to RGBA here where it doesn't (most phone GPUs).
//
// Render targets on the copy path (CGxDeviceOpenGl IDeviceSetRenderTargetCopy) are copied out of the pbuffer's
// framebuffer object. ES copies color with glCopyTexImage2D but not depth: shadow maps are blitted into a depth texture
// of the pbuffer's own depth format instead, and sample with depth comparison as on desktop.

#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidState.h"
#include <cstring>
#include <vector>

using namespace GlEs;

namespace {

const GLenum kTextureRectangle = 0x84F5;
const GLenum kTextureCubeMap = 0x8513;
const GLenum kGenerateMipmap = 0x8191;
const GLenum kMaxAnisotropy = 0x84FE;
const GLenum kCompareMode = 0x884C;
const GLenum kDepthTextureMode = 0x884B;
const GLenum kDxt1Rgb = 0x83F0;
const GLenum kDxt1 = 0x83F1;
const GLenum kDxt3 = 0x83F2;
const GLenum kDxt5 = 0x83F3;

enum Conversion {
    Conversion_None,        // uploaded as given (format and type are ES's)
    Conversion_Bgra8,       // BGRA bytes to RGBA
    Conversion_Argb4444,    // GL_UNSIGNED_SHORT_4_4_4_4_REV to GL_UNSIGNED_SHORT_4_4_4_4
    Conversion_Argb1555,    // GL_UNSIGNED_SHORT_1_5_5_5_REV to GL_UNSIGNED_SHORT_5_5_5_1
    Conversion_Dxt1,        // decoded to RGBA bytes
    Conversion_Dxt3,
    Conversion_Dxt5,
    Conversion_Compressed,  // DXT on a GPU with S3TC
};

std::vector<uint8_t> s_buffer;
GLuint s_copyFramebuffer;

GLenum BindingTarget(GLenum target) {
    if (target == kTextureRectangle) {
        return GL_TEXTURE_2D;
    }

    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        return GL_TEXTURE_CUBE_MAP;
    }

    return target;
}

GLenum ImageTarget(GLenum target) {
    return target == kTextureRectangle ? GL_TEXTURE_2D : target;
}

uint32_t BytesPerPixel(const Texture& texture) {
    switch (texture.type) {
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_HALF_FLOAT:
            return texture.format == GL_RGB && texture.type == GL_HALF_FLOAT ? 6 : 2;
        case GL_FLOAT:
            return texture.format == GL_RGB ? 12 : 4;
        default:
            return texture.format == GL_RGB ? 3 : 4;
    }
}

// Picks the ES format for a desktop internal format, data format and type, at the texture's creation.
void ChooseFormat(Texture& texture, GLint internalFormat, GLenum format, GLenum type, GLint* esInternalFormat) {
    texture.conversion = Conversion_None;
    texture.depth = false;

    if (format == GL_BGRA && (type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_BYTE)) {
        texture.conversion = Conversion_Bgra8;
        texture.format = GL_RGBA;
        texture.type = GL_UNSIGNED_BYTE;
        *esInternalFormat = GL_RGBA8;
    } else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_4_4_4_4_REV) {
        texture.conversion = Conversion_Argb4444;
        texture.format = GL_RGBA;
        texture.type = GL_UNSIGNED_SHORT_4_4_4_4;
        *esInternalFormat = GL_RGBA4;
    } else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
        texture.conversion = Conversion_Argb1555;
        texture.format = GL_RGBA;
        texture.type = GL_UNSIGNED_SHORT_5_5_5_1;
        *esInternalFormat = GL_RGB5_A1;
    } else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        texture.format = GL_RGB;
        texture.type = GL_UNSIGNED_SHORT_5_6_5;
        *esInternalFormat = GL_RGB565;
    } else if (format == GL_DEPTH_COMPONENT) {
        // The pbuffer's depth format (MacGlCreatePbuffer), so depth copies can be blitted.
        texture.depth = true;
        texture.format = GL_DEPTH_STENCIL;
        texture.type = GL_UNSIGNED_INT_24_8;
        *esInternalFormat = GL_DEPTH24_STENCIL8;
    } else if (format == GL_RGB && type == GL_HALF_FLOAT) {
        texture.format = GL_RGB;
        texture.type = GL_HALF_FLOAT;
        *esInternalFormat = GL_RGB16F;
    } else {
        texture.format = format == GL_BGRA ? GL_RGBA : format;
        texture.type = type;
        *esInternalFormat = internalFormat == GL_RGB5 ? GL_RGB565 : internalFormat;
    }
}

// The game's pixels for a width x height upload, as tight rows in the ES format.
const void* ConvertPixels(const Texture& texture, GLsizei width, GLsizei height, const void* pixels, GLenum sourceType) {
    State& s = Get();

    if (!pixels) {
        return nullptr;
    }

    uint32_t sourceBpp;

    switch (sourceType) {
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            sourceBpp = 2;
            break;
        case GL_UNSIGNED_INT_8_8_8_8_REV:
            sourceBpp = 4;
            break;
        default:
            sourceBpp = BytesPerPixel(texture);
            break;
    }

    uint32_t rowPixels = s.unpackRowLength > 0 ? static_cast<uint32_t>(s.unpackRowLength) : static_cast<uint32_t>(width);
    uint32_t alignment = s.unpackAlignment > 0 ? static_cast<uint32_t>(s.unpackAlignment) : 1;
    uint32_t sourceStride = (rowPixels * sourceBpp + alignment - 1) / alignment * alignment;
    uint32_t destStride = width * BytesPerPixel(texture);

    if (texture.conversion == Conversion_None && sourceStride == destStride) {
        return pixels;
    }

    s_buffer.resize(static_cast<size_t>(destStride) * height);
    const uint8_t* source = static_cast<const uint8_t*>(pixels);
    uint8_t* dest = s_buffer.data();

    for (GLsizei y = 0; y < height; y++, source += sourceStride, dest += destStride) {
        switch (texture.conversion) {
            case Conversion_Bgra8:
                for (GLsizei x = 0; x < width; x++) {
                    dest[x * 4 + 0] = source[x * 4 + 2];
                    dest[x * 4 + 1] = source[x * 4 + 1];
                    dest[x * 4 + 2] = source[x * 4 + 0];
                    dest[x * 4 + 3] = source[x * 4 + 3];
                }

                break;

            case Conversion_Argb4444: {
                const uint16_t* in = reinterpret_cast<const uint16_t*>(source);
                uint16_t* out = reinterpret_cast<uint16_t*>(dest);

                for (GLsizei x = 0; x < width; x++) {
                    out[x] = static_cast<uint16_t>(((in[x] & 0x0FFF) << 4) | (in[x] >> 12));
                }

                break;
            }

            case Conversion_Argb1555: {
                const uint16_t* in = reinterpret_cast<const uint16_t*>(source);
                uint16_t* out = reinterpret_cast<uint16_t*>(dest);

                for (GLsizei x = 0; x < width; x++) {
                    out[x] = static_cast<uint16_t>(((in[x] & 0x7FFF) << 1) | (in[x] >> 15));
                }

                break;
            }

            default:
                memcpy(dest, source, destStride);
                break;
        }
    }

    return s_buffer.data();
}

void Expand565(uint16_t color, uint8_t* rgb) {
    uint32_t r = (color >> 11) & 0x1F;
    uint32_t g = (color >> 5) & 0x3F;
    uint32_t b = color & 0x1F;
    rgb[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    rgb[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    rgb[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

// One 4x4 block's colors (the DXT1 part of every format) into RGBA; DXT1 has its 1-bit alpha mode.
void DecodeColorBlock(const uint8_t* block, uint8_t (*pixels)[4], bool dxt1) {
    uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    uint8_t colors[4][4];
    Expand565(c0, colors[0]);
    Expand565(c1, colors[1]);
    colors[0][3] = 255;
    colors[1][3] = 255;

    if (c0 > c1 || !dxt1) {
        for (int32_t i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i] + 1) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i] + 1) / 3);
        }

        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int32_t i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((colors[0][i] + colors[1][i]) / 2);
            colors[3][i] = 0;
        }

        colors[2][3] = 255;
        colors[3][3] = 0;
    }

    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (static_cast<uint32_t>(block[7]) << 24);

    for (int32_t i = 0; i < 16; i++) {
        memcpy(pixels[i], colors[(indices >> (i * 2)) & 3], 4);
    }
}

// Decodes width x height pixels of DXT blocks (rows of blocks, tight) to RGBA bytes in s_buffer.
const void* DecodeDxt(int32_t conversion, GLsizei width, GLsizei height, const void* data) {
    uint32_t blockBytes = conversion == Conversion_Dxt1 ? 8 : 16;
    uint32_t blocksWide = (width + 3) / 4;
    uint32_t blocksHigh = (height + 3) / 4;
    const uint8_t* block = static_cast<const uint8_t*>(data);

    s_buffer.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* out = s_buffer.data();

    for (uint32_t by = 0; by < blocksHigh; by++) {
        for (uint32_t bx = 0; bx < blocksWide; bx++, block += blockBytes) {
            uint8_t pixels[16][4];
            const uint8_t* colorBlock = conversion == Conversion_Dxt1 ? block : block + 8;
            DecodeColorBlock(colorBlock, pixels, conversion == Conversion_Dxt1);

            if (conversion == Conversion_Dxt3) {
                for (int32_t i = 0; i < 16; i++) {
                    uint32_t alpha = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                    pixels[i][3] = static_cast<uint8_t>(alpha * 17);
                }
            } else if (conversion == Conversion_Dxt5) {
                uint8_t alphas[8];
                alphas[0] = block[0];
                alphas[1] = block[1];

                if (alphas[0] > alphas[1]) {
                    for (int32_t i = 1; i < 7; i++) {
                        alphas[i + 1] = static_cast<uint8_t>(((7 - i) * alphas[0] + i * alphas[1] + 3) / 7);
                    }
                } else {
                    for (int32_t i = 1; i < 5; i++) {
                        alphas[i + 1] = static_cast<uint8_t>(((5 - i) * alphas[0] + i * alphas[1] + 2) / 5);
                    }

                    alphas[6] = 0;
                    alphas[7] = 255;
                }

                uint64_t indices = 0;

                for (int32_t i = 0; i < 6; i++) {
                    indices |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                }

                for (int32_t i = 0; i < 16; i++) {
                    pixels[i][3] = alphas[(indices >> (3 * i)) & 7];
                }
            }

            for (uint32_t py = 0; py < 4; py++) {
                uint32_t y = by * 4 + py;

                if (y >= static_cast<uint32_t>(height)) {
                    break;
                }

                for (uint32_t px = 0; px < 4; px++) {
                    uint32_t x = bx * 4 + px;

                    if (x < static_cast<uint32_t>(width)) {
                        memcpy(out + (static_cast<size_t>(y) * width + x) * 4, pixels[py * 4 + px], 4);
                    }
                }
            }
        }
    }

    return s_buffer.data();
}

void RecordSize(Texture& texture, GLint level, GLsizei width, GLsizei height) {
    texture.width = width << level;
    texture.height = height << level;
}

void GenerateMipmaps(const Texture& texture, GLenum target, GLint level) {
    if (!texture.generateMipmap || level != 0 || texture.conversion == Conversion_Compressed || texture.depth) {
        return;
    }

    // A cube map's levels are generated with its last face.
    if (target == GL_TEXTURE_CUBE_MAP_POSITIVE_X || (target > GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        return;
    }

    glGenerateMipmap(BindingTarget(target));
}

void SetCompare(Texture& texture, GLenum target, bool compare) {
    glTexParameteri(BindingTarget(target), GL_TEXTURE_COMPARE_MODE, compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);

    if (compare) {
        glTexParameteri(BindingTarget(target), GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }

    if (texture.compare != compare) {
        texture.compare = compare;
        Get().programDirty = true;
    }
}

// Copies a rectangle of the read framebuffer into the bound texture, as a blit: ES copies no depth with glCopyTex*, and
// drivers refuse (some silently) color copies between formats, like the window's RGB into the RGBA textures the game
// asks for. A blit converts between color formats. Depth needs the read framebuffer's format, which a depth texture
// has (ChooseFormat): the copy path's shadow maps.
void BlitToTexture(GLenum target, GLint level, bool depth, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    GLint readFramebuffer = 0;
    GLint drawFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);

    if (!s_copyFramebuffer) {
        glGenFramebuffers(1, &s_copyFramebuffer);
    }

    GLint texture = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &texture);

    GLenum attachment = depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_copyFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, target, texture, level);

    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        // Blits obey the scissor test; copies don't.
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

        if (scissor) {
            glDisable(GL_SCISSOR_TEST);
        }

        glBlitFramebuffer(x, y, x + width, y + height, xoffset, yoffset, xoffset + width, yoffset + height, depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);

        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        }
    }

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, target, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
}

// Diagnostics: a copy's middle pixel in the framebuffer and in the texture.
void TraceCopy(GLenum target, GLint x, GLint y, GLint textureX, GLint textureY) {
    if (!GetStats().enabled || GetStats().traceFrames <= 0 || target != GL_TEXTURE_2D) {
        return;
    }

    uint8_t source[4] = {};
    uint8_t copied[4] = {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, source);

    GLint texture = 0;
    GLint readFramebuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);

    if (!s_copyFramebuffer) {
        glGenFramebuffers(1, &s_copyFramebuffer);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_copyFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    glReadPixels(textureX, textureY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copied);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    Trace("  copy check: framebuffer %d pixel %d %d %d %d, texture %d (status %x) pixel %d %d %d %d", readFramebuffer, source[0], source[1], source[2], source[3], texture, status, copied[0], copied[1], copied[2], copied[3]);
}

}

namespace GlEs {

Texture& BoundTexture(GLenum target) {
    State& s = Get();
    TexUnit& unit = s.units[s.activeUnit];
    GLenum binding = BindingTarget(target);
    GLuint name = binding == GL_TEXTURE_CUBE_MAP ? unit.boundCube : unit.bound2D;
    Texture& texture = s.textures[name];
    texture.target = binding;
    return texture;
}

}

void GxEs_glGenTextures(GLsizei n, GLuint* textures) {
    glGenTextures(n, textures);
}

void GxEs_glDeleteTextures(GLsizei n, const GLuint* textures) {
    State& s = Get();

    for (GLsizei i = 0; i < n; i++) {
        s.textures.erase(textures[i]);

        for (TexUnit& unit : s.units) {
            if (unit.bound2D == textures[i]) {
                unit.bound2D = 0;
            }

            if (unit.boundCube == textures[i]) {
                unit.boundCube = 0;
            }
        }
    }

    glDeleteTextures(n, textures);
}

void GxEs_glBindTexture(GLenum target, GLuint name) {
    State& s = Get();
    TexUnit& unit = s.units[s.activeUnit];
    GLenum binding = BindingTarget(target);
    GLuint& bound = binding == GL_TEXTURE_CUBE_MAP ? unit.boundCube : unit.bound2D;

    if (bound != name) {
        // Fixed-function programs sample depth textures with comparison, so a unit changing between kinds changes them.
        auto previous = s.textures.find(bound);
        auto next = s.textures.find(name);
        bool previousCompare = previous != s.textures.end() && previous->second.compare;
        bool nextCompare = next != s.textures.end() && next->second.compare;

        if (previousCompare != nextCompare) {
            s.programDirty = true;
        }

        bound = name;
    }

    glBindTexture(binding, name);
}

void GxEs_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    State& s = Get();
    GLenum binding = BindingTarget(target);

    switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
            glTexParameteri(binding, pname, param == GL_CLAMP ? GL_CLAMP_TO_EDGE : param);
            break;

        case kGenerateMipmap:
            BoundTexture(target).generateMipmap = param != 0;
            break;

        case kMaxAnisotropy:
            if (s.anisotropic) {
                glTexParameterf(binding, pname, static_cast<GLfloat>(param));
            }

            break;

        case kCompareMode:
            SetCompare(BoundTexture(target), target, param != 0);
            break;

        case kDepthTextureMode:
            break;

        default:
            glTexParameteri(binding, pname, param);
            break;
    }
}

void GxEs_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels) {
    Texture& texture = BoundTexture(target);
    GLint esInternalFormat;
    ChooseFormat(texture, internalFormat, format, type, &esInternalFormat);
    RecordSize(texture, level, width, height);

    const void* data = ConvertPixels(texture, width, height, pixels, type);
    glTexImage2D(ImageTarget(target), level, esInternalFormat, width, height, 0, texture.format, texture.type, data);
    GenerateMipmaps(texture, target, level);
}

void GxEs_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels) {
    Texture& texture = BoundTexture(target);

    if (!texture.format) {
        GLint esInternalFormat;
        ChooseFormat(texture, 0, format, type, &esInternalFormat);
    }

    const void* data = ConvertPixels(texture, width, height, pixels, type);
    glTexSubImage2D(ImageTarget(target), level, xoffset, yoffset, width, height, texture.format, texture.type, data);
    GenerateMipmaps(texture, target, level);
}

void APIENTRY GxEs_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid* data) {
    State& s = Get();
    Texture& texture = BoundTexture(target);
    RecordSize(texture, level, width, height);

    if (s.s3tc) {
        texture.conversion = Conversion_Compressed;
        texture.format = internalFormat;
        texture.type = 0;
        glCompressedTexImage2D(ImageTarget(target), level, internalFormat, width, height, 0, imageSize, data);
        return;
    }

    texture.conversion = internalFormat == kDxt3 ? Conversion_Dxt3 : (internalFormat == kDxt5 ? Conversion_Dxt5 : Conversion_Dxt1);
    texture.format = GL_RGBA;
    texture.type = GL_UNSIGNED_BYTE;

    const void* pixels = data ? DecodeDxt(texture.conversion, width, height, data) : nullptr;
    glTexImage2D(ImageTarget(target), level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void APIENTRY GxEs_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid* data) {
    Texture& texture = BoundTexture(target);

    if (texture.conversion == Conversion_Compressed) {
        glCompressedTexSubImage2D(ImageTarget(target), level, xoffset, yoffset, width, height, format, imageSize, data);
        return;
    }

    if (texture.conversion < Conversion_Dxt1 || texture.conversion > Conversion_Dxt5) {
        return;
    }

    // The blocks may run past a small level's edge: only the part inside it is written.
    GLint levelWidth = texture.width >> level > 0 ? texture.width >> level : 1;
    GLint levelHeight = texture.height >> level > 0 ? texture.height >> level : 1;

    const void* pixels = DecodeDxt(texture.conversion, width, height, data);
    GLsizei copyWidth = xoffset + width > levelWidth && levelWidth > 0 ? levelWidth - xoffset : width;
    GLsizei copyHeight = yoffset + height > levelHeight && levelHeight > 0 ? levelHeight - yoffset : height;

    if (copyWidth <= 0 || copyHeight <= 0) {
        return;
    }

    if (copyWidth != width) {
        // Repack the rows to the narrower width.
        const uint8_t* source = static_cast<const uint8_t*>(pixels);
        std::vector<uint8_t> rows(static_cast<size_t>(copyWidth) * copyHeight * 4);

        for (GLsizei y = 0; y < copyHeight; y++) {
            memcpy(rows.data() + static_cast<size_t>(y) * copyWidth * 4, source + static_cast<size_t>(y) * width * 4, static_cast<size_t>(copyWidth) * 4);
        }

        glTexSubImage2D(ImageTarget(target), level, xoffset, yoffset, copyWidth, copyHeight, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        return;
    }

    glTexSubImage2D(ImageTarget(target), level, xoffset, yoffset, copyWidth, copyHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void MacGlCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    Texture& texture = BoundTexture(target);
    Trace("copy tex image %x level %d format %x %d,%d %dx%d", target, level, internalFormat, x, y, width, height);
    bool depth = internalFormat == GL_DEPTH_COMPONENT || internalFormat == GL_DEPTH_COMPONENT16 || internalFormat == GL_DEPTH_COMPONENT24 || internalFormat == GL_DEPTH_COMPONENT32;

    if (depth) {
        GLint esInternalFormat;
        ChooseFormat(texture, internalFormat, GL_DEPTH_COMPONENT, GL_FLOAT, &esInternalFormat);
        RecordSize(texture, level, width, height);
        glTexImage2D(ImageTarget(target), level, esInternalFormat, width, height, 0, texture.format, texture.type, nullptr);
        BlitToTexture(ImageTarget(target), level, true, 0, 0, x, y, width, height);

        // Shadow maps sample through SHADOW2D lookups, as on Windows' D3D path.
        SetCompare(texture, target, true);
        return;
    }

    texture.conversion = Conversion_None;
    texture.depth = false;
    texture.format = GL_RGBA;
    texture.type = GL_UNSIGNED_BYTE;
    RecordSize(texture, level, width, height);
    glTexImage2D(ImageTarget(target), level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    BlitToTexture(ImageTarget(target), level, false, 0, 0, x, y, width, height);
    CheckError("copy tex image");
    GenerateMipmaps(texture, target, level);
}

void GxEs_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    Texture& texture = BoundTexture(target);
    Trace("copy tex sub image %x level %d %d,%d from %d,%d %dx%d", target, level, xoffset, yoffset, x, y, width, height);

    if (texture.depth) {
        BlitToTexture(ImageTarget(target), level, true, xoffset, yoffset, x, y, width, height);
        return;
    }

    BlitToTexture(ImageTarget(target), level, false, xoffset, yoffset, x, y, width, height);
    CheckError("copy tex sub image");
    TraceCopy(target, x + width / 2, y + height / 2, xoffset + width / 2, yoffset + height / 2);
    GenerateMipmaps(texture, target, level);
}

void GlEsTexturesReset() {
    s_copyFramebuffer = 0;
    std::vector<uint8_t>().swap(s_buffer);
}
