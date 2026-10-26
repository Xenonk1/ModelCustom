#pragma once

#include <Windows.h>

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <vector>

namespace modelcustom
{
class PeerSyncHook
{
public:
    static PeerSyncHook& instance();

    bool initialize();
    bool is_ready() const { return initialized_; }

    bool install_vtable_hook(void** creationVtable);
    void uninstall_vtable_hook();

    void set_local_server_id(int serverId);
    void set_local_object_id(std::uint16_t objectId);
    void set_peer_model(int serverId, std::uint32_t modelHash);
    void remove_peer_model(int serverId);
    void clear_all_peers();
    std::uint32_t get_peer_model(int serverId) const;
    std::vector<int> get_tracked_peer_server_ids() const;

    bool request_recreate_peer_ped(int pedHandle, int serverId);
    bool recreate_peer(int serverId);
    bool remove_and_recreate_peer(int serverId);
    void on_peer_model_verified(int serverId);
    void register_peer_object_id(std::uint16_t objectId, int serverId);
    int get_server_id_from_object_id(std::uint16_t objectId) const;
    std::uint16_t get_object_id_from_server_id(int serverId) const;
    static void delete_clone_object(std::uint16_t objectId);

    std::uint64_t applied_count() const { return appliedCount_.load(std::memory_order_relaxed); }

    using CanApplyFn = bool (__fastcall*)(void* thisptr, void* netObj);
    using CanApplyToObjectFn = bool (__fastcall*)(void* syncTree, void* netObj);

private:
    PeerSyncHook() = default;
    ~PeerSyncHook();

    struct RecreateState
    {
        unsigned long long firstMismatchTime = 0;
        unsigned long long lastRecreateTime = 0;
        int attempts = 0;
    };

    static bool __fastcall Detour_netSyncTree_CanApplyToObject(void* syncTree, void* netObj);
    static bool __fastcall Detour_PlayerCanApply(void* thisptr, void* netObj);
    static bool Detour_CommonCanApply(void* thisptr, void* netObj, CanApplyFn target, bool isPlayerNode);
    static std::uint16_t get_client_id(void* netObj);
    void uninstall_pointer_hook();

    bool initialized_ = false;
    bool pointerHooked_ = false;
    bool canApplyToObjectHooked_ = false;
    bool vtableHooked_ = false;
    void** creationVtable_ = nullptr;
    int localServerId_ = 0;
    std::atomic<std::uint16_t> localObjectId_ = 0;
    std::atomic<std::uint64_t> appliedCount_ = 0;

    int creationModelOffset_ = 0;
    int appearanceModelOffset_ = 0;
    int appearanceHeadBlendOffset_ = 0;

    mutable std::shared_mutex mutex_;
    std::map<int, std::uint32_t> peerModels_;
    std::map<std::uint16_t, int> objectIdToPeerServerId_;
    std::map<int, std::uint16_t> peerServerIdToObjectId_;
    std::map<int, RecreateState> recreateStates_;
};
}
