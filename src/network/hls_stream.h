#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <atomic>

struct HlsSegment {
    std::wstring url;
    double duration = 0;
    int64_t byte_offset = 0;  // offset in concatenated stream
    int64_t byte_size = 0;    // -1 if unknown
};

class HlsManager {
public:
    HlsManager();
    ~HlsManager();

    bool Open(const wchar_t* url);
    void Close();
    bool IsLive() const { return is_live_.load(std::memory_order_acquire); }
    double Duration() const { return duration_.load(std::memory_order_acquire); }

    // Get the byte stream (creates on first call)
    class HlsByteStream* GetByteStream();

private:
    friend struct HlsManagerTest;

    bool ParseMasterPlaylist(const std::string& content, const std::wstring& base_url);
    bool ParseMediaPlaylist(const std::string& content, const std::wstring& base_url);
    bool DownloadUrl(const wchar_t* url, std::vector<uint8_t>& out);
    std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative);

    void DownloadLoop();
    void ReloadPlaylist();

    std::vector<HlsSegment> segments_;
    std::atomic<bool> is_live_{false};
    std::atomic<double> duration_{0.0};
    int target_duration_ = 10;
    int media_sequence_ = 0;
    std::wstring playlist_url_;
    std::wstring media_playlist_url_;
    std::wstring base_url_;

    class HlsByteStream* byte_stream_ = nullptr;

    // Download thread
    HANDLE download_thread_ = nullptr;
    std::atomic<bool> download_running_{false};
    std::atomic<int> next_segment_to_download_{0};
    HANDLE wake_event_ = nullptr;

    int consumed_up_to_ = 0;
    CRITICAL_SECTION seg_lock_;
};

class HlsByteStream : public IMFByteStream {
public:
    HlsByteStream();
    ~HlsByteStream();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override;
    STDMETHODIMP GetLength(QWORD* pqwLength) override;
    STDMETHODIMP SetLength(QWORD) override;
    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override;
    STDMETHODIMP SetCurrentPosition(QWORD qwPosition) override;
    STDMETHODIMP IsEndOfStream(BOOL* pfEndOfStream) override;
    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* pcbRead) override;
    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) override;
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override;
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override;
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override;
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD dwSeekFlags, QWORD* pqwCurrentPosition) override;
    STDMETHODIMP Flush() override;
    STDMETHODIMP Close() override;

    // Called by HlsManager
    void AddSegment(const uint8_t* data, size_t size);
    void SetEndOfStream();
    void Clear();
    bool CheckAndClearNeedsWake();
    bool HasUnreadData() const;

private:
    std::atomic<LONG> ref_count_{1};
    CRITICAL_SECTION lock_;

    struct LoadedSeg {
        uint8_t* data = nullptr;
        size_t size = 0;
        int64_t offset = 0;
    };
    std::vector<LoadedSeg> segs_;
    std::atomic<int64_t> total_bytes_{0};
    bool end_of_stream_ = false;
    bool closed_ = false;
    bool has_eos_marker_ = false;

    std::atomic<int64_t> read_pos_{0};
    std::atomic<LONG> needs_wake_{0};

    ULONG async_result_ = 0;
    HRESULT async_hr_ = S_OK;

    struct PendingRead {
        BYTE* buf = nullptr;
        ULONG size = 0;
        IMFAsyncCallback* cb = nullptr;
        IUnknown* state = nullptr;
    };
    std::vector<PendingRead> pending_reads_;

    ULONG CopyFromSegmentsLocked(BYTE* pb, ULONG cb);
    void CancelPendingReadsLocked();
    void FulfillPendingReads();
};
