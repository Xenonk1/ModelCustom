#include "model_catalog.hpp"
#include "streaming_bridge.hpp"
#include "util.hpp"

#include <Windows.h>

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
int checks = 0;

void expect(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
    ++checks;
}

void write_file(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) throw std::runtime_error("Could not write test fixture");
}

struct Fixtures
{
    std::filesystem::path config;
    std::filesystem::path root;

    Fixtures()
    {
        using namespace modelcustom;
        initialize_paths(GetModuleHandleW(nullptr));
        config = plugin_directory() / "ModelCustom.ini";
        if (std::filesystem::exists(config))
            throw std::runtime_error("Remove the INI from build/core-test before testing");
        root = plugin_directory() / ("fixtures-" + generate_random_hex_token());
        std::filesystem::create_directories(root);
    }

    ~Fixtures()
    {
        std::error_code error;
        std::filesystem::remove(config, error);

        if (root.parent_path() == modelcustom::plugin_directory() &&
            root.filename().string().starts_with("fixtures-"))
            std::filesystem::remove_all(root, error);
    }
};

void test_config()
{
    using namespace modelcustom;
    expect(!configured_folder(), "an omitted startup folder is optional");
    expect(!configured_relay_url(), "relay is disabled without a URL");
    expect(!configured_default_enabled(), "custom models start disabled");
    expect(save_ini_string("relay_url", "YOUR_ENDPOINT"), "save endpoint placeholder");
    expect(!configured_relay_url(), "endpoint placeholder does not start networking");
    expect(save_ini_string("relay_url", "https://YOUR_ENDPOINT"), "save HTTPS endpoint placeholder");
    expect(!configured_relay_url(), "HTTPS endpoint placeholder does not start networking");
    expect(save_ini_string("relay_url", "http://127.0.0.1:8787"), "save local test endpoint");
    expect(configured_relay_url() == "http://127.0.0.1:8787", "a configured endpoint is preserved");

    expect(save_ini_string("folder", "models\\my_ped"), "save relative startup folder");
    expect(configured_folder() == plugin_directory() / "models" / "my_ped",
        "startup folder resolves relative to the ASI");
    expect(save_ini_string("folder", "%LOCALAPPDATA%\\ModelCustom\\my_ped"), "save environment path");
    expect(configured_folder() == resolve_user_path("%LOCALAPPDATA%\\ModelCustom\\my_ped"),
        "startup folder expands environment variables");
    expect(save_ini_string("folder", "\"models\\friendly name\""), "save quoted startup folder");
    expect(configured_folder() == plugin_directory() / "models" / "friendly name",
        "quoted startup folder supports spaces");
    expect(save_ini_string("folder", ""), "clear startup folder");
    expect(!configured_folder(), "empty startup folder stays unset");
    expect(save_ini_string("models_dir", "models"), "save model catalog path");
    expect(configured_models_directory() == plugin_directory() / "models",
        "catalog and startup paths use the same base");
}

void test_catalog(const std::filesystem::path& root)
{
    using namespace modelcustom;
    const auto pack = root / "Friendly label";
    std::filesystem::create_directory(pack);
    write_file(pack / "model.txt", "my_ped\n");
    expect(resolve_model_name(pack) == "my_ped", "legacy model.txt uses the actual internal name");
    write_file(pack / "models.txt", "mcx1:3e07160007\n");
    expect(resolve_model_name(pack) == "shrek", "encoded models.txt has priority");
    write_file(pack / "models.txt", "mcx1:invalid\n");
    expect(resolve_model_name(pack) == "my_ped", "malformed encoded name falls back to model.txt");
    std::filesystem::remove(pack / "models.txt");
    std::filesystem::remove(pack / "model.txt");
    expect(resolve_model_name(pack) == "friendly_label", "unlabelled packs use normalized folder names");

    write_file(pack / "peds.meta", "<Name>my_ped</Name>");
    write_file(pack / "my_ped.ydd", "fixture-v1");
    const auto firstHash = compute_model_pack_sha256(pack);
    expect(firstHash.size() == 64, "model packs receive a SHA-256 digest");
    write_file(pack / "notes.txt", "not an asset");
    expect(compute_model_pack_sha256(pack) == firstHash, "non-asset files do not affect pack identity");
    write_file(pack / "my_ped.ydd", "fixture-v2");
    expect(compute_model_pack_sha256(pack) != firstHash, "changed assets change pack identity");
    expect(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "Windows SHA-256 matches the standard test vector");

    ModelCatalog catalog;
    catalog.register_explicit_model(pack, "my_ped", firstHash);
    expect(catalog.find("MY_PED", firstHash) != nullptr, "matching installed pack is found");
    expect(catalog.find("my_ped", std::string(64, '0')) == nullptr,
        "a different pack with the same name is rejected");
    expect(catalog.find("my_ped", "") == nullptr, "missing pack hash is rejected");
    expect(catalog.find("missing_ped", firstHash) == nullptr, "unknown pack is rejected");

    StreamingBridge unavailable;
    expect(!catalog.ensure_streaming_registered("my_ped", std::string(64, '0'), unavailable),
        "hash mismatch cannot bypass validation through the streaming path");
    expect(catalog.ensure_streaming_registered("my_ped", firstHash, unavailable),
        "a matching already-registered pack needs no streaming calls");

    const auto resource = root / "resource.yft";
    const std::array<std::uint32_t, 4> header = { 0x37435352, 1, 0x1234, 0x5678 };
    write_file(resource, std::string_view(reinterpret_cast<const char*>(header.data()), sizeof(header)));
    ResourceFlags flags{};
    expect(read_rsc7_flags(resource, flags) && flags.virtualFlag == 0x1234 && flags.physicalFlag == 0x5678,
        "RSC7 streaming flags are read from a complete header");
    write_file(resource, "RSC7");
    expect(!read_rsc7_flags(resource, flags), "truncated RSC7 header is rejected");
}
}

int main()
{
    try
    {
        Fixtures fixtures;
        test_config();
        test_catalog(fixtures.root);
        std::cout << "PASS: " << checks << " core checks\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
