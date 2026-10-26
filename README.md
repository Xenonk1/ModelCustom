# ModelCustom

[![Build and test](https://github.com/Xenonk1/ModelCustom/actions/workflows/ci.yml/badge.svg)](https://github.com/Xenonk1/ModelCustom/actions/workflows/ci.yml)

Custom player models for FiveM, loaded from your own local files.

A native Windows x64 ASI plugin with an F4 menu. Choose an add-on ped, toggle
it with F5, and see other plugin users' models when you share the same relay
and have the same model packs installed. No FiveM server resource is required.

**C++20 · DirectX 11 · Dear ImGui · optional WebSocket relay**

[Download v1.0.0](https://github.com/Xenonk1/ModelCustom/releases/tag/v1.0.0) ·
[Model setup](models/README.md) · [Relay setup](relay/README.md) ·
[Changelog](CHANGELOG.md)

This repository contains source code. Build outputs are distributed separately
through GitHub Releases or CI artifacts.

## Install

1. Download `ModelCustom-v1.0.0-win64.zip` from [Releases](https://github.com/Xenonk1/ModelCustom/releases/latest), or [build it yourself](#build).
2. Close FiveM. Extract the ZIP into `%LOCALAPPDATA%\FiveM\FiveM.app`.
3. Put your model folders in `FiveM.app\plugins\models`.
4. Start FiveM, press **F4**, and select a model.

The installed layout is:

```text
FiveM.app/
└── plugins/
    ├── ModelCustom.asi
    ├── ModelCustom.ini
    └── models/
        └── my_ped/
            ├── model.txt
            ├── peds.meta
            ├── my_ped.yft
            ├── my_ped.ydd
            ├── my_ped.ytd
            └── my_ped.ymt
```

`model.txt` contains the model's actual internal name, for example `my_ped`.
Use the metadata and assets supplied with that model. See
[model folders](models/README.md) for details. Character models are supplied separately.

The release includes a `.sha256` checksum file. To check the ZIP in PowerShell:

```powershell
Get-FileHash .\ModelCustom-v1.0.0-win64.zip -Algorithm SHA256
```

Compare the result with the digest in `ModelCustom-v1.0.0-win64.zip.sha256`.

## Controls

| Key | Action |
| --- | --- |
| **F4** | Open or close the menu |
| **F5** | Enable or disable the selected model |
| **F7** | Reload model selection and settings from the INI |

Selecting a model in the menu enables it. The menu also lets you change the
models directory and choose whether the selected model starts enabled.

## Seeing other players

Each client loads its own assets. ModelCustom keeps a standard model hash in
the outgoing player sync data while rendering the custom model locally.
If the required sync hooks cannot be resolved, the model change is held back.

The optional relay announces which model each plugin user has selected.
Clients on the same FiveM server are grouped into a room. A remote custom
model is used only when its ID, pack version and asset SHA-256 match a local
pack. Otherwise, that player keeps their regular appearance.

To use this with friends:

1. Deploy the [included relay](relay/README.md).
2. Set the same `relay_url` in every client's INI and restart FiveM.
3. Join the same server and install identical copies of the models you want to see.

The relay sends presence metadata over WebSocket. It does not distribute model
files or proxy FiveM traffic. Players without the plugin see the standard model.
With `relay_url` empty or left as `YOUR_ENDPOINT`, the plugin works locally.

## Configuration

Edit `plugins\ModelCustom.ini`, or change model settings in the menu:

```ini
models_dir=models
folder=
default_enabled=0
replace_model=mp_m_freemode_01
relay_url=YOUR_ENDPOINT
```

Paths are relative to the ASI directory; absolute paths and environment
variables such as `%LOCALAPPDATA%` also work. `folder` is an optional startup
selection, for example `models\my_ped`. Leave it empty to choose in F4.

Replace `YOUR_ENDPOINT` with your relay's HTTPS URL to enable peer discovery.

`replace_model` is the fallback until the player's original server model is
captured. `room_key` optionally overrides automatic server grouping; leave
it unset for normal use. Restart FiveM after changing relay settings or assets.

Legacy `model.txt` names and encoded `models.txt` names are both supported.
Component-only clothing packs are local presets and are not announced to peers.

## Build

Requires Windows, Visual Studio 2022 or Build Tools with **Desktop development
with C++**, **MSVC v143**, and a Windows SDK. Dependencies for the native plugin
are included; no GTA SDK download is needed.

```powershell
.\build.ps1
.\test.ps1
.\package.ps1 -Version 1.0.0
```

- Plugin: `build\Release\ModelCustom.asi`
- Release ZIP and SHA-256: `dist\ModelCustom-v1.0.0-win64.zip` and `.zip.sha256`
- Visual Studio solution: `ModelCustom.sln`

To install a local build while FiveM is closed:

```powershell
.\install.ps1
.\install.ps1 -FiveMApp 'D:\FiveM\FiveM.app'
```

The installer keeps an existing INI. Packaging includes the plugin, a clean
configuration and dependency notices; it excludes local model assets.

## Source layout

| Path | Contents |
| --- | --- |
| `src/` | Plugin runtime, model loading, streaming, sync and menu |
| `relay/` | Cloudflare Worker, Durable Object rooms and protocol tests |
| `tests/` | Native core tests and WinHTTP relay integration test |
| `tools/` | MSBuild discovery and optional model-name encoder |
| `third_party/imgui/` | Vendored Dear ImGui and Win32/DX11 backends |

[Developer notes](docs/development.md) cover the runtime and test commands.

## Troubleshooting

- **Menu does not open:** check that ASI plugins are allowed by your FiveM
  installation and server settings. Look for `ModelCustom.log` beside the ASI.
- **No models listed:** check `models_dir`. Each model needs its own subfolder
  with the original assets and a correct internal name.
- **A friend's model stays standard:** use the same relay, join the same server,
  and compare the model files. Different packs with the same name do not match.
- **Changed files do not appear:** restart FiveM so streaming assets are loaded again.

The plugin targets FiveM's supported game builds and relies on internal runtime
interfaces. A successful build does not validate rendering in every game build;
verify the plugin in FiveM before distributing a new binary.

Third-party licenses and attribution are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
