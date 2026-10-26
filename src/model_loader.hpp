#pragma once

#include "model_catalog.hpp"
#include "overlay.hpp"
#include "network_model_spoof.hpp"
#include "relay_client.hpp"
#include "streaming_bridge.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace modelcustom
{
class ModelLoader
{
public:
    ModelLoader(StreamingBridge& streaming, NetworkModelSpoof& networkSpoof);

    bool register_model(const std::filesystem::path& folder);
    void toggle_enabled();
    void tick();

    overlay::Snapshot ui_snapshot() const;
    bool select_catalog_model(const std::string& modelId);
    bool set_models_directory(std::string_view path, bool persist = true);
    bool reload_models_directory_from_config();
    void reload_from_config();

    bool is_model_blacklisted(std::uint32_t hash) const;
    bool is_model_blacklisted(std::string_view name) const;
    void add_blacklisted_model(const std::string& modelNameOrHash);
    void remove_blacklisted_model(const std::string& modelNameOrHash);
    void reset_blacklisted_models();
    void set_default_enabled(bool enabled);

    bool block_story_models() const { return blockStoryModels_; }
    void set_block_story_models(bool enabled);
    bool custom_server_model_enabled() const { return customServerModelEnabled_; }
    void set_custom_server_model_enabled(bool enabled);
    const std::string& custom_server_model() const { return customServerModel_; }
    void set_custom_server_model(std::string_view model);
    std::uint32_t effective_server_model_hash() const;

private:
    struct ComponentVariation
    {
        int drawable = 0;
        int texture = 0;
        int palette = 0;

        bool operator==(const ComponentVariation&) const = default;
    };

    struct PropVariation
    {
        int drawable = -1;
        int texture = 0;

        bool operator==(const PropVariation&) const = default;
    };

    struct PeerProxy
    {
        int sourcePed = 0;
        int proxyPed = 0;
        std::uint32_t modelHash = 0;
        bool visible = true;
    };

    bool capture_server_outfit(int ped, std::uint32_t modelHash);
    bool restore_server_outfit(int ped, std::uint32_t modelHash);
    bool register_component_pack(const std::filesystem::path& folder);
    bool apply_component_pack(int ped);

    std::optional<std::string> determine_model_name(
        const std::filesystem::path& folder,
        const std::filesystem::path& metadataPath) const;

    std::string resolve_hash_to_display_name(std::uint32_t hash) const;

    void process_peers();
    void update_peer_proxies();
    bool create_peer_proxy(int serverId, int sourcePed, std::uint32_t modelHash,
        std::string_view modelName);
    void remove_peer_proxy(int serverId, bool releaseModel = true);
    bool attach_peer_proxy(PeerProxy& proxy);

    StreamingBridge& streaming_;
    NetworkModelSpoof& networkSpoof_;
    ModelCatalog catalog_;
    RelayClient relayClient_;
    std::set<std::uint32_t> registeredModels_;
    std::uint32_t targetModelHash_ = 0;
    std::uint32_t serverModelHash_ = 0;
    std::string targetModelName_;
    std::string targetPackSha256_;
    std::filesystem::path modelsDirectory_;
    std::uint64_t tagCounter_ = 0;
    unsigned long long requestDeadline_ = 0;
    unsigned long long lastRequestStart_ = 0;
    unsigned long long lastResolverError_ = 0;
    unsigned long long lastSyncReport_ = 0;
    unsigned long long nextOutfitSample_ = 0;
    unsigned long long outfitStableSince_ = 0;
    unsigned long long nextOutfitRestore_ = 0;
    unsigned long long outfitRestoreDeadline_ = 0;
    unsigned long long nextRelayTick_ = 0;
    unsigned long long nextComponentApply_ = 0;
    unsigned long long componentRequestDeadline_ = 0;
    std::uint64_t lastReportedSyncWrites_ = 0;
    std::array<ComponentVariation, 12> serverComponents_{};
    std::array<PropVariation, 13> serverProps_{};
    std::map<int, ComponentVariation> componentPackVariations_;
    std::filesystem::path componentPackFolder_;
    std::string componentPackName_;
    std::optional<std::filesystem::path> pendingComponentPack_;
    std::optional<std::filesystem::path> pendingFullModel_;
    std::map<int, PeerProxy> peerProxies_;
    std::map<int, unsigned long long> peerRetryAfter_;
    std::uint32_t outfitModelHash_ = 0;
    bool requestActive_ = false;
    bool switchAwaitingNetworkObject_ = false;
    bool enabled_ = false;
    bool defaultEnabled_ = false;
    bool restorePending_ = false;
    bool outfitCaptured_ = false;
    bool serverModelObserved_ = false;
    bool relayAnnounced_ = false;
    bool componentPackActive_ = false;
    bool componentPackRestored_ = false;
    bool componentRequestActive_ = false;
    bool componentRestoreActive_ = false;
    bool componentSwitchAwaitingNetworkObject_ = false;
    bool componentNetworkSpoofArmed_ = false;
    std::uint32_t componentTargetModelHash_ = 0;
    bool blockStoryModels_ = true;
    bool customServerModelEnabled_ = false;
    std::string customServerModel_ = "mp_m_freemode_01";
    std::vector<std::string> blacklistedModels_;
    std::set<std::uint32_t> blacklistedHashes_;

    void rebuild_blacklist_hashes();
};
}
