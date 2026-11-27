// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <regex>
#include <string>

namespace cardinalShader {

inline std::string withoutComments(const std::string& source) {
    static const std::regex comments(R"(/\*[\s\S]*?\*/|//[^\n]*)");
    return std::regex_replace(source, comments, " ");
}

inline bool hasMain(const std::string& code) {
    static const std::regex pattern(R"(\bvoid\s+main\s*\()");
    return std::regex_search(code, pattern);
}

inline bool hasImage(const std::string& code) {
    static const std::regex pattern(R"(\bvoid\s+mainImage\s*\()");
    return std::regex_search(code, pattern);
}

inline bool hasSound(const std::string& code) {
    static const std::regex pattern(R"(\bvec2\s+mainSound\s*\()");
    return std::regex_search(code, pattern);
}

constexpr char twiglHelpers[] = R"GLSL(
#define PI 3.141592653589793
#define PI2 6.283185307179586
mat2 rotate2D(float angle) {
    vec2 cs = vec2(cos(angle), sin(angle));
    return mat2(cs, vec2(-cs.y, cs.x));
}
mat3 rotate3D(float angle, vec3 axis) {
    vec3 n = normalize(axis);
    float c = cos(angle);
    mat3 crossMatrix = mat3(0., n.z, -n.y, -n.z, 0., n.x, n.y, -n.x, 0.);
    return c * mat3(1.) + (1. - c) * outerProduct(n, n) + sin(angle) * crossMatrix;
}
vec3 hsv(float hue, float saturation, float value) {
    vec3 rgb = clamp(abs(mod(hue * 6. + vec3(0., 4., 2.), 6.) - 3.) - 1., 0., 1.);
    return value * mix(vec3(1.), rgb, saturation);
}
float fsnoise(vec2 point) {
    return fract(43758.5453 * sin(dot(point, vec2(12.9898, 78.233))));
}
float fsnoiseDigits(vec2 point) {
    return fract(437.585453 * sin(dot(point, vec2(.129898, .78233))));
}
)GLSL";

inline bool usesMrt(const std::string& code) {
    static const std::regex pattern(R"(\b(?:outColor1|backbuffer1|o1|b1)\b)");
    return std::regex_search(code, pattern);
}

inline bool usesFeedback(const std::string& code) {
    static const std::regex pattern(R"(\b(?:texture|texture2D|texelFetch)\s*\(\s*(?:backbuffer[01]?|b[01]?)\b)");
    return std::regex_search(code, pattern);
}

inline std::string prepare(const std::string& source, const std::string& code, bool audio) {
    static const std::regex twigl(R"(\b(?:FC|fragCoord|fragcoord|backbuffer[01]?|outColor[01])\b|\bo[01]?\s*(?:[.+*/-]?=|\.[xyzwrgba])|\buniform\s+\w+\s+(?:resolution|mouse|time|frame|r|m|t|f|b)\s*;)");
    const bool image = hasImage(code);
    const bool main = hasMain(code);
    const bool compact = !image && !audio && (!main || std::regex_search(code, twigl));
    if (!image && !audio && !compact)
        return source;

    static const std::regex version(R"(^[ \t\r\n]*#version[^\n]*\n)");
    std::string body = std::regex_replace(source, version, "");
    std::string header = R"GLSL(#version 300 es
precision highp float;
precision highp int;
uniform vec3 iResolution;
uniform float iTime, iTimeDelta, iSampleRate;
uniform int iFrame;
uniform vec4 iMouse, iDate;
uniform float iChannelTime[4];
uniform vec3 iChannelResolution[4];
uniform sampler2D iChannel0, iChannel1, iChannel2, iChannel3;
in vec2 v_uv;
layout(location = 0) out vec4 cardinalColor;
#define gl_FragColor cardinalColor
#define texture2D texture
)GLSL";

    if (compact) {
        if (usesMrt(code))
            header += "\nlayout(location = 1) out vec4 cardinalSecondColor;\n";
        static const std::regex declarations(
            R"(\buniform\s+(?:float|int|vec[234]|sampler2D)\s+(?:resolution|mouse|time|frame|backbuffer|r|m|t|f|b)\s*;|(?:\blayout\s*\([^)]*\)\s*)?\bout\s+vec4\s+(?:outColor[01]?|o[01]?)\s*;)");
        static const std::regex floatLoop(R"(for\s*\(\s*float\s+([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*;)");
        static const std::regex loopNames(R"([A-Za-z_]\w*)");
        body = std::regex_replace(body, declarations, "");
        std::string initialized;
        size_t start = 0;
        for (std::sregex_iterator it(body.begin(), body.end(), floatLoop), end; it != end; ++it) {
            const auto& match = *it;
            initialized.append(body, start, match.position() - start);
            initialized += "for(float ";
            const auto names = match.str(1);
            bool first = true;
            for (std::sregex_iterator name(names.begin(), names.end(), loopNames); name != end; ++name) {
                if (!first)
                    initialized += ", ";
                initialized += name->str() + "=0.";
                first = false;
            }
            initialized += ";";
            start = match.position() + match.length();
        }
        body = initialized + body.substr(start);
        header += twiglHelpers;
        header += R"GLSL(
#define FC gl_FragCoord
#define fragCoord gl_FragCoord
#define fragcoord gl_FragCoord
#define resolution iResolution.xy
#define mouse (iMouse.xy / iResolution.xy)
#define time iTime
#define frame float(iFrame)
#define backbuffer iChannel0
#define backbuffer0 iChannel0
#define backbuffer1 iChannel1
#define r resolution
#define m mouse
#define t time
#define f frame
#define b backbuffer
#define b0 backbuffer0
#define b1 backbuffer1
#define outColor cardinalColor
#define outColor0 cardinalColor
#define outColor1 cardinalSecondColor
#define o cardinalColor
#define o0 cardinalColor
#define o1 cardinalSecondColor
)GLSL";
        if (!main)
            return header + "\nvoid main() { cardinalColor = vec4(0., 0., 0., 1.);\n#line 1\n" + body + "\n}";
    }
    if (audio) {
        static const std::regex indexed(R"(\bvec2\s+mainSound\s*\(\s*(?:in\s+)?(?:const\s+)?(?:(?:lowp|mediump|highp)\s+)?int\b)");
        header += "\nuniform int cardinalSampleBase;\nuniform float cardinalTimeBase;\n";
        const std::string call = std::regex_search(code, indexed)
            ? "mainSound(cardinalSampleBase + offset, seconds)" : "mainSound(seconds)";
        if (main)
            header += "#define main cardinalImageMain\n";
        return header + "\n#line 1\n" + body + R"GLSL(
#undef main
void main() {
    int offset = int(gl_FragCoord.x) + int(gl_FragCoord.y) * 256;
    float seconds = cardinalTimeBase + float(offset) / iSampleRate;
    vec2 samples = )GLSL" + call + R"GLSL(;
    if (isnan(samples.x) || isinf(samples.x)) samples.x = 0.;
    if (isnan(samples.y) || isinf(samples.y)) samples.y = 0.;
    vec2 encoded = floor(clamp(samples, -1., 1.) * 32767. + 32768.5);
    vec2 high = floor(encoded / 256.);
    vec2 low = encoded - high * 256.;
    cardinalColor = vec4(low.x, high.x, low.y, high.y) / 255.;
}
)GLSL";
    }
    if (image && !main)
        return header + "\n#line 1\n" + body + "\nvoid main() { mainImage(cardinalColor, gl_FragCoord.xy); }";
    return header + "\n#line 1\n" + body;
}
}
