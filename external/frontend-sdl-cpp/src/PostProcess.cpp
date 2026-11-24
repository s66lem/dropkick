#include "PostProcess.h"
#include "PresetLists.h"

#if defined(USE_GLES) || defined(USE_GLEW)

#if defined(USE_GLES)
#include <GLES3/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <Poco/Logger.h>
#include <algorithm>
#include <string>

namespace
{
Poco::Logger& Logger() { return Poco::Logger::get("PostProcess"); }

#if defined(USE_GLES)
constexpr auto shaderVersion = "#version 300 es\nprecision highp float;\n";
#else
constexpr auto shaderVersion = "#version 330 core\n";
#endif

constexpr auto vertexShader = R"(
out vec2 vUV;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr auto fragmentShader = R"(
uniform sampler2D uScene;
uniform float uBrightness;
uniform float uTintEnabled;
uniform vec3 uTintColor;
uniform float uTintStrength;
in vec2 vUV;
layout(location = 0) out vec4 outDisplay;
#ifdef USE_HISTORY
uniform sampler2D uPrev;
uniform float uPersistence;
layout(location = 1) out vec4 outSmoothed;
#endif

void main()
{
    vec3 smoothed = texture(uScene, vUV).rgb;
#ifdef USE_HISTORY
    if (uPersistence > 0.0)
    {
        smoothed = mix(smoothed, texture(uPrev, vUV).rgb, uPersistence);
    }
    outSmoothed = vec4(smoothed, 1.0);
#endif
    vec3 color = smoothed * uBrightness;
    if (uTintEnabled > 0.5)
    {
        float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = mix(color, luminance * uTintColor, uTintStrength);
    }
    outDisplay = vec4(color, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source, const char* defines)
{
    const char* sources[]{shaderVersion, defines, source};
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 3, sources, nullptr);
    glCompileShader(shader);
    GLint ready;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ready);
    if (ready) { return shader; }

    char log[512]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    poco_error_f1(Logger(), "Post-process shader compile failed: %s", std::string(log));
    glDeleteShader(shader);
    return 0;
}

void AllocateTexture(GLuint& texture, int width, int height)
{
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
}

PostProcess::~PostProcess()
{
    DestroyTargets();
    if (_vao) { glDeleteVertexArrays(1, &_vao); }
    if (_direct.id) { glDeleteProgram(_direct.id); }
    if (_smooth.id) { glDeleteProgram(_smooth.id); }
}

bool PostProcess::Program::Initialize(bool useHistory)
{
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexShader, "");
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentShader, useHistory ? "#define USE_HISTORY\n" : "");
    if (vertex && fragment)
    {
        id = glCreateProgram();
        glAttachShader(id, vertex);
        glAttachShader(id, fragment);
        glLinkProgram(id);
        GLint linked;
        glGetProgramiv(id, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            char log[512]{};
            glGetProgramInfoLog(id, sizeof(log), nullptr, log);
            poco_error_f1(Logger(), "Post-process program link failed: %s", std::string(log));
            glDeleteProgram(id);
            id = 0;
        }
    }
    if (vertex) { glDeleteShader(vertex); }
    if (fragment) { glDeleteShader(fragment); }
    if (!id) { return false; }

    glUseProgram(id);
    glUniform1i(glGetUniformLocation(id, "uScene"), 0);
    if (useHistory)
    {
        glUniform1i(glGetUniformLocation(id, "uPrev"), 1);
        persistence = glGetUniformLocation(id, "uPersistence");
    }
    brightness = glGetUniformLocation(id, "uBrightness");
    tintEnabled = glGetUniformLocation(id, "uTintEnabled");
    tintColor = glGetUniformLocation(id, "uTintColor");
    tintStrength = glGetUniformLocation(id, "uTintStrength");
    glUseProgram(0);
    return true;
}

void PostProcess::SetReduceFlashing(bool enabled)
{
    if (_reduceFlashing != enabled) { _failed = false; }
    if (_reduceFlashing && !enabled) { DestroyHistory(); }
    _reduceFlashing = enabled;
}

void PostProcess::SetStrength(float strength)
{
    strength = std::clamp(strength, 0.0f, 1.0f);
    if (_strength != strength) { _failed = false; }
    _strength = strength;
}

void PostProcess::SetBrightness(float brightness)
{
    brightness = std::clamp(brightness, 0.0f, 1.0f);
    if (_brightness != brightness) { _failed = false; }
    _brightness = brightness;
}

void PostProcess::SetTint(bool enabled, std::string_view hexColor, float strength)
{
    float r = _tintR, g = _tintG, b = _tintB;
    ParseHexColor(hexColor, r, g, b);
    SetTint(enabled, r, g, b, strength);
}

void PostProcess::SetTint(bool enabled, float r, float g, float b, float strength)
{
    strength = std::clamp(strength, 0.0f, 1.0f);
    if (_tintEnabled != enabled || _tintR != r || _tintG != g || _tintB != b || _tintStrength != strength)
    {
        _failed = false;
    }
    _tintEnabled = enabled;
    _tintR = r;
    _tintG = g;
    _tintB = b;
    _tintStrength = strength;
}

bool PostProcess::Active() const
{
    return !_failed && (_reduceFlashing || _brightness < 0.999f || _tintEnabled);
}

uint32_t PostProcess::SceneFbo() const { return _begun ? _fbo : 0; }

bool PostProcess::EnsureScene()
{
    if (_fbo) { return true; }

    glActiveTexture(GL_TEXTURE0);
    AllocateTexture(_sceneTex, _width, _height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &_depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, _width, _height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _sceneTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthRb);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool PostProcess::EnsureHistory()
{
    if (_displayTex) { return true; }
    glActiveTexture(GL_TEXTURE0);
    AllocateTexture(_displayTex, _width, _height);
    glGenFramebuffers(2, _compositeFbo);
    constexpr GLenum buffers[]{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    for (unsigned int i = 0; i < 2; ++i)
    {
        AllocateTexture(_historyTex[i], _width, _height);
        glBindFramebuffer(GL_FRAMEBUFFER, _compositeFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _displayTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, _historyTex[i], 0);
        glDrawBuffers(2, buffers);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { return false; }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void PostProcess::Begin(int width, int height)
{
    _begun = false;
    if (width != _width || height != _height)
    {
        DestroyTargets();
        _width = width;
        _height = height;
        _failed = false;
    }
    if (!Active() || width <= 0 || height <= 0) { return; }

    auto& program = _reduceFlashing ? _smooth : _direct;
    if ((!program.id && !program.Initialize(_reduceFlashing)) || !EnsureScene() ||
        (_reduceFlashing && !EnsureHistory()))
    {
        poco_error(Logger(), "Post-process setup failed. Rendering directly to the backbuffer.");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyTargets();
        _failed = true;
        return;
    }
    if (!_vao) { glGenVertexArrays(1, &_vao); }
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glViewport(0, 0, _width, _height);
    _begun = true;
}

void PostProcess::Composite()
{
    if (!_begun) { return; }
    _begun = false;

    const unsigned int writeIdx = _historyIdx ^ 1;
    const auto& program = _reduceFlashing ? _smooth : _direct;
    glBindFramebuffer(GL_FRAMEBUFFER, _reduceFlashing ? _compositeFbo[writeIdx] : 0);
    glViewport(0, 0, _width, _height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);

    glUseProgram(program.id);
    glUniform1f(program.brightness, _brightness);
    glUniform1f(program.tintEnabled, _tintEnabled ? 1.0f : 0.0f);
    glUniform3f(program.tintColor, _tintR, _tintG, _tintB);
    glUniform1f(program.tintStrength, _tintStrength);
    if (_reduceFlashing)
    {
        glUniform1f(program.persistence, _hasHistory ? 0.9f * _strength : 0.0f);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, _hasHistory ? _historyTex[_historyIdx] : _sceneTex);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glBindVertexArray(_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    if (_reduceFlashing)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _compositeFbo[writeIdx]);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, _width, _height, 0, 0, _width, _height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        _historyIdx = writeIdx;
        _hasHistory = true;
    }
}

bool PostProcess::CapturePreview(int width, int height, std::vector<uint8_t>& outBmp)
{
    if (width <= 0 || height <= 0) { return false; }

    _previewRgba.resize(kPreviewWidth * kPreviewHeight * 4);
    if (!_previewFbo)
    {
        GLint textureBinding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
        glGenTextures(1, &_previewTex);
        glBindTexture(GL_TEXTURE_2D, _previewTex);
        if (_previewTex)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kPreviewWidth, kPreviewHeight, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBinding));

        glGenFramebuffers(1, &_previewFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _previewFbo);
        if (_previewFbo && _previewTex)
        {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _previewTex, 0);
        }
        if (!_previewFbo || !_previewTex || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &_previewFbo);
            glDeleteTextures(1, &_previewTex);
            _previewFbo = 0;
            _previewTex = 0;
            return false;
        }
    }

    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorEnabled) { glDisable(GL_SCISSOR_TEST); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _previewFbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, kPreviewWidth, kPreviewHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (scissorEnabled) { glEnable(GL_SCISSOR_TEST); }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, _previewFbo);
    GLint packAlignment = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, kPreviewWidth, kPreviewHeight, GL_RGBA, GL_UNSIGNED_BYTE, _previewRgba.data());
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return PresetLists::EncodeBmp24FromRgba(kPreviewWidth, kPreviewHeight, _previewRgba.data(),
                                           _previewRgba.size(), outBmp);
}

void PostProcess::DestroyHistory()
{
    if (_displayTex)
    {
        glDeleteFramebuffers(2, _compositeFbo);
        glDeleteTextures(2, _historyTex);
        glDeleteTextures(1, &_displayTex);
        _displayTex = 0;
    }
    _historyIdx = 0;
    _hasHistory = false;
}

void PostProcess::DestroyTargets()
{
    DestroyHistory();
    if (_previewFbo) { glDeleteFramebuffers(1, &_previewFbo); _previewFbo = 0; }
    if (_previewTex) { glDeleteTextures(1, &_previewTex); _previewTex = 0; }
    if (_fbo) { glDeleteFramebuffers(1, &_fbo); _fbo = 0; }
    if (_sceneTex) { glDeleteTextures(1, &_sceneTex); _sceneTex = 0; }
    if (_depthRb) { glDeleteRenderbuffers(1, &_depthRb); _depthRb = 0; }
}

#else

PostProcess::~PostProcess() = default;
void PostProcess::SetReduceFlashing(bool) {}
void PostProcess::SetStrength(float) {}
void PostProcess::SetBrightness(float) {}
void PostProcess::SetTint(bool, std::string_view, float) {}
void PostProcess::SetTint(bool, float, float, float, float) {}
bool PostProcess::Active() const { return false; }
void PostProcess::Begin(int, int) {}
uint32_t PostProcess::SceneFbo() const { return 0; }
void PostProcess::Composite() {}
bool PostProcess::CapturePreview(int, int, std::vector<uint8_t>&) { return false; }

#endif

bool PostProcess::ParseHexColor(std::string_view hex, float& r, float& g, float& b)
{
    return PresetLists::ParseHexColor(hex, r, g, b);
}
