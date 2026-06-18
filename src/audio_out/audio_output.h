#pragma once
#include <cstdint>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual int Write(const uint8_t* data, int size) = 0;
    virtual void SetPts(double pts) = 0;
    virtual double GetClock() = 0;
    virtual int GetBuffered() = 0;
    virtual int GetFree() = 0;
    virtual bool IsExclusive() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Reset() = 0;
    virtual int GetBytesPerSec() const = 0;
    virtual int GetInputByteRate() const = 0;

    virtual void SetVolume(float /*vol*/) {}
    virtual float GetVolume() const { return 1.0f; }
    virtual void SetMuted(bool /*m*/) {}
    virtual bool IsMuted() const { return false; }
};
