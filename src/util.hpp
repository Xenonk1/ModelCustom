#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modelcustom
{
void initialize_paths(HMODULE module);
const std::filesystem::path& plugin_directory();
bool is_cl2_instance();

void log(std::string_view message);
void log_error(std::string_view message);




void adhesive_diag(std::string_view event, std::string_view details = {});
void adhesive_diag_module_snapshot(HMODULE pluginModule);
void adhesive_diag_pointer_write(
    std::string_view event,
    void* target,
    void* previousValue,
    void* replacementValue,
    DWORD previousProtection,
    bool protectionChanged,
    bool protectionRestored);
std::string adhesive_diag_address(const void* address);
bool adhesive_diag_counter_milestone(std::uint64_t count);

std::string wide_to_utf8(std::wstring_view value);
std::wstring utf8_to_wide(std::string_view value);
std::string path_to_engine_string(const std::filesystem::path& value);
std::string trim(std::string value);
std::string lowercase(std::string value);
std::optional<std::string> read_text_file(const std::filesystem::path& path);
std::optional<std::string> extract_xml_tag(std::string_view xml, std::string_view tag);
std::optional<std::filesystem::path> configured_folder();
std::string configured_replacement_model();

std::uint32_t joaat(std::string_view value);
std::string to_hex(std::uint64_t value);

std::string sha256_hex(const void* data, std::size_t size);
std::string sha256_hex(std::string_view text);
std::string compute_model_pack_sha256(const std::filesystem::path& folder);
std::string generate_random_hex_token(std::size_t length = 32);

std::optional<std::string> configured_relay_url();
std::optional<std::string> configured_room_key();
std::optional<std::filesystem::path> configured_models_directory();
bool save_configured_models_directory(const std::filesystem::path& path);
std::optional<std::filesystem::path> resolve_user_path(std::string_view path);

bool configured_default_enabled();
bool save_configured_default_enabled(bool enabled);
bool is_story_model(std::uint32_t hash);
bool is_story_model(std::string_view name);
bool configured_block_story_models();
bool save_configured_block_story_models(bool enabled);
bool configured_custom_server_model_enabled();
bool save_configured_custom_server_model_enabled(bool enabled);
std::string configured_custom_server_model();
bool save_configured_custom_server_model(std::string_view model);
std::vector<std::string> default_blacklisted_models();
std::vector<std::string> configured_blacklisted_models();
bool save_configured_blacklisted_models(const std::vector<std::string>& models);
std::uint32_t parse_model_hash(std::string_view nameOrHash);
bool save_ini_string(std::string_view key, std::string_view value);

bool readable_address(const void* address, std::size_t bytes);
bool executable_address(const void* address);

bool replace_vtable_entry(
    void** vtable,
    std::size_t index,
    void* replacement,
    void** original);

void restore_vtable_entry(
    void** vtable,
    std::size_t index,
    void* original);
}
