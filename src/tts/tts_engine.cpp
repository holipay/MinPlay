#include "tts_engine.h"
#include "../util/log.h"
#include <process.h>

TtsEngine::TtsEngine() {}

TtsEngine::~TtsEngine() {
    Stop();
}

bool TtsEngine::Initialize() {
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                  IID_ISpVoice, (void**)&voice_);
    if (FAILED(hr) || !voice_) {
        LOG_ERROR("Failed to create SAPI voice: 0x%08lX", hr);
        return false;
    }
    return true;
}

void TtsEngine::Start(HWND hwnd, const std::vector<std::wstring>& sentences, int start) {
    Stop();

    hwnd_ = hwnd;
    sentences_ = sentences;
    current_sentence_.store(start, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    speaking_.store(false, std::memory_order_relaxed);

    if (sentences_.empty()) {
        finished_.store(true, std::memory_order_relaxed);
        return;
    }

    thread_ = std::thread([this]() { ThreadProc(); });
}

void TtsEngine::Stop() {
    stop_requested_.store(true, std::memory_order_relaxed);

    if (voice_) {
        // Cancel any blocking Speak call
        voice_->Speak(L"", SPF_PURGEBEFORESPEAK, nullptr);
    }

    if (thread_.joinable())
        thread_.join();

    if (voice_) {
        voice_->Release();
        voice_ = nullptr;
    }

    sentences_.clear();
    finished_.store(true, std::memory_order_relaxed);
    speaking_.store(false, std::memory_order_relaxed);
}

void TtsEngine::Pause() {
    if (voice_) voice_->Pause();
}

void TtsEngine::Resume() {
    if (voice_) voice_->Resume();
}

void TtsEngine::SkipToSentence(int index) {
    if (index < 0 || index >= (int)sentences_.size()) return;

    // Cancel current speech
    stop_requested_.store(true, std::memory_order_relaxed);
    if (voice_) {
        voice_->Speak(L"", SPF_PURGEBEFORESPEAK, nullptr);
    }

    // Wait for thread to finish current sentence
    if (thread_.joinable())
        thread_.join();

    // Restart from new sentence
    current_sentence_.store(index, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    speaking_.store(false, std::memory_order_relaxed);

    thread_ = std::thread([this]() { ThreadProc(); });
}

void TtsEngine::SetRate(int rate) {
    rate_.store(rate, std::memory_order_relaxed);
    if (voice_) voice_->SetRate(rate);
}

void TtsEngine::SetVolume(int vol) {
    volume_.store(vol, std::memory_order_relaxed);
    if (voice_) voice_->SetVolume((USHORT)vol);
}

void TtsEngine::ThreadProc() {
    speaking_.store(true, std::memory_order_relaxed);

    int start = current_sentence_.load(std::memory_order_relaxed);
    for (int i = start; i < (int)sentences_.size(); i++) {
        if (stop_requested_.load(std::memory_order_relaxed)) break;

        current_sentence_.store(i, std::memory_order_relaxed);

        // Synchronous Speak — blocks until sentence finishes
        voice_->Speak(sentences_[i].c_str(), 0, nullptr);

        if (stop_requested_.load(std::memory_order_relaxed)) break;

        // Notify main thread
        PostMessage(hwnd_, WM_APP + 5, (WPARAM)i, 0);
    }

    speaking_.store(false, std::memory_order_relaxed);

    if (!stop_requested_.load(std::memory_order_relaxed)) {
        finished_.store(true, std::memory_order_relaxed);
        PostMessage(hwnd_, WM_APP + 6, 0, 0);
    }
}
