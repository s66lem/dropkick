#include "StrobeFilter.h"

#if USE_GLES

#include <GLES3/gl3.h>

#include <Poco/Logger.h>

#include <algorithm>
#include <string>

namespace
{
Poco::Logger& logger() { return Poco::Logger::get("StrobeFilter"); }

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
uniform float uMaxDelta;
in vec2 vUV;
out vec4 frag;
void main()
{
    vec3 s = texture(uScene, vUV).rgb;
    vec3 p = texture(uPrev, vUV).rgb;
    vec3 d = clamp(s - p, -uMaxDelta, uMaxDelta);
    frag = vec4(p + d, 1.0);
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
        poco_error_f1(logger(), "Strobe shader compile failed: %s", std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

StrobeFilter::~StrobeFilter()
{
    Destroy();
}

void StrobeFilter::SetEnabled(bool enabled)
{
    if (!enabled && _ready)
    {
        Destroy(); // free GPU resources when turned off
    }
    _enabled = enabled;
}

bool StrobeFilter::Enabled() const
{
    return _enabled;
}

void StrobeFilter::SetStrength(float strength)
{
    _strength = std::min(1.0f, std::max(0.0f, strength));
}

uint32_t StrobeFilter::SceneFbo() const
{
    return _fbo;
}

void StrobeFilter::EnsureResources(int width, int height)
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
        poco_error(logger(), "Strobe framebuffer incomplete; disabling filter.");
        Destroy();
        _enabled = false;
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
                poco_error(logger(), "Strobe program link failed; disabling filter.");
                glDeleteProgram(_program);
                _program = 0;
            }
            else
            {
                glUseProgram(_program);
                glUniform1i(glGetUniformLocation(_program, "uScene"), 0);
                glUniform1i(glGetUniformLocation(_program, "uPrev"), 1);
                _uMaxDelta = glGetUniformLocation(_program, "uMaxDelta");
                glUseProgram(0);
            }
        }
        if (vs) { glDeleteShader(vs); }
        if (fs) { glDeleteShader(fs); }
        if (_program == 0) { Destroy(); _enabled = false; return; }
    }

    if (_vao == 0)
    {
        glGenVertexArrays(1, &_vao);
    }

    _ready = true;
}

void StrobeFilter::Begin(int width, int height)
{
    if (!_enabled || width <= 0 || height <= 0)
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

void StrobeFilter::Composite()
{
    if (!_enabled || !_ready)
    {
        return;
    }

    // Draw the slew-limited scene to the backbuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, _width, _height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(_program);
    // maxDelta: strength 0 -> 1.0 (no damping), strength 1 -> 0.02 (heavy).
    float maxDelta = std::max(0.02f, 1.0f - _strength);
    glUniform1f(_uMaxDelta, maxDelta);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, _prevTex);

    glBindVertexArray(_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // Store what we just displayed as the previous frame for next time.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _prevTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, _width, _height);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(0);
}

void StrobeFilter::Destroy()
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

StrobeFilter::~StrobeFilter() {}
void StrobeFilter::SetEnabled(bool) {}
bool StrobeFilter::Enabled() const { return false; }
void StrobeFilter::SetStrength(float) {}
void StrobeFilter::Begin(int, int) {}
uint32_t StrobeFilter::SceneFbo() const { return 0; }
void StrobeFilter::Composite() {}
void StrobeFilter::EnsureResources(int, int) {}
void StrobeFilter::Destroy() {}

#endif
