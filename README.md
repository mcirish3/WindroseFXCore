# WindroseFXCore

**A runtime CPU optimizer that helps Windrose run better on older PCs.**

WindroseFXCore doesn't change any graphics settings or game files. It's a small native DLL
that loads with the game and changes **how Windows schedules Windrose on your CPU**. It helps whatever
in-game settings you choose and works alongside FSR, DLSS, XeSS and frame generation.

The one exception is the optional **audio buffer fix** (off by default). When you turn it on, the mod writes
two audio lines to your user `Engine.ini`, backs up the original first, and restores it when you turn it off.
See [Audio dropouts or static](#audio-dropouts-or-static).

It was built for an AMD FX-8350 and is written to adapt to whatever CPU it finds.

> **Status: 1.0.2-beta.** It builds and loads correctly, but has had limited real-world testing so far.
> Please report your results (good or bad) with your log file. See [Reporting results](#reporting-results).

---

## Who it's for

Windrose is an Unreal Engine 5 game that leans heavily on the CPU. If your GPU is fine but the game
stutters, hitches in busy areas or drops frames in naval combat, your CPU is probably the bottleneck,
and this mod targets exactly that.

| CPU family | What the mod does |
|---|---|
| **AMD FX** (FX-4xxx/6xxx/8xxx/9xxx) | Full benefit. Separates the frame-critical threads onto different modules so they stop competing for each module's shared FPU, L2 and decoder. |
| **Older Intel Core i5/i7 with Hyper-Threading** (2nd–9th gen) | Keeps critical threads off each other's hyperthreads and keeps background work away from them. |
| **Ryzen 1000/2000/3000** | Same as above, and keeps critical threads inside one CCX to avoid cross-CCX latency. |
| **Intel 12th gen and newer** (P-cores and E-cores) | Places critical threads on P-cores. |
| **Dual cores, or quad cores without SMT** | Thread placement switches itself off, since there aren't enough cores to spare. The timer, priority and power fixes still apply. |

## What it does

| Fix | Why it helps |
|---|---|
| **Topology-aware thread placement** | UE5's GameThread, RenderThread and RHIThread decide your frame rate. When two of them land on the same FX module or the same hyperthreaded core, they slow each other down. The mod reads your CPU's real cache layout at launch and gives each one its own module or core. |
| **Background worker isolation** | UE's background pool (asset streaming, decompression) is kept off the critical cores, so a burst of loading can't preempt the threads that build each frame. Most of the pool's capacity stays available. |
| **Audio protection** | The audio mixer runs at time-critical priority on its own core, kept clear of background workers, so it doesn't miss audio buffers when the CPU is maxed out. |
| **Audio buffer fix** (optional, off by default) | Raises UE's audio buffer size, which isn't in Windrose's menus. This is what fixed dropouts and static on the FX-8350 test PC. See [Audio dropouts or static](#audio-dropouts-or-static). |
| **Priority floors** | Critical threads run at least at Above Normal priority. |
| **0.5 ms timer resolution** | Windows' default timer can make UE's frame limiter and thread waits overshoot by up to ~15 ms. That shows up as uneven frame pacing. |
| **Power-throttling opt-out** | Stops Windows from applying EcoQoS or timer coalescing to the game. |

**What to expect:** steadier frame times and fewer CPU-side hitches. It won't turn an old CPU into a new one,
and it can't help if your GPU is the bottleneck. Lowering resolution or using FSR handles that side.

## Install

1. Download the latest zip from [Nexus Mods](https://www.nexusmods.com/windrose/mods/574) or [Releases](../../releases).
2. In Steam, right-click **Windrose → Manage → Browse local files**.
3. Extract the zip into that folder. The files land in `R5\Binaries\Win64\`, next to the game's
   `...-Win64-Shipping.exe`:
   - `version.dll`
   - `WindroseFXCore.ini`
4. Launch the game normally.

**Vortex** users can install the [Nexus download](https://www.nexusmods.com/windrose/mods/574) through Vortex as usual.

## Check that it's working

After launching, open `R5\Binaries\Win64\WindroseFXCore.log`. On an FX-8350 it looks like this:

```
WindroseFXCore 1.0.2-beta loaded into ...\Windrose-Win64-Shipping.exe
Audio buffer: set 2048 frames x 4 buffers in Engine.ini      (only with AudioBufferFix=1)
CPU: AMD FX(tm)-8350 Eight-Core Processor
Topology: 8 logical processors, 4 execution groups, 1 L3 domain(s)
Timer resolution: 0.500 ms
Power throttling opt-out: ok (execution speed only)
Process priority: raised
Critical LPs: game 0, render 2, rhi 4, audio 6 | background mask 0xaa
  GameThread       tid  ...  prio 1
  RenderThread     tid  ...  prio 1
  RHIThread        tid  ...  prio 1
  AudioMixer       tid  ...  prio 15
```

If there's no log file at all, the DLL isn't loading. Please open an issue.

## Configure

Every fix can be turned off on its own in `WindroseFXCore.ini`. That's the easiest way to A/B test:
turn one off, play the same stretch, and compare.

| Setting | Default | Notes |
|---|---|---|
| `Enabled` | 1 | Master switch |
| `TimerResolution100ns` | 5000 | 0.5 ms. `10000` = 1 ms, `0` = leave alone |
| `DisablePowerThrottling` | 1 | |
| `ProcessPriority` | 1 | 0 Normal, 1 Above Normal, 2 High (not recommended) |
| `PlaceCriticalThreads` | 1 | |
| `IsolateBackgroundWorkers` | 1 | |
| `BoostCriticalPriority` | 1 | |
| `BoostAudioPriority` | 1 | |
| `AudioMixerPriority` | 15 | 15 = time-critical, 2 = highest |
| `PlaceAudioThread` | 1 | Gives the audio mixer its own core, clear of background workers |
| `AudioBufferFix` | 0 | Writes the audio buffer size to `Engine.ini`. See below |
| `AudioBufferFrames` | 2048 | Frames per buffer (256–8192) |
| `AudioBuffersToEnqueue` | 4 | Buffers queued ahead (2–8) |
| `VerboseLog` | 0 | Logs every worker thread too |
| `PerfLogSeconds` | 0 | Diagnostics: every N seconds, logs CPU use of the critical threads and workers (100% = one full core). `5` is a good value while testing |

## Audio dropouts or static

If Windrose's audio drops out or crackles, set `AudioBufferFix=1` in `WindroseFXCore.ini` and launch the game.

- At every launch the mod writes these lines to `%LOCALAPPDATA%\R5\Saved\Config\Windows\Engine.ini`
  (the game deletes the file again after reading it, so they're written each time):
  ```
  [/Script/WindowsTargetPlatform.WindowsTargetSettings]
  AudioCallbackBufferFrameSize=2048
  AudioNumBuffersToEnqueue=4
  ```
- The first time, an existing `Engine.ini` is copied to `Engine.ini.WindroseFXCore.bak`, and the original values of
  those two keys are recorded in `WindroseFXCore.state` next to the DLL.
- **To undo:** set `AudioBufferFix=0` (or `Enabled=0`) and launch once. The original values are put back, and
  `Engine.ini` is removed if the mod created it.
- If it still crackles, try `AudioBufferFrames=4096`. A bigger buffer adds some audio delay
  (2048 × 4 at 48 kHz is up to about 0.17 s). If you notice the delay, lower `AudioBuffersToEnqueue` to 2 or 3.

None of this is in Windrose's settings menu, so it doesn't override anything you can set yourself.

## Uninstall

1. If you turned on `AudioBufferFix`, set it to `0` and launch the game once so your `Engine.ini` is restored.
2. Delete `version.dll`, `WindroseFXCore.ini`, `WindroseFXCore.log` and `WindroseFXCore.state` (if present) from `R5\Binaries\Win64`.

Without the audio buffer fix, nothing outside the mod's own files is changed.

## FAQ

**Is it safe online?** Windrose is PvE and had no anti-cheat when this was written. If a future patch
adds anti-cheat, remove the DLL before playing online. This mod is not intended for use with anti-cheat.

**Will it get my saves or settings messed up?** No. It never touches saves or game files, and it only touches
`Engine.ini` if you turn on `AudioBufferFix`, with a backup and automatic restore.

**Does it work with other mods?** Yes, unless another mod also uses `version.dll` as its loader. In that case
only one can be installed at a time.

**The game won't start after an update.** Delete `version.dll` to rule it out, then open an issue with the game version.

## Reporting results

Open an [issue](../../issues) with:
- your CPU, GPU and RAM
- your `WindroseFXCore.log`
- what changed: average FPS, stutter, and where in the game you tested

Before/after numbers from a tool like [PresentMon](https://github.com/GameTechDev/PresentMon) or
MSI Afterburner (1% lows matter most) are especially useful.

## Building from source

The mod is plain C with no dependencies.

**With Zig (Windows, Linux or macOS):**
```
pip install ziglang
python -m ziglang cc -target x86_64-windows-gnu -O2 -shared -o version.dll src/version_proxy.c src/fxcore.c src/version.def -ladvapi32 -s
```
Or run `build.bat` (Windows) or `build.sh` (Linux/macOS).

**Making a release:** add a section to `CHANGELOG.md`, then run **Actions → release → Run workflow** with the
version number (or push a tag like `v1.0.1`). The workflow builds the DLL, packages the zip and publishes the release.
Versions containing a hyphen (`1.1.0-beta`) are marked as pre-releases.

**How it loads:** Unreal Engine's Core module links `version.dll`, so the game loads it from its own folder first.
`src/version_proxy.c` forwards all 17 real exports to `System32\version.dll`, and `src/fxcore.c` starts one
background thread that does the work (the optional Engine.ini write happens right at load, before the engine reads
its config). It only activates inside an executable whose name contains `Shipping`,
so the crash reporter and other tools are never affected.

## License

MIT. See [LICENSE](LICENSE).

Created by Patrick McCoy, with Claude (Anthropic).
