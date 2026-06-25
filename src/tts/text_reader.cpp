#include "text_reader.h"
#include <windows.h>
#include <fstream>
#include <sstream>

std::wstring TextReader::Read(const wchar_t* path) {
    // Read raw bytes
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > 50 * 1024 * 1024) {
        CloseHandle(hFile);
        return L"";
    }

    DWORD size = (DWORD)fileSize.QuadPart;
    if (size == 0) { CloseHandle(hFile); return L""; }

    std::vector<char> buf(size);
    DWORD read = 0;
    BOOL ok = ReadFile(hFile, buf.data(), size, &read, nullptr);
    CloseHandle(hFile);
    if (!ok || read != size) return L"";

    // Detect BOM and convert to wstring
    const unsigned char* raw = (const unsigned char*)buf.data();
    std::wstring result;

    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        // UTF-8 with BOM
        int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data() + 3, size - 3, nullptr, 0);
        result.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, buf.data() + 3, size - 3, &result[0], wlen);
    } else if (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        // UTF-16 LE with BOM
        result.assign((const wchar_t*)(raw + 2), (size - 2) / sizeof(wchar_t));
    } else if (size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        // UTF-16 BE with BOM — swap bytes
        int wlen = (size - 2) / 2;
        result.resize(wlen);
        for (int i = 0; i < wlen; i++)
            result[i] = (wchar_t)((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
    } else {
        // No BOM — try UTF-8, fallback to system codepage
        int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), size, nullptr, 0);
        if (wlen > 0) {
            std::wstring test(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, buf.data(), size, &test[0], wlen);
            // Check for replacement characters (invalid UTF-8)
            bool valid = true;
            for (wchar_t c : test) {
                if (c == L'\uFFFD') { valid = false; break; }
            }
            if (valid) {
                result = std::move(test);
            }
        }
        if (result.empty()) {
            // Fallback: system codepage (GBK on Chinese Windows)
            wlen = MultiByteToWideChar(CP_ACP, 0, buf.data(), size, nullptr, 0);
            result.resize(wlen);
            MultiByteToWideChar(CP_ACP, 0, buf.data(), size, &result[0], wlen);
        }
    }

    return result;
}

std::vector<std::wstring> TextReader::SplitSentences(const std::wstring& text) {
    std::vector<std::wstring> sentences;
    std::wstring current;
    const wchar_t* p = text.c_str();
    const wchar_t* end = p + text.size();

    while (p < end) {
        wchar_t c = *p++;
        current += c;

        // Sentence-ending punctuation (Chinese + English)
        bool is_sentence_end = (c == L'。' || c == L'！' || c == L'？' ||
                                c == L'.' || c == L'!' || c == L'?');

        // Paragraph break
        bool is_paragraph_break = (c == L'\n');

        if (is_sentence_end || is_paragraph_break) {
            // Trim leading/trailing whitespace
            size_t start = current.find_first_not_of(L" \t\r\n");
            size_t last = current.find_last_not_of(L" \t\r\n");
            if (start != std::wstring::npos) {
                sentences.push_back(current.substr(start, last - start + 1));
            }
            current.clear();
        }
    }

    // Handle remaining text without trailing punctuation
    if (!current.empty()) {
        size_t start = current.find_first_not_of(L" \t\r\n");
        size_t last = current.find_last_not_of(L" \t\r\n");
        if (start != std::wstring::npos) {
            sentences.push_back(current.substr(start, last - start + 1));
        }
    }

    return sentences;
}
