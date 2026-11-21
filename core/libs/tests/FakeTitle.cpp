#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

struct AsyncBlock {
    void* queue;
    void* context;
    void (*callback)(AsyncBlock* async);
    unsigned char internal[sizeof(void*) * 4];
};

struct AsyncProviderData {
    AsyncBlock* async;
    std::size_t bufferSize;
    void* buffer;
    void* context;
};

using AsyncProvider = HRESULT (*)(std::uint32_t op, const AsyncProviderData* data);
using AsyncWork = HRESULT (*)(AsyncBlock* async);

extern "C" {
__declspec(dllimport) HRESULT XAsyncBegin(AsyncBlock* async, void* context, const void* identity, const char* identityName, AsyncProvider provider);
__declspec(dllimport) HRESULT XAsyncSchedule(AsyncBlock* async, std::uint32_t delayMs);
__declspec(dllimport) void XAsyncComplete(AsyncBlock* async, HRESULT result, std::size_t requiredBufferSize);
__declspec(dllimport) void XAsyncCancel(AsyncBlock* async);
__declspec(dllimport) HRESULT XAsyncRun(AsyncBlock* async, AsyncWork work);
__declspec(dllimport) HRESULT XAsyncGetStatus(AsyncBlock* async, bool wait);
__declspec(dllimport) HRESULT XAsyncGetResultSize(AsyncBlock* async, std::size_t* bufferSize);
__declspec(dllimport) HRESULT XAsyncGetResult(AsyncBlock* async, const void* identity, std::size_t bufferSize, void* buffer, std::size_t* bufferUsed);
__declspec(dllimport) HRESULT XGameRuntimeInitialize();
__declspec(dllimport) HRESULT XGameGetXboxTitleId(std::uint32_t* titleId);
__declspec(dllimport) HRESULT XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, void** queue);
__declspec(dllimport) HRESULT XTaskQueueSubmitCallback(void* queue, std::uint32_t port, void* context, void (*callback)(void*, bool));
__declspec(dllimport) HRESULT XTaskQueueSubmitDelayedCallback(void* queue, std::uint32_t port, std::uint32_t delayMs, void* context, void (*callback)(void*, bool));
__declspec(dllimport) bool XTaskQueueDispatch(void* queue, std::uint32_t port, std::uint32_t timeoutMs);
__declspec(dllimport) HRESULT XTaskQueueTerminate(void* queue, bool wait, void* context, void (*callback)(void*));
__declspec(dllimport) void XTaskQueueCloseHandle(void* queue);
__declspec(dllimport) bool XTaskQueueGetCurrentProcessTaskQueue(void** queue);
__declspec(dllimport) void XTaskQueueSetCurrentProcessTaskQueue(void* queue);
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

enum : std::uint32_t { OpBegin, OpDoWork, OpGetResult, OpCancel, OpCleanup };

constexpr HRESULT RunResult = static_cast<HRESULT>(0x12345678);
constexpr HRESULT WorkFailure = static_cast<HRESULT>(0x8009ABCDu);
constexpr HRESULT IllegalMethodCall = static_cast<HRESULT>(0x8000000Eu);
constexpr HRESULT NotSufficientBuffer = static_cast<HRESULT>(0x8007007Au);
constexpr HRESULT NoTaskQueue = static_cast<HRESULT>(0x800701ABu);
constexpr std::uint32_t Payload = 0xC0FFEE42u;
constexpr std::uint32_t ScheduleDelayMs = 200;
constexpr DWORD DispatchTimeoutMs = 10000;
constexpr DWORD WatchdogMs = 20000;
constexpr int SwarmSize = 1000;
constexpr int Repeats = 500;

const char g_identity = 0;
const char g_otherIdentity = 0;
std::atomic<void*> g_workContext{nullptr};

struct Recorder {
    std::uint32_t ops[16] = {};
    int count = 0;
    HRESULT workResult = S_OK;
    std::size_t resultBufferSize = 0;
    std::uint32_t delay = 0;
    int steps = 0;

    void Record(std::uint32_t op) {
        if (count < 16) ops[count] = op;
        ++count;
    }

    bool Is(std::initializer_list<std::uint32_t> expected) const {
        if (count != static_cast<int>(expected.size())) return false;
        int index = 0;
        for (std::uint32_t op : expected) {
            if (ops[index++] != op) return false;
        }
        return true;
    }
};

struct Completion {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT status = E_FAIL;
    DWORD thread = 0;
    std::atomic<bool> returned{false};
    ~Completion() { CloseHandle(event); }
};

struct Counter {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> count{0};
    std::atomic<int> failures{0};
    int target = 0;
    ~Counter() { CloseHandle(event); }
};

struct Completer {
    HANDLE request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AsyncBlock* block = nullptr;
    std::atomic<bool> stop{false};
    ~Completer() { CloseHandle(request); CloseHandle(done); }
};

struct ResultInCallback {
    Recorder recorder;
    HRESULT waited = E_FAIL;
    HRESULT result = E_FAIL;
    std::uint32_t value = 0;
    std::size_t used = 0;
    int opsInCallback = 0;
};

HRESULT ReturnWork(AsyncBlock* async) {
    g_workContext = async->context;
    return RunResult;
}

HRESULT PayloadProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    switch (op) {
    case OpBegin:
        return XAsyncSchedule(data->async, 0);
    case OpDoWork:
        XAsyncComplete(data->async, S_OK, sizeof(Payload));
        return S_OK;
    case OpGetResult:
        recorder->resultBufferSize = data->bufferSize;
        std::memcpy(data->buffer, &Payload, sizeof(Payload));
        return S_OK;
    default:
        return S_OK;
    }
}

HRESULT CancelProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, 0);
    if (op == OpCancel) XAsyncComplete(data->async, E_ABORT, 0);
    return S_OK;
}

HRESULT CompleteInBeginProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    if (op == OpBegin) XAsyncComplete(data->async, S_OK, 0);
    return S_OK;
}

HRESULT WorkResultProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, 0);
    if (op == OpDoWork) return recorder->workResult;
    return S_OK;
}

HRESULT StepProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, recorder->delay);
    if (op == OpDoWork) {
        if (recorder->steps > 0) {
            --recorder->steps;
            const HRESULT scheduled = XAsyncSchedule(data->async, 0);
            return SUCCEEDED(scheduled) ? E_PENDING : scheduled;
        }
        XAsyncComplete(data->async, S_OK, 0);
    }
    return S_OK;
}

HRESULT FailBeginProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    return op == OpBegin ? E_FAIL : S_OK;
}

HRESULT CompleterProvider(std::uint32_t op, const AsyncProviderData* data) {
    if (op == OpCancel) SetEvent(static_cast<Completer*>(data->context)->request);
    return S_OK;
}

HRESULT ScheduleProvider(std::uint32_t op, const AsyncProviderData* data) {
    return op == OpBegin ? XAsyncSchedule(data->async, 0) : S_OK;
}

void RecordCompletion(AsyncBlock* async) {
    auto* completion = static_cast<Completion*>(async->context);
    completion->status = XAsyncGetStatus(async, false);
    completion->thread = GetCurrentThreadId();
    SetEvent(completion->event);
    completion->returned = true;
}

void ResultCompletion(AsyncBlock* async) {
    auto* state = static_cast<ResultInCallback*>(async->context);
    state->waited = XAsyncGetStatus(async, true);
    state->result = XAsyncGetResult(async, &g_identity, sizeof(state->value), &state->value, &state->used);
    state->opsInCallback = state->recorder.count;
}

void ChainCompletion(AsyncBlock* async) {
    auto* counter = static_cast<Counter*>(async->context);
    if (XAsyncGetStatus(async, false) != RunResult) ++counter->failures;
    if (++counter->count < counter->target) {
        if (XAsyncRun(async, ReturnWork) != S_OK) ++counter->failures;
        return;
    }
    SetEvent(counter->event);
}

void SwarmCompletion(AsyncBlock* async) {
    auto* counter = static_cast<Counter*>(async->context);
    if (XAsyncGetStatus(async, false) != RunResult) ++counter->failures;
    if (++counter->count == counter->target) SetEvent(counter->event);
}

bool Zeroed(const AsyncBlock& block) {
    for (const unsigned char byte : block.internal) {
        if (byte != 0) return false;
    }
    return true;
}

int AsyncRun(void* pool) {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("async manual queue");
    int marker = 0;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &marker;
    if (XAsyncRun(&block, ReturnWork) != S_OK) return Fail("XAsyncRun");
    if (XAsyncGetStatus(&block, false) != E_PENDING) return Fail("run status before dispatch");
    if (!XTaskQueueDispatch(manual, 0, 0)) return Fail("run work dispatch");
    if (g_workContext.load() != &marker) return Fail("run work context");
    if (XAsyncGetStatus(&block, false) != RunResult || XAsyncGetStatus(&block, true) != RunResult) return Fail("run status after dispatch");
    std::size_t size = 1;
    std::size_t used = 1;
    if (XAsyncGetResultSize(&block, &size) != RunResult || size != 0) return Fail("run result size");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, &used) != RunResult || used != 0) return Fail("run result");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, &used) != RunResult) return Fail("run result twice");
    if (XTaskQueueDispatch(manual, 1, 0)) return Fail("run without callback queued a completion");

    Completion waited;
    AsyncBlock waitBlock{};
    waitBlock.queue = manual;
    waitBlock.context = &waited;
    waitBlock.callback = RecordCompletion;
    if (XAsyncRun(&waitBlock, ReturnWork) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("XAsyncRun to wait on");
    if (XAsyncGetStatus(&waitBlock, false) != RunResult || waited.returned) return Fail("status before the completion callback");
    std::atomic<bool> dispatched{false};
    std::thread dispatcher([&] { dispatched = XTaskQueueDispatch(manual, 1, DispatchTimeoutMs); });
    const HRESULT waitedStatus = XAsyncGetStatus(&waitBlock, true);
    const bool returned = waited.returned.load();
    dispatcher.join();
    if (!dispatched) return Fail("completion dispatch timed out");
    if (waitedStatus != RunResult || !returned || waited.status != RunResult) return Fail("waiting status returned before the completion callback");
    XTaskQueueCloseHandle(manual);

    Completion pooled;
    AsyncBlock poolBlock{};
    poolBlock.queue = pool;
    poolBlock.context = &pooled;
    poolBlock.callback = RecordCompletion;
    if (XAsyncRun(&poolBlock, ReturnWork) != S_OK) return Fail("XAsyncRun on thread pool");
    if (WaitForSingleObject(pooled.event, DispatchTimeoutMs) != WAIT_OBJECT_0) return Fail("thread pool completion callback");
    if (pooled.status != RunResult || XAsyncGetStatus(&poolBlock, true) != RunResult || !pooled.returned) return Fail("thread pool status");
    if (g_workContext.load() != &pooled) return Fail("thread pool work context");

    void* immediate = nullptr;
    if (FAILED(XTaskQueueCreate(3, 3, &immediate))) return Fail("async immediate queue");
    Completion immediateRun;
    AsyncBlock immediateBlock{};
    immediateBlock.queue = immediate;
    immediateBlock.context = &immediateRun;
    immediateBlock.callback = RecordCompletion;
    if (XAsyncRun(&immediateBlock, ReturnWork) != S_OK || !immediateRun.returned || immediateRun.status != RunResult) return Fail("immediate queue run");
    if (XAsyncGetStatus(&immediateBlock, false) != RunResult) return Fail("immediate queue status");
    XTaskQueueCloseHandle(immediate);

    AsyncBlock processBlock{};
    if (XAsyncRun(&processBlock, ReturnWork) != S_OK || XAsyncGetStatus(&processBlock, true) != RunResult) return Fail("run on the process task queue");

    void* process = nullptr;
    if (!XTaskQueueGetCurrentProcessTaskQueue(&process)) return Fail("process task queue");
    XTaskQueueSetCurrentProcessTaskQueue(nullptr);
    AsyncBlock orphan{};
    const HRESULT orphaned = XAsyncRun(&orphan, ReturnWork);
    XTaskQueueSetCurrentProcessTaskQueue(process);
    XTaskQueueCloseHandle(process);
    if (orphaned != NoTaskQueue || XAsyncGetStatus(&orphan, false) != NoTaskQueue) return Fail("run with no process task queue");
    return 0;
}

int AsyncZeroBlock() {
    AsyncBlock zero{};
    std::size_t size = 1;
    std::size_t used = 1;
    XAsyncCancel(&zero);
    XAsyncComplete(&zero, S_OK, 0);
    if (XAsyncGetStatus(&zero, false) != S_OK || XAsyncGetStatus(&zero, true) != S_OK) return Fail("zeroed block status");
    if (XAsyncGetResultSize(&zero, &size) != S_OK || size != 0) return Fail("zeroed block result size");
    if (XAsyncSchedule(&zero, 0) != E_INVALIDARG) return Fail("zeroed block schedule");
    if (XAsyncGetResult(&zero, nullptr, 0, nullptr, &used) != S_OK || used != 0) return Fail("zeroed block result");
    if (!Zeroed(zero)) return Fail("zeroed block written");
    return 0;
}

int AsyncProviderCalls() {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("provider queue");
    Recorder recorder;
    AsyncBlock block{};
    block.queue = manual;
    if (XAsyncBegin(&block, &recorder, &g_identity, "Payload", PayloadProvider) != S_OK || !recorder.Is({OpBegin})) return Fail("XAsyncBegin");
    std::size_t size = 1;
    std::size_t used = 0;
    unsigned char buffer[16] = {};
    if (XAsyncGetStatus(&block, false) != E_PENDING) return Fail("provider pending status");
    if (XAsyncGetResultSize(&block, &size) != E_PENDING || size != 0) return Fail("provider pending result size");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != E_PENDING) return Fail("provider pending result");
    if (XAsyncSchedule(&block, 0) != E_UNEXPECTED) return Fail("scheduled twice");
    if (XAsyncBegin(&block, &recorder, &g_identity, "Payload", PayloadProvider) != E_INVALIDARG || !recorder.Is({OpBegin})) return Fail("begin on a block in use");
    if (!XTaskQueueDispatch(manual, 0, 0) || !recorder.Is({OpBegin, OpDoWork})) return Fail("provider do work");
    if (XAsyncGetStatus(&block, false) != S_OK) return Fail("provider completed status");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpDoWork}) || XAsyncGetStatus(&block, false) != S_OK) return Fail("cancel after completion reached the provider");
    if (XAsyncGetResultSize(&block, &size) != S_OK || size != sizeof(Payload)) return Fail("provider result size");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != S_OK || used != sizeof(Payload)) return Fail("provider result");
    if (std::memcmp(buffer, &Payload, sizeof(Payload)) != 0 || recorder.resultBufferSize != sizeof(buffer)) return Fail("provider payload");
    if (!recorder.Is({OpBegin, OpDoWork, OpGetResult, OpCleanup})) return Fail("provider op sequence");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != IllegalMethodCall) return Fail("result retrieved twice");
    if (XAsyncGetStatus(&block, false) != S_OK || XAsyncGetResultSize(&block, &size) != S_OK || size != 0) return Fail("status after result");
    if (XTaskQueueDispatch(manual, 1, 0)) return Fail("provider without callback queued a completion");

    ResultInCallback inCallback;
    AsyncBlock callbackBlock{};
    callbackBlock.queue = manual;
    callbackBlock.context = &inCallback;
    callbackBlock.callback = ResultCompletion;
    if (XAsyncBegin(&callbackBlock, &inCallback.recorder, &g_identity, "Payload", PayloadProvider) != S_OK) return Fail("payload call with callback");
    if (!XTaskQueueDispatch(manual, 0, 0) || !XTaskQueueDispatch(manual, 1, 0)) return Fail("payload call with callback dispatch");
    if (inCallback.waited != S_OK || inCallback.result != S_OK || inCallback.used != sizeof(Payload) || inCallback.value != Payload) return Fail("result read in the completion callback");
    if (inCallback.opsInCallback != 3 || !inCallback.recorder.Is({OpBegin, OpDoWork, OpGetResult, OpCleanup})) return Fail("cleanup order with a completion callback");

    Recorder mismatched;
    unsigned char untouched[16] = {};
    if (XAsyncBegin(&block, &mismatched, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("block reuse");
    if (XAsyncGetResult(&block, &g_otherIdentity, sizeof(untouched), untouched, &used) != E_INVALIDARG) return Fail("identity mismatch");
    if (!mismatched.Is({OpBegin, OpDoWork, OpCleanup}) || untouched[0] != 0) return Fail("identity mismatch reached the provider");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != IllegalMethodCall) return Fail("identity mismatch kept the block");

    Recorder nullBuffer;
    if (XAsyncBegin(&block, &nullBuffer, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("null buffer call");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), nullptr, &used) != E_INVALIDARG || !nullBuffer.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("null result buffer");

    Recorder shortBuffer;
    if (XAsyncBegin(&block, &shortBuffer, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("small buffer call");
    if (XAsyncGetResult(&block, &g_identity, 2, buffer, &used) != NotSufficientBuffer || !shortBuffer.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("small result buffer");

    Recorder failedBegin;
    if (XAsyncBegin(&block, &failedBegin, nullptr, nullptr, FailBeginProvider) != S_OK) return Fail("begin with a failing provider");
    if (XAsyncGetStatus(&block, true) != E_FAIL || !failedBegin.Is({OpBegin, OpCleanup})) return Fail("failed begin status");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, nullptr) != E_FAIL) return Fail("failed begin result");

    Recorder failedWork;
    failedWork.workResult = WorkFailure;
    if (XAsyncBegin(&block, &failedWork, nullptr, nullptr, WorkResultProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("failing work call");
    if (XAsyncGetStatus(&block, false) != WorkFailure || !failedWork.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("do work failure status");

    Recorder silentWork;
    if (XAsyncBegin(&block, &silentWork, nullptr, nullptr, WorkResultProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("silent work call");
    if (XAsyncGetStatus(&block, false) != E_UNEXPECTED) return Fail("do work success without completion");

    Recorder stepped;
    stepped.steps = 2;
    if (XAsyncBegin(&block, &stepped, nullptr, nullptr, StepProvider) != S_OK) return Fail("rescheduling call");
    for (int step = 0; step < 2; ++step) {
        if (!XTaskQueueDispatch(manual, 0, 0) || XAsyncGetStatus(&block, false) != E_PENDING) return Fail("do work rescheduled with E_PENDING");
    }
    if (!XTaskQueueDispatch(manual, 0, 0) || XAsyncGetStatus(&block, false) != S_OK) return Fail("rescheduled work completion");
    if (!stepped.Is({OpBegin, OpDoWork, OpDoWork, OpDoWork, OpCleanup}) || XTaskQueueDispatch(manual, 0, 0)) return Fail("rescheduled op sequence");

    Recorder delayed;
    delayed.delay = ScheduleDelayMs;
    const ULONGLONG scheduledAt = GetTickCount64();
    if (XAsyncBegin(&block, &delayed, nullptr, nullptr, StepProvider) != S_OK) return Fail("delayed call");
    if (XTaskQueueDispatch(manual, 0, 0)) return Fail("delayed work ran before its delay");
    if (!XTaskQueueDispatch(manual, 0, DispatchTimeoutMs)) return Fail("delayed work never ran");
    const ULONGLONG elapsed = GetTickCount64() - scheduledAt;
    if (elapsed + 16 < ScheduleDelayMs || XAsyncGetStatus(&block, false) != S_OK || !delayed.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("delayed work");
    XTaskQueueCloseHandle(manual);
    return 0;
}

int AsyncCancel() {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("cancel queue");
    Recorder recorder;
    Completion completion;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &completion;
    block.callback = RecordCompletion;
    if (XAsyncBegin(&block, &recorder, nullptr, nullptr, CancelProvider) != S_OK) return Fail("cancelable begin");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpCancel})) return Fail("cancel op");
    if (XAsyncGetStatus(&block, false) != E_ABORT) return Fail("canceled status");
    if (completion.returned) return Fail("completion callback ran outside the completion port");
    if (!XTaskQueueDispatch(manual, 1, 0) || !completion.returned || completion.status != E_ABORT) return Fail("canceled completion callback");
    if (!XTaskQueueDispatch(manual, 0, 0) || !recorder.Is({OpBegin, OpCancel, OpCleanup})) return Fail("work after cancel");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpCancel, OpCleanup}) || XAsyncGetStatus(&block, true) != E_ABORT) return Fail("cancel after completion");

    Recorder late;
    Completion lateCompletion;
    AsyncBlock lateBlock{};
    lateBlock.queue = manual;
    lateBlock.context = &lateCompletion;
    lateBlock.callback = RecordCompletion;
    if (XAsyncBegin(&lateBlock, &late, nullptr, nullptr, CancelProvider) != S_OK) return Fail("cancel before work begin");
    XAsyncCancel(&lateBlock);
    if (!XTaskQueueDispatch(manual, 0, 0) || !late.Is({OpBegin, OpCancel})) return Fail("do work ran on a canceled call");
    if (!XTaskQueueDispatch(manual, 1, 0) || lateCompletion.status != E_ABORT || !late.Is({OpBegin, OpCancel, OpCleanup})) return Fail("canceled call completion after its work");

    Recorder early;
    Completion earlyCompletion;
    AsyncBlock earlyBlock{};
    earlyBlock.queue = manual;
    earlyBlock.context = &earlyCompletion;
    earlyBlock.callback = RecordCompletion;
    if (XAsyncBegin(&earlyBlock, &early, nullptr, nullptr, CompleteInBeginProvider) != S_OK) return Fail("complete in begin");
    if (XAsyncSchedule(&earlyBlock, 0) != E_INVALIDARG) return Fail("schedule after completion without payload");
    if (!XTaskQueueDispatch(manual, 1, 0) || earlyCompletion.status != S_OK || XTaskQueueDispatch(manual, 0, 0)) return Fail("complete in begin callback");
    if (!early.Is({OpBegin, OpCleanup})) return Fail("complete in begin op sequence");

    Completion stranded;
    AsyncBlock strandedBlock{};
    strandedBlock.queue = manual;
    strandedBlock.context = &stranded;
    strandedBlock.callback = RecordCompletion;
    if (XAsyncRun(&strandedBlock, ReturnWork) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("call before terminate");
    if (stranded.returned) return Fail("completion ran before terminate");
    if (FAILED(XTaskQueueTerminate(manual, true, nullptr, nullptr))) return Fail("terminate with a queued completion");
    if (!stranded.returned || stranded.status != RunResult || stranded.thread != GetCurrentThreadId()) return Fail("completion queued at termination");
    if (XAsyncGetStatus(&strandedBlock, true) != RunResult) return Fail("status after termination");
    XTaskQueueCloseHandle(manual);
    return 0;
}

int AsyncConcurrency(void* pool) {
    Counter chain;
    chain.target = 5;
    AsyncBlock chainBlock{};
    chainBlock.queue = pool;
    chainBlock.context = &chain;
    chainBlock.callback = ChainCompletion;
    if (XAsyncRun(&chainBlock, ReturnWork) != S_OK) return Fail("chained run");
    if (WaitForSingleObject(chain.event, DispatchTimeoutMs) != WAIT_OBJECT_0 || chain.count != 5 || chain.failures != 0) return Fail("block reused from its completion callback");

    Counter swarm;
    swarm.target = SwarmSize;
    AsyncBlock* blocks = new AsyncBlock[SwarmSize]();
    for (int i = 0; i < SwarmSize; ++i) {
        blocks[i].queue = pool;
        blocks[i].context = &swarm;
        blocks[i].callback = SwarmCompletion;
        if (XAsyncRun(&blocks[i], ReturnWork) != S_OK) return Fail("swarm run");
    }
    if (WaitForSingleObject(swarm.event, DispatchTimeoutMs) != WAIT_OBJECT_0 || swarm.failures != 0) return Fail("swarm completions");
    for (int i = 0; i < SwarmSize; ++i) {
        if (XAsyncGetStatus(&blocks[i], true) != RunResult) return Fail("swarm status");
    }
    delete[] blocks;

    Completer completer;
    std::thread completing([&] {
        for (;;) {
            WaitForSingleObject(completer.request, INFINITE);
            if (completer.stop) return;
            XAsyncComplete(completer.block, E_ABORT, 0);
            SetEvent(completer.done);
        }
    });
    int failure = 0;
    for (int i = 0; i < Repeats && failure == 0; ++i) {
        AsyncBlock block{};
        block.queue = pool;
        completer.block = &block;
        if (XAsyncBegin(&block, &completer, nullptr, nullptr, CompleterProvider) != S_OK) {
            failure = Fail("completer begin");
            break;
        }
        XAsyncCancel(&block);
        if (XAsyncGetStatus(&block, true) != E_ABORT) {
            failure = Fail("completer status");
            break;
        }
        const AsyncBlock snapshot = block;
        if (WaitForSingleObject(completer.done, DispatchTimeoutMs) != WAIT_OBJECT_0) {
            failure = Fail("completer timed out");
            break;
        }
        if (std::memcmp(&snapshot, &block, sizeof(block)) != 0) failure = Fail("block written after the waiting status returned");
    }
    completer.stop = true;
    SetEvent(completer.request);
    completing.join();
    return failure;
}

int AsyncScenario() {
    void* pool = nullptr;
    if (FAILED(XTaskQueueCreate(1, 1, &pool))) return Fail("async pool queue");
    if (AsyncRun(pool) != 0 || AsyncZeroBlock() != 0 || AsyncProviderCalls() != 0 || AsyncCancel() != 0 || AsyncConcurrency(pool) != 0) return 1;
    if (FAILED(XTaskQueueTerminate(pool, true, nullptr, nullptr))) return Fail("async pool terminate");
    XTaskQueueCloseHandle(pool);
    std::printf("async ok\n");
    return 0;
}

int Async() {
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([finished] {
        if (WaitForSingleObject(finished, WatchdogMs) != WAIT_OBJECT_0) {
            std::printf("FAIL async scenario blocked for %lu ms\n", WatchdogMs);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    });
    const int result = AsyncScenario();
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    return result;
}

int AsyncTerminated(bool beforeBegin) {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("terminated queue");
    Completion completion;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &completion;
    block.callback = RecordCompletion;
    if (beforeBegin) {
        XTaskQueueTerminate(manual, true, nullptr, nullptr);
        XAsyncBegin(&block, nullptr, nullptr, nullptr, ScheduleProvider);
        return Fail("begin on a terminated queue returned");
    }
    if (XAsyncBegin(&block, nullptr, nullptr, nullptr, ScheduleProvider) != S_OK) return Fail("terminated begin");
    XTaskQueueTerminate(manual, true, nullptr, nullptr);
    return Fail("terminate with a call in flight returned");
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
    if (std::strcmp(scenario, "async") == 0) {
        return Async();
    }
    if (std::strcmp(scenario, "async_null") == 0) {
        XAsyncGetStatus(nullptr, false);
        return Fail("null async block returned");
    }
    if (std::strcmp(scenario, "async_terminated") == 0) {
        return AsyncTerminated(false);
    }
    if (std::strcmp(scenario, "async_begin_terminated") == 0) {
        return AsyncTerminated(true);
    }
    if (std::strcmp(scenario, "crash") == 0) {
        XGameRuntimeInitialize();
        volatile int* null = nullptr;
        *null = 1;
        return Fail("crash returned");
    }
    return TaskQueue();
}
