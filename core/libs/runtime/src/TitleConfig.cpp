#include "TitleConfig.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

namespace abx {

namespace {

bool IsNameEnd(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/';
}

std::optional<std::string_view> StartTag(std::string_view xml, std::string_view tag) {
    std::size_t at = 0;
    while ((at = xml.find('<', at)) != std::string_view::npos) {
        const std::size_t nameEnd = at + 1 + tag.size();
        if (xml.compare(at + 1, tag.size(), tag) == 0 && nameEnd < xml.size() && IsNameEnd(xml[nameEnd])) {
            const std::size_t close = xml.find('>', nameEnd);
            if (close == std::string_view::npos) {
                return std::nullopt;
            }
            return xml.substr(nameEnd, close - nameEnd);
        }
        ++at;
    }
    return std::nullopt;
}

std::string Attribute(std::string_view tag, std::string_view name) {
    std::size_t at = 0;
    while ((at = tag.find(name, at)) != std::string_view::npos) {
        const bool startsWord = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1]));
        std::size_t cursor = at + name.size();
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) {
            ++cursor;
        }
        if (startsWord && cursor < tag.size() && tag[cursor] == '=') {
            ++cursor;
            while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) {
                ++cursor;
            }
            if (cursor < tag.size() && (tag[cursor] == '"' || tag[cursor] == '\'')) {
                const char quote = tag[cursor];
                const std::size_t end = tag.find(quote, cursor + 1);
                if (end != std::string_view::npos) {
                    return std::string(tag.substr(cursor + 1, end - cursor - 1));
                }
            }
            return {};
        }
        at += name.size();
    }
    return {};
}

std::string ElementText(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const std::size_t begin = xml.find(open);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t textBegin = begin + open.size();
    const std::size_t end = xml.find(close, textBegin);
    if (end == std::string_view::npos) {
        return {};
    }
    std::string_view text = xml.substr(textBegin, end - textBegin);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

bool IsTitleId(const std::string& text) {
    return text.size() == 8 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool IsVersion(const std::string& text) {
    int parts = 1;
    bool digit = false;
    for (const char c : text) {
        if (c == '.') {
            if (!digit) {
                return false;
            }
            ++parts;
            digit = false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
        } else {
            return false;
        }
    }
    return digit && parts == 4;
}

std::string FromUtf16(const std::vector<char>& bytes) {
    const int units = static_cast<int>((bytes.size() - 2) / 2);
    const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, units, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, units, text.data(), length, nullptr, nullptr);
    return text;
}

}

TitleConfig ParseTitleConfig(std::string_view xml) {
    TitleConfig config;
    std::string titleId = ElementText(xml, "TitleId");
    std::transform(titleId.begin(), titleId.end(), titleId.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (IsTitleId(titleId)) {
        config.titleId = titleId;
    }
    if (const auto identity = StartTag(xml, "Identity")) {
        config.identityName = Attribute(*identity, "Name");
        const std::string version = Attribute(*identity, "Version");
        if (IsVersion(version)) {
            config.version = version;
        }
    }
    if (const auto visuals = StartTag(xml, "ShellVisuals")) {
        config.name = Attribute(*visuals, "DefaultDisplayName");
    }
    if (config.name.starts_with("ms-resource:")) {
        config.name.clear();
    }
    if (const auto executable = StartTag(xml, "Executable")) {
        config.targetDeviceFamily = Attribute(*executable, "TargetDeviceFamily");
    }
    if (config.targetDeviceFamily.empty()) {
        config.targetDeviceFamily = "XboxOne";
    }
    return config;
}

TitleConfig LoadTitleConfig(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER size{};
    std::vector<char> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22)) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
            bytes.clear();
        }
    }
    CloseHandle(file);
    if (bytes.empty()) {
        return {};
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) {
        return ParseTitleConfig(FromUtf16(bytes));
    }
    std::string_view text(bytes.data(), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    return ParseTitleConfig(text);
}

}
