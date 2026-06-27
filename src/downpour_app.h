// downpour - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "downpour_title_update_installer.h"

class DownpourApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DownpourApp>(new DownpourApp(ctx, "downpour",
        downpour_PPCImageConfig));
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
    return "Silent Hill: Downpour v1.0 | \xc2\xabLittle Bit\xc2\xbb";
  }

  // SDK v0.8.1.19+ handles Win32 MMCSS + high_resolution_timer automatically
  // (cvars win32_mmcss / win32_high_resolution_timer, both default true).
  // Override hooks here only if you need Downpour-specific behavior:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
};
