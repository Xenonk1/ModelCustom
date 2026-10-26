#include "model_catalog.hpp"

#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace modelcustom
{
namespace
{
constexpr std::array<std::string_view, 5> kPedAssetExtensions = {
    ".ydd", ".yft", ".ytd", ".ymt", ".ycd"
};

bool is_ped_asset(const std::filesystem::path& path)
{
    const auto extension = lowercase(path.extension().string());
    return std::find(kPedAssetExtensions.begin(), kPedAssetExtensions.end(), extension) !=
        kPedAssetExtensions.end();
}

constexpr std::string_view kEncodedNamePrefix = "mcx1:";
constexpr std::string_view kNameXorKey = "ModelCustom::name::v1";

bool is_safe_name(std::string_view value)
{
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c)
    {
        return std::isalnum(c) != 0 || c == '_';
    });
}

std::optional<std::string> decode_encoded_name(std::string value)
{
    value = lowercase(trim(std::move(value)));
    if (value.rfind(kEncodedNamePrefix, 0) != 0) return std::nullopt;
    value.erase(0, kEncodedNamePrefix.size());
    if (value.empty() || value.size() > 128 || (value.size() % 2) != 0) return std::nullopt;
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2)
    {
        const auto high = value[i];
        const auto low = value[i + 1];
        const auto nibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = nibble(high), lo = nibble(low);
        if (hi < 0 || lo < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((hi << 4) | lo) ^
            kNameXorKey[decoded.size() % kNameXorKey.size()]);
    }
    decoded = lowercase(trim(std::move(decoded)));
    return is_safe_name(decoded) ? std::optional<std::string>(std::move(decoded)) : std::nullopt;
}

std::optional<std::string> folder_name(const std::filesystem::path& folder)
{
    auto value = lowercase(folder.filename().string());
    for (auto& c : value)
        if (std::isalnum(static_cast<unsigned char>(c)) == 0) c = '_';
    while (value.find("__") != std::string::npos) value.replace(value.find("__"), 2, "_");
    while (!value.empty() && value.front() == '_') value.erase(value.begin());
    while (!value.empty() && value.back() == '_') value.pop_back();
    return is_safe_name(value) ? std::optional<std::string>(std::move(value)) : std::nullopt;
}
}

std::optional<std::string> resolve_model_name(const std::filesystem::path& folder)
{

    for (const auto& fileName : { L"models.txt", L"model.txt" })
    {
        if (const auto contents = read_text_file(folder / fileName))
        {
            auto firstLine = trim(*contents);
            const auto newline = firstLine.find_first_of("\r\n");
            if (newline != std::string::npos) firstLine = trim(firstLine.substr(0, newline));
            if (auto decoded = decode_encoded_name(firstLine)) return decoded;

            if (std::wstring_view(fileName) == L"model.txt" && is_safe_name(lowercase(firstLine)))
                return lowercase(std::move(firstLine));
        }
    }



    if (auto name = folder_name(folder)) return name;

    const auto metadataPath = folder / L"peds.meta";
    if (const auto metadata = read_text_file(metadataPath))
    {
        if (auto value = extract_xml_tag(*metadata, "Name"))
        {
            return lowercase(std::move(*value));
        }
    }

    return std::nullopt;
}

void ModelCatalog::scan(const std::filesystem::path& rootDirectory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(rootDirectory, ec))
    {
        return;
    }

    log("Scanning model catalog in: " + path_to_engine_string(rootDirectory));

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(rootDirectory, ec))
        {
            if (!entry.is_directory(ec))
            {
                continue;
            }

            const auto folder = entry.path();
            const auto name = resolve_model_name(folder);
            if (!name)
            {
                continue;
            }

            const auto packSha = compute_model_pack_sha256(folder);
            if (packSha.empty())
            {
                continue;
            }

            const auto modelHash = joaat(*name);
            entries_[*name] = ModelEntry{
                .modelId = *name,
                .folder = folder,
                .packSha256 = packSha,
                .modelHash = modelHash,
                .registeredStreaming = false
            };

            log("Catalog discovered model: id=" + *name + " (" + to_hex(modelHash) +
                ") sha=" + packSha.substr(0, 12) + "... in " + path_to_engine_string(folder));
        }
    }
    catch (const std::exception& ex)
    {
        log_error(std::string("ModelCatalog scan error: ") + ex.what());
    }
}

void ModelCatalog::register_explicit_model(
    const std::filesystem::path& folder,
    const std::string& modelId,
    const std::string& packSha256)
{
    const auto name = lowercase(modelId);
    entries_[name] = ModelEntry{
        .modelId = name,
        .folder = folder,
        .packSha256 = packSha256,
        .modelHash = joaat(name),
        .registeredStreaming = true
    };
}

const ModelEntry* ModelCatalog::find(const std::string& modelId, const std::string& packSha256) const
{
    const auto it = entries_.find(lowercase(modelId));
    if (it == entries_.end() || packSha256.empty() ||
        it->second.packSha256 != lowercase(packSha256))
    {
        return nullptr;
    }
    return &it->second;
}

bool ModelCatalog::ensure_streaming_registered(
    const std::string& modelId,
    const std::string& packSha256,
    StreamingBridge& streaming)
{
    if (!find(modelId, packSha256))
    {
        return false;
    }
    auto it = entries_.find(lowercase(modelId));
    if (it == entries_.end())
    {
        return false;
    }

    if (it->second.registeredStreaming)
    {
        return true;
    }

    struct Asset
    {
        std::filesystem::path path;
        ResourceFlags flags{};
    };

    std::vector<Asset> assets;
    try
    {
        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(
                 it->second.folder, std::filesystem::directory_options::skip_permission_denied))
        {
            if (!dirEntry.is_regular_file() || !is_ped_asset(dirEntry.path()))
            {
                continue;
            }

            ResourceFlags flags{};
            if (!read_rsc7_flags(dirEntry.path(), flags))
            {
                continue;
            }

            assets.push_back({ dirEntry.path(), flags });
        }
    }
    catch (...)
    {
        return false;
    }

    if (assets.empty())
    {
        return false;
    }

    const auto tag = "catalog_" + it->second.modelId + "_" + std::to_string(++tagCounter_);
    for (const auto& asset : assets)
    {
        streaming.add_streaming_file(tag, path_to_engine_string(asset.path), asset.flags);
    }

    const auto metaPath = it->second.folder / L"peds.meta";
    std::error_code ec;
    if (std::filesystem::is_regular_file(metaPath, ec))
    {
        streaming.add_ped_metadata(path_to_engine_string(metaPath));
    }

    it->second.registeredStreaming = true;
    log("Dynamically registered peer model assets for " + it->second.modelId);
    return true;
}
}
