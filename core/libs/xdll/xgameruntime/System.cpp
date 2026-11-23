#include "xdll/xgameruntime/xgameruntime.hpp"

#include <bcrypt.h>

#include <cstring>
#include <string>
#include <string_view>

namespace {

XSystemDeviceType DeviceType(const char* function) {
    const char* configured = AbxTitleDeviceFamily();
    if (configured == nullptr) {
        AbxUnsupported(function, "MicrosoftGame.config beside the executable is missing or has no <Executable>");
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
    AbxUnsupported(function, "TargetDeviceFamily in MicrosoftGame.config is not XboxOne, Scarlett or PC");
}

constexpr std::size_t DigestBytes = 32;

bool Sha256(const std::string& data, unsigned char (&digest)[DigestBytes]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0))
             && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, DigestBytes, 0));
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

void DeriveAppSpecificDeviceId(const char* titleId, char (&id)[XSystemAppSpecificDeviceIdBytes], const char* function) {
    char machineGuid[64];
    DWORD size = sizeof(machineGuid);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ, nullptr, machineGuid, &size) != ERROR_SUCCESS) {
        AbxUnsupported(function, "the MachineGuid of the host could not be read, so there is no machine-stable value to derive the ID from");
    }
    const std::string input = std::string("AnyBox XSystemGetAppSpecificDeviceId") + '\0' + machineGuid + '\0' + titleId;
    unsigned char digest[DigestBytes];
    if (!Sha256(input, digest)) {
        AbxUnsupported(function, "the SHA-256 of the machine GUID and title ID could not be computed");
    }
    static constexpr char Digits[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < (XSystemAppSpecificDeviceIdBytes - 1) / 2; ++i) {
        id[2 * i] = Digits[digest[i] >> 4];
        id[2 * i + 1] = Digits[digest[i] & 0x0f];
    }
    id[XSystemAppSpecificDeviceIdBytes - 1] = '\0';
}

XVersion HostWindowsVersion(const char* function) {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    rtlGetVersion(&version);
    DWORD revision = 0;
    DWORD size = sizeof(revision);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &revision, &size) != ERROR_SUCCESS) {
        AbxUnsupported(function, "the update build revision (UBR) of the host Windows could not be read");
    }
    return {static_cast<std::uint16_t>(version.dwMajorVersion), static_cast<std::uint16_t>(version.dwMinorVersion), static_cast<std::uint16_t>(version.dwBuildNumber),
            static_cast<std::uint16_t>(revision)};
}

}

ABX_API XSystemDeviceType XSystemGetDeviceType() {
    return DeviceType(__func__);
}

ABX_API HRESULT XSystemGetConsoleId(std::size_t consoleIdSize, char* consoleId, std::size_t* consoleIdUsed) {
    static constexpr char PcConsoleId[] = "00000000.00000000.00000000.00000000.00";
    static_assert(sizeof(PcConsoleId) == XSystemConsoleIdBytes);
    if (consoleIdSize < XSystemConsoleIdBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (consoleId == nullptr) {
        AbxUnsupported(__func__, "consoleId is null");
    }
    std::memcpy(consoleId, PcConsoleId, sizeof(PcConsoleId));
    if (consoleIdUsed != nullptr) {
        *consoleIdUsed = sizeof(PcConsoleId);
    }
    return S_OK;
}

ABX_API HRESULT XSystemGetXboxLiveSandboxId(std::size_t sandboxIdSize, char* sandboxId, std::size_t* sandboxIdUsed) {
    static constexpr char RetailSandboxId[] = "RETAIL";
    if (sandboxIdSize < XSystemXboxLiveSandboxIdMaxBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (sandboxId == nullptr) {
        AbxUnsupported(__func__, "sandboxId is null");
    }
    std::memcpy(sandboxId, RetailSandboxId, sizeof(RetailSandboxId));
    if (sandboxIdUsed != nullptr) {
        *sandboxIdUsed = sizeof(RetailSandboxId);
    }
    return S_OK;
}

ABX_API HRESULT XSystemGetAppSpecificDeviceId(std::size_t appSpecificDeviceIdSize, char* appSpecificDeviceId, std::size_t* appSpecificDeviceIdUsed) {
    if (appSpecificDeviceIdSize < XSystemAppSpecificDeviceIdBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (appSpecificDeviceId == nullptr) {
        AbxUnsupported(__func__, "appSpecificDeviceId is null");
    }
    const char* titleId = AbxTitleId();
    if (titleId == nullptr) {
        AbxUnsupported(__func__, "MicrosoftGame.config beside the executable has no valid <TitleId>");
    }
    char id[XSystemAppSpecificDeviceIdBytes];
    DeriveAppSpecificDeviceId(titleId, id, __func__);
    std::memcpy(appSpecificDeviceId, id, sizeof(id));
    if (appSpecificDeviceIdUsed != nullptr) {
        *appSpecificDeviceIdUsed = sizeof(id);
    }
    return S_OK;
}

ABX_API XSystemAnalyticsInfo XSystemGetAnalyticsInfo() {
    if (DeviceType(__func__) != XSystemDeviceType::Pc) {
        AbxUnsupported(__func__, "the title runs as an Xbox console title, and the title OS version and device form a console reports are not documented");
    }
    static constexpr char Family[] = "Windows";
    XSystemAnalyticsInfo info{};
    info.osVersion = HostWindowsVersion(__func__);
    info.hostingOsVersion = info.osVersion;
    std::memcpy(info.family, Family, sizeof(Family));
    return info;
}

ABX_API XSystemRuntimeInfo XSystemGetRuntimeInfo() {
    if (DeviceType(__func__) != XSystemDeviceType::Pc) {
        AbxUnsupported(__func__, "the title runs as an Xbox console title, and a console always reports the version of its gaming runtime, which AnyBox does not have");
    }
    return {};
}

ABX_API void XSystemAllowFullDownloadBandwidth(bool) {
}

ABX_API HRESULT XSystemHandleTrack(XSystemHandleCallback* callback, void*) {
    if (callback != nullptr) {
        AbxUnsupported(__func__, "AnyBox does not report handle creation and destruction to a tracking callback");
    }
    return S_OK;
}

ABX_API bool XSystemIsHandleValid(XSystemHandle handle) {
    if (handle != nullptr) {
        AbxUnsupported(__func__, "AnyBox keeps no registry of live runtime handles, so a non-null handle cannot be checked");
    }
    return false;
}
