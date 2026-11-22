#include "AbxApi.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

struct XMemVirtualQueryInfo {
    MEMORY_BASIC_INFORMATION basic;
    std::uint8_t console[16];
};

using XMemAllocFn = void* (*)(std::size_t, std::uint64_t);
using XMemFreeFn = void (*)(void*, std::uint64_t);
using XMemVirtualQueryFn = BOOL (*)(HANDLE, const void*, XMemVirtualQueryInfo*);
using XMemGetWorkingSetStatisticsFn = BOOL (*)(std::uint32_t, void*);

std::wstring g_libs;

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

HMODULE Load(const wchar_t* name) {
    const std::wstring path = g_libs + L"\\" + name;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

template <typename Function>
Function Get(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool Zero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

int XMem() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    const auto alloc = Get<XMemAllocFn>(module, "XMemAlloc");
    const auto free = Get<XMemFreeFn>(module, "XMemFree");
    const auto query = Get<XMemVirtualQueryFn>(module, "XMemVirtualQuery");
    const auto statistics = Get<XMemGetWorkingSetStatisticsFn>(module, "XMemGetWorkingSetStatistics");
    if (alloc == nullptr || free == nullptr || query == nullptr || statistics == nullptr) return Fail("xmem exports");

    for (const std::size_t size : {std::size_t{1}, std::size_t{100}, std::size_t{64 * 1024 - 1}, std::size_t{64 * 1024}, std::size_t{3 * 1024 * 1024 + 5}}) {
        auto* block = static_cast<std::uint8_t*>(alloc(size, 0x5e50000));
        if (block == nullptr) return Fail("XMemAlloc returned null");
        if (reinterpret_cast<std::uintptr_t>(block) % 64 != 0) return Fail("XMemAlloc alignment");
        if (!Zero(block, size)) return Fail("XMemAlloc zeroes the block");
        block[0] = 1;
        block[size - 1] = 2;

        XMemVirtualQueryInfo info;
        std::memset(&info, 0xCD, sizeof(info));
        if (!query(GetCurrentProcess(), block + size - 1, &info)) return Fail("XMemVirtualQuery");
        if (info.basic.State != MEM_COMMIT || (info.basic.Protect & PAGE_READWRITE) == 0) return Fail("XMemVirtualQuery state");
        const auto base = reinterpret_cast<std::uintptr_t>(info.basic.BaseAddress);
        const auto last = reinterpret_cast<std::uintptr_t>(block + size - 1);
        if (base > last || last >= base + info.basic.RegionSize) return Fail("XMemVirtualQuery region");
        if (!Zero(info.console, sizeof(info.console))) return Fail("XMemVirtualQuery console bytes");
        free(block, 0x5e50000);
    }

    XMemVirtualQueryInfo info;
    if (query(nullptr, &info, &info)) return Fail("XMemVirtualQuery with a null process");

    std::uint8_t buffer[32];
    std::memset(buffer, 0xAB, sizeof(buffer));
    if (statistics(0x20000001, buffer)) return Fail("XMemGetWorkingSetStatistics reports no statistics");
    for (const std::uint8_t byte : buffer) {
        if (byte != 0xAB) return Fail("XMemGetWorkingSetStatistics leaves the output alone");
    }
    std::puts("xmem ok");
    return 0;
}

template <typename Deserializer>
int CheckDeserialized(HMODULE module, const char* name, ID3DBlob* blob, D3D12_ROOT_SIGNATURE_FLAGS expected) {
    using CreateFn = HRESULT (*)(const void*, SIZE_T, REFIID, void**);
    const auto create = Get<CreateFn>(module, name);
    if (create == nullptr) return Fail(name);
    Deserializer* deserializer = nullptr;
    if (FAILED(create(blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(Deserializer), reinterpret_cast<void**>(&deserializer)))) return Fail(name);
    D3D12_ROOT_SIGNATURE_FLAGS flags{};
    if constexpr (std::is_same_v<Deserializer, ID3D12RootSignatureDeserializer>) {
        flags = deserializer->GetRootSignatureDesc()->Flags;
    } else {
        flags = deserializer->GetUnconvertedRootSignatureDesc()->Desc_1_1.Flags;
    }
    deserializer->Release();
    if (flags != expected) return Fail("deserialized flags");
    return 0;
}

int RootSignature() {
    const HMODULE module = Load(L"ab_d3d12_x.dll");
    if (module == nullptr) return Fail("load ab_d3d12_x.dll");
    const auto serialize = Get<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(module, "D3D12SerializeRootSignature");
    const auto serializeVersioned = Get<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(module, "D3D12SerializeVersionedRootSignature");
    if (serialize == nullptr || serializeVersioned == nullptr) return Fail("serializer exports");

    const auto consoleFlags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x03000000u | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3D12_ROOT_SIGNATURE_DESC legacy{};
    legacy.Flags = consoleFlags;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(serialize(&legacy, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) return Fail("D3D12SerializeRootSignature");
    if (legacy.Flags != consoleFlags) return Fail("D3D12SerializeRootSignature leaves the caller's description alone");
    if (CheckDeserialized<ID3D12RootSignatureDeserializer>(module, "D3D12CreateRootSignatureDeserializer", blob, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0) return 1;
    blob->Release();

    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1.NumParameters = 1;
    versioned.Desc_1_1.pParameters = &parameter;
    versioned.Desc_1_1.Flags = consoleFlags;
    if (FAILED(serializeVersioned(&versioned, &blob, &error))) return Fail("D3D12SerializeVersionedRootSignature");
    if (CheckDeserialized<ID3D12VersionedRootSignatureDeserializer>(module, "D3D12CreateVersionedRootSignatureDeserializer", blob, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0) return 1;
    blob->Release();
    std::puts("root signature ok");
    return 0;
}

int FreeUnknown() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    int local = 0;
    Get<XMemFreeFn>(module, "XMemFree")(&local, 0);
    return Fail("XMemFree returned");
}

int AllocZero() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    Get<XMemAllocFn>(module, "XMemAlloc")(0, 0);
    return Fail("XMemAlloc returned");
}

int UnknownVersion() {
    const HMODULE module = Load(L"ab_d3d12_x.dll");
    if (module == nullptr) return Fail("load ab_d3d12_x.dll");
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = static_cast<D3D_ROOT_SIGNATURE_VERSION>(3);
    ID3DBlob* blob = nullptr;
    Get<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(module, "D3D12SerializeVersionedRootSignature")(&desc, &blob, nullptr);
    return Fail("D3D12SerializeVersionedRootSignature returned");
}

int ExpectUnsupported(const char* scenario) {
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return Fail("GetModuleFileNameW");
    std::wstring command = L"\"" + std::wstring(self) + L"\" \"" + g_libs + L"\" ";
    for (const char* c = scenario; *c != '\0'; ++c) command += static_cast<wchar_t>(*c);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return Fail("CreateProcessW");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != static_cast<DWORD>(AbxExitUnsupported)) {
        std::printf("FAIL %s exited with 0x%lx, expected 0x%x\n", scenario, code, AbxExitUnsupported);
        return 1;
    }
    std::printf("%s stops as unsupported\n", scenario);
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: lib_probe <libs folder> <scenario>");
        return 2;
    }
    for (const char* c = argv[1]; *c != '\0'; ++c) g_libs += static_cast<wchar_t>(*c == '/' ? '\\' : *c);
    const std::string scenario = argv[2];
    if (scenario == "xmem") return XMem();
    if (scenario == "rootsig") return RootSignature();
    if (scenario == "free_unknown") return FreeUnknown();
    if (scenario == "alloc_zero") return AllocZero();
    if (scenario == "rootsig_version") return UnknownVersion();
    if (scenario == "unsupported" && argc > 3) return ExpectUnsupported(argv[3]);
    return Fail("unknown scenario");
}
