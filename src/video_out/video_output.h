#pragma once
#include <cstdint>

enum class PixelFormat { Unknown = 0, RGB32, NV12, YUY2, I420 };

class VideoOutput {
public:
    virtual ~VideoOutput() = default;
    virtual void Render(const uint8_t* data, int src_w, int src_h, int data_size, PixelFormat fmt) = 0;
    virtual void Resize(int w, int h) = 0;
};
