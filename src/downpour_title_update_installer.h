/**
 * @file        downpour_title_update_installer.h
 *
 * @brief       Stages Silent Hill: Downpour Title Update 1 payload
 *              (default.xexp) next to the installed game so the runtime can
 *              load it. The wizard accepts either an STFS LIVE container
 *              dumped from an Xbox 360 console (e.g. found under
 *              Content/<XUID>/4B4E0823/000B0000/) or a raw default.xexp
 *              file. Modelled directly on
 *              mchughalex/skate3recomp/src/skate3_title_update_installer.*
 *              with the payload list reduced to one entry — Downpour TU1
 *              only patches the executable; no DLC / web-resource overlay
 *              files ship with this update.
 */
#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace downpour {

// True when the title update payload (default.xexp) is staged in game_root
// and matches the pinned SHA-256 hash the recompilation expects.
bool IsTitleUpdateInstalled(const std::filesystem::path& game_root);

// Stages the title update payload into game_root from a local source file:
// either the TU STFS package (CON/LIVE/PIRS container) or a raw .xexp.
bool StageTitleUpdateFromFile(const std::filesystem::path& source,
                              const std::filesystem::path& game_root, std::string& error);

void ShowTitleUpdateInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                  std::function<void(rex::PathConfig)> complete);
bool RunTitleUpdateInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                         rex::ui::Window* window,
                                         rex::ui::ImGuiDrawer* drawer,
                                         rex::PathConfig runtime_paths,
                                         rex::PathConfig& installed_paths);

}  // namespace downpour
