#include "xdll/d3d12_x/d3d12_x.hpp"

#include <cstdint>

namespace {

constexpr std::uint32_t ConsoleFlagBits = 0xFF000000u;

D3D12_ROOT_SIGNATURE_FLAGS DesktopFlags(D3D12_ROOT_SIGNATURE_FLAGS flags) {
    return static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(static_cast<std::uint32_t>(flags) & ~ConsoleFlagBits);
}

HMODULE HostD3D12() {
    static const HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

template <typename Function>
Function HostFunction(const char* function) {
    const HMODULE module = HostD3D12();
    if (module == nullptr) {
        AbxUnsupported(function, "the host has no d3d12.dll");
    }
    const FARPROC address = GetProcAddress(module, function);
    if (address == nullptr) {
        AbxUnsupported(function, "the host d3d12.dll does not export this function");
    }
    return reinterpret_cast<Function>(reinterpret_cast<void*>(address));
}

}

ABX_API HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** errorBlob) {
    const auto serialize = HostFunction<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(__func__);
    if (rootSignature == nullptr) {
        return serialize(rootSignature, version, blob, errorBlob);
    }
    D3D12_ROOT_SIGNATURE_DESC desktop = *rootSignature;
    desktop.Flags = DesktopFlags(desktop.Flags);
    return serialize(&desktop, version, blob, errorBlob);
}

ABX_API HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature, ID3DBlob** blob, ID3DBlob** errorBlob) {
    const auto serialize = HostFunction<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(__func__);
    if (rootSignature == nullptr) {
        return serialize(rootSignature, blob, errorBlob);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desktop = *rootSignature;
    if (desktop.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        desktop.Desc_1_0.Flags = DesktopFlags(desktop.Desc_1_0.Flags);
    } else if (desktop.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        desktop.Desc_1_1.Flags = DesktopFlags(desktop.Desc_1_1.Flags);
    } else {
        AbxUnsupported(__func__, "root signature version is neither 1.0 nor 1.1");
    }
    return serialize(&desktop, blob, errorBlob);
}

ABX_API HRESULT D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer) {
    return HostFunction<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>(__func__)(data, size, iid, deserializer);
}

ABX_API HRESULT D3D12CreateVersionedRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer) {
    return HostFunction<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(__func__)(data, size, iid, deserializer);
}
