#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct PlaylistEntry {
    std::wstring url;
    std::wstring title;
    double duration = 0;
};

class PlaylistManager {
public:
    bool LoadFromFile(const wchar_t* path);
    bool LoadFromString(const std::string& content, const std::wstring& base_dir);

    bool HasNext() const;
    bool HasPrev() const;
    void Next();
    void Prev();
    void GoTo(int index);

    const PlaylistEntry& GetCurrent() const;
    int GetIndex() const { return current_; }
    int GetCount() const { return (int)entries_.size(); }

private:
    bool ParseM3U(const std::string& content, const std::wstring& base_dir);
    std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative);
    std::vector<PlaylistEntry> entries_;
    int current_ = 0;
};
