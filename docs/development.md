# Development

## Runtime

`main.cpp` connects to FiveM's exported game-frame event. Game natives and model
state stay on that thread. `overlay.cpp` uses an independent Win32 / DirectX 11
window and exchanges copies of UI snapshots and commands with the runtime.

`streaming_bridge.cpp` resolves FiveM streaming exports and registers the local
RSC7 assets and `peds.meta`. `model_catalog.cpp` resolves model names and hashes
pack contents. `model_loader.cpp` handles selection, toggles, restoration,
component presets and peer models.

`network_model_spoof.cpp` supplies the standard server model hash in outgoing
player sync nodes. `peer_sync_hook.cpp` handles peer model state; the loader also
supports local, non-networked proxy peds. `relay_client.cpp` maintains a WinHTTP
WebSocket connection and reconnects with backoff, capped at 30 seconds.

The runtime code depends on FiveM exports and internal sync layouts. Keep game
calls on the game thread and preserve the existing model restoration paths.

## Native tests

```powershell
.\test.ps1
```

The core tests run without FiveM. They cover INI path resolution, model naming,
pack hashing, rejection of mismatched packs and RSC7 header validation. Fixtures
and temporary settings stay in `build\core-test`.

To test the actual C++ WebSocket client, start the relay in a separate terminal:

```powershell
cd relay
npm ci
npm run dev -- --port 8787
```

Then, from the repository root:

```powershell
.\test.ps1 -Relay -RelayUrl http://127.0.0.1:8787
```

Two native clients verify join snapshots, model updates, disabling and leaving.
The test connects only to the URL you supply; it does not launch FiveM.

## Relay tests

```powershell
cd relay
npm ci
npm run check
npm test
npm run dry-run
```

The relay tests exercise room isolation, token collision handling, validation,
legacy HTTP presence and pushed WebSocket snapshots. The dry run bundles the
Worker locally without deploying it.

## Release

```powershell
.\build.ps1
.\test.ps1
.\package.ps1 -Version 1.0.0
```

`package.ps1` builds Release and writes a `plugins/` ZIP plus a SHA-256 file in
`dist/`. Its explicit file list excludes local models, test programs, logs and
private configuration. The `Version` argument names the archive; it does not
change the relay protocol or the model-pack version.

GitHub Actions builds the Windows plugin, runs core and relay tests, and
uploads the distributable archive as a workflow artifact. Download that artifact
and attach its ZIP and checksum when creating a GitHub release.

Before publishing a binary, check in FiveM: F4 selection, F5 restoration, respawn,
server reconnect, and two clients with matching and mismatched model packs.
Offline tests do not establish in-game compatibility.
