#include "util.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace modelcustom
{
bool is_cl2_instance()
{
    static bool checked = false;
    static bool result = false;
    if (!checked)
    {
        checked = true;
        const wchar_t* cmd = GetCommandLineW();
        if (cmd)
        {
            std::wstring cmdStr(cmd);
            for (auto& c : cmdStr)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (cmdStr.find(L"-cl2") != std::wstring::npos ||
                cmdStr.find(L" cl2") != std::wstring::npos ||
                cmdStr.find(L"/cl2") != std::wstring::npos ||
                cmdStr.find(L"fivem_cl2") != std::wstring::npos)
            {
                result = true;
            }
        }
        if (!result)
        {
            wchar_t exePath[MAX_PATH] = { 0 };
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            {
                std::wstring exeStr(exePath);
                for (auto& c : exeStr)
                {
                    c = static_cast<wchar_t>(towlower(c));
                }
                if (exeStr.find(L"cl2") != std::wstring::npos)
                {
                    result = true;
                }
            }
        }
    }
    return result;
}

namespace
{
std::filesystem::path g_pluginDirectory;
std::filesystem::path g_logPath;
std::filesystem::path g_adhesiveDiagPath;
SRWLOCK g_logLock = SRWLOCK_INIT;
SRWLOCK g_adhesiveDiagLock = SRWLOCK_INIT;
std::atomic<std::uint64_t> g_adhesiveDiagSequence = 0;

std::string file_name_utf8(const wchar_t* path)
{
    if (!path || !*path)
    {
        return "unknown";
    }

    const std::filesystem::path value(path);
    const auto filename = value.filename().wstring();
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        filename.data(),
        static_cast<int>(filename.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return "unknown";
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        filename.data(),
        static_cast<int>(filename.size()),
        output.data(),
        required,
        nullptr,
        nullptr);
    return output;
}

std::string diag_address_fields(const void* address)
{
    std::ostringstream text;
    text << "address=" << to_hex(reinterpret_cast<std::uintptr_t>(address));

    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory))
    {
        text << " module=unmapped rva=0x0 protect=0x0 state=0x0 type=0x0";
        return text.str();
    }

    wchar_t modulePath[32768]{};
    const auto allocationBase = reinterpret_cast<HMODULE>(memory.AllocationBase);
    const DWORD modulePathLength = allocationBase
        ? GetModuleFileNameW(allocationBase, modulePath, static_cast<DWORD>(std::size(modulePath)))
        : 0;
    const auto addressValue = reinterpret_cast<std::uintptr_t>(address);
    const auto baseValue = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);

    text << " module=" << (modulePathLength ? file_name_utf8(modulePath) : "private")
         << " rva=" << to_hex(addressValue >= baseValue ? addressValue - baseValue : 0)
         << " allocation_base=" << to_hex(baseValue)
         << " protect=" << to_hex(memory.Protect)
         << " state=" << to_hex(memory.State)
         << " type=" << to_hex(memory.Type);
    return text.str();
}

void append_diag_line(std::string_view line)
{
    AcquireSRWLockExclusive(&g_adhesiveDiagLock);

    if (!g_adhesiveDiagPath.empty())
    {
        HANDLE file = CreateFileW(
            g_adhesiveDiagPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }

    ReleaseSRWLockExclusive(&g_adhesiveDiagLock);
}

std::optional<std::string> parse_ini_value(std::string_view contents, std::string_view key)
{
    const std::string prefix = std::string(key) + "=";
    const std::string rawText(contents);
    std::istringstream lines(rawText);
    std::string line;
    while (std::getline(lines, line))
    {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';')
        {
            continue;
        }

        if (line.rfind(prefix, 0) != 0)
        {
            continue;
        }

        auto value = trim(line.substr(prefix.size()));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        if (!value.empty())
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> configured_value(std::string_view key)
{
    if (is_cl2_instance())
    {
        const auto cl2Contents = read_text_file(g_pluginDirectory / L"ModelCustom_cl2.ini");
        if (cl2Contents)
        {
            if (auto val = parse_ini_value(*cl2Contents, key))
            {
                return val;
            }
        }

        const auto mainContents = read_text_file(g_pluginDirectory / L"ModelCustom.ini");
        if (mainContents)
        {
            const std::string cl2Key = std::string(key) + "_cl2";
            if (auto val = parse_ini_value(*mainContents, cl2Key))
            {
                return val;
            }
            if (auto val = parse_ini_value(*mainContents, key))
            {
                return val;
            }
        }
        return std::nullopt;
    }

    const auto contents = read_text_file(g_pluginDirectory / L"ModelCustom.ini");
    if (!contents)
    {
        return std::nullopt;
    }

    return parse_ini_value(*contents, key);
}
}

void initialize_paths(HMODULE module)
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));

    if (length == 0 || length >= buffer.size())
    {
        g_pluginDirectory = std::filesystem::current_path();
    }
    else
    {
        buffer.resize(length);
        g_pluginDirectory = std::filesystem::path(buffer).parent_path();
    }

    if (is_cl2_instance())
    {
        g_logPath = g_pluginDirectory / L"ModelCustom_cl2.log";
        g_adhesiveDiagPath = g_pluginDirectory / L"ModelCustom_adhesive_diag_cl2.log";
    }
    else
    {
        g_logPath = g_pluginDirectory / L"ModelCustom.log";
        g_adhesiveDiagPath = g_pluginDirectory / L"ModelCustom_adhesive_diag.log";
    }
}

const std::filesystem::path& plugin_directory()
{
    return g_pluginDirectory;
}

void log(std::string_view message)
{
    AcquireSRWLockExclusive(&g_logLock);

    const std::filesystem::path targetLog = !g_logPath.empty()
        ? g_logPath
        : (!g_pluginDirectory.empty() ? g_pluginDirectory / L"ModelCustom.log" : std::filesystem::path(L"ModelCustom.log"));

    HANDLE file = CreateFileW(
        targetLog.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }

    ReleaseSRWLockExclusive(&g_logLock);

    const std::string debuggerLine = "[ModelCustom] " + std::string(message) + "\n";
    OutputDebugStringA(debuggerLine.c_str());
}

void log_error(std::string_view message)
{
    log("ERROR: " + std::string(message));
}

void adhesive_diag(std::string_view event, std::string_view details)
{
    try
    {
        SYSTEMTIME time{};
        GetSystemTime(&time);

        char timestamp[64]{};
        std::snprintf(
            timestamp,
            std::size(timestamp),
            "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds);

        std::ostringstream line;
        line << timestamp
             << " seq=" << g_adhesiveDiagSequence.fetch_add(1, std::memory_order_relaxed) + 1
             << " pid=" << GetCurrentProcessId()
             << " tid=" << GetCurrentThreadId()
             << " event=" << event;
        if (!details.empty())
        {
            line << ' ' << details;
        }

        append_diag_line(line.str());
    }
    catch (...)
    {
        OutputDebugStringA("[ModelCustom] adhesive diagnostic logging failed\n");
    }
}

std::string adhesive_diag_address(const void* address)
{
    try
    {
        return diag_address_fields(address);
    }
    catch (...)
    {
        return "address=format_error";
    }
}

void adhesive_diag_pointer_write(
    std::string_view event,
    void* target,
    void* previousValue,
    void* replacementValue,
    DWORD previousProtection,
    bool protectionChanged,
    bool protectionRestored)
{
    try
    {
        std::ostringstream details;
        details << "target.{" << diag_address_fields(target) << '} '
                << "previous.{" << diag_address_fields(previousValue) << '} '
                << "replacement.{" << diag_address_fields(replacementValue) << '} '
                << "previous_protect=" << to_hex(previousProtection)
                << " protect_ok=" << (protectionChanged ? 1 : 0)
                << " restore_ok=" << (protectionRestored ? 1 : 0);
        adhesive_diag(event, details.str());
    }
    catch (...)
    {
        OutputDebugStringA("[ModelCustom] adhesive pointer diagnostic failed\n");
    }
}

bool adhesive_diag_counter_milestone(std::uint64_t count)
{
    return count == 1 || count == 10 || count == 100 || count == 1000 ||
        (count >= 10000 && count % 10000 == 0);
}

void adhesive_diag_module_snapshot(HMODULE pluginModule)
{
    try
    {
        wchar_t processPath[32768]{};
        GetModuleFileNameW(nullptr, processPath, static_cast<DWORD>(std::size(processPath)));

        std::ostringstream header;
        header << "instance=" << (is_cl2_instance() ? "cl2" : "main")
               << " process=" << file_name_utf8(processPath)
               << " plugin.{" << diag_address_fields(pluginModule) << '}';
        adhesive_diag("session.begin", header.str());

        constexpr const wchar_t* moduleNames[] = {
            L"adhesive.dll",
            L"CoreRT.dll",
            L"scripting-gta.dll",
            L"gta-net-five.dll",
            L"gta-streaming-five.dll"
        };
        for (const auto* moduleName : moduleNames)
        {
            const auto module = GetModuleHandleW(moduleName);
            std::ostringstream details;
            details << "requested=" << file_name_utf8(moduleName)
                    << " loaded=" << (module ? 1 : 0);
            if (module)
            {
                details << " base.{" << diag_address_fields(module) << '}';
            }
            adhesive_diag("module.snapshot", details.str());
        }
    }
    catch (...)
    {
        OutputDebugStringA("[ModelCustom] adhesive module snapshot failed\n");
    }
}

std::string wide_to_utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required <= 0)
    {
        return {};
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        required,
        nullptr,
        nullptr);
    return output;
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);

    if (required <= 0)
    {
        return {};
    }

    std::wstring output(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        required);
    return output;
}

std::string path_to_engine_string(const std::filesystem::path& value)
{
    auto result = wide_to_utf8(value.wstring());
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string trim(std::string value)
{
    const auto notSpace = [](unsigned char character)
    {
        return std::isspace(character) == 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }

    std::ostringstream contents;
    contents << stream.rdbuf();
    auto result = contents.str();
    if (result.size() >= 3 &&
        static_cast<unsigned char>(result[0]) == 0xEF &&
        static_cast<unsigned char>(result[1]) == 0xBB &&
        static_cast<unsigned char>(result[2]) == 0xBF)
    {
        result.erase(0, 3);
    }

    return result;
}

std::optional<std::string> extract_xml_tag(std::string_view xml, std::string_view tag)
{
    const std::string opening = "<" + std::string(tag) + ">";
    const std::string closing = "</" + std::string(tag) + ">";
    const auto begin = xml.find(opening);
    if (begin == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto valueBegin = begin + opening.size();
    const auto end = xml.find(closing, valueBegin);
    if (end == std::string_view::npos)
    {
        return std::nullopt;
    }

    auto value = trim(std::string(xml.substr(valueBegin, end - valueBegin)));
    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

std::optional<std::filesystem::path> configured_folder()
{
    const auto value = configured_value("folder");
    return value ? resolve_user_path(*value) : std::nullopt;
}


std::string configured_replacement_model()
{
    if (auto value = configured_value("replace_model"))
    {
        return lowercase(trim(std::move(*value)));
    }

    return "mp_m_freemode_01";
}

std::uint32_t joaat(std::string_view value)
{
    std::uint32_t hash = 0;
    for (unsigned char character : value)
    {
        character = static_cast<unsigned char>(std::tolower(character));
        hash += character;
        hash += hash << 10;
        hash ^= hash >> 6;
    }

    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

std::string to_hex(std::uint64_t value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string sha256_hex(const void* data, std::size_t size)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || !hAlg)
    {
        return {};
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) < 0 || !hHash)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    if (data && size > 0)
    {
        BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(size), 0);
    }

    std::array<std::uint8_t, 32> digest{};
    BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::string hex;
    hex.reserve(64);
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const auto byte : digest)
    {
        hex.push_back(kDigits[(byte >> 4) & 0x0F]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

std::string sha256_hex(std::string_view text)
{
    return sha256_hex(text.data(), text.size());
}

std::string compute_model_pack_sha256(const std::filesystem::path& folder)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec))
    {
        return {};
    }

    struct Entry
    {
        std::string relPath;
        std::filesystem::path fullPath;
    };

    std::vector<Entry> entries;
    try
    {
        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(
                 folder, std::filesystem::directory_options::skip_permission_denied))
        {
            if (!dirEntry.is_regular_file(ec))
            {
                continue;
            }

            const auto ext = lowercase(dirEntry.path().extension().string());
            const auto filename = lowercase(dirEntry.path().filename().string());
            if (ext == ".ydd" || ext == ".yft" || ext == ".ytd" || ext == ".ymt" || ext == ".ycd" ||
                filename == "peds.meta")
            {
                auto rel = std::filesystem::relative(dirEntry.path(), folder, ec);
                auto relStr = path_to_engine_string(rel);
                entries.push_back({ lowercase(relStr), dirEntry.path() });
            }
        }
    }
    catch (...)
    {
        return {};
    }

    if (entries.empty())
    {
        return {};
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
    {
        return a.relPath < b.relPath;
    });

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || !hAlg)
    {
        return {};
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) < 0 || !hHash)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    for (const auto& entry : entries)
    {
        BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(entry.relPath.data())),
            static_cast<ULONG>(entry.relPath.size()), 0);
        const std::uint8_t zero = 0;
        BCryptHashData(hHash, const_cast<PUCHAR>(&zero), 1, 0);

        std::ifstream file(entry.fullPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            continue;
        }

        const auto fileSize = static_cast<std::uint64_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<std::uint64_t*>(&fileSize)),
            sizeof(fileSize), 0);

        std::vector<char> buffer(65536);
        std::uint64_t remaining = fileSize;
        while (remaining > 0 && file)
        {
            const auto toRead = static_cast<std::streamsize>((std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
            file.read(buffer.data(), toRead);
            const auto bytesRead = static_cast<ULONG>(file.gcount());
            if (bytesRead == 0)
            {
                break;
            }
            BCryptHashData(hHash, reinterpret_cast<PUCHAR>(buffer.data()), bytesRead, 0);
            remaining -= bytesRead;
        }
    }

    std::array<std::uint8_t, 32> digest{};
    BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::string hex;
    hex.reserve(64);
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const auto byte : digest)
    {
        hex.push_back(kDigits[(byte >> 4) & 0x0F]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

std::string generate_random_hex_token(std::size_t length)
{
    if (length == 0)
    {
        return {};
    }

    const std::size_t byteCount = (length + 1) / 2;
    std::vector<std::uint8_t> randomBytes(byteCount);
    BCryptGenRandom(nullptr, randomBytes.data(), static_cast<ULONG>(randomBytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    std::string hex;
    hex.reserve(length);
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const auto byte : randomBytes)
    {
        if (hex.size() < length) hex.push_back(kDigits[(byte >> 4) & 0x0F]);
        if (hex.size() < length) hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

std::optional<std::string> configured_relay_url()
{
    if (auto value = configured_value("relay_url"))
    {
        auto url = trim(std::move(*value));
        if (lowercase(url) != "your_endpoint" && lowercase(url) != "https://your_endpoint")
        {
            return url;
        }
    }
    return std::nullopt;
}

std::optional<std::string> configured_room_key()
{
    if (auto value = configured_value("room_key"))
    {
        return trim(std::move(*value));
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> configured_models_directory()
{
    if (const auto value = configured_value("models_dir"))
    {
        return resolve_user_path(*value);
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> resolve_user_path(std::string_view path)
{
    auto value = trim(std::string(path));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')))
    {
        value = value.substr(1, value.size() - 2);
    }
    if (value.empty() || value.find_first_of("\r\n\0") != std::string::npos)
        return std::nullopt;

    const auto wide = utf8_to_wide(value);
    if (wide.empty()) return std::nullopt;
    const DWORD required = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (required == 0 || required > 32768) return std::nullopt;
    std::wstring expanded(required, L'\0');
    if (ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), required) != required)
        return std::nullopt;
    expanded.resize(required - 1);
    std::filesystem::path result(expanded);
    if (result.is_relative()) result = g_pluginDirectory / result;
    std::error_code error;
    result = std::filesystem::absolute(result, error).lexically_normal();
    return error ? std::nullopt : std::optional<std::filesystem::path>(std::move(result));
}

bool save_ini_string(std::string_view key, std::string_view value)
{
    std::filesystem::path iniPath = g_pluginDirectory / L"ModelCustom.ini";
    std::string actualKey = std::string(key);
    if (is_cl2_instance())
    {
        const auto separate = g_pluginDirectory / L"ModelCustom_cl2.ini";
        std::error_code error;
        if (std::filesystem::is_regular_file(separate, error))
        {
            iniPath = separate;
        }
        else
        {
            actualKey += "_cl2";
        }
    }

    auto contents = read_text_file(iniPath).value_or(std::string{});
    const auto replacement = actualKey + "=" + std::string(value);
    const auto prefix = actualKey + "=";
    std::istringstream input(contents);
    std::ostringstream output;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto stripped = trim(line);
        if (!replaced && stripped.rfind(prefix, 0) == 0)
        {
            output << replacement << "\r\n";
            replaced = true;
        }
        else
        {
            output << line << "\r\n";
        }
    }
    if (!replaced)
    {
        if (!contents.empty() && contents.back() != '\n') output << "\r\n";
        output << replacement << "\r\n";
    }
    const auto bytes = output.str();
    const auto temporary = iniPath.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote || !MoveFileExW(temporary.c_str(), iniPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool save_configured_models_directory(const std::filesystem::path& path)
{
    return save_ini_string("models_dir", wide_to_utf8(path.wstring()));
}

bool configured_default_enabled()
{
    if (auto value = configured_value("default_enabled"))
    {
        const auto str = lowercase(trim(*value));
        return (str == "1" || str == "true" || str == "yes" || str == "on");
    }
    return false;
}

bool save_configured_default_enabled(bool enabled)
{
    return save_ini_string("default_enabled", enabled ? "1" : "0");
}

bool is_story_model(std::uint32_t hash)
{
    return hash == 0x0D7114C9U || hash == 0x9B22DBAFU || hash == 0x9B810FA2U;
}

bool is_story_model(std::string_view name)
{
    const auto hash = parse_model_hash(name);
    return is_story_model(hash);
}

bool configured_block_story_models()
{
    if (auto value = configured_value("block_story_models"))
    {
        const auto str = lowercase(trim(*value));
        return (str == "1" || str == "true" || str == "yes" || str == "on");
    }
    return true;
}

bool save_configured_block_story_models(bool enabled)
{
    return save_ini_string("block_story_models", enabled ? "1" : "0");
}

bool configured_custom_server_model_enabled()
{
    if (auto value = configured_value("custom_server_model_enabled"))
    {
        const auto str = lowercase(trim(*value));
        return (str == "1" || str == "true" || str == "yes" || str == "on");
    }
    return false;
}

bool save_configured_custom_server_model_enabled(bool enabled)
{
    return save_ini_string("custom_server_model_enabled", enabled ? "1" : "0");
}

std::string configured_custom_server_model()
{
    if (auto value = configured_value("custom_server_model"))
    {
        auto trimmed = lowercase(trim(*value));
        if (!trimmed.empty()) return trimmed;
    }
    return "mp_m_freemode_01";
}

bool save_configured_custom_server_model(std::string_view model)
{
    return save_ini_string("custom_server_model", model);
}

std::vector<std::string> default_blacklisted_models()
{
    return { "player_zero", "player_one", "player_two" };
}

std::vector<std::string> configured_blacklisted_models()
{
    if (auto value = configured_value("blacklisted_models"))
    {
        std::vector<std::string> result;
        std::istringstream stream(*value);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            token = trim(token);
            if (!token.empty())
            {
                result.push_back(token);
            }
        }
        if (!result.empty())
        {
            return result;
        }
    }
    return default_blacklisted_models();
}

bool save_configured_blacklisted_models(const std::vector<std::string>& models)
{
    std::string joined;
    for (size_t i = 0; i < models.size(); ++i)
    {
        if (i > 0) joined += ",";
        joined += trim(models[i]);
    }
    return save_ini_string("blacklisted_models", joined);
}

std::uint32_t parse_model_hash(std::string_view nameOrHash)
{
    auto s = trim(std::string(nameOrHash));
    if (s.empty()) return 0;
    if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
    {
        try
        {
            return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
        }
        catch (...) {}
    }
    bool allDigits = true;
    for (char c : s)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            allDigits = false;
            break;
        }
    }
    if (allDigits && !s.empty())
    {
        try
        {
            return static_cast<std::uint32_t>(std::stoul(s, nullptr, 10));
        }
        catch (...) {}
    }
    return joaat(lowercase(s));
}

bool readable_address(const void* address, std::size_t bytes)
{
    if (!address || bytes == 0)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return start <= end && bytes <= end - start;
}

bool executable_address(const void* address)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT)
    {
        return false;
    }

    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & kExecutable) != 0 && (memory.Protect & PAGE_GUARD) == 0;
}

bool replace_vtable_entry(
    void** vtable,
    std::size_t index,
    void* replacement,
    void** original)
{
    if (!vtable || !readable_address(vtable + index, sizeof(void*)) ||
        !executable_address(vtable[index]) || !executable_address(replacement))
    {
        adhesive_diag(
            "vtable.replace.rejected",
            "index=" + std::to_string(index) +
            " slot=" + adhesive_diag_address(vtable ? vtable + index : nullptr) +
            " replacement=" + adhesive_diag_address(replacement));
        return false;
    }

    void* previous = vtable[index];
    DWORD oldProtection = 0;
    const bool protectionChanged = VirtualProtect(
        vtable + index,
        sizeof(void*),
        PAGE_EXECUTE_READWRITE,
        &oldProtection) != FALSE;
    if (!protectionChanged)
    {
        adhesive_diag_pointer_write(
            "vtable.replace.protect_failed",
            vtable + index,
            previous,
            replacement,
            oldProtection,
            false,
            false);
        return false;
    }

    *original = previous;
    vtable[index] = replacement;
    FlushInstructionCache(GetCurrentProcess(), vtable + index, sizeof(void*));

    DWORD ignored = 0;
    const bool protectionRestored = VirtualProtect(
        vtable + index,
        sizeof(void*),
        oldProtection,
        &ignored) != FALSE;
    adhesive_diag_pointer_write(
        "vtable.replace",
        vtable + index,
        previous,
        replacement,
        oldProtection,
        protectionChanged,
        protectionRestored);
    return true;
}

void restore_vtable_entry(void** vtable, std::size_t index, void* original)
{
    if (!vtable || !original || !readable_address(vtable + index, sizeof(void*)))
    {
        return;
    }

    void* previous = vtable[index];
    DWORD oldProtection = 0;
    const bool protectionChanged = VirtualProtect(
        vtable + index,
        sizeof(void*),
        PAGE_EXECUTE_READWRITE,
        &oldProtection) != FALSE;
    if (protectionChanged)
    {
        vtable[index] = original;
        FlushInstructionCache(GetCurrentProcess(), vtable + index, sizeof(void*));
        DWORD ignored = 0;
        const bool protectionRestored = VirtualProtect(
            vtable + index,
            sizeof(void*),
            oldProtection,
            &ignored) != FALSE;
        adhesive_diag_pointer_write(
            "vtable.restore",
            vtable + index,
            previous,
            original,
            oldProtection,
            true,
            protectionRestored);
    }
    else
    {
        adhesive_diag_pointer_write(
            "vtable.restore.protect_failed",
            vtable + index,
            previous,
            original,
            oldProtection,
            false,
            false);
    }
}
}
