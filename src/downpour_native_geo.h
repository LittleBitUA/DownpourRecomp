// downpour - Native Render: native scene renderer (Phase 3/4)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 3 ===
// === DPOUR MIGRATION 2026-07-25: Phase 4 - UVs, normals, textures, depth ===
//
// Captures the game's REAL scene (guest vertex/index buffers, transforms and
// bound textures, all byte-swapped out of guest memory) through the hooked UE3
// RHI, and renders it with OUR native D3D12 pipeline into our own colour+depth
// targets. Gated by env DPOUR_NR_GEO (default OFF).
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;

namespace dpour_geo {

// True when env DPOUR_NR_GEO is set (checked once).
bool Enabled();
// True when env DPOUR_NR_ONLY is set: the native image fully replaces the
// emulated one (opaque composite) instead of being drawn over it.
bool NativeOnly();
// True when env DPOUR_NR_SKIPGUEST is set: the guest's own draw submissions are
// dropped, so the Xenos emulation stops rasterising the scene entirely.
bool SkipGuestDraws();

// --- Guest render thread (called from RHI hooks) ---------------------------
// RHISetViewParameters(View, &ViewProjectionMatrix, &ViewOrigin).
void SetViewProj(const std::uint8_t* base, std::uint32_t matrix_guest_addr);
// Device SetVertexShaderConstantF(StartRegister, pData, Vector4fCount): shadow
// the guest vertex-shader constant file (UE3 keeps LocalToWorld at c6).
void SetVSConstants(const std::uint8_t* base, std::uint32_t start_reg,
                    std::uint32_t data_guest_addr, std::uint32_t vec4_count);
// RHIBeginDrawingViewport / RHIEndDrawingViewport: frame boundaries.
void BeginFrame();
void EndFrame();

// One indexed triangle-list draw, as the guest submits it to its D3D device.
constexpr std::uint32_t kMaxStreams = 4;

struct DrawInputs {
  std::uint32_t ib_guest = 0;                 // FXeIndexBuffer*
  std::uint32_t vb_guest[kMaxStreams] = {};   // bound vertex buffers
  std::uint32_t stride[kMaxStreams] = {};     // their strides
  std::uint32_t decl_guest = 0;               // vertex declaration: THE vertex layout
  std::uint32_t base_vertex = 0;              // added to every index
  std::uint32_t start_index = 0;              // first index of this section
  std::uint32_t index_count = 0;              // exact count, from the device call
};
// Returns true when this draw was captured and WILL be rendered natively, so
// the caller may drop the guest's own submission for it. Anything we did not
// capture (UI, menus, movies, particles, skinned meshes, post-processing) must
// still go to the guest renderer or the frame falls apart.
bool CaptureDraw(const std::uint8_t* base, const DrawInputs& in);

// --- UI thread (called from the presenter's UIDrawer) ----------------------
// Runs our native render pass (own colour+depth targets, submitted on the
// runtime's direct queue ahead of the presenter's list) and composites the
// result onto the backbuffer using the presenter's command list.
void Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd,
            std::uint32_t width, std::uint32_t height);

}  // namespace dpour_geo
// === END DPOUR MIGRATION 2026-07-25 ===
