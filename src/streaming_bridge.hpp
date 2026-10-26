#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace modelcustom
{
struct ResourceFlags
{
    std::uint32_t virtualFlag;
    std::uint32_t physicalFlag;
};

static_assert(sizeof(ResourceFlags) == 8);

class StreamingBridge
{
public:
    bool resolve();
    bool available() const;

    void add_streaming_file(const std::string& tag, const std::string& path, ResourceFlags flags) const;
    void add_ped_metadata(const std::string& path) const;
    bool request_model(std::uint32_t modelHash) const;

private:
    using AddStreamingFileFn = void (*)(const std::string&, const std::string&, ResourceFlags);
    using AddDataFileFn = void (*)(const std::string&, const std::string&);
    using GetStreamingManagerFn = void* (*)();
    using RequestObjectFn = void (*)(void*, std::uint32_t, int);
    using GetArchetypeStreamingModuleFn = void* (*)();
    using GetStreamingIndexForLocalHashKeyFn = std::uint32_t (*)(void*, std::uint32_t);
    using GetArchetypeFromHashKeyFn = void* (*)(std::uint32_t, std::uint32_t&);

    AddStreamingFileFn addStreamingFile_ = nullptr;
    AddDataFileFn addDataFile_ = nullptr;
    GetStreamingManagerFn getStreamingManager_ = nullptr;
    RequestObjectFn requestObject_ = nullptr;
    GetArchetypeStreamingModuleFn getArchetypeStreamingModule_ = nullptr;
    GetStreamingIndexForLocalHashKeyFn getStreamingIndexForLocalHashKey_ = nullptr;
    GetArchetypeFromHashKeyFn getArchetypeFromHashKey_ = nullptr;
    mutable std::unordered_map<std::uint32_t, std::uint32_t> modelStreamingIndices_;
};

bool read_rsc7_flags(const std::filesystem::path& path, ResourceFlags& output);
}
