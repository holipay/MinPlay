#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>

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
    bool IsLive() const { return is_live_; }
    double Duration() const { return duration_; }

    // Get the byte stream (creates on first call)
    class HlsByteStream* GetByteStream();

private:
    bool ParseMasterPlaylist(const std::string& content, const std::wstring& base_url);
    bool ParseMediaPlaylist(const std::string& content, const std::wstring& base_url);
    bool DownloadUrl(const wchar_t* url, std::vector<uint8_t>& out);
    std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative);

    void DownloadLoop();

    std::vector<HlsSegment> segments_;
    bool is_live_ = false;
    double duration_ = 0;
    int target_duration_ = 10;
    int media_sequence_ = 0;
    std::wstring playlist_url_;
    std::wstring media_playlist_url_;
    std::wstring base_url_;

    class HlsByteStream* byte_stream_ = nullptr;

    // Download thread
    HANDLE download_thread_ = nullptr;
    volatile bool download_running_ = false;
    volatile int next_segment_to_download_ = 0;
    HANDLE wake_event_ = nullptr;

    // Segment buffer (parallel to segments_)
    CRITICAL_SECTION seg_lock_;
    std::vector<uint8_t*> segment_data_;  // raw TS data per segment
    std::vector<size_t> segment_sizes_;
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
    void SetSegments(const HlsSegment* segs, int count);
    void SetEndOfStream();
    void Clear();
    bool HasCapability(DWORD cap) const;

private:
    volatile LONG ref_count_ = 1;
    CRITICAL_SECTION lock_;

    struct LoadedSeg {
        uint8_t* data = nullptr;
        size_t size = 0;
        int64_t offset = 0;
    };
    std::vector<LoadedSeg> segs_;
    int64_t total_bytes_ = 0;
    bool end_of_stream_ = false;
    bool closed_ = false;
    bool has_eos_marker_ = false;

    int64_t read_pos_ = 0;

    // Async read support
    BYTE* async_buf_ = nullptr;
    ULONG async_size_ = 0;
    IMFAsyncCallback* async_cb_ = nullptr;
    IUnknown* async_state_ = nullptr;
    ULONG async_result_ = 0;
    HRESULT async_hr_ = S_OK;
    bool async_pending_ = false;
    void CompleteAsync(HRESULT hr, ULONG bytesRead);
};
