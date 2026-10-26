#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace modelcustom
{
struct PeerPresence
{
    int serverId = 0;
    std::string modelId;
    std::string modelVersion;
    std::string packSha256;
    unsigned long long lastSeenMs = 0;
};

class RelayClient
{
public:
    RelayClient();
    ~RelayClient();

    void start(const std::string& relayUrl);
    void stop();

    void set_active_model(
        const std::string& modelId,
        const std::string& modelVersion,
        const std::string& packSha256,
        bool enabled);

    void update_session(const std::string& endpoint, int localServerId);

    std::vector<PeerPresence> get_peers() const;
    bool is_enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    void thread_loop();
    void receive_loop(void* socket, std::atomic_bool& connected);

    std::string relayUrl_;
    std::atomic_bool running_ = false;
    std::atomic_bool enabled_ = false;
    std::atomic<void*> currentSocket_ = nullptr;
    HANDLE thread_ = nullptr;

    mutable std::mutex mutex_;
    std::string currentEndpoint_;
    std::string currentRoom_;
    std::string sessionToken_;
    int localServerId_ = 0;

    std::string activeModelId_;
    std::string activeModelVersion_ = "1.0.0";
    std::string activePackSha256_;

    std::vector<PeerPresence> activePeers_;
    unsigned long long stateRevision_ = 1;
};
}
