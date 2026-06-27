<div align="center">

# Silent Hill: Downpour — PC Port (DownpourRecomp)

### Play *Silent Hill: Downpour* natively on Windows, in 1080p, with keyboard + mouse — no emulator required.

[![Latest release](https://img.shields.io/github/v/release/LittleBitUA/DownpourRecomp?style=for-the-badge&label=Download&color=blue)](https://github.com/LittleBitUA/DownpourRecomp/releases/latest)
[![Total downloads](https://img.shields.io/github/downloads/LittleBitUA/DownpourRecomp/latest/total?style=for-the-badge&color=brightgreen)](https://github.com/LittleBitUA/DownpourRecomp/releases)
[![License](https://img.shields.io/github/license/LittleBitUA/DownpourRecomp?style=for-the-badge&color=lightgrey)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6?style=for-the-badge&logo=windows)](https://github.com/LittleBitUA/DownpourRecomp/releases/latest)
[![Stars](https://img.shields.io/github/stars/LittleBitUA/DownpourRecomp?style=for-the-badge&color=yellow)](https://github.com/LittleBitUA/DownpourRecomp/stargazers)

![Murphy close-up — Silent Hill: Downpour running natively on PC](docs/screenshots/murphy-closeup.png)

## [⬇  Download v1.0 for Windows](https://github.com/LittleBitUA/DownpourRecomp/releases/latest)

</div>

---

> [!NOTE]
> **v1.0 ships today.** Tested against the **USA** and **Europe** Xbox 360 releases of Silent Hill: Downpour (title id `4B4E0823`, base XEX hash `7A3D5809776EE6AB`). Title Update 1 is required and the launcher walks you through staging it on first run. Other regions / pirate dumps are untested.
>
> Massive thanks to everyone who tested v0.1.1 — every report, log, and screenshot fed directly into what shipped in v1.0.

---

## What's new in v1.0

### 🎮 Performance & framerate

- **60 FPS unlocked.** The Xbox 360 release ran the game thread at 30 FPS via a hardcoded UE3 frame-skip flag. v1.0 patches that flag at recompile time so the game logic, animation, physics, and rendering all run at the full 60 FPS your monitor can handle. Camera motion, mouse-look, and combat all feel native-PC instead of 360-locked.
- **ROV render path warm-cache.** Modern Nvidia (RTX 30-series and newer) and AMD (RDNA 2+) GPUs run the more accurate ROV (Rasterizer-Ordered Views) path *faster* than the legacy RTV path once the shader cache is warm. v1.0 ships pre-compiled ROV shaders so you get full GPU throughput from the first run.
- **Pre-warmed shader cache.** ~1,370 game shaders are packaged with the build and warm-loaded at startup. No 10-second "first-time-you-see-X stutter". One-off PSO compile-time storms on level load are eliminated.
- **Memexport readback de-flooding.** The SDK-level path that reads UE3's vertex-skinning and HUD-text memexport buffers back to the CPU was rewritten with a triple-buffered ring and targeted per-submission fence waits. Per-frame fence-wait counts in steady gameplay dropped from ~18 to 0.
- **Tuned texture cache.** Soft / hard limits raised to 3 GB / 6 GB with a 30-minute residence time, so streaming-heavy areas keep their textures resident instead of trashing the cache every camera pivot.

### 🖱️ Launcher & first-run experience

- **Standalone launcher (`PlayDownpour.exe`).** A native GUI launcher you open *before* the game starts — pick your game-data folder, set resolution / scale / FSR / colour-grade / keybinds / mouse sensitivity, then hit Play. No more editing TOML files by hand for first-time setup. Settings are round-tripped non-destructively so your manual `.toml` tweaks survive.
- **In-game settings overlay (`F4`).** All cvars also reachable mid-game via the existing F4 overlay, with hot-reload on most knobs.
- **Title Update installer fixed.** The "Start Game" button on the first-run wizard no longer hangs the window on Windows or Linux. The installer also picks up your existing TU data automatically if you point it at the right folder.
- **Portable layout.** Saves, runtime cache, and shader cache all live in the game folder by default (not `Documents/downpour`). Drop the folder on a USB stick, move it to another PC — your saves and warm shader cache come with you.
- **Linux / Wine support.** Replaced the Windows-only Bahnschrift / Segoe UI font dependency with a graceful fallback chain (Bahnschrift → Segoe → Tahoma → system sans-serif). Launcher UI is now legible in plain Proton / Wine without installing extra font packages.

### 🎨 Visuals

- **Native colour-grade post-FX.** A real ASC-CDL colour grade applied as the final blit, with 7 ship-ready presets — `identity`, `downpour_cinematic` (default), `downpour_horror`, `vivid`, `noir`, `warm_cinema`, `cold_steel`. Hot-reloadable; you can dial intensity 0.0 — 1.5 to taste.
- **Tuned 1080p defaults.** 2× supersampling (`resolution_scale = 2`), 16× anisotropic filtering, MSAA / FXAA off (supersampling already covers anti-aliasing), FSR3 for the final present. The Xbox 360's chromatic-noise bug on the fast render path remains fixed.
- **Localization-ready.** UI overlay and item text can be localized through the standard UE3 string tables — the v1.0 screenshots below show an in-progress Ukrainian translation as a proof of concept.

### 🛡️ Stability

- **Quieter logs.** PSO-stall log noise was cut ~10× by gating non-critical secondary metric warnings behind a `pso_stall_log_verbose` cvar (default off). Healthy frames no longer produce log spam; only frames above 50 ms produce a record.
- **VFS resilience.** The virtual file-system negative-result cache uses a surgical erase strategy on path creation, so transient `temp:\` / `save:\` lookups during a save operation no longer blow away 16k cached entries and cause a multi-second stutter.
- **No crashes / TDRs on the v1.0 path.** Extended play sessions on the development branch run without device-removed events on tested hardware (RTX 5070, Win 11).

### 🛠️ For modders & technical players

- **C++ source-level hooks** in the recompiled game logic (already in v0.1.1, kept and expanded).
- **Cache versioning.** PSO and shader caches are versioned per SDK build — your warm cache survives across patch updates as long as the SDK version key matches, and is regenerated cleanly when it doesn't.
- **Diagnostic overlay.** The green-panel debug HUD visible in the v1.0 screenshots below is shippable (`F4` to toggle) — PSO stall counts, fence waits, texture streaming, draw bucket breakdown, CPU/GPU phase timing.

---

## v1.0 preview

Captured 2026-06-27 from the development branch. The green panel on the left is the diagnostic overlay (`F4` to toggle); the small `60 FPS / 16.6 ms` counter in the top-right is what matters for the v1.0 perf target.

<div align="center">

| Murphy at the *Silent Hill* sign | Yard / twilight |
| --- | --- |
| ![Murphy at the Silent Hill sign, 60 FPS](docs/screenshots/v1-preview-silent-hill-sign.png) | ![Yard twilight, 60 FPS](docs/screenshots/v1-preview-yard-twilight.png) |

| Bedroom — Ukrainian localization in-game | Rusty key pickup — Ukrainian item description |
| --- | --- |
| ![Bedroom dialogue, 60 FPS, Ukrainian](docs/screenshots/v1-preview-bedroom-ua.png) | ![Rusty key pickup, 60 FPS, Ukrainian](docs/screenshots/v1-preview-rusty-key.png) |

</div>

---

## Table of contents

- [What's new in v1.0](#whats-new-in-v10)
- [v1.0 preview screenshots](#v10-preview)
- [What is this?](#what-is-this)
- [Why does this exist?](#why-does-this-exist)
- [Comparison vs Xenia and the Xbox 360](#comparison-vs-xenia-and-the-xbox-360)
- [What you need before playing](#what-you-need-before-playing)
- [How to install and play](#how-to-install-and-play)
- [Default controls](#default-controls)
- [Runtime configuration](#runtime-configuration)
- [Frequently asked questions](#frequently-asked-questions)
- [Building from source](#building-from-source)
- [Technical deep-dives](#technical-deep-dives)
- [Acknowledgements](#acknowledgements)
- [Legal](#legal)

---

## What is this?

**DownpourRecomp is a native Windows port of *Silent Hill: Downpour* (Konami / Vatra Games, Xbox 360, 2012).** The original Xbox 360 game is converted into a regular Windows program — once and for all, at build time — so it runs on your PC the same way as any other Windows app.

If you've used Xenia or RPCS3 before, this is **not** that. There is no emulator, no JIT translator running on every CPU instruction, no per-frame interpretation overhead. The PowerPC code in the original Xbox 360 binary is translated into native x86-64 C++ code ahead of time, and then linked against a small host runtime that handles the Xbox-specific parts (input, kernel calls, GPU command processor, EDRAM). The result is a real `downpour.exe` that boots like any other game.

This technique is called **static recompilation**. It's the same idea behind:
- [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) (the project that started the trend),
- [Sonic Mania: Recompiled](https://github.com/SonicMania-Recompiled/Sonic-Mania-Recompiled),
- [Skate 3 Recomp](https://github.com/Sergeanur/Skate3Recomp),
- [DPRecomp](https://github.com/LittleBitUA/DPRecomp) (the same author's *Deadly Premonition* port).

DownpourRecomp uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — a Xenia-derived Xbox 360 host runtime — as the foundation, and adds the Silent Hill: Downpour-specific glue and a narrow GPU fix that makes the fast render path produce a clean image.

> [!NOTE]
> **You provide your own legally-owned copy of the game.** The release zip is the host shell only (~27 MiB). It does not contain `default.xex`, game data, music, or any Konami / Vatra assets. See [What you need before playing](#what-you-need-before-playing).

---

## Why does this exist?

*Silent Hill: Downpour* never received an official PC release. On modern hardware your three options are:

1. **Real Xbox 360 hardware.** Works, but ageing — disc reads fail, HDMI is 720p tops, no improvements over 2012.
2. **Xenia emulator** ([xenia-canary](https://github.com/xenia-canary/xenia-canary)). Works for Downpour visually but with the overhead of dynamic translation and the dependency on Xenia's evolution.
3. **DownpourRecomp** (this project). A native Windows process, full mouse + keyboard support, the chromatic-noise artifact on the fast render path fixed, and the entire CPU side running natively at x86-64 speed.

Option 3 is what this project exists to be.

---

## Comparison vs Xenia and the Xbox 360

> [!NOTE]
> The "DownpourRecomp" column below describes **v1.0** (the current release). See [What's new in v1.0](#whats-new-in-v10) above for the full feature list.

| | Xbox 360 (original) | Xenia emulator | **DownpourRecomp v1.0** |
| --- | --- | --- | --- |
| **Resolution** | 720p (HDMI) | up to 4K (DSR) | up to 4K, native 1080p default + 2× SSAA |
| **Frame rate** | 30 FPS | 30 FPS (UE3 cap) | **60 FPS unlocked** (recompile-time patch) |
| **Input** | Xbox 360 controller only | XInput controllers | XInput + **native mouse & keyboard** |
| **Settings UI** | in-game options menu | Xenia F11 / config files | **standalone launcher + in-game F4 overlay** |
| **Disc / file requirement** | Original disc | Dumped XEX + game data | Dumped XEX + game data |
| **GPU rendering** | Xbox 360 GPU | Xenia D3D12 / Vulkan | Xenia D3D12 **ROV path** (ported, default on RTX 30+/RDNA 2+) |
| **CPU execution** | PowerPC native | Dynamic recompiler (JIT) | **Statically recompiled to native x86-64** |
| **CPU overhead** | none | per-frame translation | **none** |
| **Shader cache** | n/a (native HW) | rebuilds per-driver-update | **ships pre-warmed** (~170 MB, ~1,370 PSOs) |
| **First-run stutter** | n/a | tens of seconds of compile spikes | **eliminated** (warm cache + memexport readback de-flood) |
| **Colour grade** | as authored | none | **7 ASC-CDL presets** (cinematic / horror / vivid / noir / …) |
| **Linux / Wine support** | n/a | native Linux build | **runs in Proton / Wine** (font fallback for non-Windows systems) |
| **Portable layout** | n/a | %AppData% / OS paths | **saves + cache stay in the game folder** |
| **Chromatic-noise bug** | n/a (clean) | clean | **fixed** (see [docs](docs/chromatic-noise-fix.md)) |
| **Modding hooks** | none | limited | C++ source-level hooks |
| **Install size** | disc only | ~80 MiB + your dump | ~200 MiB (incl. pre-warmed shader cache) + your dump |

---

## What you need before playing

The download is **the application only**. You bring the game. To play, you need:

1. **A legally-owned copy of *Silent Hill: Downpour* for Xbox 360** — disc, digital download, or backup of either. Region USA or EUR (`title id 4B4E0823`, base XEX hash `7A3D5809776EE6AB`). Other regions are untested.
2. **Your own dumped `default.xex`** extracted from that copy.
3. **The full game data tree** — `nxeart`, `SHGame/`, `AvatarAssetPack/`, and the rest of the disc's files. The game streams content (audio, levels, scripts) from disk at runtime, so the XEX alone is not enough.
4. A modern Windows PC: Windows 10 or 11 (x86-64), a D3D12-capable discrete GPU (RTX 30-series or equivalent, 6+ GB VRAM recommended).

> [!IMPORTANT]
> Do not ask in the issue tracker or anywhere else where to get the XEX. Bring your own legally-acquired copy.

---

## How to install and play

1. **Download** the latest release zip: [DownpourRecomp v1.0 →](https://github.com/LittleBitUA/DownpourRecomp/releases/latest)
2. **Extract** the zip somewhere with read/write access (e.g. `C:\Games\DownpourRecomp\`).
3. **Put your game files** into an `assets/` folder next to `downpour.exe`. The expected layout:

   ```
   C:\Games\DownpourRecomp\
     downpour.exe
     rexruntimerd.dll
     TracyClientrd.dll
     downpour.toml         ← rename from downpour.toml.sample
     start.bat
     assets\
       default.xex         ← your XEX, from your dumped copy
       nxeart
       SHGame\
       AvatarAssetPack\
   ```

4. **Rename** `downpour.toml.sample` → `downpour.toml`. It already enables the chromatic-noise fix and sensible defaults.
5. **Double-click `start.bat`**, or open a terminal and run:

   ```powershell
   downpour.exe --game_data_root assets
   ```

6. The game launches in fullscreen 1080p. Press **F4** in-game to open the **settings overlay** (cvars, keybinds, mouse sensitivity, render scale). Press **`** (backtick) for the **console**.

That's it — you're playing *Silent Hill: Downpour* natively on PC.

---

## Default controls

| Action | Key / Button |
| --- | --- |
| Move | `W` `A` `S` `D` |
| Camera | Mouse |
| Interact / fire | `Space` / `LMB` |
| Aim / look | `RMB` |
| Run | `Shift` |
| Crouch | `C` |
| Reload | `R` |
| Menu | `Esc` |
| Pause / Map | `Tab` |
| Settings overlay | `F4` |
| Console | `` ` `` |

All bindings are remappable live via F4 → Input → Keybinds.

---

## Runtime configuration

The recompiled binary reads `downpour.toml` from its working directory. The sample shipped with the release already has these defaults set:

```toml
# Chromatic-noise fix on the fast RTV path. Safe to leave enabled for Downpour.
# Details: docs/chromatic-noise-fix.md
skip_depth_color_7e3_aliasing_transfers = true

# Render path: "rtv" = fast HostRenderTargets, "rov" = slower correctness fallback
render_target_path_d3d12 = "rtv"

# Native 1080p
window_width = 1920
window_height = 1080
fullscreen = true
video_mode_width = 1920
video_mode_height = 1080

# Soft present-rate cap
d3d12_present_frame_limiter = true

# Keyboard + mouse mode (hot-reloadable via F4)
mnk_mode = true
# mnk_sensitivity = 1.0   # raise for faster mouse, lower for slower

# Path to your game files (relative to this folder)
game_data_root = "./assets"

[log.levels]
gpu = "info"
```

> [!TIP]
> For internal supersampling (sharper at the cost of GPU time), open F4 → GPU and raise `draw_resolution_scale_x` / `draw_resolution_scale_y` to 2. That renders internally at 2160p and downscales to your output — similar to NVIDIA DSR or AMD VSR. Heavy on mid-range cards; leave at 1 for default 1080p.

---

## Frequently asked questions

<details>
<summary><b>Is this an emulator?</b></summary>

No. An emulator runs the original Xbox 360 instructions on a virtual CPU at runtime. DownpourRecomp converts the Xbox 360 instructions into native x86-64 code at build time, so what you run on your PC is a real Windows executable. Think of it as the game being "re-translated" once, not interpreted on every frame.

</details>

<details>
<summary><b>Why isn't the game executable included? Where do I get it?</b></summary>

Including the game's binary or any of its data files would be copyright infringement. You need to obtain the XEX and game data from your own legally-owned copy of the Xbox 360 release. We will not tell you where to download a copy, and asking will get your issue closed.

</details>

<details>
<summary><b>Can I unlock the frame rate above 30 FPS?</b></summary>

**v1.0: yes, 60 FPS is the new default.** The Xenia community patch is a guest-memory byte patch — it works under Xenia because Xenia reads guest code every frame, but in a static recomp the relevant PowerPC instructions are already translated to native C++ at build time, so writing bytes to the guest address does nothing. v1.0 instead applies the same patch directly to the recompiled C++ source, plus sets `video_mode_refresh_rate = 120` and `vsync = true` so the game logic runs at the full guest tick and the camera matches the new frame rate. Tested through the prologue and chapter 1 without the "camera slows down over 60 FPS" artifact that affects the raw byte patch under Xenia.

**v1.0: yes.** 60 FPS is the default. The Xenia community patch is a guest-memory byte patch — under emulation Xenia reads guest code every frame so the patch sticks. In a static recomp the relevant PowerPC instructions are already translated to native C++ at build time, so writing bytes to the guest address does nothing. v1.0 applies the same patch directly to the recompiled C++ source and sets `video_mode_refresh_rate = 120` + `vsync = true` so the game logic runs at the full guest tick and the camera matches the new frame rate.

Beyond 60 FPS is not currently planned — the UE3 tick code Vatra wrote was tuned for 30/60, and uncapped framerates introduce animation, physics, and camera issues that need per-system fixes.

</details>

<details>
<summary><b>Does mouse and keyboard work?</b></summary>

Yes — *Silent Hill: Downpour* never had an official PC port, so this is the first time the game can be played with a mouse and keyboard. The ReXGlue SDK ships a mouse-to-right-stick mapping with smoothing (rewritten in v0.1.1 for clean continuous mouse motion). All controller buttons are remappable to keyboard keys via F4 → Input.

</details>

<details>
<summary><b>What about ultrawide, HDR, ray tracing?</b></summary>

The game is a 2012 UE3 title — no native HDR or RT support. Ultrawide is **not** automatically letterboxed; you'll get a stretched 16:9 image at 21:9 unless you cap your window/output to 16:9. Aspect-correct ultrawide support would require a UE3 FOV patch (similar to Skate 3 Recomp's approach) and is not implemented in v0.1.1.

</details>

<details>
<summary><b>Do achievements unlock?</b></summary>

No. There is no Xbox Live backend, so any code path that submits an achievement is stubbed out. Saves work; achievements don't.

</details>

<details>
<summary><b>Will the save files transfer to/from a real Xbox 360?</b></summary>

The save layout matches the Xbox 360 format, so in principle yes — but no one's verified a round-trip yet. If you do, please open an issue with the result.

</details>

<details>
<summary><b>Why does GitHub call this a "native PC port" and not an "emulator"?</b></summary>

Because the file you run, `downpour.exe`, is a normal Windows executable produced by Clang from C++ source code. There is no virtual CPU. The CPU instructions that used to live in the Xbox 360 binary have been *translated* to C++ at build time. That's what "static recompilation" means.

</details>

<details>
<summary><b>I'm getting a crash / artefact / weird behaviour. What do I do?</b></summary>

Open an issue on [GitHub Issues](https://github.com/LittleBitUA/DownpourRecomp/issues) with:

- The exact symptom and the scene where it happens.
- A screenshot or short video if visual.
- The `logs/` folder next to `downpour.exe` (text logs, not too large).
- Your `downpour.toml` so we know your configuration.
- GPU model and driver version.

Do **not** attach any game files or binaries that link against game data.

</details>

---

## Building from source

<details>
<summary><b>Click to expand — full build instructions</b></summary>

### Requirements

- **Windows 10/11 x86-64** with up-to-date GPU drivers (D3D12 + DXIL/DXBC).
- A discrete GPU. RTX 30-series or equivalent (≥ 6 GB VRAM) for 1080p; RTX 40-/50-series for headroom on 1440p / 2160p downscale.
- **Visual Studio 2022** (17.8+) with the "Desktop development with C++" workload, Windows 10 SDK, and CMake/Ninja components.
- **CMake** ≥ 3.25 and **Ninja** ≥ 1.11.
- **LLVM/Clang-cl** is used by ReXGlue for the recompiled translation units; install LLVM and put `clang.exe` / `clang-cl.exe` on `PATH`.
- The ReXGlue SDK installed somewhere on disk.
- A legal copy of Silent Hill: Downpour (`title id 4B4E0823`, hash `7A3D5809776EE6AB`).

### 1. Build and install the ReXGlue SDK

DownpourRecomp consumes the SDK as an installed CMake package. One-time, ~10 minutes:

```bash
git clone https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
cmake --preset win-amd64-relwithdebinfo
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
```

The install prefix defaults to `out/install/win-amd64/`.

### 2. Provide the game executable

```
DownpourRecomp/
  assets/
    default.xex          ← your XEX, not in this repo
```

`.gitignore` blocks accidental commits of `*.xex`, `*.iso`, `*.god`, `*.dlc`, etc.

### 3. Run codegen

```bash
rexglue codegen --manifest downpour_manifest.toml
```

Translates the PPC code into C++ under `generated/default/` (~280 MB, excluded from git).

### 4. Configure and build

```bash
cmake --preset win-amd64-relwithdebinfo \
      -DREXGLUE_DIR="<path-to-rexglue-install>/lib/cmake/rexglue"
cmake --build out/build/win-amd64 --config RelWithDebInfo --target downpour
```

Produces `downpour.exe`. Copy `rexruntimerd.dll` from the SDK install next to it.

### 5. Provide the runtime game data

The runtime needs the full game file tree (the XEX alone is not enough). Point at it via `game_data_root` in `downpour.toml` or `--game_data_root` CLI.

</details>

---

## Technical deep-dives

- 📜 [**Chromatic-noise fix**](docs/chromatic-noise-fix.md) — full RenderDoc Pixel History backward trace from the noisy swapchain pixel to the divergent depth → 7e3 ownership transfer (EID 9506), the narrow one-direction cvar that resolves it, and a "what NOT to retry" dead-ends list.

---

## Project structure

This repository contains:

- **Recompiled game** codegen output: function tables, vtable address fix-ups, indirect-call targets discovered at runtime (`downpour_config.toml`).
- A thin `DownpourApp` shell that overrides ReXApp hooks for Downpour-specific behaviour.
- `xenia_patches.toml` — optional game patches (Unlock FPS, force 16x AF, disable FXAA, show FPS counter — reference-only in this port; see [FAQ](#frequently-asked-questions)).

---

## Acknowledgements

- The [Xenia](https://github.com/xenia-canary/xenia-canary) team — the entire D3D12 GPU backend that ReXGlue derives from is their work.
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) maintainers for the static recomp tooling.
- The static-recomp recipe pioneered by [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) and followed by Sonic Mania: Recompiled, Skate 3 Recomp, and [Deadly Premonition Recomp (DPRecomp)](https://github.com/LittleBitUA/DPRecomp).

---

## Legal

This repository contains **no Konami or Vatra Games assets, no game code, no game data, no game audio.** It is original code (build configuration, codegen metadata, application shell) that targets a separately-supplied legally-owned copy of *Silent Hill: Downpour* for personal use under the user's own jurisdiction's fair-use / private-copy provisions.

Do not distribute the game executable, game data, or any binary that links against game data. Pull requests that include game content will be rejected.

Code under this repository is released under the **BSD 3-Clause license** — see [LICENSE](LICENSE).

The license applies only to the host-side source under `src/`, build scripts, CMake files, TOML configs, and documentation. The recompiled game code produced at build time (`generated/default/`) is derived from the copyrighted *Silent Hill: Downpour* binary and is **not** covered by this license.

---

<div align="center">

**Related projects by the same author:**
[DPRecomp — Deadly Premonition (PC port)](https://github.com/LittleBitUA/DPRecomp)

</div>
