// downpour - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "downpour_title_update_installer.h"

// === DPOUR MIGRATION 2026-07-24: native-render Phase 1b overlay registration ===
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#ifndef DPOUR_NO_NATIVE_RENDER
#include "downpour_native_overlay.h"
#endif

// dpour-fork 2026-07-02: knob to compensate UE3 DoF blur kernel sizes for
// upscaled render resolutions. Vertical-banding artifact reported in
// Downpour when resolution_scale > 1 is caused by BlurKernelSize +
// BlurBloomKernelSize + MaxNearBlurAmount being sized for guest-native 720p
// but the UberPostProcess Uber shader sampling a downsampled intermediate at
// the same UV-offset step regardless of host resolution. Multiplying the
// stored float default by the resolution scale factor tells the shader to
// widen the kernel proportionally.
//
// TU1 addresses (verified against DPRecomp codegen sub_832ED230 /
// sub_832ED250 / sub_832ED270 at downpour_recomp.138.cpp:3447 — the
// storage-base register r10 = -2089025536 = 0x83800000, NOT 0x837C0000
// as a stale memory note claimed):
//   0x821569c8  L"DOF_BlurKernelSize"        storage 0x838061D4
//   0x821569f0  L"DOF_BlurBloomKernelSize"   storage 0x838061DC
//   0x82156a20  L"DOF_MaxNearBlurAmount"     storage 0x838061E4
// Storage stride = 8 bytes per property. Layout inside cell is not yet
// nailed down; the patch tries offsets 0 and 4 within each cell and logs
// raw bytes so a follow-up session can lock in the exact offset.
REXCVAR_DECLARE(std::int32_t, draw_resolution_scale_x);

class DownpourApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DownpourApp>(new DownpourApp(ctx, "downpour",
        PPCImageConfig));
  }

  // Gate the runtime launch behind Title Update 1: if the user has not yet
  // staged default.xexp next to the game files, open the installer wizard and
  // pause path finalization until they finish. Mirrors the Skate 3 / EA
  // installer pattern (see mchughalex/skate3recomp/src/skate3_app_common.cpp).
  // Honors a DOWNPOUR_INSTALL_TU env override for headless installs.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig runtime_paths = defaults;
    if (downpour::IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
      return runtime_paths;
    }
    REXLOG_INFO(
        "Silent Hill: Downpour Title Update 1 not staged at {}; launching the "
        "title update installer.",
        runtime_paths.game_data_root.string());
    downpour::ShowTitleUpdateInstallWizard(
        imgui_drawer(), std::move(runtime_paths), std::move(resume));
    return std::nullopt;
  }

  // Portable layout: keep user data (saves + per-content profiles) and the
  // runtime PSO/texture cache right next to the executable rather than under
  // %Documents%/downpour. This makes the distribution a true drop-anywhere
  // archive — no leftovers in user profile, easy to back up / move / delete.
  // Cvar overrides (`user_data_root`, `cache_path`) in downpour.toml still
  // win; this only changes the *default* when neither cvar is set.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_folder = rex::filesystem::GetExecutableFolder();
    if (exe_folder.empty()) {
      return;  // Defensive: keep SDK defaults if we can't resolve.
    }
    const bool user_was_default =
        paths.user_data_root == (rex::filesystem::GetUserFolder() / "downpour");
    if (user_was_default) {
      paths.user_data_root = exe_folder / "user";
      // SDK derives default cache_root = user_data_root/cache when no cvar.
      // If the resolved cache still lives under the old %Documents% path,
      // redirect it too so they stay together.
      const auto old_user_cache =
          rex::filesystem::GetUserFolder() / "downpour" / "cache";
      if (paths.cache_root == old_user_cache || paths.cache_root.empty()) {
        paths.cache_root = exe_folder / "cache";
      }
    }
  }

  // Custom window title — overrides the default "downpour <build-stamp>" form
  // so end users see the marketing title in the OS task bar / alt-tab list,
  // not the internal SDK identifier. NVIDIA ShadowPlay / OBS / streaming tools
  // also use the window title to label captures, which is why we want it
  // clean and stable rather than version-stamped per build.
  std::string GetWindowTitle() const override {
    return "Silent Hill: Downpour v1.1.7 | \xc2\xabLittle Bit\xc2\xbb";
  }

  // dpour-fork 2026-07-02: post-XEX-load data patch — write scaled BlurKernel
  // defaults into guest memory so UE3's UberPostProcess samples with a
  // kernel radius proportional to the host resolution scale. This is the
  // Downpour-specific fix for the DoF checkerboarding artifact users see at
  // resolution_scale > 1. Runs before the guest main thread starts, so
  // subsequent Coalesced.ini load (which sets configurable properties) can
  // still override, but the baked default is our scaled version.
  //
  // Storage cells found at DAT_837c61d4 / DAT_837c61dc / DAT_837c61e4 (each
  // 8 bytes wide). Layout inside the cell is UNVERIFIED — this pass writes
  // the scaled float at BOTH offset 0 AND offset 4 so whichever holds the
  // actual value gets picked up. Raw bytes are logged before and after so
  // a follow-up can pin the exact offset and drop the belt-and-suspenders.
  void OnPostLoadXexImage() override {
    const std::int32_t scale = REXCVAR_GET(draw_resolution_scale_x);
    if (scale <= 1) {
      REXLOG_INFO("[dof-scale] draw_resolution_scale_x={} -- no patch needed",
                  scale);
      return;
    }
    auto* memory = runtime()->memory();
    if (!memory) {
      REXLOG_WARN("[dof-scale] runtime()->memory() is null; skipping patch");
      return;
    }
    // Storage cell guest addresses. Verified 2026-07-02 by hooking
    // sub_82363CB0 at runtime — r3 arrives as 0x837c61XX (matches the ORIGINAL
    // stale-memory note; my earlier "correction" to 0x838061XX was an
    // arithmetic mistake). NB: these cells hold FName-index metadata, NOT
    // float defaults — hook log shows sub_82363CB0 writes consecutive
    // FName indices (0x0A3B/A3C/A3D) at offset 0. Patching this cell does
    // NOT change the actual blur kernel value. Kept as scaffolding for a
    // future revision that hooks the actual CDO storage.
    constexpr std::uint32_t kStorageCells[] = {
        0x837c61d4u,  // DOF_BlurKernelSize (metadata slot)
        0x837c61dcu,  // DOF_BlurBloomKernelSize (metadata slot)
        0x837c61e4u,  // DOF_MaxNearBlurAmount (metadata slot)
    };
    constexpr const char* kNames[] = {"BlurKernelSize",
                                       "BlurBloomKernelSize",
                                       "MaxNearBlurAmount"};

    auto swap_be = [](std::uint32_t value) -> std::uint32_t {
      return ((value & 0xFFu) << 24) | ((value & 0xFF00u) << 8) |
             ((value & 0xFF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
    };

    for (std::size_t i = 0; i < 3; ++i) {
      auto* host_bytes = memory->TranslateVirtual<std::uint8_t*>(kStorageCells[i]);
      if (!host_bytes) {
        REXLOG_WARN("[dof-scale] TranslateVirtual({:08x}) returned null",
                    kStorageCells[i]);
        continue;
      }
      // Log all 8 bytes of the storage cell BEFORE mutation so we can see
      // what layout looks like.
      REXLOG_INFO("[dof-scale] {} @ {:08x} pre-patch bytes: "
                  "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                  kNames[i], kStorageCells[i], host_bytes[0], host_bytes[1],
                  host_bytes[2], host_bytes[3], host_bytes[4], host_bytes[5],
                  host_bytes[6], host_bytes[7]);

      // Try both possible float slots — offset 0 and offset 4 within the cell.
      for (std::size_t offset : {std::size_t{0}, std::size_t{4}}) {
        std::uint32_t raw_be = 0;
        std::memcpy(&raw_be, host_bytes + offset, 4);
        const std::uint32_t raw_host = swap_be(raw_be);
        float current = std::bit_cast<float>(raw_host);
        // Sanity: skip obvious non-float garbage. UE3 blur kernel defaults
        // are typically in [0.0f, 8.0f]. NaN and giant values mean this
        // offset is a flag/pointer, not the value.
        if (!std::isfinite(current) || current < 0.0f || current > 32.0f) {
          REXLOG_INFO("[dof-scale] {} @+{} = {} -- non-plausible, skip", kNames[i],
                      offset, current);
          continue;
        }
        const float scaled = current * static_cast<float>(scale);
        const std::uint32_t scaled_host = std::bit_cast<std::uint32_t>(scaled);
        const std::uint32_t scaled_be = swap_be(scaled_host);
        std::memcpy(host_bytes + offset, &scaled_be, 4);
        REXLOG_INFO("[dof-scale] {} @+{} : {} -> {} (scale={})",
                    kNames[i], offset, current, scaled, scale);
      }
    }
  }

  // === DPOUR MIGRATION 2026-07-24: native-render Phase 1b ===
  // Runs on the UI thread once the presenter + ImGui are set up. We piggyback
  // on it to register the native-D3D12 overlay drawer (no-op unless the env var
  // DPOUR_NR_OVERLAY is set). Revert: delete this override.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    (void)drawer;
    // Runtime/graphics_system are not wired yet at OnCreateDialogs time (the SDK
    // moves config.graphics into the runtime later, and calls window
    // SetPresenter right after this). Defer to the UI thread and retry each tick
    // until runtime()->graphics_system() is live, then register once.
    ScheduleNativeOverlayRegistration(0);
  }

  void ScheduleNativeOverlayRegistration(int attempt) {
    rex::ui::Window* w = window();
    if (w == nullptr) {
      return;
    }
    w->app_context().CallInUIThreadDeferred([this, attempt]() {
      rex::Runtime* rt = runtime();
      rex::system::IGraphicsSystem* gs = rt ? rt->graphics_system() : nullptr;
#ifdef DPOUR_NO_NATIVE_RENDER
      (void)gs;
      (void)attempt;
#else
      if (!dpour_nr::MaybeRegisterNativeOverlay(gs) && attempt < 1200) {
        ScheduleNativeOverlayRegistration(attempt + 1);  // not ready — next tick
      }
#endif
    });
  }

  // SDK v0.8.1.19+ handles Win32 MMCSS + high_resolution_timer automatically
  // (cvars win32_mmcss / win32_high_resolution_timer, both default true).
  // Override hooks here only if you need Downpour-specific behavior:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnShutdown() override {}
};
