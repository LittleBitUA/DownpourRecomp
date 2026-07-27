// downpour - Native Render: RHI draw/state hook harness
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 2 (RHI render-path hooks) ===
// === DPOUR MIGRATION 2026-07-25: Phase 4 - stream offsets, textures, guest-draw gate ===
//
// These hooks ARE the boundary of the native renderer: they intercept the
// game's REAL UE3 RHI + D3D device calls, hand the data to our native D3D12
// backend, and (unless told otherwise) call straight through. Same
// GUEST_FUNCTION_HOOK + `__imp__` trampoline mechanism as Phase 0.
//
// Resolved addresses (content-based RE against the X360 UE3 XeD3DDrv source,
// anchored on GDirect3DDevice = 0x83790010 - see reference_downpour_rhi_address_map):
//   sub_829C8AA8 = RHISetStreamSource        sub_829C8BD8 = RHISetViewport
//   sub_829C8D80 = RHISetBoundShaderState    sub_829C8E68 = RHISetSamplerState
//   sub_829C94F0 = RHISetViewParameters      sub_829C9BB0 = RHISetRenderTarget
//   sub_829CA050 = RHIDrawPrimitive          sub_829CA0E8 = RHIDrawIndexedPrimitive
//   sub_829CAF60 = RHIClear                  sub_829CDDC0 = RHICopyToResolveTarget
//   sub_829CE888 = RHICopyFromResolveTarget  sub_829CD600 = RHICreateTargetableSurface
//   sub_829D42F8 = RHIBeginDrawingViewport   sub_829D4458 = RHIEndDrawingViewport
// D3D device methods (0x82D1xxxx):
//   sub_82D1A5C0 = SetStreamSource(dev, stream, res, byteOffset, stride)
//   sub_82D1D790 = SetVertexShaderConstantF(dev, startReg, data, vec4Count)
//   sub_82D1EC98 = DrawVertices(dev, primType, startVertex, vertexCount)
//   sub_82D1F0B0 = DrawIndexedVertices(dev, primType, baseVertex, startIndex, indexCount)

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>     // REX_HOOK_RAW, REX_EXTERN, PPCContext
#include <rex/logging.h>  // REXLOG_INFO

#include "downpour_native_constants.h"  // guest VS/PS constant banks
#include "downpour_native_pipeline.h"   // root signature, PSO cache, sampler heap
#include "downpour_native_decl.h"  // vertex declarations, straight from the RHI
#include "downpour_native_geo.h"   // native geometry capture / renderer
#include "downpour_native_scene.h"    // the draw path: the game's own shaders drawing
#include "downpour_native_shaders.h"  // translated DXIL for the game's own shaders
#include "downpour_native_tex.h"   // native texture cache
#include "downpour_native_ue3.h"   // the engine's own mesh stream (data-driven layer)
#include "downpour_native_vbuffers.h"  // host vertex/index buffers + guest-free invalidation
#include "downpour_shader_dump.h"  // harvest Xenos shader containers

namespace {

bool HooksEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR_HOOKS");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

// Is ANY part of the native renderer switched on?
//
// This exists because "the feature is off" was not the same thing as "the code
// costs nothing". The shader-constant hooks below shadow every parameter the
// game sets - hundreds of thousands of times a session, with a page probe each -
// and none of that data is read unless something is enabled. Measured against a
// binary built without the native layer at all, the difference was a 90th
// percentile frame of 109 ms versus 18 ms: the game held 60 FPS either way, but
// the tail was ruined.
//
// The test is "any DPOUR_NR* variable is set" rather than a list of names, so a
// switch added later cannot be forgotten here and silently do nothing.
bool NativeActive() {
  static const bool active = [] {
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
      if (std::strncmp(*e, "DPOUR_NR", 8) == 0) {
        const char* eq = std::strchr(*e, '=');
        if (eq != nullptr && eq[1] != '\0' && eq[1] != '0') {
          return true;
        }
      }
    }
    return false;
  }();
  return active;
}

inline void CountAndLog(std::atomic<std::uint64_t>& counter, const char* label,
                        std::uint64_t interval) {
  const std::uint64_t n = counter.fetch_add(1) + 1;
  if (n == 1 || (n % interval) == 0) {
    REXLOG_INFO("[native-hook] {} call #{}", label, n);
  }
}

// Shadow of the guest's bound geometry state. All of it is written from the one
// render thread and read from the same thread inside the draw hook, so relaxed
// atomics are enough (they only keep the compiler honest).
// UE3 binds up to sixteen vertex streams (its own reset loop clears sixteen
// entries), and a vertex declaration is free to name any of them. Following only
// four meant every declaration that reached higher was dropped for "a stream is
// not bound" while the stream in question was bound perfectly well - we simply
// were not watching it.
constexpr std::uint32_t kMaxStreams = 16;
std::atomic<std::uint32_t> g_stream_vb[kMaxStreams] = {};      // FXeVertexBuffer* (RHI object)
std::atomic<std::uint32_t> g_stream_d3dvb[kMaxStreams] = {};   // IDirect3DVertexBuffer9* (device)
std::atomic<std::uint32_t> g_stream_stride[kMaxStreams] = {};  // bytes per vertex
std::atomic<std::uint32_t> g_stream_offset[kMaxStreams] = {};  // byte offset applied at draw time
std::atomic<std::uint32_t> g_stream_fresh{0};   // streams rebound for the next draw
std::atomic<std::uint32_t> g_last_ib{0};
// RHIDrawIndexedPrimitive(IndexBuffer, PrimitiveType, BaseVertexIndex, MinIndex,
// NumVertices, StartIndex, NumPrimitives) - XeD3DCommands.cpp:855. MinIndex and
// NumVertices are the exact vertex range the section touches, which is what lets
// the native path upload precisely what a draw reads instead of scanning indices
// to find out.
std::atomic<std::uint32_t> g_last_min_index{0};
std::atomic<std::uint32_t> g_last_num_vertices{0};
std::atomic<std::uint32_t> g_bound_decl{0};
std::atomic<std::uint32_t> g_bound_vs{0};
std::atomic<std::uint32_t> g_bound_ps{0};
// Translated DXIL for whatever is bound right now (null until a shader cache
// has been built - see downpour_native_shaders.h).
std::atomic<std::uintptr_t> g_native_vs{0};
std::atomic<std::uintptr_t> g_native_ps{0};

bool DrawSurveyEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR_DRAWSURVEY");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

inline std::uint32_t GuestReadBE32(const std::uint8_t* base, std::uint32_t addr) {
  std::uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

// Survey helper: print `count` big-endian floats (and their raw words) from a
// guest address, so a vertex layout can be read straight out of the log.
[[maybe_unused]] void DumpFloatsBE(const std::uint8_t* base, std::uint32_t addr, int count,
                                   const char* tag) {
  if (base == nullptr || !GuestAddrPlausible(addr)) {
    REXLOG_INFO("[native-up] {} addr={:#x} (not mapped)", tag, addr);
    return;
  }
  char line[512];
  int n = std::snprintf(line, sizeof(line), "%s@%#x:", tag, addr);
  for (int i = 0; i < count && n > 0 && n < static_cast<int>(sizeof(line)) - 24; ++i) {
    const std::uint32_t bits = GuestReadBE32(base, addr + i * 4);
    float f;
    std::memcpy(&f, &bits, 4);
    n += std::snprintf(line + n, sizeof(line) - n, " %.4g", f);
  }
  REXLOG_INFO("[native-up] {}", line);
}

}  // namespace

// One log-only passthrough hook per resolved RHI function.
#define DPOUR_RHI_LOGHOOK(subsym, label, interval)           \
  REX_EXTERN(__imp__##subsym);                               \
  REX_HOOK_RAW(subsym) {                                     \
    if (HooksEnabled()) {                                    \
      static std::atomic<std::uint64_t> _cnt{0};             \
      CountAndLog(_cnt, label, (interval));                  \
    }                                                        \
    __imp__##subsym(ctx, base);                              \
  }

// --- frame boundaries --------------------------------------------------------
REX_EXTERN(__imp__sub_829D42F8);
REX_HOOK_RAW(sub_829D42F8) {
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHIBeginDrawingViewport", 60);
  }
  if (NativeActive()) {
    dpour_geo::BeginFrame();
    dpour_scene::BeginFrame();
    dpour_ue3::BeginFrame();
  }
  __imp__sub_829D42F8(ctx, base);
}
REX_EXTERN(__imp__sub_829D4458);
REX_HOOK_RAW(sub_829D4458) {
  if (NativeActive()) {
    dpour_geo::EndFrame();
    dpour_scene::EndFrame();
    dpour_ue3::EndFrame();
    dpour_shaders::MaybeHarvest(base);
    dpour_shadercache::ReportStats();
  }
  // One line every few hundred frames, only while the shader path is being
  // brought up: it is what confirms sub_82D1D868 really is the pixel-shader
  // constant setter, since a wrong guess would show zero writes.
  if (dpour_shadercache::Available()) {
    static std::atomic<std::uint64_t> frames{0};
    if ((frames.fetch_add(1, std::memory_order_relaxed) % 600) == 0) {
      dpour_consts::LogStats();
      if (dpour_pipeline::DryRunEnabled()) {
        dpour_pipeline::DryRunReport();
      }
      if (dpour_scene::Enabled()) {
        dpour_scene::LogStats();
      }
      dpour_ue3::LogStats();
    }
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHIEndDrawingViewport", 60);
  }
  __imp__sub_829D4458(ctx, base);
}

// --- transforms --------------------------------------------------------------
// RHISetViewParameters: r4 = &ViewProjectionMatrix (FMatrix, 16 big-endian floats).
REX_EXTERN(__imp__sub_829C94F0);
REX_HOOK_RAW(sub_829C94F0) {
  if (NativeActive()) {
    dpour_geo::SetViewProj(base, ctx.r4.u32);
    dpour_scene::NoteViewProj(base, ctx.r4.u32);
  }
  __imp__sub_829C94F0(ctx, base);
}

// Device SetVertexShaderConstantF(dev, StartRegister, pData, Vector4fCount).
// Shadowing it gives us the whole guest VS constant file - including
// LocalToWorld, which UE3 vertex factories need for object-space positions.
REX_EXTERN(__imp__sub_82D1D790);
REX_HOOK_RAW(sub_82D1D790) {
  if (NativeActive()) {
    dpour_geo::SetVSConstants(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    dpour_consts::SetVertexConstants(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_82D1D790(ctx, base);
}

// Device SetPixelShaderConstantF(dev, StartRegister, pData, Vector4fCount).
//
// Identified statically rather than guessed: the game's RHISetVertexShaderParameter
// and RHISetPixelShaderParameter have identical bodies apart from the device
// method (XeD3DCommands.cpp:400 and :518), and in the recompiled code
// sub_829C92B0 and sub_829C93D0 match instruction group for instruction group -
// same five helper calls at the same offsets - with sub_829C92B0 ending in the
// known SetVertexShaderConstantF and sub_829C93D0 ending here.
REX_EXTERN(__imp__sub_82D1D868);
REX_HOOK_RAW(sub_82D1D868) {
  if (NativeActive()) {
    dpour_consts::SetPixelConstants(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_82D1D868(ctx, base);
}

// Device SetSamplerState(dev, Sampler, D3DSAMPLERSTATETYPE, DWORD value).
// RHISetSamplerState pushes MAG/MIN/MIP filter and ADDRESSU/V/W through here
// per texture slot (XeD3DCommands.cpp:313-317), which is where the translated
// shaders' sampler indices come from.
REX_EXTERN(__imp__sub_82D19E60);
REX_HOOK_RAW(sub_82D19E60) {
  if (NativeActive()) {
    dpour_pipeline::SetGuestSamplerState(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_82D19E60(ctx, base);
}

// --- geometry state ----------------------------------------------------------
// RHISetStreamSource(StreamIndex, VertexBuffer, Stride, ...) - the RHI-level
// binding: which buffer and what stride.
REX_EXTERN(__imp__sub_829C8AA8);
REX_HOOK_RAW(sub_829C8AA8) {
  const std::uint32_t stream = ctx.r3.u32;
  if (NativeActive() && stream < kMaxStreams) {
    g_stream_vb[stream].store(ctx.r4.u32, std::memory_order_relaxed);
    g_stream_stride[stream].store(ctx.r5.u32, std::memory_order_relaxed);
  }
  __imp__sub_829C8AA8(ctx, base);
}

// Device SetStreamSource(dev, StreamNumber, pStreamData, OffsetInBytes, Stride).
// RHIDrawIndexedPrimitive re-binds every stream right before the draw with
// OffsetInBytes = Stride * BaseVertexIndex, so THIS is where the per-draw
// vertex offset lives (the device draw call always passes BaseVertexIndex = 0).
REX_EXTERN(__imp__sub_82D1A5C0);
REX_HOOK_RAW(sub_82D1A5C0) {
  const std::uint32_t stream = ctx.r4.u32;
  if (NativeActive() && stream < kMaxStreams) {
    g_stream_offset[stream].store(ctx.r6.u32, std::memory_order_relaxed);
    // The D3D vertex buffer and the stride, as the hardware itself receives them
    // immediately before the draw. This is the authoritative binding: the
    // RHI-level shadow above misses whatever the RHI rebinds behind its own
    // back, and a stream the shader needs but whose stride we recorded as zero
    // is a draw dropped - which was happening to 43372 of them.
    g_stream_d3dvb[stream].store(ctx.r5.u32, std::memory_order_relaxed);
    g_stream_stride[stream].store(ctx.r7.u32, std::memory_order_relaxed);
    // RHIDrawIndexedPrimitive re-binds exactly the streams the bound vertex
    // declaration uses, immediately before the draw. That makes this mask the
    // vertex layout of the draw about to happen - which is how we tell UE3's
    // position-only depth prepass (stream 0 alone, no textures bound) from the
    // base pass (stream 0 + the tangent/UV stream, with the material's
    // textures). Capturing the prepass instead of the base pass is why an
    // earlier build could only ever find lightmaps.
    g_stream_fresh.fetch_or(1u << stream, std::memory_order_relaxed);
  }
  __imp__sub_82D1A5C0(ctx, base);
}

// RHISetBoundShaderState: VertexDeclaration@+0, VertexShader@+8, PixelShader@+12.
REX_EXTERN(__imp__sub_829C8D80);
REX_HOOK_RAW(sub_829C8D80) {
  if (NativeActive() && GuestAddrPlausible(ctx.r3.u32)) {
    const std::uint32_t bss = ctx.r3.u32;
    g_bound_decl.store(GuestReadBE32(base, bss + 0), std::memory_order_relaxed);
    g_bound_vs.store(GuestReadBE32(base, bss + 8), std::memory_order_relaxed);
    g_bound_ps.store(GuestReadBE32(base, bss + 12), std::memory_order_relaxed);
    // The only place the game's real shaders can be captured: the container is
    // split apart at creation time, and only the bound clone carries microcode
    // patched for the actual vertex declaration.
    dpour_shaders::OnBoundShaderState(base, bss);

    // And the other direction: pair what was just bound with its translated
    // DXIL. The vertex shader to look up is the ORIGINAL (bss+8), not the
    // declaration-patched clone at +4.
    //
    // This matters and it is measurable. The clone's microcode is rewritten per
    // vertex declaration, so keying on it means one cache entry per
    // (shader x declaration) pair - a live run resolved 53 shaders and missed
    // 390. The original is one object per shader, and the declaration is
    // accounted for where it belongs on this path: in the host input layout and
    // in the PSO key, exactly as UnleashedRecomp does it.
    if (dpour_shadercache::Available()) {
      g_native_vs.store(reinterpret_cast<std::uintptr_t>(dpour_shadercache::ForGuestShader(
                            base, g_bound_vs.load(std::memory_order_relaxed))),
                        std::memory_order_relaxed);
      g_native_ps.store(reinterpret_cast<std::uintptr_t>(dpour_shadercache::ForGuestShader(
                            base, g_bound_ps.load(std::memory_order_relaxed))),
                        std::memory_order_relaxed);
    }
  }
  __imp__sub_829C8D80(ctx, base);
}

// FXeVertexShader::FXeVertexShader(this, const void* Code) = sub_829CFB50 and
// FXePixelShader::FXePixelShader = sub_829CFD30.
//
// Found from the live objects: every shader object starts with the vtable
// 0x821BB9B8 (vs) / 0x821BB9C0 (ps), and these are the only two functions that
// write those, set UsageFlags = RUF_Static at +16, null Resource/BaseAddress at
// +8/+12, and then read the container header out of r4.
//
// This is the ONLY moment the container exists in one piece. The D3D object the
// constructor builds does NOT keep the container header - checked against live
// memory, it starts with 00000006 00000001 ... - so the earlier plan of
// stitching the D3D object back together with the microcode could never work.
REX_EXTERN(__imp__sub_829CFB50);
REX_HOOK_RAW(sub_829CFB50) {
  const std::uint32_t shader_object = ctx.r3.u32;
  const std::uint32_t code = ctx.r4.u32;
  __imp__sub_829CFB50(ctx, base);
  if (NativeActive()) {
    dpour_shaders::OnShaderCreated(base, shader_object, code);
  }
}
REX_EXTERN(__imp__sub_829CFD30);
REX_HOOK_RAW(sub_829CFD30) {
  const std::uint32_t shader_object = ctx.r3.u32;
  const std::uint32_t code = ctx.r4.u32;
  __imp__sub_829CFD30(ctx, base);
  if (NativeActive()) {
    dpour_shaders::OnShaderCreated(base, shader_object, code);
  }
}

// RHICreateVertexDeclaration(FVertexDeclarationRHIRef* out, const
// FVertexDeclarationElementList& Elements) = sub_829D40F8.
//
// Identified from the game's own source: it zeroes *r3, builds an
// FVertexDeclarationKey on the stack from r4 (sub_829D3D28 - which reads
// StreamIndex at +0, Offset at +1 and switches on Type-1 over exactly the 12
// EVertexElementType cases), looks the key up in GVertexDeclarationCache and
// stores the resulting IDirect3DVertexDeclaration9 into *r3.
//
// This is the only moment the layout is visible: the D3DVERTEXELEMENT9 array is
// a stack temporary and the declaration object itself is driver-compiled, which
// is why scanning guest memory for it never found anything.
REX_EXTERN(__imp__sub_829D40F8);
REX_HOOK_RAW(sub_829D40F8) {
  const std::uint32_t out_ref = ctx.r3.u32;
  const std::uint32_t elements = ctx.r4.u32;
  __imp__sub_829D40F8(ctx, base);
  if (NativeActive() && GuestAddrPlausible(out_ref) && GuestAddrPlausible(elements)) {
    dpour_decl::OnCreateDeclaration(base, GuestReadBE32(base, out_ref), elements);
  }
}

// --- textures ----------------------------------------------------------------
// Device SetTexture(dev, Sampler, D3DBaseTexture*) = sub_82D1D180. EVERY texture
// binding funnels through here - RHISetSamplerState (sub_829C8E68) is only one
// of its callers - and r5 is already the D3DBaseTexture whose 24-byte header IS
// the Xenos fetch constant, so this is both the complete and the cheapest place
// to watch.
REX_EXTERN(__imp__sub_82D1D180);
REX_HOOK_RAW(sub_82D1D180) {
  if (NativeActive() && ctx.r4.u32 < 16u) {
    dpour_tex::SetSampler(ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82D1D180(ctx, base);
}

// --- draws -------------------------------------------------------------------
// Device DrawIndexedVertices(dev, PrimitiveType, BaseVertexIndex, StartIndex,
// IndexCount). The RHI wrapper has already resolved the exact index count from
// the primitive-type table here, so this is where geometry capture belongs.
namespace {

// Fills in everything the native scene renderer needs that is common to both
// draw kinds. Kept here rather than in the renderer because this is where the
// shadowed guest state lives.
void FillCommonDrawInputs(dpour_scene::DrawDesc& d, std::uint32_t prim_type) {
  d.vs = reinterpret_cast<const dpour_shadercache::Shader*>(
      g_native_vs.load(std::memory_order_relaxed));
  d.ps = reinterpret_cast<const dpour_shadercache::Shader*>(
      g_native_ps.load(std::memory_order_relaxed));
  // A draw with no bound pixel shader is UE3's position-only depth prepass, and
  // is reproduced as a depth-only pipeline rather than skipped - it is what
  // fills our depth buffer with the same occlusion the guest sees.
  if (g_bound_ps.load(std::memory_order_relaxed) == 0) {
    d.ps = nullptr;
  }
  d.decl_guest = g_bound_decl.load(std::memory_order_relaxed);
  d.prim_type = prim_type;
  for (std::uint32_t s = 0; s < kMaxStreams && s < dpour_scene::kMaxStreams; ++s) {
    d.vb_guest[s] = g_stream_vb[s].load(std::memory_order_relaxed);
    d.vb_d3d[s] = g_stream_d3dvb[s].load(std::memory_order_relaxed);
    d.stride[s] = g_stream_stride[s].load(std::memory_order_relaxed);
    // RHIDrawIndexedPrimitive re-binds every stream with
    // OffsetInBytes = Stride * BaseVertexIndex just before the draw, so the
    // per-draw vertex offset is here and the device call always passes 0.
    d.stream_offset[s] = g_stream_offset[s].load(std::memory_order_relaxed);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82D1F0B0);
REX_HOOK_RAW(sub_82D1F0B0) {
  if (!NativeActive()) {
    __imp__sub_82D1F0B0(ctx, base);
    return;
  }
  const std::uint32_t streams = g_stream_fresh.exchange(0, std::memory_order_relaxed);
  bool replaced = false;

  // The draw path: the game's own translated shaders, its own vertex layout, its
  // own constants and its own pipeline state, drawn by us. Every primitive type
  // and every pass goes through here - what to keep is decided inside, by which
  // render target the pass belongs to.
  bool scene_took_it = false;
  if (dpour_scene::Enabled()) {
    dpour_scene::DrawDesc d;
    FillCommonDrawInputs(d, ctx.r4.u32);
    d.indexed = true;
    d.ib_guest = g_last_ib.load(std::memory_order_relaxed);
    d.start_index = ctx.r6.u32;
    d.index_count = ctx.r7.u32;
    d.min_index = g_last_min_index.load(std::memory_order_relaxed);
    d.num_vertices = g_last_num_vertices.load(std::memory_order_relaxed);
    scene_took_it = dpour_scene::CaptureDraw(base, d);
  }
  // UE3 draws every opaque mesh TWICE: once in the position-only depth prepass
  // (which binds NO pixel shader and no material textures) and once in the base
  // pass. Capturing the prepass copy is why the scene could only ever come out
  // untextured - the base colour simply is not bound yet at that point.
  const bool shaded_pass = g_bound_ps.load(std::memory_order_relaxed) != 0;
  if (ctx.r4.u32 == 4 && shaded_pass) {  // D3DPT_TRIANGLELIST
    // Survey: what actually distinguishes UE3's passes here? Log the bound
    // shader pair, the streams rebound for this draw and the texture we would
    // pick, for a handful of draws once the world is up.
    // Survey only REAL world geometry (big indexed draws), which is where the
    // base-colour question actually matters.
    if (DrawSurveyEnabled() && ctx.r7.u32 >= 600 &&
        g_stream_stride[0].load(std::memory_order_relaxed) == 12) {
      static std::atomic<std::uint64_t> big_seen{0};
      static std::atomic<std::uint32_t> surveyed{0};
      const std::uint64_t nb = big_seen.fetch_add(1) + 1;
      const std::uint32_t s = (nb > 60000 && (nb % 4001u) == 0)
                                  ? surveyed.fetch_add(1, std::memory_order_relaxed)
                                  : 99u;
      if (s < 5) {
        REXLOG_INFO(
            "[native-draw] #{} decl={:#x} vs={:#x} ps={:#x} streams={:#x} s0stride={} "
            "s1stride={} idx={}",
            s, g_bound_decl.load(std::memory_order_relaxed),
            g_bound_vs.load(std::memory_order_relaxed), g_bound_ps.load(std::memory_order_relaxed),
            streams, g_stream_stride[0].load(std::memory_order_relaxed),
            g_stream_stride[1].load(std::memory_order_relaxed), ctx.r7.u32);
        dpour_tex::SurveyDraw(base, s);
      }
    }
    // Dry run: build the input layout and the pipeline the game's own shaders
    // would need, then throw them away. Proves the translated DXIL, the root
    // signature and the vertex declaration all agree before anything is drawn.
    if (dpour_pipeline::DryRunEnabled()) {
      const auto* nvs = reinterpret_cast<const dpour_shadercache::Shader*>(
          g_native_vs.load(std::memory_order_relaxed));
      const auto* nps = reinterpret_cast<const dpour_shadercache::Shader*>(
          g_native_ps.load(std::memory_order_relaxed));
      std::uint16_t strides[dpour_pipeline::kMaxStreams]{};
      for (std::uint32_t s = 0; s < kMaxStreams && s < dpour_pipeline::kMaxStreams; ++s) {
        strides[s] = static_cast<std::uint16_t>(g_stream_stride[s].load(std::memory_order_relaxed));
      }
      dpour_pipeline::DryRunDraw(
          dpour_decl::Resolve(base, g_bound_decl.load(std::memory_order_relaxed)), nvs, nps,
          strides, dpour_pipeline::kMaxStreams);
    }

    dpour_geo::DrawInputs in;
    in.ib_guest = g_last_ib.load(std::memory_order_relaxed);
    // The Phase 3 geometry renderer still only understands four streams, so it
    // is fed exactly four - the shadow being wider must not overrun its array.
    for (std::uint32_t s = 0; s < kMaxStreams && s < dpour_geo::kMaxStreams; ++s) {
      in.vb_guest[s] = g_stream_vb[s].load(std::memory_order_relaxed);
      in.stride[s] = g_stream_stride[s].load(std::memory_order_relaxed);
    }
    in.decl_guest = g_bound_decl.load(std::memory_order_relaxed);
    // Stream byte offset / stride == the base vertex the guest applied.
    const std::uint32_t off0 = g_stream_offset[0].load(std::memory_order_relaxed);
    in.base_vertex = (in.stride[0] != 0) ? (off0 / in.stride[0]) : 0u;
    in.start_index = ctx.r6.u32;
    in.index_count = ctx.r7.u32;
    replaced = dpour_geo::CaptureDraw(base, in);
  }
  dpour_tex::NextDraw();
  // Only drop the guest's submission for draws we actually replaced. Skipping
  // everything (as an earlier build did) also threw away the menus, the HUD,
  // the movies and every pass we do not reproduce - which is why the frame came
  // back as garbage instead of "no Xenia".
  if ((replaced && dpour_geo::SkipGuestDraws()) ||
      (scene_took_it && dpour_scene::ShouldSkipGuestDraw())) {
    return;
  }
  __imp__sub_82D1F0B0(ctx, base);
}

// Device DrawVertices(dev, PrimitiveType, StartVertex, VertexCount) - the
// non-indexed path (UI / particles). Only gated, not captured yet.
REX_EXTERN(__imp__sub_82D1EC98);
REX_HOOK_RAW(sub_82D1EC98) {
  if (!NativeActive()) {
    __imp__sub_82D1EC98(ctx, base);
    return;
  }
  bool scene_took_it = false;
  if (dpour_scene::Enabled()) {
    dpour_scene::DrawDesc d;
    FillCommonDrawInputs(d, ctx.r4.u32);
    d.indexed = false;
    d.start_vertex = ctx.r5.u32;
    d.vertex_count = ctx.r6.u32;
    scene_took_it = dpour_scene::CaptureDraw(base, d);
  }
  dpour_tex::NextDraw();
  if (scene_took_it && dpour_scene::ShouldSkipGuestDraw()) {
    return;
  }
  __imp__sub_82D1EC98(ctx, base);
}

// --- user-pointer draws (UE3 Canvas / SimpleElement = the whole 2D UI) -------
//
// Resolved in the recompiled code (downpour_recomp.84.cpp / .57.cpp):
//
//   sub_82D1E250 = D3DDevice::BeginVertices(dev, d3dprim, VertexCount, Stride)
//                  -> returns the WRITE POINTER in r3 (0 = command buffer full).
//                  It multiplies r5*r6 for the byte size and stores stride>>2
//                  as a byte at dev+12904 - r6 is the stride, not a pointer.
//   sub_82D1E758 = D3DDevice::BeginIndexedVertices(dev, d3dprim,
//                  BaseVertexOffset (MinVertexIndex, already negated by the
//                  RHI), NumVertices, NumIndices, IndexFmt (bit 2 set = 32-bit),
//                  VertexStride, &OutIndexData, [r1+84] = &OutVertexData)
//                  -> returns 0 on success, out pointers filled.
//
// "EndVertices" exists as no function: every call site publishes inline with a
// bare [dev+13844] -> dev+48 store (RHIDrawPrimitiveUP does it, DrawVerticesUP
// sub_82D1E710 does it, and half a dozen engine-inlined UP sites do it). So
// capture opens a PENDING draw here at Begin - state is complete, Begin itself
// commits it guest-side - and the scene closes it (copies the bytes) at the
// next captured event, long after the game's own End ran. Hooking the DEVICE
// entries rather than the RHI ones is what catches the inlined copies at all.
REX_EXTERN(__imp__sub_82D1E250);
REX_HOOK_RAW(sub_82D1E250) {
  const std::uint32_t prim = ctx.r4.u32;
  const std::uint32_t count = ctx.r5.u32;
  const std::uint32_t stride = ctx.r6.u32;
  __imp__sub_82D1E250(ctx, base);
  // Stand down when the capture runs at the RHI entry above: that entry calls
  // straight into here, so capturing at both records every UP draw twice.
  if (NativeActive() && dpour_scene::Enabled() && !dpour_scene::UpAtApiLevel() &&
      ctx.r3.u32 != 0) {
    dpour_scene::UPDraw u;
    FillCommonDrawInputs(u.d, prim);
    u.d.indexed = false;
    u.vtx_guest = ctx.r3.u32;
    u.vtx_count = count;
    u.vtx_stride = stride;
    dpour_scene::CaptureUPBegin(base, u);
    dpour_tex::NextDraw();
  }
}
REX_EXTERN(__imp__sub_82D1E758);
REX_HOOK_RAW(sub_82D1E758) {
  const std::uint32_t prim = ctx.r4.u32;
  const std::int32_t base_vertex = static_cast<std::int32_t>(ctx.r5.u32);
  const std::uint32_t num_vertices = ctx.r6.u32;
  const std::uint32_t num_indices = ctx.r7.u32;
  const std::uint32_t index_fmt = ctx.r8.u32;
  const std::uint32_t vtx_stride = ctx.r9.u32;
  const std::uint32_t out_idx_ref = ctx.r10.u32;
  // Ninth argument, on the guest stack. Both callers in the binary
  // (RHIDrawIndexedPrimitiveUP and the occlusion-cube batch) place it at
  // [r1+84], which is where the Xenon ABI puts it.
  const std::uint32_t out_vtx_ref = GuestReadBE32(base, ctx.r1.u32 + 84);
  __imp__sub_82D1E758(ctx, base);
  // Same stand-down as the non-indexed entry above.
  if (NativeActive() && dpour_scene::Enabled() && !dpour_scene::UpAtApiLevel() &&
      ctx.r3.u32 == 0 && GuestAddrPlausible(out_idx_ref) && GuestAddrPlausible(out_vtx_ref)) {
    dpour_scene::UPDraw u;
    FillCommonDrawInputs(u.d, prim);
    u.d.indexed = true;
    u.vtx_guest = GuestReadBE32(base, out_vtx_ref);
    u.vtx_count = num_vertices;
    u.vtx_stride = vtx_stride;
    u.idx_guest = GuestReadBE32(base, out_idx_ref);
    u.idx_count = num_indices;
    u.idx_32bit = (index_fmt & 4u) != 0;
    u.base_vertex = base_vertex;
    if (GuestAddrPlausible(u.vtx_guest) && GuestAddrPlausible(u.idx_guest)) {
      dpour_scene::CaptureUPBegin(base, u);
      dpour_tex::NextDraw();
    }
  }
}

// --- user-pointer draws, AT THE ENTRY THE REFERENCES HOOK ---------------------
//
// UnleashedRecomp hooks DrawPrimitiveUP (video.cpp:7840) and MarathonRecomp does
// the same; neither goes anywhere near BeginVertices or the command ring. We had
// been hooking one level below, at the device, and that is where "the UP draws
// cannot be suppressed" came from - a blocker of our own making, since Begin IS
// the allocation and declining it hands the game a null pointer.
//
// Here the data belongs to the CALLER and is already written, so the capture
// copies it on the spot and, when we own the device, the call simply does not
// go through: no BeginVertices, no allocation, no packets.
//
//   RHIDrawPrimitiveUP(PrimType, NumPrimitives, VertexData, Stride)
//     = sub_829CA900   (XeD3DCommands.cpp:1146)
//   RHIDrawIndexedPrimitiveUP(PrimType, MinVertexIndex, NumVertices,
//                             NumPrimitives, IndexData, IndexStride,
//                             VertexData, VertexStride)
//     = sub_829CAE50   (XeD3DCommands.cpp:1269)

// UE3's EPrimitiveType -> the Xenon D3DPRIMITIVETYPE, from the game's own
// GetD3DPrimitiveType (XeD3DCommands.cpp:803). Note this is the CONSOLE's
// enumeration, where TRIANGLESTRIP is 6 and QUADLIST is 13 - not the PC one.
static std::uint32_t D3DPrimFromUE3(std::uint32_t ue3) {
  switch (ue3) {
    case 0: return 4;   // PT_TriangleList  -> D3DPT_TRIANGLELIST
    case 1: return 6;   // PT_TriangleStrip -> D3DPT_TRIANGLESTRIP
    case 2: return 2;   // PT_LineList      -> D3DPT_LINELIST
    case 3: return 13;  // PT_QuadList      -> D3DPT_QUADLIST
    default: return 0;  // tessellated patches and anything else: not ours
  }
}

// GetPrimitiveTypeCount (XeD3DCommands.cpp:1022), verbatim.
static std::uint32_t VertexCountFromPrims(std::uint32_t ue3, std::uint32_t prims) {
  switch (ue3) {
    case 0: return prims * 3;
    case 1: return prims + 2;
    case 2: return prims * 2;
    case 3: return prims * 4;
    default: return 0;
  }
}

REX_EXTERN(__imp__sub_829CA900);
REX_HOOK_RAW(sub_829CA900) {
  const std::uint32_t ue3_prim = ctx.r3.u32;
  const std::uint32_t num_prims = ctx.r4.u32;
  const std::uint32_t vtx_guest = ctx.r5.u32;
  const std::uint32_t stride = ctx.r6.u32;
  if (NativeActive() && dpour_scene::Enabled() && dpour_scene::UpAtApiLevel()) {
    const std::uint32_t d3d_prim = D3DPrimFromUE3(ue3_prim);
    const std::uint32_t vtx_count = VertexCountFromPrims(ue3_prim, num_prims);
    if (d3d_prim != 0 && vtx_count != 0 && GuestAddrPlausible(vtx_guest)) {
      dpour_scene::UPDraw u;
      FillCommonDrawInputs(u.d, d3d_prim);
      u.d.indexed = false;
      u.vtx_guest = vtx_guest;
      u.vtx_count = vtx_count;
      u.vtx_stride = stride;
      dpour_scene::CaptureUPBegin(base, u);
      dpour_scene::CaptureUPEnd();  // the caller's buffer is full NOW
      dpour_tex::NextDraw();
    }
    if (dpour_scene::OwnDevice()) {
      return;  // never reaches BeginVertices, so nothing is allocated or queued
    }
  }
  __imp__sub_829CA900(ctx, base);
}

REX_EXTERN(__imp__sub_829CAE50);
REX_HOOK_RAW(sub_829CAE50) {
  const std::uint32_t ue3_prim = ctx.r3.u32;
  const std::uint32_t min_vtx = ctx.r4.u32;
  const std::uint32_t num_vertices = ctx.r5.u32;
  const std::uint32_t num_prims = ctx.r6.u32;
  const std::uint32_t idx_guest = ctx.r7.u32;
  const std::uint32_t idx_stride = ctx.r8.u32;
  const std::uint32_t vtx_guest = ctx.r9.u32;
  const std::uint32_t vtx_stride = ctx.r10.u32;
  if (NativeActive() && dpour_scene::Enabled() && dpour_scene::UpAtApiLevel()) {
    const std::uint32_t d3d_prim = D3DPrimFromUE3(ue3_prim);
    const std::uint32_t idx_count = VertexCountFromPrims(ue3_prim, num_prims);
    if (d3d_prim != 0 && idx_count != 0 && num_vertices != 0 &&
        GuestAddrPlausible(vtx_guest) && GuestAddrPlausible(idx_guest)) {
      dpour_scene::UPDraw u;
      FillCommonDrawInputs(u.d, d3d_prim);
      u.d.indexed = true;
      u.vtx_guest = vtx_guest;
      u.vtx_count = num_vertices;
      u.vtx_stride = vtx_stride;
      u.idx_guest = idx_guest;
      u.idx_count = idx_count;
      u.idx_32bit = (idx_stride == 4);
      // The RHI negates MinVertexIndex before handing it to the device, because
      // the device rebases the indices onto the copied vertex range. We copy the
      // caller's vertices from index 0, so the rebase is ours to apply and it
      // has the same sign the device sees.
      u.base_vertex = -static_cast<std::int32_t>(min_vtx);
      dpour_scene::CaptureUPBegin(base, u);
      dpour_scene::CaptureUPEnd();
      dpour_tex::NextDraw();
    }
    if (dpour_scene::OwnDevice()) {
      return;
    }
  }
  __imp__sub_829CAE50(ctx, base);
}

// --- resource lifetime: the DestructResource of this game ---------------------
//
// UnleashedRecomp hooks DestructResource (video.cpp:7800) and defers the
// destruction until the GPU has finished the frame (ProcDestructResource ->
// g_tempResources). Downpour's equivalent choke point is AddUnusedXeResource
// (XeD3DResources.cpp:118): EVERY resource the game retires - dynamic
// vertex-buffer orphans, destructed textures, freed surfaces - passes through
// it, carrying the allocation's base address, and DeleteUnusedXeResources frees
// the queue one frame later, after which the address is reused.
//
// Resolved by content, not guess:
//   sub_829CF628 = AddUnusedXeResource(r3 = IDirect3DResource9* or null,
//                  r4 = BaseAddress, r5 = SecondaryAddress, ...) - appends to
//                  the double-buffered queue at 0x837CCFC0 (base +/- frame*12)
//                  and adds the size to the counter at 0x837AB344. Six callers,
//                  all inside the RHI module.
//   sub_829CF7A8 = DeleteUnusedXeResources - walks the PREVIOUS frame's queue
//                  (BlockUntilNotBusy per entry) and swaps the buffers; called
//                  from RHIEndDrawingViewport right before XePerformSwap, which
//                  is exactly where the reference's DestructTempResources sits
//                  in its own frame.
//
// The hook invalidates our caches AT RETIREMENT TIME - one frame before the
// address can be reused - so a draw captured after the free can never be served
// bytes that now belong to a different resource. This closes, at the reference's
// own choke point, the address-reuse family of bugs: the AuxColor 128x128 that
// answered for a later 1024x720, and the dynamic orphans that made "buffer size
// unknown" the single largest draw loss (22900 in one run).
REX_EXTERN(__imp__sub_829CF628);
REX_HOOK_RAW(sub_829CF628) {
  // r4 = BaseAddress. r5 is a BOOL, not an address - the vertex-buffer
  // allocator's own call site reads `li r5,1` with r3=0 and the base in r4.
  if (NativeActive() && ctx.r4.u32 != 0) {
    dpour_vbuf::InvalidateGuestAddress(ctx.r4.u32);
    dpour_tex::InvalidateGuestAddress(ctx.r4.u32);
  }
  __imp__sub_829CF628(ctx, base);
}

// RHISetRenderTarget(NewRenderTarget, NewDepthStencilTarget) - XeD3DCommands.cpp:668.
// A frame draws into several surfaces (shadow depth, reflections, the scene,
// then the post-process chain), and this is the only thing that tells them
// apart, so the native path groups its draws by the pair bound here.
REX_EXTERN(__imp__sub_829C9BB0);
REX_HOOK_RAW(sub_829C9BB0) {
  if (NativeActive()) {
    // First dword of the FSurfaceRHIRef = the raw surface object; that is what
    // the scene marker matches (see dpour_scene::SetRenderTarget).
    std::uint32_t color_object = 0;
    std::uint32_t depth_object = 0;
    const std::uint32_t cref = ctx.r3.u32;
    const std::uint32_t dref = ctx.r4.u32;
    std::uint32_t v;
    if (cref >= 0x10000u && cref < 0xC0000000u) {
      std::memcpy(&v, base + cref, 4);
      color_object = _byteswap_ulong(v);
    }
    if (dref >= 0x10000u && dref < 0xC0000000u) {
      std::memcpy(&v, base + dref, 4);
      depth_object = _byteswap_ulong(v);
    }
    // GD3DBackBuffer (guest 0x83790044) holds the raw back-buffer surface. The
    // game binds it through ordinary refs - RHIBeginDrawingViewport's tail, UI
    // code, post-processing - so the ref pointer identifies nothing; only the
    // dereferenced object does. This comparison is what finds the game's own
    // final composition pass (tonemap quads + UI), the pass worth publishing.
    const std::uint32_t bb_object = GuestReadBE32(base, 0x83790044u);
    const bool is_backbuffer =
        (bb_object != 0 && color_object == bb_object) || (cref == 0 && dref == 0);
    dpour_scene::SetRenderTarget(ctx.r3.u32, ctx.r4.u32, color_object, depth_object,
                                 is_backbuffer);
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHISetRenderTarget", 500);
  }
  __imp__sub_829C9BB0(ctx, base);
}

// --- the engine's own mesh stream (the data-driven layer) -------------------
// FMeshDrawingPolicy::DrawMesh(const FMeshElement& Mesh) = sub_828194E8, r4 = &Mesh.
//
// THE choke point: it is the only caller of RHIDrawIndexedPrimitive in the
// binary, so every mesh the renderer submits passes through here exactly once,
// carrying what the D3D layer can never recover - the transform, the material,
// the depth priority group, whether it casts shadows. This is the same place
// skate3recomp's renderer taps its engine, and the structure behind r4 is
// documented in the game's own Scene.h rather than reverse-engineered.
//
// Runs BEFORE the original so the stream is in submission order even if the
// engine's own draw is later suppressed.
REX_EXTERN(__imp__sub_828194E8);
REX_HOOK_RAW(sub_828194E8) {
  if (NativeActive() && dpour_ue3::Enabled()) {
    dpour_ue3::OnDrawMesh(base, ctx.r4.u32);
  }
  __imp__sub_828194E8(ctx, base);
}

// D3DDevice::SetRenderTarget(dev, RenderTargetIndex, pRenderTarget) =
// sub_82D1A7F8 and SetDepthStencilSurface(dev, pNewZStencil) = sub_82D1AB88.
//
// THE complete view of which surface a draw lands on. RHISetRenderTarget is
// only one of the callers: RHIEndDrawingViewport binds the back buffer straight
// through the device (XeD3DViewport.cpp:95), the MSAA block binds the scene's
// EDRAM surface through SetMainpassRenderTargets, and the post chain does the
// same. Anything bound that way was invisible to the pass tracker and its draws
// were charged to whatever the RHI had bound last - which is why the back
// buffer, the surface XePerformSwap resolves to the front buffer and therefore
// the frame itself, appeared to receive two draws a frame.
//
// r5 is already the surface OBJECT here, not an FSurfaceRHIRef, so it needs no
// dereference and it matches what the create hook registered.
REX_EXTERN(__imp__sub_82D1A7F8);
REX_HOOK_RAW(sub_82D1A7F8) {
  if (NativeActive() && dpour_scene::DeviceRtEnabled()) {
    const std::uint32_t bb_object = GuestReadBE32(base, 0x83790044u);
    dpour_scene::DeviceSetRenderTarget(ctx.r4.u32, ctx.r5.u32,
                                       bb_object != 0 && ctx.r5.u32 == bb_object);
  }
  __imp__sub_82D1A7F8(ctx, base);
}
REX_EXTERN(__imp__sub_82D1AB88);
REX_HOOK_RAW(sub_82D1AB88) {
  if (NativeActive() && dpour_scene::DeviceRtEnabled()) {
    dpour_scene::DeviceSetDepthStencil(ctx.r4.u32);
  }
  __imp__sub_82D1AB88(ctx, base);
}

// RHISetViewport(UINT MinX, UINT MinY, FLOAT MinZ, UINT MaxX, UINT MaxY,
// FLOAT MaxZ) - XeD3DCommands.cpp:149. The floats travel in f1/f2 and the
// integers in r3-r6, which is what sizes the native target: rendering at the
// guest's own resolution is what makes the viewport, the scissor and the
// shaders' half-pixel offset exactly the numbers the game set.
REX_EXTERN(__imp__sub_829C8BD8);
REX_HOOK_RAW(sub_829C8BD8) {
  if (NativeActive()) {
    dpour_scene::SetViewport(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHISetViewport", 500);
  }
  __imp__sub_829C8BD8(ctx, base);
}

DPOUR_RHI_LOGHOOK(sub_829CE888, "RHICopyFromResolveTarget", 500)

// RHIClear - XeD3DCommands.cpp:1410, the API entry UnleashedRecomp hooks as
// `Clear` (video.cpp:7831). Under own-device the guest's clear is dropped: it
// clears EDRAM for a frame nobody assembles, and our own targets are cleared by
// the replay, per target and per replay.
//
// The device-level Clear (sub_82D27328) is hooked as well and does the same, but
// only as a backstop for clears that reach the device without passing through
// here - the API entry is the one that matches the references.
REX_EXTERN(__imp__sub_829CAF60);
REX_HOOK_RAW(sub_829CAF60) {
  // Captured as a stream item in BOTH modes - the reference treats Clear as a
  // command (RenderCommandType::Clear), not as something to skip. Register
  // layout verified against the recomp: r3=bClearColor (-> mask 15),
  // r4=&FLinearColor, r5=bClearDepth (-> |16), r7=bClearStencil (-> |32),
  // Depth in f1 (the GPR slot is reserved, so r6 is skipped), and the GInvertZ
  // flip happens INSIDE this function - so f1 here is the engine-side value.
  if (NativeActive() && dpour_scene::Enabled()) {
    dpour_scene::OnClear(base, ctx.r3.u32 != 0, ctx.r4.u32, ctx.r5.u32 != 0,
                         static_cast<float>(ctx.f1.f64));
  }
  if (NativeActive() && dpour_scene::Enabled() && dpour_scene::OwnDevice()) {
    static std::atomic<std::uint64_t> dropped{0};
    const std::uint64_t n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 1000) {
      REXLOG_INFO("[native-draw] own-device: RHIClear captured, guest submission dropped x{}", n);
    }
    return;
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHIClear", 500);
  }
  __imp__sub_829CAF60(ctx, base);
}

// D3DDevice::Clear = sub_82D27328 - a PACKET WRITER (it reaches the command
// buffer through sub_82D26BE8; see the classification in
// reference_downpour_device_api_classification), and group 2 of the own-device
// migration after the draws themselves.
//
// Under DPOUR_NR_OWN_DEVICE the guest's clear is dropped: it clears EDRAM for an
// emulated frame that, in that mode, nobody assembles or shows. Our own targets
// are cleared by the replay itself, per target and per replay, so nothing here
// is needed to make our image whole.
//
// Outside that mode this hook is entirely pass-through - the emulated frame is
// still the frame, and clearing is part of drawing it.
REX_EXTERN(__imp__sub_82D27328);
REX_HOOK_RAW(sub_82D27328) {
  if (NativeActive() && dpour_scene::Enabled() && dpour_scene::OwnDevice()) {
    static std::atomic<std::uint64_t> dropped{0};
    const std::uint64_t n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 1000 || n == 100000) {
      REXLOG_INFO("[native-draw] own-device: guest Clear dropped x{}", n);
    }
    return;
  }
  __imp__sub_82D27328(ctx, base);
}

// RHICopyToResolveTarget(SourceSurface, bKeepOriginalSurface, &ResolveParams) -
// XeD3DRenderTarget.cpp:397. When the source is the scene colour surface and
// injection is on, dpour_scene writes the NATIVE image into the resolve texture
// and the original is skipped entirely - both the FPS saving and the guarantee
// that the emulated resolve cannot overwrite the injected bytes.
REX_EXTERN(__imp__sub_829CDDC0);
REX_HOOK_RAW(sub_829CDDC0) {
  bool replaced = false;
  if (NativeActive()) {
    std::uint32_t surface_object = 0;
    const std::uint32_t ref = ctx.r3.u32;
    if (ref >= 0x10000u && ref < 0xC0000000u) {
      std::uint32_t v;
      std::memcpy(&v, base + ref, 4);
      surface_object = _byteswap_ulong(v);
    }
    // r5 = const FResolveParams& - the explicit destination override, which the
    // RHI consults BEFORE the surface's own resolve texture.
    replaced = dpour_scene::OnResolveScene(base, ref, surface_object, ctx.r5.u32);
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHICopyToResolveTarget", 2000);
  }
  if (!replaced) {
    __imp__sub_829CDDC0(ctx, base);
  }
}

// The MSAA block brackets (sub_829CB2F8 / sub_829CB568) USED TO BE HOOKED HERE.
// They are gone, and the reason is in the game's own source rather than in a
// measurement: Downpour ships with GUseTilingCode = FALSE, and the engineers
// wrote why next to it (XeD3DDevice.cpp:115-118 - "MSAA at 720p is disabled by
// default, as the quality improvements from allowing normal shadows on the
// dominant light in the base pass are better than 2xMSAA"). With tiling off,
// RHIMSAABeginRendering and RHIMSAAEndRendering are never called at all.
//
// So these hooks never fired, and the scene-block machinery behind them - a
// bracket that was supposed to say "everything drawn between these IS the
// scene" - never ran either. It cost us real time anyway: the pass diversion it
// once performed was one of the three roots found on 26.07, and it stayed in
// the file afterwards as something to reason about. Neither reference has any
// equivalent, because neither reference asks which draws are "the scene".
// RHICreateTargetableSurface(SizeX, SizeY, Format, ResolveTargetTexture,
// Flags, UsageStr) - but the FSurfaceRHIRef return travels through a hidden
// sret pointer (the recomp starts with `mr r22,r3`), so on entry r3 is the
// result slot and the usage string sits one register later than the source
// signature suggests: r9 (r8 kept as a logged fallback). Downpour ships with
// GUseTilingCode=FALSE, which means the scene is identified by binding the
// surface created here under the name "SceneColor" - not by the MSAA block.
REX_EXTERN(__imp__sub_829CD600);
REX_HOOK_RAW(sub_829CD600) {
  const std::uint32_t sret = ctx.r3.u32;
  const std::uint32_t size_x = ctx.r4.u32;
  const std::uint32_t size_y = ctx.r5.u32;
  // BYTE Format - UE3's EPixelFormat (EngineTextureClasses.h:153), the register
  // right after SizeY in RHICreateTargetableSurface(SizeX, SizeY, Format,
  // ResolveTargetTexture, Flags, UsageStr) with the sret shift.
  const std::uint32_t pixel_format = ctx.r6.u32;
  const std::uint32_t usage_r9 = ctx.r9.u32;
  const std::uint32_t usage_r8 = ctx.r8.u32;
  const std::uint32_t resolve_tex_r7 = ctx.r7.u32;  // ResolveTargetTexture
  __imp__sub_829CD600(ctx, base);
  if (NativeActive()) {
    dpour_scene::OnTargetableSurfaceCreated(base, sret, usage_r9, usage_r8, resolve_tex_r7,
                                            size_x, size_y, pixel_format);
  }
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    CountAndLog(_cnt, "RHICreateTargetableSurface", 20);
  }
}
DPOUR_RHI_LOGHOOK(sub_829CA050, "RHIDrawPrimitive", 20000)

// RHIDrawIndexedPrimitive(IndexBuffer, PrimitiveType, BaseVertexIndex, MinIndex,
// NumVertices, StartIndex, NumPrimitives): latch the index buffer; the capture
// itself happens in the device draw hook above, which carries the exact count.
REX_EXTERN(__imp__sub_829CA0E8);
REX_HOOK_RAW(sub_829CA0E8) {
  if (!NativeActive()) {
    __imp__sub_829CA0E8(ctx, base);
    return;
  }
  g_last_ib.store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_min_index.store(ctx.r6.u32, std::memory_order_relaxed);
  g_last_num_vertices.store(ctx.r7.u32, std::memory_order_relaxed);
  if (HooksEnabled()) {
    static std::atomic<std::uint64_t> _cnt{0};
    const std::uint64_t n = _cnt.fetch_add(1) + 1;
    if (n <= 5 || (n % 20000) == 0) {
      REXLOG_INFO(
          "[native-hook] RHIDrawIndexedPrimitive #{} prim={} baseVtx={} minIdx={} ib={:#x} | "
          "s0 vb={:#x} stride={} off={} | s1 vb={:#x} stride={} off={}",
          n, ctx.r4.u32, static_cast<std::int32_t>(ctx.r5.u32), ctx.r6.u32, ctx.r3.u32,
          g_stream_vb[0].load(std::memory_order_relaxed),
          g_stream_stride[0].load(std::memory_order_relaxed),
          g_stream_offset[0].load(std::memory_order_relaxed),
          g_stream_vb[1].load(std::memory_order_relaxed),
          g_stream_stride[1].load(std::memory_order_relaxed),
          g_stream_offset[1].load(std::memory_order_relaxed));
      if (n <= 5) {
        REXLOG_INFO("[native-hook]   -> bound decl={:#x} vs={:#x} ps={:#x} tex0={:#x}",
                    g_bound_decl.load(std::memory_order_relaxed),
                    g_bound_vs.load(std::memory_order_relaxed),
                    g_bound_ps.load(std::memory_order_relaxed),
                    dpour_tex::BoundBaseTexture(base));
      }
    }
  }
  __imp__sub_829CA0E8(ctx, base);
}

// === END DPOUR MIGRATION 2026-07-25 ===
