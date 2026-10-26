#include "game_frame_bridge.hpp"
#include "model_loader.hpp"
#include "native_api.hpp"
#include "network_model_spoof.hpp"
#include "overlay.hpp"
#include "peer_sync_hook.hpp"
#include "streaming_bridge.hpp"
#include "util.hpp"

#include <Windows.h>

#include <atomic>
#include <new>

namespace modelcustom
{
namespace
{
HMODULE g_module = nullptr;
std::atomic_bool g_stopping = false;
NetworkModelSpoof g_networkSpoof;
StreamingBridge* g_streaming = nullptr;
ModelLoader* g_loader = nullptr;
unsigned long long g_nextStreamingResolve = 0;
unsigned long long g_nextUiUpdate = 0;

void load_from_config(ModelLoader& loader)
{
    loader.reload_from_config();
    if (const auto folder = configured_folder())
    {
        loader.register_model(*folder);
    }
    else
    {
        log("No startup model configured; select a model in the F4 menu");
    }
}

bool initialize_runtime()
{
    if (g_loader)
    {
        return true;
    }

    const auto now = GetTickCount64();
    if (now < g_nextStreamingResolve)
    {
        return false;
    }
    g_nextStreamingResolve = now + 500;

    if (!native::can_invoke_natives())
    {
        return false;
    }
    if (!g_streaming)
    {
        g_streaming = new (std::nothrow) StreamingBridge();
    }
    if (!g_streaming || (!g_streaming->available() && !g_streaming->resolve()))
    {
        return false;
    }

    log("Main-game-frame runtime started without ScriptHookV registration or ShAllowed writes");
    adhesive_diag("runtime.start", "game_frame_bridge=1 scripthook_thread=0 sh_allowed_write=0");
    static_cast<void>(g_networkSpoof.initialize());
    static_cast<void>(PeerSyncHook::instance().initialize());
    g_loader = new (std::nothrow) ModelLoader(*g_streaming, g_networkSpoof);
    if (!g_loader)
    {
        log_error("Could not allocate ModelLoader");
        return false;
    }

    load_from_config(*g_loader);
    overlay::publish(g_loader->ui_snapshot());
    overlay::start(g_module);
    g_nextUiUpdate = 0;
    log("Entering ModelCustom direct game-frame tick loop");
    native::subtitle("~b~ModelCustom~s~ | F4: Menu / F5: Toggle / F7: Reload", 5500);
    return true;
}

bool on_main_game_frame()
{
    if (g_stopping)
    {
        return true;
    }
    if (!initialize_runtime())
    {
        return true;
    }

    for (const auto& command : overlay::take_commands())
    {
        switch (command.action)
        {
        case overlay::Action::toggle: g_loader->toggle_enabled(); break;
        case overlay::Action::reload_config: load_from_config(*g_loader); break;
        case overlay::Action::select_model: g_loader->select_catalog_model(command.model); break;
        case overlay::Action::set_models_path: g_loader->set_models_directory(command.model); break;
        case overlay::Action::set_default_enabled: g_loader->set_default_enabled(command.model == "1"); break;
        case overlay::Action::set_block_story_models: g_loader->set_block_story_models(command.model == "1"); break;
        case overlay::Action::set_custom_server_model_enabled: g_loader->set_custom_server_model_enabled(command.model == "1"); break;
        case overlay::Action::set_custom_server_model: g_loader->set_custom_server_model(command.model); break;
        case overlay::Action::add_blacklist: g_loader->add_blacklisted_model(command.model); break;
        case overlay::Action::remove_blacklist: g_loader->remove_blacklisted_model(command.model); break;
        case overlay::Action::reset_blacklist: g_loader->reset_blacklisted_models(); break;
        }
        g_nextUiUpdate = 0;
    }

    if (overlay::captures_input())
    {
        native::invoke_void(native::kDisableAllControlActions, { 0 });
        native::invoke_void(native::kDisableAllControlActions, { 1 });
        native::invoke_void(native::kDisableAllControlActions, { 2 });
    }

    if ((GetAsyncKeyState(VK_F5) & 1) != 0)
    {
        g_loader->toggle_enabled();
    }

    if ((GetAsyncKeyState(VK_F7) & 1) != 0)
    {
        load_from_config(*g_loader);
    }

    g_loader->tick();
    const auto now = GetTickCount64();
    if (overlay::running() && now >= g_nextUiUpdate)
    {
        overlay::publish(g_loader->ui_snapshot());
        g_nextUiUpdate = now + 250;
    }

    return true;
}

DWORD WINAPI bootstrap_thread(void*)
{
    initialize_paths(g_module);
    adhesive_diag_module_snapshot(g_module);
    log("Local ped bootstrap started; waiting for exported FiveM game-frame event");

    while (!g_stopping)
    {
        if (game_frame::connect(on_main_game_frame))
        {
            return 0;
        }
        Sleep(20);
    }

    adhesive_diag("bootstrap.stop", "stopping=1");
    return 0;
}
}
}

static HANDLE g_singletonMutex = nullptr;

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    using namespace modelcustom;

    if (reason == DLL_PROCESS_ATTACH)
    {
        const std::wstring mutexName = L"Local\\ModelCustom_PID_" + std::to_wstring(GetCurrentProcessId());
        g_singletonMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return TRUE;
        }

        g_module = module;
        DisableThreadLibraryCalls(module);

        const HANDLE thread = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_stopping = true;
        overlay::request_stop();
        if (reserved == nullptr)
        {
            game_frame::disconnect();
            g_networkSpoof.shutdown();
            PeerSyncHook::instance().clear_all_peers();
        }

        if (g_singletonMutex)
        {
            CloseHandle(g_singletonMutex);
            g_singletonMutex = nullptr;
        }
    }

    return TRUE;
}
