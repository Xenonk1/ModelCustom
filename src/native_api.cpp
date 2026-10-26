#include "native_api.hpp"

#include "util.hpp"

#include <cstring>
#include <cstddef>
#include <string>

namespace modelcustom::native
{
namespace
{
using NativeHandler = void (*)(void* context);
using GetNativeHandlerFn = NativeHandler (*)(std::uint64_t hash);
using GetActiveThreadFn = void* (*)();
using SetActiveThreadFn = void (*)(void* thread);
using GetThreadCollectionFn = void* (*)();
using GetThreadContextFn = void* (*)(void* thread);

GetNativeHandlerFn g_getRageNativeHandler = nullptr;
GetActiveThreadFn g_getActiveThread = nullptr;
SetActiveThreadFn g_setActiveThread = nullptr;
GetThreadCollectionFn g_getThreadCollection = nullptr;
GetThreadContextFn g_getThreadContext = nullptr;

struct ThreadCollectionView
{
    void** data;
    std::uint16_t count;
    std::uint16_t capacity;
};

struct ThreadContextPrefix
{
    std::uint32_t threadId;
    std::uint32_t scriptHash;
    std::uint32_t state;
};

struct ScrVector
{
    alignas(8) float x;
    alignas(8) float y;
    alignas(8) float z;
};

struct alignas(16) ScrVec3N
{
    float x;
    float y;
    float z;
};

struct ScrVectorSpace
{
    ScrVector* outVectors[4];
    ScrVec3N inVectors[4];
};

struct ScrCallContext
{
    void* returnValue;
    std::uint32_t argumentCount;
    void* arguments;
    std::uint32_t dataCount;
    ScrVectorSpace vectorSpace;
    std::uint8_t vectorPad[96];
    std::uint64_t stack[32];

    ScrCallContext()
    {
        std::memset(stack, 0, sizeof(stack));
        arguments = stack;
        returnValue = stack;
        argumentCount = 0;
        dataCount = 0;
        std::memset(&vectorSpace, 0, sizeof(vectorSpace));
        std::memset(vectorPad, 0, sizeof(vectorPad));
    }

    void push(std::uint64_t value)
    {
        if (argumentCount < 32)
        {
            stack[argumentCount++] = value;
        }
    }

    std::uint64_t result() const
    {
        return stack[0];
    }
};

static_assert(sizeof(ScrVectorSpace) == 96);
static_assert(offsetof(ScrCallContext, vectorSpace) == 32);

template <typename Function>
Function resolve(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool resolve_rage_scripting()
{
    if (g_getRageNativeHandler)
    {
        return true;
    }

    const HMODULE module = GetModuleHandleW(L"rage-scripting-five.dll");
    if (!module)
    {
        return false;
    }

    g_getRageNativeHandler = resolve<GetNativeHandlerFn>(
        module,
        "?GetNativeHandler@scrEngine@rage@@SAP6AXPEAVscrNativeCallContext@2@@Z_K@Z");
    g_getActiveThread = resolve<GetActiveThreadFn>(
        module,
        "?GetActiveThread@scrEngine@rage@@SAPEAVscrThread@2@XZ");
    g_setActiveThread = resolve<SetActiveThreadFn>(
        module,
        "?SetActiveThread@scrEngine@rage@@SAXPEAVscrThread@2@@Z");
    g_getThreadCollection = resolve<GetThreadCollectionFn>(
        module,
        "?GetThreadCollection@scrEngine@rage@@SAPEAV?$pgPtrCollection@VGtaThread@@@2@XZ");
    g_getThreadContext = resolve<GetThreadContextFn>(
        module,
        "?GetContext@scrThread@rage@@QEAAPEAUscrThreadContext@2@XZ");
    if (g_getRageNativeHandler)
    {
        log("Resolved direct FiveM native handler bridge");
    }

    return g_getRageNativeHandler != nullptr;
}

void* find_live_script_thread()
{
    if (!g_getThreadCollection || !g_getThreadContext)
    {
        return nullptr;
    }

    const auto* collection = static_cast<const ThreadCollectionView*>(g_getThreadCollection());
    if (!collection || !collection->data || collection->count == 0 ||
        collection->count > collection->capacity || collection->count > 1024)
    {
        return nullptr;
    }

    for (std::uint16_t index = 0; index < collection->count; ++index)
    {
        void* thread = collection->data[index];
        if (!thread)
        {
            continue;
        }

        const auto* context = static_cast<const ThreadContextPrefix*>(g_getThreadContext(thread));

        if (context && context->threadId != 0 && context->state != 2)
        {
            return thread;
        }
    }
    return nullptr;
}

std::uint64_t invoke_handler_guarded(
    NativeHandler handler,
    ScrCallContext* context,
    bool* raisedException)
{
    __try
    {
        handler(context);
        return context->result();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *raisedException = true;
        return 0;
    }
}
}

bool can_invoke_natives()
{
    return resolve_rage_scripting();
}

ScriptContextScope::ScriptContextScope()
{
    if (!resolve_rage_scripting() || !g_getActiveThread || !g_setActiveThread)
    {
        return;
    }

    previous_ = g_getActiveThread();
    if (previous_)
    {
        ready_ = true;
        return;
    }

    if (void* borrowed = find_live_script_thread())
    {
        g_setActiveThread(borrowed);
        changed_ = true;
        ready_ = true;
    }
}

ScriptContextScope::~ScriptContextScope()
{
    if (changed_ && g_setActiveThread)
    {
        g_setActiveThread(previous_);
    }
}

std::uint64_t invoke(std::uint64_t hash, std::initializer_list<std::uint64_t> arguments)
{


    if (resolve_rage_scripting())
    {
        if (const auto handler = g_getRageNativeHandler(hash))
        {
            ScrCallContext context;
            for (const auto argument : arguments)
            {
                context.push(argument);
            }

            bool raisedException = false;
            const auto result = invoke_handler_guarded(handler, &context, &raisedException);
            if (!raisedException)
            {
                return result;
            }
            log_error("Direct native handler raised an exception for " + to_hex(hash));
        }
    }

    return 0;
}

void invoke_void(std::uint64_t hash, std::initializer_list<std::uint64_t> arguments)
{
    static_cast<void>(invoke(hash, arguments));
}

bool invoke_vector3(
    std::uint64_t hash,
    std::initializer_list<std::uint64_t> arguments,
    Vector3& result)
{
    if (!resolve_rage_scripting())
    {
        return false;
    }

    const auto handler = g_getRageNativeHandler(hash);
    if (!handler)
    {
        return false;
    }

    ScrCallContext context;
    for (const auto argument : arguments)
    {
        context.push(argument);
    }

    bool raisedException = false;
    static_cast<void>(invoke_handler_guarded(handler, &context, &raisedException));
    if (raisedException)
    {
        log_error("Direct vector native handler raised an exception for " + to_hex(hash));
        return false;
    }

    struct ScrVectorLayout
    {
        float x;
        std::uint32_t padX;
        float y;
        std::uint32_t padY;
        float z;
        std::uint32_t padZ;
    };

    const auto* retVec = reinterpret_cast<const ScrVectorLayout*>(context.returnValue);
    if (std::isfinite(retVec->x) && std::isfinite(retVec->y) && std::isfinite(retVec->z) &&
        (retVec->x != 0.0F || retVec->y != 0.0F || retVec->z != 0.0F))
    {
        result = { retVec->x, retVec->y, retVec->z };
        return true;
    }

    const auto& inVec = context.vectorSpace.inVectors[0];
    if (std::isfinite(inVec.x) && std::isfinite(inVec.y) && std::isfinite(inVec.z) &&
        (inVec.x != 0.0F || inVec.y != 0.0F || inVec.z != 0.0F))
    {
        result = { inVec.x, inVec.y, inVec.z };
        return true;
    }

    result = { retVec->x, retVec->y, retVec->z };
    return true;
}

void subtitle(std::string_view message, int durationMilliseconds)
{


    static_cast<void>(message);
    static_cast<void>(durationMilliseconds);
}

std::string get_current_server_endpoint()
{
    const auto ptr = invoke(kGetCurrentServerEndpoint);
    if (!ptr)
    {
        return {};
    }

    const auto* str = reinterpret_cast<const char*>(ptr);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(str, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return {};
    }

    if (str[0] == '\0')
    {
        return {};
    }

    return std::string(str);
}


int get_player_server_id(int playerId)
{
    return static_cast<int>(invoke(kGetPlayerServerId, { static_cast<std::uint64_t>(playerId) }));
}

int get_player_from_server_id(int serverId)
{
    return static_cast<int>(invoke(kGetPlayerFromServerId, { static_cast<std::uint64_t>(serverId) }));
}

bool is_player_active(int playerId)
{
    if (playerId < 0 || playerId >= 256)
    {
        return false;
    }
    return invoke(kNetworkIsPlayerActive, { static_cast<std::uint64_t>(playerId) }) != 0;
}

int resolve_player_from_server_id(int serverId)
{
    if (serverId <= 0)
    {
        return -1;
    }

    const int direct = get_player_from_server_id(serverId);
    if (direct >= 0 && direct < 256 && is_player_active(direct))
    {
        return direct;
    }

    for (int i = 0; i < 256; ++i)
    {
        if (is_player_active(i) && get_player_server_id(i) == serverId)
        {
            return i;
        }
    }

    return -1;
}

int get_player_ped(int playerId)
{
    return static_cast<int>(invoke(kGetPlayerPed, { static_cast<std::uint64_t>(playerId) }));
}
}
