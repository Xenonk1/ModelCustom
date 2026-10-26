#include "overlay.hpp"
#include "overlay_position.hpp"
#include "util.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace modelcustom::overlay
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr wchar_t kWindowClass[] = L"ModelCustom.OwnOverlay";
std::atomic_bool g_stop = false;
std::atomic_bool g_running = false;
std::atomic_bool g_capture = false;
std::mutex g_dataMutex;
Snapshot g_snapshot;
std::vector<Command> g_commands;
HANDLE g_thread = nullptr;
#ifdef MODELCUSTOM_OVERLAY_TEST
std::atomic_bool g_testForceOpen = false;
#endif

void check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " failed: " + to_hex(static_cast<unsigned long>(hr)));
}

struct Renderer
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> visual;
    UINT width = 0, height = 0;
#ifdef MODELCUSTOM_OVERLAY_TEST
    bool testFrameCaptured = false;

    void capture_test_frame(const wchar_t* captureVariable, bool allowCapture = true)
    {
        if (!allowCapture || testFrameCaptured || !target || width == 0 || height == 0) return;
        wchar_t path[32768]{};
        const DWORD pathLength = GetEnvironmentVariableW(captureVariable, path,
            static_cast<DWORD>(std::size(path)));
        if (pathLength == 0 || pathLength >= std::size(path)) return;
        if (ImGui::GetFrameCount() < 2) return;
        testFrameCaptured = true;

        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resource.As(&source))) return;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return;
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

        const DWORD pixelBytes = width * height * 4;
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = static_cast<LONG>(width);
        infoHeader.biHeight = -static_cast<LONG>(height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = pixelBytes;

        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
            WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);
            const DWORD rowBytes = width * 4;
            for (UINT row = 0; row < height; ++row)
            {
                const auto* pixels = static_cast<const unsigned char*>(mapped.pData) + row * mapped.RowPitch;
                WriteFile(file, pixels, rowBytes, &written, nullptr);
            }
            CloseHandle(file);
        }
        context->Unmap(staging.Get(), 0);
    }
#endif

    void resize(UINT newWidth, UINT newHeight)
    {
        if (newWidth == width && newHeight == height && target) return;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        check(swapchain->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
        ComPtr<ID3D11Texture2D> buffer;
        check(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
        check(device->CreateRenderTargetView(buffer.Get(), nullptr, &target), "CreateRenderTargetView");
        width = newWidth;
        height = newHeight;
    }

    void initialize(HWND window)
    {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
            &device, nullptr, &context), "D3D11CreateDevice");

        ComPtr<IDXGIDevice> dxgiDevice;
        check(device.As(&dxgiDevice), "IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 16;
        desc.Height = 16;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
#ifdef MODELCUSTOM_OVERLAY_TEST
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        check(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swapchain),
            "CreateSwapChainForHwnd");
#else
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &swapchain), "CreateSwapChainForComposition");
        check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&composition)), "DCompositionCreateDevice");
        check(composition->CreateTargetForHwnd(window, TRUE, &compositionTarget), "CreateTargetForHwnd");
        check(composition->CreateVisual(&visual), "CreateVisual");
        check(visual->SetContent(swapchain.Get()), "SetContent");
        check(compositionTarget->SetRoot(visual.Get()), "SetRoot");
        check(composition->Commit(), "Commit");
#endif
    }

    void render(const wchar_t* captureVariable = L"MODELCUSTOM_CAPTURE_FRAME", bool allowCapture = true)
    {
        constexpr float clear[4] = { 0, 0, 0, 0 };
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#ifdef MODELCUSTOM_OVERLAY_TEST
        capture_test_frame(captureVariable, allowCapture);
#else
        static_cast<void>(captureVariable);
        static_cast<void>(allowCapture);
#endif
        check(swapchain->Present(0, 0), "Present");
    }
};

struct WindowState
{
    ImGuiContext* gui = nullptr;
    bool closeRequested = false;
    bool dragging = false;
    bool positionChanged = false;
    int titleHeight = 0;
    int titleDragRight = 0;
    POINT dragOffset{};
    POINT requestedPosition{};
};

void update_drag(HWND window, WindowState& state)
{
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        state.requestedPosition = { cursor.x - state.dragOffset.x, cursor.y - state.dragOffset.y };
        state.positionChanged = true;
        SetWindowPos(window, HWND_TOPMOST, state.requestedPosition.x, state.requestedPosition.y,
            0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    struct ContextScope
    {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ~ContextScope() { ImGui::SetCurrentContext(previous); }
    } contextScope;
    ImGui::SetCurrentContext(state ? state->gui : nullptr);
    if (state)
    {
        if (message == WM_LBUTTONDOWN)
        {
            const int x = static_cast<short>(LOWORD(lParam));
            const int y = static_cast<short>(HIWORD(lParam));
            if (x >= 0 && x < state->titleDragRight && y >= 0 && y < state->titleHeight)
            {
                POINT cursor{};
                RECT bounds{};
                if (GetCursorPos(&cursor) && GetWindowRect(window, &bounds))
                {
                    state->dragOffset = { cursor.x - bounds.left, cursor.y - bounds.top };
                    state->dragging = true;
                    SetCapture(window);
                    return 0;
                }
            }
        }
        if (state->dragging && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP))
        {
            update_drag(window, *state);
            if (message == WM_LBUTTONUP)
            {
                state->dragging = false;
                if (GetCapture() == window) ReleaseCapture();
            }
            return 0;
        }
        if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
            (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE))
        {
            state->dragging = false;
            if (message != WM_CAPTURECHANGED && GetCapture() == window) ReleaseCapture();
        }
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return 1;
    switch (message)
    {
    case WM_CLOSE:
        if (state) state->closeRequested = true;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND find_game_window()
{
    HWND found = nullptr;
    EnumWindows([](HWND window, LPARAM param) -> BOOL
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        wchar_t name[80]{};
        GetClassNameW(window, name, 80);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window) && wcscmp(name, L"grcWindow") == 0)
        {
            *reinterpret_cast<HWND*>(param) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

void enqueue(Action action, std::string model = {})
{
    std::lock_guard lock(g_dataMutex);
    if (g_commands.size() < 16) g_commands.push_back({ action, std::move(model) });
}

std::string display_model_label(const std::string& modelId)
{
    std::string label = modelId;
    constexpr std::string_view marker = "_18plus";
    std::size_t position = 0;
    while ((position = label.find(marker, position)) != std::string::npos)
    {
        label.replace(position, marker.size(), " 18+");
        position += 5;
    }
    return label;
}

std::string friendly_blacklist_label(std::string_view item)
{
    const auto hash = parse_model_hash(item);
    if (hash == 0x0D7114C9U) return "player_zero (Michael)";
    if (hash == 0x9B22DBAFU) return "player_one (Franklin)";
    if (hash == 0x9B810FA2U) return "player_two (Trevor)";
    if (hash == 0x705E61F2U) return "mp_m_freemode_01 (Male)";
    if (hash == 0x9C9EFFD8U) return "mp_f_freemode_01 (Female)";
    return std::string(item);
}

void draw_menu(bool& open, WindowState& state, const ImVec2& menuSize)
{
    Snapshot snapshot;
    { std::lock_guard lock(g_dataMutex); snapshot = g_snapshot; }
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize(menuSize);
    constexpr auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Client Model Changer", &open, flags))
    {
        ImGui::End();
        return;
    }
    state.titleHeight = static_cast<int>(ImGui::GetFrameHeight());
    state.titleDragRight = static_cast<int>(ImGui::GetWindowWidth() - ImGui::GetFrameHeight() -
        ImGui::GetStyle().FramePadding.x);

    bool enabled = snapshot.enabled;
    if (ImGui::Checkbox("Enable", &enabled)) enqueue(Action::toggle);

    if (snapshot.pending)
    {
        ImGui::SameLine();
        ImGui::TextColored({ 0.85f, 0.65f, 1.0f, 1.0f }, "(Syncing...)");
    }

    ImGui::Spacing();

    if (ImGui::BeginTabBar("Pages"))
    {
        if (ImGui::BeginTabItem("Installed models"))
        {
            if (ImGui::BeginChild("Models", { 0, -ImGui::GetFrameHeightWithSpacing() }, ImGuiChildFlags_Borders))
            {
                if (snapshot.models.empty()) ImGui::TextWrapped("No model folders found in Models path.");
                for (const auto& model : snapshot.models)
                {
                    const auto label = display_model_label(model);
                    ImGui::PushID(model.c_str());
                    if (ImGui::Selectable(label.c_str(), model == snapshot.model)) enqueue(Action::select_model, model);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Community"))
        {
            ImGui::TextDisabled("ID                 Name                         Model");
            if (ImGui::BeginChild("Peers", { 0, -ImGui::GetFrameHeightWithSpacing() }, ImGuiChildFlags_Borders))
            {
                if (snapshot.peers.empty()) ImGui::TextUnformatted("No peer reports available.");
                for (const auto& peer : snapshot.peers)
                {
                    const auto label = display_model_label(peer.model);
                    ImGui::Text("%-18d %-28s %s", peer.serverId, label.c_str(),
                        peer.installed ? "installed" : "missing");
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings"))
        {
            if (ImGui::BeginChild("SettingsChild", { 0, -ImGui::GetFrameHeightWithSpacing() }, ImGuiChildFlags_Borders))
            {

                ImGui::TextUnformatted("Startup");
                ImGui::Separator();
                bool defEnabled = snapshot.defaultEnabled;
                if (ImGui::Checkbox("Enable custom model on startup", &defEnabled))
                {
                    enqueue(Action::set_default_enabled, defEnabled ? "1" : "0");
                }
                ImGui::Spacing();


                ImGui::TextUnformatted("Model Sync");
                ImGui::Separator();
                const auto currentLabel = snapshot.currentLocalModel.empty() ? "none" : snapshot.currentLocalModel;
                const auto spoofedLabel = snapshot.spoofedRemoteModel.empty() ? "mp_m_freemode_01" : snapshot.spoofedRemoteModel;
                ImGui::Text("In-Game:  %s", display_model_label(currentLabel).c_str());
                if (snapshot.customServerModelEnabled)
                {
                    ImGui::Text("Server:   %s (Custom)", display_model_label(spoofedLabel).c_str());
                }
                else
                {
                    ImGui::Text("Server:   %s (Auto)", display_model_label(spoofedLabel).c_str());
                }
                ImGui::Spacing();


                bool blockStory = snapshot.blockStoryModels;
                if (ImGui::Checkbox("Block GTA V story models", &blockStory))
                {
                    enqueue(Action::set_block_story_models, blockStory ? "1" : "0");
                }


                bool customServer = snapshot.customServerModelEnabled;
                if (ImGui::Checkbox("Override spoofed server model", &customServer))
                {
                    enqueue(Action::set_custom_server_model_enabled, customServer ? "1" : "0");
                }

                if (customServer)
                {
                    static std::array<char, 128> customModelBuffer{};
                    static std::string displayedCustomModel;
                    static bool editingCustom = false;
                    if (!editingCustom && displayedCustomModel != snapshot.customServerModel)
                    {
                        displayedCustomModel = snapshot.customServerModel;
                        const auto count = (std::min)(displayedCustomModel.size(), customModelBuffer.size() - 1);
                        std::memcpy(customModelBuffer.data(), displayedCustomModel.data(), count);
                        customModelBuffer[count] = '\0';
                    }

                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.f);
                    const bool submitted = ImGui::InputTextWithHint("##custom_server_model",
                        "e.g. mp_m_freemode_01", customModelBuffer.data(), customModelBuffer.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    editingCustom = ImGui::IsItemActive();
                    ImGui::SameLine();
                    if (ImGui::Button("Set", { 60.f, 0 }) || submitted || ImGui::IsItemDeactivatedAfterEdit())
                    {
                        displayedCustomModel = trim(customModelBuffer.data());
                        enqueue(Action::set_custom_server_model, displayedCustomModel);
                    }

                    if (ImGui::SmallButton("mp_m_freemode_01"))
                    {
                        displayedCustomModel = "mp_m_freemode_01";
                        const auto count = (std::min)(displayedCustomModel.size(), customModelBuffer.size() - 1);
                        std::memcpy(customModelBuffer.data(), displayedCustomModel.data(), count);
                        customModelBuffer[count] = '\0';
                        enqueue(Action::set_custom_server_model, displayedCustomModel);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("mp_f_freemode_01"))
                    {
                        displayedCustomModel = "mp_f_freemode_01";
                        const auto count = (std::min)(displayedCustomModel.size(), customModelBuffer.size() - 1);
                        std::memcpy(customModelBuffer.data(), displayedCustomModel.data(), count);
                        customModelBuffer[count] = '\0';
                        enqueue(Action::set_custom_server_model, displayedCustomModel);
                    }
                }
                ImGui::Spacing();

                if (ImGui::Button("Reload Config"))
                {
                    enqueue(Action::reload_config);
                }
                ImGui::Spacing();


                ImGui::TextUnformatted("Models Directory");
                ImGui::Separator();
                static std::array<char, 4096> pathBuffer{};
                static std::string displayedPath;
                static bool editingPath = false;
                if (!editingPath && displayedPath != snapshot.modelsPath)
                {
                    displayedPath = snapshot.modelsPath;
                    const auto count = std::min(displayedPath.size(), pathBuffer.size() - 1);
                    std::memcpy(pathBuffer.data(), displayedPath.data(), count);
                    pathBuffer[count] = '\0';
                }
                ImGui::SetNextItemWidth(-1.f);
                const bool pathSubmitted = ImGui::InputTextWithHint("##models_path",
                    "Folder containing model folders", pathBuffer.data(), pathBuffer.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                editingPath = ImGui::IsItemActive();
                if (pathSubmitted || ImGui::IsItemDeactivatedAfterEdit())
                {
                    displayedPath = pathBuffer.data();
                    enqueue(Action::set_models_path, displayedPath);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("F4 Menu   /   F5 Toggle   /   F7 Reload");
    ImGui::End();
}

void run_window(HMODULE module)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(comResult, "Overlay CoInitializeEx");
    struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) throw std::runtime_error("Overlay RegisterClassEx failed");
    struct ClassScope
    {
        HMODULE module;
        ~ClassScope()
        {
            UnregisterClassW(kWindowClass, module);
        }
    } classScope{ module };
    WindowState state;
#ifdef MODELCUSTOM_OVERLAY_TEST
    constexpr DWORD extendedWindowStyle = WS_EX_TOOLWINDOW;
#else
    constexpr DWORD extendedWindowStyle = WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
#endif
    HWND window = CreateWindowExW(extendedWindowStyle, kWindowClass,
        L"Client Model Changer", WS_POPUP, 0, 0, 620, 490, nullptr, nullptr, module, &state);
    if (!window) throw std::runtime_error("Overlay CreateWindowEx failed");
    struct WindowScope { HWND window; ~WindowScope() { DestroyWindow(window); } } windowScope{ window };
    Renderer renderer;
    renderer.initialize(window);
    IMGUI_CHECKVERSION();
    state.gui = ImGui::CreateContext();
    struct ImGuiScope
    {
        WindowState& state;
        bool win32 = false, dx11 = false;
        ~ImGuiScope()
        {
            ImGui::SetCurrentContext(state.gui);
            if (dx11) ImGui_ImplDX11_Shutdown();
            if (win32) ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(state.gui);
            state.gui = nullptr;
        }
    } imguiScope{ state };
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    imguiScope.win32 = ImGui_ImplWin32_Init(window);
    if (!imguiScope.win32) throw std::runtime_error("ImGui Win32 initialization failed");
    imguiScope.dx11 = ImGui_ImplDX11_Init(renderer.device.Get(), renderer.context.Get());
    if (!imguiScope.dx11) throw std::runtime_error("ImGui DX11 initialization failed");
    log("Overlay ready: own Win32/DX11/DirectComposition window; F4 toggles menu; no Present hook");

    HWND game = nullptr;
    bool open = false, wasF4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    float lastScale = 0;
    PanelPosition position;
    while (!g_stop)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) g_stop = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (g_stop) break;
        if (!IsWindow(game)) game = find_game_window();
        const HWND foreground = GetForegroundWindow();
#ifdef MODELCUSTOM_OVERLAY_TEST
        const bool forceOpen = g_testForceOpen.load();
#else
        constexpr bool forceOpen = false;
#endif
        const bool active = forceOpen || (game && (foreground == game || foreground == window) && !IsIconic(game));
        const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (active && f4 && !wasF4) open = !open;
        if (forceOpen) open = true;
        wasF4 = f4;
        if (!active || state.closeRequested || (open && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))) open = false;
        state.closeRequested = false;

        if (open)
        {
            RECT rect{};
            POINT origin{};
            if (!GetClientRect(game, &rect) || !ClientToScreen(game, &origin) || rect.right < 100 || rect.bottom < 100)
                open = false;
            else
            {
                const float scale = std::max(0.75f, GetDpiForWindow(game) / 96.f);
                const int menuWidth = std::min(static_cast<int>(620 * scale), static_cast<int>(rect.right));
                const int menuHeight = std::min(static_cast<int>(490 * scale), static_cast<int>(rect.bottom));
                const int width = menuWidth;
                const int height = menuHeight;
                if (state.positionChanged)
                {
                    position.move_to_screen(state.requestedPosition, origin, scale);
                    state.positionChanged = false;
                }
                const auto panel = position.resolve(origin, rect.right, rect.bottom, width, height, scale);
                const int x = panel.x;
                const int y = panel.y;
                RECT current{};
                GetWindowRect(window, &current);
                if (current.left != x || current.top != y || current.right - current.left != width ||
                    current.bottom - current.top != height || !IsWindowVisible(window))
                    SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
                if (!IsWindowVisible(window))
                {
                    ShowWindow(window, SW_SHOW);
                    SetForegroundWindow(window);
                    SetFocus(window);
                }
                ImGui::SetCurrentContext(state.gui);
                if (lastScale != scale)
                {
                    auto& style = ImGui::GetStyle();
                    style = ImGuiStyle{};
                    ImGui::StyleColorsDark();
                    style.ScaleAllSizes(scale);
                    style.FontScaleDpi = scale;
                    lastScale = scale;
                }
                renderer.resize(static_cast<UINT>(width), static_cast<UINT>(height));
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                draw_menu(open, state, { static_cast<float>(menuWidth), static_cast<float>(menuHeight) });
                ImGui::Render();
                renderer.render();
                check(DwmFlush(), "DwmFlush");
            }
        }
        g_capture = open && active;
        if (!open && IsWindowVisible(window))
        {
            state.dragging = false;
            if (GetCapture() == window) ReleaseCapture();
            ShowWindow(window, SW_HIDE);
            if (GetForegroundWindow() == window || foreground == window)
                SetForegroundWindow(game);
        }
        if (!open)
            MsgWaitForMultipleObjectsEx(0, nullptr, 60, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    g_capture = false;
}

DWORD WINAPI overlay_thread(void* parameter)
{
    HMODULE module = static_cast<HMODULE>(parameter);
    try { run_window(module); }
    catch (const std::exception& exception) { log_error(std::string("Overlay disabled: ") + exception.what()); }
    catch (...) { log_error("Overlay disabled: unexpected error"); }
    g_capture = false;
    g_running = false;
    log("Overlay thread stopped");
    FreeLibraryAndExitThread(module, 0);
}
}

bool start(HMODULE module)
{
    if (g_thread) return g_running;
    HMODULE reference = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &reference)) return false;
    g_stop = false;
    g_running = true;
    g_thread = CreateThread(nullptr, 0, overlay_thread, reference, 0, nullptr);
    if (!g_thread)
    {
        g_running = false;
        FreeLibrary(reference);
        log_error("Could not start overlay thread");
        return false;
    }
    return true;
}

void request_stop() { g_stop = true; g_capture = false; }
void stop()
{
    request_stop();
    if (g_thread)
    {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
bool captures_input() { return g_capture; }
bool running() { return g_running; }
void publish(Snapshot snapshot)
{
    std::lock_guard lock(g_dataMutex);
    g_snapshot = std::move(snapshot);
}
std::vector<Command> take_commands()
{
    std::lock_guard lock(g_dataMutex);
    std::vector<Command> result;
    result.swap(g_commands);
    return result;
}
#ifdef MODELCUSTOM_OVERLAY_TEST
void test_force_open(bool open) { g_testForceOpen = open; }
#endif
}
