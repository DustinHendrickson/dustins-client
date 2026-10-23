// Programs for the OpenGL device on ES 3 (GlAndroid.h): the fixed-function pipeline as generated GLSL ES, ARB vertex
// and fragment programs translated to GLSL ES, and the link of the two stages a draw uses.
//
// A draw's program is picked by a key built from the state that changes the code: for each stage, either its ARB
// program or the fixed-function settings (lights in use, color material, texture coordinate generation, each texture
// unit's target and environment, fog mode, alpha test), plus whether points are drawn. Keys and their linked programs are
// cached for the life of the context. The vertex shader is made for its fragment shader, writing only the varyings it
// reads, which matters on tiled mobile GPUs.
//
// Values that don't change the code are uniforms, uploaded per group when the group changed since the program last drew
// (GlAndroidState.h Group). The ARB programs' program.env parameters are two uniform buffers that every program shares.
//
// The ARB translator covers what the game's shaders use (Cg 1.x output): the arbvp1 and arbfp1 instruction sets with
// saturation, swizzles, write masks, negation, relative addressing of parameter arrays through A0, and the
// ARB_fog_linear and ARB_fragment_program_shadow options. Each instruction becomes one GLSL statement on vec4s.

#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidState.h"
#include <android/log.h>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace GlEs;

namespace {

const GLenum kVertexProgram = 0x8620;

enum Sampler {
    Sampler_None = 0,
    Sampler_2D,
    Sampler_Cube,
    Sampler_Rect,
    Sampler_Shadow,
};

enum Uniform {
    U_Mvp,
    U_Mv,
    U_NormalMatrix,
    U_TexMatrix,
    U_LightAmbient,
    U_LightDiffuse,
    U_LightSpecular,
    U_LightPosition,
    U_LightAttenuation,
    U_SceneAmbient,
    U_MatAmbient,
    U_MatDiffuse,
    U_MatSpecular,
    U_MatEmission,
    U_MatShininess,
    U_FogParams,
    U_FogColor,
    U_AlphaRef,
    U_TexEnvColor,
    U_EyePlane,
    U_ObjectPlane,
    U_PointParams,
    U_PointAttenuation,
    U_Count,
};

const char* const kUniformNames[U_Count] = {
    "u_mvp", "u_mv", "u_normalMatrix", "u_texMatrix", "u_lightAmbient", "u_lightDiffuse", "u_lightSpecular",
    "u_lightPosition", "u_lightAttenuation", "u_sceneAmbient", "u_matAmbient", "u_matDiffuse", "u_matSpecular",
    "u_matEmission", "u_matShininess", "u_fogParams", "u_fogColor", "u_alphaRef", "u_texEnvColor", "u_eyePlane",
    "u_objectPlane", "u_pointParams", "u_pointAttenuation",
};

struct LinkedProgram {
    GLuint id = 0;
    bool valid = false;
    GLint locations[U_Count];
    uint32_t versions[Group_Count];
    uint32_t attribs = 0;       // attribute locations read
    bool arbVertex = false;     // generic arrays (vertex program) rather than the conventional ones
};

// GLES state as last set by PrepareDraw, to skip calls that change nothing.
struct AttribCache {
    bool enabled = false;
    GLuint buffer = 0xFFFFFFFF;
    GLint size = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    const GLvoid* pointer = nullptr;
    float constant[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
};

std::unordered_map<std::string, std::unique_ptr<LinkedProgram>> s_programs;
LinkedProgram* s_current;
GLuint s_usedProgram;
bool s_lastPoints;
GLuint s_vertexEnvBuffer;
GLuint s_fragmentEnvBuffer;
AttribCache s_attribCache[kAttribs];

std::string Format(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::string Float(float value) {
    if (std::isnan(value)) {
        return "0.0";
    }

    if (std::isinf(value)) {
        return value > 0.0f ? "1e38" : "-1e38";
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9g", value);

    if (!strpbrk(buffer, ".e")) {
        strcat(buffer, ".0");
    }

    return buffer;
}

std::string Vec4(const float* v) {
    return "vec4(" + Float(v[0]) + ", " + Float(v[1]) + ", " + Float(v[2]) + ", " + Float(v[3]) + ")";
}

void Log(const char* format, const char* detail) {
    __android_log_print(ANDROID_LOG_WARN, "WoW", format, detail);
    fprintf(stderr, format, detail);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------------------------------------------------------------
// The ARB translator.

struct ParamElement {
    bool literal = true;
    float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int32_t env = 0;
};

struct Symbol {
    enum Kind {
        Temp,
        Address,
        Param,          // one element
        ParamArray,
        Binding,        // ATTRIB / OUTPUT / ALIAS: another name for a binding or symbol
    } kind;

    std::vector<ParamElement> elements;
    std::string binding;
    int32_t function = -1;      // ParamArray: its relative-addressing function, once emitted
};

class Translator {
    public:
    Translator(ArbProgram& program, bool vertex) : m_program(program), m_vertex(vertex) {
    }

    bool Translate(const char* text, size_t length);

    const std::string& Error() const {
        return m_error;
    }

    private:
    ArbProgram& m_program;
    bool m_vertex;
    std::map<std::string, Symbol> m_symbols;
    std::string m_declarations;     // temps, functions (before arb_main)
    std::string m_body;             // arb_main's statements
    std::string m_error;
    int32_t m_functions = 0;

    std::string Env(int32_t index) {
        return Format("%s[%d]", m_vertex ? "u_vertexEnv" : "u_fragmentEnv", index);
    }

    bool ParseFloat(const std::string& text, float* value);
    bool ParseLiteral(const std::string& text, float* value);
    bool ParseParamElements(const std::string& text, std::vector<ParamElement>& elements, bool array);
    bool DeclareParam(const std::string& text);
    std::string ElementExpression(const ParamElement& element);
    std::string BindingExpression(const std::string& binding, bool destination, std::string* mask);
    bool Operand(const std::string& text, bool destination, std::string* expression, std::string* swizzle, bool* negate);
    std::string Source(const std::string& text);
    bool Instruction(const std::string& opcode, const std::vector<std::string>& operands);
    bool Statement(const std::string& statement);
};

std::string Trim(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return "";
    }

    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Splits on commas outside brackets and braces.
std::vector<std::string> SplitOperands(const std::string& text) {
    std::vector<std::string> parts;
    int32_t depth = 0;
    std::string current;

    for (char c : text) {
        if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
        }

        if (c == ',' && depth == 0) {
            parts.push_back(Trim(current));
            current.clear();
        } else {
            current += c;
        }
    }

    if (!Trim(current).empty()) {
        parts.push_back(Trim(current));
    }

    return parts;
}

bool IsSwizzle(const std::string& part) {
    if (part.empty() || part.size() > 4) {
        return false;
    }

    bool xyzw = part.find_first_not_of("xyzw") == std::string::npos;
    bool rgba = part.find_first_not_of("rgba") == std::string::npos;
    return xyzw || rgba;
}

std::string NormalizeSwizzle(std::string swizzle) {
    for (char& c : swizzle) {
        switch (c) {
            case 'r': c = 'x'; break;
            case 'g': c = 'y'; break;
            case 'b': c = 'z'; break;
            case 'a': c = 'w'; break;
            default: break;
        }
    }

    return swizzle;
}

bool Translator::ParseFloat(const std::string& text, float* value) {
    std::string trimmed = Trim(text);
    char* end = nullptr;
    *value = strtof(trimmed.c_str(), &end);
    return end && end != trimmed.c_str() && Trim(end).empty();
}

// {x}, {x, y}, ... (missing components are 0, w 1) or a bare scalar (all four components).
bool Translator::ParseLiteral(const std::string& text, float* value) {
    std::string trimmed = Trim(text);

    if (!trimmed.empty() && trimmed[0] == '{') {
        size_t close = trimmed.find('}');

        if (close == std::string::npos) {
            return false;
        }

        std::vector<std::string> parts = SplitOperands(trimmed.substr(1, close - 1));

        if (parts.empty() || parts.size() > 4) {
            return false;
        }

        value[0] = 0.0f;
        value[1] = 0.0f;
        value[2] = 0.0f;
        value[3] = 1.0f;

        for (size_t i = 0; i < parts.size(); i++) {
            if (!ParseFloat(parts[i], &value[i])) {
                return false;
            }
        }

        return true;
    }

    float scalar;

    if (!ParseFloat(trimmed, &scalar)) {
        return false;
    }

    value[0] = value[1] = value[2] = value[3] = scalar;
    return true;
}

// The elements of a PARAM's value: literals, program.env[n] / program.local[n], or ranges [a..b] of them.
bool Translator::ParseParamElements(const std::string& text, std::vector<ParamElement>& elements, bool array) {
    std::string trimmed = Trim(text);

    if (array) {
        if (trimmed.empty() || trimmed[0] != '{' || trimmed.back() != '}') {
            return false;
        }

        for (const std::string& part : SplitOperands(trimmed.substr(1, trimmed.size() - 2))) {
            if (!ParseParamElements(part, elements, false)) {
                return false;
            }
        }

        return true;
    }

    if (!trimmed.compare(0, 12, "program.env[") || !trimmed.compare(0, 14, "program.local[")) {
        size_t open = trimmed.find('[');
        size_t close = trimmed.find(']');

        if (close == std::string::npos) {
            return false;
        }

        std::string range = trimmed.substr(open + 1, close - open - 1);
        int32_t first = 0;
        int32_t last = 0;
        size_t dots = range.find("..");

        if (dots != std::string::npos) {
            first = atoi(range.substr(0, dots).c_str());
            last = atoi(range.substr(dots + 2).c_str());
        } else {
            first = last = atoi(range.c_str());
        }

        if (first < 0 || last >= kEnvParams || last < first) {
            return false;
        }

        for (int32_t i = first; i <= last; i++) {
            ParamElement element;
            element.literal = false;
            element.env = i;
            elements.push_back(element);
        }

        return true;
    }

    ParamElement element;

    if (!ParseLiteral(trimmed, element.value)) {
        m_error = "unsupported PARAM binding: " + trimmed;
        return false;
    }

    elements.push_back(element);
    return true;
}

bool Translator::DeclareParam(const std::string& text) {
    size_t equals = text.find('=');

    if (equals == std::string::npos) {
        return false;
    }

    std::string name = Trim(text.substr(0, equals));
    std::string value = Trim(text.substr(equals + 1));
    bool array = false;
    size_t bracket = name.find('[');

    if (bracket != std::string::npos) {
        array = true;
        name = Trim(name.substr(0, bracket));
    }

    Symbol symbol;
    symbol.kind = array ? Symbol::ParamArray : Symbol::Param;

    if (!ParseParamElements(value, symbol.elements, array) || symbol.elements.empty()) {
        return false;
    }

    // A single PARAM may also be given a one-element binding like program.env[3] without braces.
    if (!array && symbol.elements.size() != 1) {
        symbol.kind = Symbol::ParamArray;
    }

    m_symbols[name] = symbol;
    return true;
}

std::string Translator::ElementExpression(const ParamElement& element) {
    return element.literal ? Vec4(element.value) : Env(element.env);
}

// A binding (vertex.*, fragment.*, result.*, program.*): its GLSL expression, recording the program's inputs and
// outputs. *mask gets a trailing swizzle or mask, if any.
std::string Translator::BindingExpression(const std::string& text, bool destination, std::string* mask) {
    // Split the trailing swizzle from the binding: the last dotted part, if it's one and isn't part of the binding.
    std::string binding = text;
    mask->clear();
    size_t dot = binding.rfind('.');

    if (dot != std::string::npos && binding.find(']', dot) == std::string::npos && IsSwizzle(binding.substr(dot + 1))) {
        std::string head = binding.substr(0, dot);

        // "result.color.a" and the like: the head must still be a binding with a dot in it.
        if (head.find('.') != std::string::npos) {
            *mask = binding.substr(dot + 1);
            binding = head;
        }
    }

    auto index = [&](const std::string& prefix, int32_t fallback) -> int32_t {
        size_t open = binding.find('[', prefix.size() - 1);
        return open == std::string::npos ? fallback : atoi(binding.c_str() + open + 1);
    };

    auto starts = [&](const char* prefix) {
        return !binding.compare(0, strlen(prefix), prefix);
    };

    if (m_vertex && !destination) {
        int32_t attrib = -1;

        if (starts("vertex.attrib[")) {
            attrib = index("vertex.attrib[", 0);
        } else if (binding == "vertex.position") {
            attrib = 0;
        } else if (binding == "vertex.weight" || starts("vertex.weight[")) {
            attrib = 1;
        } else if (binding == "vertex.normal") {
            attrib = 2;
        } else if (binding == "vertex.color" || binding == "vertex.color.primary") {
            attrib = 3;
        } else if (binding == "vertex.color.secondary") {
            attrib = 4;
        } else if (binding == "vertex.fogcoord") {
            attrib = 5;
        } else if (starts("vertex.texcoord")) {
            attrib = 8 + index("vertex.texcoord[", 0);
        }

        if (attrib >= 0 && attrib < kAttribs) {
            m_program.inputs |= 1u << attrib;
            return Format("a_attrib%d", attrib);
        }
    }

    if (!m_vertex && !destination) {
        if (starts("fragment.texcoord")) {
            int32_t unit = index("fragment.texcoord[", 0);

            if (unit >= 0 && unit < kUnits) {
                m_program.inputs |= Varying_Tex0 << unit;
                return Format("v_tex%d", unit);
            }
        } else if (binding == "fragment.color" || binding == "fragment.color.primary") {
            m_program.inputs |= Varying_Color0;
            return "v_color0";
        } else if (binding == "fragment.color.secondary") {
            m_program.inputs |= Varying_Color1;
            return "v_color1";
        } else if (binding == "fragment.fogcoord") {
            m_program.inputs |= Varying_Fog;
            return "vec4(v_fog, 0.0, 0.0, 1.0)";
        } else if (binding == "fragment.position") {
            return "gl_FragCoord";
        }
    }

    if (starts("result.")) {
        if (m_vertex) {
            if (binding == "result.position") {
                return "o_pos";
            }

            if (binding == "result.color" || binding == "result.color.primary" || binding == "result.color.front" || binding == "result.color.front.primary") {
                m_program.outputs |= Varying_Color0;
                return "o_col0";
            }

            if (binding == "result.color.secondary" || binding == "result.color.front.secondary") {
                m_program.outputs |= Varying_Color1;
                return "o_col1";
            }

            if (starts("result.color.back")) {
                return "o_unused";
            }

            if (starts("result.texcoord")) {
                int32_t unit = index("result.texcoord[", 0);

                if (unit >= 0 && unit < kUnits) {
                    m_program.outputs |= Varying_Tex0 << unit;
                    return Format("o_tex%d", unit);
                }
            }

            if (binding == "result.fogcoord") {
                m_program.outputs |= Varying_Fog;
                return "o_fog";
            }

            if (binding == "result.pointsize") {
                m_program.pointSize = true;
                return "o_psize";
            }
        } else {
            if (binding == "result.color") {
                return "o_frag";
            }

            if (binding == "result.depth") {
                return "o_depth";
            }
        }
    }

    if (!destination && (starts("program.env[") || starts("program.local["))) {
        int32_t param = index(starts("program.env[") ? "program.env[" : "program.local[", 0);

        if (param >= 0 && param < kEnvParams) {
            return Env(param);
        }
    }

    m_error = "unsupported binding: " + text;
    return "";
}

// An operand: [-] base [index] [.swizzle]. For a source, *expression is a vec4 (before the swizzle); for a destination,
// an lvalue, with *swizzle as its write mask.
bool Translator::Operand(const std::string& text, bool destination, std::string* expression, std::string* swizzle, bool* negate) {
    std::string operand = Trim(text);
    *negate = false;
    swizzle->clear();

    if (!operand.empty() && (operand[0] == '-' || operand[0] == '+')) {
        *negate = operand[0] == '-';
        operand = Trim(operand.substr(1));
    }

    if (operand.empty()) {
        m_error = "empty operand";
        return false;
    }

    // Literals: {..} or a number.
    if (operand[0] == '{' || isdigit(static_cast<unsigned char>(operand[0])) || operand[0] == '.') {
        if (destination) {
            m_error = "literal destination";
            return false;
        }

        std::string literal = operand;
        size_t close = operand.find('}');

        if (operand[0] == '{' && close != std::string::npos && close + 1 < operand.size() && operand[close + 1] == '.') {
            *swizzle = operand.substr(close + 2);
            literal = operand.substr(0, close + 1);
        }

        float value[4];

        if (!ParseLiteral(literal, value)) {
            m_error = "bad literal: " + operand;
            return false;
        }

        *expression = Vec4(value);
        return true;
    }

    // Bindings with dots in them.
    if (!operand.compare(0, 7, "vertex.") || !operand.compare(0, 9, "fragment.") || !operand.compare(0, 7, "result.") || !operand.compare(0, 8, "program.") || !operand.compare(0, 6, "state.")) {
        *expression = BindingExpression(operand, destination, swizzle);
        return !expression->empty();
    }

    // A name, maybe indexed, maybe with a swizzle.
    size_t end = 0;

    while (end < operand.size() && (isalnum(static_cast<unsigned char>(operand[end])) || operand[end] == '_' || operand[end] == '$')) {
        end++;
    }

    std::string name = operand.substr(0, end);
    std::string rest = operand.substr(end);
    std::string indexText;
    bool indexed = false;

    if (!rest.empty() && rest[0] == '[') {
        size_t close = rest.find(']');

        if (close == std::string::npos) {
            m_error = "bad index: " + operand;
            return false;
        }

        indexed = true;
        indexText = Trim(rest.substr(1, close - 1));
        rest = rest.substr(close + 1);
    }

    if (!rest.empty()) {
        if (rest[0] != '.' || !IsSwizzle(rest.substr(1))) {
            m_error = "bad swizzle: " + operand;
            return false;
        }

        *swizzle = rest.substr(1);
    }

    auto found = m_symbols.find(name);

    if (found == m_symbols.end()) {
        m_error = "unknown name: " + name;
        return false;
    }

    Symbol& symbol = found->second;

    switch (symbol.kind) {
        case Symbol::Temp:
            *expression = "t_" + name;
            return true;

        case Symbol::Address:
            *expression = "t_" + name;
            return true;

        case Symbol::Param:
            if (destination) {
                m_error = "PARAM destination";
                return false;
            }

            *expression = ElementExpression(symbol.elements[0]);
            return true;

        case Symbol::Binding: {
            std::string mask;
            *expression = BindingExpression(symbol.binding, destination, &mask);

            if (swizzle->empty()) {
                *swizzle = mask;
            }

            return !expression->empty();
        }

        case Symbol::ParamArray: {
            if (destination || !indexed) {
                m_error = "bad use of PARAM array " + name;
                return false;
            }

            // A constant index, or the address register plus an offset.
            size_t address = isalpha(static_cast<unsigned char>(indexText.empty() ? '0' : indexText[0])) ? 0 : std::string::npos;

            if (address == std::string::npos) {
                int32_t element = atoi(indexText.c_str());

                if (element < 0 || element >= static_cast<int32_t>(symbol.elements.size())) {
                    m_error = "PARAM index out of range: " + operand;
                    return false;
                }

                *expression = ElementExpression(symbol.elements[element]);
                return true;
            }

            // "A0.x", "A0.x + 12", "A0.x - 3": the register is the first word.
            size_t registerEnd = indexText.find_first_of(" +-", address);
            std::string registerName = indexText.substr(address, registerEnd == std::string::npos ? std::string::npos : registerEnd - address);
            size_t registerDot = registerName.find('.');

            if (registerDot != std::string::npos) {
                registerName = registerName.substr(0, registerDot);
            }

            int32_t offset = 0;
            size_t sign = indexText.find_first_of("+-", address);

            if (sign != std::string::npos) {
                offset = atoi(Trim(indexText.substr(sign + 1)).c_str());

                if (indexText[sign] == '-') {
                    offset = -offset;
                }
            }

            if (symbol.function < 0) {
                // vec4 p<n>(int i): the array's elements, in runs of literals and of consecutive env parameters.
                symbol.function = m_functions++;
                std::string function = Format("vec4 p%d(int i) {\n", symbol.function);
                size_t count = symbol.elements.size();
                size_t i = 0;

                while (i < count) {
                    size_t run = i + 1;

                    if (!symbol.elements[i].literal) {
                        while (run < count && !symbol.elements[run].literal && symbol.elements[run].env == symbol.elements[run - 1].env + 1) {
                            run++;
                        }

                        function += Format("    if (i < %d) return %s[i - %d];\n", static_cast<int32_t>(run), m_vertex ? "u_vertexEnv" : "u_fragmentEnv", static_cast<int32_t>(i) - symbol.elements[i].env);
                    } else {
                        function += Format("    if (i < %d) return ", static_cast<int32_t>(run)) + ElementExpression(symbol.elements[i]) + ";\n";
                    }

                    i = run;
                }

                function += "    return vec4(0.0);\n}\n";
                m_declarations += function;
            }

            *expression = Format("p%d(t_%s + %d)", symbol.function, registerName.c_str(), offset);
            return true;
        }
    }

    return false;
}

// A source operand as a vec4 expression, swizzled and negated.
std::string Translator::Source(const std::string& text) {
    std::string expression;
    std::string swizzle;
    bool negate;

    if (!Operand(text, false, &expression, &swizzle, &negate)) {
        return "";
    }

    if (!swizzle.empty()) {
        swizzle = NormalizeSwizzle(swizzle);

        // A scalar or short swizzle replicates its last component.
        while (swizzle.size() < 4) {
            swizzle += swizzle.back();
        }

        expression = "(" + expression + ")." + swizzle;
    }

    return negate ? "(-" + expression + ")" : expression;
}

bool Translator::Instruction(const std::string& opcodeText, const std::vector<std::string>& operands) {
    std::string opcode = opcodeText;
    bool saturate = false;

    if (opcode.size() > 4 && opcode.compare(opcode.size() - 4, 4, "_SAT") == 0) {
        saturate = true;
        opcode = opcode.substr(0, opcode.size() - 4);
    }

    if (opcode == "KIL") {
        if (operands.size() != 1) {
            return false;
        }

        std::string a = Source(operands[0]);

        if (a.empty()) {
            return false;
        }

        m_body += "    if (any(lessThan(" + a + ", vec4(0.0)))) discard;\n";
        return true;
    }

    if (operands.empty()) {
        m_error = "no operands for " + opcode;
        return false;
    }

    // The destination.
    std::string dest;
    std::string mask;
    bool negate;

    if (!Operand(operands[0], true, &dest, &mask, &negate)) {
        return false;
    }

    mask = NormalizeSwizzle(mask);

    if (opcode == "ARL") {
        std::string a = Source(operands[1]);

        if (a.empty()) {
            return false;
        }

        m_body += "    " + dest + " = int(floor((" + a + ").x));\n";
        return true;
    }

    std::vector<std::string> sources;

    for (size_t i = 1; i < operands.size(); i++) {
        if (opcode.size() == 3 && opcode[0] == 'T' && (opcode == "TEX" || opcode == "TXP" || opcode == "TXB") && i >= 2) {
            break;
        }

        std::string source = Source(operands[i]);

        if (source.empty()) {
            return false;
        }

        sources.push_back(source);
    }

    auto need = [&](size_t count) {
        if (sources.size() < count) {
            m_error = "too few operands for " + opcode;
            return false;
        }

        return true;
    };

    std::string a = sources.size() > 0 ? sources[0] : "";
    std::string b = sources.size() > 1 ? sources[1] : "";
    std::string c = sources.size() > 2 ? sources[2] : "";
    std::string value;

    if (opcode == "MOV" && need(1)) {
        value = a;
    } else if (opcode == "ABS" && need(1)) {
        value = "abs(" + a + ")";
    } else if (opcode == "ADD" && need(2)) {
        value = "(" + a + " + " + b + ")";
    } else if (opcode == "SUB" && need(2)) {
        value = "(" + a + " - " + b + ")";
    } else if (opcode == "MUL" && need(2)) {
        value = "(" + a + " * " + b + ")";
    } else if (opcode == "MAD" && need(3)) {
        value = "(" + a + " * " + b + " + " + c + ")";
    } else if (opcode == "DP3" && need(2)) {
        value = "vec4(dot((" + a + ").xyz, (" + b + ").xyz))";
    } else if (opcode == "DP4" && need(2)) {
        value = "vec4(dot(" + a + ", " + b + "))";
    } else if (opcode == "DPH" && need(2)) {
        value = "vec4(dot((" + a + ").xyz, (" + b + ").xyz) + (" + b + ").w)";
    } else if (opcode == "MIN" && need(2)) {
        value = "min(" + a + ", " + b + ")";
    } else if (opcode == "MAX" && need(2)) {
        value = "max(" + a + ", " + b + ")";
    } else if (opcode == "SLT" && need(2)) {
        value = "vec4(lessThan(" + a + ", " + b + "))";
    } else if (opcode == "SGE" && need(2)) {
        value = "vec4(greaterThanEqual(" + a + ", " + b + "))";
    } else if (opcode == "FLR" && need(1)) {
        value = "floor(" + a + ")";
    } else if (opcode == "FRC" && need(1)) {
        value = "fract(" + a + ")";
    } else if (opcode == "RCP" && need(1)) {
        value = "vec4(1.0 / (" + a + ").x)";
    } else if (opcode == "RSQ" && need(1)) {
        value = "vec4(inversesqrt(abs((" + a + ").x)))";
    } else if (opcode == "POW" && need(2)) {
        value = "vec4(pow(abs((" + a + ").x), (" + b + ").x))";
    } else if (opcode == "EX2" && need(1)) {
        value = "vec4(exp2((" + a + ").x))";
    } else if (opcode == "LG2" && need(1)) {
        value = "vec4(log2(abs((" + a + ").x)))";
    } else if (opcode == "EXP" && need(1)) {
        value = "vec4(exp2(floor((" + a + ").x)), fract((" + a + ").x), exp2((" + a + ").x), 1.0)";
    } else if (opcode == "LOG" && need(1)) {
        value = "vec4(floor(log2(abs((" + a + ").x))), abs((" + a + ").x) / exp2(floor(log2(abs((" + a + ").x)))), log2(abs((" + a + ").x)), 1.0)";
    } else if (opcode == "LIT" && need(1)) {
        value = "vec4(1.0, max((" + a + ").x, 0.0), (" + a + ").x > 0.0 ? exp2(clamp((" + a + ").w, -128.0, 128.0) * log2(max((" + a + ").y, 1e-30))) : 0.0, 1.0)";
    } else if (opcode == "DST" && need(2)) {
        value = "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)";
    } else if (opcode == "LRP" && need(3)) {
        value = "(" + a + " * " + b + " + (vec4(1.0) - " + a + ") * " + c + ")";
    } else if (opcode == "CMP" && need(3)) {
        value = "mix(" + c + ", " + b + ", vec4(lessThan(" + a + ", vec4(0.0))))";
    } else if (opcode == "XPD" && need(2)) {
        value = "vec4(cross((" + a + ").xyz, (" + b + ").xyz), 0.0)";
    } else if (opcode == "SCS" && need(1)) {
        value = "vec4(cos((" + a + ").x), sin((" + a + ").x), 0.0, 0.0)";
    } else if ((opcode == "TEX" || opcode == "TXP" || opcode == "TXB") && need(1)) {
        if (operands.size() < 4) {
            m_error = "bad texture instruction";
            return false;
        }

        std::string unitText = Trim(operands[2]);
        std::string target = Trim(operands[3]);
        size_t open = unitText.find('[');
        int32_t unit = open == std::string::npos ? 0 : atoi(unitText.c_str() + open + 1);

        if (unit < 0 || unit >= kSamplers) {
            m_error = "bad texture unit";
            return false;
        }

        Sampler sampler = Sampler_2D;

        if (target == "CUBE") {
            sampler = Sampler_Cube;
        } else if (target == "RECT") {
            sampler = Sampler_Rect;
        } else if (target == "SHADOW2D") {
            sampler = Sampler_Shadow;
        } else if (target != "2D") {
            m_error = "unsupported texture target " + target;
            return false;
        }

        if (!m_program.samplers[unit]) {
            m_program.samplers[unit] = sampler;
        }

        std::string s = Format("s%d", unit);

        switch (sampler) {
            case Sampler_Cube:
                value = opcode == "TXB" ? "texture(" + s + ", (" + a + ").xyz, (" + a + ").w)" : "texture(" + s + ", (" + a + ").xyz)";
                break;

            case Sampler_Rect: {
                std::string coord = opcode == "TXP" ? "((" + a + ").xy / (" + a + ").w)" : "(" + a + ").xy";
                value = "texture(" + s + ", " + coord + " / vec2(textureSize(" + s + ", 0)))";
                break;
            }

            case Sampler_Shadow:
                value = opcode == "TXP" ? "vec4(vec3(textureProj(" + s + ", " + a + ")), 1.0)" : "vec4(vec3(texture(" + s + ", (" + a + ").xyz)), 1.0)";
                break;

            default:
                if (opcode == "TXP") {
                    value = "textureProj(" + s + ", " + a + ")";
                } else if (opcode == "TXB") {
                    value = "texture(" + s + ", (" + a + ").xy, (" + a + ").w)";
                } else {
                    value = "texture(" + s + ", (" + a + ").xy)";
                }

                break;
        }
    } else {
        if (m_error.empty()) {
            m_error = "unsupported instruction " + opcodeText;
        }

        return false;
    }

    if (saturate) {
        value = "clamp(" + value + ", 0.0, 1.0)";
    }

    if (mask.empty() || mask == "xyzw") {
        m_body += "    " + dest + " = " + value + ";\n";
    } else {
        m_body += "    " + dest + "." + mask + " = (" + value + ")." + mask + ";\n";
    }

    return true;
}

bool Translator::Statement(const std::string& statementText) {
    std::string statement = Trim(statementText);

    if (statement.empty()) {
        return true;
    }

    size_t space = statement.find_first_of(" \t\r\n");
    std::string keyword = statement.substr(0, space);
    std::string rest = space == std::string::npos ? "" : Trim(statement.substr(space));

    if (keyword == "OPTION") {
        if (rest == "ARB_fog_linear") {
            m_program.fogLinear = true;
        } else if (rest == "ARB_position_invariant") {
            m_error = "ARB_position_invariant is not supported";
            return false;
        }

        // ARB_fragment_program_shadow and precision hints need nothing.
        return true;
    }

    if (keyword == "TEMP" || keyword == "ADDRESS") {
        for (const std::string& name : SplitOperands(rest)) {
            Symbol symbol;
            symbol.kind = keyword == "TEMP" ? Symbol::Temp : Symbol::Address;
            m_symbols[name] = symbol;
            m_declarations += keyword == "TEMP" ? "vec4 t_" + name + " = vec4(0.0);\n" : "int t_" + name + " = 0;\n";
        }

        return true;
    }

    if (keyword == "PARAM") {
        if (!DeclareParam(rest)) {
            if (m_error.empty()) {
                m_error = "bad PARAM: " + rest;
            }

            return false;
        }

        return true;
    }

    if (keyword == "ATTRIB" || keyword == "OUTPUT" || keyword == "ALIAS") {
        size_t equals = rest.find('=');

        if (equals == std::string::npos) {
            return false;
        }

        std::string name = Trim(rest.substr(0, equals));
        std::string target = Trim(rest.substr(equals + 1));

        if (keyword == "ALIAS") {
            auto found = m_symbols.find(target);

            if (found == m_symbols.end()) {
                return false;
            }

            m_symbols[name] = found->second;
        } else {
            Symbol symbol;
            symbol.kind = Symbol::Binding;
            symbol.binding = target;
            m_symbols[name] = symbol;
        }

        return true;
    }

    return Instruction(keyword, SplitOperands(rest));
}

bool Translator::Translate(const char* text, size_t length) {
    std::string source(text, length);

    // Comments run to the end of the line.
    std::string stripped;
    bool comment = false;

    for (char c : source) {
        if (c == '#') {
            comment = true;
        } else if (c == '\n') {
            comment = false;
        }

        if (!comment) {
            stripped += c;
        }
    }

    // The device's patches (CGxDeviceOpenGl IVertexShaderPatch) leave spaces in "program.env  [1..93]" and the like.
    std::string compact;

    for (size_t i = 0; i < stripped.size(); i++) {
        if (isspace(static_cast<unsigned char>(stripped[i]))) {
            size_t next = stripped.find_first_not_of(" \t\r\n", i);

            if (next != std::string::npos && stripped[next] == '[') {
                i = next - 1;
                continue;
            }
        }

        compact += stripped[i];
    }

    stripped.swap(compact);

    size_t header = stripped.find(m_vertex ? "!!ARBvp1.0" : "!!ARBfp1.0");

    if (header == std::string::npos) {
        m_error = "no program header";
        m_program.errorPosition = 0;
        return false;
    }

    size_t position = header + 10;
    size_t end = stripped.find("END", position);

    while (end != std::string::npos && (isalnum(static_cast<unsigned char>(stripped[end - 1])) || (end + 3 < stripped.size() && isalnum(static_cast<unsigned char>(stripped[end + 3]))))) {
        end = stripped.find("END", end + 3);
    }

    if (end == std::string::npos) {
        m_error = "no END";
        m_program.errorPosition = static_cast<int32_t>(stripped.size());
        return false;
    }

    while (position < end) {
        size_t semicolon = stripped.find(';', position);

        if (semicolon == std::string::npos || semicolon > end) {
            if (!Trim(stripped.substr(position, end - position)).empty()) {
                m_error = "missing ;";
                m_program.errorPosition = static_cast<int32_t>(position);
                return false;
            }

            break;
        }

        if (!Statement(stripped.substr(position, semicolon - position))) {
            m_program.errorPosition = static_cast<int32_t>(position);

            if (m_error.empty()) {
                m_error = "bad statement: " + Trim(stripped.substr(position, semicolon - position));
            }

            return false;
        }

        position = semicolon + 1;
    }

    // Outputs are globals the link-time main() reads after arb_main().
    std::string globals;

    if (m_vertex) {
        globals = "vec4 o_pos = vec4(0.0, 0.0, 0.0, 1.0);\nvec4 o_col0 = vec4(0.0);\nvec4 o_col1 = vec4(0.0);\n"
                  "vec4 o_fog = vec4(0.0);\nvec4 o_psize = vec4(1.0);\nvec4 o_unused = vec4(0.0);\n";

        for (int32_t i = 0; i < kUnits; i++) {
            globals += Format("vec4 o_tex%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        }
    } else {
        globals = "vec4 o_frag = vec4(0.0);\nvec4 o_depth = vec4(0.0);\n";
    }

    m_program.glsl = globals + m_declarations + "void arb_main() {\n" + m_body + "}\n";
    m_program.valid = true;
    m_program.errorPosition = -1;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keys.

// A unit's fixed-function texture target, for the generated fragment shader.
Sampler UnitSampler(const State& s, int32_t index) {
    const TexUnit& unit = s.units[index];

    if (unit.enabledCube) {
        return Sampler_Cube;
    }

    if (!unit.enabled2D) {
        return Sampler_None;
    }

    auto found = s.textures.find(unit.bound2D);
    return found != s.textures.end() && found->second.compare ? Sampler_Shadow : Sampler_2D;
}

void AppendKey(std::string& key, uint32_t value) {
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// The fragment stage: its key, and the varyings it reads.
void FragmentKey(const State& s, bool points, std::string& key, uint32_t* varyings) {
    const ArbProgram* program = nullptr;

    if (s.fragmentProgram && s.boundFragmentProgram) {
        auto found = s.programs.find(s.boundFragmentProgram);

        if (found != s.programs.end()) {
            program = &found->second;
        }
    }

    AppendKey(key, s.alphaTest ? s.alphaFunc : GL_ALWAYS);

    if (program) {
        AppendKey(key, 1);
        AppendKey(key, program->serial);
        *varyings = program->inputs | (program->fogLinear ? Varying_Fog : 0);
        return;
    }

    AppendKey(key, 0);
    *varyings = Varying_Color0;

    bool colorSum = s.vertexProgram || (s.lighting && s.separateSpecular);
    AppendKey(key, colorSum);

    if (colorSum) {
        *varyings |= Varying_Color1;
    }

    AppendKey(key, s.fog ? s.fogMode : 0);

    if (s.fog) {
        *varyings |= Varying_Fog;
    }

    bool pointCoord = points && s.pointSprite;

    for (int32_t i = 0; i < kUnits; i++) {
        Sampler sampler = UnitSampler(s, i);
        AppendKey(key, sampler);

        if (!sampler) {
            continue;
        }

        const TexUnit& unit = s.units[i];
        const TexEnv& env = unit.env;
        bool replaced = pointCoord && unit.coordReplace;
        AppendKey(key, replaced);

        if (!replaced) {
            *varyings |= Varying_Tex0 << i;
        }

        AppendKey(key, env.mode);

        if (env.mode == GL_COMBINE) {
            AppendKey(key, env.combineRgb);
            AppendKey(key, env.combineAlpha);

            for (int32_t j = 0; j < 3; j++) {
                AppendKey(key, env.sourceRgb[j] | (env.operandRgb[j] << 16));
                AppendKey(key, env.sourceAlpha[j] | (env.operandAlpha[j] << 16));
            }

            AppendKey(key, static_cast<uint32_t>(env.scaleRgb * 4.0f) | (static_cast<uint32_t>(env.scaleAlpha * 4.0f) << 8));
        }
    }
}

uint32_t TexGenCode(GLenum mode) {
    switch (mode) {
        case GL_EYE_LINEAR: return 1;
        case GL_OBJECT_LINEAR: return 2;
        case GL_SPHERE_MAP: return 3;
        case 0x8511: return 4;      // GL_NORMAL_MAP
        case 0x8512: return 5;      // GL_REFLECTION_MAP
        default: return 6;
    }
}

void VertexKey(const State& s, bool points, uint32_t varyings, std::string& key) {
    AppendKey(key, varyings);
    AppendKey(key, points);

    if (s.vertexProgram && s.boundVertexProgram) {
        auto found = s.programs.find(s.boundVertexProgram);

        if (found != s.programs.end()) {
            AppendKey(key, 1);
            AppendKey(key, found->second.serial);
            AppendKey(key, points && s.vertexProgramPointSize);
            return;
        }
    }

    AppendKey(key, 0);
    AppendKey(key, s.lighting);

    if (s.lighting) {
        uint32_t lights = 0;

        for (int32_t i = 0; i < kLights; i++) {
            if (s.lights[i].enabled) {
                lights |= 1u << i;

                if (s.lights[i].position[3] != 0.0f) {
                    lights |= 1u << (i + 8);
                }
            }
        }

        AppendKey(key, lights);
        AppendKey(key, s.colorMaterial ? s.colorMaterialMode : 0);
        AppendKey(key, s.localViewer | (s.separateSpecular << 1) | (s.normalize << 2));
    }

    for (int32_t i = 0; i < kUnits; i++) {
        if (!(varyings & (Varying_Tex0 << i))) {
            continue;
        }

        const TexUnit& unit = s.units[i];
        uint32_t texGen = 0;

        for (int32_t c = 0; c < 4; c++) {
            if (unit.texGenEnabled[c]) {
                texGen |= TexGenCode(unit.texGenMode[c]) << (c * 4);
            }
        }

        AppendKey(key, texGen);
        AppendKey(key, unit.matrixIdentity);
    }

    if (!s.lighting && s.normalize) {
        AppendKey(key, 1);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Generated shaders.

const char* const kHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                            "precision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler2DShadow;\n";

std::string VaryingDeclarations(uint32_t varyings, const char* qualifier) {
    std::string text;

    if (varyings & Varying_Color0) {
        text += Format("%s vec4 v_color0;\n", qualifier);
    }

    if (varyings & Varying_Color1) {
        text += Format("%s vec4 v_color1;\n", qualifier);
    }

    if (varyings & Varying_Fog) {
        text += Format("%s float v_fog;\n", qualifier);
    }

    for (int32_t i = 0; i < kUnits; i++) {
        if (varyings & (Varying_Tex0 << i)) {
            text += Format("%s vec4 v_tex%d;\n", qualifier, i);
        }
    }

    return text;
}

const char* TexGenMode(GLenum mode) {
    switch (mode) {
        case GL_OBJECT_LINEAR: return "object";
        case GL_EYE_LINEAR: return "eye";
        case GL_SPHERE_MAP: return "sphere";
        case 0x8511: return "normal";
        case 0x8512: return "reflection";
        default: return "";
    }
}

std::string FixedVertexShader(const State& s, bool points, uint32_t varyings, uint32_t* attribs) {
    std::string text = kHeader;
    text += "uniform mat4 u_mvp;\nuniform mat4 u_mv;\nuniform mat3 u_normalMatrix;\nuniform mat4 u_texMatrix[8];\n"
            "uniform vec4 u_lightAmbient[8];\nuniform vec4 u_lightDiffuse[8];\nuniform vec4 u_lightSpecular[8];\n"
            "uniform vec4 u_lightPosition[8];\nuniform vec3 u_lightAttenuation[8];\nuniform vec4 u_sceneAmbient;\n"
            "uniform vec4 u_matAmbient;\nuniform vec4 u_matDiffuse;\nuniform vec4 u_matSpecular;\nuniform vec4 u_matEmission;\n"
            "uniform float u_matShininess;\nuniform vec4 u_eyePlane[32];\nuniform vec4 u_objectPlane[32];\n"
            "uniform vec4 u_pointParams;\nuniform vec3 u_pointAttenuation;\n";
    text += Format("layout(location = %d) in vec4 a_position;\n", kLocPosition);
    text += Format("layout(location = %d) in vec3 a_normal;\n", kLocNormal);
    text += Format("layout(location = %d) in vec4 a_color;\n", kLocColor);
    *attribs = (1u << kLocPosition) | (1u << kLocColor);

    bool needNormal = s.lighting;
    std::string texCode;

    for (int32_t i = 0; i < kUnits; i++) {
        if (!(varyings & (Varying_Tex0 << i))) {
            continue;
        }

        const TexUnit& unit = s.units[i];
        bool allGenerated = true;

        for (int32_t c = 0; c < 4; c++) {
            if (!unit.texGenEnabled[c]) {
                allGenerated = false;
            }
        }

        if (!allGenerated) {
            text += Format("layout(location = %d) in vec4 a_texcoord%d;\n", kLocTexCoord0 + i, i);
            *attribs |= 1u << (kLocTexCoord0 + i);
            texCode += Format("    vec4 t%d = a_texcoord%d;\n", i, i);
        } else {
            texCode += Format("    vec4 t%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        }

        static const char* const s_coords = "xyzw";

        for (int32_t c = 0; c < 4; c++) {
            if (!unit.texGenEnabled[c]) {
                continue;
            }

            std::string mode = TexGenMode(unit.texGenMode[c]);
            int32_t plane = i * 4 + c;

            if (mode == "object") {
                texCode += Format("    t%d.%c = dot(u_objectPlane[%d], a_position);\n", i, s_coords[c], plane);
            } else if (mode == "eye") {
                texCode += Format("    t%d.%c = dot(u_eyePlane[%d], eye);\n", i, s_coords[c], plane);
            } else if (mode == "sphere") {
                needNormal = true;
                texCode += Format("    t%d.%c = sphere.%c;\n", i, s_coords[c], s_coords[c > 1 ? 1 : c]);
            } else if (mode == "reflection") {
                needNormal = true;
                texCode += Format("    t%d.%c = reflection.%c;\n", i, s_coords[c], s_coords[c > 2 ? 2 : c]);
            } else if (mode == "normal") {
                needNormal = true;
                texCode += Format("    t%d.%c = n.%c;\n", i, s_coords[c], s_coords[c > 2 ? 2 : c]);
            }
        }

        if (!unit.matrixIdentity) {
            texCode += Format("    t%d = u_texMatrix[%d] * t%d;\n", i, i, i);
        }

        texCode += Format("    v_tex%d = t%d;\n", i, i);
    }

    if (needNormal) {
        *attribs |= 1u << kLocNormal;
    }

    text += VaryingDeclarations(varyings, "out");
    text += "void main() {\n    vec4 eye = u_mv * a_position;\n    gl_Position = u_mvp * a_position;\n";
    text += needNormal ? "    vec3 n = u_normalMatrix * a_normal;\n" : "    vec3 n = vec3(0.0, 0.0, 1.0);\n";

    if (s.normalize && needNormal) {
        text += "    n = normalize(n);\n";
    }

    text += "    vec3 u = normalize(eye.xyz);\n    vec3 reflection = reflect(u, n);\n"
            "    float m = 2.0 * sqrt(reflection.x * reflection.x + reflection.y * reflection.y + (reflection.z + 1.0) * (reflection.z + 1.0));\n"
            "    vec2 sphere = reflection.xy / max(m, 1e-6) + 0.5;\n";

    if (s.lighting) {
        GLenum cm = s.colorMaterial ? s.colorMaterialMode : 0;
        text += Format("    vec4 matAmbient = %s;\n", cm == GL_AMBIENT || cm == GL_AMBIENT_AND_DIFFUSE ? "a_color" : "u_matAmbient");
        text += Format("    vec4 matDiffuse = %s;\n", cm == GL_DIFFUSE || cm == GL_AMBIENT_AND_DIFFUSE ? "a_color" : "u_matDiffuse");
        text += Format("    vec4 matSpecular = %s;\n", cm == GL_SPECULAR ? "a_color" : "u_matSpecular");
        text += Format("    vec4 matEmission = %s;\n", cm == GL_EMISSION ? "a_color" : "u_matEmission");
        text += "    vec3 color = matEmission.rgb + u_sceneAmbient.rgb * matAmbient.rgb;\n    vec3 specular = vec3(0.0);\n";
        text += s.localViewer ? "    vec3 viewer = -u;\n" : "    vec3 viewer = vec3(0.0, 0.0, 1.0);\n";

        for (int32_t i = 0; i < kLights; i++) {
            const Light& light = s.lights[i];

            if (!light.enabled) {
                continue;
            }

            text += "    {\n";

            if (light.position[3] == 0.0f) {
                text += Format("        vec3 l = normalize(u_lightPosition[%d].xyz);\n        float attenuation = 1.0;\n", i);
            } else {
                text += Format("        vec3 d = u_lightPosition[%d].xyz - eye.xyz;\n        float distance = length(d);\n        vec3 l = d / max(distance, 1e-6);\n", i);
                text += Format("        float attenuation = 1.0 / max(u_lightAttenuation[%d].x + u_lightAttenuation[%d].y * distance + u_lightAttenuation[%d].z * distance * distance, 1e-6);\n", i, i, i);
            }

            text += "        float nl = max(dot(n, l), 0.0);\n";
            text += Format("        color += attenuation * (u_lightAmbient[%d].rgb * matAmbient.rgb + nl * u_lightDiffuse[%d].rgb * matDiffuse.rgb);\n", i, i);
            text += "        if (nl > 0.0) {\n            float nh = max(dot(n, normalize(l + viewer)), 0.0);\n";
            text += Format("            specular += attenuation * (u_matShininess == 0.0 ? 1.0 : pow(nh, u_matShininess)) * u_lightSpecular[%d].rgb * matSpecular.rgb;\n", i);
            text += "        }\n    }\n";
        }

        if (varyings & Varying_Color0) {
            text += s.separateSpecular ? "    v_color0 = clamp(vec4(color, matDiffuse.a), 0.0, 1.0);\n" : "    v_color0 = clamp(vec4(color + specular, matDiffuse.a), 0.0, 1.0);\n";
        }

        if (varyings & Varying_Color1) {
            text += s.separateSpecular ? "    v_color1 = vec4(clamp(specular, 0.0, 1.0), 0.0);\n" : "    v_color1 = vec4(0.0);\n";
        }
    } else {
        if (varyings & Varying_Color0) {
            text += "    v_color0 = a_color;\n";
        }

        if (varyings & Varying_Color1) {
            text += "    v_color1 = vec4(0.0);\n";
        }
    }

    text += texCode;

    if (varyings & Varying_Fog) {
        text += "    v_fog = abs(eye.z);\n";
    }

    if (points) {
        text += "    float pointDistance = length(eye.xyz);\n"
                "    gl_PointSize = clamp(u_pointParams.x * inversesqrt(max(dot(u_pointAttenuation, vec3(1.0, pointDistance, pointDistance * pointDistance)), 1e-6)), u_pointParams.y, u_pointParams.z);\n";
    }

    text += "}\n";
    return text;
}

std::string ArbVertexShader(const ArbProgram& program, bool points, bool programPointSize, uint32_t varyings, uint32_t* attribs) {
    std::string text = kHeader;
    text += Format("layout(std140) uniform GxVertexEnv { vec4 u_vertexEnv[%d]; };\nuniform vec4 u_pointParams;\n", kEnvParams);

    for (int32_t i = 0; i < kAttribs; i++) {
        if (program.inputs & (1u << i)) {
            text += Format("layout(location = %d) in vec4 a_attrib%d;\n", i, i);
        }
    }

    *attribs = program.inputs;
    text += VaryingDeclarations(varyings, "out");
    text += program.glsl;
    text += "void main() {\n    arb_main();\n    gl_Position = o_pos;\n";

    // Vertex program colors are clamped before rasterization, as GL does.
    if (varyings & Varying_Color0) {
        text += "    v_color0 = clamp(o_col0, 0.0, 1.0);\n";
    }

    if (varyings & Varying_Color1) {
        text += "    v_color1 = clamp(o_col1, 0.0, 1.0);\n";
    }

    if (varyings & Varying_Fog) {
        text += "    v_fog = o_fog.x;\n";
    }

    for (int32_t i = 0; i < kUnits; i++) {
        if (varyings & (Varying_Tex0 << i)) {
            text += Format("    v_tex%d = o_tex%d;\n", i, i);
        }
    }

    if (points) {
        text += programPointSize && program.pointSize ? "    gl_PointSize = clamp(o_psize.x, u_pointParams.y, u_pointParams.z);\n" : "    gl_PointSize = u_pointParams.x;\n";
    }

    text += "}\n";
    return text;
}

const char* SamplerType(Sampler sampler) {
    switch (sampler) {
        case Sampler_Cube: return "samplerCube";
        case Sampler_Shadow: return "sampler2DShadow";
        default: return "sampler2D";
    }
}

std::string AlphaTest(GLenum func) {
    const char* compare = nullptr;

    switch (func) {
        case GL_NEVER: return "    discard;\n";
        case GL_LESS: compare = "<"; break;
        case GL_EQUAL: compare = "=="; break;
        case GL_LEQUAL: compare = "<="; break;
        case GL_GREATER: compare = ">"; break;
        case GL_NOTEQUAL: compare = "!="; break;
        case GL_GEQUAL: compare = ">="; break;
        default: return "";
    }

    return Format("    if (!(color.a %s u_alphaRef)) discard;\n", compare);
}

std::string FogCode(GLenum mode) {
    switch (mode) {
        case GL_LINEAR:
            return "    color.rgb = mix(u_fogColor.rgb, color.rgb, clamp((u_fogParams.y - v_fog) * u_fogParams.w, 0.0, 1.0));\n";
        case GL_EXP:
            return "    color.rgb = mix(u_fogColor.rgb, color.rgb, clamp(exp(-u_fogParams.z * v_fog), 0.0, 1.0));\n";
        case GL_EXP2:
            return "    color.rgb = mix(u_fogColor.rgb, color.rgb, clamp(exp(-(u_fogParams.z * v_fog) * (u_fogParams.z * v_fog)), 0.0, 1.0));\n";
        default:
            return "";
    }
}

std::string CombineSource(GLenum source, int32_t unit) {
    if (source >= GL_TEXTURE0 && source < GL_TEXTURE0 + kUnits) {
        return Format("tex%d", static_cast<int32_t>(source - GL_TEXTURE0));
    }

    switch (source) {
        case GL_TEXTURE: return Format("tex%d", unit);
        case GL_CONSTANT: return Format("u_texEnvColor[%d]", unit);
        case GL_PRIMARY_COLOR: return "primary";
        default: return "color";
    }
}

std::string CombineOperand(GLenum source, GLenum operand, int32_t unit, bool alpha) {
    std::string value = CombineSource(source, unit);

    if (alpha) {
        return operand == GL_ONE_MINUS_SRC_ALPHA ? "(1.0 - " + value + ".a)" : value + ".a";
    }

    switch (operand) {
        case GL_ONE_MINUS_SRC_COLOR: return "(vec3(1.0) - " + value + ".rgb)";
        case GL_SRC_ALPHA: return "vec3(" + value + ".a)";
        case GL_ONE_MINUS_SRC_ALPHA: return "vec3(1.0 - " + value + ".a)";
        default: return value + ".rgb";
    }
}

std::string CombineFunction(GLenum function, const std::string& a0, const std::string& a1, const std::string& a2, bool alpha) {
    switch (function) {
        case GL_REPLACE: return a0;
        case GL_ADD: return "(" + a0 + " + " + a1 + ")";
        case GL_ADD_SIGNED: return alpha ? "(" + a0 + " + " + a1 + " - 0.5)" : "(" + a0 + " + " + a1 + " - vec3(0.5))";
        case GL_INTERPOLATE: return "mix(" + a1 + ", " + a0 + ", " + a2 + ")";
        case GL_SUBTRACT: return "(" + a0 + " - " + a1 + ")";
        case GL_DOT3_RGB:
        case GL_DOT3_RGBA:
            return alpha ? "(4.0 * dot(" + a0 + " - vec3(0.5), " + a1 + " - vec3(0.5)))" : "vec3(4.0 * dot(" + a0 + " - vec3(0.5), " + a1 + " - vec3(0.5)))";
        default: return "(" + a0 + " * " + a1 + ")";
    }
}

std::string FixedFragmentShader(const State& s, bool points, uint32_t varyings) {
    std::string text = kHeader;
    text += "uniform vec4 u_texEnvColor[8];\nuniform vec4 u_fogParams;\nuniform vec4 u_fogColor;\nuniform float u_alphaRef;\n";
    text += VaryingDeclarations(varyings, "in");
    text += "out vec4 fragColor;\n";

    bool pointCoord = points && s.pointSprite;
    std::string samples;
    std::string stages;

    for (int32_t i = 0; i < kUnits; i++) {
        Sampler sampler = UnitSampler(s, i);

        if (!sampler) {
            samples += Format("    vec4 tex%d = vec4(1.0);\n", i);
            continue;
        }

        const TexUnit& unit = s.units[i];
        text += Format("uniform %s s%d;\n", SamplerType(sampler), i);

        std::string coord = pointCoord && unit.coordReplace ? "vec4(gl_PointCoord, 0.0, 1.0)" : Format("v_tex%d", i);

        switch (sampler) {
            case Sampler_Cube:
                samples += Format("    vec4 tex%d = texture(s%d, ", i, i) + coord + ".xyz);\n";
                break;
            case Sampler_Shadow:
                samples += Format("    vec4 tex%d = vec4(vec3(textureProj(s%d, ", i, i) + coord + ")), 1.0);\n";
                break;
            default:
                samples += Format("    vec4 tex%d = textureProj(s%d, ", i, i) + coord + ");\n";
                break;
        }

        const TexEnv& env = unit.env;
        std::string t = Format("tex%d", i);

        switch (env.mode) {
            case GL_REPLACE:
                stages += "    color = " + t + ";\n";
                break;
            case GL_DECAL:
                stages += "    color = vec4(mix(color.rgb, " + t + ".rgb, " + t + ".a), color.a);\n";
                break;
            case GL_BLEND:
                stages += Format("    color = vec4(mix(color.rgb, u_texEnvColor[%d].rgb, ", i) + t + ".rgb), color.a * " + t + ".a);\n";
                break;
            case GL_ADD:
                stages += "    color = vec4(clamp(color.rgb + " + t + ".rgb, 0.0, 1.0), color.a * " + t + ".a);\n";
                break;
            case GL_COMBINE: {
                std::string rgb = CombineFunction(env.combineRgb, CombineOperand(env.sourceRgb[0], env.operandRgb[0], i, false), CombineOperand(env.sourceRgb[1], env.operandRgb[1], i, false), CombineOperand(env.sourceRgb[2], env.operandRgb[2], i, false), false);
                std::string alpha = CombineFunction(env.combineAlpha, CombineOperand(env.sourceAlpha[0], env.operandAlpha[0], i, true), CombineOperand(env.sourceAlpha[1], env.operandAlpha[1], i, true), CombineOperand(env.sourceAlpha[2], env.operandAlpha[2], i, true), true);

                if (env.combineRgb == GL_DOT3_RGBA) {
                    alpha = rgb + ".r";
                }

                stages += "    {\n        vec3 rgb = clamp(" + rgb + " * " + Float(env.scaleRgb) + ", 0.0, 1.0);\n";
                stages += "        float a = clamp(" + alpha + " * " + Float(env.scaleAlpha) + ", 0.0, 1.0);\n";
                stages += "        color = vec4(rgb, a);\n    }\n";
                break;
            }
            default:    // GL_MODULATE
                stages += "    color = color * " + t + ";\n";
                break;
        }
    }

    text += "void main() {\n";
    text += (varyings & Varying_Color0) ? "    vec4 primary = v_color0;\n" : "    vec4 primary = vec4(1.0);\n";
    text += "    vec4 color = primary;\n";
    text += samples + stages;

    if (varyings & Varying_Color1) {
        text += "    color.rgb = clamp(color.rgb + v_color1.rgb, 0.0, 1.0);\n";
    }

    if (s.fog) {
        text += FogCode(s.fogMode);
    }

    if (s.alphaTest) {
        text += AlphaTest(s.alphaFunc);
    }

    text += "    fragColor = color;\n}\n";
    return text;
}

std::string ArbFragmentShader(const State& s, const ArbProgram& program, uint32_t varyings) {
    std::string text = kHeader;
    text += Format("layout(std140) uniform GxFragmentEnv { vec4 u_fragmentEnv[%d]; };\n", kEnvParams);
    text += "uniform vec4 u_fogParams;\nuniform vec4 u_fogColor;\nuniform float u_alphaRef;\n";
    text += VaryingDeclarations(varyings, "in");
    text += "out vec4 fragColor;\n";

    for (int32_t i = 0; i < kSamplers; i++) {
        if (program.samplers[i]) {
            text += Format("uniform %s s%d;\n", SamplerType(static_cast<Sampler>(program.samplers[i])), i);
        }
    }

    text += program.glsl;
    text += "void main() {\n    arb_main();\n    vec4 color = o_frag;\n";

    if (program.fogLinear) {
        text += FogCode(GL_LINEAR);
    }

    if (s.alphaTest) {
        text += AlphaTest(s.alphaFunc);
    }

    text += "    fragColor = color;\n}\n";
    return text;
}

GLuint Compile(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (!compiled) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        Log("GLSL compile failed: %s", log);
        Log("%s", source.c_str());
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

std::unique_ptr<LinkedProgram> Build(const State& s, bool points) {
    std::unique_ptr<LinkedProgram> linked(new LinkedProgram());

    for (GLint& location : linked->locations) {
        location = -1;
    }

    for (uint32_t& version : linked->versions) {
        version = 0xFFFFFFFF;
    }

    const ArbProgram* vertexProgram = nullptr;
    const ArbProgram* fragmentProgram = nullptr;

    if (s.vertexProgram && s.boundVertexProgram) {
        auto found = s.programs.find(s.boundVertexProgram);

        if (found != s.programs.end()) {
            vertexProgram = &found->second;
        }
    }

    if (s.fragmentProgram && s.boundFragmentProgram) {
        auto found = s.programs.find(s.boundFragmentProgram);

        if (found != s.programs.end()) {
            fragmentProgram = &found->second;
        }
    }

    if ((vertexProgram && !vertexProgram->valid) || (fragmentProgram && !fragmentProgram->valid)) {
        return linked;
    }

    uint32_t varyings = 0;
    std::string fragmentKey;
    FragmentKey(s, points, fragmentKey, &varyings);

    std::string fragmentSource = fragmentProgram ? ArbFragmentShader(s, *fragmentProgram, varyings) : FixedFragmentShader(s, points, varyings);
    std::string vertexSource = vertexProgram ? ArbVertexShader(*vertexProgram, points, s.vertexProgramPointSize, varyings, &linked->attribs) : FixedVertexShader(s, points, varyings, &linked->attribs);
    linked->arbVertex = vertexProgram != nullptr;

    if (GetStats().enabled && GetStats().traceFrames > 0) {
        if (FILE* file = fopen("Logs/gl-programs.txt", "a")) {
            fprintf(file, "==== program\n%s\n----\n%s\n", vertexSource.c_str(), fragmentSource.c_str());
            fclose(file);
        }
    }

    GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource);

    if (!vertex || !fragment) {
        if (vertex) {
            glDeleteShader(vertex);
        }

        if (fragment) {
            glDeleteShader(fragment);
        }

        return linked;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);

    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        Log("GLSL link failed: %s", log);
        glDeleteProgram(program);
        return linked;
    }

    linked->id = program;
    linked->valid = true;

    for (int32_t i = 0; i < U_Count; i++) {
        std::string name = kUniformNames[i];

        // Arrays are set from their first element.
        if (i == U_TexMatrix || (i >= U_LightAmbient && i <= U_LightAttenuation) || i == U_TexEnvColor || i == U_EyePlane || i == U_ObjectPlane) {
            name += "[0]";
        }

        linked->locations[i] = glGetUniformLocation(program, name.c_str());
    }

    glUseProgram(program);
    s_usedProgram = program;

    for (int32_t i = 0; i < kSamplers; i++) {
        GLint sampler = glGetUniformLocation(program, Format("s%d", i).c_str());

        if (sampler >= 0) {
            glUniform1i(sampler, i);
        }
    }

    GLuint block = glGetUniformBlockIndex(program, "GxVertexEnv");

    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, block, 0);
    }

    block = glGetUniformBlockIndex(program, "GxFragmentEnv");

    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, block, 1);
    }

    return linked;
}

// ---------------------------------------------------------------------------------------------------------------------
// Draw setup.

void UploadUniforms(LinkedProgram& program) {
    State& s = Get();
    const GLint* l = program.locations;

    auto changed = [&](Group group) {
        if (program.versions[group] == s.versions[group]) {
            return false;
        }

        program.versions[group] = s.versions[group];
        return true;
    };

    if (changed(Group_Transform)) {
        if (l[U_Mvp] >= 0) {
            float mvp[16];
            MatrixMultiply(s.projection, s.modelView, mvp);
            glUniformMatrix4fv(l[U_Mvp], 1, GL_FALSE, mvp);
        }

        if (l[U_Mv] >= 0) {
            glUniformMatrix4fv(l[U_Mv], 1, GL_FALSE, s.modelView);
        }

        if (l[U_NormalMatrix] >= 0) {
            // The inverse transpose of the modelview's upper 3x3.
            float inverse[16];
            MatrixInverse(s.modelView, inverse);
            float normal[9] = {
                inverse[0], inverse[4], inverse[8],
                inverse[1], inverse[5], inverse[9],
                inverse[2], inverse[6], inverse[10],
            };
            glUniformMatrix3fv(l[U_NormalMatrix], 1, GL_FALSE, normal);
        }
    }

    if (changed(Group_TexMatrix) && l[U_TexMatrix] >= 0) {
        float matrices[kUnits * 16];

        for (int32_t i = 0; i < kUnits; i++) {
            memcpy(matrices + i * 16, s.units[i].matrix, sizeof(float) * 16);
        }

        glUniformMatrix4fv(l[U_TexMatrix], kUnits, GL_FALSE, matrices);
    }

    if (changed(Group_Lights)) {
        float ambient[kLights * 4];
        float diffuse[kLights * 4];
        float specular[kLights * 4];
        float position[kLights * 4];
        float attenuation[kLights * 3];

        for (int32_t i = 0; i < kLights; i++) {
            const Light& light = s.lights[i];
            memcpy(ambient + i * 4, light.ambient, sizeof(float) * 4);
            memcpy(diffuse + i * 4, light.diffuse, sizeof(float) * 4);
            memcpy(specular + i * 4, light.specular, sizeof(float) * 4);
            memcpy(position + i * 4, light.position, sizeof(float) * 4);
            memcpy(attenuation + i * 3, light.attenuation, sizeof(float) * 3);
        }

        if (l[U_LightAmbient] >= 0) {
            glUniform4fv(l[U_LightAmbient], kLights, ambient);
        }

        if (l[U_LightDiffuse] >= 0) {
            glUniform4fv(l[U_LightDiffuse], kLights, diffuse);
        }

        if (l[U_LightSpecular] >= 0) {
            glUniform4fv(l[U_LightSpecular], kLights, specular);
        }

        if (l[U_LightPosition] >= 0) {
            glUniform4fv(l[U_LightPosition], kLights, position);
        }

        if (l[U_LightAttenuation] >= 0) {
            glUniform3fv(l[U_LightAttenuation], kLights, attenuation);
        }

        if (l[U_SceneAmbient] >= 0) {
            glUniform4fv(l[U_SceneAmbient], 1, s.lightModelAmbient);
        }
    }

    if (changed(Group_Material)) {
        if (l[U_MatAmbient] >= 0) {
            glUniform4fv(l[U_MatAmbient], 1, s.matAmbient);
        }

        if (l[U_MatDiffuse] >= 0) {
            glUniform4fv(l[U_MatDiffuse], 1, s.matDiffuse);
        }

        if (l[U_MatSpecular] >= 0) {
            glUniform4fv(l[U_MatSpecular], 1, s.matSpecular);
        }

        if (l[U_MatEmission] >= 0) {
            glUniform4fv(l[U_MatEmission], 1, s.matEmission);
        }

        if (l[U_MatShininess] >= 0) {
            glUniform1f(l[U_MatShininess], s.matShininess);
        }
    }

    if (changed(Group_Fog)) {
        if (l[U_FogParams] >= 0) {
            float range = s.fogEnd - s.fogStart;
            glUniform4f(l[U_FogParams], s.fogStart, s.fogEnd, s.fogDensity, range != 0.0f ? 1.0f / range : 0.0f);
        }

        if (l[U_FogColor] >= 0) {
            glUniform4fv(l[U_FogColor], 1, s.fogColor);
        }
    }

    if (changed(Group_Alpha) && l[U_AlphaRef] >= 0) {
        glUniform1f(l[U_AlphaRef], s.alphaRef);
    }

    if (changed(Group_TexEnv) && l[U_TexEnvColor] >= 0) {
        float colors[kUnits * 4];

        for (int32_t i = 0; i < kUnits; i++) {
            memcpy(colors + i * 4, s.units[i].env.color, sizeof(float) * 4);
        }

        glUniform4fv(l[U_TexEnvColor], kUnits, colors);
    }

    if (changed(Group_TexGen)) {
        float eye[kUnits * 16];
        float object[kUnits * 16];

        for (int32_t i = 0; i < kUnits; i++) {
            memcpy(eye + i * 16, s.units[i].eyePlane, sizeof(float) * 16);
            memcpy(object + i * 16, s.units[i].objectPlane, sizeof(float) * 16);
        }

        if (l[U_EyePlane] >= 0) {
            glUniform4fv(l[U_EyePlane], kUnits * 4, eye);
        }

        if (l[U_ObjectPlane] >= 0) {
            glUniform4fv(l[U_ObjectPlane], kUnits * 4, object);
        }
    }

    if (changed(Group_Point)) {
        float minimum = s.pointSizeMin > s.pointSizeRange[0] ? s.pointSizeMin : s.pointSizeRange[0];
        float maximum = s.pointSizeMax < s.pointSizeRange[1] ? s.pointSizeMax : s.pointSizeRange[1];

        if (l[U_PointParams] >= 0) {
            glUniform4f(l[U_PointParams], s.pointSize, minimum, maximum, 0.0f);
        }

        if (l[U_PointAttenuation] >= 0) {
            glUniform3fv(l[U_PointAttenuation], 1, s.pointAttenuation);
        }
    }
}

void UploadEnv() {
    State& s = Get();

    if (s.vertexEnvDirtyMax >= s.vertexEnvDirtyMin) {
        glBindBuffer(GL_UNIFORM_BUFFER, s_vertexEnvBuffer);
        glBufferSubData(GL_UNIFORM_BUFFER, s.vertexEnvDirtyMin * 16, (s.vertexEnvDirtyMax - s.vertexEnvDirtyMin + 1) * 16, s.vertexEnv[s.vertexEnvDirtyMin]);
        s.vertexEnvDirtyMin = kEnvParams;
        s.vertexEnvDirtyMax = -1;
    }

    if (s.fragmentEnvDirtyMax >= s.fragmentEnvDirtyMin) {
        glBindBuffer(GL_UNIFORM_BUFFER, s_fragmentEnvBuffer);
        glBufferSubData(GL_UNIFORM_BUFFER, s.fragmentEnvDirtyMin * 16, (s.fragmentEnvDirtyMax - s.fragmentEnvDirtyMin + 1) * 16, s.fragmentEnv[s.fragmentEnvDirtyMin]);
        s.fragmentEnvDirtyMin = kEnvParams;
        s.fragmentEnvDirtyMax = -1;
    }
}

// Points each attribute the program reads at its array, or gives it the array's constant.
void SetupAttribs(const LinkedProgram& program) {
    State& s = Get();
    GLuint boundBuffer = s.arrayBuffer;

    static const float s_normal[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const float s_zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    for (int32_t location = 0; location < kAttribs; location++) {
        AttribCache& cache = s_attribCache[location];

        if (!(program.attribs & (1u << location))) {
            if (cache.enabled) {
                glDisableVertexAttribArray(location);
                cache.enabled = false;
            }

            continue;
        }

        const Array* array = nullptr;
        const float* constant = s_zero;

        if (program.arbVertex) {
            array = &s.attribs[location];
        } else if (location == kLocPosition) {
            array = &s.vertexArray;
        } else if (location == kLocNormal) {
            array = &s.normalArray;
            constant = s_normal;
        } else if (location == kLocColor) {
            array = &s.colorArray;
            constant = s.currentColor;
        } else if (location >= kLocTexCoord0 && location < kLocTexCoord0 + kUnits) {
            array = &s.units[location - kLocTexCoord0].texCoord;
        }

        if (array && array->enabled) {
            if (!cache.enabled) {
                glEnableVertexAttribArray(location);
                cache.enabled = true;
            }

            if (cache.buffer != array->buffer || cache.size != array->size || cache.type != array->type || cache.normalized != array->normalized || cache.stride != array->stride || cache.pointer != array->pointer) {
                if (boundBuffer != array->buffer) {
                    glBindBuffer(GL_ARRAY_BUFFER, array->buffer);
                    boundBuffer = array->buffer;
                }

                glVertexAttribPointer(location, array->size, array->type, array->normalized, array->stride, array->pointer);
                cache.buffer = array->buffer;
                cache.size = array->size;
                cache.type = array->type;
                cache.normalized = array->normalized;
                cache.stride = array->stride;
                cache.pointer = array->pointer;
            }
        } else {
            if (cache.enabled) {
                glDisableVertexAttribArray(location);
                cache.enabled = false;
            }

            if (memcmp(cache.constant, constant, sizeof(cache.constant))) {
                glVertexAttrib4fv(location, constant);
                memcpy(cache.constant, constant, sizeof(cache.constant));
            }
        }
    }

    if (boundBuffer != s.arrayBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuffer);
    }
}

}

namespace GlEs {

void TranslateArbProgram(ArbProgram& program, const char* text, size_t length) {
    Translator translator(program, program.target == kVertexProgram);

    if (!translator.Translate(text, length)) {
        program.valid = false;
        program.error = translator.Error();
    }
}

void ProgramsInitialize() {
    ProgramsReset();

    glGenBuffers(1, &s_vertexEnvBuffer);
    glBindBuffer(GL_UNIFORM_BUFFER, s_vertexEnvBuffer);
    glBufferData(GL_UNIFORM_BUFFER, kEnvParams * 16, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_vertexEnvBuffer);

    glGenBuffers(1, &s_fragmentEnvBuffer);
    glBindBuffer(GL_UNIFORM_BUFFER, s_fragmentEnvBuffer);
    glBufferData(GL_UNIFORM_BUFFER, kEnvParams * 16, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, s_fragmentEnvBuffer);
}

void ProgramsReset() {
    // The context's objects are gone with it: only the bookkeeping is dropped.
    s_programs.clear();
    s_current = nullptr;
    s_usedProgram = 0;
    s_vertexEnvBuffer = 0;
    s_fragmentEnvBuffer = 0;

    for (AttribCache& cache : s_attribCache) {
        cache = AttribCache();
    }
}

bool PrepareDraw(GLenum mode) {
    State& s = Get();
    bool points = mode == GL_POINTS;

    if (s.programDirty || points != s_lastPoints || !s_current) {
        uint32_t varyings = 0;
        std::string key;
        FragmentKey(s, points, key, &varyings);
        VertexKey(s, points, varyings, key);

        auto found = s_programs.find(key);

        if (found == s_programs.end()) {
            found = s_programs.emplace(key, Build(s, points)).first;
            GetStats().programsBuilt++;
        }

        s_current = found->second.get();
        s.programDirty = false;
        s_lastPoints = points;
    }

    LinkedProgram& program = *s_current;
    Stats& stats = GetStats();

    if (stats.enabled) {
        stats.draws++;
        stats.arbVertex += s.vertexProgram && s.boundVertexProgram;
        stats.arbFragment += s.fragmentProgram && s.boundFragmentProgram;
        stats.points += points;
        stats.skipped += !program.valid;
    }

    if (!program.valid) {
        return false;
    }

    if (s_usedProgram != program.id) {
        glUseProgram(program.id);
        s_usedProgram = program.id;
    }

    UploadEnv();
    UploadUniforms(program);
    SetupAttribs(program);
    return true;
}

}
