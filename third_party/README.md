# Vendored dependencies

`imgui/` contains Dear ImGui **v1.92.5**, its core headers and sources,
and the official Win32 / DirectX 11 backends used by the menu.

Upstream: <https://github.com/ocornut/imgui/tree/v1.92.5>.
License: MIT; see [LICENSE.txt](imgui/LICENSE.txt).

The plugin uses Windows SDK libraries for graphics, HTTP/WebSockets and hashing.
It does not need ScriptHookV headers or a separate GTA SDK to compile.
