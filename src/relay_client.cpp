#include "relay_client.hpp"

#include "util.hpp"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <thread>
#include <utility>

namespace modelcustom
{
namespace
{
constexpr DWORD kWebSocketSuccess = WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
constexpr std::size_t kMaxMessageBytes = 128 * 1024;

struct ClientState
{
    std::string room;
    std::string token;
    std::string modelId;
    std::string modelVersion;
    std::string packSha256;
    int player = 0;
    unsigned long long revision = 0;
};

struct WebSocketConnection
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;

    ~WebSocketConnection()
    {
        if (socket) WinHttpCloseHandle(socket);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

std::string extract_json_string(std::string_view json, std::string_view key)
{
    const std::string search = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(search);
    if (keyPos == std::string_view::npos) return {};

    const auto colonPos = json.find(':', keyPos + search.size());
    if (colonPos == std::string_view::npos) return {};

    const auto quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string_view::npos) return {};

    const auto quoteEnd = json.find('"', quoteStart + 1);
    if (quoteEnd == std::string_view::npos) return {};

    return std::string(json.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
}

int extract_json_int(std::string_view json, std::string_view key)
{
    const std::string search = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(search);
    if (keyPos == std::string_view::npos) return -1;

    const auto colonPos = json.find(':', keyPos + search.size());
    if (colonPos == std::string_view::npos) return -1;

    auto start = colonPos + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;

    auto end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) ++end;
    if (start == end) return -1;

    try
    {
        return std::stoi(std::string(json.substr(start, end - start)));
    }
    catch (...)
    {
        return -1;
    }
}

std::vector<PeerPresence> parse_peers_json(std::string_view response)
{
    std::vector<PeerPresence> peers;
    const auto peersPos = response.find("\"peers\"");
    if (peersPos == std::string_view::npos) return peers;

    const auto arrayStart = response.find('[', peersPos);
    if (arrayStart == std::string_view::npos) return peers;
    const auto arrayEnd = response.find(']', arrayStart);
    if (arrayEnd == std::string_view::npos) return peers;

    const auto arrayContent = response.substr(arrayStart + 1, arrayEnd - arrayStart - 1);
    std::size_t cursor = 0;
    while (cursor < arrayContent.size())
    {
        const auto objectStart = arrayContent.find('{', cursor);
        if (objectStart == std::string_view::npos) break;
        const auto objectEnd = arrayContent.find('}', objectStart);
        if (objectEnd == std::string_view::npos) break;

        const auto object = arrayContent.substr(objectStart, objectEnd - objectStart + 1);
        const int player = extract_json_int(object, "player");
        const auto modelId = extract_json_string(object, "modelId");
        const auto modelVersion = extract_json_string(object, "modelVersion");
        const auto packSha256 = extract_json_string(object, "packSha256");
        if (player > 0 && !modelId.empty() && !packSha256.empty())
        {
            peers.push_back(PeerPresence{
                .serverId = player,
                .modelId = lowercase(modelId),
                .modelVersion = modelVersion.empty() ? "1.0.0" : modelVersion,
                .packSha256 = lowercase(packSha256),
                .lastSeenMs = GetTickCount64()
            });
        }
        cursor = objectEnd + 1;
    }
    return peers;
}

std::string json_escape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character >= 0x20) escaped.push_back(static_cast<char>(character));
            break;
        }
    }
    return escaped;
}

std::string model_payload(const ClientState& state, std::string_view type)
{
    std::ostringstream json;
    json << "{\"protocol\":2,\"type\":\"" << type << "\"";
    if (type == "hello")
    {
        json << ",\"player\":" << state.player
             << ",\"token\":\"" << json_escape(state.token) << "\"";
    }
    json << ",\"modelId\":\"" << json_escape(state.modelId) << "\""
         << ",\"modelVersion\":\"" << json_escape(state.modelVersion) << "\""
         << ",\"packSha256\":\"" << json_escape(state.packSha256) << "\"}";
    return json.str();
}

bool send_text(HINTERNET socket, const std::string& payload)
{
    return WinHttpWebSocketSend(
        socket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size())) == NO_ERROR;
}

bool open_websocket(const std::string& relayUrl, const std::string& room, WebSocketConnection& result)
{
    const auto fullUrl = relayUrl + "/v2/connect?room=" + room;
    const auto wideUrl = utf8_to_wide(fullUrl);
    URL_COMPONENTSW url{};
    url.dwStructSize = sizeof(url);
    url.dwHostNameLength = static_cast<DWORD>(-1);
    url.dwUrlPathLength = static_cast<DWORD>(-1);
    url.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &url)) return false;

    const std::wstring host(url.lpszHostName, url.dwHostNameLength);
    std::wstring path(url.lpszUrlPath, url.dwUrlPathLength);
    if (url.dwExtraInfoLength > 0) path.append(url.lpszExtraInfo, url.dwExtraInfoLength);

    result.session = WinHttpOpen(
        L"ModelCustom-Client/2.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!result.session) return false;
    WinHttpSetTimeouts(result.session, 3000, 3000, 5000, 5000);

    result.connection = WinHttpConnect(result.session, host.c_str(), url.nPort, 0);
    if (!result.connection) return false;

    const DWORD flags = url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        result.connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) return false;

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    const bool hasStatus = WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (!hasStatus || statusCode != HTTP_STATUS_SWITCH_PROTOCOLS)
    {
        WinHttpCloseHandle(request);
        return false;
    }

    result.socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    return result.socket != nullptr;
}

void interruptible_sleep(const std::atomic_bool& running, unsigned long milliseconds)
{
    const auto deadline = GetTickCount64() + milliseconds;
    while (running.load() && GetTickCount64() < deadline) Sleep(100);
}
}

RelayClient::RelayClient() = default;

RelayClient::~RelayClient()
{
    stop();
}

void RelayClient::start(const std::string& relayUrl)
{
    if (running_.load()) return;

    relayUrl_ = relayUrl;
    while (!relayUrl_.empty() && relayUrl_.back() == '/') relayUrl_.pop_back();
    if (relayUrl_.empty()) return;

    running_.store(true);
    thread_ = CreateThread(
        nullptr,
        0,
        [](void* parameter) -> DWORD
        {
            static_cast<RelayClient*>(parameter)->thread_loop();
            return 0;
        },
        this,
        0,
        nullptr);
    if (!thread_)
    {
        running_.store(false);
        log_error("Could not start RelayClient WebSocket thread");
        return;
    }
    log("RelayClient WebSocket worker started for: " + relayUrl_);
}

void RelayClient::stop()
{
    if (!running_.exchange(false)) return;
    void* s = currentSocket_.exchange(nullptr);
    if (s)
    {
        WinHttpWebSocketShutdown(static_cast<HINTERNET>(s), kWebSocketSuccess, nullptr, 0);
    }
    if (thread_)
    {
        WaitForSingleObject(thread_, 10000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    log("RelayClient stopped");
}

void RelayClient::set_active_model(
    const std::string& modelId,
    const std::string& modelVersion,
    const std::string& packSha256,
    bool enabled)
{
    const auto normalizedId = lowercase(modelId);
    const auto normalizedVersion = modelVersion.empty() ? "1.0.0" : modelVersion;
    const auto normalizedSha = lowercase(packSha256);

    std::lock_guard lock(mutex_);
    if (enabled_.load() == enabled && activeModelId_ == normalizedId &&
        activeModelVersion_ == normalizedVersion && activePackSha256_ == normalizedSha)
    {
        return;
    }
    enabled_.store(enabled);
    activeModelId_ = normalizedId;
    activeModelVersion_ = normalizedVersion;
    activePackSha256_ = normalizedSha;
    ++stateRevision_;
}

void RelayClient::update_session(const std::string& endpoint, int localServerId)
{
    const auto configuredKey = configured_room_key();
    const std::string roomIdentifier = (configuredKey && !configuredKey->empty())
        ? ("room_key:" + lowercase(*configuredKey))
        : ("endpoint:" + lowercase(endpoint));

    std::lock_guard lock(mutex_);
    if (currentEndpoint_ == roomIdentifier && localServerId_ == localServerId) return;

    currentEndpoint_ = roomIdentifier;
    localServerId_ = localServerId;

    std::string seed;
    if (configuredKey && !configuredKey->empty()) seed = "modelcustom_key_" + lowercase(*configuredKey);
    else if (!endpoint.empty()) seed = lowercase(endpoint);

    if (!seed.empty() && localServerId > 0)
    {
        currentRoom_ = sha256_hex(seed);
        sessionToken_ = generate_random_hex_token(32);
        const std::string description = (configuredKey && !configuredKey->empty())
            ? ("room_key=" + *configuredKey)
            : ("server=" + endpoint);
        log("RelayClient room selected: " + description + " room=" +
            currentRoom_.substr(0, 12) + "... (serverId=" + std::to_string(localServerId) + ")");
    }
    else
    {
        currentRoom_.clear();
        sessionToken_.clear();
        activePeers_.clear();
    }
    ++stateRevision_;
}

std::vector<PeerPresence> RelayClient::get_peers() const
{
    std::lock_guard lock(mutex_);
    return activePeers_;
}

void RelayClient::receive_loop(void* socketValue, std::atomic_bool& connected)
{
    const auto socket = static_cast<HINTERNET>(socketValue);
    std::array<char, 4096> buffer{};
    std::string message;

    while (running_.load() && connected.load())
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(
            socket,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            &type);
        if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) break;

        if (message.size() + bytesRead > kMaxMessageBytes) break;
        message.append(buffer.data(), bytesRead);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;

        if (message.find("\"protocol\":2") != std::string::npos &&
            message.find("\"type\":\"snapshot\"") != std::string::npos &&
            message.find("\"peers\"") != std::string::npos)
        {
            auto peers = parse_peers_json(message);
            std::lock_guard lock(mutex_);
            const bool discoveredFirstPeer = activePeers_.empty() && !peers.empty();
            activePeers_ = std::move(peers);
            if (discoveredFirstPeer)
            {
                log("Relay WebSocket discovered " + std::to_string(activePeers_.size()) + " peer(s)");
            }
        }
        message.clear();
    }
    connected.store(false);
}

void RelayClient::thread_loop()
{
    unsigned int consecutiveFailures = 0;
    while (running_.load())
    {
        ClientState state;
        {
            std::lock_guard lock(mutex_);
            state.room = currentRoom_;
            state.token = sessionToken_;
            state.player = localServerId_;
            state.revision = stateRevision_;
            if (enabled_.load() && !activeModelId_.empty() && !activePackSha256_.empty())
            {
                state.modelId = activeModelId_;
                state.modelVersion = activeModelVersion_;
                state.packSha256 = activePackSha256_;
            }
            else
            {
                state.modelId = "none";
                state.modelVersion = "1.0.0";
                state.packSha256.assign(64, '0');
            }
        }

        if (state.room.empty() || state.token.empty() || state.player <= 0)
        {
            interruptible_sleep(running_, 200);
            continue;
        }

        WebSocketConnection websocket;
        if (!open_websocket(relayUrl_, state.room, websocket) ||
            !send_text(websocket.socket, model_payload(state, "hello")))
        {
            ++consecutiveFailures;
            const unsigned long backoff = std::min(30000UL,
                1000UL << std::min(consecutiveFailures - 1, 5U));
            interruptible_sleep(running_, backoff + static_cast<unsigned long>(GetTickCount64() % 500));
            continue;
        }

        currentSocket_.store(websocket.socket);
        log("RelayClient joined WebSocket room " + state.room.substr(0, 12) + "...");
        const auto connectedAt = GetTickCount64();
        std::atomic_bool connected = true;
        std::thread receiver([this, socket = websocket.socket, &connected]()
        {
            receive_loop(socket, connected);
        });

        unsigned long long sentRevision = state.revision;
        bool sessionChanged = false;
        while (running_.load() && connected.load())
        {
            ClientState current;
            {
                std::lock_guard lock(mutex_);
                current.room = currentRoom_;
                current.token = sessionToken_;
                current.player = localServerId_;
                current.revision = stateRevision_;
                if (enabled_.load() && !activeModelId_.empty() && !activePackSha256_.empty())
                {
                    current.modelId = activeModelId_;
                    current.modelVersion = activeModelVersion_;
                    current.packSha256 = activePackSha256_;
                }
                else
                {
                    current.modelId = "none";
                    current.modelVersion = "1.0.0";
                    current.packSha256.assign(64, '0');
                }
            }

            if (current.room != state.room || current.token != state.token || current.player != state.player)
            {
                sessionChanged = true;
                break;
            }
            if (current.revision != sentRevision)
            {
                if (!send_text(websocket.socket, model_payload(current, "update")))
                {
                    connected.store(false);
                    break;
                }
                sentRevision = current.revision;
            }
            Sleep(100);
        }

        currentSocket_.store(nullptr);
        send_text(websocket.socket, "{\"protocol\":2,\"type\":\"leave\"}");
        WinHttpWebSocketShutdown(websocket.socket, kWebSocketSuccess, nullptr, 0);
        if (receiver.joinable()) receiver.join();
        {
            std::lock_guard lock(mutex_);
            activePeers_.clear();
        }

        if (!running_.load()) break;
        if (sessionChanged)
        {
            consecutiveFailures = 0;
            continue;
        }

        if (GetTickCount64() - connectedAt >= 10000)
        {
            consecutiveFailures = 0;
        }
        else
        {
            ++consecutiveFailures;
            const unsigned long backoff = std::min(30000UL,
                1000UL << std::min(consecutiveFailures - 1, 5U));
            interruptible_sleep(running_, backoff + static_cast<unsigned long>(GetTickCount64() % 500));
        }
    }
}
}
