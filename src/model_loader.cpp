#include "model_loader.hpp"

#include "native_api.hpp"
#include "peer_sync_hook.hpp"
#include "util.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace modelcustom
{
namespace
{
constexpr std::array<std::string_view, 5> kPedAssetExtensions = {
    ".ydd", ".yft", ".ytd", ".ymt", ".ycd"
};




constexpr std::array<std::pair<std::string_view, int>, 12> kComponentPrefixes = {{
    { "head", 0 }, { "berd", 1 }, { "hair", 2 }, { "uppr", 3 },
    { "lowr", 4 }, { "hand", 5 }, { "feet", 6 }, { "teef", 7 },
    { "accs", 8 }, { "task", 9 }, { "decl", 10 }, { "jbib", 11 }
}};

bool is_ped_asset(const std::filesystem::path& path)
{
    const auto extension = lowercase(path.extension().string());
    return std::find(kPedAssetExtensions.begin(), kPedAssetExtensions.end(), extension) !=
        kPedAssetExtensions.end();
}

bool is_safe_model_name(std::string_view value)
{
    if (value.empty() || value.size() > 64)
    {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '_';
    });
}

std::optional<std::pair<int, int>> component_drawable_from_file(
    const std::filesystem::path& path)
{
    if (lowercase(path.extension().string()) != ".ydd")
    {
        return std::nullopt;
    }

    const auto stem = lowercase(path.stem().string());
    const auto separator = stem.find('_');
    if (separator == std::string::npos)
    {
        return std::nullopt;
    }

    int componentId = -1;
    for (const auto& [prefix, id] : kComponentPrefixes)
    {
        if (stem.compare(0, separator, prefix) == 0)
        {
            componentId = id;
            break;
        }
    }
    if (componentId < 0)
    {
        return std::nullopt;
    }

    const auto firstDigit = separator + 1;
    if (firstDigit >= stem.size() ||
        !std::isdigit(static_cast<unsigned char>(stem[firstDigit])))
    {
        return std::nullopt;
    }

    int drawable = 0;
    std::size_t cursor = firstDigit;
    while (cursor < stem.size() && std::isdigit(static_cast<unsigned char>(stem[cursor])))
    {
        drawable = drawable * 10 + (stem[cursor] - '0');
        if (drawable > 255)
        {
            return std::nullopt;
        }
        ++cursor;
    }

    return std::pair{ componentId, drawable };
}

bool is_component_pack_folder(const std::filesystem::path& folder)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(folder / L"peds.meta", error))
    {
        return false;
    }

    bool hasComponentYdd = false;
    bool hasFullPedAsset = false;
    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 folder,
                 std::filesystem::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto extension = lowercase(entry.path().extension().string());
            if (extension == ".yft" || extension == ".ymt" || extension == ".ycd")
            {
                hasFullPedAsset = true;
            }
            if (component_drawable_from_file(entry.path()))
            {
                hasComponentYdd = true;
            }
        }
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return false;
    }




    return hasComponentYdd && !hasFullPedAsset;
}
}

ModelLoader::ModelLoader(StreamingBridge& streaming, NetworkModelSpoof& networkSpoof)
    : streaming_(streaming), networkSpoof_(networkSpoof)
{
    defaultEnabled_ = configured_default_enabled();
    enabled_ = defaultEnabled_;
    blockStoryModels_ = configured_block_story_models();
    customServerModelEnabled_ = configured_custom_server_model_enabled();
    customServerModel_ = configured_custom_server_model();
    blacklistedModels_ = configured_blacklisted_models();
    rebuild_blacklist_hashes();

    serverModelHash_ = joaat(configured_replacement_model());
    if (is_model_blacklisted(serverModelHash_))
    {
        serverModelHash_ = 0x705E61F2;
    }

    if (const auto relayUrl = configured_relay_url())
    {
        relayClient_.start(*relayUrl);
    }
    modelsDirectory_ = configured_models_directory().value_or(plugin_directory());
    static_cast<void>(PeerSyncHook::instance().initialize());
}

std::string ModelLoader::resolve_hash_to_display_name(std::uint32_t hash) const
{
    if (hash == 0) return "none";
    if (hash == 0x705E61F2U) return "mp_m_freemode_01";
    if (hash == 0x9C9EFFD8U) return "mp_f_freemode_01";
    if (hash == 0x0D7114C9U) return "player_zero (Michael)";
    if (hash == 0x9B22DBAFU) return "player_one (Franklin)";
    if (hash == 0x9B810FA2U) return "player_two (Trevor)";

    if (hash == targetModelHash_ && !targetModelName_.empty())
    {
        return targetModelName_;
    }

    for (const auto& [id, entry] : catalog_.entries())
    {
        if (joaat(id) == hash)
        {
            return id;
        }
    }

    for (const auto& item : blacklistedModels_)
    {
        if (parse_model_hash(item) == hash)
        {
            return item;
        }
    }

    return "mp_m_freemode_01";
}

std::uint32_t ModelLoader::effective_server_model_hash() const
{
    if (customServerModelEnabled_ && !customServerModel_.empty())
    {
        std::uint32_t hash = parse_model_hash(customServerModel_);
        if (hash == 0) hash = joaat(customServerModel_);
        if (hash != 0)
        {
            if (blockStoryModels_ && is_story_model(hash))
            {
                return 0x705E61F2;
            }
            return hash;
        }
    }

    if (serverModelHash_ != 0 && !registeredModels_.contains(serverModelHash_))
    {
        if (blockStoryModels_ && is_story_model(serverModelHash_))
        {
            return 0x705E61F2;
        }
        return serverModelHash_;
    }

    return 0x705E61F2;
}

overlay::Snapshot ModelLoader::ui_snapshot() const
{
    overlay::Snapshot result;
    result.enabled = enabled_;
    result.defaultEnabled = defaultEnabled_;
    result.blockStoryModels = blockStoryModels_;
    result.customServerModelEnabled = customServerModelEnabled_;
    result.customServerModel = customServerModel_;
    result.pending = requestActive_ || restorePending_ || switchAwaitingNetworkObject_ ||
        componentRequestActive_ || componentRestoreActive_ || componentSwitchAwaitingNetworkObject_;
    result.model = targetModelName_;
    result.modelsPath = wide_to_utf8(modelsDirectory_.wstring());
    result.blacklistedModels = blacklistedModels_;

    std::uint32_t currentLocalHash = 0;
    const auto ped = static_cast<int>(native::invoke(native::kPlayerPedId));
    if (ped != 0 && native::invoke(native::kDoesEntityExist, { static_cast<std::uint64_t>(ped) }) != 0)
    {
        currentLocalHash = static_cast<std::uint32_t>(native::invoke(
            native::kGetEntityModel, { static_cast<std::uint64_t>(ped) }));
    }
    result.currentLocalModel = resolve_hash_to_display_name(currentLocalHash);

    const std::uint32_t safeServerModel = effective_server_model_hash();
    result.spoofedRemoteModel = resolve_hash_to_display_name(safeServerModel);

    for (const auto& [id, entry] : catalog_.entries())
        result.models.push_back(id);
    for (const auto& peer : relayClient_.get_peers())
    {
        const auto* entry = catalog_.find(peer.modelId, peer.packSha256);
        const bool installed = entry != nullptr && peer.modelVersion == kModelPackVersion;
        result.peers.push_back({ peer.serverId, installed ? peer.modelId : "custom", installed });
    }
    return result;
}

void ModelLoader::rebuild_blacklist_hashes()
{
    blacklistedHashes_.clear();
    for (const auto& item : blacklistedModels_)
    {
        const auto hash = parse_model_hash(item);
        if (hash != 0)
        {
            blacklistedHashes_.insert(hash);
        }
    }
}

bool ModelLoader::is_model_blacklisted(std::uint32_t hash) const
{
    if (hash == 0) return false;
    if (blockStoryModels_ && is_story_model(hash)) return true;
    return blacklistedHashes_.contains(hash);
}

bool ModelLoader::is_model_blacklisted(std::string_view name) const
{
    if (name.empty()) return false;
    const auto hash = parse_model_hash(name);
    return is_model_blacklisted(hash);
}

void ModelLoader::add_blacklisted_model(const std::string& modelNameOrHash)
{
    const auto cleaned = lowercase(trim(modelNameOrHash));
    if (cleaned.empty()) return;

    for (const auto& existing : blacklistedModels_)
    {
        if (lowercase(trim(existing)) == cleaned) return;
    }

    blacklistedModels_.push_back(cleaned);
    rebuild_blacklist_hashes();
    save_configured_blacklisted_models(blacklistedModels_);
    log("Added model to blacklist: " + cleaned);
    native::subtitle("~b~ModelCustom~s~ | Blacklisted: ~r~" + cleaned, 3000);
}

void ModelLoader::remove_blacklisted_model(const std::string& modelNameOrHash)
{
    const auto cleaned = lowercase(trim(modelNameOrHash));
    auto it = std::remove_if(blacklistedModels_.begin(), blacklistedModels_.end(),
        [&](const std::string& m) { return lowercase(trim(m)) == cleaned; });
    if (it != blacklistedModels_.end())
    {
        blacklistedModels_.erase(it, blacklistedModels_.end());
        rebuild_blacklist_hashes();
        save_configured_blacklisted_models(blacklistedModels_);
        log("Removed model from blacklist: " + cleaned);
        native::subtitle("~b~ModelCustom~s~ | Removed from blacklist: ~y~" + cleaned, 3000);
    }
}

void ModelLoader::reset_blacklisted_models()
{
    blacklistedModels_ = default_blacklisted_models();
    rebuild_blacklist_hashes();
    save_configured_blacklisted_models(blacklistedModels_);
    log("Blacklist reset to defaults");
    native::subtitle("~b~ModelCustom~s~ | Blacklist reset to default story peds", 3000);
}

void ModelLoader::set_default_enabled(bool enabled)
{
    defaultEnabled_ = enabled;
    save_configured_default_enabled(enabled);
    log("Default enabled changed to: " + std::string(enabled ? "true" : "false"));
}

void ModelLoader::set_block_story_models(bool enabled)
{
    blockStoryModels_ = enabled;
    save_configured_block_story_models(enabled);
    log("Block story models changed to: " + std::string(enabled ? "true" : "false"));
    native::subtitle(enabled
        ? "~b~ModelCustom~s~ | Model filter: ~g~Story models blocked~s~"
        : "~b~ModelCustom~s~ | Model filter: ~y~Story model blocking disabled~s~", 3000);
}

void ModelLoader::set_custom_server_model_enabled(bool enabled)
{
    customServerModelEnabled_ = enabled;
    save_configured_custom_server_model_enabled(enabled);
    log("Custom server model override changed to: " + std::string(enabled ? "true" : "false"));
    native::subtitle(enabled
        ? "~b~ModelCustom~s~ | Server spoof: ~g~Custom model active~s~"
        : "~b~ModelCustom~s~ | Server spoof: ~b~Auto (In-game ped)~s~", 3000);
}

void ModelLoader::set_custom_server_model(std::string_view model)
{
    const auto cleaned = lowercase(trim(std::string(model)));
    if (cleaned.empty()) return;
    customServerModel_ = cleaned;
    save_configured_custom_server_model(cleaned);
    log("Custom server model set to: " + cleaned);
    native::subtitle("~b~ModelCustom~s~ | Spoofed Server Model: ~g~" + cleaned, 3000);
}

void ModelLoader::reload_from_config()
{
    reload_models_directory_from_config();
    defaultEnabled_ = configured_default_enabled();
    blockStoryModels_ = configured_block_story_models();
    customServerModelEnabled_ = configured_custom_server_model_enabled();
    customServerModel_ = configured_custom_server_model();
    blacklistedModels_ = configured_blacklisted_models();
    rebuild_blacklist_hashes();
    log("ModelCustom configuration reloaded from INI");
    native::subtitle("~b~ModelCustom~s~ | Config reloaded", 2500);
}

bool ModelLoader::set_models_directory(std::string_view value, bool persist)
{
    const auto resolved = resolve_user_path(value);
    std::error_code error;
    if (!resolved || !std::filesystem::is_directory(*resolved, error))
    {
        log_error("Models path is not a readable directory: " + std::string(value));
        native::subtitle("~r~ModelCustom~s~ | Models path is not a folder", 3500);
        return false;
    }
    if (persist && !save_configured_models_directory(*resolved))
    {
        log_error("Could not save models_dir to ModelCustom.ini");
        native::subtitle("~r~ModelCustom~s~ | Could not save models path", 3500);
        return false;
    }
    modelsDirectory_ = *resolved;
    catalog_.clear();
    catalog_.scan(modelsDirectory_);
    log("Models directory loaded: " + path_to_engine_string(modelsDirectory_));
    if (persist) native::subtitle("~b~ModelCustom~s~ | Models directory reloaded", 2500);
    return true;
}

bool ModelLoader::reload_models_directory_from_config()
{
    const auto directory = configured_models_directory().value_or(plugin_directory());
    return set_models_directory(wide_to_utf8(directory.wstring()), false);
}

bool ModelLoader::select_catalog_model(const std::string& modelId)
{
    const auto entry = catalog_.entries().find(modelId);
    if (entry == catalog_.entries().end()) return false;

    const auto folder = entry->second.folder;
    if (!register_model(folder)) return false;






    if (!enabled_ && !pendingComponentPack_ && !pendingFullModel_)
    {
        toggle_enabled();
        log("Selected catalog model enabled the local changer: " + modelId);
    }
    return true;
}

bool ModelLoader::capture_server_outfit(int ped, std::uint32_t modelHash)
{
    if (ped == 0 || modelHash == 0 || registeredModels_.contains(modelHash))
    {
        return false;
    }

    std::array<ComponentVariation, 12> components{};
    std::array<PropVariation, 13> props{};

    for (std::size_t componentId = 0; componentId < components.size(); ++componentId)
    {
        auto& component = components[componentId];
        component.drawable = static_cast<int>(native::invoke(
            native::kGetPedDrawableVariation,
            { static_cast<std::uint64_t>(ped), componentId }));
        component.texture = static_cast<int>(native::invoke(
            native::kGetPedTextureVariation,
            { static_cast<std::uint64_t>(ped), componentId }));
        component.palette = static_cast<int>(native::invoke(
            native::kGetPedPaletteVariation,
            { static_cast<std::uint64_t>(ped), componentId }));
    }

    for (std::size_t propId = 0; propId < props.size(); ++propId)
    {
        auto& prop = props[propId];
        prop.drawable = static_cast<int>(native::invoke(
            native::kGetPedPropIndex,
            { static_cast<std::uint64_t>(ped), propId }));
        prop.texture = prop.drawable >= 0
            ? static_cast<int>(native::invoke(
                native::kGetPedPropTextureIndex,
                { static_cast<std::uint64_t>(ped), propId }))
            : 0;
    }

    const bool changed = !outfitCaptured_ || outfitModelHash_ != modelHash ||
        serverComponents_ != components || serverProps_ != props;

    serverComponents_ = components;
    serverProps_ = props;
    serverModelHash_ = modelHash;
    outfitModelHash_ = modelHash;
    outfitCaptured_ = true;
    serverModelObserved_ = true;
    if (changed)
    {
        log("Captured changed local server outfit for model=" + to_hex(modelHash));
    }
    return changed;
}

bool ModelLoader::restore_server_outfit(int ped, std::uint32_t modelHash)
{
    if (!outfitCaptured_ || outfitModelHash_ != modelHash)
    {



        return false;
    }

    for (std::size_t componentId = 0; componentId < serverComponents_.size(); ++componentId)
    {
        const auto& component = serverComponents_[componentId];
        native::invoke_void(native::kSetPedComponentVariation, {
            static_cast<std::uint64_t>(ped),
            componentId,
            static_cast<std::uint64_t>(component.drawable),
            static_cast<std::uint64_t>(component.texture),
            static_cast<std::uint64_t>(component.palette)
        });
    }

    for (std::size_t propId = 0; propId < serverProps_.size(); ++propId)
    {
        const auto& prop = serverProps_[propId];
        if (prop.drawable < 0)
        {
            native::invoke_void(native::kClearPedProp, {
                static_cast<std::uint64_t>(ped), propId });
        }
        else
        {
            native::invoke_void(native::kSetPedPropIndex, {
                static_cast<std::uint64_t>(ped),
                propId,
                static_cast<std::uint64_t>(prop.drawable),
                static_cast<std::uint64_t>(prop.texture),
                1
            });
        }
    }

    return true;
}

bool ModelLoader::apply_component_pack(int ped)
{
    if (!componentPackActive_ || ped == 0 || componentPackVariations_.empty())
    {
        return false;
    }

    for (const auto& [componentId, variation] : componentPackVariations_)
    {
        native::invoke_void(native::kSetPedComponentVariation, {
            static_cast<std::uint64_t>(ped),
            static_cast<std::uint64_t>(componentId),
            static_cast<std::uint64_t>(variation.drawable),
            static_cast<std::uint64_t>(variation.texture),
            static_cast<std::uint64_t>(variation.palette)
        });
    }
    return true;
}

bool ModelLoader::register_component_pack(const std::filesystem::path& requestedFolder)
{
    std::error_code error;
    const auto folder = std::filesystem::absolute(requestedFolder, error);
    if (error || !std::filesystem::is_directory(folder, error))
    {
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Component folder is not valid", 4000);
        return false;
    }

    const auto modelName = resolve_model_name(folder);
    if (!modelName || !is_safe_model_name(*modelName))
    {
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Invalid component pack name", 4000);
        return false;
    }

    if (is_model_blacklisted("player_one") || is_model_blacklisted(*modelName))
    {
        log_error("Cannot register component pack " + *modelName + ": required player_one or name is blacklisted");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Component base is blacklisted!", 4000);
        return false;
    }

    if (componentPackActive_ && componentPackFolder_ == folder)
    {
        enabled_ = true;
        componentPackRestored_ = false;
        return true;
    }

    const auto ped = static_cast<int>(native::invoke(native::kPlayerPedId));
    const auto currentModel = (ped != 0 && native::invoke(native::kDoesEntityExist, {
        static_cast<std::uint64_t>(ped) }) != 0)
        ? static_cast<std::uint32_t>(native::invoke(
            native::kGetEntityModel, { static_cast<std::uint64_t>(ped) }))
        : 0U;





    if (currentModel != 0 && registeredModels_.contains(currentModel))
    {
        pendingComponentPack_ = folder;
        if (enabled_)
        {
            toggle_enabled();
        }
        else
        {
            native::subtitle("~y~~h~ModelCustom~h~~s~ ~c~|~s~ Restoring server ped before component pack...", 3500);
        }
        return true;
    }

    if (ped == 0 || currentModel == 0)
    {
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Player ped is not ready", 4000);
        return false;
    }

    if (componentPackActive_)
    {
        static_cast<void>(restore_server_outfit(ped, outfitModelHash_));
        componentPackActive_ = false;
        componentPackRestored_ = false;
        componentPackVariations_.clear();
    }

    static_cast<void>(capture_server_outfit(ped, currentModel));

    struct Asset
    {
        std::filesystem::path path;
        ResourceFlags flags{};
    };

    std::vector<Asset> assets;
    std::map<int, ComponentVariation> variations;
    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 folder,
                 std::filesystem::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file() || !is_ped_asset(entry.path()))
            {
                continue;
            }
            if (assets.size() >= 1024)
            {
                native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Too many component assets", 4000);
                return false;
            }

            ResourceFlags flags{};
            if (!read_rsc7_flags(entry.path(), flags))
            {
                log_error("Skipping non-RSC7 component asset: " + path_to_engine_string(entry.path()));
                continue;
            }
            assets.push_back({ entry.path(), flags });

            if (const auto component = component_drawable_from_file(entry.path()))
            {
                const auto [componentId, drawable] = *component;
                auto& variation = variations[componentId];
                if (drawable >= variation.drawable)
                {
                    variation.drawable = drawable;
                    variation.texture = 0;
                    variation.palette = 0;
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& exception)
    {
        log_error(std::string("Could not enumerate component folder: ") + exception.what());
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Failed to read component folder", 4000);
        return false;
    }

    if (assets.empty() || variations.empty())
    {
        log_error("Component pack requires valid RSC7 clothing .ydd assets");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: No valid clothing components found", 4000);
        return false;
    }

    std::sort(assets.begin(), assets.end(), [](const Asset& left, const Asset& right)
    {
        return left.path.native() < right.path.native();
    });

    const auto name = lowercase(*modelName);
    const auto tag = "modelcustom_components_" + std::to_string(GetTickCount64()) + "_" +
        std::to_string(++tagCounter_);
    for (const auto& asset : assets)
    {
        streaming_.add_streaming_file(tag, path_to_engine_string(asset.path), asset.flags);
        log("Registered component asset: " + path_to_engine_string(asset.path));
    }

    targetModelHash_ = 0;
    targetModelName_ = name;
    targetPackSha256_ = compute_model_pack_sha256(folder);
    requestActive_ = false;
    switchAwaitingNetworkObject_ = false;
    restorePending_ = false;
    networkSpoof_.disarm();
    relayClient_.set_active_model("none", kModelPackVersion, "", false);

    componentPackVariations_ = std::move(variations);
    componentPackFolder_ = folder;
    componentPackName_ = name;
    componentPackActive_ = true;
    componentPackRestored_ = false;
    nextComponentApply_ = 0;
    componentRequestDeadline_ = 0;
    componentRequestActive_ = false;
    componentRestoreActive_ = false;
    componentSwitchAwaitingNetworkObject_ = false;
    componentNetworkSpoofArmed_ = false;
    componentTargetModelHash_ = joaat("player_one");
    enabled_ = true;
    pendingComponentPack_.reset();

    catalog_.register_explicit_model(folder, name, targetPackSha256_);
    log("Registered local component pack " + name + " with " +
        std::to_string(componentPackVariations_.size()) + " clothing components; model hash unchanged");
    native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Components: ~g~~h~" + name + "~h~~s~", 4500);
    return true;
}

bool ModelLoader::register_model(const std::filesystem::path& requestedFolder)
{
    std::error_code error;
    const auto folder = std::filesystem::absolute(requestedFolder, error);
    if (error || !std::filesystem::is_directory(folder, error))
    {
        log_error("Selected path is not a directory");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Selected path is not a folder", 4000);
        return false;
    }

    if (is_component_pack_folder(folder))
    {
        return register_component_pack(folder);
    }

    if (componentPackActive_)
    {
        const auto ped = static_cast<int>(native::invoke(native::kPlayerPedId));
        const auto currentModel = (ped != 0)
            ? static_cast<std::uint32_t>(native::invoke(
                native::kGetEntityModel, { static_cast<std::uint64_t>(ped) }))
            : 0U;
        if (enabled_ || currentModel == componentTargetModelHash_ || !componentPackRestored_)
        {
            pendingFullModel_ = folder;
            if (enabled_)
            {
                toggle_enabled();
            }
            else
            {
                componentRestoreActive_ = false;
                componentPackRestored_ = false;
            }
            return true;
        }
        if (ped != 0)
        {
            static_cast<void>(restore_server_outfit(ped, outfitModelHash_));
        }
        componentPackActive_ = false;
        componentPackRestored_ = false;
        componentPackVariations_.clear();
        componentTargetModelHash_ = 0;
        componentRequestActive_ = false;
        componentRestoreActive_ = false;
        componentSwitchAwaitingNetworkObject_ = false;
        componentNetworkSpoofArmed_ = false;
    }

    const auto metadataPath = folder / L"peds.meta";
    if (!std::filesystem::is_regular_file(metadataPath, error))
    {
        log_error("peds.meta was not found in " + path_to_engine_string(folder));
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Missing peds.meta file", 4000);
        return false;
    }

    const auto modelName = determine_model_name(folder, metadataPath);
    if (!modelName || !is_safe_model_name(*modelName))
    {
        log_error("Could not determine a safe model name from model.txt or peds.meta");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Invalid model name", 4000);
        return false;
    }

    struct Asset
    {
        std::filesystem::path path;
        ResourceFlags flags{};
    };

    std::vector<Asset> assets;
    bool hasYft = false;
    bool hasYdd = false;
    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 folder,
                 std::filesystem::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file() || !is_ped_asset(entry.path()))
            {
                continue;
            }

            if (assets.size() >= 1024)
            {
                log_error("Model folder contains more than 1024 streaming assets");
                native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Too many streaming assets (>1024)", 4000);
                return false;
            }

            ResourceFlags flags{};
            if (!read_rsc7_flags(entry.path(), flags))
            {
                log_error("Skipping non-RSC7 asset: " + path_to_engine_string(entry.path()));
                continue;
            }

            const auto extension = lowercase(entry.path().extension().string());
            hasYft = hasYft || extension == ".yft";
            hasYdd = hasYdd || extension == ".ydd";
            assets.push_back({ entry.path(), flags });
        }
    }
    catch (const std::filesystem::filesystem_error& exception)
    {
        log_error(std::string("Could not enumerate model folder: ") + exception.what());
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Failed to read model folder", 4000);
        return false;
    }

    if (assets.empty() || !hasYft || !hasYdd)
    {
        log_error("A ped model requires valid RSC7 .yft and .ydd assets");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Valid .yft and .ydd assets required", 4000);
        return false;
    }

    std::sort(assets.begin(), assets.end(), [](const Asset& left, const Asset& right)
    {
        return left.path.native() < right.path.native();
    });

    const auto modelHash = joaat(*modelName);
    if (is_model_blacklisted(modelHash) || is_model_blacklisted(*modelName))
    {
        log_error("Cannot load model " + *modelName + ": model is in the blacklist");
        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Model is blacklisted!", 4000);
        return false;
    }

    if (!registeredModels_.contains(modelHash))
    {
        const auto tag = "modelcustom_" + std::to_string(GetTickCount64()) + "_" +
            std::to_string(++tagCounter_);
        for (const auto& asset : assets)
        {
            streaming_.add_streaming_file(
                tag,
                path_to_engine_string(asset.path),
                asset.flags);
            log("Registered add-on ped asset: " + path_to_engine_string(asset.path));
        }



        streaming_.add_ped_metadata(path_to_engine_string(metadataPath));
        registeredModels_.insert(modelHash);
    }

    targetModelHash_ = modelHash;
    targetModelName_ = lowercase(*modelName);
    targetPackSha256_ = compute_model_pack_sha256(folder);
    requestActive_ = false;
    switchAwaitingNetworkObject_ = false;
    nextOutfitSample_ = 0;
    outfitStableSince_ = 0;

    catalog_.register_explicit_model(folder, targetModelName_, targetPackSha256_);

    relayClient_.set_active_model(targetModelName_, kModelPackVersion, targetPackSha256_, enabled_);

    log("Registered full add-on ped " + targetModelName_ + " (" +
        to_hex(targetModelHash_) + ") sha=" + targetPackSha256_.substr(0, 12) +
        "...; waiting for a network-safe local switch");
    if (enabled_)
    {
        native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Model Registered: ~g~~h~" + targetModelName_ + "~h~~s~", 4500);
    }
    return true;
}

void ModelLoader::toggle_enabled()
{
    if (componentPackActive_)
    {
        enabled_ = !enabled_;
        relayClient_.set_active_model("none", kModelPackVersion, "", false);
        if (enabled_)
        {
            componentPackRestored_ = false;
            componentRestoreActive_ = false;
            componentRequestActive_ = false;
            componentSwitchAwaitingNetworkObject_ = false;
            componentNetworkSpoofArmed_ = false;
            nextComponentApply_ = 0;
            log("Local component pack enabled with F5: " + componentPackName_);
            native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Components: ~g~~h~ENABLED~h~~s~", 3500);
        }
        else
        {
            componentRequestActive_ = false;
            componentSwitchAwaitingNetworkObject_ = false;
            componentRestoreActive_ = false;
            componentPackRestored_ = false;
            log("Local component pack disabled with F5: " + componentPackName_);
            native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Components: ~y~~h~DISABLED~h~~s~", 3500);
        }
        return;
    }

    enabled_ = !enabled_;
    requestActive_ = false;
    switchAwaitingNetworkObject_ = false;
    lastRequestStart_ = 0;
    relayClient_.set_active_model(targetModelName_, kModelPackVersion, targetPackSha256_, enabled_);

    if (enabled_)
    {
        restorePending_ = false;
        nextOutfitSample_ = 0;
        outfitStableSince_ = 0;
        nextOutfitRestore_ = 0;
        outfitRestoreDeadline_ = 0;
        log("ModelCustom enabled with F5");
        native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Model: ~g~~h~ENABLED~h~~s~", 3500);
    }
    else
    {



        restorePending_ = true;
        nextOutfitRestore_ = 0;
        outfitRestoreDeadline_ = 0;

        log("ModelCustom local model disabled with F5; restoring server model " +
            to_hex(serverModelHash_));
        native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Model: ~y~~h~DISABLED~h~~s~", 3500);
    }
}

void ModelLoader::process_peers()
{
    const auto peers = relayClient_.get_peers();
    const auto localPlayer = native::invoke(native::kPlayerId);
    const int localServerId = native::get_player_server_id(static_cast<int>(localPlayer));
    std::set<int> desiredPeerServerIds;

    for (const auto& peer : peers)
    {
        if (peer.serverId <= 0 || (localServerId > 0 && peer.serverId == localServerId))
        {
            continue;
        }

        if (peer.modelId == "none" || peer.modelId.empty() ||
            peer.modelVersion != kModelPackVersion)
        {
            continue;
        }

        const auto* entry = catalog_.find(peer.modelId, peer.packSha256);
        if (!entry)
        {
            continue;
        }
        desiredPeerServerIds.insert(peer.serverId);

        if (!catalog_.ensure_streaming_registered(peer.modelId, peer.packSha256, streaming_))
        {
            continue;
        }

        if (!streaming_.request_model(entry->modelHash))
        {
            log_error("Could not request peer model through gta-streaming-five: " + peer.modelId);
            continue;
        }
        const bool isModelLoaded = (native::invoke(native::kHasModelLoaded, { entry->modelHash }) != 0);
        if (!isModelLoaded)
        {
            continue;
        }

        if (PeerSyncHook::instance().is_ready())
        {
            PeerSyncHook::instance().set_peer_model(peer.serverId, entry->modelHash);

            const int player = native::resolve_player_from_server_id(peer.serverId);
            const int ped = (player >= 0) ? native::get_player_ped(player) : 0;
            if (ped != 0 && native::invoke(native::kDoesEntityExist, {
                    static_cast<std::uint64_t>(ped) }) != 0)
            {
                const auto currentModel = static_cast<std::uint32_t>(native::invoke(
                    native::kGetEntityModel, { static_cast<std::uint64_t>(ped) }));
                if (currentModel == entry->modelHash)
                {
                    PeerSyncHook::instance().on_peer_model_verified(peer.serverId);
                }
                else
                {
                    PeerSyncHook::instance().request_recreate_peer_ped(ped, peer.serverId);
                }
            }

            if (peerProxies_.contains(peer.serverId))
            {
                remove_peer_proxy(peer.serverId, false);
            }
        }
        else
        {
            const int player = native::resolve_player_from_server_id(peer.serverId);
            const int ped = (player >= 0) ? native::get_player_ped(player) : 0;
            if (ped == 0 || native::invoke(native::kDoesEntityExist, {
                    static_cast<std::uint64_t>(ped) }) == 0)
            {
                remove_peer_proxy(peer.serverId);
                continue;
            }

            const auto existing = peerProxies_.find(peer.serverId);
            if (existing != peerProxies_.end() &&
                (existing->second.sourcePed != ped || existing->second.modelHash != entry->modelHash ||
                    native::invoke(native::kDoesEntityExist, {
                        static_cast<std::uint64_t>(existing->second.proxyPed) }) == 0))
            {
                remove_peer_proxy(peer.serverId);
            }

            if (!peerProxies_.contains(peer.serverId))
            {
                const auto retry = peerRetryAfter_.find(peer.serverId);
                if (retry != peerRetryAfter_.end() && GetTickCount64() < retry->second)
                {
                    continue;
                }

                if (create_peer_proxy(peer.serverId, ped, entry->modelHash, peer.modelId))
                {
                    peerRetryAfter_.erase(peer.serverId);
                }
                else
                {
                    peerRetryAfter_[peer.serverId] = GetTickCount64() + 5000;
                }
            }
        }
    }

    if (PeerSyncHook::instance().is_ready())
    {
        for (const int trackedServerId : PeerSyncHook::instance().get_tracked_peer_server_ids())
        {
            if (!desiredPeerServerIds.contains(trackedServerId))
            {
                PeerSyncHook::instance().remove_and_recreate_peer(trackedServerId);
            }
        }
    }

    std::vector<int> stalePeers;
    for (const auto& [serverId, proxy] : peerProxies_)
    {
        static_cast<void>(proxy);
        if (!desiredPeerServerIds.contains(serverId))
        {
            stalePeers.push_back(serverId);
        }
    }
    for (const int serverId : stalePeers)
    {
        remove_peer_proxy(serverId);
        peerRetryAfter_.erase(serverId);
    }
}

bool ModelLoader::attach_peer_proxy(PeerProxy& proxy)
{
    if (proxy.sourcePed == 0 || proxy.proxyPed == 0)
    {
        return false;
    }

    native::invoke_void(native::kAttachEntityToEntity, {
        static_cast<std::uint64_t>(proxy.proxyPed),
        static_cast<std::uint64_t>(proxy.sourcePed),
        static_cast<std::uint64_t>(-1),
        native::float_arg(0.0F), native::float_arg(0.0F), native::float_arg(0.0F),
        native::float_arg(0.0F), native::float_arg(0.0F), native::float_arg(0.0F),
        0, 0, 0, 1, 2, 1
    });

    return native::invoke(native::kIsEntityAttachedToEntity, {
        static_cast<std::uint64_t>(proxy.proxyPed),
        static_cast<std::uint64_t>(proxy.sourcePed) }) != 0;
}

bool ModelLoader::create_peer_proxy(
    int serverId,
    int sourcePed,
    std::uint32_t modelHash,
    std::string_view modelName)
{
    if (serverId <= 0 || sourcePed == 0 || modelHash == 0)
    {
        return false;
    }

    native::ScriptContextScope scriptContext;
    if (!scriptContext.ready())
    {
        log_error("Could not borrow a live FiveM script context for peer proxy creation");
        return false;
    }

    native::Vector3 sourcePosition{};
    if (!native::invoke_vector3(native::kGetEntityCoords, {
            static_cast<std::uint64_t>(sourcePed), 1 }, sourcePosition))
    {
        log_error("Could not read source coords for peer serverId=" + std::to_string(serverId));
        return false;
    }

    if (std::abs(sourcePosition.x) < 0.01F &&
        std::abs(sourcePosition.y) < 0.01F &&
        std::abs(sourcePosition.z) < 0.01F)
    {
        log_error("Source coords for peer serverId=" + std::to_string(serverId) + " are at origin (0,0,0)");
        return false;
    }

    const auto headingBits = static_cast<std::uint32_t>(native::invoke(
        native::kGetEntityHeading, { static_cast<std::uint64_t>(sourcePed) }));
    const float sourceHeading = std::bit_cast<float>(headingBits);

    const int proxyPed = static_cast<int>(native::invoke(native::kCreatePed, {
        4,
        modelHash,
        native::float_arg(sourcePosition.x),
        native::float_arg(sourcePosition.y),
        native::float_arg(sourcePosition.z),
        native::float_arg(sourceHeading),
        0,
        0
    }));
    if (proxyPed == 0 || native::invoke(native::kDoesEntityExist, {
            static_cast<std::uint64_t>(proxyPed) }) == 0)
    {
        log_error("Could not create local visual proxy for peer serverId=" +
            std::to_string(serverId));
        return false;
    }

    native::invoke_void(native::kSetEntityAsMissionEntity, {
        static_cast<std::uint64_t>(proxyPed), 1, 1 });
    native::invoke_void(native::kSetEntityCollision, {
        static_cast<std::uint64_t>(proxyPed), 0, 0 });
    native::invoke_void(native::kSetEntityNoCollisionEntity, {
        static_cast<std::uint64_t>(proxyPed), static_cast<std::uint64_t>(sourcePed), 0 });
    native::invoke_void(native::kSetEntityNoCollisionEntity, {
        static_cast<std::uint64_t>(sourcePed), static_cast<std::uint64_t>(proxyPed), 0 });
    native::invoke_void(native::kSetEntityInvincible, {
        static_cast<std::uint64_t>(proxyPed), 1 });
    native::invoke_void(native::kSetBlockingOfNonTemporaryEvents, {
        static_cast<std::uint64_t>(proxyPed), 1 });
    native::invoke_void(native::kSetPedCanRagdoll, {
        static_cast<std::uint64_t>(proxyPed), 0 });
    native::invoke_void(native::kSetPedDefaultComponentVariation, {
        static_cast<std::uint64_t>(proxyPed) });
    native::invoke_void(native::kSetEntityVisible, {
        static_cast<std::uint64_t>(proxyPed), 1, 0 });
    native::invoke_void(native::kResetEntityAlpha, {
        static_cast<std::uint64_t>(proxyPed) });
    native::invoke_void(native::kSetEntityAlpha, {
        static_cast<std::uint64_t>(proxyPed), 255, 0 });

    PeerProxy proxy{
        .sourcePed = sourcePed,
        .proxyPed = proxyPed,
        .modelHash = modelHash,
        .visible = true
    };
    if (!attach_peer_proxy(proxy))
    {
        int handle = proxyPed;
        native::invoke_void(native::kDeletePed, {
            reinterpret_cast<std::uint64_t>(&handle) });
        log_error("Could not attach local visual proxy for peer serverId=" +
            std::to_string(serverId));
        return false;
    }



    native::invoke_void(native::kSetEntityAlpha, {
        static_cast<std::uint64_t>(sourcePed), 0, 0 });

    peerProxies_[serverId] = proxy;
    adhesive_diag(
        "peer.proxy.create",
        "server_id=" + std::to_string(serverId) +
        " source_ped=" + std::to_string(sourcePed) +
        " proxy_ped=" + std::to_string(proxyPed) +
        " model_hash=" + to_hex(modelHash) +
        " networked=0 gta_net_hooks=0");
    log("Peer serverId=" + std::to_string(serverId) +
        " displayed through attached local proxy model " + std::string(modelName));
    return true;
}

void ModelLoader::remove_peer_proxy(int serverId, bool releaseModel)
{
    const auto entry = peerProxies_.find(serverId);
    if (entry == peerProxies_.end())
    {
        return;
    }

    const auto proxy = entry->second;
    if (proxy.sourcePed != 0 && native::invoke(native::kDoesEntityExist, {
            static_cast<std::uint64_t>(proxy.sourcePed) }) != 0)
    {
        native::invoke_void(native::kResetEntityAlpha, {
            static_cast<std::uint64_t>(proxy.sourcePed) });
    }

    if (proxy.proxyPed != 0 && native::invoke(native::kDoesEntityExist, {
            static_cast<std::uint64_t>(proxy.proxyPed) }) != 0)
    {
        native::invoke_void(native::kDetachEntity, {
            static_cast<std::uint64_t>(proxy.proxyPed), 1, 1 });
        native::invoke_void(native::kSetEntityAsMissionEntity, {
            static_cast<std::uint64_t>(proxy.proxyPed), 1, 1 });
        int handle = proxy.proxyPed;
        native::invoke_void(native::kDeletePed, {
            reinterpret_cast<std::uint64_t>(&handle) });
    }

    if (releaseModel && proxy.modelHash != 0)
    {
        native::invoke_void(native::kSetModelAsNoLongerNeeded, { proxy.modelHash });
    }

    adhesive_diag(
        "peer.proxy.delete",
        "server_id=" + std::to_string(serverId) +
        " proxy_ped=" + std::to_string(proxy.proxyPed));
    peerProxies_.erase(entry);
}

void ModelLoader::update_peer_proxies()
{
    native::ScriptContextScope scriptContext;
    if (!scriptContext.ready())
    {
        return;
    }

    std::vector<int> invalidPeers;
    for (auto& [serverId, proxy] : peerProxies_)
    {
        if (proxy.sourcePed == 0 || proxy.proxyPed == 0 ||
            native::invoke(native::kDoesEntityExist, {
                static_cast<std::uint64_t>(proxy.sourcePed) }) == 0 ||
            native::invoke(native::kDoesEntityExist, {
                static_cast<std::uint64_t>(proxy.proxyPed) }) == 0)
        {
            invalidPeers.push_back(serverId);
            continue;
        }

        const bool inVehicle = native::invoke(native::kIsPedInAnyVehicle, {
            static_cast<std::uint64_t>(proxy.sourcePed), 1 }) != 0;
        const bool shouldShowProxy = !inVehicle;
        if (proxy.visible != shouldShowProxy)
        {
            proxy.visible = shouldShowProxy;
            native::invoke_void(native::kSetEntityVisible, {
                static_cast<std::uint64_t>(proxy.proxyPed),
                shouldShowProxy ? 1ULL : 0ULL,
                0
            });
        }

        if (shouldShowProxy)
        {
            if (native::invoke(native::kIsEntityAttachedToEntity, {
                    static_cast<std::uint64_t>(proxy.proxyPed),
                    static_cast<std::uint64_t>(proxy.sourcePed) }) == 0)
            {
                attach_peer_proxy(proxy);
            }

            native::invoke_void(native::kSetEntityVisible, {
                static_cast<std::uint64_t>(proxy.proxyPed), 1, 0 });
            native::invoke_void(native::kSetEntityAlpha, {
                static_cast<std::uint64_t>(proxy.proxyPed), 255, 0 });



            native::invoke_void(native::kSetEntityAlpha, {
                static_cast<std::uint64_t>(proxy.sourcePed), 0, 0 });


            const auto selectedWeapon = static_cast<std::uint32_t>(native::invoke(
                native::kGetSelectedPedWeapon, { static_cast<std::uint64_t>(proxy.sourcePed) }));
            if (selectedWeapon != 0)
            {
                if (native::invoke(native::kHasPedGotWeapon, {
                        static_cast<std::uint64_t>(proxy.proxyPed), selectedWeapon, 0 }) == 0)
                {
                    native::invoke_void(native::kGiveWeaponToPed, {
                        static_cast<std::uint64_t>(proxy.proxyPed), selectedWeapon, 999, 0, 1 });
                }
                native::invoke_void(native::kSetCurrentPedWeapon, {
                    static_cast<std::uint64_t>(proxy.proxyPed), selectedWeapon, 1 });
            }
        }
        else
        {

            native::invoke_void(native::kResetEntityAlpha, {
                static_cast<std::uint64_t>(proxy.sourcePed) });
        }
    }

    for (const int serverId : invalidPeers)
    {
        remove_peer_proxy(serverId);
    }
}


void ModelLoader::tick()
{
    const auto now = GetTickCount64();

    const auto localPlayer = native::invoke(native::kPlayerId);
    const auto localPed = native::invoke(native::kPlayerPedId);
    if (localPed != 0 && native::invoke(native::kDoesEntityExist, { localPed }) != 0)
    {
        const bool isDead = native::invoke(native::kIsEntityDead, { localPed }) != 0;
        const bool inMenu = overlay::captures_input() || native::invoke(native::kIsPauseMenuActive) != 0;

        if (!isDead && !inMenu)
        {

            native::invoke_void(native::kDisablePlayerFiring, { localPlayer, 0 });
            native::invoke_void(native::kEnableControlAction, { 0, 24, 1 });
            native::invoke_void(native::kEnableControlAction, { 0, 140, 1 });
            native::invoke_void(native::kEnableControlAction, { 0, 141, 1 });
            native::invoke_void(native::kEnableControlAction, { 0, 142, 1 });
            native::invoke_void(native::kEnableControlAction, { 0, 257, 1 });

            native::invoke_void(native::kSetPedCombatAttributes, { localPed, 5, 1 });
            native::invoke_void(native::kSetPedCombatAttributes, { localPed, 46, 1 });

            native::invoke_void(native::kSetPlayerControl, { localPlayer, 1, 0 });
            native::invoke_void(native::kSetPedConfigFlag, { localPed, 122, 0 });
            native::invoke_void(native::kSetPedConfigFlag, { localPed, 314, 0 });
            native::invoke_void(native::kSetPedConfigFlag, { localPed, 224, 1 });

            if (native::invoke(native::kHasPedGotWeapon, { localPed, native::kWeaponUnarmed, 0 }) == 0)
            {
                native::invoke_void(native::kGiveWeaponToPed, { localPed, native::kWeaponUnarmed, 0, 0, 0 });
            }
        }
    }

    update_peer_proxies();
    if (now >= nextRelayTick_)
    {
        nextRelayTick_ = now + 500;
        const auto endpoint = native::get_current_server_endpoint();
        const int localServerId = native::get_player_server_id(static_cast<int>(localPlayer));
        relayClient_.update_session(endpoint, localServerId);

        if (localServerId > 0)
        {
            PeerSyncHook::instance().set_local_server_id(localServerId);
            if (localPed != 0)
            {
                const auto netId = native::invoke(native::kPedToNet, { localPed });
                if (netId > 0 && netId <= 0xFFFF)
                {
                    PeerSyncHook::instance().set_local_object_id(static_cast<std::uint16_t>(netId));
                }
            }
            if (!relayAnnounced_)
            {
                relayAnnounced_ = true;
                const auto key = configured_room_key();
                const std::string name = (key && !key->empty()) ? *key : endpoint;
                if (!name.empty())
                {
                    native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Network Relay: ~g~CONNECTED~s~ ~c~(" +
                        name + " • Client ID: " + std::to_string(localServerId) + ")~s~", 4500);
                }
            }
        }

        process_peers();
    }

    if (componentPackActive_)
    {
        const auto ped = static_cast<int>(native::invoke(native::kPlayerPedId));
        if (ped == 0 || native::invoke(native::kDoesEntityExist, {
                static_cast<std::uint64_t>(ped) }) == 0)
        {
            return;
        }

        const auto currentModel = static_cast<std::uint32_t>(native::invoke(
            native::kGetEntityModel, { static_cast<std::uint64_t>(ped) }));

        if (enabled_)
        {




            if (currentModel != componentTargetModelHash_ &&
                !componentRequestActive_ && !componentSwitchAwaitingNetworkObject_ &&
                !componentRestoreActive_ && !registeredModels_.contains(currentModel) &&
                currentModel != 0 && currentModel != serverModelHash_)
            {
                if (!is_model_blacklisted(currentModel))
                {
                    serverModelHash_ = currentModel;
                }
                else
                {
                    log("Ignored blacklisted server model hash: " + to_hex(currentModel));
                }
                static_cast<void>(capture_server_outfit(ped, currentModel));
                networkSpoof_.disarm();
                componentNetworkSpoofArmed_ = false;
            }




            if (componentSwitchAwaitingNetworkObject_ &&
                currentModel != componentTargetModelHash_)
            {
                return;
            }

            if (currentModel != componentTargetModelHash_)
            {
                if (!componentRequestActive_)
                {
                    componentRequestActive_ = true;
                    componentRequestDeadline_ = now + 20000;
                    log("Requesting local Franklin base for component pack " + componentPackName_);
                }

                static_cast<void>(streaming_.request_model(componentTargetModelHash_));
                if (native::invoke(native::kHasModelLoaded, { componentTargetModelHash_ }) == 0)
                {
                    if (now >= componentRequestDeadline_)
                    {
                        componentRequestActive_ = false;
                        log_error("Timed out requesting player_one for component pack " + componentPackName_);
                        native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Franklin base streaming timed out", 4000);
                    }
                    return;
                }

                if (!componentNetworkSpoofArmed_)
                {
                    const uint32_t safeServerModel = effective_server_model_hash();
                    if (!networkSpoof_.prepare_switch(ped, safeServerModel))
                    {
                        if (!networkSpoof_.is_resolver_ready() && now - lastResolverError_ > 5000)
                        {
                            lastResolverError_ = now;
                            log_error("Component model switch withheld: network spoof patterns failed to resolve");
                            native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Network resolver not ready", 4500);
                        }
                        componentRequestActive_ = false;
                        return;
                    }
                    componentNetworkSpoofArmed_ = true;
                }

                const auto player = native::invoke(native::kPlayerId);
                native::invoke_void(native::kSetPlayerModel, { player, componentTargetModelHash_ });
                native::invoke_void(native::kSetModelAsNoLongerNeeded, { componentTargetModelHash_ });
                componentRequestActive_ = false;
                componentSwitchAwaitingNetworkObject_ = true;

                const auto newPed = static_cast<int>(native::invoke(native::kPlayerPedId));
                if (newPed != 0 && networkSpoof_.refresh_local_ped(newPed))
                {
                    componentSwitchAwaitingNetworkObject_ = false;
                    networkSpoof_.finish_switch();
                    log("Local Franklin base constructed for component pack " + componentPackName_);
                    native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Components: ~g~~h~ACTIVE~h~~s~", 4500);
                }
                return;
            }

            if (componentSwitchAwaitingNetworkObject_)
            {
                if (!networkSpoof_.refresh_local_ped(ped))
                {
                    return;
                }
                componentSwitchAwaitingNetworkObject_ = false;
                networkSpoof_.finish_switch();
            }




            if (now >= nextComponentApply_)
            {
                if (apply_component_pack(ped))
                {
                    nextComponentApply_ = now + 500;
                }
            }
        }
        else
        {
            const auto modelToRestore = effective_server_model_hash();

            if (currentModel != modelToRestore)
            {
                if (!componentRestoreActive_)
                {
                    componentRestoreActive_ = true;
                    componentRequestDeadline_ = now + 20000;
                }
                static_cast<void>(streaming_.request_model(modelToRestore));
                if (native::invoke(native::kHasModelLoaded, { modelToRestore }) == 0)
                {
                    if (now >= componentRequestDeadline_)
                    {
                        componentRestoreActive_ = false;
                        log_error("Timed out restoring server model after component pack");
                    }
                    return;
                }

                const auto player = native::invoke(native::kPlayerId);
                native::invoke_void(native::kSetPlayerModel, { player, modelToRestore });
                native::invoke_void(native::kSetModelAsNoLongerNeeded, { modelToRestore });
                const auto restoredPed = static_cast<int>(native::invoke(native::kPlayerPedId));
                if (restoredPed != 0)
                {
                    if (outfitCaptured_ && outfitModelHash_ == modelToRestore)
                    {
                        static_cast<void>(restore_server_outfit(restoredPed, modelToRestore));
                    }
                    else
                    {
                        native::invoke_void(native::kSetPedDefaultComponentVariation, {
                            static_cast<std::uint64_t>(restoredPed) });
                    }
                }
                return;
            }

            if (!componentPackRestored_)
            {
                static_cast<void>(restore_server_outfit(ped, modelToRestore));
                componentPackRestored_ = true;
                componentRestoreActive_ = false;
                componentSwitchAwaitingNetworkObject_ = false;
                componentNetworkSpoofArmed_ = false;
                networkSpoof_.disarm();
                log("Component pack disabled; restored server model=" + to_hex(modelToRestore));

                if (pendingFullModel_)
                {
                    const auto pending = *pendingFullModel_;
                    pendingFullModel_.reset();
                    componentPackActive_ = false;
                    componentPackVariations_.clear();
                    componentTargetModelHash_ = 0;
                    enabled_ = true;
                    static_cast<void>(register_model(pending));
                }
            }
        }
        return;
    }

    if (targetModelHash_ == 0)
    {
        return;
    }

    const auto ped = static_cast<int>(native::invoke(native::kPlayerPedId));
    if (ped == 0 || native::invoke(native::kDoesEntityExist, {
            static_cast<std::uint64_t>(ped) }) == 0)
    {
        return;
    }

    const auto currentModel = static_cast<std::uint32_t>(native::invoke(
        native::kGetEntityModel,
        { static_cast<std::uint64_t>(ped) }));

    if (!enabled_)
    {
        if (currentModel != targetModelHash_)
        {



            if (restorePending_ && outfitRestoreDeadline_ != 0 &&
                currentModel == serverModelHash_)
            {
                if (now >= nextOutfitRestore_)
                {
                    static_cast<void>(restore_server_outfit(ped, currentModel));
                    nextOutfitRestore_ = now + 100;
                }

                if (now < outfitRestoreDeadline_)
                {
                    return;
                }
            }
            else if (!registeredModels_.contains(currentModel) && currentModel != 0)
            {
                if (!is_model_blacklisted(currentModel))
                {
                    serverModelHash_ = currentModel;
                    serverModelObserved_ = true;
                }
                else
                {
                    log("Ignored blacklisted server model during disabled state: " + to_hex(currentModel));
                }
            }

            if (restorePending_)
            {
                restorePending_ = false;
                nextOutfitRestore_ = 0;
                outfitRestoreDeadline_ = 0;
                networkSpoof_.disarm();
                log("ModelCustom disabled; server model active=" +
                    to_hex(serverModelHash_));
                native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Model: ~y~~h~DISABLED~h~~s~", 3000);

                if (pendingComponentPack_)
                {
                    const auto pending = *pendingComponentPack_;
                    pendingComponentPack_.reset();
                    static_cast<void>(register_component_pack(pending));
                }
            }
            return;
        }

        if (!restorePending_)
        {
            return;
        }

        uint32_t modelToRestore = effective_server_model_hash();

        static_cast<void>(streaming_.request_model(modelToRestore));
        if (native::invoke(native::kHasModelLoaded, { modelToRestore }) == 0)
        {
            return;
        }

        const auto player = native::invoke(native::kPlayerId);
        native::invoke_void(native::kSetPlayerModel, { player, modelToRestore });
        native::invoke_void(native::kSetModelAsNoLongerNeeded, { modelToRestore });

        const auto restoredPed = static_cast<int>(native::invoke(native::kPlayerPedId));
        if (restoredPed != 0)
        {
            if (outfitCaptured_ && outfitModelHash_ == modelToRestore)
            {
                static_cast<void>(restore_server_outfit(restoredPed, modelToRestore));
            }
            else
            {
                native::invoke_void(native::kSetPedDefaultComponentVariation, {
                    static_cast<std::uint64_t>(restoredPed)
                });
            }
            native::invoke_void(native::kGiveWeaponToPed, {
                static_cast<std::uint64_t>(restoredPed), native::kWeaponUnarmed, 0, 0, 1
            });
            native::invoke_void(native::kSetCurrentPedWeapon, {
                static_cast<std::uint64_t>(restoredPed), native::kWeaponUnarmed, 1
            });
        }


        nextOutfitRestore_ = now + 100;
        outfitRestoreDeadline_ = now + 1000;
        return;
    }

    if (currentModel == targetModelHash_)
    {




        if (switchAwaitingNetworkObject_ &&
            networkSpoof_.network_model_hash() != 0 &&
            networkSpoof_.refresh_local_ped(ped))
        {
            switchAwaitingNetworkObject_ = false;
            networkSpoof_.finish_switch();
            log("Network-safe custom model switch completed: local=" +
                targetModelName_ + ", remote=" + to_hex(serverModelHash_));
            native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Model: ~g~~h~ACTIVE~h~~s~ ~c~(" +
                targetModelName_ + ")~s~", 4500);
        }

        const auto patchedWrites = networkSpoof_.patched_write_count();
        if (patchedWrites != lastReportedSyncWrites_ && now - lastSyncReport_ > 3000)
        {
            lastSyncReport_ = now;
            lastReportedSyncWrites_ = patchedWrites;
            log("Player sync payload model hashes patched=" +
                std::to_string(patchedWrites));
        }

        return;
    }



    const bool currentIsServerModel =
        !registeredModels_.contains(currentModel) && currentModel != 0;
    if (currentIsServerModel)
    {
        if (!is_model_blacklisted(currentModel))
        {
            serverModelHash_ = currentModel;
            serverModelObserved_ = true;
        }
        else
        {
            log("Ignored blacklisted server model: " + to_hex(currentModel) + "; preserving safe fallback " + to_hex(serverModelHash_));
        }

        if (now >= nextOutfitSample_)
        {
            if (capture_server_outfit(ped, currentModel) || outfitStableSince_ == 0)
            {
                outfitStableSince_ = now;
            }
            nextOutfitSample_ = now + 100;
        }




        constexpr unsigned long long kOutfitSettleMilliseconds = 1500;
        if (!outfitCaptured_ || outfitModelHash_ != currentModel ||
            outfitStableSince_ == 0 || now - outfitStableSince_ < kOutfitSettleMilliseconds)
        {
            return;
        }
    }

    if (!requestActive_)
    {
        if (now - lastRequestStart_ < 1500)
        {
            return;
        }

        requestActive_ = true;
        lastRequestStart_ = now;
        requestDeadline_ = now + 20000;
        log("Requesting local model " + targetModelName_ + "; captured remote hash " +
            to_hex(serverModelHash_));
    }

    static_cast<void>(streaming_.request_model(targetModelHash_));
    if (native::invoke(native::kHasModelLoaded, { targetModelHash_ }) == 0)
    {
        if (now >= requestDeadline_)
        {
            log_error("Timed out requesting model " + targetModelName_);
            native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Model streaming timed out", 4000);
            requestActive_ = false;
        }
        return;
    }

    if (currentIsServerModel && capture_server_outfit(ped, currentModel))
    {


        outfitStableSince_ = now;
        requestActive_ = false;
        return;
    }

    const uint32_t safeServerModel = effective_server_model_hash();
    if (!networkSpoof_.prepare_switch(ped, safeServerModel))
    {
        if (!networkSpoof_.is_resolver_ready())
        {
            if (now - lastResolverError_ > 5000)
            {
                lastResolverError_ = now;
                log_error("Custom model switch withheld: network spoof patterns failed to resolve");
                native::subtitle("~r~~h~ModelCustom~h~~s~ ~c~|~s~ Error: Network spoof resolver not ready", 4500);
            }
            requestActive_ = false;
        }
        return;
    }

    const auto player = native::invoke(native::kPlayerId);
    native::invoke_void(native::kSetPlayerModel, { player, targetModelHash_ });
    native::invoke_void(native::kSetModelAsNoLongerNeeded, { targetModelHash_ });

    const auto newPed = static_cast<int>(native::invoke(native::kPlayerPedId));
    if (newPed != 0)
    {
        native::invoke_void(native::kSetPedDefaultComponentVariation, {
            static_cast<std::uint64_t>(newPed) });
        native::invoke_void(native::kGiveWeaponToPed, {
            static_cast<std::uint64_t>(newPed), native::kWeaponUnarmed, 0, 0, 1 });
        native::invoke_void(native::kSetCurrentPedWeapon, {
            static_cast<std::uint64_t>(newPed), native::kWeaponUnarmed, 1 });
    }

    requestActive_ = false;
    switchAwaitingNetworkObject_ = true;
    if (newPed != 0 && networkSpoof_.refresh_local_ped(newPed))
    {
        switchAwaitingNetworkObject_ = false;
        networkSpoof_.finish_switch();
        log("Network-safe custom model switch completed immediately: local=" +
            targetModelName_ + ", remote=" + to_hex(effective_server_model_hash()));
        native::subtitle("~b~~h~ModelCustom~h~~s~ ~c~|~s~ Local Model: ~g~~h~ACTIVE~h~~s~ ~c~(" +
            targetModelName_ + ")~s~", 4500);
    }
    else
    {
        log("Custom ped constructed; waiting for its local network object");
    }
}

std::optional<std::string> ModelLoader::determine_model_name(
    const std::filesystem::path& folder,
    const std::filesystem::path& metadataPath) const
{
    static_cast<void>(metadataPath);
    return resolve_model_name(folder);
}
}
