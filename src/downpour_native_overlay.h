// downpour - Native Render: on-screen native D3D12 overlay (Phase 1b)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 1b ===
// See downpour_native_overlay.cpp. Registers a native D3D12 UIDrawer that draws
// a marker onto the REAL swapchain backbuffer via the runtime's own device —
// the first visible native pixel. Gated by env DPOUR_NR_OVERLAY (default OFF).
#pragma once

namespace rex::system {
class IGraphicsSystem;
}

namespace dpour_nr {

// Register the native overlay drawer. Must be called ON THE UI THREAD once the
// graphics system + presenter are live (deferred from DownpourApp until
// runtime()->graphics_system() is non-null). No-op unless env DPOUR_NR_OVERLAY
// is set. Returns true once registration actually happened (so the caller can
// stop retrying).
bool MaybeRegisterNativeOverlay(rex::system::IGraphicsSystem* graphics_system);

}  // namespace dpour_nr
// === END DPOUR MIGRATION 2026-07-24 ===
