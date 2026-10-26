#include "relay_client.hpp"
#include "util.hpp"

#include <Windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
using namespace modelcustom;

bool wait_for_peer(
    RelayClient& client,
    int player,
    const std::string& model,
    std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto peers = client.get_peers();
        for (const auto& peer : peers)
        {
            if (peer.serverId == player && peer.modelId == model) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

bool wait_for_empty(RelayClient& client, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (client.get_peers().empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}
}

int main(int argc, char** argv)
{
    const std::string relayUrl = argc > 1 ? argv[1] : "http://127.0.0.1:8787";
    const std::string packSha(64, 'c');
    initialize_paths(GetModuleHandleW(nullptr));

    RelayClient first;
    RelayClient second;
    first.set_active_model("shrek", "1.0.0", packSha, true);
    second.set_active_model("goku", "1.0.0", packSha, true);
    first.start(relayUrl);
    second.start(relayUrl);
    first.update_session("relay-smoke-server:30120", 70);
    second.update_session("relay-smoke-server:30120", 71);

    if (!wait_for_peer(first, 71, "goku") || !wait_for_peer(second, 70, "shrek"))
    {
        std::cerr << "FAIL: clients did not receive the join snapshot\n";
        return 1;
    }

    second.set_active_model("sonic", "1.0.0", packSha, true);
    if (!wait_for_peer(first, 71, "sonic"))
    {
        std::cerr << "FAIL: model update was not pushed\n";
        return 1;
    }

    second.set_active_model("none", "1.0.0", "", false);
    if (!wait_for_peer(first, 71, "none"))
    {
        std::cerr << "FAIL: disabled state was not pushed\n";
        return 1;
    }

    const auto secondStopStarted = std::chrono::steady_clock::now();
    second.stop();
    const auto secondStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - secondStopStarted).count();
    if (!wait_for_empty(first))
    {
        std::cerr << "FAIL: disconnect was not pushed\n";
        return 1;
    }

    const auto firstStopStarted = std::chrono::steady_clock::now();
    first.stop();
    const auto firstStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - firstStopStarted).count();
    if (secondStopMs >= 2000 || firstStopMs >= 2000)
    {
        std::cerr << "FAIL: WebSocket shutdown waited for a transport timeout\n";
        return 1;
    }
    std::cout << "PASS: WinHTTP WebSocket join/update/disable/leave"
              << " (stop " << secondStopMs << " ms, " << firstStopMs << " ms)\n";
    return 0;
}
