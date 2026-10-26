#pragma once

#include "streaming_bridge.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace modelcustom
{
inline constexpr char kModelPackVersion[] = "1.0.0";


std::optional<std::string> resolve_model_name(const std::filesystem::path& folder);

struct ModelEntry
{
    std::string modelId;
    std::filesystem::path folder;
    std::string packSha256;
    std::uint32_t modelHash = 0;
    bool registeredStreaming = false;
};

class ModelCatalog
{
public:
    void scan(const std::filesystem::path& rootDirectory);
    void clear() { entries_.clear(); }
    void register_explicit_model(const std::filesystem::path& folder, const std::string& modelId, const std::string& packSha256);

    const ModelEntry* find(const std::string& modelId, const std::string& packSha256) const;
    bool ensure_streaming_registered(const std::string& modelId, const std::string& packSha256, StreamingBridge& streaming);

    const std::map<std::string, ModelEntry>& entries() const { return entries_; }

private:
    std::map<std::string, ModelEntry> entries_;
    std::uint64_t tagCounter_ = 0;
};
}
