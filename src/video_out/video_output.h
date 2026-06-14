#pragma once
#include <cstdint>

class VideoOutput {
public:
    virtual ~VideoOutput() = default;
    virtual void Render(const uint8_t* data, int src_w, int src_h, int data_size) = 0;
    virtual void Resize(int w, int h) = 0;
};
