#include <windows.h>

#include <cstdint>

extern "C" {
__declspec(dllexport) HRESULT XGameRuntimeInitialize() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameGetXboxTitleId(std::uint32_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueCreate(std::uint32_t, std::uint32_t, void**) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueSubmitCallback(void*, std::uint32_t, void*, void*) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueSubmitDelayedCallback(void*, std::uint32_t, std::uint32_t, void*, void*) { return E_FAIL; }
__declspec(dllexport) bool XTaskQueueDispatch(void*, std::uint32_t, std::uint32_t) { return false; }
__declspec(dllexport) HRESULT XTaskQueueTerminate(void*, bool, void*, void*) { return E_FAIL; }
__declspec(dllexport) void XTaskQueueCloseHandle(void*) {}
__declspec(dllexport) HRESULT XUserAddAsync() { return E_FAIL; }
__declspec(dllexport) std::uint32_t XSystemGetDeviceType() { return 0xffffffffu; }
__declspec(dllexport) bool XPackageIsPackagedProcess() { return true; }
}
