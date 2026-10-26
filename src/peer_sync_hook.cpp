#include "peer_sync_hook.hpp"

#include "native_api.hpp"
#include "network_model_spoof.hpp"
#include "util.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace modelcustom
{
namespace
{
HMODULE g_netModule = nullptr;
HMODULE g_coreModule = nullptr;
const std::int32_t* g_pModelHashOffset = nullptr;
const std::int32_t* g_pAppearanceModelHashOffset = nullptr;
const std::int32_t* g_pAppearanceHeadBlendOffset = nullptr;
void** g_ppTheClones = nullptr;

using CanApplyToObjectFn = PeerSyncHook::CanApplyToObjectFn;
CanApplyToObjectFn* g_ppCanApplyToObject = nullptr;
CanApplyToObjectFn g_realOrigCanApplyToObject = nullptr;

constexpr std::size_t kNodeCanApplyIndex = 20;
using CanApplyFn = PeerSyncHook::CanApplyFn;
using NodeCanApplyFn = CanApplyFn;
NodeCanApplyFn g_origCreationCanApply = nullptr;
CanApplyFn* g_ppOrigPlayerCreationCanApply = nullptr;
CanApplyFn g_realOrigPlayerCreationCanApply = nullptr;

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
    const std::vector<int>& pattern)
{
    for (const auto& [begin, size] : executable_sections(module))
    {
        if (size < pattern.size())
        {
            continue;
        }

        const auto* scanEnd = begin + (size - pattern.size() + 1);
        for (const auto* cursor = begin; cursor < scanEnd; ++cursor)
        {
            bool match = true;
            for (std::size_t i = 0; i < pattern.size(); ++i)
            {
                if (pattern[i] != -1 &&
                    cursor[i] != static_cast<std::uint8_t>(pattern[i]))
                {
                    match = false;
                    break;
                }
            }

            if (match)
            {
                return cursor;
            }
        }
    }

    return nullptr;
}
}

PeerSyncHook& PeerSyncHook::instance()
{
    static PeerSyncHook hook;
    return hook;
}

PeerSyncHook::~PeerSyncHook()
{
    uninstall_pointer_hook();
    uninstall_vtable_hook();
}

void PeerSyncHook::uninstall_pointer_hook()
{
    std::scoped_lock lock(mutex_);
    if (canApplyToObjectHooked_ && g_ppCanApplyToObject && g_realOrigCanApplyToObject)
    {
        const auto previous = *g_ppCanApplyToObject;
        DWORD oldProtect = 0;
        const bool protectionChanged = VirtualProtect(
            g_ppCanApplyToObject,
            sizeof(void*),
            PAGE_READWRITE,
            &oldProtect) != FALSE;
        *g_ppCanApplyToObject = g_realOrigCanApplyToObject;
        DWORD ignored = 0;
        const bool protectionRestored = VirtualProtect(
            g_ppCanApplyToObject,
            sizeof(void*),
            oldProtect,
            &ignored) != FALSE;
        adhesive_diag_pointer_write(
            "peer.pointer.restore.can_apply_to_object",
            g_ppCanApplyToObject,
            reinterpret_cast<void*>(previous),
            reinterpret_cast<void*>(g_realOrigCanApplyToObject),
            oldProtect,
            protectionChanged,
            protectionRestored);
        canApplyToObjectHooked_ = false;
        log("PeerSyncHook: restored CanApplyToObject pointer in gta-net-five.dll");
    }

    if (pointerHooked_)
    {
        if (g_ppOrigPlayerCreationCanApply && g_realOrigPlayerCreationCanApply)
        {
            const auto previous = *g_ppOrigPlayerCreationCanApply;
            DWORD oldProtect = 0;
            const bool protectionChanged = VirtualProtect(
                g_ppOrigPlayerCreationCanApply,
                sizeof(void*),
                PAGE_READWRITE,
                &oldProtect) != FALSE;
            *g_ppOrigPlayerCreationCanApply = g_realOrigPlayerCreationCanApply;
            DWORD ignored = 0;
            const bool protectionRestored = VirtualProtect(
                g_ppOrigPlayerCreationCanApply,
                sizeof(void*),
                oldProtect,
                &ignored) != FALSE;
            adhesive_diag_pointer_write(
                "peer.pointer.restore.player_can_apply",
                g_ppOrigPlayerCreationCanApply,
                reinterpret_cast<void*>(previous),
                reinterpret_cast<void*>(g_realOrigPlayerCreationCanApply),
                oldProtect,
                protectionChanged,
                protectionRestored);
        }
        pointerHooked_ = false;
        log("PeerSyncHook: restored Creation CanApply pointer in gta-net-five.dll");
    }
}

bool PeerSyncHook::initialize()
{
    if (initialized_)
    {
        return true;
    }

    g_netModule = GetModuleHandleW(L"gta-net-five.dll");
    if (!g_netModule)
    {
        adhesive_diag("peer.resolver.module_missing", "module=gta-net-five.dll");
        return false;
    }



    static const std::vector<int> kCanApplyPattern = {
        0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x83, 0x7A, 0x08, 0x0B
    };

    const auto* canApplyBytes = find_pattern(g_netModule, kCanApplyPattern);
    if (!canApplyBytes)
    {
        log_error("PeerSyncHook: failed to find CPlayerCreationDataNode__CanApply pattern in gta-net-five.dll");
        return false;
    }



    const auto* movsxdBytes = static_cast<const std::uint8_t*>(nullptr);
    for (std::size_t i = 0; i < 64; ++i)
    {
        if (canApplyBytes[i] == 0x48 && canApplyBytes[i + 1] == 0x63 && canApplyBytes[i + 2] == 0x1D)
        {
            movsxdBytes = &canApplyBytes[i];
            break;
        }
    }

    if (!movsxdBytes)
    {
        log_error("PeerSyncHook: failed to find movsxd pattern in CanApply");
        return false;
    }

    const auto relOffset = *reinterpret_cast<const std::int32_t*>(movsxdBytes + 3);
    g_pModelHashOffset = reinterpret_cast<const std::int32_t*>(movsxdBytes + 7 + relOffset);
    if (g_pModelHashOffset && readable_address(g_pModelHashOffset, sizeof(std::int32_t)))
    {
        creationModelOffset_ = *g_pModelHashOffset;
    }



    const auto* jmpBytes = static_cast<const std::uint8_t*>(nullptr);
    for (std::size_t i = 0; i < 150; ++i)
    {
        if (canApplyBytes[i] == 0x48 && canApplyBytes[i + 1] == 0xFF && canApplyBytes[i + 2] == 0x25)
        {
            jmpBytes = &canApplyBytes[i];
            break;
        }
    }

    if (jmpBytes)
    {
        const auto relJmp = *reinterpret_cast<const std::int32_t*>(jmpBytes + 3);
        g_ppOrigPlayerCreationCanApply = reinterpret_cast<CanApplyFn*>(
            const_cast<std::uint8_t*>(jmpBytes + 7 + relJmp));

        if (g_ppOrigPlayerCreationCanApply && readable_address(g_ppOrigPlayerCreationCanApply, sizeof(void*)))
        {
            g_realOrigPlayerCreationCanApply = *g_ppOrigPlayerCreationCanApply;

            DWORD oldProtect = 0;
            const bool protectionChanged = VirtualProtect(
                g_ppOrigPlayerCreationCanApply,
                sizeof(void*),
                PAGE_READWRITE,
                &oldProtect) != FALSE;
            *g_ppOrigPlayerCreationCanApply = &Detour_PlayerCanApply;
            DWORD ignored = 0;
            const bool protectionRestored = VirtualProtect(
                g_ppOrigPlayerCreationCanApply,
                sizeof(void*),
                oldProtect,
                &ignored) != FALSE;
            adhesive_diag_pointer_write(
                "peer.pointer.install.player_can_apply",
                g_ppOrigPlayerCreationCanApply,
                reinterpret_cast<void*>(g_realOrigPlayerCreationCanApply),
                reinterpret_cast<void*>(&Detour_PlayerCanApply),
                oldProtect,
                protectionChanged,
                protectionRestored);
            pointerHooked_ = true;
            log("PeerSyncHook: successfully installed Player CanApply pointer hook (" +
                to_hex(reinterpret_cast<std::uintptr_t>(g_ppOrigPlayerCreationCanApply)) +
                " -> orig=" + to_hex(reinterpret_cast<std::uintptr_t>(g_realOrigPlayerCreationCanApply)) + ")");
        }
    }
    else
    {
        log_error("PeerSyncHook: failed to find jmp pattern in CanApply");
    }



    static const std::vector<int> kCanApplyCallPattern = {
        0xC6, 0x40, 0x4B, 0x01, 0x49, 0x8B, 0xD6, 0x49, 0x8B, 0xCC, 0xE8
    };
    const auto* callMatch = find_pattern(g_netModule, kCanApplyCallPattern);
    if (callMatch)
    {
        const auto* callInsn = callMatch + 10;
        const auto relCall = *reinterpret_cast<const std::int32_t*>(callInsn + 1);
        const auto* thunk = callInsn + 5 + relCall;

        if (thunk[0] == 0x48 && thunk[1] == 0xFF && thunk[2] == 0x25)
        {
            const auto relJmp = *reinterpret_cast<const std::int32_t*>(thunk + 3);
            g_ppCanApplyToObject = reinterpret_cast<CanApplyToObjectFn*>(
                const_cast<std::uint8_t*>(thunk + 7 + relJmp));
        }
        else if (thunk[0] == 0xFF && thunk[1] == 0x25)
        {
            const auto relJmp = *reinterpret_cast<const std::int32_t*>(thunk + 2);
            g_ppCanApplyToObject = reinterpret_cast<CanApplyToObjectFn*>(
                const_cast<std::uint8_t*>(thunk + 6 + relJmp));
        }
    }

    if (!g_ppCanApplyToObject)
    {

        static const std::vector<int> kThunkPattern = {
            0x5F, 0x5E, 0x5D, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC,
            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
            0x48, 0xFF, 0x25
        };
        const auto* thunkMatch = find_pattern(g_netModule, kThunkPattern);
        if (thunkMatch)
        {
            const auto* jmpInsn = thunkMatch + 16;
            const auto relJmp = *reinterpret_cast<const std::int32_t*>(jmpInsn + 3);
            g_ppCanApplyToObject = reinterpret_cast<CanApplyToObjectFn*>(
                const_cast<std::uint8_t*>(jmpInsn + 7 + relJmp));
        }
    }

    if (g_ppCanApplyToObject && readable_address(g_ppCanApplyToObject, sizeof(void*)))
    {
        g_realOrigCanApplyToObject = *g_ppCanApplyToObject;
        DWORD oldProtect = 0;
        const bool protectionChanged = VirtualProtect(
            g_ppCanApplyToObject,
            sizeof(void*),
            PAGE_READWRITE,
            &oldProtect) != FALSE;
        *g_ppCanApplyToObject = &Detour_netSyncTree_CanApplyToObject;
        DWORD ignored = 0;
        const bool protectionRestored = VirtualProtect(
            g_ppCanApplyToObject,
            sizeof(void*),
            oldProtect,
            &ignored) != FALSE;
        adhesive_diag_pointer_write(
            "peer.pointer.install.can_apply_to_object",
            g_ppCanApplyToObject,
            reinterpret_cast<void*>(g_realOrigCanApplyToObject),
            reinterpret_cast<void*>(&Detour_netSyncTree_CanApplyToObject),
            oldProtect,
            protectionChanged,
            protectionRestored);
        canApplyToObjectHooked_ = true;
        log("PeerSyncHook: successfully installed netSyncTree::CanApplyToObject pointer hook (" +
            to_hex(reinterpret_cast<std::uintptr_t>(g_ppCanApplyToObject)) +
            " -> orig=" + to_hex(reinterpret_cast<std::uintptr_t>(g_realOrigCanApplyToObject)) + ")");
    }
    else
    {
        log_error("PeerSyncHook: failed to find netSyncTree::CanApplyToObject pointer in gta-net-five.dll");
    }



    static const std::vector<int> kTheClonesPattern = {
        0x0F, 0xB7, 0xF8, 0x48, 0x8B, 0x11, 0x4C, 0x8B, 0x42, 0x20
    };

    const auto* clonesMatch = find_pattern(g_netModule, kTheClonesPattern);
    if (!clonesMatch)
    {
        log_error("PeerSyncHook: failed to find TheClones pattern in gta-net-five.dll");
        return false;
    }


    const auto* movTheClones = clonesMatch - 7;
    if (movTheClones[0] != 0x48 || movTheClones[1] != 0x8B || movTheClones[2] != 0x0D)
    {
        log_error("PeerSyncHook: unexpected instruction preceding TheClones pattern");
        return false;
    }

    const auto relClones = *reinterpret_cast<const std::int32_t*>(movTheClones + 3);
    g_ppTheClones = reinterpret_cast<void**>(const_cast<std::uint8_t*>(movTheClones + 7 + relClones));


    g_coreModule = GetModuleHandleW(L"gta-core-five.dll");
    if (g_coreModule)
    {
        static const std::vector<int> kAppearancePattern = {
            0x81, 0xF9, 0xF2, 0x61, 0x5E, 0x70
        };
        const auto* cmpMatch = find_pattern(g_coreModule, kAppearancePattern);
        if (cmpMatch)
        {
            const auto* start = cmpMatch - 35;
            const auto* mov1 = static_cast<const std::uint8_t*>(nullptr);
            const auto* mov2 = static_cast<const std::uint8_t*>(nullptr);
            for (const auto* p = start; p < cmpMatch - 7; ++p)
            {
                if (p[0] == 0x48 && p[1] == 0x63 && p[2] == 0x05)
                {
                    if (!mov1)
                    {
                        mov1 = p;
                        p += 6;
                    }
                    else if (!mov2)
                    {
                        mov2 = p;
                        break;
                    }
                }
            }
            if (mov1 && mov2)
            {
                const auto rel1 = *reinterpret_cast<const std::int32_t*>(mov1 + 3);
                const auto rel2 = *reinterpret_cast<const std::int32_t*>(mov2 + 3);
                g_pAppearanceModelHashOffset = reinterpret_cast<const std::int32_t*>(mov1 + 7 + rel1);
                g_pAppearanceHeadBlendOffset = reinterpret_cast<const std::int32_t*>(mov2 + 7 + rel2);
            }
        }

        if (!g_pAppearanceModelHashOffset || !g_pAppearanceHeadBlendOffset)
        {
            const auto* base = reinterpret_cast<const char*>(g_coreModule);
            g_pAppearanceModelHashOffset = reinterpret_cast<const std::int32_t*>(base + 0x1453B0);
            g_pAppearanceHeadBlendOffset = reinterpret_cast<const std::int32_t*>(base + 0x1453B4);
        }

        if (g_pAppearanceModelHashOffset && g_pAppearanceHeadBlendOffset &&
            readable_address(g_pAppearanceModelHashOffset, sizeof(std::int32_t)) &&
            readable_address(g_pAppearanceHeadBlendOffset, sizeof(std::int32_t)))
        {
            appearanceModelOffset_ = *g_pAppearanceModelHashOffset;
            appearanceHeadBlendOffset_ = *g_pAppearanceHeadBlendOffset;
            log("PeerSyncHook: Appearance offsets resolved: modelOffset=" +
                std::to_string(appearanceModelOffset_) +
                " headBlendOffset=" + std::to_string(appearanceHeadBlendOffset_));
        }
    }

    initialized_ = true;
    log("PeerSyncHook: resolver ready; TheClones=" + to_hex(reinterpret_cast<std::uintptr_t>(g_ppTheClones)) +
        " CreationModelOffsetPtr=" + to_hex(reinterpret_cast<std::uintptr_t>(g_pModelHashOffset)));
    adhesive_diag(
        "peer.resolver.ready",
        "net_module=" + adhesive_diag_address(g_netModule) +
        " core_module=" + adhesive_diag_address(g_coreModule) +
        " can_apply=" + adhesive_diag_address(canApplyBytes) +
        " player_pointer=" + adhesive_diag_address(g_ppOrigPlayerCreationCanApply) +
        " tree_pointer=" + adhesive_diag_address(g_ppCanApplyToObject) +
        " clones_pointer=" + adhesive_diag_address(g_ppTheClones) +
        " creation_offset=" + std::to_string(creationModelOffset_) +
        " appearance_offset=" + std::to_string(appearanceModelOffset_) +
        " blend_offset=" + std::to_string(appearanceHeadBlendOffset_));
    return true;
}

bool PeerSyncHook::install_vtable_hook(void** creationVtable)
{
    if (pointerHooked_)
    {
        return true;
    }

    if (!creationVtable)
    {
        return false;
    }

    if (!initialized_)
    {
        initialize();
    }

    std::scoped_lock lock(mutex_);
    if (vtableHooked_ && creationVtable_ == creationVtable)
    {
        return true;
    }

    void* original = nullptr;
    if (!replace_vtable_entry(
            creationVtable,
            kNodeCanApplyIndex,
            reinterpret_cast<void*>(&Detour_PlayerCanApply),
            &original))
    {
        log_error("PeerSyncHook: failed to replace vtable index 20 (CanApply)");
        return false;
    }

    creationVtable_ = creationVtable;
    g_origCreationCanApply = reinterpret_cast<NodeCanApplyFn>(original);
    vtableHooked_ = true;
    log("PeerSyncHook: successfully installed VTable CanApply hook on CPlayerCreationDataNode (index 20)");
    adhesive_diag(
        "peer.vtable.install.player_can_apply",
        "index=" + std::to_string(kNodeCanApplyIndex) +
        " vtable=" + adhesive_diag_address(creationVtable_) +
        " slot=" + adhesive_diag_address(creationVtable_ + kNodeCanApplyIndex) +
        " original=" + adhesive_diag_address(reinterpret_cast<void*>(g_origCreationCanApply)) +
        " replacement=" + adhesive_diag_address(reinterpret_cast<void*>(&Detour_PlayerCanApply)));
    return true;
}

void PeerSyncHook::uninstall_vtable_hook()
{
    std::scoped_lock lock(mutex_);
    if (vtableHooked_ && creationVtable_ && g_origCreationCanApply)
    {
        restore_vtable_entry(
            creationVtable_,
            kNodeCanApplyIndex,
            reinterpret_cast<void*>(g_origCreationCanApply));
        adhesive_diag(
            "peer.vtable.restore.player_can_apply",
            "index=" + std::to_string(kNodeCanApplyIndex) +
            " vtable=" + adhesive_diag_address(creationVtable_) +
            " original=" + adhesive_diag_address(reinterpret_cast<void*>(g_origCreationCanApply)));
        vtableHooked_ = false;
        creationVtable_ = nullptr;
        g_origCreationCanApply = nullptr;
        log("PeerSyncHook: restored CanApply vtable entry");
    }
}

void PeerSyncHook::set_local_server_id(int serverId)
{
    localServerId_ = serverId;
}

void PeerSyncHook::set_local_object_id(std::uint16_t objectId)
{
    localObjectId_.store(objectId, std::memory_order_release);
}

void PeerSyncHook::set_peer_model(int serverId, std::uint32_t modelHash)
{
    std::unique_lock lock(mutex_);
    peerModels_[serverId] = modelHash;
}

void PeerSyncHook::remove_peer_model(int serverId)
{
    std::unique_lock lock(mutex_);
    peerModels_.erase(serverId);
    recreateStates_.erase(serverId);
}

void PeerSyncHook::clear_all_peers()
{
    std::vector<std::uint16_t> objectsToDelete;
    {
        std::unique_lock lock(mutex_);
        for (const auto& [serverId, _] : peerModels_)
        {
            const auto it = peerServerIdToObjectId_.find(serverId);
            if (it != peerServerIdToObjectId_.end() && it->second > 0)
            {
                objectsToDelete.push_back(it->second);
            }
        }
        peerModels_.clear();
        recreateStates_.clear();
    }

    for (const auto objId : objectsToDelete)
    {
        delete_clone_object(objId);
    }
}

std::uint32_t PeerSyncHook::get_peer_model(int serverId) const
{
    std::shared_lock lock(mutex_);
    const auto it = peerModels_.find(serverId);
    if (it != peerModels_.end())
    {
        return it->second;
    }
    return 0;
}

std::vector<int> PeerSyncHook::get_tracked_peer_server_ids() const
{
    std::shared_lock lock(mutex_);
    std::vector<int> result;
    result.reserve(peerModels_.size());
    for (const auto& [serverId, _] : peerModels_)
    {
        result.push_back(serverId);
    }
    return result;
}

bool __fastcall PeerSyncHook::Detour_netSyncTree_CanApplyToObject(void* syncTree, void* netObj)
{
    if (!g_realOrigCanApplyToObject)
    {
        return false;
    }

    if (!syncTree || !netObj || !readable_address(netObj, 76))
    {
        return g_realOrigCanApplyToObject(syncTree, netObj);
    }


    const auto objectType = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(netObj) + 8);
    if (objectType != 11)
    {
        return g_realOrigCanApplyToObject(syncTree, netObj);
    }


    if (netObj == get_local_net_object())
    {
        return g_realOrigCanApplyToObject(syncTree, netObj);
    }


    const auto isRemote = *reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<const char*>(netObj) + 75);
    if (!isRemote)
    {
        return g_realOrigCanApplyToObject(syncTree, netObj);
    }

    const auto resolvedObjectId = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(netObj) + 10);
    const auto localObjId = instance().localObjectId_.load(std::memory_order_acquire);
    if (localObjId != 0 && resolvedObjectId == localObjId)
    {
        return g_realOrigCanApplyToObject(syncTree, netObj);
    }

    int resolvedServerId = static_cast<int>(get_client_id(netObj));
    if (resolvedServerId <= 0)
    {
        resolvedServerId = *reinterpret_cast<const std::uint8_t*>(
            reinterpret_cast<const char*>(netObj) + 73);
    }
    if (resolvedServerId <= 0)
    {
        resolvedServerId = instance().get_server_id_from_object_id(resolvedObjectId);
    }

    if (resolvedServerId > 0 && resolvedObjectId > 0)
    {
        instance().register_peer_object_id(resolvedObjectId, resolvedServerId);
    }

    if (resolvedServerId > 0 && resolvedServerId != instance().localServerId_)
    {
        const uint32_t customModelHash = instance().get_peer_model(resolvedServerId);
        if (customModelHash != 0)
        {
            const auto dynamicNodesOffset = get_sync_tree_nodes_offset();
            const auto nodesOffset = dynamicNodesOffset != 0 ? dynamicNodesOffset : 0x30;
            if (readable_address(reinterpret_cast<const char*>(syncTree) + nodesOffset, sizeof(void*) * 13))
            {
                auto** nodes = reinterpret_cast<void**>(
                    static_cast<char*>(syncTree) + nodesOffset);


                void* creationNode = nodes[0];
                const int creationOffset = instance().creationModelOffset_;
                if (creationNode && creationOffset > 0 &&
                    readable_address(reinterpret_cast<char*>(creationNode) + creationOffset, sizeof(std::uint32_t)))
                {
                    auto& hashRef = *reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<char*>(creationNode) + creationOffset);
                    hashRef = customModelHash;
                }


                void* appearanceNode = nodes[12];
                if (appearanceNode)
                {
                    const int appModelOffset = instance().appearanceModelOffset_;
                    if (appModelOffset > 0 &&
                        readable_address(reinterpret_cast<char*>(appearanceNode) + appModelOffset, sizeof(std::uint32_t)))
                    {
                        auto& hashRef = *reinterpret_cast<std::uint32_t*>(
                            reinterpret_cast<char*>(appearanceNode) + appModelOffset);
                        hashRef = customModelHash;
                    }

                    const int appBlendOffset = instance().appearanceHeadBlendOffset_;
                    if (appBlendOffset > 0 &&
                        readable_address(reinterpret_cast<char*>(appearanceNode) + appBlendOffset, sizeof(bool)))
                    {
                        auto& blendRef = *reinterpret_cast<bool*>(
                            reinterpret_cast<char*>(appearanceNode) + appBlendOffset);
                        blendRef = false;
                    }
                }

                const auto count = instance().appliedCount_.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;
                if (adhesive_diag_counter_milestone(count))
                {
                    adhesive_diag(
                        "peer.sync_tree_model_patch",
                        "count=" + std::to_string(count) +
                        " server_id=" + std::to_string(resolvedServerId) +
                        " object_id=" + std::to_string(resolvedObjectId) +
                        " model_hash=" + to_hex(customModelHash) +
                        " sync_tree=" + adhesive_diag_address(syncTree) +
                        " net_object=" + adhesive_diag_address(netObj));
                }
            }
        }
    }

    return g_realOrigCanApplyToObject(syncTree, netObj);
}

bool __fastcall PeerSyncHook::Detour_PlayerCanApply(void* thisptr, void* netObj)
{
    CanApplyFn target = g_realOrigPlayerCreationCanApply ? g_realOrigPlayerCreationCanApply : g_origCreationCanApply;
    return Detour_CommonCanApply(thisptr, netObj, target, true);
}

bool PeerSyncHook::Detour_CommonCanApply(void* thisptr, void* netObj, CanApplyFn target, bool isPlayerNode)
{
    if (!target)
    {
        return false;
    }

    if (!thisptr || !netObj || !readable_address(netObj, 76))
    {
        return target(thisptr, netObj);
    }


    if (netObj == get_local_net_object())
    {
        return target(thisptr, netObj);
    }

    const auto objectType = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(netObj) + 8);

    if (objectType != 11 || !isPlayerNode)
    {
        return target(thisptr, netObj);
    }


    const auto isRemote = *reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<const char*>(netObj) + 75);
    if (!isRemote)
    {
        return target(thisptr, netObj);
    }

    const auto resolvedObjectId = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const char*>(netObj) + 10);

    const auto localObjId = instance().localObjectId_.load(std::memory_order_acquire);
    if (localObjId != 0 && resolvedObjectId == localObjId)
    {
        return target(thisptr, netObj);
    }

    int resolvedServerId = static_cast<int>(get_client_id(netObj));
    if (resolvedServerId <= 0 && readable_address(reinterpret_cast<const char*>(netObj) + 74, 1))
    {
        resolvedServerId = *reinterpret_cast<const std::uint8_t*>(
            reinterpret_cast<const char*>(netObj) + 73);
    }
    if (resolvedServerId <= 0)
    {
        resolvedServerId = instance().get_server_id_from_object_id(resolvedObjectId);
    }

    if (resolvedServerId > 0 && resolvedObjectId > 0)
    {
        instance().register_peer_object_id(resolvedObjectId, resolvedServerId);
    }

    uint32_t prevModelHash = 0;
    uint32_t customModelHash = 0;
    bool injectedBefore = false;

    if (resolvedServerId > 0 && resolvedServerId != instance().localServerId_)
    {
        customModelHash = instance().get_peer_model(resolvedServerId);
        if (customModelHash != 0)
        {
            const int32_t offset = instance().creationModelOffset_;
            if (offset > 0)
            {
                auto& hashRef = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<char*>(thisptr) + offset);
                prevModelHash = hashRef;
                hashRef = customModelHash;
                const auto count = instance().appliedCount_.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;
                if (adhesive_diag_counter_milestone(count))
                {
                    adhesive_diag(
                        "peer.can_apply_model_patch",
                        "count=" + std::to_string(count) +
                        " server_id=" + std::to_string(resolvedServerId) +
                        " object_id=" + std::to_string(resolvedObjectId) +
                        " previous_hash=" + to_hex(prevModelHash) +
                        " model_hash=" + to_hex(customModelHash) +
                        " node=" + adhesive_diag_address(thisptr) +
                        " net_object=" + adhesive_diag_address(netObj));
                }
                injectedBefore = true;
            }
        }
    }

    bool result = target(thisptr, netObj);

    if (injectedBefore && !result)
    {
        const int32_t offset = instance().creationModelOffset_;
        if (offset > 0)
        {
            auto& hashRef = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<char*>(thisptr) + offset);
            hashRef = prevModelHash;
            result = target(thisptr, netObj);
        }
    }

    return result;
}

std::uint16_t PeerSyncHook::get_client_id(void* netObj)
{
    if (!g_ppTheClones || !readable_address(g_ppTheClones, sizeof(void*)) || !*g_ppTheClones || !netObj)
    {
        return 0;
    }

    void* theClones = *g_ppTheClones;
    if (!readable_address(theClones, sizeof(void*)))
    {
        return 0;
    }
    void** vtable = *reinterpret_cast<void***>(theClones);
    if (!vtable || !readable_address(vtable, sizeof(void*) * 5))
    {
        return 0;
    }

    using GetClientIdFn = std::uint16_t (__fastcall*)(void*, void*);
    auto fn = reinterpret_cast<GetClientIdFn>(vtable[4]);
    return fn ? fn(theClones, netObj) : 0;
}

void PeerSyncHook::delete_clone_object(std::uint16_t objectId)
{
    if (!g_ppTheClones || !readable_address(g_ppTheClones, sizeof(void*)) || !*g_ppTheClones)
    {
        return;
    }

    void* theClones = *g_ppTheClones;
    if (!readable_address(theClones, sizeof(void*)))
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(theClones);
    if (!vtable || !readable_address(vtable, sizeof(void*) * 13))
    {
        return;
    }

    using DeleteObjectIdFn = void (__fastcall*)(void*, std::uint16_t, std::uint16_t, bool);
    auto fn = reinterpret_cast<DeleteObjectIdFn>(vtable[12]);
    if (fn)
    {
        adhesive_diag(
            "peer.clone_delete",
            "object_id=" + std::to_string(objectId) +
            " clones=" + adhesive_diag_address(theClones) +
            " vtable=" + adhesive_diag_address(vtable) +
            " function=" + adhesive_diag_address(reinterpret_cast<void*>(fn)));
        fn(theClones, objectId, 0, true);
    }
}

void PeerSyncHook::register_peer_object_id(std::uint16_t objectId, int serverId)
{
    std::unique_lock lock(mutex_);
    const auto it = peerServerIdToObjectId_.find(serverId);
    if (it != peerServerIdToObjectId_.end() && it->second == objectId)
    {
        return;
    }
    objectIdToPeerServerId_[objectId] = serverId;
    peerServerIdToObjectId_[serverId] = objectId;
}

bool PeerSyncHook::recreate_peer(int serverId)
{
    if (!initialized_ || !g_ppTheClones || !*g_ppTheClones)
    {
        return false;
    }

    std::uint16_t objectId = 0;
    {
        std::shared_lock lock(mutex_);
        const auto it = peerServerIdToObjectId_.find(serverId);
        if (it != peerServerIdToObjectId_.end())
        {
            objectId = it->second;
        }
    }

    if (objectId == 0)
    {
        return false;
    }

    const auto now = GetTickCount64();
    {
        std::unique_lock lock(mutex_);
        auto& state = recreateStates_[serverId];
        if (now - state.lastRecreateTime > 8000)
        {
            state.attempts = 0;
        }
        if (now - state.lastRecreateTime < 2000)
        {
            return false;
        }
        if (state.attempts >= 5)
        {
            return false;
        }
        state.lastRecreateTime = now;
        state.attempts++;
    }

    log("PeerSyncHook: triggering recreate for peer serverId=" + std::to_string(serverId) +
        " objectId=" + std::to_string(objectId));
    delete_clone_object(objectId);
    return true;
}

bool PeerSyncHook::remove_and_recreate_peer(int serverId)
{
    std::uint16_t objectId = 0;
    {
        std::unique_lock lock(mutex_);
        peerModels_.erase(serverId);
        recreateStates_.erase(serverId);
        const auto it = peerServerIdToObjectId_.find(serverId);
        if (it != peerServerIdToObjectId_.end())
        {
            objectId = it->second;
        }
    }

    if (objectId == 0)
    {
        const int player = native::resolve_player_from_server_id(serverId);
        if (player >= 0)
        {
            const int ped = native::get_player_ped(player);
            if (ped != 0)
            {
                const auto netId = native::invoke(native::kPedToNet, { static_cast<std::uint64_t>(ped) });
                if (netId > 0 && netId <= 0xFFFF)
                {
                    objectId = static_cast<std::uint16_t>(netId);
                }
            }
        }
    }

    if (objectId > 0 && g_ppTheClones && *g_ppTheClones)
    {
        log("PeerSyncHook: peer serverId=" + std::to_string(serverId) +
            " left/disabled; deleting clone objectId=" + std::to_string(objectId) +
            " to restore normal server ped");
        delete_clone_object(objectId);
        return true;
    }

    return false;
}

int PeerSyncHook::get_server_id_from_object_id(std::uint16_t objectId) const
{
    std::shared_lock lock(mutex_);
    const auto it = objectIdToPeerServerId_.find(objectId);
    if (it != objectIdToPeerServerId_.end())
    {
        return it->second;
    }
    return 0;
}

std::uint16_t PeerSyncHook::get_object_id_from_server_id(int serverId) const
{
    std::shared_lock lock(mutex_);
    const auto it = peerServerIdToObjectId_.find(serverId);
    if (it != peerServerIdToObjectId_.end())
    {
        return it->second;
    }
    return 0;
}

bool PeerSyncHook::request_recreate_peer_ped(int pedHandle, int serverId)
{
    if (!initialized_ || !g_ppTheClones || !*g_ppTheClones)
    {
        return false;
    }

    const auto now = GetTickCount64();
    {
        std::scoped_lock lock(mutex_);
        auto& state = recreateStates_[serverId];
        if (state.firstMismatchTime == 0)
        {
            state.firstMismatchTime = now;
            return false;
        }
        if (now - state.firstMismatchTime < 2500)
        {
            return false;
        }
        if (now - state.lastRecreateTime < 4000)
        {
            return false;
        }
        if (state.attempts >= 3)
        {
            return false;
        }
        state.lastRecreateTime = now;
        state.attempts++;
    }

    std::uint16_t objectId = 0;
    const auto netId = native::invoke(native::kPedToNet, { static_cast<std::uint64_t>(pedHandle) });
    if (netId > 0 && netId <= 0xFFFF)
    {
        objectId = static_cast<std::uint16_t>(netId);
    }

    if (objectId == 0)
    {
        const auto altNetId = native::invoke(native::kNetworkGetNetworkIdFromEntity, { static_cast<std::uint64_t>(pedHandle) });
        if (altNetId > 0 && altNetId <= 0xFFFF)
        {
            objectId = static_cast<std::uint16_t>(altNetId);
        }
    }

    if (objectId == 0)
    {
        log_error("PeerSyncHook: cannot resolve objectId for serverId=" + std::to_string(serverId));
        return false;
    }

    register_peer_object_id(objectId, serverId);
    log("PeerSyncHook: triggering recreation of peer ped serverId=" + std::to_string(serverId) +
        " objectId=" + std::to_string(objectId));
    delete_clone_object(objectId);
    return true;
}

void PeerSyncHook::on_peer_model_verified(int serverId)
{
    std::scoped_lock lock(mutex_);
    auto it = recreateStates_.find(serverId);
    if (it != recreateStates_.end())
    {
        it->second.firstMismatchTime = 0;
        it->second.attempts = 0;
    }
}
}
