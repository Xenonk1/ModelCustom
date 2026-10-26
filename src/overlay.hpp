#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace modelcustom::overlay
{
struct Peer
{
    int serverId = 0;
    std::string model;
    bool installed = false;
};


struct Snapshot
{
    bool enabled = false;
    bool defaultEnabled = false;
    bool blockStoryModels = true;
    bool customServerModelEnabled = false;
    std::string customServerModel;
    bool pending = false;
    std::string model;
    std::string currentLocalModel;
    std::string spoofedRemoteModel;
    std::string modelsPath;
    std::vector<std::string> models;
    std::vector<Peer> peers;
    std::vector<std::string> blacklistedModels;
};

enum class Action {
    toggle,
    reload_config,
    select_model,
    set_models_path,
    set_default_enabled,
    set_block_story_models,
    set_custom_server_model_enabled,
    set_custom_server_model,
    add_blacklist,
    remove_blacklist,
    reset_blacklist
};
struct Command
{
    Action action;
    std::string model;
};

bool start(HMODULE module);
void request_stop();
void stop();
bool captures_input();
bool running();
void publish(Snapshot snapshot);
std::vector<Command> take_commands();
#ifdef MODELCUSTOM_OVERLAY_TEST
void test_force_open(bool open);
#endif
}
