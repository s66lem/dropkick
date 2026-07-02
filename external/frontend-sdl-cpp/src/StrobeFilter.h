#pragma once

#include <cstdint>

/**
 * @brief Optional "reduce flashing" post-process.
 *
 * When enabled, projectM renders into an offscreen framebuffer and a fullscreen
 * pass limits how fast each pixel's brightness may change per frame (comparing
 * against the previous displayed frame), blunting strobing on any preset. When
 * disabled it does nothing and the caller renders straight to the backbuffer.
 *
 * GLES 3.1 only; on non-GLES builds every method is a no-op and Enabled() is false.
 */
class StrobeFilter
{
public:
    ~StrobeFilter();

    void SetEnabled(bool enabled);
    bool Enabled() const;

    /** @param strength 0..1, higher = more damping (smaller allowed brightness step). */
    void SetStrength(float strength);

    /**
     * @brief Prepares resources at the given size and binds the offscreen scene framebuffer
     * (cleared). Render projectM into SceneFbo() after calling this. No-op if disabled.
     */
    void Begin(int width, int height);

    /** @return The framebuffer object id to render the scene into (valid after Begin()). */
    uint32_t SceneFbo() const;

    /**
     * @brief Draws the slew-limited scene to the backbuffer and stores it as the previous frame.
     * No-op if disabled.
     */
    void Composite();

private:
    void EnsureResources(int width, int height);
    void Destroy();

    bool _enabled{false};
    float _strength{0.6f};
    int _width{0};
    int _height{0};

    unsigned int _fbo{0};
    unsigned int _sceneTex{0};
    unsigned int _depthRb{0};
    unsigned int _prevTex{0};
    unsigned int _vao{0};
    unsigned int _program{0};
    int _uMaxDelta{-1};
    bool _ready{false};
};
