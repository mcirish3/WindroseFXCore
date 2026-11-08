# Changelog

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
