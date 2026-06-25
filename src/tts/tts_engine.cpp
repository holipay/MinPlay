#include "tts_engine.h"
#include "../util/log.h"
#include <sphelper.h>

TtsEngine::TtsEngine() {}

TtsEngine::~TtsEngine() {
    Stop();
}

// Find a SAPI voice that supports Chinese — match by name keywords
static ISpObjectToken* FindChineseVoice() {
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(SpGetCategoryFromId(SPCAT_VOICES, &cat)) || !cat)
        return nullptr;

    IEnumSpObjectTokens* tokens = nullptr;
    if (FAILED(cat->EnumTokens(nullptr, nullptr, &tokens)) || !tokens) {
        cat->Release();
        return nullptr;
    }

    ISpObjectToken* token = nullptr;
    ISpObjectToken* fallback = nullptr;
    while (tokens->Next(1, &token, nullptr) == S_OK) {
        LPWSTR name = nullptr;
        if (SUCCEEDED(token->GetStringValue(nullptr, &name)) && name) {
            bool is_chinese = (wcsstr(name, L"Chinese") != nullptr) ||
                              (wcsstr(name, L"中文") != nullptr) ||
                              (wcsstr(name, L"Huihui") != nullptr) ||
                              (wcsstr(name, L"Hui") != nullptr);
            CoTaskMemFree(name);

            if (is_chinese) {
                tokens->Release();
                cat->Release();
                return token;
            }
        }
        if (!fallback) {
            fallback = token;
            token = nullptr;
        }
        if (token) token->Release();
        token = nullptr;
    }

    tokens->Release();
    cat->Release();
    return fallback;
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

    stop_requested_.store(true, std::memory_order_relaxed);
    if (voice_) {
        voice_->Speak(L"", SPF_PURGEBEFORESPEAK, nullptr);
    }

    if (thread_.joinable())
        thread_.join();

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
    // SAPI requires STA — initialize COM as apartment-threaded on this thread
    HRESULT comhr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(comhr);

    // Create SAPI voice on this STA thread
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                  IID_ISpVoice, (void**)&voice_);
    if (FAILED(hr) || !voice_) {
        LOG_ERROR("TTS: Failed to create SAPI voice: 0x%08lX", hr);
        finished_.store(true, std::memory_order_relaxed);
        PostMessage(hwnd_, WM_APP + 6, 0, 0);
        if (com_inited) CoUninitialize();
        return;
    }

    // Try to find and set a Chinese voice
    ISpObjectToken* cnToken = FindChineseVoice();
    if (cnToken) {
        voice_->SetVoice(cnToken);
        cnToken->Release();
        LOG_INFO("TTS: Using Chinese voice");
    } else {
        LOG_INFO("TTS: No Chinese voice found, using default");
    }

    // Apply any rate/volume set before thread started
    voice_->SetRate(rate_.load(std::memory_order_relaxed));
    voice_->SetVolume((USHORT)volume_.load(std::memory_order_relaxed));

    speaking_.store(true, std::memory_order_relaxed);

    int start = current_sentence_.load(std::memory_order_relaxed);
    for (int i = start; i < (int)sentences_.size(); i++) {
        if (stop_requested_.load(std::memory_order_relaxed)) break;

        current_sentence_.store(i, std::memory_order_relaxed);

        voice_->Speak(sentences_[i].c_str(), 0, nullptr);

        if (stop_requested_.load(std::memory_order_relaxed)) break;

        PostMessage(hwnd_, WM_APP + 5, (WPARAM)i, 0);
    }

    voice_->Release();
    voice_ = nullptr;

    speaking_.store(false, std::memory_order_relaxed);

    if (!stop_requested_.load(std::memory_order_relaxed)) {
        finished_.store(true, std::memory_order_relaxed);
        PostMessage(hwnd_, WM_APP + 6, 0, 0);
    }

    if (com_inited) CoUninitialize();
}
