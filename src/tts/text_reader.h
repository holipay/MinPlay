#pragma once
#include <string>
#include <vector>

class TextReader {
public:
    static std::wstring Read(const wchar_t* path);
    static std::vector<std::wstring> SplitSentences(const std::wstring& text);
};
