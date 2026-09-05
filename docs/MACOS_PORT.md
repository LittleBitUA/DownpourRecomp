# macOS Apple Silicon port

Native ARM64 build of the Downpour host shell running the Vulkan backend over
MoltenVK. This document records what was wrong, what was fixed, and how to
build it. It ships **no game code and no game data** — you supply your own
legally-owned dump, exactly as on Windows.

## Status

The title boots, renders and presents on Apple Silicon with the same present
chain the Windows build uses, including the ASC-CDL colour grade. Audio, save/
load, gameplay over long sessions and controller support are **not** yet
validated — see "Known gaps" below.

## What was actually broken

The port compiled and launched, but the window stayed black and the process
died within seconds of the first frame. Every crash report showed the same
stack:

```
MVKDescriptorSetLayoutBinding::populateShaderConversionConfig   <- libMoltenVK
MVKPipelineLayout::populateShaderConversionConfig
MVKGraphicsPipeline::initShaderConversionConfig
vkCreateGraphicsPipelines
rex::ui::vulkan::VulkanPresenter::CreateGuestOutputPaintPipeline
rex::ui::vulkan::VulkanPresenter::PaintAndPresentImpl
rex::ui::SDLWindowedAppContext::RunMainLoop                     <- main thread
```

This was **not** a MoltenVK or Metal instability. It was an out-of-bounds read
in the Vulkan presenter.

Downpour added three guest-output paint effects — `kColourGrade`, `kBox` and
`kBoxDither` — and implemented them only in the D3D12 presenter. The Vulkan
presenter was never updated, so:

1. `GetGuestOutputPaintPipelineLayoutIndex()` had no case for them and its
   `default:` returned `kGuestOutputPaintPipelineLayoutCount`. With
   `REXGLUE_ENABLE_FIDELITYFX=OFF` (the macOS configuration) that value is `1`,
   and `guest_output_paint_pipeline_layouts_` holds exactly one element. Index
   `[1]` read the next member in the struct — `guest_output_paint_vs_`, a
   `VkShaderModule` — and passed it to `vkCreateGraphicsPipelines` as the
   pipeline layout. MoltenVK walked an `MVKShaderModule` as an
   `MVKPipelineLayout` and dereferenced its code buffer as a pointer. The
   faulting address in every report (`0xd65f03c0528001e0`) decodes to two ARM64
   instructions, which is what that looks like from the outside.
2. No SPIR-V fragment module existed for those effects, so
   `guest_output_paint_fs_[kColourGrade]` was `VK_NULL_HANDLE`.
3. The `assert_true` guards that would have caught both are compiled out in
   RelWithDebInfo.

`colour_grade_enable` defaults to `true`, and the paint-flow builder swaps the
final pass to `kColourGrade` on every frame, so this fired on the very first
present, every launch. Windows never saw it because Windows uses D3D12.

## What changed

In `rexglue-sdk-dpour`:

- **Presenter layout indices** (`include/rex/ui/vulkan/presenter.h`) — added
  layout indices for the colour-grade and box passes and made the `default:`
  case return a valid index. No path may return a value equal to the array
  size.
- **SPIR-V for the three effects** — `scripts/build_vulkan_shaders.sh` compiles
  `guest_output_colour_grade_ps.hlsl` and `guest_output_box_ps.hlsl` with
  `glslc -x hlsl -DXE_VULKAN=1` into `src/ui/shaders/vulkan_spirv/`. The HLSL is
  shared with D3D12; only the resource declarations differ, behind `XE_VULKAN`.
  Vulkan puts the vertex shader's rectangle constants in push-constant bytes
  0–15, so the fragment block is shifted to byte 16 with HLSL `packoffset`
  registers (glslang ignores `[[vk::offset]]`).
- **Fallback instead of a crash** (`src/ui/vulkan/vulkan_presenter.cpp`) — if a
  selected effect has no fragment module, the presenter logs once and falls
  back to bilinear rather than handing a null module to the driver.
- **Shader-compile progress** (`src/graphics/vulkan/pipeline_cache.cpp`,
  `src/ui/rex_app.cpp`) — the Vulkan pipeline cache now publishes the same
  progress counters the D3D12 one does, and the "Preparing shaders" dialog is
  enabled on macOS. MoltenVK compiles every pipeline through the Metal shader
  compiler at startup, so a cold cache blocks for minutes; without the dialog
  the window is simply frozen. Completion is published from a scope guard
  because the function has several early returns and the launch waits on it.
- **Guest memory backing** (`src/core/memory_posix.cpp`) — Darwin caps POSIX
  shared-memory objects well below the 4.5 GiB guest arena, so the mapping uses
  an unlinked temporary file instead. Same `MAP_SHARED` aliasing, stays sparse,
  cannot leave a file behind on a crash.
- **Input teardown race** (`src/kernel/xam/xam_input.cpp`,
  `src/input/input_system.{h,cpp}`) — guest threads keep calling
  `XamInputGetState` while the host tears the runtime down. The XAM entry points
  dereferenced the input system without a null check, and `InputSystem`'s driver
  list had no lock at all while `Shutdown()` cleared it. Both are fixed.

In `DownpourRecomp`: native macOS file dialogs for ISO/TU selection
(`src/downpour_file_picker.mm`), and the CMake plumbing for the
`macos-arm64-*` presets.

## Prerequisites

- Apple Silicon Mac, current Command Line Tools.
- `brew install llvm molten-vk vulkan-loader shaderc spirv-tools`
- CMake ≥ 3.25 and Ninja.
- A legally owned USA or Europe Xbox 360 dump matching the repository's
  supported game and TU1 hashes. **Do not place game data under version
  control.**

## Build

1. Put your merged `default.tu1.xex` at `assets/default.tu1.xex` with the base
   `default.xex` and `default.xexp` alongside it, as the manifest requires.
2. Generate the derived sources and build:

   ```sh
   ./scripts/build_macos.sh
   ```

   Or by hand:

   ```sh
   /path/to/rexglue codegen downpour_manifest.toml
   cmake --preset macos-arm64-relwithdebinfo -DREXSDK_DIR=/path/to/rexglue-sdk-dpour
   cmake --build --preset macos-arm64-relwithdebinfo --target downpour --parallel 4
   ```

To regenerate the Vulkan shader headers after editing the shared HLSL, run
`scripts/build_vulkan_shaders.sh` in the SDK tree.

## Configuration notes

Measured on an M2 MacBook Air (2560x1664 panel, 1710x1112 logical):

| Setting | Effect |
| --- | --- |
| `sdl_high_pixel_density = false` | Swapchain at the logical size (1710x1074, roughly 1080p). The workable default. |
| `sdl_high_pixel_density = true` | Swapchain at native backing pixels (3420x2148). Four times the pixels; the GPU cannot keep up and the main thread spends its time blocked in `nextDrawable`. |
| `draw_resolution_scale_x/y` | Internal supersampling of the guest's 720p framebuffer. This is where sharpness actually comes from, and it is expensive. |
| `log_level = "debug"` | Writes ~5 MB/minute. Use `"info"` for playing. |

Portuguese: set `user_language = 9`. The dump carries the `-prt` localization
set, so menus, subtitles and journal text switch over.

## Known gaps

- Frame rate is roughly 20 fps at 1710x1074 with `draw_resolution_scale = 1` in
  a RelWithDebInfo build with debug logging. A Release build has not been
  measured.
- Audio, save/load, controller input and clean shutdown are not validated.
- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` is still set in the launcher. It was
  added while the presenter crash was misattributed to MoltenVK argument
  buffers; whether it is still needed has not been re-tested.
- Long-session stability is unmeasured. The fixes above were verified over runs
  of a few minutes, not a playthrough.
