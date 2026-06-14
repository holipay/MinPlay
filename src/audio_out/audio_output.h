#pragma once
#include <cstdint>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual void Write(const uint8_t* data, int size) = 0;
    virtual void SetPts(double pts) = 0;
    virtual double GetClock() = 0;
    virtual int GetBuffered() = 0;
    virtual int GetFree() = 0;
    virtual bool IsExclusive() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Reset() = 0;
};
