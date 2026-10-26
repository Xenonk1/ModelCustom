# Changelog

## 1.0.0 — 2026-10-26

Initial public source release of ModelCustom for FiveM.

- Local add-on ped loading with an F4 model browser, F5 toggle and F7 reload.
- Configurable model directory, startup selection and server model fallback.
- Optional Cloudflare WebSocket relay for discovering other plugin users.
- Peer model checks against the installed model ID, pack version and SHA-256.
- Local component-pack support, separate from peer model discovery.
- Portable default configuration with a `YOUR_ENDPOINT` relay placeholder.
- Windows build, install, test and packaging scripts, plus GitHub Actions CI.

The Windows release ZIP contains the ASI plugin, configuration, installation
instructions and dependency notices. Model assets are supplied separately.

Runtime compatibility depends on FiveM's internal interfaces. Native and relay
tests run outside FiveM; this release has not been tested in a live game session.
