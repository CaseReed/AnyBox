#pragma once

#include <string>
#include <string_view>

namespace abx {

struct TitleConfig {
    std::string titleId;
    std::string identityName;
    std::string version;
    std::string name;
    std::string targetDeviceFamily;
};

TitleConfig ParseTitleConfig(std::string_view xml);
TitleConfig LoadTitleConfig(const std::wstring& path);

}
