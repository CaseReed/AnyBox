#include "xdll/xgameruntime/xgameruntime.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_set>

static_assert(sizeof(XAsyncBlock) == sizeof(void*) * 7);
static_assert(offsetof(XAsyncBlock, queue) == 0);
static_assert(offsetof(XAsyncBlock, context) == sizeof(void*));
static_assert(offsetof(XAsyncBlock, callback) == sizeof(void*) * 2);
static_assert(offsetof(XAsyncBlock, internal) == sizeof(void*) * 3);
static_assert(sizeof(XAsyncBlock::internal) == sizeof(void*) * 4);
static_assert(sizeof(XAsyncProviderData) == sizeof(void*) * 4);
static_assert(offsetof(XAsyncProviderData, async) == 0);
static_assert(offsetof(XAsyncProviderData, bufferSize) == sizeof(void*));
static_assert(offsetof(XAsyncProviderData, buffer) == sizeof(void*) * 2);
static_assert(offsetof(XAsyncProviderData, context) == sizeof(void*) * 3);
static_assert(sizeof(XAsyncOp) == sizeof(std::uint32_t));

namespace {

constexpr std::uint32_t LiveSignature = 0x4C584241;
constexpr std::uint32_t DoneSignature = 0x44584241;
constexpr std::uint32_t RetrievedSignature = 0x52584241;
constexpr std::uint32_t ProviderSignature = 0x50584241;

constexpr HRESULT IllegalMethodCall = static_cast<HRESULT>(0x8000000Eu);
constexpr HRESULT NotSufficientBuffer = static_cast<HRESULT>(0x8007007Au);
constexpr HRESULT NoTaskQueue = static_cast<HRESULT>(0x800701ABu);

constexpr const char* QueueCallbackFunction = "XAsyncComplete";

struct Call;

struct BlockState {
    std::uint32_t signature;
    HRESULT status;
    Call* call;
    std::uint64_t serial;
};

static_assert(sizeof(BlockState) <= sizeof(XAsyncBlock::internal));
static_assert(std::is_trivially_copyable_v<BlockState>);

struct Call {
    std::uint64_t serial = 0;
    XAsyncBlock* userBlock = nullptr;
    XAsyncBlock providerBlock{};
    XAsyncProviderData data{};
    XAsyncProvider* provider = nullptr;
    XAsyncCompletionRoutine* callback = nullptr;
    const void* identity = nullptr;
    XTaskQueueHandle queue = nullptr;
    HRESULT status = E_PENDING;
    std::size_t requiredSize = 0;
    std::size_t refs = 0;
    bool attached = false;
    bool retrieved = false;
    bool workScheduled = false;
    bool cancelRunning = false;
    bool finished = false;
};

struct Registry {
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_set<Call*> calls;
    std::uint64_t lastSerial = 0;
};

Registry& Calls() {
    static Registry* registry = new Registry();
    return *registry;
}

thread_local const Call* t_completing = nullptr;

enum class Kind {
    Unknown,
    Empty,
    Live,
    Done,
    Retrieved,
    Provider,
};

struct Found {
    Kind kind;
    HRESULT status;
    Call* call;
};

BlockState Load(const XAsyncBlock* block) {
    BlockState state;
    std::memcpy(&state, block->internal, sizeof(state));
    return state;
}

void Store(XAsyncBlock* block, const BlockState& state) {
    std::memcpy(block->internal, &state, sizeof(state));
}

bool Empty(const XAsyncBlock* block) {
    for (const unsigned char byte : block->internal) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

Found Find(const Registry& registry, XAsyncBlock* block) {
    const BlockState state = Load(block);
    const bool known = state.call != nullptr && registry.calls.count(state.call) != 0 && state.call->serial == state.serial;
    switch (state.signature) {
    case ProviderSignature:
        if (known && &state.call->providerBlock == block) {
            return {Kind::Provider, state.call->status, state.call};
        }
        break;
    case LiveSignature:
        if (known && state.call->userBlock == block && state.call->attached) {
            return {Kind::Live, state.call->status, state.call};
        }
        break;
    case DoneSignature:
    case RetrievedSignature:
        return {state.signature == DoneSignature ? Kind::Done : Kind::Retrieved, state.status, known && state.call->userBlock == block ? state.call : nullptr};
    default:
        break;
    }
    if (Empty(block)) {
        return {Kind::Empty, S_OK, nullptr};
    }
    return {Kind::Unknown, E_PENDING, nullptr};
}

Found FindKnown(const Registry& registry, XAsyncBlock* block, const char* function) {
    const Found found = Find(registry, block);
    if (found.kind == Kind::Unknown) {
        AbxUnsupported(function, "the async block is neither zeroed nor in use by an XAsync call");
    }
    return found;
}

bool Attached(const Found& found) {
    return (found.kind == Kind::Live || found.kind == Kind::Provider) && found.call->attached;
}

bool Drop(Registry& registry, Call* call) {
    if (--call->refs != 0) {
        return false;
    }
    registry.calls.erase(call);
    return true;
}

void Destroy(Call* call) {
    call->provider(XAsyncOp::Cleanup, &call->data);
    XTaskQueueCloseHandle(call->queue);
    delete call;
}

void Release(Call* call) {
    Registry& registry = Calls();
    bool last = false;
    {
        std::lock_guard lock(registry.mutex);
        last = Drop(registry, call);
    }
    if (last) {
        Destroy(call);
    }
}

bool Detach(Registry& registry, Call* call, std::uint32_t signature) {
    call->attached = false;
    call->retrieved = signature == RetrievedSignature;
    Store(call->userBlock, {signature, call->status, call, call->serial});
    return Drop(registry, call);
}

void CompletionCallback(void* context, bool) {
    Call* call = static_cast<Call*>(context);
    Registry& registry = Calls();
    const Call* outer = t_completing;
    t_completing = call;
    call->callback(call->userBlock);
    t_completing = outer;
    bool last = false;
    {
        std::lock_guard lock(registry.mutex);
        call->finished = true;
        registry.changed.notify_all();
        last = Drop(registry, call);
    }
    if (last) {
        Destroy(call);
    }
}

void CompleteCall(Call* call, HRESULT result, std::size_t requiredBufferSize, const char* function) {
    if (result == E_PENDING) {
        return;
    }
    Registry& registry = Calls();
    std::unique_lock lock(registry.mutex);
    if (call->status != E_PENDING) {
        return;
    }
    call->status = result;
    call->requiredSize = FAILED(result) ? 0 : requiredBufferSize;
    call->data.bufferSize = call->requiredSize;
    bool last = false;
    if (call->requiredSize == 0 && call->attached) {
        last = Detach(registry, call, DoneSignature);
    }
    if (call->callback != nullptr) {
        ++call->refs;
        lock.unlock();
        if (FAILED(XTaskQueueSubmitCallback(call->queue, XTaskQueuePort::Completion, call, CompletionCallback))) {
            AbxUnsupported(function, "the completion callback cannot be queued because the call's task queue is terminated");
        }
        return;
    }
    call->finished = true;
    registry.changed.notify_all();
    lock.unlock();
    if (last) {
        Destroy(call);
    }
}

void CancelCall(Call* call) {
    Registry& registry = Calls();
    {
        std::lock_guard lock(registry.mutex);
        if (call->status != E_PENDING || call->cancelRunning) {
            return;
        }
        call->cancelRunning = true;
    }
    call->provider(XAsyncOp::Cancel, &call->data);
    std::lock_guard lock(registry.mutex);
    call->cancelRunning = false;
}

void WorkCallback(void* context, bool canceled) {
    Call* call = static_cast<Call*>(context);
    Registry& registry = Calls();
    bool attached = false;
    {
        std::lock_guard lock(registry.mutex);
        call->workScheduled = false;
        attached = call->attached;
    }
    if (attached && canceled) {
        CancelCall(call);
        CompleteCall(call, E_ABORT, 0, QueueCallbackFunction);
    } else if (attached) {
        const HRESULT result = call->provider(XAsyncOp::DoWork, &call->data);
        if (result != E_PENDING) {
            CompleteCall(call, SUCCEEDED(result) ? E_UNEXPECTED : result, 0, QueueCallbackFunction);
        }
    }
    Release(call);
}

Call* Acquire(XAsyncBlock* asyncBlock) {
    Registry& registry = Calls();
    std::lock_guard lock(registry.mutex);
    Call* call = Find(registry, asyncBlock).call;
    if (call != nullptr) {
        ++call->refs;
    }
    return call;
}

HRESULT RunProvider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
        XAsyncComplete(data->async, reinterpret_cast<XAsyncWork*>(data->context)(data->async), 0);
        return S_OK;
    default:
        return S_OK;
    }
}

}

ABX_API HRESULT XAsyncBegin(XAsyncBlock* asyncBlock, void* context, const void* identity, const char*, XAsyncProvider* provider) {
    if (asyncBlock == nullptr || provider == nullptr) {
        AbxUnsupported(__func__, "null async block or provider");
    }
    Registry& registry = Calls();
    {
        std::lock_guard lock(registry.mutex);
        if (Find(registry, asyncBlock).kind == Kind::Live) {
            return E_INVALIDARG;
        }
    }
    const auto fail = [&](HRESULT failure) {
        std::lock_guard lock(registry.mutex);
        Store(asyncBlock, {DoneSignature, failure, nullptr, 0});
        return failure;
    };
    XTaskQueueHandle queue = nullptr;
    try {
        if (asyncBlock->queue == nullptr) {
            if (!XTaskQueueGetCurrentProcessTaskQueue(&queue)) {
                return fail(NoTaskQueue);
            }
        } else {
            const HRESULT duplicated = XTaskQueueDuplicateHandle(asyncBlock->queue, &queue);
            if (FAILED(duplicated)) {
                return fail(duplicated);
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(E_OUTOFMEMORY);
    }
    Call* call = new (std::nothrow) Call();
    if (call == nullptr) {
        XTaskQueueCloseHandle(queue);
        return fail(E_OUTOFMEMORY);
    }
    call->userBlock = asyncBlock;
    call->providerBlock = *asyncBlock;
    call->providerBlock.queue = queue;
    call->data = {&call->providerBlock, 0, nullptr, context};
    call->provider = provider;
    call->callback = asyncBlock->callback;
    call->identity = identity;
    call->queue = queue;
    call->refs = 2;
    call->attached = true;
    {
        std::lock_guard lock(registry.mutex);
        try {
            registry.calls.insert(call);
        } catch (const std::bad_alloc&) {
            delete call;
            XTaskQueueCloseHandle(queue);
            Store(asyncBlock, {DoneSignature, E_OUTOFMEMORY, nullptr, 0});
            return E_OUTOFMEMORY;
        }
        call->serial = ++registry.lastSerial;
        Store(&call->providerBlock, {ProviderSignature, E_PENDING, call, call->serial});
        Store(asyncBlock, {LiveSignature, E_PENDING, call, call->serial});
    }
    const HRESULT begun = provider(XAsyncOp::Begin, &call->data);
    if (FAILED(begun)) {
        CompleteCall(call, begun, 0, __func__);
    }
    Release(call);
    return S_OK;
}

ABX_API HRESULT XAsyncSchedule(XAsyncBlock* asyncBlock, std::uint32_t delayInMs) {
    if (asyncBlock == nullptr) {
        AbxUnsupported(__func__, "null async block");
    }
    Registry& registry = Calls();
    Call* call = nullptr;
    {
        std::lock_guard lock(registry.mutex);
        const Found found = FindKnown(registry, asyncBlock, __func__);
        if (FAILED(found.status) && found.status != E_PENDING) {
            return found.status;
        }
        if (!Attached(found)) {
            return E_INVALIDARG;
        }
        if (found.call->workScheduled) {
            return E_UNEXPECTED;
        }
        call = found.call;
        call->workScheduled = true;
        ++call->refs;
    }
    const HRESULT queued = XTaskQueueSubmitDelayedCallback(call->queue, XTaskQueuePort::Work, delayInMs, call, WorkCallback);
    if (FAILED(queued)) {
        Release(call);
    }
    return queued;
}

ABX_API void XAsyncComplete(XAsyncBlock* asyncBlock, HRESULT result, std::size_t requiredBufferSize) {
    if (result == E_PENDING) {
        return;
    }
    if (asyncBlock == nullptr) {
        AbxUnsupported(__func__, "null async block");
    }
    Call* call = Acquire(asyncBlock);
    if (call == nullptr) {
        return;
    }
    CompleteCall(call, result, requiredBufferSize, __func__);
    Release(call);
}

ABX_API void XAsyncCancel(XAsyncBlock* asyncBlock) {
    if (asyncBlock == nullptr) {
        AbxUnsupported(__func__, "null async block");
    }
    Call* call = Acquire(asyncBlock);
    if (call == nullptr) {
        return;
    }
    CancelCall(call);
    Release(call);
}

ABX_API HRESULT XAsyncRun(XAsyncBlock* asyncBlock, XAsyncWork* work) {
    if (asyncBlock == nullptr || work == nullptr) {
        AbxUnsupported(__func__, "null async block or work callback");
    }
    return XAsyncBegin(asyncBlock, reinterpret_cast<void*>(work), reinterpret_cast<const void*>(&XAsyncRun), __func__, RunProvider);
}

ABX_API HRESULT XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait) {
    if (asyncBlock == nullptr) {
        AbxUnsupported(__func__, "null async block");
    }
    Registry& registry = Calls();
    std::unique_lock lock(registry.mutex);
    const Found found = FindKnown(registry, asyncBlock, __func__);
    Call* call = found.call;
    if (!wait || call == nullptr || call == t_completing) {
        return found.status;
    }
    ++call->refs;
    registry.changed.wait(lock, [call] { return call->finished; });
    const HRESULT status = call->status;
    const bool last = Drop(registry, call);
    lock.unlock();
    if (last) {
        Destroy(call);
    }
    return status;
}

ABX_API HRESULT XAsyncGetResultSize(XAsyncBlock* asyncBlock, std::size_t* bufferSize) {
    if (asyncBlock == nullptr || bufferSize == nullptr) {
        AbxUnsupported(__func__, "null async block or buffer size");
    }
    Registry& registry = Calls();
    std::lock_guard lock(registry.mutex);
    const Found found = FindKnown(registry, asyncBlock, __func__);
    *bufferSize = Attached(found) ? found.call->requiredSize : 0;
    return found.status;
}

ABX_API HRESULT XAsyncGetResult(XAsyncBlock* asyncBlock, const void* identity, std::size_t bufferSize, void* buffer, std::size_t* bufferUsed) {
    if (asyncBlock == nullptr) {
        AbxUnsupported(__func__, "null async block");
    }
    Registry& registry = Calls();
    std::unique_lock lock(registry.mutex);
    const Found found = FindKnown(registry, asyncBlock, __func__);
    if (FAILED(found.status)) {
        return found.status;
    }
    if (found.kind == Kind::Retrieved || (found.kind == Kind::Provider && found.call->retrieved)) {
        return IllegalMethodCall;
    }
    if (!Attached(found)) {
        if (bufferUsed != nullptr) {
            *bufferUsed = 0;
        }
        return found.status;
    }
    Call* call = found.call;
    ++call->refs;
    HRESULT result = S_OK;
    if (identity != call->identity || buffer == nullptr) {
        result = E_INVALIDARG;
    } else if (bufferSize < call->requiredSize) {
        result = NotSufficientBuffer;
    } else {
        if (bufferUsed != nullptr) {
            *bufferUsed = call->requiredSize;
        }
        call->data.bufferSize = bufferSize;
        call->data.buffer = buffer;
        lock.unlock();
        result = call->provider(XAsyncOp::GetResult, &call->data);
        lock.lock();
    }
    if (result != E_PENDING && call->attached) {
        Detach(registry, call, RetrievedSignature);
    }
    const bool last = Drop(registry, call);
    lock.unlock();
    if (last) {
        Destroy(call);
    }
    return result;
}
