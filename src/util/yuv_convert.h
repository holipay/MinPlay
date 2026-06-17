#pragma once
#include <cstdint>

uint8_t* ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h);
uint8_t* ConvertI420ToNV12(const uint8_t* i420, int w, int h);
bool ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h, uint8_t* out, int out_size);
bool ConvertI420ToNV12(const uint8_t* i420, int w, int h, uint8_t* out, int out_size);
