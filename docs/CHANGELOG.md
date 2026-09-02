# DownpourRecomp — release history

The current release's notes live in the [README](../README.md#whats-new-in-v117) and on the
[GitHub releases page](https://github.com/LittleBitUA/DownpourRecomp/releases).
This file archives the full notes of every previous release.

---

## v1.1.6

### 🛟 Pre-update save backup — defence against unverified data loss

One v1.1.4 → v1.1.5 user reported that after auto-updating they "lost all of their Downpour files and progress". The auto-update PowerShell script provably never deletes any of `user/`, `assets/`, `downpour.toml`, or `launcher.ini` — it only copies six specific files (`PlayDownpour.exe`, `downpour.exe`, `rexruntimerd.dll`, `TracyClientrd.dll`, `gamecontrollerdb.txt`, `README.txt`) over the existing install plus refreshing the shareable shader cache. We could not reproduce the loss and the user couldn't recover the `dpr_update.log` (the script removes it on success, leaving it only on failure).

Whatever happened in that one report, the right answer is a safety net. v1.1.6's update script now runs an explicit pre-update step that archives `user/` + `downpour.toml` + `launcher.ini` + `downpour.toml.backup` into `%TEMP%\dpr_user_backup_v<your-current-version>.zip` BEFORE touching anything. The zip persists across runs and is overwritten only by another update from the same version. If anything ever goes wrong, that zip is your one-click restore: unzip it back into your install directory.

### 🐭 New Mouse setting: Use Raw Input (default ON)

The v1.1.2+ raw-input path bypasses Windows pointer ballistics ("Enhance pointer precision") in favour of direct HID counts at ~1 kHz polling. For most users this feels much better than the original v1.1.1 `WM_MOUSEMOVE` path. But on extremely high-DPI gaming mice (16,000+ DPI) at least one user reported that no amount of `Raw Input Scale` + `Stick Scale` tuning recovered the feel they had on v1.1.1. If you're in that camp, uncheck the new **"Use Raw Input"** checkbox at the top of Settings → Mouse to get back the Windows-ballistics-aware behaviour that high-DPI mice were calibrated against by their manufacturer firmware.

### 🎛️ Launcher sliders

Until v1.1.5 every numeric value in launcher Settings was a plain text edit box. v1.1.6 pairs each ranged numeric field with a draggable Win32 trackbar (`msctls_trackbar32`) just like every native game's option screen. Applies to all `kFloat` and ranged `kInt` cvars: Mouse Sensitivity, Mouse Smoothing, Mouse Acceleration Curve, Stick Decay, Raw Input Scale, Stick Scale, Deadzone Compensation, DualSense trigger parameters, resolution scale, FPS cap, frame budget, and the engine-side UE3 shadow/anisotropy/LOD-bias sliders.

### 🎵 Launcher music controls

Settings → Advanced gained **Launcher Music Volume** (0-100%, default 25%) and **Mute Launcher Music**. Persisted in `launcher.ini`.

### Other fixes

* Version-comparison bug — the "Update available" banner used raw string equality; it now parses `(major, minor, patch)` tuples and fires only when the GitHub tag is actually newer.
* Window title's `kLauncherVersion` baseline + VERSIONINFO PE resource bumped to `1.1.6.0`.

---

## v1.1.5

The Win11 taskbar / Alt-Tab caption was reading `Silent Hill: Downpour v1.0 | «Little Bit»` through every release since v1.0 ship — the title string was hardcoded once and never updated even though we bumped `kLauncherVersion` five times. Same for the in-canvas top-right corner version label, and the PE `VERSIONINFO` resource.

v1.1.5 makes all three driven from a single source: at runtime, `wWinMain` composes the full window caption as `kWindowTitle + L" " + kLauncherVersion + L" | «Little Bit»"`. Every future release now only needs `kLauncherVersion` bumped (one line) + four numbers in `resources.rc`.

---

## v1.1.4

### 🐭 Root-cause fix for "mouse very slow" — stick saturation

v1.1.3 only fixed the surface symptom. The actual root cause was a hardcoded `kBaseScale = 1500` multiplier inside `mnk_input_driver.cpp`: with `mnk_raw_input_scale = 0.5`, even 1 mm of physical mouse motion at 1600 DPI produced a stick target of ~280,000 — saturated to 32,767 regardless. So community reports of "sensitivity 6.0 = no change" were accurate: sensitivity has no effect past saturation.

`kBaseScale` became the cvar **`mnk_stick_scale`** (default `150`), combined with a lower `mnk_raw_input_scale` default (`0.20`). Sensitivity finally controlled slope from "twitchy" to "deliberate" for the same physical mouse range.

### WM_MOUSEMOVE fallback (Wine, VMs)

Setups where `RegisterRawInputDevices` doesn't deliver `WM_INPUT` (some Wine prefixes, VMs, remote desktop) use the pixel-delta fallback path; the fix there was bumping `Stick Scale` to 500-1500 in launcher Settings.

---

## v1.1.3

### 🐭 Mouse-calibration hotfix

v1.1.2 hardcoded a divide-by-8 scale on raw HID mouse deltas, which missed that Windows applies pointer ballistics to `WM_MOUSEMOVE` deltas by default — raw input has no ballistics, so the divide felt much slower in motion. v1.1.3 replaced the hardcoded divider with the tunable cvar `mnk_raw_input_scale` (default `0.5`), surfaced in launcher **Settings → Mouse**.

---

## v1.1.2

### 🖱️ Raw-mouse input

The SDK's Win32Window started calling `RegisterRawInputDevices` and handling `WM_INPUT` raw-mouse messages. Gaming mice emit at ~1000 Hz with sub-pixel HID counts; the previous `WM_MOUSEMOVE` path delivered integer-pixel deltas at the monitor refresh rate, which made slow mouse motion feel like the right stick was being dragged through molasses. Defaults adjusted: `mnk_smoothing` 0.15 → 0.10, `mnk_decay` 0.30 → 0.10.

### 🖥️ VSync no longer silently overridden

`d3d12_allow_variable_refresh_rate_and_tearing` default flipped from `true` to `false` after a community report — `vsync = true` was silently ignored because the tearing swap-chain path took precedence. G-Sync / FreeSync users can flip it back in launcher Settings → Advanced.

### 📉 Auto-tune of resolution scale for low-end GPUs

The launcher reads the primary GPU's `DedicatedVideoMemory` via DXGI. GPUs under 8 GiB (RTX 3050, GTX 1650, Steam Deck) and Intel integrated seed `resolution_scale = "1"` on fresh installs instead of `"2"`.

---

## v1.1.1

### ⚙️ Settings no longer reset after F4

The launcher's `EnsurePerfDefaultsInToml` now reseeds any registered cvar that disappeared from `downpour.toml` (the SDK's F4 SaveConfig drops anything matching compiled defaults). This fixed the recurring "my language flipped back to Ukrainian", "my keybinds reverted", and "DualSense triggers stopped working" reports.

### 🩺 Auto-updater no longer fails silently

The PowerShell update helper writes a step-by-step log to `%TEMP%\dpr_update.log`, wraps everything in `try/catch`, and on failure relaunches whatever `PlayDownpour.exe` is in place so you're never stranded. On next boot the launcher offers to open the log.

### 🚀 SDK: batched end-of-frame memexport drain

Downpour issues ~37 memexport readback fallbacks per frame in skinning-heavy scenes, each with a ~1 ms fence-wait stall. v1.1.1 defers them into a single end-of-frame drain. Expected AMD/RTV uplift in heavy scenes: ~27 FPS → ~45-50 FPS.

---

## v1.1

### 🆕 Auto-updates

GitHub-driven in-place update: on boot the launcher checks the releases endpoint; a pill banner appears when a newer tag exists. Clicking it downloads the zip, a hidden PowerShell helper copies the new binaries over the install (preserving `assets/`, `user/`, `logs/`, `downpour.toml`, `launcher.ini`), and relaunches.

### 🛠️ Render-path fixes from community feedback

AMD GPU vendor detection: RDNA 2 and earlier produced artifacts on the ROV path; the launcher now reads the DXGI vendor ID on first run and pins `render_target_path_d3d12 = "rtv"` automatically on AMD.

### 🐛 Bug fixes

* `mnk_capture_mouse` now defaults to `true` — no more visible system cursor during gameplay.
* Launcher language parsing accepts both quote styles (fixes English-selected-but-Ukrainian-displayed).
* First-run button reads `UNPACK GAME` / `РОЗПАКУВАТИ ГРУ` until assets exist.

---

## v1.0

### 🎮 Performance & framerate

* **60 FPS unlocked** — recompile-time patch of the UE3 frame-skip flag.
* **ROV render path warm-cache** + ~1,370 pre-compiled shaders shipped.
* **Memexport readback de-flooding** — triple-buffered ring, per-frame fence waits dropped from ~18 to 0.
* Tuned texture cache (3/6 GB soft/hard, 30-min residence).

### 🖱️ Launcher & first-run experience

Standalone `PlayDownpour.exe` launcher, in-game F4 overlay, TU installer fixes, portable layout (saves + caches live in the game folder), Linux/Wine font fallbacks.

### 🎮 Input

Mouse-and-keyboard mode with smoothing/decay/deadzone; full DualSense support over USB and Bluetooth with Level-1 adaptive triggers; Xbox controllers via SDL3; fully rebindable.

### 🎨 Visuals

Native ASC-CDL colour-grade post-FX with 7 presets; tuned 1080p defaults (2× SSAA, 16× AF, FSR3 present); chromatic-noise bug on the fast render path fixed.

### 🛡️ Stability

PSO-stall log noise cut ~10×; VFS negative-result cache erase strategy fixed; no crashes/TDRs on the v1.0 path in extended sessions on the test bench.
