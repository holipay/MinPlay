#include "playlist.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "../util/log.h"

bool PlaylistManager::LoadFromFile(const wchar_t* path) {
    // Read file as UTF-8
    char path_mb[2048];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path_mb, sizeof(path_mb), nullptr, nullptr);

    std::ifstream f(path_mb, std::ios::binary);
    if (!f.is_open()) {
        LOG_ERROR("Cannot open playlist file: %S", path);
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Extract base directory from file path for resolving relative URLs
    std::wstring base_dir;
    std::wstring wpath(path);
    size_t sep = wpath.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
        base_dir = wpath.substr(0, sep + 1);

    return LoadFromString(content, base_dir);
}

bool PlaylistManager::LoadFromString(const std::string& content, const std::wstring& base_dir) {
    entries_.clear();
    current_ = 0;
    return ParseM3U(content, base_dir);
}

bool PlaylistManager::ParseM3U(const std::string& content, const std::wstring& base_dir) {
    std::istringstream stream(content);
    std::string line;
    PlaylistEntry pending;

    while (std::getline(stream, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Skip empty lines
        if (line.empty())
            continue;

        // Skip comments except #EXTINF
        if (line[0] == '#') {
            if (line.compare(0, 7, "#EXTINF:") == 0) {
                // Parse #EXTINF:duration,title
                size_t colon = 7;
                size_t comma = line.find(',', colon);
                if (comma != std::string::npos) {
                    pending.duration = atof(line.substr(colon, comma - colon).c_str());
                    pending.title.assign(line.begin() + comma + 1, line.end());
                }
            }
            continue;
        }

        // This line is a URL
        // Convert to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wurl(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &wurl[0], wlen);
        wurl.pop_back(); // remove null terminator

        // Resolve relative URLs
        wurl = ResolveUrl(base_dir, wurl);

        pending.url = std::move(wurl);
        entries_.push_back(std::move(pending));
        pending = PlaylistEntry{};
    }

    if (entries_.empty()) {
        LOG_WARN("Playlist is empty");
        return false;
    }

    LOG_INFO("Playlist loaded: %d tracks", (int)entries_.size());
    return true;
}

std::wstring PlaylistManager::ResolveUrl(const std::wstring& base, const std::wstring& relative) {
    // If it's already an absolute URL (has ://), return as-is
    if (relative.find(L"://") != std::wstring::npos)
        return relative;

    // If it's an absolute local path (drive letter D:\ or UNC \\), return as-is
    if (relative.size() >= 2 && relative[1] == L':')
        return relative;
    if (relative.size() >= 2 && relative[0] == L'\\' && relative[1] == L'\\')
        return relative;

    // If base is empty, return relative as-is
    if (base.empty())
        return relative;

    return base + relative;
}

bool PlaylistManager::HasNext() const {
    return current_ < (int)entries_.size() - 1;
}

bool PlaylistManager::HasPrev() const {
    return current_ > 0;
}

void PlaylistManager::Next() {
    if (HasNext())
        current_++;
}

void PlaylistManager::Prev() {
    if (HasPrev())
        current_--;
}

void PlaylistManager::GoTo(int index) {
    if (index >= 0 && index < (int)entries_.size())
        current_ = index;
}

const PlaylistEntry& PlaylistManager::GetCurrent() const {
    static const PlaylistEntry empty;
    if (entries_.empty())
        return empty;
    return entries_[current_];
}
