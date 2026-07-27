// downpour - Native Render: standalone D3D12 device self-test (Phase 1a)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 1a ===
// One-shot, isolated D3D12 bring-up proof. See downpour_native_d3d12.cpp.
#pragma once

namespace dpour_nr {

// Runs the isolated native-D3D12 self-test exactly once (thread-safe). No-op on
// every call after the first. Safe to call from the per-frame present hook.
void RunD3D12SelfTestOnce();

}  // namespace dpour_nr
// === END DPOUR MIGRATION 2026-07-24 ===
