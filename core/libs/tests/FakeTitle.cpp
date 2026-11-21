#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
__declspec(dllimport) HRESULT XGameRuntimeInitialize();
__declspec(dllimport) HRESULT XGameGetXboxTitleId(std::uint32_t* titleId);
__declspec(dllimport) HRESULT XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, void** queue);
__declspec(dllimport) HRESULT XTaskQueueSubmitCallback(void* queue, std::uint32_t port, void* context, void (*callback)(void*, bool));
__declspec(dllimport) HRESULT XTaskQueueSubmitDelayedCallback(void* queue, std::uint32_t port, std::uint32_t delayMs, void* context, void (*callback)(void*, bool));
__declspec(dllimport) bool XTaskQueueDispatch(void* queue, std::uint32_t port, std::uint32_t timeoutMs);
__declspec(dllimport) HRESULT XTaskQueueTerminate(void* queue, bool wait, void* context, void (*callback)(void*));
__declspec(dllimport) void XTaskQueueCloseHandle(void* queue);
__declspec(dllimport) HRESULT XUserAddAsync();
__declspec(dllimport) std::uint32_t XSystemGetDeviceType();
__declspec(dllimport) bool XPackageIsPackagedProcess();
}

namespace {

std::atomic<int> g_ran{0};
std::atomic<int> g_canceled{0};
bool g_terminated = false;

void Count(void*, bool canceled) {
    (canceled ? g_canceled : g_ran).fetch_add(1);
}

void Terminated(void*) {
    g_terminated = true;
}

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

int TaskQueue() {
    if (FAILED(XGameRuntimeInitialize())) return Fail("XGameRuntimeInitialize");
    std::uint32_t titleId = 0;
    if (FAILED(XGameGetXboxTitleId(&titleId)) || titleId != 0x1234ABCDu) return Fail("XGameGetXboxTitleId");
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("create manual");
    XTaskQueueSubmitCallback(manual, 0, nullptr, Count);
    XTaskQueueSubmitCallback(manual, 1, nullptr, Count);
    if (!XTaskQueueDispatch(manual, 0, 0) || !XTaskQueueDispatch(manual, 1, 0)) return Fail("manual dispatch");
    if (XTaskQueueDispatch(manual, 0, 0)) return Fail("empty port dispatched");
    XTaskQueueSubmitDelayedCallback(manual, 0, 20, nullptr, Count);
    if (!XTaskQueueDispatch(manual, 0, 5000)) return Fail("delayed dispatch");
    XTaskQueueSubmitCallback(manual, 0, nullptr, Count);
    XTaskQueueSubmitDelayedCallback(manual, 0, 60000, nullptr, Count);
    if (FAILED(XTaskQueueTerminate(manual, true, nullptr, Terminated)) || !g_terminated) return Fail("terminate");
    if (XTaskQueueSubmitCallback(manual, 0, nullptr, Count) != E_ABORT) return Fail("submit after terminate");
    XTaskQueueCloseHandle(manual);
    void* pool = nullptr;
    if (FAILED(XTaskQueueCreate(1, 3, &pool))) return Fail("create pool");
    for (int i = 0; i < 16; ++i) XTaskQueueSubmitCallback(pool, 0, nullptr, Count);
    XTaskQueueSubmitCallback(pool, 1, nullptr, Count);
    g_terminated = false;
    XTaskQueueTerminate(pool, true, nullptr, Terminated);
    XTaskQueueCloseHandle(pool);
    if (g_ran.load() != 20 || g_canceled.load() != 2) {
        std::printf("FAIL counts ran=%d canceled=%d\n", g_ran.load(), g_canceled.load());
        return 1;
    }
    std::printf("task queue ok\n");
    return 0;
}

}

int main(int argc, char** argv) {
    const char* scenario = argc > 1 ? argv[1] : "taskqueue";
    if (std::strcmp(scenario, "stub") == 0) {
        XUserAddAsync();
        return Fail("stub returned");
    }
    if (std::strcmp(scenario, "device") == 0) {
        std::printf("device type %u packaged %d\n", XSystemGetDeviceType(), XPackageIsPackagedProcess() ? 1 : 0);
        return 0;
    }
    if (std::strcmp(scenario, "crash") == 0) {
        XGameRuntimeInitialize();
        volatile int* null = nullptr;
        *null = 1;
        return Fail("crash returned");
    }
    return TaskQueue();
}
