// downpour - Native Render: the game's own draws, on our own D3D12 pipeline
//
// === DPOUR MIGRATION 2026-07-25: draw path ===
//
// This is the piece every other module was built for. Everything the game needs
// to draw already exists natively - its shaders translated to DXIL, its vertex
// declarations turned into host input layouts, its vertex and index data
// byte-swapped into host buffers, its textures decoded, its constant banks
// shadowed, its pipeline state read out of the device - so a draw becomes what
// it is in UnleashedRecomp: bind, set constants, DrawIndexedInstanced.
//
// Nothing here reconstructs GPU packets and nothing emulates the Xenos. The
// division of work is the reference one:
//
//   guest render thread   intercept the draw, resolve every input, build (or
//                         reuse) the pipeline, snapshot the constants, record a
//                         flat description of the draw
//   render thread         replay the frame's descriptions into our own colour +
//                         depth targets on our own command list, then composite
//
// Capture must resolve everything up front because the two threads are not in
// lockstep: the constant banks and the device state keep moving after a draw is
// recorded, so anything read later would belong to a different draw.
//
// VISIBLE, therefore default OFF, behind its own DPOUR_NR_DRAW - the frame the
// game already produces is not touched until this has been looked at. It is
// deliberately not the same flag as DPOUR_NR_SHADERS, which only loads the
// translated shader cache and is what the dry-run verification runs with.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace dpour_shadercache {
struct Shader;
}

namespace dpour_scene {

// DPOUR_NR_SHADERS: capture and render the game's draws natively.
bool Enabled();
// DPOUR_NR_SHADERS_ONLY: composite opaquely, so the native image is all there is
// to see - the honest way to judge it without the emulated frame underneath.
bool NativeOnly();
// DPOUR_NR_SHADERS_SKIPGUEST: drop the guest's own submission for draws we
// reproduced, so the Xenos emulation stops rasterising them.
bool SkipGuestDraws();

// DPOUR_NR_OWN_DEVICE: the Unleashed/Marathon arrangement - we own the device,
// the guest's submissions never reach the GPU, and there is no emulated frame
// to fall back to or compose with. Implies the guest-output replacement.
//
// Callers outside this module use it to drop the guest's PACKET WRITERS. Which
// functions those are was measured, not guessed: of the 74 device functions the
// game's RHI calls, 39 reach the command buffer and 35 only update the device's
// register shadow (reference_downpour_device_api_classification).
bool OwnDevice();

// --- guest render thread ----------------------------------------------------
void BeginFrame();
void EndFrame();

// RHISetRenderTarget(NewRenderTarget, NewDepthStencilTarget): a frame contains
// several passes - shadow depth, reflections, the scene, post-processing - and
// they are told apart by which surface they draw into. color_guest/depth_guest
// are the FSurfaceRHIRef POINTERS the RHI call carries (stable per target,
// good enough as pass keys); color_object is the dereferenced first dword -
// the raw surface object - which is what the create hook records and what the
// scene marker matches on. The two are NOT interchangeable: the ref lives in
// GSceneRenderTargets' static array (0x837c....), the object on the heap.
//
// is_backbuffer: the colour object equals the guest's GD3DBackBuffer
// (0x83790044). The game binds its back buffer through ordinary refs (stack
// temporaries and GSceneRenderTargets members), so "a null colour ref" never
// identifies it - only the object comparison does. The back-buffer pass is the
// game's OWN final composition (post-process quads sampling the scene resolve,
// then the UI), which makes it the one honest thing to put on screen.
void SetRenderTarget(std::uint32_t color_guest, std::uint32_t depth_guest,
                     std::uint32_t color_object, std::uint32_t depth_object,
                     bool is_backbuffer);

// D3DDevice::SetRenderTarget(index, surface) / SetDepthStencilSurface(surface) -
// the DEVICE-level binds, and the only complete view of which surface a draw
// lands on.
//
// RHISetRenderTarget is not that view. The engine binds targets straight
// through the device in several places (the MSAA block's
// SetMainpassRenderTargets, RHIEndDrawingViewport's own
// SetRenderTarget(0, GD3DBackBuffer), the post-process chain), so passes bound
// that way were invisible and their draws were charged to whatever the RHI had
// bound last. Measured: the back-buffer pass showed 2 draws a frame while
// another target showed 110 - and the back buffer is exactly what XePerformSwap
// resolves to the front buffer (XeD3DDevice.cpp:426), i.e. the frame itself.
// Hooking the device entries is the same lesson the UP draws taught: the RHI
// layer is not where the truth is.
void DeviceSetRenderTarget(std::uint32_t index, std::uint32_t surface_object,
                           bool is_backbuffer);
void DeviceSetDepthStencil(std::uint32_t surface_object);
// Whether the device-level binds drive pass tracking (kill switch:
// DPOUR_NR_DRAW_NODEVRT). The RHI hook stands down when they do.
bool DeviceRtEnabled();

// (SceneBlockBegin / SceneBlockEnd were here. Removed: Downpour ships with
// GUseTilingCode = FALSE, so the MSAA bracket they hooked is never called, and
// neither reference has an equivalent - neither asks which draws are "the
// scene". See the note where the hooks used to live, downpour_native_draws.cpp.)

// RHICreateTargetableSurface hook: Downpour ships with GUseTilingCode = FALSE
// (XeD3DDevice.cpp:118 says so in as many words), so the MSAA block above
// never runs for it. Instead the scene binds through
// GSceneRenderTargets.BeginRenderingSceneColor() -> RHISetRenderTarget with the
// "SceneColor" surface - so knowing WHICH surface that is identifies the scene
// exactly. Every named render target passes through RHICreateTargetableSurface
// with a usage string; this records the ones that matter and logs them all.
// sret = the hidden return pointer (r3 on entry; the FSurfaceRHIRef lands
// there), usage_a/usage_b = the two registers that may carry the UsageStr
// (r9 expected with the sret shift, r8 logged as the fallback).
// `pixel_format` is UE3's EPixelFormat byte (EngineTextureClasses.h:153) - the
// game's own statement of what this surface holds. The reference gives every
// guest surface its guest format's host equivalent (ConvertFormat,
// video.cpp:3065); a surface that is A8R8G8B8 on the console clips above 1.0
// exactly where the console does, which one hardcoded half-float format cannot.
void OnTargetableSurfaceCreated(const std::uint8_t* base, std::uint32_t sret,
                                std::uint32_t usage_a, std::uint32_t usage_b,
                                std::uint32_t resolve_texture, std::uint32_t size_x,
                                std::uint32_t size_y, std::uint32_t pixel_format);

// The guest is retiring a resource (AddUnusedXeResource). When it is a surface
// we hold a target for, the target and its descriptor indices go back into
// circulation - the DestructResource role of the reference (video.cpp:713).
//
// Measured: this queue carries textures and vertex-buffer orphans, not
// surfaces - 849 calls, no surface among them - because a targetable surface is
// a refcounted IDirect3DSurface9 rather than an FXeGPUResource
// (XeD3DRenderTarget.h:68). Kept because it is correct and free, but the
// registry's ceiling is what actually had to move.
void RetireSurface(std::uint32_t resource_or_surface);

// Reference-style RT sampling (UnleashedRecomp ProcStretchRect): the resolve
// call links its destination texture to the bound surface; a later sample of
// that texture binds the surface's own native target SRV. This callback is
// registered into dpour_tex so Acquire can consult the link map without a
// header cycle. Returns the SRV slot or kInvalidSlot.
// `base_addr` is the texture's GPU base address (dpour_tex::GuestBaseAddress).
// Textures the engine overlapped in one shared buffer carry the SAME address and
// must resolve to the SAME native resource - see GuestBaseAddress for the two
// lines of the game's own source that establish it. Pass 0 when unknown.
std::uint32_t RtBackedSrvSlot(std::uint32_t texture_rhi_guest, std::uint32_t base_addr);

// The guest texture ("SceneColor".Texture, FXeTexture2D*) the scene surface
// resolves into - the injection point for replacing the guest's scene with the
// native one: whoever samples this texture after a resolve is the game's own
// post-processing chain.
std::uint32_t SceneResolveTexture();

// RHICopyToResolveTarget hook (guest render thread). Reference behaviour: the
// resolve LINKS its destination texture to the source surface (no pixels move,
// UnleashedRecomp ProcStretchRect); a later sample of that texture serves the
// surface target's SRV. Returns false always for now - the guest's own
// resolve keeps running until every consumer renders natively.
// `resolve_params` = the third argument, a guest `const FResolveParams&`
// (RHI.h:630). Its ResolveTarget member OVERRIDES the surface's own resolve
// texture - XeD3DRenderTarget.cpp:399 picks it first - so a resolve that
// carries one links a different destination than surface_ref+4 names. Pass 0
// when the caller does not have it.
bool OnResolveScene(const std::uint8_t* base, std::uint32_t surface_ref,
                    std::uint32_t surface_object, std::uint32_t resolve_params);

// True when a captured scene draw may be dropped from the guest's submission.
// Plain DPOUR_NR_DRAW_SKIPGUEST when injection is off; with injection on, the
// skip is refused until the injection has actually replaced the scene texture
// at least once - skipping draws while the game still composes its own frame
// from that texture is precisely the corrupted-screen failure mode.
bool ShouldSkipGuestDraw();

// RHISetViewParameters(View, &ViewProjectionMatrix, &ViewOrigin): the frame's
// real view-projection, kept ONLY for the vertex-transform probe
// (DPOUR_NR_DRAW_VSPROBE) - it is a known-good matrix to search the uploaded
// constant banks for, not something the draw path itself uses.
void NoteViewProj(const std::uint8_t* base, std::uint32_t matrix_guest);

// RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ). The native target is sized
// from this, so viewport, scissor and the half-pixel offset are the guest's own
// numbers rather than something rescaled.
void SetViewport(std::uint32_t min_x, std::uint32_t min_y, std::uint32_t max_x,
                 std::uint32_t max_y);

// Sixteen, because that is how many the guest can bind and how many its vertex
// declarations may name - not four, which is merely how many the common vertex
// factories happen to use.
constexpr std::uint32_t kMaxStreams = 16;

struct DrawDesc {
  const dpour_shadercache::Shader* vs = nullptr;
  const dpour_shadercache::Shader* ps = nullptr;  // null on UE3's depth prepass
  std::uint32_t decl_guest = 0;
  std::uint32_t vb_guest[kMaxStreams] = {};  // FXeVertexBuffer*, for the usage flags
  // The D3D vertex buffer the device was actually handed. Its Xenos fetch
  // constant carries both the data address and the buffer's size, which is the
  // authoritative answer to "what may this draw read" - and unlike the RHI-level
  // shadow it is set immediately before every draw.
  std::uint32_t vb_d3d[kMaxStreams] = {};
  std::uint32_t stride[kMaxStreams] = {};
  std::uint32_t stream_offset[kMaxStreams] = {};  // stride * BaseVertexIndex
  // Set when vb_guest[s] and vb_d3d[s] were captured inside ONE
  // RHISetStreamSource -> device SetStreamSource call. Then the RHI object's
  // BaseAddress field IS the bound buffer's data, with nothing to deduce - the
  // property the references get for free by owning the resource.
  std::uint32_t vb_paired[kMaxStreams] = {};
  std::uint32_t ib_guest = 0;
  std::uint32_t prim_type = 0;  // D3DPRIMITIVETYPE
  bool indexed = false;
  std::uint32_t start_index = 0;
  std::uint32_t index_count = 0;
  std::uint32_t min_index = 0;    // RHIDrawIndexedPrimitive's MinIndex
  std::uint32_t num_vertices = 0; // ... and NumVertices: the exact vertex range
  std::uint32_t start_vertex = 0;
  std::uint32_t vertex_count = 0;
};

// Returns true when the draw was fully reproduced, so the caller may drop the
// guest's own submission for it.
bool CaptureDraw(const std::uint8_t* base, const DrawDesc& d);

// --- user-pointer draws (the whole 2D UI, and the final full-screen passes) --
//
// UP draws carry their vertices inline: the device's BeginVertices
// (sub_82D1E250) hands the game a write pointer into the command buffer and
// EndVertices publishes it - but End is INLINED at every call site (verified in
// the recompiled code: the publish is a bare [dev+13844]->dev+48 store), so
// there is no function boundary at which the data is complete. The capture is
// therefore split: everything about the draw EXCEPT the bytes (pipeline,
// constants, textures, pass) is snapshotted at Begin time - the engine has
// fully set its state by then, Begin itself is what commits it guest-side -
// and the bytes are copied at the next captured event, by which point the
// game's inlined End has long since run. Begin/End pairs cannot interleave
// (GInBeginVertices guards them), so one pending slot is enough.
struct UPDraw {
  DrawDesc d;                    // state inputs; vb/ib fields unused
  std::uint32_t vtx_guest = 0;   // where the game writes the vertices
  std::uint32_t vtx_count = 0;
  std::uint32_t vtx_stride = 0;
  std::uint32_t idx_guest = 0;   // indexed variant (BeginIndexedVertices)
  std::uint32_t idx_count = 0;
  bool idx_32bit = false;
  // BeginIndexedVertices' MinVertexIndex arrives already negated (the RHI
  // negates it so the device rebases indices onto the copied range); it is
  // exactly D3D12's BaseVertexLocation.
  std::int32_t base_vertex = 0;
};
void CaptureUPBegin(const std::uint8_t* base, const UPDraw& u);

// RHIClear as a stream item - the reference's RenderCommandType::Clear. The
// game's own colour, at the clear's own position, into the pass bound at the
// time. `depth` is the engine-side value; the GInvertZ flip is mirrored at
// replay, where our depth convention is decided.
void OnClear(const std::uint8_t* base, bool clear_color, std::uint32_t color_guest,
             bool clear_depth, float depth);

// Close the pending UP capture NOW, copying its bytes while they are still
// valid. The device-level capture cannot do this - it opens at BeginVertices,
// before the game has written anything - so it defers to the next captured
// event. The API-level capture below has no such problem: RHIDrawPrimitiveUP is
// handed a buffer the caller has already filled, so the copy happens inside the
// call, which is what UnleashedRecomp's DrawPrimitiveUP hook does.
void CaptureUPEnd();

// DPOUR_NR_UP_API (implied by DPOUR_NR_OWN_DEVICE): capture user-pointer draws
// at the RHI entry points the references hook - RHIDrawPrimitiveUP
// (sub_829CA900) and RHIDrawIndexedPrimitiveUP (sub_829CAE50) - instead of at
// the device's BeginVertices one level below.
//
// This is not a refinement, it removes a blocker that was self-inflicted.
// Hooking BeginVertices meant the guest's UP submission could never be dropped:
// Begin IS the allocation, it returns the pointer the game then writes into, so
// declining it hands back null and the game breaks. UnleashedRecomp
// (video.cpp:7840) and MarathonRecomp both hook DrawPrimitiveUP, the API entry,
// where the data is the CALLER's and returning early costs nothing.
//
// When on, the BeginVertices hooks stand down: the RHI entry above them calls
// straight into BeginVertices, so capturing at both would record every UP draw
// twice.
bool UpAtApiLevel();

// --- render thread ----------------------------------------------------------
void Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd,
            std::uint32_t width, std::uint32_t height);

void LogStats();

}  // namespace dpour_scene
// === END DPOUR MIGRATION 2026-07-25 ===
