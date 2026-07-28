// downpour - Native Render: the bring-up panel inside the SDK's F3 overlay
//
// === DPOUR MIGRATION 2026-07-29: STOP RESTARTING TO COMPARE =================
//
// Every renderer question this project has answered was answered by comparing
// two pictures, and until now each comparison cost a process restart plus a walk
// back through a menu and a loading screen. Three of the last four findings took
// that round trip several times over. The switches themselves are cheap; the
// walk is what was expensive.
//
// So they move here, into the overlay the SDK already binds to F3. Nothing about
// the renderer changes: the environment variables still seed every value, which
// is what scripted and headless runs use, and this only lets them be moved
// afterwards.
//
// WHAT IS DELIBERATELY NOT HERE. Anything that reallocates: target formats, the
// own-device switch, the resolve-copy pipeline. Those decide what exists rather
// than what is shown, and a live toggle would have to tear down resources the
// GPU is still reading. They stay startup-only, where a restart is honest.

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/overlay/debug_overlay.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "downpour_native_scene.h"
#include "downpour_native_surfaces.h"

// === THE SAME SWITCHES, IN THE SETTINGS TREE ================================
//
// F4's settings overlay builds its tree from cvar categories, and it is where
// anyone looks for a setting - the F3 overlay is where counters live. So the
// three knobs are cvars under Downpour, and BOTH surfaces drive the same
// storage: the panel below writes them and reads them back, so moving one moves
// the other and neither can drift.
//
// They also inherit what cvars already do: the runtime's config applies them at
// startup, the console can set them by name, and REX_* environment overrides
// keep working (src/core/cvar.cpp ApplyEnvironment).
REXCVAR_DEFINE_STRING(nr_show_surface, "", "Downpour/Native Render",
                      "Show ONE surface whole instead of the game's frame. Empty = the frame. "
                      "Takes a guest address (0x40cb6680) or a name fragment (DefaultColor).");
REXCVAR_DEFINE_DOUBLE(nr_amplify, 0.0, "Downpour/Native Render",
                      "Multiply the composited colour. 0 = off. Use 0.125 for scene colour "
                      "(the shaders write x8), 0.03125 for the fixed-point surfaces, "
                      "-1 to classify (grey never written, yellow written black, blue positive).");
REXCVAR_DEFINE_BOOL(nr_keep_color_bias, true, "Downpour/Native Render",
                    "Keep the game's own EDRAM exponent bias: its shaders multiply by 2^bias and "
                    "each resolve divides by the SAME surface's bias, as the console does. Off "
                    "forces c0 to 1 and stops the resolves scaling.");

namespace dpour_scene {
namespace {

// The exposure presets are not round numbers picked by eye: they are the exact
// divisors XeGetRenderTargetColorExpBias can return (XeD3DRenderTarget.cpp:94),
// which is what any surface in this game is written with.
struct AmplifyPreset {
  const char* label;
  float value;
  const char* why;
};
constexpr AmplifyPreset kAmplifyPresets[] = {
    {"off (1x)", 0.0f, "what the composite would show on its own"},
    {"1/8  bias 3", 0.125f, "scene colour: shaders write x8, the resolve divides it back"},
    {"1/32 bias 5", 0.03125f, "DefaultColorFixedPoint and the A16B16G16R16 surfaces"},
    {"4x", 4.0f, "lift a picture that is too dark to judge"},
    {"32x", 32.0f, "is it very dark, or exactly zero?"},
    {"classify", -1.0f, "grey never written, yellow written black, blue positive"},
};

bool FloatsClose(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void DrawPanel() {
  // --- what reaches the screen ------------------------------------------------
  ImGui::TextUnformatted("Shown");
  ImGui::Separator();

  std::string current = ShowSurfaceSpecCopy();
  const bool showing_frame = current.empty();
  if (ImGui::RadioButton("the game's frame", showing_frame)) {
    SetShowSurfaceSpec(nullptr);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("The composed frame, through the post chain and the final quad.\n"
                      "Anything else below bypasses all of that and shows one surface whole.");
  }

  // The picker is the game's own surface table - it names every one of them at
  // RHICreateTargetableSurface, and the surface layer records the name, the
  // size, the EDRAM range and the family it belongs to.
  static std::vector<dpour_surfaces::Surface> surfaces;
  static double last_refresh = -1.0;
  const double now = ImGui::GetTime();
  if (last_refresh < 0.0 || now - last_refresh > 1.0) {
    last_refresh = now;
    surfaces.clear();
    dpour_surfaces::ListSurfaces(surfaces);
    // Colour first, then by name, so the interesting ones are not scattered
    // among twenty AuxColors of different sizes.
    std::sort(surfaces.begin(), surfaces.end(),
              [](const dpour_surfaces::Surface& a, const dpour_surfaces::Surface& b) {
                if (a.is_depth != b.is_depth) {
                  return !a.is_depth;
                }
                const int c = _stricmp(a.name, b.name);
                return c != 0 ? c < 0 : a.object < b.object;
              });
  }

  if (ImGui::BeginCombo("surface", showing_frame ? "(none)" : current.c_str())) {
    for (const auto& s : surfaces) {
      char label[96];
      std::snprintf(label, sizeof(label), "%s  %ux%u  %#x%s", s.name, s.w, s.h, s.object,
                    s.is_depth ? "  [depth]" : "");
      char spec[32];
      std::snprintf(spec, sizeof(spec), "%#x", s.object);
      if (ImGui::Selectable(label, current == spec)) {
        SetShowSurfaceSpec(spec);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("EDRAM [%u..%u)  %u tiles\nnative target: %#x%s", s.edram_offset,
                          s.edram_offset + s.edram_size, s.edram_size, s.group,
                          s.group != s.object ? "  (shares the DefaultColor family's)" : "");
      }
    }
    ImGui::EndCombo();
  }
  if (!showing_frame) {
    ImGui::SameLine();
    if (ImGui::SmallButton("back to frame")) {
      SetShowSurfaceSpec(nullptr);
    }
  }

  // --- exposure ---------------------------------------------------------------
  ImGui::Spacing();
  ImGui::TextUnformatted("Exposure");
  ImGui::Separator();

  float amp = AmplifyFactor();
  for (const auto& p : kAmplifyPresets) {
    if (ImGui::RadioButton(p.label, FloatsClose(amp, p.value))) {
      SetAmplifyFactor(p.value);
      amp = p.value;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", p.why);
    }
  }
  if (amp > 0.0f) {
    float slider = amp;
    if (ImGui::SliderFloat("x", &slider, 0.01f, 64.0f, "%.4f", ImGuiSliderFlags_Logarithmic)) {
      SetAmplifyFactor(slider);
    }
  }

  // --- the exponent bias ------------------------------------------------------
  ImGui::Spacing();
  ImGui::TextUnformatted("EDRAM exponent bias");
  ImGui::Separator();

  bool keep = KeepColorBias();
  if (ImGui::Checkbox("keep the game's own bias", &keep)) {
    SetKeepColorBias(keep);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "ON  - the game's shaders multiply by 2^bias (PSR_ColorBiasFactor) and each\n"
        "      resolve divides by the SAME surface's bias. This is what the console does.\n"
        "OFF - c0 is forced to 1 and the resolves stop scaling. Self-consistent, but the\n"
        "      engine also pushes bias-derived scalars into constants we cannot reach\n"
        "      (EmissiveAlphaMaskScale, ShadowRendering.h:880), so those stay at 8.");
  }
  ImGui::TextDisabled("a surface's divisor comes from the surface, never from its family");
}

}  // namespace

// Two faces, one state. Called once per frame from BeginFrame.
//
// Whichever side moved last wins, which is the only rule that makes both
// usable: a cvar changed from the settings overlay or the console reaches the
// renderer, and a control moved in the F3 panel shows up in the settings tree
// and gets written out with the rest of the config.
//
// The FIRST pass is different, and deliberately so: the environment variables
// seed the renderer before any of this runs, and a seeded value must not be
// erased by a cvar still sitting at its default. So on the first pass the
// renderer pushes outward, unless the cvar is already off its default - which
// only happens when the config or the command line set it on purpose.
void SyncDebugKnobs() {
  static bool primed = false;
  static std::string seen_surface;
  static double seen_amp = 0.0;
  static bool seen_bias = true;

  if (!primed) {
    primed = true;
    const std::string env_surface = ShowSurfaceSpecCopy();
    if (!env_surface.empty() && REXCVAR_GET(nr_show_surface).empty()) {
      REXCVAR_SET(nr_show_surface, env_surface);
    }
    const float env_amp = AmplifyFactor();
    if (env_amp != 0.0f && REXCVAR_GET(nr_amplify) == 0.0) {
      REXCVAR_SET(nr_amplify, static_cast<double>(env_amp));
    }
    REXCVAR_SET(nr_keep_color_bias, KeepColorBias());
    seen_surface = REXCVAR_GET(nr_show_surface);
    seen_amp = REXCVAR_GET(nr_amplify);
    seen_bias = REXCVAR_GET(nr_keep_color_bias);
    SetShowSurfaceSpec(seen_surface.empty() ? nullptr : seen_surface.c_str());
    SetAmplifyFactor(static_cast<float>(seen_amp));
    return;
  }

  const std::string& cv_surface = REXCVAR_GET(nr_show_surface);
  if (cv_surface != seen_surface) {
    seen_surface = cv_surface;
    SetShowSurfaceSpec(cv_surface.empty() ? nullptr : cv_surface.c_str());
  } else {
    const std::string live = ShowSurfaceSpecCopy();
    if (live != seen_surface) {
      seen_surface = live;
      REXCVAR_SET(nr_show_surface, live);
    }
  }

  const double cv_amp = REXCVAR_GET(nr_amplify);
  if (cv_amp != seen_amp) {
    seen_amp = cv_amp;
    SetAmplifyFactor(static_cast<float>(cv_amp));
  } else {
    const double live = static_cast<double>(AmplifyFactor());
    if (live != seen_amp) {
      seen_amp = live;
      REXCVAR_SET(nr_amplify, live);
    }
  }

  const bool cv_bias = REXCVAR_GET(nr_keep_color_bias);
  if (cv_bias != seen_bias) {
    seen_bias = cv_bias;
    SetKeepColorBias(cv_bias);
  } else if (KeepColorBias() != seen_bias) {
    seen_bias = KeepColorBias();
    REXCVAR_SET(nr_keep_color_bias, seen_bias);
  }
}

void RegisterDebugPanel() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  rex::ui::RegisterDebugOverlaySection("Downpour native render", &DrawPanel, /*wants_input=*/true);
}

}  // namespace dpour_scene
