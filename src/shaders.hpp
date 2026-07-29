// shaders.hpp — GLSL ソースとコンパイル/リンクの補助。
// シェーダ本体はバージョン行を持たない。compileShader が対象に応じて
//   ネイティブ: #version 330 core
//   Web:        #version 300 es (+ fragment は precision 宣言)
// を先頭に付与する。WebGL2(GLES3) は 330 core とほぼ同機能なので本体は共通。
#pragma once
#include "gl.hpp"
#include <string>
#include <iostream>

namespace fl {

inline const char* MODEL_VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vN;
out vec3 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vN = aNormal;
    vColor = aColor;
}
)";

inline const char* MODEL_FS = R"(
in vec3 vN;
in vec3 vColor;
uniform vec3  uLightDir;
uniform float uWire;
uniform float uHighlight; // 1.0 → 黄色ハイライト
out vec4 frag;
void main() {
    vec3 N = normalize(vN);
    float d = max(dot(N, normalize(uLightDir)), 0.0);
    vec3 base = (uHighlight > 0.5) ? vec3(1.0, 0.86, 0.15) : vColor;
    vec3 c = base * (0.38 + 0.72 * d);
    if (uWire > 0.5) c = vec3(0.16, 0.86, 0.55);
    frag = vec4(c, 1.0);
}
)";

inline const char* LINE_VS = R"(
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

inline const char* LINE_FS = R"(
uniform vec3 uColor;
out vec4 frag;
void main() { frag = vec4(uColor, 1.0); }
)";

inline GLuint compileShader(GLenum type, const char* src) {
    // 対象に応じた #version ヘッダを本体の先頭に付与する。
#ifdef __EMSCRIPTEN__
    std::string header = "#version 300 es\n";
    if (type == GL_FRAGMENT_SHADER)
        header += "precision highp float;\nprecision highp int;\n";
#else
    std::string header = "#version 330 core\n";
#endif
    std::string full = header + src;
    const char* fullSrc = full.c_str();

    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &fullSrc, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, 1024, nullptr, log);
        std::cerr << "shader compile error: " << log << std::endl;
    }
    return s;
}

inline GLuint linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, 1024, nullptr, log);
        std::cerr << "program link error: " << log << std::endl;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

} // namespace fl
