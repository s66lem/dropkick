#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

/** Fullscreen brightness, tint, and optional temporal smoothing on GLES or desktop GL. */
class PostProcess
{
public:
    static constexpr int kPreviewWidth = 240;
    static constexpr int kPreviewHeight = 150;

    ~PostProcess();

    void SetReduceFlashing(bool enabled);
    void SetStrength(float strength);
    void SetBrightness(float brightness);
    void SetTint(bool enabled, std::string_view hexColor, float strength);
    void SetTint(bool enabled, float r, float g, float b, float strength);
    static bool ParseHexColor(std::string_view hex, float& r, float& g, float& b);

    bool Active() const;
    void Begin(int width, int height);
    uint32_t SceneFbo() const;
    void Composite();
    bool CapturePreview(int width, int height, std::vector<uint8_t>& outBmp);

private:
    struct Program
    {
        unsigned int id{0};
        int persistence{-1};
        int brightness{-1};
        int tintEnabled{-1};
        int tintColor{-1};
        int tintStrength{-1};

        bool Initialize(bool useHistory);
    };

    bool EnsureScene();
    bool EnsureHistory();
    void DestroyHistory();
    void DestroyTargets();

    bool _reduceFlashing{false};
    float _strength{0.6f};
    float _brightness{1.0f};
    bool _tintEnabled{false};
    float _tintR{0.0f};
    float _tintG{1.0f};
    float _tintB{0.0f};
    float _tintStrength{1.0f};

    int _width{0};
    int _height{0};
    unsigned int _fbo{0};
    unsigned int _sceneTex{0};
    unsigned int _depthRb{0};
    unsigned int _displayTex{0};
    unsigned int _historyTex[2]{};
    unsigned int _compositeFbo[2]{};
    unsigned int _historyIdx{0};
    bool _hasHistory{false};
    unsigned int _previewFbo{0};
    unsigned int _previewTex{0};
    std::vector<uint8_t> _previewRgba;
    unsigned int _vao{0};
    Program _direct;
    Program _smooth;
    bool _begun{false};
    bool _failed{false};
};
