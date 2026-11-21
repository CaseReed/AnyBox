#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

struct XTaskQueueObject;
using XTaskQueueHandle = XTaskQueueObject*;

enum class XTaskQueueDispatchMode : std::uint32_t {
    Manual,
    ThreadPool,
    SerializedThreadPool,
    Immediate,
};

enum class XTaskQueuePort : std::uint32_t {
    Work,
    Completion,
};

using XTaskQueueCallback = void(void* context, bool canceled);
using XTaskQueueTerminatedCallback = void(void* context);

inline constexpr std::uint32_t XTaskQueueNoWait = 0;
inline constexpr std::uint32_t XTaskQueueInfinite = 0xffffffffu;

struct XAsyncBlock;

using XAsyncCompletionRoutine = void(XAsyncBlock* asyncBlock);
using XAsyncWork = HRESULT(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine* callback;
    unsigned char internal[sizeof(void*) * 4];
};

enum class XAsyncOp : std::uint32_t {
    Begin,
    DoWork,
    GetResult,
    Cancel,
    Cleanup,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    std::size_t bufferSize;
    void* buffer;
    void* context;
};

using XAsyncProvider = HRESULT(XAsyncOp op, const XAsyncProviderData* data);

enum class XSystemDeviceType : std::uint32_t {
    Unknown = 0x00,
    Pc = 0x01,
    XboxOne = 0x02,
    XboxOneS = 0x03,
    XboxOneX = 0x04,
    XboxOneXDevkit = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit = 0x08,
};
