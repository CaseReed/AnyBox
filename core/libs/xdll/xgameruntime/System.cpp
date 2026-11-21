#include "xdll/xgameruntime/xgameruntime.hpp"

#include <string_view>

ABX_API XSystemDeviceType XSystemGetDeviceType() {
    const char* configured = AbxTitleDeviceFamily();
    if (configured == nullptr) {
        return XSystemDeviceType::Unknown;
    }
    const std::string_view family(configured);
    if (family == "Scarlett") {
        return XSystemDeviceType::XboxScarlettAnaconda;
    }
    if (family == "XboxOne") {
        return XSystemDeviceType::XboxOneX;
    }
    if (family == "PC") {
        return XSystemDeviceType::Pc;
    }
    AbxUnsupported(__func__, "TargetDeviceFamily in MicrosoftGame.config is not XboxOne, Scarlett or PC");
}
