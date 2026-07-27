// downpour - Native Render: on-screen native D3D12 overlay (Phase 1b)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 1b (first visible pixel) ===
//
// What this proves: native D3D12 draw commands, recorded by OUR code against the
// RUNTIME's real device, landing on the REAL swapchain backbuffer — the first
// on-screen native pixel. It draws a small magenta quad in the top-left corner,
// on top of the Xenos-emulated game image.
//
// How it stays safe and sanctioned (no PM4, no synchronization hacks):
//   * We register a rex::ui::UIDrawer with the presenter — the SAME extension
//     point Dear ImGui uses. The presenter invokes our Draw() on the UI thread,
//     inside its own command list, with the backbuffer RTV already bound and in
//     RENDER_TARGET state (see d3d12_presenter.cpp: OMSetRenderTargets then
//     ExecuteUIDrawersFromUIThread). We only append draw state + a DrawInstanced;
//     we never transition resources, touch the swapchain, or fight the queue.
//   * We reuse the runtime's device via D3D12Provider::GetDevice(), so there is
//     no cross-device interop.
//   * Our own tiny root signature + PSO (SV_VertexID quad, constant magenta PS).
//     Each UI drawer sets its own state, so leftover state can't corrupt others.
//
// Gated by env DPOUR_NR_OVERLAY (default OFF) — this is a VISIBLE change, so it
// gets its own flag, separate from DPOUR_NR. Revert: delete this file +
// downpour_native_overlay.h, drop them from CMakeLists.txt, remove the
// OnCreateDialogs hook in downpour_app.h.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_overlay.h"

#include <cstdint>
#include <cstdlib>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <rex/logging.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

#include "downpour_native_geo.h"  // Phase 3: native game-geometry renderer
#include "downpour_native_pipeline.h"  // Phase 5: root signature / PSO for real shaders
#include "downpour_native_scene.h"     // the draw path: the game's own shaders drawing
#include "downpour_native_tex.h"       // the shared bindless heap

using Microsoft::WRL::ComPtr;

namespace dpour_nr {
namespace {

// Backbuffer format the presenter composites into (d3d12_presenter.h:kSwapChainFormat).
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// Tiny self-contained shaders: a top-left corner quad from SV_VertexID (no
// vertex buffer, no input layout) painted constant magenta.
// A top-left corner quad from SV_VertexID (no vertex buffer). The pixel colour
// pulses from a per-frame root constant (b0) — proving dynamic per-frame native
// rendering AND the root-constant / per-draw shader-parameter path (the same
// machinery Phase 4 needs for RHISetPixelShaderParameter -> CBV).
constexpr char kShaderSrc[] = R"HLSL(
cbuffer Pulse : register(b0) { float uT; };
static const float2 kVerts[6] = {
  float2(-1.0,  1.0), float2(-0.5,  1.0), float2(-1.0,  0.5),
  float2(-0.5,  1.0), float2(-0.5,  0.5), float2(-1.0,  0.5)
};
float4 VSMain(uint vid : SV_VertexID) : SV_Position {
  return float4(kVerts[vid], 0.0, 1.0);
}
float4 PSMain() : SV_Target {
  float s = 0.5 + 0.5 * sin(uT);
  return float4(1.0, s, 1.0, 1.0);  // magenta <-> white pulse
}
)HLSL";

class NativeOverlayDrawer final : public rex::ui::UIDrawer {
 public:
  NativeOverlayDrawer(const rex::ui::d3d12::D3D12Provider* provider, bool marker_quad_enabled)
      : provider_(provider), marker_quad_enabled_(marker_quad_enabled) {}

  void Draw(rex::ui::UIDrawContext& context) override {
    auto& d3d_ctx = static_cast<rex::ui::d3d12::D3D12UIDrawContext&>(context);
    ID3D12GraphicsCommandList* cmd = d3d_ctx.command_list();
    if (cmd == nullptr) {
      return;
    }
    const UINT w = context.render_target_width();
    const UINT h = context.render_target_height();

    // Phase 3: render the game's REAL captured geometry with our native D3D12
    // pipeline (own device/PSO/shaders). Gated by DPOUR_NR_GEO.
    dpour_geo::Render(provider_->GetDevice(), provider_->GetDirectQueue(), cmd, w, h);

    // The draw path: the frame the game itself described, drawn from its own
    // translated shaders. Gated by DPOUR_NR_SHADERS.
    dpour_scene::Render(provider_->GetDevice(), provider_->GetDirectQueue(), cmd, w, h);

    // Phase 5: the translated-shader path needs the bindless heap (and through
    // it the device) even when the Phase 3 renderer is off - the dry run builds
    // pipelines without drawing anything, so it must not require a visual mode
    // to be enabled just to get at the device.
    if (dpour_pipeline::DryRunEnabled()) {
      dpour_tex::EnsureHeap(provider_->GetDevice());
    }

    if (!marker_quad_enabled_ || !EnsurePipeline()) {
      return;
    }
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetGraphicsRootSignature(root_sig_.Get());
    // Per-frame pulse value fed as a root 32-bit constant (b0).
    const float t = static_cast<float>(frame_++) * 0.08f;
    cmd->SetGraphicsRoot32BitConstants(0, 1, &t, 0);
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, 1, 0, 0);

    if (!logged_first_draw_) {
      logged_first_draw_ = true;
      REXLOG_INFO(
          "[native-overlay] first native draw onto the REAL backbuffer "
          "(magenta corner quad via runtime device) — on-screen native path LIVE");
    }
  }

 private:
  bool EnsurePipeline() {
    if (pso_) {
      return true;
    }
    if (init_failed_) {
      return false;
    }
    init_failed_ = true;  // flip back to false only on full success

    ID3D12Device* device = provider_->GetDevice();
    if (device == nullptr) {
      REXLOG_ERROR("[native-overlay] provider device is null");
      return false;
    }

    // Compile the shaders.
    ComPtr<ID3DBlob> vs, ps, err;
    HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "dpour_overlay", nullptr, nullptr,
                            "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-overlay] VS compile failed hr={:#010x} {}",
                   static_cast<uint32_t>(hr),
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "");
      return false;
    }
    err.Reset();
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "dpour_overlay", nullptr, nullptr, "PSMain",
                    "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-overlay] PS compile failed hr={:#010x} {}",
                   static_cast<uint32_t>(hr),
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "");
      return false;
    }

    // Root signature: one root 32-bit constant (b0, pixel-visible) for the pulse.
    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_param.Constants.ShaderRegister = 0;
    root_param.Constants.RegisterSpace = 0;
    root_param.Constants.Num32BitValues = 1;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &root_param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-overlay] serialize root sig failed hr={:#010x}",
                   static_cast<uint32_t>(hr));
      return false;
    }
    hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                     IID_PPV_ARGS(&root_sig_));
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-overlay] CreateRootSignature failed hr={:#010x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    // Pipeline state.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig_.Get();
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.SampleMask = UINT_MAX;

    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;

    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;

    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = kBackBufferFormat;
    pso_desc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) {
      REXLOG_ERROR("[native-overlay] CreateGraphicsPipelineState failed hr={:#010x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    init_failed_ = false;
    REXLOG_INFO("[native-overlay] pipeline ready (root sig + PSO on runtime device)");
    return true;
  }

  const rex::ui::d3d12::D3D12Provider* provider_;
  bool marker_quad_enabled_ = false;
  ComPtr<ID3D12RootSignature> root_sig_;
  ComPtr<ID3D12PipelineState> pso_;
  bool init_failed_ = false;
  bool logged_first_draw_ = false;
  unsigned frame_ = 0;  // drives the per-frame pulse root constant
};

}  // namespace

bool MaybeRegisterNativeOverlay(rex::system::IGraphicsSystem* graphics_system) {
  const char* v = std::getenv("DPOUR_NR_OVERLAY");
  const bool marker_quad = (v != nullptr && v[0] != '\0' && v[0] != '0');
  // The drawer also hosts the Phase 3 native geometry renderer, and it is the
  // only place the D3D12 device reaches our code - the Phase 5 dry run needs it
  // too, even though it draws nothing.
  if (!marker_quad && !dpour_geo::Enabled() && !dpour_pipeline::DryRunEnabled() &&
      !dpour_scene::Enabled()) {
    return true;  // default OFF: nothing to do, stop retrying
  }
  if (graphics_system == nullptr) {
    return false;  // not ready yet — caller should retry
  }

  auto* gs = static_cast<rex::graphics::GraphicsSystem*>(graphics_system);
  auto* provider = static_cast<rex::ui::d3d12::D3D12Provider*>(gs->provider());
  rex::ui::Presenter* presenter = gs->presenter();
  if (provider == nullptr || presenter == nullptr) {
    return false;  // not ready yet — caller should retry
  }

  // Lives for the process lifetime (never unregistered).
  static NativeOverlayDrawer drawer(provider, marker_quad);
  presenter->AddUIDrawerFromUIThread(&drawer, /*z_order=*/1000000);
  REXLOG_INFO(
      "[native-overlay] native D3D12 drawer registered (marker_quad={}, native_geometry={}, "
      "device={})",
      marker_quad, dpour_geo::Enabled(), static_cast<void*>(provider->GetDevice()));
  return true;
}

}  // namespace dpour_nr
// === END DPOUR MIGRATION 2026-07-24 ===
