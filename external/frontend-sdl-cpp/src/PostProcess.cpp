#include "PostProcess.h"

#if USE_GLES

#include <GLES3/gl3.h>

#include <Poco/Logger.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
Poco::Logger& logger() { return Poco::Logger::get("PostProcess"); }

const char* kVertexShader = R"(#version 310 es
out vec2 vUV;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 310 es
precision highp float;
uniform sampler2D uScene;
uniform sampler2D uPrev;
uniform float uPersistence;   // 0 = no smoothing, ->1 = heavy trails
uniform float uBrightness;    // screen multiplier
uniform float uTintEnabled;   // >0.5 = tint on
uniform vec3  uTintColor;     // target hue
uniform float uTintStrength;  // 0 = original, 1 = full monochrome
in vec2 vUV;
out vec4 frag;
void main()
{
    vec3 scene    = texture(uScene, vUV).rgb;
    vec3 prev     = texture(uPrev,  vUV).rgb;
    vec3 smoothed = mix(scene, prev, uPersistence);   // temporal low-pass: dampens flashes + hue cycling
    vec3 c        = smoothed * uBrightness;
    if (uTintEnabled > 0.5)
    {
        float lum  = dot(c, vec3(0.2126, 0.7152, 0.0722));
        vec3  mono = lum * uTintColor;
        c = mix(c, mono, uTintStrength);
    }
    frag = vec4(c, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        poco_error_f1(logger(), "Post-process shader compile failed: %s", std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

PostProcess::~PostProcess()
{
    Destroy();
}

bool PostProcess::ParseHexColor(const std::string& hex, float& r, float& g, float& b)
{
    std::string h = hex;
    if (!h.empty() && h[0] == '#') { h = h.substr(1); }
    if (h.size() != 6) { return false; }
    for (char c : h)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c))) { return false; }
    }
    auto hexByte = [](const std::string& s) {
        return static_cast<float>(std::stoi(s, nullptr, 16)) / 255.0f;
    };
    r = hexByte(h.substr(0, 2));
    g = hexByte(h.substr(2, 2));
    b = hexByte(h.substr(4, 2));
    return true;
}

void PostProcess::SetReduceFlashing(bool enabled) { _reduceFlashing = enabled; }

void PostProcess::SetStrength(float strength)
{
    _strength = std::min(1.0f, std::max(0.0f, strength));
}

void PostProcess::SetBrightness(float brightness)
{
    _brightness = std::min(1.0f, std::max(0.0f, brightness));
}

void PostProcess::SetTint(bool enabled, const std::string& hexColor, float strength)
{
    _tintEnabled = enabled;
    _tintStrength = std::min(1.0f, std::max(0.0f, strength));
    float r, g, b;
    if (ParseHexColor(hexColor, r, g, b)) { _tintR = r; _tintG = g; _tintB = b; }
}

bool PostProcess::Active() const
{
    return _reduceFlashing || _brightness < 0.999f || _tintEnabled;
}

uint32_t PostProcess::SceneFbo() const
{
    return _fbo;
}

void PostProcess::EnsureResources(int width, int height)
{
    if (_ready && width == _width && height == _height)
    {
        return;
    }
    Destroy();
    _width = width;
    _height = height;

    glGenTextures(1, &_sceneTex);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &_prevTex);
    glBindTexture(GL_TEXTURE_2D, _prevTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &_depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glGenFramebuffers(1, &_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _sceneTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthRb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        poco_error(logger(), "Post-process framebuffer incomplete; disabling.");
        Destroy();
        _reduceFlashing = false;
        _brightness = 1.0f;
        _tintEnabled = false;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (_program == 0)
    {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
        if (vs && fs)
        {
            _program = glCreateProgram();
            glAttachShader(_program, vs);
            glAttachShader(_program, fs);
            glLinkProgram(_program);
            GLint ok = GL_FALSE;
            glGetProgramiv(_program, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                poco_error(logger(), "Post-process program link failed; disabling.");
                glDeleteProgram(_program);
                _program = 0;
            }
            else
            {
                glUseProgram(_program);
                glUniform1i(glGetUniformLocation(_program, "uScene"), 0);
                glUniform1i(glGetUniformLocation(_program, "uPrev"), 1);
                _uPersistence = glGetUniformLocation(_program, "uPersistence");
                _uBrightness = glGetUniformLocation(_program, "uBrightness");
                _uTintEnabled = glGetUniformLocation(_program, "uTintEnabled");
                _uTintColor = glGetUniformLocation(_program, "uTintColor");
                _uTintStrength = glGetUniformLocation(_program, "uTintStrength");
                glUseProgram(0);
            }
        }
        if (vs) { glDeleteShader(vs); }
        if (fs) { glDeleteShader(fs); }
        if (_program == 0)
        {
            Destroy();
            _reduceFlashing = false;
            _brightness = 1.0f;
            _tintEnabled = false;
            return;
        }
    }

    if (_vao == 0)
    {
        glGenVertexArrays(1, &_vao);
    }

    _ready = true;
}

void PostProcess::Begin(int width, int height)
{
    if (!Active() || width <= 0 || height <= 0)
    {
        return;
    }
    EnsureResources(width, height);
    if (!_ready)
    {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glViewport(0, 0, _width, _height);
}

void PostProcess::Composite()
{
    if (!Active() || !_ready)
    {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, _width, _height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(_program);
    // Persistence only applies when "reduce flashing" is on. k in [0, 0.9].
    float persistence = _reduceFlashing ? (0.9f * _strength) : 0.0f;
    glUniform1f(_uPersistence, persistence);
    glUniform1f(_uBrightness, _brightness);
    glUniform1f(_uTintEnabled, _tintEnabled ? 1.0f : 0.0f);
    glUniform3f(_uTintColor, _tintR, _tintG, _tintB);
    glUniform1f(_uTintStrength, _tintStrength);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, _prevTex);

    glBindVertexArray(_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // Store the displayed frame as "previous" for next frame's persistence term.
    // (Persistence is computed in displayed space; at the default brightness=1 / tint-off this is
    // an exact EMA of the scene. Tint preserves luminance so tinted trails are correct; dimming
    // only darkens trails slightly — cosmetically fine.)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _prevTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, _width, _height);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(0);
}

void PostProcess::Destroy()
{
    if (_fbo) { glDeleteFramebuffers(1, &_fbo); _fbo = 0; }
    if (_sceneTex) { glDeleteTextures(1, &_sceneTex); _sceneTex = 0; }
    if (_prevTex) { glDeleteTextures(1, &_prevTex); _prevTex = 0; }
    if (_depthRb) { glDeleteRenderbuffers(1, &_depthRb); _depthRb = 0; }
    if (_vao) { glDeleteVertexArrays(1, &_vao); _vao = 0; }
    if (_program) { glDeleteProgram(_program); _program = 0; }
    _ready = false;
    _width = 0;
    _height = 0;
}

#else // !USE_GLES — no-op stub for desktop builds

PostProcess::~PostProcess() {}
bool PostProcess::ParseHexColor(const std::string&, float&, float&, float&) { return false; }
void PostProcess::SetReduceFlashing(bool) {}
void PostProcess::SetStrength(float) {}
void PostProcess::SetBrightness(float) {}
void PostProcess::SetTint(bool, const std::string&, float) {}
bool PostProcess::Active() const { return false; }
void PostProcess::Begin(int, int) {}
uint32_t PostProcess::SceneFbo() const { return 0; }
void PostProcess::Composite() {}
void PostProcess::EnsureResources(int, int) {}
void PostProcess::Destroy() {}

#endif
