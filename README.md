# DownpourRecomp

A static recompilation of **Silent Hill: Downpour** (Xbox 360, 2012) for native Windows, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

![Murphy close-up](docs/screenshots/murphy-closeup.png)

> Recompilation, not emulation. The Xbox 360 PowerPC binary is statically translated into native x86-64 code at build time. There is no per-frame instruction dispatch overhead; the game runs as a regular Windows process backed by an Xbox 360 host runtime (CPU registers, kernel imports, GPU command processor).

---

## Status

End-to-end playable as of 2026-06-19. Boot, menus, prologue, main story scenes, audio, input, save/load — all functional. The long-standing chromatic-noise bug on the fast RTV render-target-cache path has been **fixed** (see [The fix](#the-fix) below).

| Subsystem | State |
|---|---|
| CPU recompilation | Stable |
| Kernel imports | Workarounds in place; no known blockers |
| Audio (XAudio2) | Working |
| Input (keyboard + mouse, gamepad) | Working |
| GPU — D3D12 RTV/DSV path | **Working** (fast, default) |
| GPU — D3D12 ROV path | Working (slower, correctness fallback) |
| Save / load | Working |
| Achievements / online features | Stubbed (single-player only) |

![Prison Block 4 intro](docs/screenshots/block-4-intro.png)
![Murphy in the prison corridor](docs/screenshots/prison-corridor.png)

---

## About this project

This is one of a small family of ReXGlue-based static-recompilation ports. ReXGlue is a Xenia-derived host runtime that provides the Xbox 360 ABI: PPC register file, MMIO, kernel object table, threading, and the GPU command processor + EDRAM-emulating render target / texture cache. DownpourRecomp links against the ReXGlue runtime and provides:

- The Silent Hill: Downpour codegen output (function tables, vtable address fix-ups, indirect-call targets discovered at runtime — checked into `downpour_config.toml`).
- A thin `DownpourApp` shell that overrides ReXApp hooks for any Downpour-specific behaviour.
- A `xenia_patches.toml` file with optional game patches (Unlock FPS, force 16x AF, disable FXAA, show FPS counter — disabled by default).

The repository **does not contain**:

- The game executable (`default.xex`).
- Any portion of the game data (movies, levels, audio).
- ROM images, ISOs, GoD packages, or DLC content.

You bring your own legally-owned copy of the game.

---

## Requirements

- **Windows 10/11 x86-64** with up-to-date GPU drivers (D3D12 + DXIL/DXBC).
- A discrete GPU is strongly recommended. RTX 30-series or equivalent (≥ 6 GB VRAM) for stable 1080p; RTX 40-/50-series for headroom on 1440p / 2160p downscale or higher draw scale.
- **Visual Studio 2022** (17.8+) with the "Desktop development with C++" workload, Windows 10 SDK, and CMake/Ninja components. Or any equivalent toolchain that can build modern C++23.
- **CMake** ≥ 3.25 and **Ninja** ≥ 1.11.
- **LLVM/Clang-cl** is used by ReXGlue for the recompiled translation units; install LLVM and put `clang.exe` / `clang-cl.exe` on `PATH`.
- The ReXGlue SDK installed somewhere on disk (see below).
- A legal copy of Silent Hill: Downpour — region USA/EUR (`title id 4B4E0823`). The hash that this project's codegen was generated against is `7A3D5809776EE6AB`.

---

## Building from source

### 1. Set up the ReXGlue SDK

DownpourRecomp consumes the ReXGlue SDK as an installed CMake package. Build and install the SDK first (one-time, takes ~10 minutes):

```bash
git clone https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
cmake --preset win-amd64-relwithdebinfo
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
```

Note the install prefix (defaults to `out/install/win-amd64/` inside the SDK tree).

### 2. Provide the game executable

Place your own legally-extracted XEX next to the codegen output:

```
DownpourRecomp/
  assets/
    default.xex      <-- your XEX, NOT included in this repository
```

`.gitignore` blocks accidental commits of any `*.xex`, `*.iso`, `*.god`, `*.dlc`, etc. Do not bypass it.

### 3. Run codegen

```bash
rexglue codegen --manifest downpour_manifest.toml
```

This translates the PPC code in `assets/default.xex` into C++ source files under `generated/default/`. The output is large (~280 MB) and is excluded from git.

### 4. Configure and build

```bash
cmake --preset win-amd64-relwithdebinfo \
      -DREXGLUE_DIR="<path-to-rexglue-install>/lib/cmake/rexglue"
cmake --build out/build/win-amd64 --config RelWithDebInfo --target downpour
```

The build outputs `downpour.exe` and depends on `rexruntimerd.dll` (copy the DLL from the ReXGlue install next to the executable).

### 5. Provide game data

The runtime needs the full game file tree (XEX is not enough — the game streams content, audio, and scripts from disk):

```
<some-folder>/
  Game/
    SHGame/...         <-- game content
    AvatarAssetPack/   <-- Konami avatar files
    default.xex        <-- the same XEX as in assets/
    nxeart             <-- title metadata
```

Point the runtime at this folder using `game_data_root` in `downpour.toml` (see next section) or via the `--game_data_root` command-line argument.

---

## Runtime configuration

The recompiled binary reads `downpour.toml` from its working directory. A minimal config:

```toml
# RTV chromatic noise fix — see "The fix" section below for forensic details.
skip_depth_color_7e3_aliasing_transfers = true

# D3D12 path: "rtv" for the fast HostRenderTargets path; "rov" for the slow but
# always-correct PixelShaderInterlock path (fallback if you ever see noise on a
# new scene the fix doesn't cover — please open an issue).
render_target_path_d3d12 = "rtv"

# Window size.
window_width = 1920
window_height = 1080
fullscreen = true
video_mode_width = 1920
video_mode_height = 1080

# Internal render resolution scaling. 1 = native 720p, 2 = 1440p downscaled to
# the window (similar to DSR / VSR). Open the in-game cvar overlay (F4) to
# tweak live.
draw_resolution_scale_x = 1
draw_resolution_scale_y = 1

# Frame pacing.
d3d12_present_frame_limiter = true

# Mouse / keyboard mode.
mnk_mode = true
mnk_sensitivity = 0.300000

# Path to the game file tree.
game_data_root = "C:/Path/To/Your/SilentHillDownpour"

[log.levels]
gpu = "info"
```

All cvars are also editable live via the **F4 in-game settings overlay** (provided by the ReXGlue SDK).

---

## The fix

Silent Hill: Downpour exhibits a UE3 G-buffer EDRAM-aliasing pattern that triggers a depth → 7e3 (`k_2_10_10_10_FLOAT`) ownership transfer in the rexglue HostRenderTargets path. The transfer pixel shader treats the depth bits as packed 7e3 floats and decodes them via `Float7e3To32`, producing values like `(8.1875, 0.25, 0.8125, 0.333)` from clean `(0.013, 0.012, 0.008, 0.333)` HDR scene content. Those exploded HDR values then propagate through the resolve → shared-memory → texture_load → gamma → composite chain, visible at the swapchain as high-frequency red/green chromatic speckle on Murphy and other surfaces.

Localized via RenderDoc Pixel History on the noisy pixel in the lift scene: the corruption point is **EID 9506** in a representative capture — a graphics-pipeline ownership transfer pixel shader (Pipeline State 1949) reading `xe_transfer_depth` + `xe_transfer_stencil` SRVs from a `kD24FS8 4xMSAA` source RT and writing to the `k_2_10_10_10_FLOAT 1xMSAA` HDR scene RT. Clean Xenia Canary avoids this entirely by keeping persistent host RTs and switching SRV views rather than running a format-converting transfer.

The fix is a narrow cvar in the ReXGlue SDK common render target cache — `skip_depth_color_7e3_aliasing_transfers` — that drops the queued ownership transfer **only in the depth → 7e3 direction**. The reverse direction (7e3 → depth) is preserved because Downpour menu font rendering uses it legitimately; skipping that direction breaks the brightness slider screen (blue bands + ghost text).

The cvar is opt-in (default `false` in the SDK). DownpourRecomp's runtime config enables it.

---

## Known limitations

- **Online features** (multiplayer hooks, leaderboards) are stubbed.
- **Achievements** do not unlock anywhere (no XBL backend).
- **Avatars** in the Xbox 360 dashboard sense are not rendered.
- Anything that depends on **MTH (Microsoft Touch / Hardware) extensions** beyond what is necessary for boot.
- Patching toggles in `xenia_patches.toml` are not yet wired through codegen; flipping `is_enabled = true` does not currently apply the patch. See the file's header comment for the planned workflow.

---

## Acknowledgements

- The [Xenia](https://github.com/xenia-canary/xenia-canary) team — the entire D3D12 GPU backend that ReXGlue derives from is their work.
- The ReXGlue SDK maintainers for the static recomp tooling.
- The static-recomp recipe pioneered by N64Recomp / N64: Recompiled and followed by Sonic Mania: Recompiled, Skate 3 Recomp, Deadly Premonition Recomp.

---

## Legal

This repository contains **no Konami or Vatra Games assets**. It is original code (build configuration, codegen metadata, application shell) that targets an existing legally-owned copy of Silent Hill: Downpour for personal use under the user's own jurisdiction's fair-use / private-copy provisions.

Do not distribute the game executable, game data, or any binary built against game data. Pull requests that include game content will be rejected.

Code under this repository is released under the BSD 3-Clause license — see [LICENSE](LICENSE).
