# Changelog

## 1.0.2-beta — 2026-11-08

Fixes audio dropouts and static, found by A/B testing on an FX-8350 (Windows 10).

- New, optional (off by default): `AudioBufferFix=1` raises UE's audio buffer size, which isn't in Windrose's menus. The mod writes `AudioCallbackBufferFrameSize` / `AudioNumBuffersToEnqueue` to the user `Engine.ini` at every launch, since the game removes them on exit. The original `Engine.ini` is backed up to `Engine.ini.WindroseFXCore.bak`, and setting `AudioBufferFix=0` (or `Enabled=0`) restores the original values on the next launch. Size is set with `AudioBufferFrames` (default 2048) and `AudioBuffersToEnqueue` (default 4).
- Tested and ruled out: turning off `IsolateBackgroundWorkers` made no meaningful difference to the audio, so it stays on by default. The buffer size alone fixed it.
- The log now says when the background mask isn't applied (`IsolateBackgroundWorkers=0`).
- This is the first version that can change a config file, and only when `AudioBufferFix` is on. The README and uninstall steps are updated to match.

## 1.0.1-beta — 2026-11-07

Based on the first real-world log (FX-8350, Windows 10).

- Audio: the mixer thread now gets its own preferred core, kept clear of background workers, and runs at time-critical priority (configurable with `AudioMixerPriority` and `PlaceAudioThread`). Previously its core was shared with the background pool.
- Power-throttling opt-out now works on Windows 10. It previously requested a Windows 11-only option and was rejected.
- The mod's own scanning thread runs at below-normal priority so it can never compete with the game.

## 1.0.0-beta — 2026-11-07

First public release.

- version.dll proxy loader for Windrose (Unreal Engine 5), active only inside the Shipping executable
- Topology-aware placement of GameThread, RenderThread and RHIThread
  - AMD FX modules, SMT cores, Ryzen multi-CCX (stays in one L3 domain) and Intel hybrid (prefers P-cores)
  - Switches itself off on CPUs with fewer than 3 spare core groups
- Background worker pool kept off the critical cores
- Priority floors for critical threads and the audio mixer
- 0.5 ms system timer resolution
- Power throttling (EcoQoS) opt-out
- Per-feature switches in WindroseFXCore.ini; diagnostic log in WindroseFXCore.log
