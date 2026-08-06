// downpour - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "downpour_iso_installer.h"
#include "downpour_title_update_installer.h"

class DownpourApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DownpourApp>(new DownpourApp(ctx, "downpour",
        downpour_PPCImageConfig));
  }

  // Gate the runtime launch behind the two first-run install steps:
  //
  //  1. Game data — if assets/default.xex is missing, open the disc image
  //     installer wizard: the user picks their own Downpour .iso and its
  //     XDVDFS game partition is extracted into game_data_root. The wizard
  //     also best-effort auto-stages TU1 so a fresh install usually needs
  //     exactly one user action (picking the ISO).
  //  2. Title Update 1 — if default.xexp is still not staged (offline
  //     install, mirror down), open the TU installer wizard.
  //
  // Mirrors the Skate 3 / EA installer pattern (see
  // mchughalex/skate3recomp/src/skate3_app_common.cpp). Honors
  // DOWNPOUR_INSTALL_ISO and DOWNPOUR_INSTALL_TU env overrides for headless
  // installs (a path to the source file, or "download" for the TU).
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig runtime_paths = defaults;
    const auto& game_root = runtime_paths.game_data_root;

    if (!downpour::IsGameDataInstalled(game_root)) {
      if (const char* iso = std::getenv("DOWNPOUR_INSTALL_ISO");
          iso != nullptr && *iso != '\0') {
        std::string error;
        REXLOG_INFO("Installing game data from DOWNPOUR_INSTALL_ISO={}", iso);
        if (!downpour::InstallGameDataFromIso(iso, game_root, nullptr, nullptr,
                                              error)) {
          REXLOG_ERROR("Automated game data installation failed: {}", error);
        }
      }
    }
    if (!downpour::IsGameDataInstalled(game_root)) {
      REXLOG_INFO(
          "Silent Hill: Downpour game data not found at {}; launching the "
          "disc image installer.",
          game_root.string());
      downpour::ShowIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                     std::move(resume));
      return std::nullopt;
    }

    if (!downpour::IsTitleUpdateInstalled(game_root)) {
      if (const char* tu = std::getenv("DOWNPOUR_INSTALL_TU");
          tu != nullptr && *tu != '\0') {
        std::string error;
        REXLOG_INFO("Installing title update from DOWNPOUR_INSTALL_TU={}", tu);
        const bool ok =
            std::string_view(tu) == "download"
                ? downpour::TryDownloadAndStageTitleUpdate(game_root, error)
                : downpour::StageTitleUpdateFromFile(tu, game_root, error);
        if (!ok) {
          REXLOG_ERROR("Automated title update installation failed: {}", error);
        }
      }
    }
    if (downpour::IsTitleUpdateInstalled(game_root)) {
      return runtime_paths;
    }
    REXLOG_INFO(
        "Silent Hill: Downpour Title Update 1 not staged at {}; launching the "
        "title update installer.",
        game_root.string());
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
