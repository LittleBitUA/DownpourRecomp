// downpour - Native Render (Phase 0 harness)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 0 (hook harness) ===
//
// Goal of this file: prove, end-to-end on the REAL recompiled binary, that we
// can intercept a UE3 RHI / guest render function at its guest address, run
// native C++ code, and hand control back to the original — the exact mechanism
// MarathonRecomp / UnleashedRecomp use (GUEST_FUNCTION_HOOK over a weak guest
// symbol, with an `__imp__` trampoline to the original body). This is the
// foundation every later native-render phase builds on. NO PM4, no Xenia
// crutches: we sit ABOVE the guest's XeD3D driver, at the statically-linked
// out-of-line RHI boundary.
//
// Why sub_82D22B08:
//   `__imp__VdSwap` is called at exactly ONE site in the whole recomp
//   (downpour_recomp.84.cpp, guest 0x82D22EE4), inside DEFINE_REX_FUNC(
//   sub_82D22B08). Refined by call-graph analysis: sub_82D22B08 is the low-level
//   D3D `Direct3DDevice::Swap` primitive (the direct VdSwap wrapper that advances
//   the command-ring put-pointer), not the free `XePerformSwap` (that is
//   sub_82D23340, which calls Swap; RHIEndDrawingViewport is inlined into the
//   engine render-thread present sub_82D9E830). Either way sub_82D22B08 is THE
//   present point: sole VdSwap site, runs once per presented frame — an ideal,
//   unambiguous probe. PROVEN LIVE 2026-07-24 (log: frames 1,61,121…901, clean run).
//
// Safety / gating (honors the project rule: no default-ON render changes):
//   * DEFAULT OFF. With env var DPOUR_NR unset/0 we call straight through to
//     the original with ZERO added behavior — the build is byte-for-byte
//     equivalent in effect to not having this hook.
//   * With DPOUR_NR=1 we additionally log every 60th present, then still call
//     the original. Nothing about the frame is altered yet; this phase only
//     observes.
//
// Revert (if this ever breaks anything): delete this file and remove its line
// from CMakeLists.txt (DOWNPOUR_SOURCES), then rebuild. grep DPOUR MIGRATION.

#define _CRT_SECURE_NO_WARNINGS  // std::getenv gate read below

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>       // REX_HOOK_RAW, REX_EXTERN, PPCContext, REX_FUNC
#include <rex/logging.h>    // REXLOG_INFO

#include "downpour_native_d3d12.h"  // Phase 1a native-D3D12 self-test

// The original recompiled body of the present routine. DEFINE_REX_FUNC emits
// `sub_82D22B08` as a weak alias to this strong symbol, so our strong override
// below wins the link while this remains callable as the passthrough.
REX_EXTERN(__imp__sub_82D22B08);

namespace {

// Read the gate once (env var). Default OFF.
bool NativeRenderEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

std::atomic<std::uint64_t> g_present_count{0};

}  // namespace

// Strong override of the weak guest symbol sub_82D22B08 (XePerformSwap).
// REX_HOOK_RAW gives us the raw (ctx, base) so we can observe and then delegate
// to the untouched original.
REX_HOOK_RAW(sub_82D22B08) {
  if (NativeRenderEnabled()) {
    // Phase 1a: prove the native D3D12 device pipeline once, off the first
    // present. call_once → no-op on every subsequent frame.
    dpour_nr::RunD3D12SelfTestOnce();

    const std::uint64_t n = g_present_count.fetch_add(1) + 1;
    if ((n % 60) == 1) {
      REXLOG_INFO(
          "[native-render] present hook LIVE (D3D Swap sub_82D22B08) "
          "frame {} — RHI hook boundary proven",
          n);
    }
  }
  // Hand back to the original guest present so the frame still swaps.
  __imp__sub_82D22B08(ctx, base);
}

// === END DPOUR MIGRATION 2026-07-24 ===
