#pragma once
#include <windows.h>
#include <memory>

struct PlaybackFrame;
enum class VideoFilter { Normal, Grayscale, Invert };
struct VideoEffects {
    float brightness = 1.f;
    int rotation = 0; // Clockwise degrees: 0, 90, 180, 270 (multiples wrap).
    float zoom = 1.f; // >1 crops the center, <1 adds black borders.
    VideoFilter filter = VideoFilter::Normal;
};

// All calls, including destruction, must occur on the initializing UI thread.
// The caller creates/destroys ImGui and owns its Win32 platform backend.
class OpenGLRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();
    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
    bool initialize(HWND window);
    void resize(unsigned int width, unsigned int height);
    bool updateVideo(std::shared_ptr<const PlaybackFrame> frame, VideoEffects effects);
    // RGBA8 texture with top row at UV v=0, usable as an ImTextureID without flipping.
    // Dimensions are frame width/height, swapped for 90/270 degrees.
    unsigned int frameTexture() const;
    void begin();
    void present();
    // Captures this window's back buffer and ImGui draw data as PNG, even after present.
    bool saveScreenshot(const wchar_t* path);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
