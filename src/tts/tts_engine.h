#pragma once
#include <windows.h>
#include <sapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

class TtsEngine {
public:
    TtsEngine();
    ~TtsEngine();

    bool Initialize();
    void Start(HWND hwnd, const std::vector<std::wstring>& sentences, int start = 0);
    void Stop();
    void Pause();
    void Resume();
    void SkipToSentence(int index);
    void SetRate(int rate);
    void SetVolume(int vol);
    int GetVolume() const { return volume_.load(std::memory_order_relaxed); }
    int GetRate() const { return rate_.load(std::memory_order_relaxed); }
    int GetCurrentSentence() const { return current_sentence_.load(std::memory_order_relaxed); }
    int GetSentenceCount() const { return (int)sentences_.size(); }
    bool IsFinished() const { return finished_.load(std::memory_order_relaxed); }
    bool IsSpeaking() const { return speaking_.load(std::memory_order_relaxed); }

private:
    void ThreadProc();

    ISpVoice* voice_ = nullptr;
    HWND hwnd_ = nullptr;
    std::vector<std::wstring> sentences_;
    std::atomic<int> current_sentence_{0};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> speaking_{false};
    std::atomic<int> volume_{100};
    std::atomic<int> rate_{0};
    std::thread thread_;
};
