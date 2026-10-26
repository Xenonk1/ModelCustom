#pragma once

#include <cstdint>

namespace modelcustom
{
class NetworkModelSpoof
{
public:
    bool initialize();



    bool prepare_switch(int currentPed, std::uint32_t networkModelHash);



    bool refresh_local_ped(int currentPed);
    void finish_switch();
    void disarm();
    void shutdown();

    bool ready() const;
    bool is_resolver_ready() const;
    std::uint32_t network_model_hash() const;
    std::uint64_t patched_write_count() const;
    void* local_net_object() const;

private:
    bool install_node_hooks(void* syncTree);
};

void* get_local_net_object();
std::uint8_t get_sync_tree_nodes_offset();
}
