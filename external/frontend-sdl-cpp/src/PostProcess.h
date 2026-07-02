#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Fullscreen post-process pass (GLES 3.1 only).
 *
 * projectM renders into an offscreen framebuffer; a single fullscreen shader then
 * applies, per pixel:
 *   1. Temporal persistence  — blend toward the previous displayed frame (strobe/flash reduction).
 *   2. Brightness            — uniform multiply.
 *   3. Monochrome tint       — collapse to luminance, remap to a single hue, mix by intensity.
 * The displayed frame is copied back into the "previous" texture for next frame's persistence.
 *
 * The pass runs only when Active() is true (any effect enabled); otherwise the caller
 * renders straight to the backbuffer. On non-GLES builds every method is a no-op and
 * Active() is false.
 */
class PostProcess
{
public:
    ~PostProcess();

    void SetReduceFlashing(bool enabled);
    void SetStrength(float strength);      //!< 0..1, higher = more persistence/smoothing.
    void SetBrightness(float brightness);  //!< 0..1 screen brightness multiplier.
    /** @param hexColor "#rrggbb" or "rrggbb". @param strength 0=original colors, 1=full monochrome. */
    void SetTint(bool enabled, const std::string& hexColor, float strength);

    /** @return true if any effect is active (persistence, dimming, or tint). */
    bool Active() const;

    /** Binds the offscreen scene framebuffer at the given size. No-op if inactive. */
    void Begin(int width, int height);

    /** @return framebuffer id to render the scene into (valid after Begin()); 0 if setup failed. */
    uint32_t SceneFbo() const;

    /** Draws the processed scene to the backbuffer and stores it for next frame. No-op if inactive. */
    void Composite();

private:
    void EnsureResources(int width, int height);
    void Destroy();
    static bool ParseHexColor(const std::string& hex, float& r, float& g, float& b);

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
    unsigned int _prevTex{0};
    unsigned int _vao{0};
    unsigned int _program{0};
    int _uPersistence{-1};
    int _uBrightness{-1};
    int _uTintEnabled{-1};
    int _uTintColor{-1};
    int _uTintStrength{-1};
    bool _ready{false};
};
