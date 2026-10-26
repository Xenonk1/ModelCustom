#include "network_model_spoof.hpp"

#include "util.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace modelcustom
{
namespace
{
constexpr std::size_t kEntityNetObjectOffset = 208;
constexpr std::size_t kPlayerCreationNodeIndex = 0;
constexpr std::size_t kPlayerAppearanceNodeIndex = 12;


constexpr std::size_t kNetObjectGetSyncTreeIndex = 12;
constexpr std::size_t kNodeWriteObjectIndex = 24;

struct DatBitBuffer
{
    void* data;
    std::uint32_t bitOffset;
    std::uint32_t maxBit;
    std::uint32_t cursor;
    std::uint32_t currentBit;
    std::uint32_t unknownBit;
    std::uint8_t flags;
};

using GetBaseFromGuidFn = void* (*)(int handle);
using GetSyncTreeFn = void* (*)(void* netObject);
using NodeWriteObjectFn = void (*)(
    void* node,
    void* netObject,
    DatBitBuffer* buffer,
    void* logger,
    bool readFromObject);
using WriteUnsignedFn = bool (*)(void* data, std::uint32_t value, int bits, int offset);

GetBaseFromGuidFn g_getBaseFromGuid = nullptr;
WriteUnsignedFn g_writeUnsigned = nullptr;
std::uint8_t g_syncTreeDataNodesOffset = 0;

std::atomic<void*> g_localNetObject = nullptr;
std::atomic<std::uint32_t> g_networkModelHash = 0;
std::atomic<std::uint64_t> g_patchedWrites = 0;
std::atomic_bool g_switchInProgress = false;

void** g_creationVtable = nullptr;
void** g_appearanceVtable = nullptr;
void** g_creationHookVtable = nullptr;
void** g_appearanceHookVtable = nullptr;
std::size_t g_creationVtableEntries = 0;
std::size_t g_appearanceVtableEntries = 0;
void* g_creationNode = nullptr;
void* g_appearanceNode = nullptr;
NodeWriteObjectFn g_originalCreationWrite = nullptr;
NodeWriteObjectFn g_originalAppearanceWrite = nullptr;

HMODULE find_gta_module()
{



    return GetModuleHandleW(nullptr);
}

std::vector<std::pair<const std::uint8_t*, std::size_t>> executable_sections(HMODULE module)
{
    std::vector<std::pair<const std::uint8_t*, std::size_t>> result;
    if (!readable_address(module, sizeof(IMAGE_DOS_HEADER)))
    {
        return result;
    }

    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    {
        return result;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable_address(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return result;
    }




    const auto imageBegin = reinterpret_cast<std::uintptr_t>(base);
    const auto imageEnd = imageBegin + nt->OptionalHeader.SizeOfImage;
    auto cursor = imageBegin;
    while (cursor < imageEnd)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) !=
            sizeof(memory) || memory.RegionSize == 0)
        {
            break;
        }

        const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto regionEnd = regionBegin + memory.RegionSize;
        const auto clippedBegin = (std::max)(regionBegin, imageBegin);
        const auto clippedEnd = (std::min)(regionEnd, imageEnd);
        constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        if (memory.State == MEM_COMMIT &&
            (memory.Protect & kExecutable) != 0 &&
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            clippedEnd > clippedBegin)
        {
            result.emplace_back(
                reinterpret_cast<const std::uint8_t*>(clippedBegin),
                clippedEnd - clippedBegin);
        }

        if (regionEnd <= cursor)
        {
            break;
        }
        cursor = regionEnd;
    }

    return result;
}

const std::uint8_t* find_pattern(
    HMODULE module,
    const std::vector<int>& pattern,
    std::size_t* matchCount = nullptr)
{
    const std::uint8_t* first = nullptr;
    std::size_t count = 0;

    for (const auto& [begin, size] : executable_sections(module))
    {
        if (pattern.empty() || size < pattern.size())
        {
            continue;
        }

        for (std::size_t offset = 0; offset <= size - pattern.size(); ++offset)
        {
            bool matches = true;
            for (std::size_t index = 0; index < pattern.size(); ++index)
            {
                if (pattern[index] >= 0 && begin[offset + index] != pattern[index])
                {
                    matches = false;
                    break;
                }
            }

            if (matches)
            {
                first = first ? first : begin + offset;
                ++count;
            }
        }
    }

    if (matchCount)
    {
        *matchCount = count;
    }
    return first;
}

template <typename Function>
Function resolve(HMODULE module, const char* exportName)
{
    return reinterpret_cast<Function>(GetProcAddress(module, exportName));
}

void spoof_model_bits(void* netObject, DatBitBuffer* buffer, std::uint32_t startBit)
{
    const auto expectedObject = g_localNetObject.load(std::memory_order_acquire);
    if ((!g_switchInProgress.load(std::memory_order_acquire) && netObject != expectedObject) ||
        !buffer || !readable_address(buffer, sizeof(*buffer)) ||
        !buffer->data || !g_writeUnsigned)
    {
        return;
    }

    const auto hash = g_networkModelHash.load(std::memory_order_acquire);
    if (hash == 0 || buffer->cursor < startBit + 32 || buffer->cursor > buffer->maxBit)
    {
        return;
    }




    if (g_writeUnsigned(
        buffer->data,
        hash,
        32,
        static_cast<int>(buffer->bitOffset + startBit)))
    {
        const auto count = g_patchedWrites.fetch_add(1, std::memory_order_relaxed) + 1;
        if (adhesive_diag_counter_milestone(count))
        {
            adhesive_diag(
                "sync.payload_patch",
                "count=" + std::to_string(count) +
                " model_hash=" + to_hex(hash) +
                " bit_offset=" + std::to_string(buffer->bitOffset + startBit) +
                " cursor=" + std::to_string(buffer->cursor) +
                " max_bit=" + std::to_string(buffer->maxBit) +
                " net_object=" + adhesive_diag_address(netObject) +
                " payload=" + adhesive_diag_address(buffer->data));
        }
    }
}

void creation_write_hook(
    void* node,
    void* netObject,
    DatBitBuffer* buffer,
    void* logger,
    bool readFromObject)
{
    const auto startBit = buffer && readable_address(buffer, sizeof(*buffer)) ? buffer->cursor : 0;
    if (g_originalCreationWrite)
    {
        g_originalCreationWrite(node, netObject, buffer, logger, readFromObject);
    }
    spoof_model_bits(netObject, buffer, startBit);
}

void appearance_write_hook(
    void* node,
    void* netObject,
    DatBitBuffer* buffer,
    void* logger,
    bool readFromObject)
{
    const auto startBit = buffer && readable_address(buffer, sizeof(*buffer)) ? buffer->cursor : 0;
    if (g_originalAppearanceWrite)
    {
        g_originalAppearanceWrite(node, netObject, buffer, logger, readFromObject);
    }
    spoof_model_bits(netObject, buffer, startBit);
}

void log_node_hooks_ready(
    void** creationVtable,
    void** appearanceVtable,
    NodeWriteObjectFn originalCreation,
    NodeWriteObjectFn originalAppearance)
{
    adhesive_diag(
        "network.node_hooks.ready",
        "creation_vtable=" + adhesive_diag_address(creationVtable) +
        " appearance_vtable=" + adhesive_diag_address(appearanceVtable) +
        " creation_original=" + adhesive_diag_address(reinterpret_cast<void*>(originalCreation)) +
        " appearance_original=" + adhesive_diag_address(reinterpret_cast<void*>(originalAppearance)));
}

std::size_t vtable_entry_count(void** vtable)
{
    constexpr std::size_t kMaximumEntries = 64;
    std::size_t count = 0;
    while (count < kMaximumEntries &&
           readable_address(vtable + count, sizeof(void*)) &&
           executable_address(vtable[count]))
    {
        ++count;
    }
    return count;
}

void** clone_vtable(
    void** source,
    void* replacement,
    NodeWriteObjectFn& original,
    std::size_t& entryCount,
    const char* diagnosticEvent)
{
    const auto count = vtable_entry_count(source);
    if (count <= kNodeWriteObjectIndex || !readable_address(source - 1, sizeof(void*)))
    {
        return nullptr;
    }




    const auto bytes = (count + 1) * sizeof(void*);
    auto** allocation = static_cast<void**>(VirtualAlloc(
        nullptr,
        bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (!allocation)
    {
        return nullptr;
    }

    auto** clone = allocation + 1;
    allocation[0] = source[-1];
    std::memcpy(clone, source, count * sizeof(void*));
    original = reinterpret_cast<NodeWriteObjectFn>(source[kNodeWriteObjectIndex]);
    clone[kNodeWriteObjectIndex] = replacement;

    DWORD previousProtection = 0;
    if (!VirtualProtect(allocation, bytes, PAGE_READONLY, &previousProtection))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        original = nullptr;
        return nullptr;
    }

    entryCount = count;
    adhesive_diag(
        diagnosticEvent,
        "source=" + adhesive_diag_address(source) +
        " clone=" + adhesive_diag_address(clone) +
        " entries=" + std::to_string(count) +
        " original=" + adhesive_diag_address(reinterpret_cast<void*>(original)) +
        " replacement=" + adhesive_diag_address(replacement));
    return clone;
}

bool install_private_vtable(void* node, void** expected, void** replacement)
{
    if (!readable_address(node, sizeof(void*)) || !expected || !replacement)
    {
        return false;
    }

    auto* slot = reinterpret_cast<void* volatile*>(node);
    const auto current = *slot;
    if (current == replacement)
    {
        return true;
    }
    if (current != expected)
    {
        return false;
    }

    return InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
}

void restore_private_vtable(void*& node, void** original, void** replacement, const char* event)
{
    if (node && readable_address(node, sizeof(void*)) && original && replacement)
    {
        auto* slot = reinterpret_cast<void* volatile*>(node);
        if (InterlockedCompareExchangePointer(slot, original, replacement) == replacement)
        {
            adhesive_diag(
                event,
                "node=" + adhesive_diag_address(node) +
                " restored=" + adhesive_diag_address(original));
        }
    }
    node = nullptr;
}

void* net_object_from_ped(int ped)
{
    if (!g_getBaseFromGuid || ped == 0)
    {
        return nullptr;
    }

    void* entity = nullptr;
    __try
    {
        entity = g_getBaseFromGuid(ped);
        if (!readable_address(entity, kEntityNetObjectOffset + sizeof(void*)))
        {
            return nullptr;
        }
        return *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(entity) + kEntityNetObjectOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void* sync_tree_from_net_object(void* netObject)
{
    if (!readable_address(netObject, sizeof(void*)))
    {
        return nullptr;
    }

    __try
    {
        auto** vtable = *reinterpret_cast<void***>(netObject);
        if (!readable_address(vtable + kNetObjectGetSyncTreeIndex, sizeof(void*)) ||
            !executable_address(vtable[kNetObjectGetSyncTreeIndex]))
        {
            return nullptr;
        }

        const auto getSyncTree = reinterpret_cast<GetSyncTreeFn>(
            vtable[kNetObjectGetSyncTreeIndex]);
        return getSyncTree(netObject);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}
}

bool NetworkModelSpoof::initialize()
{
    if (g_getBaseFromGuid && g_writeUnsigned && g_syncTreeDataNodesOffset != 0)
    {
        return true;
    }

    const auto streaming = GetModuleHandleW(L"gta-streaming-five.dll");
    const auto gta = find_gta_module();
    if (!streaming || !gta)
    {
        adhesive_diag(
            "network.resolver.modules_missing",
            "streaming_loaded=" + std::to_string(streaming != nullptr) +
            " gta_loaded=" + std::to_string(gta != nullptr));
        log_error("Network model spoof init failed: streaming=" +
            std::to_string(streaming != nullptr) + ", gta=" + std::to_string(gta != nullptr));
        return false;
    }

    g_getBaseFromGuid = resolve<GetBaseFromGuidFn>(
        streaming,
        "?GetBaseFromGuid@fwScriptGuid@rage@@SAPEAVfwEntity@@H@Z");

    const std::vector<int> dataNodesPattern = {
        0x48, 0x8D, 0x4B, -1, 0x33, 0xD2, 0x41, 0xB8,
        -1, -1, -1, -1, 0xE8, -1, -1, -1, -1,
        0x48, 0x8D, 0x8B, -1, -1, -1, -1, 0x33, 0xD2, 0x41, 0xB8,
        -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xC3
    };
    std::size_t dataNodesMatches = 0;
    const auto* dataNodesMatch = find_pattern(gta, dataNodesPattern, &dataNodesMatches);
    if (dataNodesMatch && dataNodesMatches == 1)
    {
        g_syncTreeDataNodesOffset = dataNodesMatch[3];
    }

    const std::vector<int> writeUnsignedPattern = {
        0x49, 0xC1, 0xFA, 0x03, 0x41, 0x83, 0xE1, 0x07,
        0x4C, 0x03, 0xD1, 0xB9, 0x20, 0x00, 0x00, 0x00
    };
    std::size_t writeUnsignedMatches = 0;
    const auto* writeUnsignedMatch = find_pattern(gta, writeUnsignedPattern, &writeUnsignedMatches);
    if (writeUnsignedMatch && writeUnsignedMatches == 1 &&
        executable_address(writeUnsignedMatch - 0xE))
    {
        g_writeUnsigned = reinterpret_cast<WriteUnsignedFn>(
            const_cast<std::uint8_t*>(writeUnsignedMatch - 0xE));
    }

    if (!g_getBaseFromGuid || !g_writeUnsigned || g_syncTreeDataNodesOffset == 0)
    {
        adhesive_diag(
            "network.resolver.failed",
            "get_base=" + adhesive_diag_address(reinterpret_cast<void*>(g_getBaseFromGuid)) +
            " bit_writer=" + adhesive_diag_address(reinterpret_cast<void*>(g_writeUnsigned)) +
            " bit_writer_matches=" + std::to_string(writeUnsignedMatches) +
            " data_node_matches=" + std::to_string(dataNodesMatches) +
            " data_nodes_offset=" + to_hex(g_syncTreeDataNodesOffset));
        log_error("Network model spoof resolver failed (guid=" +
            std::to_string(g_getBaseFromGuid != nullptr) +
            ", bitWriterMatches=" + std::to_string(writeUnsignedMatches) +
            ", dataNodeMatches=" + std::to_string(dataNodesMatches) + ")");
        return false;
    }

    log("Network model spoof resolver ready; sync-tree data node offset=" +
        to_hex(g_syncTreeDataNodesOffset));
    adhesive_diag(
        "network.resolver.ready",
        "gta=" + adhesive_diag_address(gta) +
        " streaming=" + adhesive_diag_address(streaming) +
        " get_base=" + adhesive_diag_address(reinterpret_cast<void*>(g_getBaseFromGuid)) +
        " bit_writer=" + adhesive_diag_address(reinterpret_cast<void*>(g_writeUnsigned)) +
        " data_nodes_offset=" + to_hex(g_syncTreeDataNodesOffset));
    return true;
}

bool NetworkModelSpoof::install_node_hooks(void* syncTree)
{
    if (!syncTree || g_syncTreeDataNodesOffset == 0)
    {
        return false;
    }

    __try
    {
        auto** nodes = reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(syncTree) + g_syncTreeDataNodesOffset);
        if (!readable_address(nodes, (kPlayerAppearanceNodeIndex + 1) * sizeof(void*)))
        {
            return false;
        }

        auto* creationNode = nodes[kPlayerCreationNodeIndex];
        auto* appearanceNode = nodes[kPlayerAppearanceNodeIndex];
        if (!readable_address(creationNode, sizeof(void*)) ||
            !readable_address(appearanceNode, sizeof(void*)))
        {
            return false;
        }

        auto** creationVtable = *reinterpret_cast<void***>(creationNode);
        auto** appearanceVtable = *reinterpret_cast<void***>(appearanceNode);

        if (!g_creationHookVtable)
        {
            g_creationVtable = creationVtable;
            g_creationHookVtable = clone_vtable(
                creationVtable,
                reinterpret_cast<void*>(&creation_write_hook),
                g_originalCreationWrite,
                g_creationVtableEntries,
                "network.private_vtable.create.creation");
            if (!g_creationHookVtable)
            {
                return false;
            }
        }
        else if (creationVtable != g_creationVtable && creationVtable != g_creationHookVtable)
        {
            return false;
        }

        if (!g_appearanceHookVtable)
        {
            g_appearanceVtable = appearanceVtable;
            g_appearanceHookVtable = clone_vtable(
                appearanceVtable,
                reinterpret_cast<void*>(&appearance_write_hook),
                g_originalAppearanceWrite,
                g_appearanceVtableEntries,
                "network.private_vtable.create.appearance");
            if (!g_appearanceHookVtable)
            {
                return false;
            }
        }
        else if (appearanceVtable != g_appearanceVtable && appearanceVtable != g_appearanceHookVtable)
        {
            return false;
        }

        if (!install_private_vtable(creationNode, g_creationVtable, g_creationHookVtable) ||
            !install_private_vtable(appearanceNode, g_appearanceVtable, g_appearanceHookVtable))
        {
            return false;
        }

        g_creationNode = creationNode;
        g_appearanceNode = appearanceNode;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    log_node_hooks_ready(
        g_creationVtable,
        g_appearanceVtable,
        g_originalCreationWrite,
        g_originalAppearanceWrite);
    return g_originalCreationWrite && g_originalAppearanceWrite;
}

bool NetworkModelSpoof::prepare_switch(int currentPed, std::uint32_t networkModelHash)
{
    if (networkModelHash == 0 || !initialize())
    {
        return false;
    }

    auto* netObject = net_object_from_ped(currentPed);
    if (!ready())
    {
        auto* syncTree = sync_tree_from_net_object(netObject);
        if (!netObject || !install_node_hooks(syncTree))
        {
            return false;
        }
    }

    if (netObject)
    {
        g_localNetObject.store(netObject, std::memory_order_release);
    }
    g_networkModelHash.store(networkModelHash, std::memory_order_release);
    g_patchedWrites.store(0, std::memory_order_release);
    g_switchInProgress.store(true, std::memory_order_release);
    log("Armed local player sync hash spoof: remote model=" + to_hex(networkModelHash));
    adhesive_diag(
        "network.spoof.arm",
        "model_hash=" + to_hex(networkModelHash) +
        " ped=" + std::to_string(currentPed) +
        " net_object=" + adhesive_diag_address(netObject));
    return true;
}

bool NetworkModelSpoof::refresh_local_ped(int currentPed)
{
    auto* netObject = net_object_from_ped(currentPed);
    if (!netObject)
    {
        return false;
    }

    auto* syncTree = sync_tree_from_net_object(netObject);
    if (!install_node_hooks(syncTree))
    {
        return false;
    }

    const auto previous = g_localNetObject.exchange(netObject, std::memory_order_acq_rel);
    if (previous != netObject)
    {
        log("Local player network object refreshed after model construction");
    }
    return true;
}

void NetworkModelSpoof::finish_switch()
{
    g_switchInProgress.store(false, std::memory_order_release);
    adhesive_diag(
        "network.spoof.switch_finished",
        "patched_writes=" + std::to_string(g_patchedWrites.load(std::memory_order_acquire)));
}

void NetworkModelSpoof::disarm()
{
    const auto patchedWrites = g_patchedWrites.load(std::memory_order_acquire);
    const auto previousHash = g_networkModelHash.load(std::memory_order_acquire);
    const auto previousObject = g_localNetObject.load(std::memory_order_acquire);
    g_switchInProgress.store(false, std::memory_order_release);
    g_localNetObject.store(nullptr, std::memory_order_release);
    g_networkModelHash.store(0, std::memory_order_release);
    g_patchedWrites.store(0, std::memory_order_release);
    restore_private_vtable(
        g_creationNode,
        g_creationVtable,
        g_creationHookVtable,
        "network.private_vtable.restore.creation");
    restore_private_vtable(
        g_appearanceNode,
        g_appearanceVtable,
        g_appearanceHookVtable,
        "network.private_vtable.restore.appearance");
    adhesive_diag(
        "network.spoof.disarm",
        "model_hash=" + to_hex(previousHash) +
        " patched_writes=" + std::to_string(patchedWrites) +
        " net_object=" + adhesive_diag_address(previousObject));
}

void NetworkModelSpoof::shutdown()
{
    adhesive_diag("network.shutdown.begin", "ready=" + std::to_string(ready()));
    disarm();

    if (g_creationHookVtable)
    {
        VirtualFree(g_creationHookVtable - 1, 0, MEM_RELEASE);
    }
    if (g_appearanceHookVtable)
    {
        VirtualFree(g_appearanceHookVtable - 1, 0, MEM_RELEASE);
    }

    g_creationVtable = nullptr;
    g_appearanceVtable = nullptr;
    g_creationHookVtable = nullptr;
    g_appearanceHookVtable = nullptr;
    g_creationVtableEntries = 0;
    g_appearanceVtableEntries = 0;
    g_originalCreationWrite = nullptr;
    g_originalAppearanceWrite = nullptr;
    adhesive_diag("network.shutdown.end", "hooks_restored=1");
}

bool NetworkModelSpoof::ready() const
{
    return g_creationVtable && g_appearanceVtable &&
        g_creationHookVtable && g_appearanceHookVtable &&
        g_creationNode && g_appearanceNode &&
        g_originalCreationWrite && g_originalAppearanceWrite;
}

bool NetworkModelSpoof::is_resolver_ready() const
{
    return g_getBaseFromGuid != nullptr && g_writeUnsigned != nullptr && g_syncTreeDataNodesOffset != 0;
}

std::uint32_t NetworkModelSpoof::network_model_hash() const
{
    return g_networkModelHash.load(std::memory_order_acquire);
}

std::uint64_t NetworkModelSpoof::patched_write_count() const
{
    return g_patchedWrites.load(std::memory_order_acquire);
}

void* NetworkModelSpoof::local_net_object() const
{
    return g_localNetObject.load(std::memory_order_acquire);
}

void* get_local_net_object()
{
    return g_localNetObject.load(std::memory_order_acquire);
}

std::uint8_t get_sync_tree_nodes_offset()
{
    return g_syncTreeDataNodesOffset;
}
}
