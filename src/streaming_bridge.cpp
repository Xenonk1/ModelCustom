#include "streaming_bridge.hpp"

#include "util.hpp"

#include <Windows.h>

#include <cstring>
#include <fstream>
#include <limits>

namespace modelcustom
{
namespace
{
constexpr std::uint32_t kRsc7Magic = 0x37435352U;

#pragma pack(push, 1)
struct Rsc7Header
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t virtualFlag;
    std::uint32_t physicalFlag;
};
#pragma pack(pop)

static_assert(sizeof(Rsc7Header) == 16);

constexpr char kAddStreamingExport[] =
    "?CfxCollection_AddStreamingFileByTag@@YAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0UResourceFlags@rage@@@Z";
constexpr char kAddDataFileExport[] =
    "?AddDataFileToLoadList@streaming@@YAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z";
constexpr char kGetStreamingManagerExport[] =
    "?GetInstance@Manager@streaming@@SAPEAV12@XZ";
constexpr char kRequestObjectExport[] =
    "?RequestObject@Manager@streaming@@QEAAXIH@Z";
constexpr char kGetArchetypeStreamingModuleExport[] =
    "?GetStreamingModule@fwArchetypeManager@rage@@SAPEAVstrStreamingModule@streaming@@XZ";
constexpr char kGetStreamingIndexForLocalHashKeyExport[] =
    "?GetStreamingIndexForLocalHashKey@streaming@@YAIPEAVstrStreamingModule@1@I@Z";
constexpr char kGetArchetypeFromHashKeyExport[] =
    "?GetArchetypeFromHashKey@fwArchetypeManager@rage@@SAPEAVfwArchetype@2@IAEAUfwModelId@2@@Z";
}

bool StreamingBridge::resolve()
{
    const HMODULE module = GetModuleHandleW(L"gta-streaming-five.dll");
    if (!module)
    {
        log_error("gta-streaming-five.dll is not loaded");
        return false;
    }

    addStreamingFile_ = reinterpret_cast<AddStreamingFileFn>(GetProcAddress(module, kAddStreamingExport));
    addDataFile_ = reinterpret_cast<AddDataFileFn>(GetProcAddress(module, kAddDataFileExport));
    getStreamingManager_ = reinterpret_cast<GetStreamingManagerFn>(
        GetProcAddress(module, kGetStreamingManagerExport));
    requestObject_ = reinterpret_cast<RequestObjectFn>(
        GetProcAddress(module, kRequestObjectExport));
    getArchetypeStreamingModule_ = reinterpret_cast<GetArchetypeStreamingModuleFn>(
        GetProcAddress(module, kGetArchetypeStreamingModuleExport));
    getStreamingIndexForLocalHashKey_ = reinterpret_cast<GetStreamingIndexForLocalHashKeyFn>(
        GetProcAddress(module, kGetStreamingIndexForLocalHashKeyExport));
    getArchetypeFromHashKey_ = reinterpret_cast<GetArchetypeFromHashKeyFn>(
        GetProcAddress(module, kGetArchetypeFromHashKeyExport));

    if (!available())
    {
        log_error("Required gta-streaming-five exports were not found");
        return false;
    }

    return true;
}

bool StreamingBridge::available() const
{
    return addStreamingFile_ != nullptr && addDataFile_ != nullptr &&
        getStreamingManager_ != nullptr && requestObject_ != nullptr &&
        getArchetypeStreamingModule_ != nullptr &&
        getStreamingIndexForLocalHashKey_ != nullptr && getArchetypeFromHashKey_ != nullptr;
}

void StreamingBridge::add_streaming_file(
    const std::string& tag,
    const std::string& path,
    ResourceFlags flags) const
{
    addStreamingFile_(tag, path, flags);
}

void StreamingBridge::add_ped_metadata(const std::string& path) const
{
    static const std::string kPedMetadataType = "PED_METADATA_FILE";
    addDataFile_(kPedMetadataType, path);
}

bool StreamingBridge::request_model(std::uint32_t modelHash) const
{
    if (modelHash == 0 || !getStreamingManager_ || !requestObject_ ||
        !getArchetypeStreamingModule_ || !getStreamingIndexForLocalHashKey_ ||
        !getArchetypeFromHashKey_)
    {
        return false;
    }

    void* manager = getStreamingManager_();
    void* module = getArchetypeStreamingModule_();
    if (!manager || !module)
    {
        return false;
    }

    if (const auto cached = modelStreamingIndices_.find(modelHash);
        cached != modelStreamingIndices_.end())
    {
        requestObject_(manager, cached->second, 0);
        return true;
    }




    std::uint32_t baseIndex = 0;
    std::memcpy(&baseIndex, static_cast<const std::uint8_t*>(module) + sizeof(void*),
        sizeof(baseIndex));




    std::uint32_t modelId = 0x0FFFFFFFU;
    const void* archetype = getArchetypeFromHashKey_(modelHash, modelId);
    std::uint32_t localIndex = modelId & 0xFFFFU;
    if (!archetype || localIndex == 0xFFFFU)
    {
        localIndex = getStreamingIndexForLocalHashKey_(module, modelHash);
    }
    if (localIndex == std::numeric_limits<std::uint32_t>::max() || localIndex == 0xFFFFU)
    {
        return false;
    }

    const auto globalIndex = baseIndex + localIndex;
    modelStreamingIndices_.emplace(modelHash, globalIndex);
    requestObject_(manager, globalIndex, 0);
    log("Resolved model streaming request: model=" + to_hex(modelHash) +
        " local_index=" + std::to_string(localIndex) +
        " global_index=" + std::to_string(globalIndex));
    return true;
}

bool read_rsc7_flags(const std::filesystem::path& path, ResourceFlags& output)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }

    Rsc7Header header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        header.magic != kRsc7Magic)
    {
        return false;
    }

    output.virtualFlag = header.virtualFlag;
    output.physicalFlag = header.physicalFlag;
    return true;
}
}
