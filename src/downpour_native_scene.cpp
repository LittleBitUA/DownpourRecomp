// downpour - Native Render: the game's own draws, on our own D3D12 pipeline
//
// === DPOUR MIGRATION 2026-07-25: draw path ===
// See downpour_native_scene.h for the shape of this and why capture resolves
// everything before the render thread ever sees a draw.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_scene.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <rex/logging.h>

#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/native_rhi_d3d12.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

#include "downpour_native_constants.h"
#include "downpour_native_decl.h"
#include "downpour_native_pipeline.h"
#include "downpour_native_shaders.h"
#include "downpour_native_state.h"
#include "downpour_native_tex.h"
#include "downpour_native_ue3.h"  // the engine's own per-mesh verdict
#include "downpour_native_vbuffers.h"

using Microsoft::WRL::ComPtr;

namespace dpour_scene {

// Defined below; the guest-output callback (in the anonymous namespace) records
// the frame through it. Returns true when it composited into `out_res`, i.e.
// when the presenter should show OUR image instead of the emulated one.
bool RenderRecorded(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* dl,
                    ID3D12GraphicsCommandList* cmd, std::uint32_t width, std::uint32_t height,
                    ID3D12Resource* out_res, std::uint64_t submission, std::uint64_t completed);
namespace {

// The native pass renders at the guest's own resolution. That is not a
// compromise for speed: it is what makes the result comparable. The viewport,
// the scissor and the half-pixel offset the shaders apply are then literally the
// numbers the game set, with nothing rescaled behind its back. Upscaling belongs
// in the composite, where it costs one bilinear sample.
constexpr std::uint32_t kDefaultWidth = 1280;
constexpr std::uint32_t kDefaultHeight = 720;

// The Xbox 360 scene colour target is 7e3 (10-bit float RGB, 3-bit alpha), so
// the shaders write values above 1. A UNORM target would clip them before the
// game's own tonemapping has had a chance to run; half-float keeps them.
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kDepthResFormat = DXGI_FORMAT_R32_TYPELESS;
// Backbuffer the presenter composites into (d3d12_presenter.h kSwapChainFormat).
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

constexpr std::uint32_t kPassFrames = 3;
constexpr std::size_t kMaxDrawsPerFrame = 16384;

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// TWO ADDRESS KINDS, TWO CHECKS. Getting this wrong cost a whole session:
//
//   OBJECT POINTERS (FXeVertexBuffer*, IDirect3DVertexBuffer9*, FLinearColor*)
//   come out of the game's virtual heap and are measured at 0x4xxxxxxx-0xBxxxxxxx.
//   The 0xC0000000 ceiling is a real sanity check for these, and it is the only
//   thing standing between a garbage field and a read of arbitrary memory that
//   happens to be committed.
//
//   DATA ADDRESSES (what a Xenos fetch constant points at) come out of
//   appPhysicalAlloc and are measured at 0xF3xxxxxx-0xF5xxxxxx - ABOVE that
//   ceiling. Applying the pointer check to them dropped every static mesh in
//   the world (censused 28.07: fetch == BaseAddress == 0xF3df9000, refused).
//
// Commit eb2612a loosened the SHARED predicate to fix the second case, which
// silently loosened the first as well - in ten call sites including the Clear
// colour and the user-pointer draw data. Two names now, one job each.
inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }
inline bool GuestDataAddrPlausible(std::uint32_t a) { return a >= 0x1000u; }

bool SpanReadable(const void* p, std::size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
      (mbi.Protect & PAGE_NOACCESS) != 0) {
    return false;
  }
  const auto* start = static_cast<const std::uint8_t*>(mbi.BaseAddress);
  return static_cast<const std::uint8_t*>(p) + n <= start + mbi.RegionSize;
}

// VirtualQuery is a SYSTEM CALL, and this is asked once per bound stream per
// draw - tens of thousands of times a frame. Answering it directly cost more
// than the entire rest of the renderer: gameplay ran at 6 FPS with a 170 ms
// frame, essentially all of it here.
//
// The probes are all the same shape - the 20-byte header of an RHI resource
// object - and a level keeps re-binding the same few thousand objects, so the
// answer is memoised per address, negative results included. An object that
// cannot be read now will not become readable by being asked again in the same
// frame.
bool ObjectReadable(const std::uint8_t* base, std::uint32_t addr, std::uint32_t size) {
  static thread_local std::unordered_map<std::uint32_t, bool> memo;
  auto it = memo.find(addr);
  if (it != memo.end()) {
    return it->second;
  }
  const bool ok = SpanReadable(base + addr, size);
  if (memo.size() > 262144) {
    memo.clear();
  }
  memo.emplace(addr, ok);
  return ok;
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// The size of a guest vertex/index buffer, in bytes, or 0 when it cannot be
// established.
//
// This is needed because UE3 does not always tell the RHI which vertices a draw
// touches - RHIDrawIndexedPrimitive's MinIndex/NumVertices are zero on most of
// Downpour's draws - and sizing the host copy from the drawn range therefore
// dropped 96% of the world. The buffer's real size is in the D3D object the RHI
// resource points at: XGSetVertexBufferHeader writes an Xbox 360 vertex FETCH
// CONSTANT there, whose first dword is the base address and whose second holds
// the size in dwords (XeD3DVertexBuffer.cpp:27).
//
// The offset of that fetch constant inside the D3D object is not documented
// here, so it is not guessed: the candidates are probed and the one whose
// address field MATCHES the BaseAddress we already know is the right one. A
// wrong offset cannot pass that test.
struct BufferFetch {
  std::uint32_t addr = 0;
  std::uint32_t size = 0;
};

// Where the fetch constant sits inside the D3D object. Learned once from a case
// where the answer can be CHECKED - the RHI resource knows its own BaseAddress,
// so the offset whose address field matches it is the right one - and reused
// afterwards for D3D objects reached directly from the device, where there is
// nothing to check against.
std::atomic<std::int32_t> g_fetch_offset{-1};

bool ReadFetchAt(const std::uint8_t* base, std::uint32_t d3d, std::uint32_t off,
                 BufferFetch& out) {
  const std::uint32_t f0 = LoadBE32(base + d3d + off);
  const std::uint32_t f1 = LoadBE32(base + d3d + off + 4);
  const std::uint32_t addr = f0 & 0xFFFFFFFCu;
  const std::uint64_t bytes = static_cast<std::uint64_t>((f1 >> 2) & 0xFFFFFFu) * 4ull;
  // A DATA address - the physical heap above 0xC0000000 is fair game HERE, and
  // nowhere else in this file. See the two predicates at the top.
  if (!GuestDataAddrPlausible(addr) || bytes < 4 || bytes > (48ull << 20)) {
    return false;
  }
  out.addr = addr;
  out.size = static_cast<std::uint32_t>(bytes);
  return true;
}

// Address AND size of a guest vertex/index buffer, from the Xenos fetch constant
// that XGSetVertexBufferHeader wrote into its D3D object
// (XeD3DVertexBuffer.cpp:27).
//
// This exists because UE3 does not reliably tell the RHI which vertices a draw
// touches - RHIDrawIndexedPrimitive's MinIndex/NumVertices arrive as nonsense on
// most of Downpour's draws - so the drawn range cannot size the upload. The
// buffer's own header can.
//
// `rhi_object` is optional and used only to learn/confirm the offset;
// `d3d_object` is what the device was actually handed.
// Why ResolveBuffer said no, for the census below. The reference has no
// equivalent of this whole mechanism - Unleashed's SetStreamSource receives the
// buffer as its own host struct (video.cpp:5116) because creation is hooked
// (video.cpp:3160), so "the bound buffer cannot be read" is a failure class
// that exists only here, where the game owns creation and we read its structs.
enum : std::uint8_t {
  kVbFailNone = 0,
  kVbFailD3dBad = 1,      // the D3D struct pointer itself is implausible/unreadable
  kVbFailNoKey = 2,       // no RHI BaseAddress to verify against, learned offset missed
  kVbFailMismatch = 3,    // fetch constant found, but no offset matched BaseAddress
};

bool ResolveBuffer(const std::uint8_t* base, std::uint32_t rhi_object, std::uint32_t d3d_object,
                   BufferFetch& out, std::uint8_t* fail_reason = nullptr) {
  std::uint32_t d3d = d3d_object;
  std::uint32_t known_addr = 0;
  if (GuestAddrPlausible(rhi_object) && ObjectReadable(base, rhi_object, 20)) {
    known_addr = LoadBE32(base + rhi_object + 12);  // FXeGPUResource::BaseAddress
    if (!GuestAddrPlausible(d3d)) {
      d3d = LoadBE32(base + rhi_object + 8);  // ::Resource
    }
  }
  if (!GuestAddrPlausible(d3d) || !ObjectReadable(base, d3d, 64)) {
    if (fail_reason != nullptr) {
      *fail_reason = kVbFailD3dBad;
    }
    return false;
  }

  const std::int32_t learned = g_fetch_offset.load(std::memory_order_relaxed);
  if (learned >= 0 && ReadFetchAt(base, d3d, static_cast<std::uint32_t>(learned), out)) {
    if (known_addr == 0 || (out.addr & 0xFFFFFFFCu) == (known_addr & 0xFFFFFFFCu)) {
      return true;
    }
  }
  if (known_addr == 0) {
    if (fail_reason != nullptr) {
      *fail_reason = kVbFailNoKey;
    }
    return false;  // nothing to verify against, and the learned offset did not fit
  }
  for (const std::uint32_t off : {24u, 28u, 20u, 32u, 16u}) {
    if (ReadFetchAt(base, d3d, off, out) &&
        (out.addr & 0xFFFFFFFCu) == (known_addr & 0xFFFFFFFCu)) {
      g_fetch_offset.store(static_cast<std::int32_t>(off), std::memory_order_relaxed);
      return true;
    }
  }
  // Refused after every offset: the device stream was bound by something that
  // never went through RHISetStreamSource, so the two shadows name different
  // buffers. Measured 28.07: these are FGPUMemMove's memexport defrag draws
  // (XeD3DUtil.cpp:137 - a stack-built D3D struct, stride 64). They MUST stay
  // uncaptured: a captured draw is a suppressed guest submission under
  // own-device, and suppressing the defrag pass corrupts the memory it moves.
  if (fail_reason != nullptr) {
    *fail_reason = kVbFailMismatch;
  }
  return false;
}

// Census of draws dropped because the bound vertex buffer could not be
// resolved. Same idea as the white-texture census that found the resolve-link
// hole: the counter alone said "11827 vb object" during gameplay while the
// world stayed white, and named nothing. Keyed by the RHI object (or the D3D
// object when the RHI shadow is empty), keeps the raw fetch dwords at +24 so
// the print can say whether the fetch was absent, out of clamp, or pointing at
// different memory than the RHI object's BaseAddress.
struct VbFailEntry {
  std::uint32_t key = 0;
  std::uint32_t rhi = 0;
  std::uint32_t d3d = 0;
  std::uint32_t known_addr = 0;
  std::uint32_t raw0 = 0;  // BE dword at d3d+24
  std::uint32_t raw1 = 0;  // BE dword at d3d+28
  std::uint32_t stream = 0;
  std::uint32_t stride = 0;
  std::uint8_t reason = kVbFailNone;
  std::uint64_t count = 0;
};
std::mutex g_vbfail_mutex;
VbFailEntry g_vbfail[12];
std::atomic<std::uint64_t> g_vbfail_reason[4] = {};

void NoteVbObjFail(const std::uint8_t* base, std::uint32_t stream, std::uint32_t rhi,
                   std::uint32_t d3d, std::uint32_t stride, std::uint8_t reason) {
  g_vbfail_reason[reason < 4 ? reason : 0].fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t key = rhi != 0 ? rhi : d3d;
  std::lock_guard<std::mutex> lock(g_vbfail_mutex);
  VbFailEntry* slot = nullptr;
  for (auto& e : g_vbfail) {
    if (e.count != 0 && e.key == key) {
      slot = &e;
      break;
    }
    if (slot == nullptr && e.count == 0) {
      slot = &e;
    }
  }
  if (slot == nullptr) {
    return;  // table full - the top offenders are already named
  }
  slot->key = key;
  slot->rhi = rhi;
  slot->d3d = d3d;
  slot->stream = stream;
  slot->stride = stride;
  slot->reason = reason;
  slot->known_addr = 0;
  slot->raw0 = 0;
  slot->raw1 = 0;
  if (GuestAddrPlausible(rhi) && ObjectReadable(base, rhi, 20)) {
    slot->known_addr = LoadBE32(base + rhi + 12);
  }
  const std::uint32_t d3d_eff =
      GuestAddrPlausible(d3d) ? d3d
      : (slot->known_addr != 0 && GuestAddrPlausible(rhi) && ObjectReadable(base, rhi, 20))
          ? LoadBE32(base + rhi + 8)
          : 0;
  if (GuestAddrPlausible(d3d_eff) && ObjectReadable(base, d3d_eff, 32)) {
    slot->raw0 = LoadBE32(base + d3d_eff + 24);
    slot->raw1 = LoadBE32(base + d3d_eff + 28);
  }
  ++slot->count;
}

// D3DPRIMITIVETYPE -> what the input assembler needs. Fans, rect lists and quad
// lists have no D3D12 equivalent and are left to the guest renderer rather than
// silently drawn as something else.
bool Topology(std::uint32_t prim, D3D12_PRIMITIVE_TOPOLOGY& topo,
              D3D12_PRIMITIVE_TOPOLOGY_TYPE& type) {
  switch (prim) {
    case 1:
      topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      return true;
    case 2:
      topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case 3:
      topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case 4:
      topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      return true;
    case 6:
      topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      return true;
    default:
      return false;
  }
}

// THE CPU STAGING ARENA - UnleashedRecomp's IntermediaryUploadAllocator
// (video.cpp:533), 16 MB blocks and all.
//
// Capture writes the bytes of a draw HERE, into plain memory, and records an
// OFFSET. The GPU address is created later, in the replay, out of the frame's
// upload allocator - which is what FlushRenderStateForRenderThread does
// (video.cpp:4520) and what ProcDrawPrimitiveUP does for its vertices (:4711).
//
// What this replaces: capture allocated GPU memory and baked the ADDRESS into
// the snapshot. Anything reading that snapshot afterwards - a second replay, a
// replay after the allocator rotated - read whatever now lived at that address.
// On screen: the loading screen's text still standing in gameplay and coming
// apart one glyph at a time, because the geometry was fine and the constants
// underneath it belonged to a later frame. Every workaround built around that
// (one replay per published frame, conservative completion, slack in the upload
// caches) exists only because the address was baked too early. They come out
// with it.
struct CpuArena {
  static constexpr std::uint32_t kBlockBytes = 16u << 20;
  std::vector<std::unique_ptr<std::uint8_t[]>> blocks;
  std::uint32_t index = 0;
  std::uint32_t offset = 0;

  // Returns the writable pointer and, through out_handle, what the snapshot
  // stores. Allocations never straddle a block, so the handle decomposes back
  // into (block, byte) by plain division.
  std::uint8_t* Alloc(std::uint32_t bytes, std::uint32_t& out_handle) {
    if (bytes == 0 || bytes > kBlockBytes) {
      return nullptr;
    }
    const std::uint32_t aligned = (bytes + 15u) & ~15u;
    if (offset + aligned > kBlockBytes) {
      ++index;
      offset = 0;
    }
    if (blocks.size() <= index) {
      blocks.resize(index + 1);
    }
    auto& b = blocks[index];
    if (!b) {
      b = std::make_unique<std::uint8_t[]>(kBlockBytes);
    }
    out_handle = index * kBlockBytes + offset;
    std::uint8_t* at = b.get() + offset;
    offset += aligned;
    return at;
  }

  const std::uint8_t* At(std::uint32_t handle) const {
    const std::uint32_t i = handle / kBlockBytes;
    if (i >= blocks.size() || !blocks[i]) {
      return nullptr;
    }
    return blocks[i].get() + (handle % kBlockBytes);
  }

  void Reset() {
    index = 0;
    offset = 0;
  }
};
constexpr std::uint32_t kNoStage = 0xFFFFFFFFu;

// --- a captured draw, with every input already resolved ----------------------
struct SceneDraw {
  ID3D12PipelineState* pso = nullptr;
  D3D12_VERTEX_BUFFER_VIEW vbv[kMaxStreams]{};
  std::uint32_t vbv_count = 0;
  D3D12_INDEX_BUFFER_VIEW ibv{};
  // Handles into the staging arena, NOT GPU addresses. Resolved in the replay.
  std::uint32_t cb_vertex = kNoStage;
  std::uint32_t cb_pixel = kNoStage;
  std::uint32_t cb_shared = kNoStage;
  std::uint32_t cb_vertex_bytes = 0;
  std::uint32_t cb_pixel_bytes = 0;
  // User-primitive geometry, staged the same way (ProcDrawPrimitiveUP allocates
  // its vertices from the FRAME allocator at draw time, video.cpp:4711).
  std::uint32_t up_vtx = kNoStage;
  std::uint32_t up_vtx_bytes = 0;
  std::uint32_t up_idx = kNoStage;
  std::uint32_t up_idx_bytes = 0;
  std::uint32_t count = 0;  // indices or vertices
  std::uint32_t start = 0;
  // BeginIndexedVertices rebases indices onto the copied vertex range with a
  // negated MinVertexIndex; regular draws keep 0 (their offset lives in the
  // stream binding).
  std::int32_t base_vertex = 0;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  bool indexed = false;
  std::uint32_t pass = 0;  // which render-target pass this draw belongs to
  // Carried purely so the log can name what was being drawn. A GPU hang reports
  // nothing about which shader caused it, and "it hung after a new shader was
  // resolved" is not an identification.
  std::uint64_t vs_hash = 0;
  std::uint64_t ps_hash = 0;
  // The engine's own verdict on this draw, from FMeshDrawingPolicy::DrawMesh
  // (see downpour_native_ue3.h). 0xFF = the engine said nothing, which means
  // the draw did not come from a mesh element at all (UI, post-process quads).
  std::uint8_t dpg = 0xFF;
};

// A RESOLVE THE GAME ASKED FOR, recorded where it happened in the draw stream.
//
// RHICopyToResolveTarget (XeD3DRenderTarget.cpp:399) copies a surface into its
// resolve texture and, doing so, UNDOES the exponent bias the shaders wrote with
// (:519, D3DRESOLVE_EXPONENTBIAS(-GetColorExpBias())). We have been aliasing
// instead: handing the sampler the surface's own target, which skips the copy
// AND the un-biasing, so everything the game composes from the scene texture
// reads 2^bias times too bright - eight times, for the scene colour
// (SCENE_COLOR_BIAS_FACTOR_EXP = 3). That is the white screen, and our own probe
// had already photographed it as PS c0 = 8.0.
//
// The bias is NOT a constant to hardcode: XeGetRenderTargetColorExpBias
// (XeD3DRenderTarget.cpp:94) returns 3, 5 or 0 depending on the surface and
// texture formats. It is read per surface, from the game's own struct.
struct PendingResolve {
  std::uint32_t surface = 0;     // source surface object (target registry key)
  std::uint32_t d3d_tex = 0;     // destination D3D texture object (link key)
  std::int32_t bias = 0;         // XeSurfaceInfo.ColorExpBias
  std::uint32_t draw_index = 0;  // how many draws had been staged when it fired
};

// The game's own Clear, captured as a stream item - RenderCommandType::Clear in
// the reference (video.cpp:824), executed by ProcClear at its position with the
// game's own colour into the currently bound target. Dropping the guest clear
// under own-device and substituting one black clear per replay threw both
// away: the colour (a screen whose content IS a coloured clear plus quads came
// out ours-black) and the position (a surface the game clears mid-frame and
// reuses accumulated both uses).
struct PendingClear {
  std::uint32_t pass = 0;        // pass slot bound when the clear fired
  float rgba[4] = {};
  float depth = 1.0f;            // engine-side value; the GInvertZ flip is applied at replay
  bool clear_color = false;
  bool clear_depth = false;
  std::uint32_t draw_index = 0;  // position in the item stream
};

std::mutex g_mutex;
std::vector<SceneDraw> g_staging;
std::vector<SceneDraw> g_published;
// FOUR ARENAS IN ROTATION, so a snapshot and the bytes it points at can never
// come apart. Capture fills one, publish hands it to the replay and moves
// capture to the next. The replay notes WHICH arena its snapshot came with and
// reads only that one - a fresh publish cannot pull the ground from under a
// replay in progress, because it lands three arenas away.
//
// Rotating rather than allocating anew: an arena holds 16 MB blocks, and
// building them per frame would trade one bug for a stall.
constexpr std::uint32_t kArenaCount = 4;
CpuArena g_arenas[kArenaCount];
std::uint32_t g_stage_arena_index = 0;      // capture writes here
std::uint32_t g_published_arena_index = 0;  // the replay reads here
std::vector<PendingResolve> g_resolve_staging;
std::vector<PendingResolve> g_resolve_published;
std::vector<PendingClear> g_clear_staging;
std::vector<PendingClear> g_clear_published;
std::atomic<ID3D12Device*> g_device{nullptr};

// Render-target passes. A frame draws into several surfaces; only the one that
// receives the most geometry is the scene, and replaying a shadow pass into it
// would smear a light's view across the image.
// SIXTEEN WAS NOT ENOUGH, and the shortfall was silent. A measured Downpour
// frame binds 23 DISTINCT colour surfaces (logged: "device pass: surface ..."),
// and with the MSAA diversion gone every one of them wants its own slot. Past
// the sixteenth, PassSlotFor used to hand back slot 0 - which is the BACK
// BUFFER on most frames - so shadow, downsample and post draws were rendered
// straight into the image that gets composited. Enough slots for every surface
// the game can bind, and an explicit refusal past that, never slot 0.
constexpr std::uint32_t kMaxPasses = 64;
struct PassSlot {
  std::uint32_t color = 0;   // the ref pointer the RHI call carried (pass key)
  std::uint32_t depth = 0;
  std::uint32_t color_object = 0;  // dereferenced surface objects - the
  std::uint32_t depth_object = 0;  // identities the target registry keys on
  // The bound surface's host colour format (SurfaceMeta.dxgi at bind time,
  // kColorFormat when unknown). The PSO built for a draw in this pass must name
  // the format of the target the replay will bind, or D3D12 rejects the pair.
  DXGI_FORMAT rtv_format = kColorFormat;
  // The replay binds this pass with NO depth buffer (the back buffer under
  // own-device - the console itself binds it RHISetRenderTarget(GD3DBackBuffer,
  // NULL)). A PSO that names a DSV format while no DSV is bound is a draw the
  // hardware silently discards - which blacked out every menu and UI quad the
  // moment the direct bind went in.
  bool no_depth = false;
  std::uint32_t draws = 0;
  // The viewport in force while this pass was drawing. Sizing the native target
  // from "the last viewport anyone set" made it flip between the scene's, the
  // UI's and a post-process pass's every single frame, reallocating the targets
  // each time; the size that matters is the one the PUBLISHED pass used.
  std::uint32_t view_w = kDefaultWidth;
  std::uint32_t view_h = kDefaultHeight;
  // The colour object matched the guest's GD3DBackBuffer at bind time: this is
  // the game's own final composition pass (post-process quads + UI). Kept per
  // bind rather than latched, since ref pointers (the pass key) can be stack
  // temporaries that a later frame reuses for a different surface.
  bool is_backbuffer = false;
};
PassSlot g_passes[kMaxPasses]{};
PassSlot g_published_passes[kMaxPasses]{};  // snapshot the render thread replays from
std::uint32_t g_pass_count = 0;
std::uint32_t g_pass_current = 0;
std::uint32_t g_published_pass = 0;
// The last slot is reserved for the scene as marked by the MSAA block hooks
// (The reserved "scene pass" slot and its stand-in colour are DELETED, with
// the MSAA bracket that fed them: Downpour ships GUseTilingCode = FALSE, so the
// bracket never ran, and no reference asks which draws are "the scene".)
// UE3's ESceneDepthPriorityGroup: 0 = UnrealEdBackground, 1 = World,
// 2 = Foreground. Measured live: 119815 world against 2441 foreground.
constexpr std::uint8_t kSDPGWorld = 1;
// Registry key for the native back-buffer target. The guest back buffer never
// passes through RHICreateTargetableSurface, so it has no created surface
// object to key on; this reserved key stands in for it.
constexpr std::uint32_t kBackbufferKey = 0xB0BACBADu;
// The surface pair the scene pass drew into last frame. Only draws going there
// get resolved in full.
//
// Without this every draw in the frame - shadow maps, reflections, the whole
// post-process chain, the UI - was given a pipeline, a constant snapshot and a
// host buffer, and then discarded because it belonged to the wrong pass. That is
// roughly ten times the work for the same picture, and it showed: gameplay ran
// at 10 FPS with nothing drawn at all.
std::uint32_t g_target_color = 0;
std::uint32_t g_target_depth = 0;
bool g_have_target = false;
// The depth surface the world's BASE pass targets, learned from the first
// shaded world draw. It is what separates the camera depth prepass (same
// surface, belongs in our scene) from a shadow-depth pass (different surface,
// must never fill our depth). Guarded by g_mutex like the pass table.
std::uint32_t g_scene_depth_object = 0;
// The colour surface the base pass writes into, named by the first shaded world
// draw of the frame and reset with it. Per-frame rather than sticky: the game
// changes render resolution between menus and gameplay and reuses surfaces.
std::uint32_t g_frame_scene_color = 0;
std::uint32_t g_scene_prepass_draws = 0;
// Samples refused because the texture aliases the bound render target.
std::atomic<std::uint64_t> g_rt_alias_refused{0};
std::atomic<std::uint64_t> g_scene_depth_func[16] = {};
std::atomic<std::uint64_t> g_scene_depth_writes{0};

// The guest surface objects the game created as the scene colour target
// ("DefaultColor" plus its Raw / FixedPoint aliases over the same EDRAM) and
// scene depth ("DefaultDepth") - from the RHICreateTargetableSurface hook.
// Binding any colour alias IS the start of the scene on Downpour's non-tiling
// path. Aliases are few and fixed; recreation (video mode change) just
// re-fills the same slots.
constexpr std::uint32_t kMaxSceneAliases = 4;
std::atomic<std::uint32_t> g_scene_color_aliases[kMaxSceneAliases]{};
std::atomic<std::uint32_t> g_scene_color_surface{0};
std::atomic<std::uint32_t> g_scene_depth_surface{0};
// The FXeTexture2D the "DefaultColor" surface resolves into (creation arg).
std::atomic<std::uint32_t> g_scene_resolve_texture{0};
// The destination texture's fetch constant, located by exact-dimension scan of
// the FSurfaceRHIRef at the first scene resolve (see LocateResolveTextureFetch).
std::atomic<std::uint32_t> g_scene_fetch_addr{0};

// --- reference-style target registry (the UnleashedRecomp shape) -------------
// Every guest surface object gets its own native target; a resolve LINKS the
// destination texture to the bound surface (no pixels move); sampling a linked
// texture binds the surface target's SRV. This is ProcStretchRect, verbatim.
// Sizes captured at RHICreateTargetableSurface; surfaces created another way
// (the backbuffer) fall back to the pass viewport.
struct SurfaceMeta {
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  // Named by the game itself at RHICreateTargetableSurface: DefaultDepth,
  // ShadowDepthZ, DominantShadowDepthZ, TranslucencyShadowDepthZ. The reference
  // makes the same distinction on the format and acts on it in two places
  // (video.cpp:3572 and :3582, "Depth stencil textures in this game are
  // guaranteed to be transient") - it does not copy depth resolves at all.
  bool is_depth = false;
  // The game's OWN name for this surface, copied because the guest string it
  // came from may be freed. "5 of 17 resolves had no source" is not actionable;
  // "SceneColorLDR and FilterColor had no source" says which pass to look at.
  char name[24] = {};
  // Host format for this surface, from the game's own EPixelFormat at creation
  // (the reference's ConvertFormat, video.cpp:3065). DXGI_FORMAT_UNKNOWN when
  // the create hook never saw it (the back buffer) - callers fall back to
  // kColorFormat.
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
};

// UE3 EPixelFormat (EngineTextureClasses.h:153, verified against the game's own
// header, values 0..26) -> the host format the reference would pick. Two
// deliberate deviations from a literal table:
//   - PF_FloatRGB is the Xenon 7e3 EDRAM format (10-bit float RGB): values run
//     past 1.0, so it maps to half-float, the same choice the reference makes
//     for its HDR formats.
//   - depth formats return UNKNOWN here; depth targets have their own path.
DXGI_FORMAT DxgiFromPixelFormat(std::uint32_t pf) {
  switch (pf) {
    case 1: return DXGI_FORMAT_R32G32B32A32_FLOAT;  // PF_A32B32G32R32F
    case 2: return DXGI_FORMAT_R8G8B8A8_UNORM;      // PF_A8R8G8B8
    case 3: return DXGI_FORMAT_R8_UNORM;            // PF_G8
    case 4: return DXGI_FORMAT_R16_UNORM;           // PF_G16
    case 9: return DXGI_FORMAT_R16G16B16A16_FLOAT;  // PF_FloatRGB (7e3 EDRAM)
    case 10: return DXGI_FORMAT_R16G16B16A16_FLOAT; // PF_FloatRGBA
    case 14: return DXGI_FORMAT_R32_FLOAT;          // PF_R32F
    case 15: return DXGI_FORMAT_R16G16_UNORM;       // PF_G16R16
    case 16: return DXGI_FORMAT_R16G16_FLOAT;       // PF_G16R16F
    case 17: return DXGI_FORMAT_R16G16_FLOAT;       // PF_G16R16F_FILTER
    case 18: return DXGI_FORMAT_R32G32_FLOAT;       // PF_G32R32F
    case 19: return DXGI_FORMAT_R10G10B10A2_UNORM;  // PF_A2B10G10R10
    case 20: return DXGI_FORMAT_R16G16B16A16_UNORM; // PF_A16B16G16R16
    case 22: return DXGI_FORMAT_R16_FLOAT;          // PF_R16F
    case 23: return DXGI_FORMAT_R16_FLOAT;          // PF_R16F_FILTER
    default: return DXGI_FORMAT_UNKNOWN;  // depth family, compressed, unknown
  }
}

// DPOUR_NR_SURFACE_FMT=1 (implied by DPOUR_NR_OWN_DEVICE): native targets take
// the guest surface's own format instead of one hardcoded half-float. An
// 8-bit surface then clips above 1.0 exactly where the console clips it.
// DEFAULT OFF outside own-device: it changes every pipeline's RTV format.
// (EnvOn and OwnDeviceMode are defined later in this namespace; the definition
// sits after them.)
bool SurfaceFormatOn();
bool OwnDeviceMode();
extern float g_depth_clear;  // defined with the scene targets below
std::mutex g_reg_mutex;
std::unordered_map<std::uint32_t, SurfaceMeta> g_surface_meta;       // surface obj -> size
std::unordered_map<std::uint32_t, std::uint32_t> g_texture_link;     // texture obj -> surface obj
// SURFACES A RESOLVE POINTS AT MUST HAVE A TARGET BEFORE ANYTHING SAMPLES THEM.
//
// The reference creates the host texture inside CreateSurface (video.cpp:3184),
// so every guest surface owns one from birth and a sample can never find
// nothing. Ours built targets lazily, in bind_pass, which means only surfaces we
// had already DRAWN into had an SRV - measured 28.07: 15 of 54. The scene's own
// resolve was among the missing:
//
//   resolve link: key 0x411c1c00 <- surface 0x40cb8f40 (1280x720) srv -1
//
// 0x40cb8f40 is DefaultColorRaw, and 0x411c1c00 is the texture the game's final
// composition quad samples - the one the white census names every session. The
// link was right, the surface simply had no texture behind it, so the sample
// fell through to white and painted the screen.
//
// EVERY surface, at creation - the reference makes no exception and neither do
// we. The device lives on the render thread, so the guest thread queues and the
// replay drains.
std::vector<std::uint32_t> g_pending_surface_targets;
std::unordered_map<std::uint32_t, std::uint32_t> g_surface_srv;      // surface obj -> bindless SRV
struct NativeTarget {
  ComPtr<ID3D12Resource> color;
  ComPtr<ID3D12Resource> depth;
  std::uint32_t rtv_index = 0;
  std::uint32_t dsv_index = 0;
  std::uint32_t srv_slot = 0;  // bindless heap slot serving this target
  std::uint32_t w = 0, h = 0;
  // The colour format this target was built with (the guest surface's own under
  // SurfaceFormatOn, kColorFormat otherwise). The PSO's RTV format must match
  // it, and a format change on the same key is a rebuild, same as a resize.
  DXGI_FORMAT format = kColorFormat;
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  std::uint64_t cleared_frame = 0;
};
std::unordered_map<std::uint32_t, NativeTarget> g_reg_targets;  // render thread only
ComPtr<ID3D12DescriptorHeap> g_reg_rtv_heap;
ComPtr<ID3D12DescriptorHeap> g_reg_dsv_heap;
std::uint32_t g_reg_rtv_next = 0;
std::uint32_t g_reg_dsv_next = 0;
// INDICES COME BACK. Measured, 2026-07-28: the game creates 57 targetable
// surfaces over a few minutes of play while the registry stopped at 48, and the
// log said what that costs in as many words - "target registry FULL at 48
// surfaces, every later surface renders nowhere". A surface with no target is a
// surface whose draws go nowhere and whose texture link samples nothing, which
// is a black screen arriving several minutes in.
//
// A bigger ceiling alone would only postpone it: the guest creates and destroys
// surfaces for the whole session, so what matters is that a destroyed one gives
// its slot back. That is what the reference does - TextureDescriptorAllocator
// (video.cpp:354) pops from `freed` before growing, and DestructResource hands
// a surface's descriptor back to it (video.cpp:713). These are the same two
// lists under a fixed-size heap, which is the only part D3D12 makes us size up
// front.
std::vector<std::uint32_t> g_reg_rtv_freed;
std::vector<std::uint32_t> g_reg_dsv_freed;
std::vector<std::uint32_t> g_reg_srv_freed;
// Surfaces the guest has retired, queued by the guest thread and drained by the
// replay - the device and the registry belong to the render thread.
std::vector<std::uint32_t> g_pending_surface_retire;
// WHY THE SLOTS COME BACK BUT NOT THROUGH THE RETIREMENT QUEUE.
//
// AddUnusedXeResource is the reference's DestructResource for everything the
// game allocates itself, and it is where our texture and vertex-buffer caches
// are invalidated. It is NOT where render-target surfaces arrive: its argument
// is an FXeGPUResource's D3D object plus that resource's BaseAddress
// (XeD3DResources.h:19), and a targetable surface is not one of those. The
// game's own header settles it - `class FSurfaceRHIRef : public
// TRefCountPtr<IDirect3DSurface9>` (XeD3DRenderTarget.h:68). There is no FXe
// wrapper around a surface at all; the ref IS the D3D surface, refcounted, and
// it dies when the last reference drops rather than on a retirement queue.
//
// Measured before reading that: 849 retirement calls in one run, none of them a
// surface. So the hook below is kept (it costs a hash lookup and would be right
// the day a surface does pass through), the counters stay so the next person
// sees the same thing without a build, and the ceiling is what actually had to
// move.
std::atomic<std::uint64_t> g_retire_calls{0};
std::atomic<std::uint64_t> g_retire_matched{0};
constexpr std::uint32_t kRegMaxTargets = 256;

std::uint32_t TakeIndex(std::vector<std::uint32_t>& freed, std::uint32_t& next) {
  if (!freed.empty()) {
    const std::uint32_t v = freed.back();
    freed.pop_back();
    return v;
  }
  return next++;
}

// Destination textures for the real resolve copy - one per resolved D3D texture
// object. The reference owns exactly this: ExecutePendingStretchRectCommands
// renders a copy of the surface INTO the destination texture, which is what
// makes it safe for a later draw to sample it while rendering back into the
// source surface. See ResolveCopyEnabled() for why this is behind a flag.
struct ResolveTexture {
  ComPtr<ID3D12Resource> tex;
  std::uint32_t rtv_index = 0;
  std::uint32_t srv_slot = 0;
  std::uint32_t w = 0, h = 0;
};
std::unordered_map<std::uint32_t, ResolveTexture> g_resolve_textures;  // render thread only
std::unordered_map<std::uint32_t, std::uint32_t> g_resolve_srv;  // d3d tex -> srv (g_reg_mutex)
// WHEN each of those was last actually written by us, in guest frames. A slot
// that exists is not a slot that holds this frame's content: the ghost of the
// loading screen in the menu was a texture whose copy stopped being refreshed
// three states ago and kept being served anyway. Both maps live under
// g_reg_mutex; the helpers below assume the caller already holds it.
std::unordered_map<std::uint32_t, std::uint64_t> g_resolve_copy_frame;  // d3d tex -> guest frame
std::unordered_map<std::uint32_t, std::uint64_t> g_surface_drawn_frame;  // surface -> guest frame
extern std::atomic<std::uint64_t> g_frame;
// Three guest frames of slack, not one: capture runs a frame ahead of the replay
// that makes the copy, and a loading stall replays one published snapshot for a
// while. Two would be the tight bound; the third is there so a hitch does not
// flicker the picture between native and decoded.
constexpr std::uint64_t kFreshFrames = 3;

// DEFAULT OFF - it made the menu WHITE, which is worse than the ghost it was
// meant to remove. Measured 2026-07-26, reverted the same minute.
//
// The reasoning was sound and the result still refutes it: refusing to serve a
// texture we never refreshed should hand the game the guest texture the emulated
// path wrote. It does not - the decode comes back white, the same failure the
// glyph corruption showed earlier. So there is no working fallback to yield TO,
// and the ghost has to be fixed by making the copy happen, not by declining to
// serve a stale one. Kept behind the flag because the two maps it fills are the
// measurement that tells us WHICH surfaces are never rendered by us.
// DPOUR_NR_ALIAS_KEY=1 to enable. DEFAULT OFF until it is shown to do what the
// source says it should: the first measurement had GuestBaseAddress returning 0
// for every resolve destination, so the key silently fell back to the texture
// object and the mechanism was doing nothing. Shipping an unverified mechanism
// on top of a user-visible regression is how the last two hours went.
bool AliasKeyOn() {
  static const bool on = EnvOn("DPOUR_NR_ALIAS_KEY");
  return on;
}

bool FreshGateOff() {
  static const bool on = EnvOn("DPOUR_NR_FRESH_GATE");
  return !on;
}

// DPOUR_NR_FLAT=1 - THE skate3 MODEL. Stop reproducing the game's frame graph.
//
// Three references, two working models, and ours was neither (28.07 analysis):
//
//   A. Unleashed/Marathon replace D3D9 entirely, so every guest resource IS
//      their struct and the game's frame graph can be reproduced faithfully -
//      they own every node of it.
//   B. skate3 does the opposite: it never touches the game's render targets,
//      resolves or post quads. It takes the geometry, renders ITS OWN scene
//      into ITS OWN target with ITS OWN post chain, and writes that straight
//      into guest_output (skate3_native_scene_gpu.cpp:7918 clear + :10475
//      output).
//
// We were pursuing A's goal (reproduce the graph) without A's prerequisite
// (own the resources), which is why every repaired link only revealed the next
// one: a graph rebuilt from outside has no bound on how many nodes are still
// wrong. This flag takes B instead - every captured draw goes into one scene
// target in submission order, and one blit puts it on screen. The pass table,
// the vote, the per-surface registry, the resolve links and the composite are
// all bypassed; if this works they get deleted rather than configured.
bool FlatSceneMode() {
  static const bool on = EnvOn("DPOUR_NR_FLAT");
  return on;
}

// DPOUR_NR_SKIP_DEPTH_RESOLVE=1. Skip the resolve COPY for surfaces the game
// named as depth ("...Depth..." at RHICreateTargetableSurface): the reference
// does not copy depth resolves at all (video.cpp:3572 and :3582, "Depth stencil
// textures in this game are guaranteed to be transient").
//
// DEFAULT OFF. It should be a no-op - those resolves already fall out below for
// want of a native target - but it shipped in the same build as a white screen,
// and "should be a no-op" is a claim to measure, not to assert.
bool SkipDepthResolves() {
  static const bool on = EnvOn("DPOUR_NR_SKIP_DEPTH_RESOLVE");
  return on;
}

// DPOUR_NR_DRAW_CWMASK=1. Honour the guest's RB_COLOR_MASK in the pipeline
// instead of writing RGBA unconditionally.
//
// This is state translation, not a filter: the game turns colour writes off for
// the depth prepass, for occlusion work and for the studio's own depth capture,
// and a draw that writes colour when the guest asked for none paints over the
// target. It also replaces the "no pixel shader = prepass" test we had been
// relying on, which this game's own source refutes - SceneRendering.cpp:2625
// runs the masked-material lists in every prepass, and those bind a pixel shader
// for the alpha test.
//
// DEFAULT OFF: it can only ever REMOVE pixels, so if the offset were wrong the
// failure mode is an empty screen. dpour_state::LogStats prints the census that
// says whether it matters at all before anyone turns it on.
bool ColorWriteMaskOn() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_CWMASK");
  return on;
}

// DPOUR_NR_EDRAM_ALIAS=1. Give the whole DefaultColor family ONE native target.
//
// MEASURED, 27.07, this run's own log:
//   resolve link: key 0x411c1c00 <- surface 0x40cb7860   ("DefaultColorRaw")
//   resolve link: key 0x411c1c00 <- surface 0x40cb78a0   ("DefaultColorFixedPoint")
//   resolve WITHOUT SOURCE x200: "DefaultColorFixedPoint" ... -> tex 0x411c1c00
//   passes: ... *0x40cb7820=188@1280x720 ...             (the scene's 188 draws)
// Two surfaces, ONE destination texture, and the 188 scene draws landed in a
// THIRD surface. On the console all three are the same EDRAM tiles - the game's
// own engineers drew the map in XeD3DRenderTarget.cpp:761-769, and
// SceneRenderTargets.cpp:1051 sets SceneColorFixedPoint = Raw.Texture outright -
// so reading through any of them reads what was drawn through the others. Giving
// each its own target loses exactly that, which is why the texture the composite
// samples was never written by us.
//
// This is not "guess which pass is the scene": the family is identified by the
// game's own usage strings, which the create hook already collects.
//
// DEFAULT OFF - it is a visible change.
bool EdramAliasOn() {
  // Implied by own-device: the alias is a property of the GAME - its engineers
  // drew the tile map (XeD3DRenderTarget.cpp:761-769) and set
  // SceneColorFixedPoint = Raw.Texture outright (SceneRenderTargets.cpp:1051).
  // A run without it measures "resolve WITHOUT SOURCE x200: DefaultColorRaw"
  // straight back into the log, which is the menu-white mechanism again.
  static const bool on = EnvOn("DPOUR_NR_EDRAM_ALIAS") || OwnDeviceMode();
  return on;
}

// Is this surface object the game's own scene colour, or one of the aliases it
// created alongside it? Identified by the usage strings the game passes to
// RHICreateTargetableSurface, which the create hook records - not by size, not
// by draw count.
bool IsSceneColorSurface(std::uint32_t surface) {
  if (surface == 0) {
    return false;
  }
  for (const auto& slot : g_scene_color_aliases) {
    const std::uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == 0) {
      return false;
    }
    if (cur == surface) {
      return true;
    }
  }
  return false;
}

// DPOUR_NR_OWN_DEVICE=1. THE UNLEASHED/MARATHON ARRANGEMENT: we own the device,
// so the guest's own submissions do not reach the GPU and there is no emulated
// frame at all - ours is the frame, not a replacement for one.
//
// Decided 27.07.2026 after establishing that our architecture was a third one,
// present in no reference: we hooked like Unleashed but called __imp__ through,
// so both pipelines ran. That is where the doubled frame time came from, where
// the question "which pass is the frame" came from (it does not exist in
// Unleashed or Marathon), and where the white screens came from - they appear on
// the SEAM between the two pipelines.
//
// WHAT THIS COVERS TODAY: the indexed and non-indexed draws, which we reproduce
// in full. Measured groundwork in reference_downpour_device_api_classification:
// of the 74 device functions the game's RHI calls, 39 write GPU packets and 35
// only update the device's register shadow. The 35 can keep running untouched -
// that is precisely why reading the shadow was the right way to recover state.
//
// WHAT IT DOES NOT COVER YET, and the reason is structural rather than
// unfinished work: the user-pointer draws cannot be suppressed at our hook.
// BeginVertices IS the allocation - it hands the game the pointer it then
// memcpy's into - so declining it returns null and the game breaks. Their
// publish is the inlined [dev+13844] -> [dev+48] store at each call site, which
// is not a function and cannot be hooked. Suppressing UP work needs the device
// replaced further down, not a flag here.
//
// DEFAULT OFF, and it must stay off until someone has watched the game with it
// on: it decides what reaches the screen.
bool OwnDeviceMode() {
  static const bool on = EnvOn("DPOUR_NR_OWN_DEVICE");
  return on;
}

bool SurfaceFormatOn() {
  static const bool on = EnvOn("DPOUR_NR_SURFACE_FMT") || OwnDeviceMode();
  return on;
}

// === DPOUR MIGRATION 2026-07-27: what own-device mode RETIRES ==================
//
// Everything listed here exists to negotiate with an emulated frame. Once the
// guest's submissions no longer reach the GPU there is no such frame, the
// negotiation has no other side, and all of it comes out. It is left standing
// until own-device mode has been watched and reaches parity, because until then
// the old path is the only one that produces a picture at all - deleting it
// first would leave nothing to compare against and nothing to fall back to.
//
// DELETE, in this order, once own-device mode is confirmed:
//
//   YieldWithoutSceneOn / the yield block in RenderRecorded
//       skate3's arrangement: hand the frame back to the emulator on menus,
//       loading and movies. There is nothing to hand it back to.
//
//   GuestOutMode / DPOUR_NR_DRAW_GUESTOUT and the "REPLACED natively" log
//       "replacement" stops being a concept; ours is simply the frame.
//
//   BackbufferPrioEnabled + the draw-count vote in EndFrame
//       the whole "which pass is the frame" question, which - as the reference
//       comparison finally made plain - does not exist in Unleashed or Marathon,
//       because every guest surface has its own target and the back buffer is
//       the swapchain texture. It was never a question about this game; it was a
//       question about our own architecture.
//
//   InjectEnabled + the readback/encode pipeline (already idle)
//       writing our image back into guest memory so the emulated pipeline could
//       carry it. No emulated pipeline, no readback.
//
//   NativeOnly / DPOUR_NR_DRAW_ONLY
//       "composite opaquely so the native image is all there is to see" is what
//       own-device mode is, permanently.
//
// KEEP - these are properties of the GAME, not of the old arrangement:
//   the target registry and CanonicalSurface (the EDRAM alias is real, the
//   game's own engineers documented the tile map), the resolve links and copies,
//   RB_COLOR_MASK translation, the state shadow reader, everything in the
//   capture path.
// === END DPOUR MIGRATION 2026-07-27 ==========================================

// DPOUR_NR_YIELD_NO_SCENE=1. Yield the frame to the emulated output when the
// game drew nothing into its own scene colour surface.
//
// THIS IS SKATE3'S GATE, asked of our data model. RenderScene opens with
//   if (!g_scene || g_scene->items.empty()) return false;
// (skate3_native_scene_gpu.cpp:7637) and its g_scene holds WORLD draws only -
// the 2D overlay is published on a separate list. Ours holds everything, so our
// `items.empty()` is true only when the game drew literally nothing; the intro
// movies, which submit their video quads through the ordinary path, sailed past
// it and published a frame we cannot reproduce. That is the white screen.
//
// NOT PORTED, and the reason matters: skate3's other gate reads the presence
// context (rex::kernel::guest_presence::GameplayContextValue() == 0, :5089).
// Measured 27.07 in our own kernel log - Downpour writes
// XGIUserSetContextEx(user=0, context=0x8001, value=0x1) ONCE at startup and
// never again, three context writes in a whole session. The signal carries no
// menu/gameplay information in this game, so there is nothing to port.
//
// DEFAULT OFF - it decides what reaches the screen.
bool YieldWithoutSceneOn() {
  static const bool on = EnvOn("DPOUR_NR_YIELD_NO_SCENE");
  return on;
}

// The surface the registry should key on: for a DefaultColor family member, the
// primary (slot 0); for anything else, itself.
std::uint32_t CanonicalSurface(std::uint32_t surface) {
  if (!EdramAliasOn() || surface == 0) {
    return surface;
  }
  const std::uint32_t primary = g_scene_color_aliases[0].load(std::memory_order_relaxed);
  if (primary == 0 || primary == surface) {
    return surface;
  }
  for (std::uint32_t i = 1; i < kMaxSceneAliases; ++i) {
    const std::uint32_t cur = g_scene_color_aliases[i].load(std::memory_order_relaxed);
    if (cur == 0) {
      break;
    }
    if (cur == surface) {
      return primary;
    }
  }
  return surface;
}

bool FrameIsFresh(const std::unordered_map<std::uint32_t, std::uint64_t>& when, std::uint32_t key) {
  if (FreshGateOff()) {
    return true;
  }
  const auto it = when.find(key);
  if (it == when.end()) {
    return false;
  }
  const std::uint64_t now = g_frame.load(std::memory_order_relaxed);
  return now <= it->second + kFreshFrames;
}

bool CopyIsFresh(std::uint32_t d3d_tex) { return FrameIsFresh(g_resolve_copy_frame, d3d_tex); }
bool SurfaceIsFresh(std::uint32_t surface) { return FrameIsFresh(g_surface_drawn_frame, surface); }
ComPtr<ID3D12DescriptorHeap> g_resolve_rtv_heap;
std::uint32_t g_resolve_rtv_next = 0;
constexpr std::uint32_t kResolveMaxTextures = 32;
// STICKY FAILURE - the skate3 discipline, verified in their code: g_r.failed is
// set at EVERY resource and pipeline creation failure
// (skate3_native_scene_gpu.cpp:2781, 2843, 2928, 2956, 2974, 3342, 4042 ...),
// EnsurePipeline opens with `if (g_r.failed) return false;` (:4146), and only an
// explicit ResetSceneFailure() clears it (:11567). A native path that cannot
// build its resources hands the frame back to the emulator and STAYS there; it
// does not retry a failing CreateCommittedResource sixty times a second while
// the log fills and the driver runs out of whatever it ran out of the first
// time. Failure is where we crash, so failure is where we stop.
bool g_reg_failed = false;

bool EnsureRegHeaps(ID3D12Device* device) {
  if (g_reg_rtv_heap && g_reg_dsv_heap) {
    return true;
  }
  if (g_reg_failed) {
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC rtv{};
  rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv.NumDescriptors = kRegMaxTargets;
  D3D12_DESCRIPTOR_HEAP_DESC dsv = rtv;
  dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  if (FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_reg_rtv_heap))) ||
      FAILED(device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&g_reg_dsv_heap)))) {
    REXLOG_ERROR("[native-scene] target registry descriptor heaps failed - native path off");
    g_reg_failed = true;
    return false;
  }
  return true;
}

// Defined with the retirement list further down: a resource still readable by a
// recorded-but-unfinished submission must outlive our last reference to it.
void DestroyDeferred(ComPtr<ID3D12Resource>&& res);

// The colour + depth resources and their three views, for a target whose
// descriptor indices are already assigned. Split out of GetOrCreateRegTarget so
// the rebuild below runs exactly the same code as the first creation - a second
// copy of it is how the two drift apart.
bool BuildRegTargetResources(ID3D12Device* device, NativeTarget& t, std::uint32_t key,
                             std::uint32_t w, std::uint32_t h, DXGI_FORMAT color_format) {
  t.w = w;
  t.h = h;
  t.format = color_format;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC cd{};
  cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  cd.Width = w;
  cd.Height = h;
  cd.DepthOrArraySize = 1;
  cd.MipLevels = 1;
  cd.Format = color_format;
  cd.SampleDesc.Count = 1;
  cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cclear{};
  cclear.Format = color_format;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cd,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cclear,
                                             IID_PPV_ARGS(&t.color)))) {
    REXLOG_ERROR("[native-scene] registry colour target {:#x} {}x{} failed - native path off", key,
                 w, h);
    g_reg_failed = true;
    return false;
  }
  D3D12_RESOURCE_DESC dd = cd;
  dd.Format = kDepthResFormat;
  dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE dclear{};
  dclear.Format = kDepthFormat;
  dclear.DepthStencil.Depth = g_depth_clear;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &dd,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &dclear,
                                             IID_PPV_ARGS(&t.depth)))) {
    REXLOG_ERROR("[native-scene] registry depth target {:#x} {}x{} failed - native path off", key,
                 w, h);
    g_reg_failed = true;
    return false;
  }
  {  // named for the DRED post-mortem (see the watchdog)
    wchar_t n[64];
    std::swprintf(n, 64, L"dpour.rt.%08x", key);
    t.color->SetName(n);
    std::swprintf(n, 64, L"dpour.rtdepth.%08x", key);
    t.depth->SetName(n);
  }
  // Freshly created, so the states are the ones asked for above, and nothing has
  // cleared this content yet.
  t.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  t.cleared_frame = 0;
  const std::uint32_t rtv_stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const std::uint32_t dsv_stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvh = g_reg_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtvh.ptr += static_cast<std::size_t>(t.rtv_index) * rtv_stride;
  device->CreateRenderTargetView(t.color.Get(), nullptr, rtvh);
  D3D12_CPU_DESCRIPTOR_HANDLE dsvh = g_reg_dsv_heap->GetCPUDescriptorHandleForHeapStart();
  dsvh.ptr += static_cast<std::size_t>(t.dsv_index) * dsv_stride;
  D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
  dv.Format = kDepthFormat;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(t.depth.Get(), &dv, dsvh);
  // A bindless SRV slot, so a texture linked to this surface samples the
  // target directly - the whole point of the registry. On a rebuild the slot is
  // kept and the descriptor overwritten, so every link already pointing at this
  // surface follows the new resource instead of dangling on the old one.
  if (t.srv_slot == dpour_tex::kInvalidSlot) {
    t.srv_slot = dpour_tex::AllocDescriptorSlot();
  }
  if (t.srv_slot != dpour_tex::kInvalidSlot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = color_format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh{};
    sh.ptr = static_cast<std::size_t>(dpour_tex::CpuHandleAt(t.srv_slot));
    device->CreateShaderResourceView(t.color.Get(), &sv, sh);
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    g_surface_srv[key] = t.srv_slot;
  }
  return true;
}

// `size_is_authoritative` - the caller read w/h from the game's own
// RHICreateTargetableSurface record (or knows the fixed back-buffer size), as
// opposed to falling back on the pass viewport. Only an authoritative size may
// trigger the rebuild below; the viewport moves within a frame (the game clips
// UI work to small rects on a full-size surface) and rebuilding on it would
// thrash every target in the registry once a frame.
NativeTarget* GetOrCreateRegTarget(ID3D12Device* device, std::uint32_t key, std::uint32_t w,
                                   std::uint32_t h, bool size_is_authoritative,
                                   DXGI_FORMAT color_format) {
  auto it = g_reg_targets.find(key);
  if (it != g_reg_targets.end()) {
    NativeTarget& t = it->second;
    // THE GUEST REUSES SURFACE ADDRESSES. XeD3DResources.cpp:110-184 frees a
    // surface one frame after its last use and the allocator hands the same
    // address straight back out, so "we already have a target for this key" does
    // not mean "for this surface". Measured in our own log: 0x40cb9c10 arrived
    // as AuxColor 128x128 and later as AuxColor 1024x720, and every draw of the
    // second one went into the first one's 128x128 target - which is exactly
    // what a stale, wrongly-scaled image on screen looks like.
    //
    // A format change on the same key is the same event seen through the other
    // attribute, and rebuilds for the same reason.
    const bool format_matches = !SurfaceFormatOn() || t.format == color_format;
    if (!size_is_authoritative || w == 0 || h == 0 ||
        (t.w == w && t.h == h && format_matches)) {
      return &t;
    }
    if (g_reg_failed) {
      return nullptr;
    }
    static std::uint32_t rebuilt = 0;
    if (rebuilt < 16) {
      ++rebuilt;
      REXLOG_INFO("[native-scene] target {:#x} changed {}x{} fmt {} -> {}x{} fmt {} - rebuilding "
                  "(guest reused the surface address)",
                  key, t.w, t.h, static_cast<int>(t.format), w, h,
                  static_cast<int>(color_format));
    }
    DestroyDeferred(std::move(t.color));
    DestroyDeferred(std::move(t.depth));
    if (!BuildRegTargetResources(device, t, key, w, h, color_format)) {
      g_reg_targets.erase(it);
      return nullptr;
    }
    return &t;
  }
  if (g_reg_failed) {
    return nullptr;
  }
  // Now that the MSAA bracket no longer funnels the scene into one shared
  // target, EVERY guest surface asks for its own - so the two ways this can come
  // up empty are worth naming. A silent nullptr here reads downstream as "the
  // resolve had no source", which is the symptom we just spent a day chasing.
  if ((g_reg_rtv_freed.empty() && g_reg_rtv_next >= kRegMaxTargets) ||
      (g_reg_dsv_freed.empty() && g_reg_dsv_next >= kRegMaxTargets)) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("[native-scene] target registry FULL at {} surfaces - {:#x} {}x{} and every "
                   "later surface renders nowhere",
                   kRegMaxTargets, key, w, h);
    }
    return nullptr;
  }
  if (w == 0 || h == 0) {
    static std::uint32_t unsized = 0;
    if (unsized < 8) {
      ++unsized;
      REXLOG_WARN("[native-scene] surface {:#x} has no size yet - no target, draws dropped", key);
    }
    return nullptr;
  }
  NativeTarget t{};
  t.rtv_index = TakeIndex(g_reg_rtv_freed, g_reg_rtv_next);
  t.dsv_index = TakeIndex(g_reg_dsv_freed, g_reg_dsv_next);
  t.srv_slot = g_reg_srv_freed.empty() ? dpour_tex::kInvalidSlot : g_reg_srv_freed.back();
  if (!g_reg_srv_freed.empty()) {
    g_reg_srv_freed.pop_back();
  }
  if (!BuildRegTargetResources(device, t, key, w, h, color_format)) {
    // Hand the indices back rather than burning them: a build that failed on a
    // transient allocation would otherwise eat a registry slot per attempt.
    g_reg_rtv_freed.push_back(t.rtv_index);
    g_reg_dsv_freed.push_back(t.dsv_index);
    if (t.srv_slot != dpour_tex::kInvalidSlot) {
      g_reg_srv_freed.push_back(t.srv_slot);
    }
    return nullptr;
  }
  auto [ins, ok] = g_reg_targets.emplace(key, std::move(t));
  REXLOG_INFO("[native-scene] target registered: surface {:#x} {}x{} (srv {})", key, w, h,
              ins->second.srv_slot);
  return &ins->second;
}

// The guest has retired this surface, so its target and its three descriptor
// indices go back into circulation - the DestructResource role of the reference
// (video.cpp:708-718), which frees a GuestSurface's descriptor the moment the
// resource is destructed.
//
// The resources themselves are destroyed DEFERRED, never here: the GPU may still
// be reading this target for a frame already submitted. That is the reference's
// rule too - ProcDestructResource parks the resource in g_tempResources[g_frame]
// and only the frame boundary actually releases it.
void RetireRegTarget(std::uint32_t key) {
  const auto it = g_reg_targets.find(key);
  if (it == g_reg_targets.end()) {
    return;
  }
  NativeTarget& t = it->second;
  DestroyDeferred(std::move(t.color));
  DestroyDeferred(std::move(t.depth));
  g_reg_rtv_freed.push_back(t.rtv_index);
  g_reg_dsv_freed.push_back(t.dsv_index);
  if (t.srv_slot != dpour_tex::kInvalidSlot) {
    g_reg_srv_freed.push_back(t.srv_slot);
  }
  g_reg_targets.erase(it);
  {
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    g_surface_srv.erase(key);
    // A link pointing at a surface that no longer exists would otherwise serve
    // the next surface that lands on this address.
    for (auto lit = g_texture_link.begin(); lit != g_texture_link.end();) {
      lit = lit->second == key ? g_texture_link.erase(lit) : std::next(lit);
    }
  }
  static std::uint32_t logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("[native-scene] target retired: surface {:#x} (registry now {})", key,
                g_reg_targets.size());
  }
}

// (The scene-injection pipeline was here: render our target, read it back,
// re-encode it into the guest texture's own 7e3/tiled/endian layout on a
// worker thread, and memcpy it over the game's resolve. ~100 lines, a
// readback buffer, a condition variable and a worker thread - all
// unreachable: the worker was never started and the injection counter never
// moved. It solved "how do we hand our image back to the emulated pipeline",
// a question the references never face because there is no emulated pipeline
// to hand anything to.)
std::atomic<std::uint64_t> g_injections{0};

bool InjectEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("DPOUR_NR_DRAW_INJECT");
    return v != nullptr && v[0] != 0 && v[0] != '0';
  }();
  return on;
}

// The viewport the guest set most recently: what a draw being captured right now
// renders through, and so what its half-pixel offset must be based on.
std::atomic<std::uint32_t> g_view_w{kDefaultWidth};
std::atomic<std::uint32_t> g_view_h{kDefaultHeight};
// ... and the published pass's, which sizes the native target.
std::atomic<std::uint32_t> g_published_w{kDefaultWidth};
std::atomic<std::uint32_t> g_published_h{kDefaultHeight};

std::atomic<std::uint64_t> g_frame{0};
// The GUEST frame number the GPU has finished with. Not a fence value: the
// constant ring recycles its slices by frame, and comparing a frame index
// against a fence counter would either recycle a slice the GPU is still reading
// or never recycle one at all.
std::atomic<std::uint64_t> g_gpu_completed{0};
std::uint64_t g_published_frame = 0;

// Constant snapshots are reused while the bank has not changed: consecutive
// draws of one material share their pixel constants, and the revision counters
// say so exactly.
std::uint64_t g_vs_rev_cached = ~0ull;
std::uint64_t g_ps_rev_cached = ~0ull;
// The last staged constant bank and its size, so consecutive draws sharing a
// bank share one staging allocation. This is the reference's dirty-state idea
// (SetDirtyValue / g_dirtyStates.vertexShaderConstants) at capture: the bank is
// re-staged only when its contents actually changed.
std::uint32_t g_vs_cb_cached = kNoStage;
std::uint32_t g_ps_cb_cached = kNoStage;
std::uint32_t g_vs_cb_bytes = 0;
std::uint32_t g_ps_cb_bytes = 0;
std::uint64_t g_cache_frame = ~0ull;

// Diagnostics
std::atomic<std::uint64_t> g_seen{0};
std::atomic<std::uint64_t> g_drawn{0};
std::atomic<std::uint64_t> g_no_device{0};
std::atomic<std::uint64_t> g_no_shader{0};
std::atomic<std::uint64_t> g_no_layout{0};
std::atomic<std::uint64_t> g_no_pso{0};
std::atomic<std::uint64_t> g_no_topology{0};
// Split by reason: "the buffers failed" was true of 96% of the world's draws
// and said nothing about which of the five different things went wrong.
std::atomic<std::uint64_t> g_no_range{0};    // no usable vertex range from the RHI
std::atomic<std::uint64_t> g_no_stream{0};   // a stream the layout needs is not bound
std::atomic<std::uint64_t> g_no_vbobj{0};    // vertex buffer object unreadable
std::atomic<std::uint64_t> g_no_vbdata{0};   // its payload could not be uploaded
std::atomic<std::uint64_t> g_no_ibobj{0};
std::atomic<std::uint64_t> g_no_ibdata{0};
std::atomic<std::uint64_t> g_no_constants{0};
std::atomic<std::uint64_t> g_overflow{0};
std::atomic<std::uint64_t> g_other_pass{0};
// How long capture costs the guest render thread. Guessing at this twice was
// enough: the number belongs in the log next to everything else.
std::atomic<std::uint64_t> g_capture_ticks{0};
std::atomic<std::uint64_t> g_capture_frames{0};
std::atomic<std::uint64_t> g_render_ticks{0};
std::atomic<std::uint64_t> g_render_frames{0};
// Whether the buffer's own size could be read out of its fetch constant. If this
// is mostly "unknown" the probe offsets are wrong and everything downstream is
// running on the unreliable range hint instead.
std::atomic<std::uint64_t> g_size_known{0};
std::atomic<std::uint64_t> g_size_unknown{0};

std::int64_t TickNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

double TicksToMs(std::uint64_t ticks) {
  static const double scale = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 / static_cast<double>(f.QuadPart);
  }();
  return static_cast<double>(ticks) * scale;
}
std::atomic<bool> g_logged_first{false};

// --- render-thread resources -------------------------------------------------
ComPtr<ID3D12Resource> g_color;
ComPtr<ID3D12Resource> g_depth;
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
ComPtr<ID3D12DescriptorHeap> g_dsv_heap;
ComPtr<ID3D12DescriptorHeap> g_srv_heap;  // colour + depth, for the composite
ComPtr<ID3D12RootSignature> g_comp_root;
ComPtr<ID3D12PipelineState> g_comp_pso;
// The guest-output path (skate3-sdk NativeGuestOutputRenderer): the same
// composite, drawn OPAQUELY into our own R10G10B10A2 texture, which the
// registered callback then copies over the presenter's guest output image -
// the emulated frame is REPLACED, not painted over. Returning false from the
// callback (no frame yet, size mismatch, sticky failure) shows the emulated
// frame instead - the reference fallback discipline.
ComPtr<ID3D12PipelineState> g_comp_pso_guestout;
// THE COMPOSITE DRAWS STRAIGHT INTO THE PRESENTER'S GUEST OUTPUT.
//
// It used to render into a texture of our own and let the callback CopyResource
// it over the guest output - which forced the two to be the same size, and any
// resolution_scale > 1 (the presenter then asks for 2560x1440 while we draw
// 1280x720) made every single frame fail that equality and yield to the
// emulated output. Everything looked healthy in the log and the screen showed
// the emulator; it cost most of a day.
//
// skate3 has no such texture and no such test: it renders into
// context.guest_output as a render target and ends with
// Barrier(guest_output, kRenderTarget, kGuestOutput)
// (skate3_native_scene_gpu.cpp:11546). A full-screen triangle writes whatever
// size the presenter asks for, so upscaling and FSR ride on top of us instead
// of shutting us off.
//
// (True supersampling still needs OUR targets built at the output size, the way
// skate3's EnsureOutputSizedTargets does; this only removes the size gate.)
ComPtr<ID3D12DescriptorHeap> g_guestout_rtv_heap;  // kPassFrames RTVs, ring
// The guest output resource of the callback currently running, and the size it
// asked for. Written by the callback, read by the composite it calls into.
std::atomic<std::uint32_t> g_guestout_want_w{0};
std::atomic<std::uint32_t> g_guestout_want_h{0};
std::atomic<bool> g_guestout_failed{false};
// What the composite showed last frame (telemetry: key + how many draws the
// chosen source target actually received).
std::atomic<std::uint32_t> g_comp_source_key{0};
std::atomic<std::uint32_t> g_comp_source_draws{0};
// DOES ANY OF THIS ACTUALLY LAND ON SCREEN?
//
// Every diagnosis so far has gone through a screenshot and a walk back into the
// game, and the counters that looked like answers were not: "2262 draws wrote
// depth" counts what the DEPTH STATE said, not what the rasteriser did. An
// occlusion query around the scene pass counts pixels that actually passed, and
// puts the number in the log where it costs nobody a trip.
ComPtr<ID3D12QueryHeap> g_query_heap;
ComPtr<ID3D12Resource> g_query_readback;
std::atomic<std::uint64_t> g_scene_pixels{0};
std::atomic<std::uint64_t> g_scene_pixel_frames{0};
std::atomic<std::uint32_t> g_scene_issued{0};
std::atomic<std::uint32_t> g_scene_issued_shaded{0};
// Replays that ended with the presenter's image bound and handed back - i.e.
// frames whose pixels are OURS. The difference between this and the replay
// count is the number of frames the presenter had to fill some other way.
std::atomic<std::uint64_t> g_guestout_direct_frames{0};

// THE COMMAND PROCESSOR'S OWN SUBMISSION COUNTERS, LATCHED PER CALLBACK.
//
// nrhi::Device::CurrentSubmission()/CompletedSubmission() are, in the SDK's own
// words, "the basis of all lifetime/readback gating" (native_rhi.h:545). The
// callback reads them and stores them here so code that cannot see the context
// - target rebuilds, the deferred-release list - answers "is the GPU done with
// this" from the same source instead of from a private counter with nothing
// behind it.
std::atomic<std::uint64_t> g_submission_current{0};
std::atomic<std::uint64_t> g_submission_completed{0};

// DestroyDeferred, by hand, for the resources this module still owns as raw
// D3D12 (the nrhi Device offers it for its own objects). A resource released
// while a recorded-but-unfinished submission still reads it is the plainest
// use-after-free there is, and it does not fault at the call site - it faults
// later, inside the driver, as a device removal with no line number attached.
struct RetiredResource {
  ComPtr<ID3D12Resource> res;
  std::uint64_t submission;
};
std::mutex g_retire_mutex;
std::vector<RetiredResource> g_retired;

void DestroyDeferred(ComPtr<ID3D12Resource>&& res) {
  if (!res) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  g_retired.push_back(RetiredResource{std::move(res),
                                      g_submission_current.load(std::memory_order_relaxed)});
}

// Release what the GPU has finished with. Cheap and self-limiting: the list is
// empty except in the few frames after a target rebuild or a resize.
void FlushRetired(std::uint64_t completed) {
  std::lock_guard<std::mutex> lock(g_retire_mutex);
  if (g_retired.empty()) {
    return;
  }
  g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                 [completed](const RetiredResource& r) {
                                   return r.submission <= completed;
                                 }),
                  g_retired.end());
}

std::uint64_t g_alloc_fence[kPassFrames]{};
// Guest frame each in-flight submission belongs to, so "the GPU is done with
// frame N" can be answered in the same units the constant ring counts in.
std::uint64_t g_alloc_frame[kPassFrames]{};
std::uint32_t g_pass_index = 0;

// Publish "the GPU has finished this guest frame" - from the CALLBACK, every
// frame, not only from the tail of a successful replay.
//
// THE LATCH THIS BREAKS: the constant ring only rotates a slice once this value
// passes the frame that wrote it (pipeline::BeginFrame). Updating it solely at
// the end of the replay means any early return freezes it - and one of those
// early returns is "nothing was published", which is exactly what a
// ring-starved frame produces. No constants -> no draws -> nothing published ->
// this never runs -> the ring is never reused. A one-way door: the native path
// went dark for the rest of the session while every log line still read healthy.
void UpdateGpuCompleted(std::uint64_t completed) {
  // CONSERVATIVE: a guest frame counts as finished only when NOTHING still in
  // flight is holding it.
  //
  // Taking the newest finished slot was wrong the moment a frame could be
  // replayed more than once. Two submissions can carry the same guest frame;
  // the first completes, the frame is declared done, the constant ring rotates
  // its slice - and the second submission is still reading it. On screen that is
  // text that comes apart one glyph at a time while the geometry stays put,
  // because the vertices are fine and the constants underneath them now belong
  // to a later frame.
  std::uint64_t newest_done = 0;
  std::uint64_t oldest_in_flight = 0;
  for (std::uint32_t i = 0; i < kPassFrames; ++i) {
    if (g_alloc_fence[i] == 0) {
      continue;
    }
    if (g_alloc_fence[i] <= completed) {
      newest_done = std::max(newest_done, g_alloc_frame[i]);
    } else if (oldest_in_flight == 0 || g_alloc_frame[i] < oldest_in_flight) {
      oldest_in_flight = g_alloc_frame[i];
    }
  }
  const std::uint64_t done_frame =
      oldest_in_flight != 0 ? (oldest_in_flight > 0 ? oldest_in_flight - 1 : 0)
                            : newest_done;
  if (done_frame > g_gpu_completed.load(std::memory_order_relaxed)) {
    g_gpu_completed.store(done_frame, std::memory_order_relaxed);
  }
}
std::uint32_t g_target_w = 0;
std::uint32_t g_target_h = 0;
bool g_targets_failed = false;
bool g_comp_failed = false;
// The game renders with a reversed depth buffer when GInvertZ is set; the
// comparison functions in the register are already the flipped ones, so the only
// thing left for us is to clear to the other end of the range.
std::atomic<int> g_invert_z{-1};
float g_depth_clear = 1.0f;  // (declared above for the target registry)

// The composite. Our pass draws only what the game's scene pass drew, so
// everything it did NOT draw - the UI, the menus, the movies, every pass we do
// not reproduce yet - has to keep coming from the frame underneath. Coverage is
// taken from the depth buffer plus "did anything write colour here", because the
// target is cleared to exactly zero.
constexpr char kCompositeSrc[] = R"HLSL(
Texture2D<float4> uColor : register(t0);
Texture2D<float>  uDepth : register(t1);
cbuffer Comp : register(b0) {
  float uDepthClear;
  uint  uOpaque;
  uint  uDepthView;
  float uAmplify;   // 0 = off. Multiplies colour, to tell "very dark" from "exactly zero".
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
  VSOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  uint2 dim;
  uColor.GetDimensions(dim.x, dim.y);
  int2 p = int2(i.uv * float2(dim));
  float4 c = uColor.Load(int3(p, 0));
  float  d = uDepth.Load(int3(p, 0));
  if (uDepthView != 0) {
    // Show OUR depth buffer instead of our colour. This answers the one question
    // a black picture cannot: whether the geometry, the vertex shaders and the
    // transform are right, with the materials taken out of it entirely.
    //
    // It lives here, in the composite, rather than as a replacement pixel
    // shader, because the game's shaders are DXIL and a locally compiled one is
    // DXBC - mixing the two in a pipeline is rejected outright, which is exactly
    // how an earlier attempt at this managed to break every single draw.
    // No division by (1 - clear): the clear value IS 1.0 when the game runs a
    // normal depth buffer, and that produced a NaN which saturated to a solid
    // orange screen - a value that looks exactly like "everything is at maximum
    // depth" and says nothing at all.
    float n = saturate(abs(d - uDepthClear));
    if (n <= 0.00001) discard;
    // A depth buffer's useful range is squeezed into a sliver near the camera,
    // so a linear ramp is almost flat. The gamma spreads it out and the contour
    // bands make surfaces readable even where the ramp is still flat.
    float g = pow(n, 0.30);
    float band = frac(n * 64.0);
    return float4(g, g * 0.6 + band * 0.25, 1.0 - g, 1.0);
  }
  if (uOpaque == 0) {
    // Coverage is what we actually PAINTED, not what we touched. UE3's depth
    // prepass writes depth over the whole screen and no colour at all, so taking
    // depth as coverage declared every pixel covered and composited our empty
    // black target over the entire frame - a black screen.
    if (!any(c.rgb > 0.0)) discard;
  }
  if (uAmplify < 0.0) {
    // CLASSIFY what the shaders actually wrote. "Nothing on screen" has been
    // read as "the target is zero", but the test for that - any(c.rgb > 0) - is
    // equally false for negatives and for NaN, and those three have completely
    // different causes. Red = NaN, green = negative, blue = positive (however
    // small), grey = exactly zero.
    if (any(isnan(c.rgb))) return float4(1.0, 0.0, 0.0, 1.0);
    if (any(c.rgb < 0.0))  return float4(0.0, 1.0, 0.0, 1.0);
    if (any(c.rgb > 0.0))  return float4(0.0, 0.0, 1.0, 1.0);
    // RGB is zero - but was the pixel WRITTEN at all? The target is cleared to
    // alpha 0, so an alpha that is no longer zero means a pixel shader ran and
    // deliberately produced black; alpha still zero means nothing ever touched
    // this pixel. Those are opposite bugs and the picture is identical.
    if (c.a != 0.0) return float4(1.0, 1.0, 0.0, 1.0);  // yellow: written black
    return float4(0.15, 0.15, 0.15, 1.0);               // grey: never written
  }
  if (uAmplify > 0.0) {
    // A picture that is merely very dark and one that is exactly zero look the
    // same on screen and have completely different causes: the first is a scale
    // problem (the scene colour is written with an EDRAM exponent bias), the
    // second means the shaders wrote nothing at all. Multiplying separates them
    // in one look. Magenta marks pixels that are exactly zero.
    if (!any(c.rgb > 0.0)) return float4(0.6, 0.0, 0.6, 1.0);
    return float4(saturate(c.rgb * uAmplify), 1.0);
  }
  return float4(c.rgb, 1.0);
}
)HLSL";

bool EnsureComposite(ID3D12Device* device) {
  if (g_comp_pso) {
    return true;
  }
  if (g_comp_failed || device == nullptr) {
    return false;
  }
  g_comp_failed = true;

  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(D3DCompile(kCompositeSrc, sizeof(kCompositeSrc) - 1, "dpour_scene_composite", nullptr,
                        nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
    REXLOG_ERROR("[native-scene] composite VS compile failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  err.Reset();
  if (FAILED(D3DCompile(kCompositeSrc, sizeof(kCompositeSrc) - 1, "dpour_scene_composite", nullptr,
                        nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
    REXLOG_ERROR("[native-scene] composite PS compile failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  range.BaseShaderRegister = 0;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 0;
  params[1].Constants.Num32BitValues = 4;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 2;
  rs.pParameters = params;
  rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, rs_err;
  if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rs_err)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_comp_root)))) {
    REXLOG_ERROR("[native-scene] composite root signature failed");
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = g_comp_root.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = kBackBufferFormat;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_comp_pso)))) {
    REXLOG_ERROR("[native-scene] composite PSO failed");
    return false;
  }
  // The same composite against the guest-output format, for the replacement
  // path (the presenter's guest output is R10G10B10A2, not the swap format).
  desc.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_comp_pso_guestout)))) {
    REXLOG_ERROR("[native-scene] guest-output composite PSO failed");
    g_guestout_failed.store(true, std::memory_order_relaxed);
  }
  g_comp_failed = false;
  return true;
}

// --- the real resolve copy ---------------------------------------------------
//
// The reference's ExecutePendingStretchRectCommands, in the shape our pipeline
// can carry: a full-screen triangle from the source surface's target into a
// texture of ours, scaling by 2^-ColorExpBias on the way. Point sampling, like
// the Xenos resolve itself - source and destination are the same size, and a
// filtered copy would blur a 1:1 transfer for nothing.
const char kResolveSrc[] = R"HLSL(
Texture2D<float4> uSrc : register(t0);
cbuffer Params : register(b0) { float uScale; float3 uPad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
  VSOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  uint2 dim;
  uSrc.GetDimensions(dim.x, dim.y);
  int2 p = int2(i.uv * float2(dim));
  float4 c = uSrc.Load(int3(p, 0));
  // Colour only. The bias is described throughout the game's source as a COLOUR
  // exponent bias, and the destination formats it is used with (PF_FloatRGB,
  // A2B10G10R10) carry no alpha worth scaling.
  return float4(c.rgb * uScale, c.a);
}
)HLSL";

ComPtr<ID3D12RootSignature> g_resolve_root;
ComPtr<ID3D12PipelineState> g_resolve_pso;
bool g_resolve_failed = false;

bool EnsureResolvePipeline(ID3D12Device* device) {
  if (g_resolve_pso) {
    return true;
  }
  if (g_resolve_failed || device == nullptr) {
    return false;
  }
  g_resolve_failed = true;  // sticky until it gets all the way through

  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(D3DCompile(kResolveSrc, sizeof(kResolveSrc) - 1, "dpour_resolve", nullptr, nullptr,
                        "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
    REXLOG_ERROR("[native-scene] resolve VS compile failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  err.Reset();
  if (FAILED(D3DCompile(kResolveSrc, sizeof(kResolveSrc) - 1, "dpour_resolve", nullptr, nullptr,
                        "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
    REXLOG_ERROR("[native-scene] resolve PS compile failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  // One SRV, one root constant. Deliberately its own signature rather than the
  // composite's: that one declares a table of TWO descriptors, and pointing it
  // into the bindless heap would put whatever happens to occupy the next slot
  // into the pipeline as well.
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 0;
  params[1].Constants.Num32BitValues = 4;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 2;
  rs.pParameters = params;
  rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, rs_err;
  if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rs_err)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_resolve_root)))) {
    REXLOG_ERROR("[native-scene] resolve root signature failed");
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = g_resolve_root.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = kColorFormat;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_resolve_pso)))) {
    REXLOG_ERROR("[native-scene] resolve PSO failed - resolve copies off");
    return false;
  }
  g_resolve_failed = false;
  REXLOG_INFO("[native-scene] resolve copy pipeline ready");
  return true;
}

// The destination texture for one resolved guest texture object. Created once
// and kept: the game resolves into the same handful of textures every frame.
ResolveTexture* EnsureResolveTexture(ID3D12Device* device, std::uint32_t d3d_tex, std::uint32_t w,
                                     std::uint32_t h) {
  auto it = g_resolve_textures.find(d3d_tex);
  if (it != g_resolve_textures.end()) {
    return it->second.w == w && it->second.h == h ? &it->second : nullptr;
  }
  if (g_resolve_failed || g_resolve_rtv_next >= kResolveMaxTextures || w == 0 || h == 0) {
    return nullptr;
  }
  if (!g_resolve_rtv_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kResolveMaxTextures;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_resolve_rtv_heap)))) {
      REXLOG_ERROR("[native-scene] resolve RTV heap failed - resolve copies off");
      g_resolve_failed = true;
      return nullptr;
    }
  }
  ResolveTexture rt{};
  rt.w = w;
  rt.h = h;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = w;
  td.Height = h;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = kColorFormat;
  td.SampleDesc.Count = 1;
  td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv{};
  cv.Format = kColorFormat;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                             IID_PPV_ARGS(&rt.tex)))) {
    REXLOG_ERROR("[native-scene] resolve texture {:#x} {}x{} failed - resolve copies off", d3d_tex,
                 w, h);
    g_resolve_failed = true;
    return nullptr;
  }
  {
    wchar_t n[64];
    std::swprintf(n, 64, L"dpour.resolve.%08x", d3d_tex);
    rt.tex->SetName(n);
  }
  rt.srv_slot = dpour_tex::AllocDescriptorSlot();
  if (rt.srv_slot == dpour_tex::kInvalidSlot) {
    return nullptr;  // the bindless heap is full; the alias still works
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = kColorFormat;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE sh{};
  sh.ptr = static_cast<std::size_t>(dpour_tex::CpuHandleAt(rt.srv_slot));
  device->CreateShaderResourceView(rt.tex.Get(), &sv, sh);

  rt.rtv_index = g_resolve_rtv_next++;
  const std::uint32_t stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvh = g_resolve_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtvh.ptr += static_cast<std::size_t>(rt.rtv_index) * stride;
  device->CreateRenderTargetView(rt.tex.Get(), nullptr, rtvh);

  auto [ins, ok] = g_resolve_textures.emplace(d3d_tex, std::move(rt));
  {
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    g_resolve_srv[d3d_tex] = ins->second.srv_slot;
  }
  REXLOG_INFO("[native-scene] resolve copy target: tex {:#x} {}x{} (srv {})", d3d_tex, w, h,
              ins->second.srv_slot);
  return &ins->second;
}

bool g_targets_inverted = false;

bool EnsureTargets(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
  // The depth direction is only knowable once the game has actually drawn
  // something, and the first frame with anything to draw comes long before that.
  // So the targets are rebuilt if the answer changes - without this they were
  // created with the wrong clear value on frame one and kept it for the whole
  // session, which made every pixel fail the depth test and looked exactly like
  // broken shading. (Measured: 249263 draws compare GREATER, zero compare LESS.)
  const bool inverted = dpour_state::ObservedInvertedDepth();
  if (g_color && g_target_w == width && g_target_h == height && g_targets_inverted == inverted) {
    return true;
  }
  if (g_color && g_targets_inverted != inverted) {
    REXLOG_INFO("[native-scene] depth direction settled: {} - rebuilding targets",
                inverted ? "INVERTED (clear 0)" : "normal (clear 1)");
    g_targets_failed = false;
  }
  if (g_targets_failed || device == nullptr || width == 0 || height == 0) {
    return false;
  }
  g_targets_failed = true;
  // Taken from the comparison direction the game's own draws use, not from a
  // global whose address is disputed. Clearing to the wrong end makes every
  // pixel fail the depth test, and a scene that fails the depth test is
  // indistinguishable from a scene that is shaded black.
  g_targets_inverted = inverted;
  g_depth_clear = inverted ? 0.0f : 1.0f;

  // Anything still in flight referenced the old targets, so they outlive this
  // call by however long the command processor needs - the DestroyDeferred
  // contract, done by hand for the resources this module still owns as raw
  // D3D12. What stood here waited on OUR fence, which nothing has signalled
  // since the replay moved onto the command processor's list: the wait was a
  // no-op and the Reset() below freed targets a submission was still reading.
  DestroyDeferred(std::move(g_color));
  DestroyDeferred(std::move(g_depth));
  g_color.Reset();
  g_depth.Reset();

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kColorFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_CLEAR_VALUE clear_color{};
  clear_color.Format = kColorFormat;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             &clear_color, IID_PPV_ARGS(&g_color)))) {
    REXLOG_ERROR("[native-scene] colour target creation failed");
    return false;
  }

  desc.Format = kDepthResFormat;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear_depth{};
  clear_depth.Format = kDepthFormat;
  clear_depth.DepthStencil.Depth = g_depth_clear;
  // Both targets start in the state the render loop transitions OUT of, so the
  // very first frame's barrier describes the state the resource is actually in.
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             &clear_depth, IID_PPV_ARGS(&g_depth)))) {
    REXLOG_ERROR("[native-scene] depth target creation failed");
    return false;
  }

  if (!g_rtv_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    h.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&g_rtv_heap)))) {
      return false;
    }
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&g_dsv_heap)))) {
      return false;
    }
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // One [colour, depth] descriptor pair PER FRAME IN FLIGHT. The composite
    // source is picked per frame now, and rewriting a single shared slot while
    // an in-flight frame still reads it is a descriptor race - the white
    // screen followed by DEVICE_REMOVED the moment the menu started flipping
    // sources every frame.
    h.NumDescriptors = kPassFrames * 2;
    h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&g_srv_heap)))) {
      return false;
    }
  }

  D3D12_RENDER_TARGET_VIEW_DESC rtv{};
  rtv.Format = kColorFormat;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(g_color.Get(), &rtv,
                                 g_rtv_heap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = kDepthFormat;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(g_depth.Get(), &dsv,
                                 g_dsv_heap->GetCPUDescriptorHandleForHeapStart());

  const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE srv = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  sd.Format = kColorFormat;
  device->CreateShaderResourceView(g_color.Get(), &sd, srv);
  srv.ptr += inc;
  sd.Format = kDepthSrvFormat;
  device->CreateShaderResourceView(g_depth.Get(), &sd, srv);

  // (The allocator ring, the command list and the fence that used to be built
  // here went with the second timeline they served. Nothing referenced them
  // after the replay moved onto the command processor's list; they were still
  // being created every time the targets were.)

  g_target_w = width;
  g_target_h = height;
  g_targets_failed = false;
  REXLOG_INFO("[native-scene] targets ready: {}x{} colour {} + depth (clear {})", width, height,
              static_cast<int>(kColorFormat), g_depth_clear);
  return true;
}

// Which pass slot the current render target belongs to.
std::uint32_t PassSlotFor(std::uint32_t color, std::uint32_t depth) {
  // Grouped by the COLOUR surface alone. Downpour renders the scene in EDRAM
  // tiles and rebinds depth between its passes, so keying on the (colour, depth)
  // pair split one logical target into a dozen "passes" and published whichever
  // fragment happened to be largest - 106 draws out of 61147 reproduced, which
  // is why almost nothing appeared.
  (void)depth;
  for (std::uint32_t i = 0; i < g_pass_count; ++i) {
    if (g_passes[i].color == color) {
      return i;
    }
  }
  if (g_pass_count < kMaxPasses) {
    g_passes[g_pass_count].color = color;
    g_passes[g_pass_count].depth = depth;
    g_passes[g_pass_count].draws = 0;
    return g_pass_count++;
  }
  // NOT slot 0. An unroutable draw is dropped, not redirected: slot 0 is a real
  // surface (the back buffer, most frames), and pouring the overflow into it
  // renders shadow maps and post-process passes into the composited image.
  static bool warned = false;
  if (!warned) {
    warned = true;
    REXLOG_ERROR("[native-scene] pass table FULL at {} surfaces - surface {:#x} and later ones "
                 "draw nowhere this frame",
                 kMaxPasses, color);
  }
  return kMaxPasses;
}

}  // namespace

bool Enabled() {
  // Its OWN flag, deliberately not DPOUR_NR_SHADERS. That one only means "load
  // the translated shader cache", which changes nothing on screen and is what
  // the dry-run verification runs with - a verification pass that promises the
  // picture is untouched must not start drawing because a draw path was added
  // behind the same switch.
  static const bool on = EnvOn("DPOUR_NR_DRAW");
  return on;
}

bool NativeOnly() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_ONLY");
  return on;
}

// DPOUR_NR_RESOLVE_COPY - the real resolve copy, DEFAULT OFF.
//
// Off, a resolve only records the link and the sampler is handed the source
// surface's own target (the alias). On, the surface is COPIED into a texture of
// ours, scaled by 2^-ColorExpBias, and that texture is what the sampler gets -
// which is what the game's own resolve does, un-biasing included.
//
// Default off on purpose, and not as timidity: it changes every pixel the game
// composes from a resolved texture, and it lands on top of an already large
// structural change (the composite moving into the presenter's image) that has
// not been looked at yet. Two unverified visual changes in one build means a
// wrong picture tells us nothing about which one did it - which is exactly how
// the last two days went. One flag, one run, one answer.
bool ResolveCopyEnabled() {
  static const bool on = EnvOn("DPOUR_NR_RESOLVE_COPY");
  return on;
}

// DPOUR_NR_KEEP_COLOR_BIAS - keep the game's EDRAM exponent bias instead of
// neutralising it. Default OFF, i.e. the bias is neutralised.
//
// THE MARATHON MOVE, at the closest point we own. Marathon does not emulate the
// EDRAM tiling constraint - it hooks the guest function that decides it and
// returns 0 ("We return 0 to always disable tiling", video.cpp SurfaceSize).
// Downpour's colour exponent bias is the same kind of console artefact: the
// shaders multiply their output by 2^bias and the resolve divides it back out,
// purely because EDRAM is 10 MB and the scene needs the range.
//
// It reaches the shaders through one place, and the game's own source says
// which (XeD3DCommands.cpp:536):
//
//     void RHISetRenderTargetBias(FLOAT ColorBias) {
//       FVector4 ColorBiasParameter(ColorBias, 0, 0, 0);
//       GDirect3DDevice->SetPixelShaderConstantF(PSR_ColorBiasFactor, ..., 1);
//     }
//
// PSR_ColorBiasFactor is register 0 of the PIXEL constant bank - a bank we
// already stage for every draw. Writing 1.0 there is not an invention either:
// the game itself does exactly that on the paths that want no bias
// (XeD3DCommands.cpp:1603, RHISetRenderTargetBias(1)).
//
// With no multiply there is nothing to divide back, so our resolve copies stop
// scaling too and the whole chain is consistent with itself. What it replaces:
// half the resolve copies were not happening (7 of 17 in the last run), so half
// the samples stayed 8x too bright - which is the white screen.
bool KeepColorBias() {
  static const bool on = EnvOn("DPOUR_NR_KEEP_COLOR_BIAS");
  return on;
}

// DPOUR_NR_PS_C0 - the value written into PSR_ColorBiasFactor.x when the bias is
// neutralised. Default 1.0, the game's own no-bias value. It is a knob only so
// the multiply can be PROVEN to reach the shader: the pixel bank is uploaded on
// the CPU and read on the GPU, and nothing between the two is observable from
// here. Set it to something extreme and the picture either changes - the bank
// arrives - or it does not, and the constants never reach the shader at all.
float PixelC0Value() {
  static const float v = [] {
    const char* s = std::getenv("DPOUR_NR_PS_C0");
    if (s == nullptr || s[0] == '\0') {
      return 1.0f;
    }
    const double d = std::strtod(s, nullptr);
    return std::isfinite(d) ? static_cast<float>(d) : 1.0f;
  }();
  return v;
}

// DPOUR_NR_C0_PROBE - photograph the pixel constant bank at draw time.
//
// The classify probe put the defect between "the pixel shader ran" and "the
// pixel has colour", and the game's source names the only thing in that gap:
// every shader returns BiasColor(c) = float4(c.rgb * SCENE_COLOR_BIAS_FACTOR.x,
// c.a) (Common.usf:286). RGB scaled, alpha untouched - which is exactly the
// yellow the classifier reported. So c0.x is either zero or it is not, and this
// prints it rather than reasoning about it.
bool C0ProbeEnabled() {
  static const bool on = EnvOn("DPOUR_NR_C0_PROBE");
  return on;
}

bool SkipGuestDraws() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_SKIPGUEST");
  return on;
}

// Public face of OwnDeviceMode(), for the hooks in downpour_native_draws.cpp
// that have to drop the guest's packet writers.
bool OwnDevice() { return OwnDeviceMode(); }

// RHIClear, captured as a stream item the way the reference captures it
// (RenderCommandType::Clear). color_guest points at the FLinearColor (four
// big-endian floats); depth arrives engine-side, un-flipped - the GInvertZ
// flip lives inside the game's RHIClear and is mirrored at replay.
void OnClear(const std::uint8_t* base, bool clear_color, std::uint32_t color_guest,
             bool clear_depth, float depth) {
  if (!Enabled() || base == nullptr) {
    return;
  }
  PendingClear pc{};
  pc.clear_color = clear_color;
  pc.clear_depth = clear_depth;
  pc.depth = depth;
  if (clear_color && GuestAddrPlausible(color_guest) && ObjectReadable(base, color_guest, 16)) {
    for (int i = 0; i < 4; ++i) {
      const std::uint32_t bits = LoadBE32(base + color_guest + 4u * static_cast<std::uint32_t>(i));
      float f;
      std::memcpy(&f, &bits, 4);
      pc.rgba[i] = f;
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  pc.pass = g_pass_current;
  pc.draw_index = static_cast<std::uint32_t>(g_staging.size());
  if (g_clear_staging.size() < 128) {
    g_clear_staging.push_back(pc);
  }
}

bool UpAtApiLevel() {
  static const bool on = EnvOn("DPOUR_NR_UP_API") || OwnDeviceMode();
  return on;
}

namespace {
// Diagnostic gates, both default OFF.
//
// DPOUR_NR_DRAW_NODRAW does everything a frame normally does - captures, builds
// pipelines, uploads buffers and constants, clears the targets, composites -
// and issues no draw call. It splits "the plumbing is wrong" from "a shader or
// its geometry is wrong" in one run, which guessing between them cannot.
//
// DPOUR_NR_DRAW_MAXDRAWS caps how many draws a frame submits, so the same split
// can be bisected when the answer is "a shader".
bool NoDraw() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_NODRAW");
  return on;
}

bool LogSubmissions() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_LOG");
  return on;
}

bool NoCull() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_NOCULL");
  return on;
}

bool SceneListEnabled() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_SCENELIST");
  return on;
}

// One big-endian guest pointer, or 0 when the address is not a readable object.
std::uint32_t ReadGuestPtr(const std::uint8_t* base, std::uint32_t at) {
  if (base == nullptr || !GuestAddrPlausible(at) || !ObjectReadable(base, at, 4)) {
    return 0;
  }
  const std::uint32_t v = LoadBE32(base + at);
  return GuestAddrPlausible(v) ? v : 0;
}

// THE VERTEX-TRANSFORM PROBE (DPOUR_NR_DRAW_VSPROBE).
//
// The depth buffer of a scene that rasterises millions of pixels came out flat -
// no contour, no structure - which says the geometry reaching the rasteriser is
// degenerate, i.e. the failure is in the VERTEX stage and everything downstream
// (materials, textures, lightmaps) is being blamed for a consequence.
//
// Three things can do that, and this probe separates them by measurement rather
// than by argument:
//   the MATRIX   the shader's view-projection constant is not in the bank we
//                upload (we shadow two sources: the hooked setters and the
//                device's own register file, and neither has ever been proven to
//                carry it)
//   the LAYOUT   the input layout feeds the position element from the wrong
//                offset, stream or type
//   the DATA     the vertex bytes themselves are wrong (swap, stride, offset)
//
// RHISetViewParameters hands us the frame's real view-projection matrix, and
// FMeshDrawingPolicy::DrawMesh hands us the mesh's real LocalToWorld. Both are
// KNOWN GOOD - the wireframe mode drew the actual world with them. So the probe
// searches the whole 256-register bank for either matrix: if a shader's
// transform constants are there, the matrix is not the problem and the layout
// or the data is; if neither matrix appears anywhere, the bank we upload cannot
// possibly transform anything and nothing further downstream matters.
bool VsProbeEnabled() {
  static const bool on = EnvOn("DPOUR_NR_DRAW_VSPROBE");
  return on;
}

// Defined just below with the rest of the constant plumbing.
void SwapBankToHost(void* dst, const std::uint8_t* src, std::uint32_t bytes);

// The frame's view-projection, straight from RHISetViewParameters. Written on
// the guest render thread, read on the same thread inside the probe.
float g_probe_vp[16] = {};
bool g_have_probe_vp = false;

inline float LoadBEFloat(const std::uint8_t* p) {
  const std::uint32_t bits = LoadBE32(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Does a run of four registers hold this matrix? Both storage conventions are
// tried: UE3 sets FMatrix rows directly for some parameters and the transpose
// for others, and which one a given shader uses is not something to guess at.
bool MatrixAtRegister(const float* bank, std::uint32_t reg, const float m[16], bool transposed) {
  for (std::uint32_t r = 0; r < 4; ++r) {
    for (std::uint32_t c = 0; c < 4; ++c) {
      const float want = transposed ? m[c * 4 + r] : m[r * 4 + c];
      const float got = bank[(reg + r) * 4 + c];
      const float scale = std::fabs(want) > 1.0f ? std::fabs(want) : 1.0f;
      if (!(std::fabs(got - want) <= scale * 1e-3f)) {
        return false;
      }
    }
  }
  return true;
}

// Register the matrix starts at, or -1. `transposed` reports which convention
// matched.
int FindMatrix(const float* bank, const float m[16], bool& transposed) {
  for (std::uint32_t reg = 0; reg + 4 <= dpour_consts::kRegisters; ++reg) {
    if (MatrixAtRegister(bank, reg, m, false)) {
      transposed = false;
      return static_cast<int>(reg);
    }
    if (MatrixAtRegister(bank, reg, m, true)) {
      transposed = true;
      return static_cast<int>(reg);
    }
  }
  return -1;
}

void ReportBank(const char* which, const float* bank, const float vp[16], bool have_vp,
                const float l2w[16], bool have_l2w) {
  std::uint32_t nonzero = 0;
  std::uint32_t nonfinite = 0;
  for (std::uint32_t i = 0; i < dpour_consts::kRegisters * 4; ++i) {
    if (!std::isfinite(bank[i])) {
      ++nonfinite;
    } else if (bank[i] != 0.0f) {
      ++nonzero;
    }
  }
  REXLOG_INFO("[vsprobe] {} bank: {} non-zero floats, {} non-finite", which, nonzero, nonfinite);
  for (std::uint32_t r = 0; r < 8; ++r) {
    REXLOG_INFO("[vsprobe]   {} c{}: {:.4f} {:.4f} {:.4f} {:.4f}", which, r, bank[r * 4 + 0],
                bank[r * 4 + 1], bank[r * 4 + 2], bank[r * 4 + 3]);
  }
  if (have_vp) {
    bool t = false;
    const int reg = FindMatrix(bank, vp, t);
    if (reg >= 0) {
      REXLOG_INFO("[vsprobe]   {}: VIEW-PROJECTION found at c{} ({})", which, reg,
                  t ? "transposed" : "rows");
    } else {
      REXLOG_INFO("[vsprobe]   {}: view-projection NOT PRESENT anywhere in 256 registers", which);
    }
  }
  if (have_l2w) {
    bool t = false;
    const int reg = FindMatrix(bank, l2w, t);
    if (reg >= 0) {
      REXLOG_INFO("[vsprobe]   {}: LOCAL-TO-WORLD found at c{} ({})", which, reg,
                  t ? "transposed" : "rows");
    } else {
      REXLOG_INFO("[vsprobe]   {}: local-to-world NOT PRESENT anywhere in 256 registers", which);
    }
  }
}

// IS THE PIXEL BANK THE PIXEL BANK?
//
// The vertex side proved itself by accident: the hooked-setter shadow and the
// device register file agree on every register the hook ever saw and differ
// only where the hook saw nothing (c4/c5). That is the signature of a correct
// offset behind an incomplete shadow - and the same test settles the pixel
// bank, which has never been checked at all and is the prime suspect for
// shaders that output rgb exactly zero with a live alpha.
//
// If dev+6016 is the pixel register file, it must agree with the hooked shadow
// wherever the hook captured a write. If it disagrees, the offset is wrong and
// every material constant the shaders read is somebody else's memory.
void ReportPixelBanks(const std::uint8_t* base) {
  static float dev[dpour_consts::kRegisters * 4];
  static float hook[dpour_consts::kRegisters * 4];
  const std::uint8_t* dev_ps = dpour_consts::DevicePixelBank(base);
  if (dev_ps == nullptr) {
    REXLOG_INFO("[vsprobe]   PS: device bank unavailable");
    return;
  }
  SwapBankToHost(dev, dev_ps, dpour_consts::kDeviceBankBytes);
  std::memset(hook, 0, sizeof(hook));
  dpour_consts::CopyPixelBank(hook, dpour_consts::kBankBytes);

  std::uint32_t written = 0;
  std::uint32_t agree = 0;
  std::uint32_t disagree = 0;
  std::uint32_t first_bad = 0xFFFFFFFFu;
  for (std::uint32_t r = 0; r < dpour_consts::kRegisters; ++r) {
    bool any = false;
    for (std::uint32_t c = 0; c < 4; ++c) {
      if (hook[r * 4 + c] != 0.0f) {
        any = true;
      }
    }
    if (!any) {
      continue;  // the hook never wrote it, so it says nothing either way
    }
    ++written;
    bool same = true;
    for (std::uint32_t c = 0; c < 4; ++c) {
      const float a = hook[r * 4 + c];
      const float b = dev[r * 4 + c];
      const float scale = std::fabs(a) > 1.0f ? std::fabs(a) : 1.0f;
      if (!(std::fabs(a - b) <= scale * 1e-3f)) {
        same = false;
      }
    }
    if (same) {
      ++agree;
    } else {
      ++disagree;
      if (first_bad == 0xFFFFFFFFu) {
        first_bad = r;
      }
    }
  }
  std::uint32_t dev_nonzero = 0;
  std::uint32_t dev_nonfinite = 0;
  for (std::uint32_t i = 0; i < dpour_consts::kRegisters * 4; ++i) {
    if (!std::isfinite(dev[i])) {
      ++dev_nonfinite;
    } else if (dev[i] != 0.0f) {
      ++dev_nonzero;
    }
  }
  REXLOG_INFO("[vsprobe]   PS banks: hook wrote {} registers, {} agree with device, {} DISAGREE"
              " | device {} non-zero floats, {} non-finite",
              written, agree, disagree, dev_nonzero, dev_nonfinite);
  if (first_bad != 0xFFFFFFFFu) {
    REXLOG_INFO("[vsprobe]   PS first mismatch c{}: hook ({:.4f} {:.4f} {:.4f} {:.4f}) "
                "device ({:.4f} {:.4f} {:.4f} {:.4f})",
                first_bad, hook[first_bad * 4 + 0], hook[first_bad * 4 + 1],
                hook[first_bad * 4 + 2], hook[first_bad * 4 + 3], dev[first_bad * 4 + 0],
                dev[first_bad * 4 + 1], dev[first_bad * 4 + 2], dev[first_bad * 4 + 3]);
  }
  for (std::uint32_t r = 0; r < 6; ++r) {
    REXLOG_INFO("[vsprobe]   PS c{}: device ({:.4f} {:.4f} {:.4f} {:.4f}) hook ({:.4f} {:.4f} "
                "{:.4f} {:.4f})",
                r, dev[r * 4 + 0], dev[r * 4 + 1], dev[r * 4 + 2], dev[r * 4 + 3],
                hook[r * 4 + 0], hook[r * 4 + 1], hook[r * 4 + 2], hook[r * 4 + 3]);
  }
}

// row vector * matrix, the convention FMatrix is written in.
void TransformPoint(const float m[16], const float p[3], float out[4]) {
  for (int c = 0; c < 4; ++c) {
    out[c] = p[0] * m[0 * 4 + c] + p[1] * m[1 * 4 + c] + p[2] * m[2 * 4 + c] + m[3 * 4 + c];
  }
}

// Everything the vertex stage of one scene draw is made of, in one place: the
// two known-good matrices, both constant banks, the layout, and the actual
// vertices the draw reads - taken through the transform on the CPU so the clip
// coordinates can be compared against what the screen shows.
void ProbeVertexTransform(const std::uint8_t* base, const DrawDesc& d,
                          const dpour_decl::Layout& decl, std::uint32_t color_object,
                          std::uint32_t depth_object, const dpour_state::Pipeline& state) {
  // Real world geometry, not the first thing that happens to be shaded. The
  // first run of this probe caught two six-index sprite quads and told us
  // nothing about the level - a UE3 static mesh section is hundreds of indices.
  static std::uint32_t reported = 0;
  static std::uint32_t small_reported = 0;
  const bool big = d.index_count >= 300;
  if (!big && small_reported >= 1) {
    return;
  }
  if (reported >= 5) {
    return;
  }
  ++reported;
  if (!big) {
    ++small_reported;
  }

  float l2w[16] = {};
  bool have_l2w = false;
  if (const dpour_ue3::MeshItem* mesh = dpour_ue3::CurrentMesh()) {
    std::memcpy(l2w, mesh->local_to_world, sizeof(l2w));
    have_l2w = true;
  }

  REXLOG_INFO("[vsprobe] ===== scene draw: decl={:#x} elements={} indexed={} count={} start={} =====",
              d.decl_guest, decl.element_count, d.indexed ? 1 : 0, d.index_count, d.start_index);
  REXLOG_INFO("[vsprobe]   target: color={:#x} depth={:#x} | depth_func={} depth_write={} cull={}",
              color_object, depth_object, state.depth_func, state.depth_write ? 1 : 0,
              state.cull_mode);
  if (g_have_probe_vp) {
    for (int r = 0; r < 4; ++r) {
      REXLOG_INFO("[vsprobe]   VP row {}: {:.4f} {:.4f} {:.4f} {:.4f}", r, g_probe_vp[r * 4 + 0],
                  g_probe_vp[r * 4 + 1], g_probe_vp[r * 4 + 2], g_probe_vp[r * 4 + 3]);
    }
  } else {
    REXLOG_INFO("[vsprobe]   NO view-projection captured (RHISetViewParameters never ran)");
  }
  if (have_l2w) {
    REXLOG_INFO("[vsprobe]   L2W translation: {:.2f} {:.2f} {:.2f}", l2w[12], l2w[13], l2w[14]);
  }

  // The two candidate banks, converted exactly the way the uploaded ones are.
  static float dev_bank[dpour_consts::kRegisters * 4];
  static float hook_bank[dpour_consts::kRegisters * 4];
  bool have_dev = false;
  if (const std::uint8_t* dev = dpour_consts::DeviceVertexBank(base)) {
    SwapBankToHost(dev_bank, dev, dpour_consts::kDeviceBankBytes);
    ReportBank("device", dev_bank, g_probe_vp, g_have_probe_vp, l2w, have_l2w);
    have_dev = true;
  } else {
    REXLOG_INFO("[vsprobe]   device bank unavailable (GDirect3DDevice not up)");
  }
  std::memset(hook_bank, 0, sizeof(hook_bank));
  dpour_consts::CopyVertexBank(hook_bank, dpour_consts::kBankBytes);
  ReportBank("hooked", hook_bank, g_probe_vp, g_have_probe_vp, l2w, have_l2w);
  ReportPixelBanks(base);

  // UE3 DOES NOT TRANSFORM BY THE VIEW-PROJECTION ALONE.
  //
  // Downpour's levels run to tens of thousands of units from the origin, where
  // float world positions lose the precision a depth buffer needs, so UE3 uses
  // TranslatedWorldToClip: every position is first shifted by PreViewTranslation
  // (= -CameraPosition) and only then projected. Measured here: c0..c3 are the
  // matrix RHISetViewParameters carries, and c5 is that translation. Leaving it
  // out puts every vertex behind the camera (w < 0) - which is exactly the
  // "degenerate geometry" this probe was written to explain, and exactly what
  // the hooked-setter bank produces, because it never captures c5 at all.
  float pre_view[3] = {0.0f, 0.0f, 0.0f};
  const bool have_pre_view = have_dev;
  if (have_pre_view) {
    pre_view[0] = dev_bank[5 * 4 + 0];
    pre_view[1] = dev_bank[5 * 4 + 1];
    pre_view[2] = dev_bank[5 * 4 + 2];
    REXLOG_INFO("[vsprobe]   PreViewTranslation (c5): {:.2f} {:.2f} {:.2f}", pre_view[0],
                pre_view[1], pre_view[2]);
  }

  // The layout as the input assembler will see it.
  for (std::uint32_t e = 0; e < decl.element_count && e < 12; ++e) {
    REXLOG_INFO("[vsprobe]   element {}: stream={} offset={} type={} usage={}.{}", e,
                decl.elements[e].stream, decl.elements[e].offset, decl.elements[e].type,
                decl.elements[e].usage, decl.elements[e].usage_index);
  }
  if (!decl.position.valid()) {
    REXLOG_INFO("[vsprobe]   NO POSITION ELEMENT in this declaration");
    return;
  }
  const std::uint32_t s = decl.position.stream;
  REXLOG_INFO("[vsprobe]   position: stream={} offset={} type={} stride={} stream_offset={}", s,
              decl.position.offset, decl.position.type, d.stride[s], d.stream_offset[s]);

  BufferFetch vb;
  if (!ResolveBuffer(base, d.vb_guest[s], d.vb_d3d[s], vb)) {
    REXLOG_INFO("[vsprobe]   position stream has no resolvable buffer");
    return;
  }
  BufferFetch ib;
  const bool have_ib = d.indexed && ResolveBuffer(base, d.ib_guest, 0, ib);

  // Three vertices - one triangle - taken through the same transform the shader
  // is supposed to apply. A degenerate draw shows itself here: identical
  // positions, w at or below zero, or NDC far outside [-1,1].
  for (std::uint32_t k = 0; k < 3; ++k) {
    std::uint32_t index = k;
    if (have_ib) {
      const std::uint32_t at = ib.addr + (d.start_index + k) * 2u;
      if (!ObjectReadable(base, at, 2)) {
        REXLOG_INFO("[vsprobe]   index {} unreadable at {:#x}", k, at);
        return;
      }
      std::uint16_t raw;
      std::memcpy(&raw, base + at, 2);
      index = _byteswap_ushort(raw);
    }
    const std::uint32_t at =
        vb.addr + d.stream_offset[s] + index * d.stride[s] + decl.position.offset;
    if (!ObjectReadable(base, at, 16)) {
      REXLOG_INFO("[vsprobe]   vertex {} (index {}) unreadable at {:#x}", k, index, at);
      return;
    }
    float p[4] = {};
    dpour_decl::ReadElement(base + at, decl.position.type, p);
    REXLOG_INFO("[vsprobe]   v{} idx={} local ({:.3f} {:.3f} {:.3f} {:.3f})", k, index, p[0], p[1],
                p[2], p[3]);
    if (!g_have_probe_vp) {
      continue;
    }
    float world[4] = {p[0], p[1], p[2], 1.0f};
    if (have_l2w) {
      TransformPoint(l2w, p, world);
    }
    const float translated[3] = {world[0] + pre_view[0], world[1] + pre_view[1],
                                 world[2] + pre_view[2]};
    float clip[4];
    TransformPoint(g_probe_vp, translated, clip);
    if (clip[3] > 1e-6f) {
      const float nx = clip[0] / clip[3];
      const float ny = clip[1] / clip[3];
      const float nz = clip[2] / clip[3];
      const bool on_screen = nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f;
      REXLOG_INFO("[vsprobe]   v{} world ({:.1f} {:.1f} {:.1f}) w={:.2f} ndc ({:.3f} {:.3f} {:.3f}) {}",
                  k, world[0], world[1], world[2], clip[3], nx, ny, nz,
                  on_screen ? "ON SCREEN" : "off screen");
    } else {
      REXLOG_INFO("[vsprobe]   v{} world ({:.1f} {:.1f} {:.1f}) w={:.4f} - BEHIND CAMERA", k,
                  world[0], world[1], world[2], clip[3]);
    }
  }
}

// Big-endian guest floats to host order, a whole constant bank at a time.
void SwapBankToHost(void* dst, const std::uint8_t* src, std::uint32_t bytes) {
  auto* d = static_cast<std::uint32_t*>(dst);
  const std::uint32_t words = bytes / 4;
  for (std::uint32_t i = 0; i < words; ++i) {
    std::uint32_t v;
    std::memcpy(&v, src + i * 4, 4);
    d[i] = _byteswap_ulong(v);
  }
}

std::uint32_t MaxDrawsPerFrame() {
  static const std::uint32_t n = [] {
    const char* v = std::getenv("DPOUR_NR_DRAW_MAXDRAWS");
    const int parsed = (v != nullptr) ? std::atoi(v) : 0;
    return parsed > 0 ? static_cast<std::uint32_t>(parsed) : 0xFFFFFFFFu;
  }();
  return n;
}

// Defined further down with the UP capture machinery; the frame lifecycle
// functions above it are close points too.
void CloseUPPending();

std::atomic<std::uint64_t> g_up_seen{0};
std::atomic<std::uint64_t> g_up_captured{0};
std::atomic<std::uint64_t> g_up_dropped{0};
std::atomic<std::uint64_t> g_up_capped{0};
// UP draws captured since the last present. The staging list only clears when
// the guest presents, and a loading transition can run hundreds of guest
// iterations without presenting (the smoke log shows 351 resolves inside one
// 1.7-second "frame") - which accumulated thousands of full-screen blended
// quads into one replay and hung the GPU for longer than the TDR limit. A real
// UI frame uses ~70 UP draws; the cap only exists for the pathological window.
std::atomic<std::uint32_t> g_up_frame_count{0};
std::uint32_t MaxUPPerFrame() {
  static const std::uint32_t n = [] {
    const char* v = std::getenv("DPOUR_NR_DRAW_MAXUP");
    const int parsed = (v != nullptr) ? std::atoi(v) : 0;
    return parsed > 0 ? static_cast<std::uint32_t>(parsed) : 512u;
  }();
  return n;
}
// Isolation gates for the two step-2 additions, so a regression can be pinned
// to one of them with two smoke runs instead of a theory.
bool EnvFlag(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}
bool NoUPCapture() {
  static const bool on = EnvFlag("DPOUR_NR_DRAW_NOUP");
  return on;
}
// DPOUR_NR_DRAW_GUESTOUT: hand the finished native frame to the presenter's
// guest output through the skate3-sdk NativeGuestOutputRenderer hook - the
// screen then shows OUR frame (with per-frame fallback to the emulated one),
// instead of the overlay composite painted over the emulated frame.
bool GuestOutMode() {
  // Implied by own-device mode: if the guest's draws never reached the GPU,
  // ours is the only image there is, so it must be what the presenter shows.
  // Having to set two variables to get one coherent state is how a run ends up
  // half-migrated and impossible to read.
  static const bool on = EnvFlag("DPOUR_NR_DRAW_GUESTOUT") || OwnDeviceMode();
  return on;
}

// The registered NativeGuestOutputRenderer callback. Runs on the command
// processor's thread inside the guest-output refresh; everything it touches is
// atomics or resources this module never destroys.
bool GuestOutputRenderCallback(const rex::graphics::NativeGuestOutputRenderContext& ctx,
                               void* /*user*/) {
  namespace rg = rex::graphics;
  if (!Enabled() || !GuestOutMode() || g_guestout_failed.load(std::memory_order_relaxed) ||
      ctx.backend != rg::NativeGuestOutputBackend::kD3D12) {
    return false;
  }
  // Telemetry only now. It used to gate the whole path ("until our texture is
  // this size, yield"), which is the trap resolution_scale fell into; the
  // composite writes the presenter's image directly and takes any size.
  g_guestout_want_w.store(ctx.guest_output_width, std::memory_order_relaxed);
  g_guestout_want_h.store(ctx.guest_output_height, std::memory_order_relaxed);
  {
    static std::atomic<std::uint32_t> last_w{0};
    const std::uint32_t w = ctx.guest_output_width;
    if (last_w.exchange(w, std::memory_order_relaxed) != w) {
      REXLOG_INFO("[native-scene] guest output size: {}x{}", w, ctx.guest_output_height);
    }
  }

  // THE FRAME IS RECORDED HERE, on the command processor's own list - the
  // skate3 arrangement on this same SDK.
  //
  // The device comes from the context, not from the presenter. Depending on the
  // presenter to hand it over is what broke this path once already: the only
  // thing that registered this callback was the presenter's per-present entry,
  // so when that entry stopped drawing, nothing registered and the emulated
  // frame went to screen with the whole native path healthy behind it.
  ID3D12Resource* out_res = rg::d3d12::NativeRhiGetD3D12TextureResource(ctx.guest_output);
  auto* rec = rg::d3d12::NativeRhiGetDeferredCommandList(ctx.device);
  if (out_res == nullptr || rec == nullptr) {
    return false;
  }
  ID3D12Device* dev = g_device.load(std::memory_order_relaxed);
  if (dev == nullptr) {
    ComPtr<ID3D12Device> owner;
    if (FAILED(out_res->GetDevice(IID_PPV_ARGS(&owner)))) {
      return false;
    }
    dev = owner.Get();
    g_device.store(dev, std::memory_order_relaxed);
    dpour_tex::EnsureHeap(dev);
    dpour_pipeline::Init(dev);
  }
  // Real lifetime gating, from the command processor's own counters. Ours used
  // to be a lag heuristic (a private counter minus kPassFrames) with nothing on
  // the GPU behind it, which is not a fence at all - it decided when vertex,
  // index and texture uploads were safe to recycle by guessing. These two are
  // "the basis of all lifetime/readback gating" per native_rhi.h:545.
  const std::uint64_t submission = ctx.device->CurrentSubmission();
  const std::uint64_t completed = ctx.device->CompletedSubmission();
  g_submission_current.store(submission, std::memory_order_relaxed);
  g_submission_completed.store(completed, std::memory_order_relaxed);
  FlushRetired(completed);
  // Unconditionally, before anything below can decide to return early.
  UpdateGpuCompleted(completed);
  const bool drew = RenderRecorded(dev, rec, nullptr, ctx.guest_output_width,
                                   ctx.guest_output_height, out_res, submission, completed);
  if (!drew) {
    return false;
  }
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[native-scene] guest output REPLACED natively for the first time ({}x{})",
                ctx.guest_output_width, ctx.guest_output_height);
  }
  return true;
}
// The back-buffer pass replays like any other. It was gated because doing so
// turned every screen white - but that was the symptom of the missing resolve
// LINK, not of the pass: its two composition draws sample the scene through a
// texture that no native target backed. With OnResolveScene now keying the
// link on the D3D texture the game actually binds (the game's own
// RHICopyToResolveTarget takes ResolveTarget2D->Resource), those draws read
// what we rendered, which is the whole point of replaying them.
//
// Kill switch kept, inverted: DPOUR_NR_DRAW_NOBACKBUF.
bool BackbufferPassEnabled() {
  static const bool off = EnvFlag("DPOUR_NR_DRAW_NOBACKBUF");
  return !off;
}

// Publish the back-buffer target as the composite source whenever the game
// drew into it. Default on: the back-buffer pass is the game's own final
// composition, which no draw-count vote can beat. The kill switch exists so a
// live run can fall back to the vote for comparison without a rebuild.
bool BackbufferPrioEnabled() {
  static const bool off = EnvFlag("DPOUR_NR_DRAW_NOBBPRIO");
  return !off;
}

// DPOUR_NR_DRAW_AMPLIFY=N - multiply the composited colour by N. Separates a
// scene that is written far too dark (an EDRAM exponent-bias scale problem)
// from one that was never written at all; the second shows up as magenta.
float AmplifyFactor() {
  static const float f = [] {
    const char* v = std::getenv("DPOUR_NR_DRAW_AMPLIFY");
    const double parsed = (v != nullptr) ? std::atof(v) : 0.0;
    // A negative value selects the classify mode in the composite shader.
    if (parsed < 0.0) {
      return -1.0f;
    }
    return (parsed > 0.0 && parsed < 4096.0) ? static_cast<float>(parsed) : 0.0f;
  }();
  return f;
}
}  // namespace

void BeginFrame() {
  // REGISTER UNCONDITIONALLY, AND NOT FROM THE PRESENTER.
  //
  // skate3 does exactly this (skate3_native_scene_gpu.cpp Install()), and its
  // comment says why: the renderer is registered "even when the scene cvar
  // starts off", because the callback itself yields to the emulated output
  // while disabled - so the toggle can flip at any time and nothing depends on
  // who called whom. Ours used to register inside the presenter's per-present
  // entry, which made "is the native path in the chain at all" depend on a UI
  // drawer running. It stopped running, and the whole path went silently dark.
  {
    static std::atomic<bool> registered{false};
    if (!registered.exchange(true, std::memory_order_relaxed)) {
      rex::graphics::SetNativeGuestOutputRenderer(&GuestOutputRenderCallback, nullptr);
      REXLOG_INFO("[native-scene] guest-output renderer registered (draw path {})",
                  Enabled() ? "on" : "off");
    }
  }
  if (!Enabled()) {
    return;
  }
  const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  g_capture_frames.fetch_add(1, std::memory_order_relaxed);
  dpour_vbuf::BeginFrame(frame);
  dpour_tex::BeginFrame();
  ID3D12Device* device = g_device.load(std::memory_order_relaxed);
  if (device != nullptr) {
    dpour_pipeline::BeginFrame(device, frame, g_gpu_completed.load(std::memory_order_relaxed));
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  g_staging.clear();
  g_resolve_staging.clear();
  g_clear_staging.clear();
  // The render target does not change just because a frame ended, so the pass
  // that was current carries over as slot 0 with a fresh count. Resetting the
  // count without carrying the identity left draws made before the frame's first
  // SetRenderTarget accumulating on a stale slot, which then won the vote.
  PassSlot carry{};
  if (g_pass_current < kMaxPasses) {
    carry = g_passes[g_pass_current];  // identity AND surface objects AND viewport
  }
  carry.draws = 0;
  g_passes[0] = carry;
  g_up_frame_count.store(0, std::memory_order_relaxed);
  g_pass_count = 1;
  g_pass_current = 0;
  g_frame_scene_color = 0;  // the base pass renames it every frame
  g_vs_rev_cached = ~0ull;
  g_ps_rev_cached = ~0ull;
  g_cache_frame = frame;
}

void EndFrame() {
  if (!Enabled()) {
    return;
  }
  // The last UP draw of the frame has no later event to close it; the frame
  // boundary is that event.
  CloseUPPending();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pass_count == 0) {
    return;
  }
  // `best` sizes the stats line and nothing else - every captured draw is
  // published regardless, and the composite takes the back buffer. The old
  // "an MSAA block means the scene slot won" branch is gone with the diversion
  // that fed it: no draw can land in kScenePassSlot any more.
  std::uint32_t best = 0;
  for (std::uint32_t i = 1; i < g_pass_count; ++i) {
    if (g_passes[i].draws > g_passes[best].draws) {
      best = i;
    }
  }
  // PUBLISH WHAT WAS RESOLVED, not what won the vote.
  //
  // Once a target is chosen, only draws going to it are resolved at all - so
  // everything in the staging list already belongs to it. Filtering that list by
  // "the pass with the most draws this frame" then published NOTHING whenever
  // the vote picked a different pass than the one being resolved, which is
  // exactly what a frame where the UI out-draws the scene looks like. The result
  // was an empty publish, no composite, and the emulated frame showing through -
  // which reads as "the native renderer did nothing" and is indistinguishable
  // from it.
  //
  // The vote still runs; it decides the target for the NEXT frame.
  // Reference shape: the WHOLE frame is published, in submission order, along
  // with a snapshot of the pass table so the render thread can switch targets
  // as the order dictates. `best` only sizes the scene target and the stats.
  g_published = g_staging;
  // The bytes travel with the draws that reference them: the arena capture just
  // filled becomes the one the replay reads, and capture moves to the next.
  g_published_arena_index = g_stage_arena_index;
  g_stage_arena_index = (g_stage_arena_index + 1) % kArenaCount;
  g_arenas[g_stage_arena_index].Reset();
  g_vs_cb_cached = kNoStage;  // nothing staged in the fresh arena yet
  g_ps_cb_cached = kNoStage;
  g_resolve_published = g_resolve_staging;
  g_clear_published = g_clear_staging;
  std::memcpy(g_published_passes, g_passes, sizeof(g_published_passes));
  g_published_pass = best;
  g_published_frame = g_frame.load(std::memory_order_relaxed);
  g_published_w.store(g_passes[best].view_w, std::memory_order_relaxed);
  g_published_h.store(g_passes[best].view_h, std::memory_order_relaxed);
  g_target_color = g_passes[best].color;
  g_target_depth = g_passes[best].depth;
  g_have_target = true;
}

void SetRenderTarget(std::uint32_t color_guest, std::uint32_t depth_guest,
                     std::uint32_t color_object, std::uint32_t depth_object,
                     bool is_backbuffer) {
  if (!Enabled()) {
    return;
  }
  if (DeviceRtEnabled()) {
    // The device-level hook below is authoritative and sees this same bind one
    // call later (RHISetRenderTarget's first act is
    // GDirect3DDevice->SetRenderTarget). Switching here as well would key a
    // second pass slot on the ref pointer and exhaust the sixteen slots.
    (void)color_guest;
    (void)depth_guest;
    (void)color_object;
    (void)depth_object;
    (void)is_backbuffer;
    return;
  }
  // A pending UP draw belongs to the pass that was current at its Begin; close
  // it before the pass changes underneath it.
  //
  // (A scene-colour special case stood here - recognising "DefaultColor" binds
  // and RETURNING before the pass tracker below, a leftover of the deleted
  // scene-slot machinery. It meant a scene bind on this fallback path kept the
  // PREVIOUS pass current, charging the world's draws to whatever was bound
  // before. The reference treats every bind the same, so now we do too.)
  CloseUPPending();
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::uint32_t before = g_pass_count;
  g_pass_current = PassSlotFor(color_guest, depth_guest);
  if (g_pass_current < kMaxPasses) {
    g_passes[g_pass_current].color_object = color_object;
    g_passes[g_pass_current].depth_object = depth_object;
    g_passes[g_pass_current].is_backbuffer = is_backbuffer;
    // One line per NEW pass identity, capped: which surface ref maps to which
    // surface object, and whether it is the back buffer. Reading "which pass is
    // the game's final composition" off the stats line was guesswork, and the
    // guess (the back-buffer pass) turned out to hold two draws while another
    // pass held a hundred and ten.
    if (g_pass_count != before) {
      static std::uint32_t announced = 0;
      if (announced < 24) {
        ++announced;
        REXLOG_INFO(
            "[native-scene] pass identity: ref {:#x} -> object {:#x}{} (depth ref {:#x} -> {:#x})",
            color_guest, color_object, is_backbuffer ? "  == BACKBUFFER" : "", depth_guest,
            depth_object);
      }
    }
  }
}

bool DeviceRtEnabled() {
  static const bool off = EnvFlag("DPOUR_NR_DRAW_NODEVRT");
  return !off;
}

void DeviceSetRenderTarget(std::uint32_t index, std::uint32_t surface_object,
                           bool is_backbuffer) {
  if (!Enabled() || !DeviceRtEnabled() || index != 0) {
    return;  // MRT slots above zero do not decide which pass a draw belongs to
  }
  // A pending UP draw belongs to the pass that was current at its Begin.
  CloseUPPending();
  // NO SPECIAL CASE FOR "THE SCENE". This used to divert the game's scene
  // colour surface into a reserved slot, which is the invention that cost a
  // day: UnleashedRecomp's ProcSetRenderTarget (video.cpp:3334) does nothing
  // but bind the surface the game asked for, because every guest surface has
  // its own native target and the back buffer IS the swapchain image
  // (video.cpp:1597). There is no "which pass is the scene" question to answer,
  // and answering it is what produced eighteen identical AuxColor candidates, a
  // pass vote, and a scene target full of six-index UI quads.
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::uint32_t before = g_pass_count;
  // Keyed on the surface OBJECT, which is what the device is handed. The RHI
  // path keyed on the ref POINTER, and two refs holding the same surface split
  // one physical target across two pass slots (seen live: refs 0x407c7e88 and
  // 0x401e542c both holding object 0x40022370).
  g_pass_current = PassSlotFor(surface_object, 0);
  if (g_pass_current < kMaxPasses) {
    g_passes[g_pass_current].color_object = surface_object;
    g_passes[g_pass_current].is_backbuffer = is_backbuffer;
    // The bound surface's format travels with the pass so the PSO built for its
    // draws names the same format the replay will bind. Under the alias, the
    // canonical surface's record is the authoritative one.
    if (SurfaceFormatOn() && !is_backbuffer) {
      DXGI_FORMAT fmt = kColorFormat;
      const std::uint32_t canon = CanonicalSurface(surface_object);
      std::lock_guard<std::mutex> lk(g_reg_mutex);
      const auto meta = g_surface_meta.find(canon);
      if (meta != g_surface_meta.end() && meta->second.dxgi != DXGI_FORMAT_UNKNOWN) {
        fmt = meta->second.dxgi;
      }
      g_passes[g_pass_current].rtv_format = fmt;
      g_passes[g_pass_current].no_depth = false;
    } else if (is_backbuffer && OwnDeviceMode()) {
      // The back-buffer pass binds the presenter's own image in this mode, so
      // its PSOs must name the presenter's format, not ours - and NO depth
      // format, because the bind carries no DSV.
      g_passes[g_pass_current].rtv_format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
      g_passes[g_pass_current].no_depth = true;
    } else {
      g_passes[g_pass_current].rtv_format = kColorFormat;
      g_passes[g_pass_current].no_depth = false;
    }
    if (g_pass_count != before) {
      static std::uint32_t announced = 0;
      if (announced < 24) {
        ++announced;
        REXLOG_INFO("[native-scene] device pass: surface {:#x}{}", surface_object,
                    is_backbuffer ? "  == BACKBUFFER (this is the frame)" : "");
      }
    }
  }
}

void DeviceSetDepthStencil(std::uint32_t surface_object) {
  if (!Enabled() || !DeviceRtEnabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pass_current < kMaxPasses) {
    g_passes[g_pass_current].depth_object = surface_object;
  }
}


namespace {
// Reads a NUL-terminated UTF-16BE guest string of printable ASCII into out.
// Returns false when the pointer does not look like such a string at all.
bool ReadGuestWideAscii(const std::uint8_t* base, std::uint32_t guest, char* out, std::size_t cap) {
  if (guest < 0x10000u || guest >= 0xC0000000u) {
    return false;
  }
  const std::uint8_t* p = base + guest;
  // One-shot boot path (a few dozen calls ever), so a page probe is fine.
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  std::size_t n = 0;
  while (n + 1 < cap) {
    const std::uint8_t hi = p[2 * n];
    const std::uint8_t lo = p[2 * n + 1];
    if (hi != 0) {
      return false;
    }
    if (lo == 0) {
      break;
    }
    if (lo < 0x20 || lo > 0x7E) {
      return false;
    }
    out[n++] = static_cast<char>(lo);
  }
  out[n] = '\0';
  return n > 0;
}
}  // namespace

std::uint32_t SceneResolveTexture() {
  return g_scene_resolve_texture.load(std::memory_order_relaxed);
}

bool ShouldSkipGuestDraw() {
  // OWN-DEVICE MODE: a draw we reproduced does not also go to the emulated GPU.
  // Unconditional, because in this mode there is no emulated frame to protect -
  // see OwnDeviceMode() for what it does and does not cover.
  if (OwnDeviceMode()) {
    return true;
  }
  if (!SkipGuestDraws()) {
    return false;
  }
  if (InjectEnabled()) {
    return g_injections.load(std::memory_order_relaxed) != 0;
  }
  return true;
}

bool OnResolveScene(const std::uint8_t* base, std::uint32_t surface_ref,
                    std::uint32_t surface_object, std::uint32_t resolve_params) {
  if (!Enabled() || base == nullptr || surface_object == 0) {
    return false;
  }
  // Reference behaviour (UnleashedRecomp ProcStretchRect): a resolve LINKS the
  // destination texture to the source surface; no pixels move.
  //
  // WHICH texture is not a guess - the game's own header says so.
  // FSurfaceRHIRef (Xenon/XeD3DDrv/Inc/XeD3DRenderTarget.h:67) is
  //   +0   IDirect3DSurface9*         (the TRefCountPtr base)
  //   +4   ResolveTargetTexture2D     FTexture2DRHIRef -> FXeTexture2D*
  //   +8   ResolveTargetTextureCube
  //   +12  XeSurfaceInfo { Offset +12, Size +16, ColorExpBias +20, Flags +24 }
  // and RHICopyToResolveTarget (XeD3DRenderTarget.cpp:399) takes exactly
  // SourceSurface.ResolveTargetTexture2D as the destination.
  //
  // The KEY, though, is not that RHI object: the device is handed the D3D
  // texture behind it - RHICopyToResolveTarget itself writes
  //   IDirect3DTexture9* D3DTexture2D = (IDirect3DTexture9*)ResolveTarget2D->Resource;
  // and every bind reaches us through device SetTexture with that same pointer
  // (FXeGPUResource::Resource at +8). Registering the RHI object instead - which
  // is what the previous guess-every-member loop did - produced a link nothing
  // could ever hit, and additionally registered XeSurfaceInfo.Offset, an EDRAM
  // tile number, as if it were a texture address.
  //
  // ...UNLESS the call carries an explicit destination. The very first line of
  // RHICopyToResolveTarget is
  //   FTextureRHIParamRef ResolveTarget2D = ResolveParams.ResolveTarget
  //       ? ResolveParams.ResolveTarget : SourceSurface.ResolveTargetTexture2D;
  // (XeD3DRenderTarget.cpp:399), so the override wins outright. FResolveParams
  // (RHI.h:630-659) is CubeFace +0, X1 +4, Y1 +8, X2 +12, Y2 +16,
  // ResolveTarget +20 - all four-byte members, no padding on this ABI. Reading
  // only surface_ref+4 linked those resolves to the WRONG destination texture,
  // so a later sample of the real one found no link and fell through to the
  // decoded guest bytes.
  std::uint32_t rhi_tex = resolve_params != 0 ? ReadGuestPtr(base, resolve_params + 20) : 0;
  const bool explicit_target = rhi_tex != 0;
  if (rhi_tex == 0) {
    rhi_tex = ReadGuestPtr(base, surface_ref + 4);
  }
  if (rhi_tex == 0) {
    return false;  // depth or cube resolve: no 2D destination
  }
  const std::uint32_t d3d_tex = ReadGuestPtr(base, rhi_tex + 8);
  if (d3d_tex == 0) {
    return false;
  }
  // ...and the key on top of THAT is the memory the texture occupies, not the
  // texture. RHICreateSharedTexture2D points several render-target textures at
  // one buffer (SceneRenderTargets.cpp:1074 and five more call sites) and ends
  // with XGOffsetResourceAddress(Resource, SharedMemory->BaseAddress)
  // (XeD3DTexture.cpp:1246), so they all report the same base address in their
  // fetch constants. Keyed on the object, a resolve into FilterColor left
  // SceneColorLDR - the texture the final quad samples, and the SAME buffer -
  // holding whatever we last wrote into it, which is why the menu wore the
  // loading screen. Keyed on the address, one resolve refreshes all of them,
  // exactly as one buffer does on the console.
  const std::uint32_t addr = AliasKeyOn() ? dpour_tex::GuestBaseAddress(base, d3d_tex) : 0;
  const std::uint32_t alias = addr != 0 ? addr : d3d_tex;
  // Queue the copy where it happened in the draw stream. Order matters and is
  // not negotiable: the game resolves a surface, then samples the result while
  // rendering back into that same surface. A copy taken at the end of the frame
  // would hand every such pass the FINAL contents of the surface instead of what
  // it held at resolve time. The draw count at this moment is that position.
  if (ResolveCopyEnabled()) {
    std::int32_t bias = 0;
    if (ObjectReadable(base, surface_ref + 20, 4)) {
      bias = static_cast<std::int32_t>(LoadBE32(base + surface_ref + 20));
    }
    // A bias outside what the game's own table can produce (0, 3, 5 - see
    // XeGetRenderTargetColorExpBias) means the struct is not what we think it
    // is; take no bias rather than scale by a garbage exponent.
    if (bias < -8 || bias > 8) {
      bias = 0;
    }
    std::lock_guard<std::mutex> dl(g_mutex);
    if (g_resolve_staging.size() < 64) {
      g_resolve_staging.push_back(PendingResolve{CanonicalSurface(surface_object), alias, bias,
                                                 static_cast<std::uint32_t>(g_staging.size())});
    }
  }

  std::lock_guard<std::mutex> lk(g_reg_mutex);
  // The LINK is to the canonical surface too: DefaultColorRaw and
  // DefaultColorFixedPoint resolve into one texture, and whoever samples it must
  // reach the target the scene was actually drawn into.
  const std::uint32_t link_surface = CanonicalSurface(surface_object);
  const bool is_new = g_texture_link.find(alias) == g_texture_link.end() ||
                      g_texture_link[alias] != link_surface;
  g_texture_link[alias] = link_surface;
  // AND UNDER THE OBJECT, ALWAYS. The address key above exists for this engine's
  // shared render-target memory, and it is the right key whenever the fetch
  // constant can be read. But the moment it cannot, the only thing a sampler
  // binding carries is the D3DBaseTexture pointer itself - and the link was
  // filed under an address the caller has no way to compute. Measured 28.07:
  // that is EXACTLY the failing case. `WHITE served ... reason 1 (no fetch)`
  // means the fetch failed, so Acquire asked RtBackedSrvSlot(object, 0), and the
  // lookup missed a link that was sitting right there under its address. The
  // final composition quad then sampled white, which is the screen the user saw.
  //
  // The reference files this link on the OBJECT and nothing else
  // (UnleashedRecomp ProcStretchRect: texture->sourceSurface = surface), because
  // its texture objects ARE its own structs. Ours must answer to both questions,
  // so it answers to both keys.
  if (alias != d3d_tex) {
    g_texture_link[d3d_tex] = link_surface;
  }
  // Something will sample this surface, so it needs a texture to be sampled
  // FROM - now, not once a draw happens to land in it. See the note on
  // g_pending_surface_targets.
  if (link_surface != 0 &&
      std::find(g_pending_surface_targets.begin(), g_pending_surface_targets.end(),
                link_surface) == g_pending_surface_targets.end()) {
    g_pending_surface_targets.push_back(link_surface);
  }
  // EVERY distinct link, not just the first: one link tells us the mechanism
  // works, but not whether the SCENE's surface is among them - which is the
  // only one the game's final composition actually samples.
  if (is_new) {
    static std::atomic<std::uint32_t> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 24) {
      const auto srv = g_surface_srv.find(surface_object);
      const auto meta = g_surface_meta.find(surface_object);
      REXLOG_INFO("[native-scene] resolve link: key {:#x} (tex {:#x} addr {:#x}) <- surface {:#x} "
                  "({}x{}) srv {} explicit={}",
                  alias, d3d_tex, addr, surface_object,
                  meta != g_surface_meta.end() ? meta->second.w : 0,
                  meta != g_surface_meta.end() ? meta->second.h : 0,
                  srv != g_surface_srv.end() ? static_cast<int>(srv->second) : -1,
                  explicit_target ? 1 : 0);
    }
  }
  // The guest's own resolve still runs - the emulated pipeline stays whole
  // until every consumer renders natively.
  return false;
}

std::uint32_t RtBackedSrvSlot(std::uint32_t texture_rhi_guest, std::uint32_t base_addr) {
  if (!Enabled() || texture_rhi_guest == 0) {
    return dpour_tex::kInvalidSlot;
  }
  // The alias key: memory first, object second. Six of this engine's render
  // targets live in ONE buffer on purpose, so a resolve into any of them is a
  // resolve into all - and keying on the object alone made each of them a
  // private copy that nobody else's resolve ever refreshed.
  const std::uint32_t alias = AliasKeyOn() && base_addr != 0 ? base_addr : texture_rhi_guest;
  std::uint32_t surface = 0;
  {
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    const auto link = g_texture_link.find(alias);
    if (link == g_texture_link.end()) {
      return dpour_tex::kInvalidSlot;
    }
    surface = link->second;
  }

  // NEVER SERVE THE TARGET THE DRAW IS WRITING INTO.
  //
  // Serving a surface's own SRV in place of its resolve texture is the
  // reference's fast path (UnleashedRecomp ProcSetTexture -> SetSurface), but
  // the reference only takes it when nothing is writing that surface: when the
  // game needs a resolve it cannot alias, ExecutePendingStretchRectCommands
  // makes a REAL copy into the destination texture first.
  //
  // Downpour's post-processing does exactly the case that forbids aliasing - it
  // resolves a surface to a texture, then samples that texture while rendering
  // BACK INTO THE SAME surface. Handing out the target's SRV there binds a
  // resource that is simultaneously the bound render target: a read-write
  // hazard, which is undefined and hangs the device. It stayed invisible only
  // because the link key was wrong and this function never returned a slot; the
  // TDR appeared in the very first session where the link worked.
  //
  // The guest render thread that samples is the same one that binds targets, so
  // the current pass is exactly the right thing to compare against.
  // REVERTED 2026-07-26: refusing the alias here CORRUPTED the UI - the loading
  // screen's text, which had been rendering perfectly, came back with broken
  // glyphs. The refusal falls back to DECODING the guest texture, and a resolve
  // target's guest memory is not something our native path ever wrote: the
  // fallback is worse than the aliasing it avoids.
  //
  // The reference does not refuse either - it COPIES. ProcSetTexture serves the
  // surface directly only when it can, and ExecutePendingStretchRectCommands
  // renders a real copy of the surface into the destination texture when the
  // alias is not safe. Doing this properly means owning that copy, which is
  // part of the nrhi migration (CopyTextureToBuffer/blit + a per-texture target),
  // not a one-line guard.
  (void)g_rt_alias_refused;

  std::lock_guard<std::mutex> lk(g_reg_mutex);
  if (ResolveCopyEnabled()) {
    // A copy exists for this texture: serve THAT. It holds the surface as it was
    // at resolve time, with the exponent bias taken back out - the two things
    // the alias cannot give. If no copy has been made yet (first frames, or a
    // texture resolved before our targets existed) fall through to the alias
    // rather than to the guest-memory decode: the decode is what corrupted the
    // loading-screen glyphs when this path refused outright.
    const auto copy = g_resolve_srv.find(alias);
    if (copy != g_resolve_srv.end() && CopyIsFresh(alias)) {
      return copy->second;
    }
  }
  // SERVE NATIVELY ONLY WHAT WE RENDERED NATIVELY.
  //
  // The reference never faces this question: Unleashed replaces the whole RHI,
  // so every guest surface holds what the game drew and a resolve always has a
  // source. Ours is partial - some surfaces are still written only by the
  // emulated path - and for those our target is empty and our copy is whatever
  // was last put there, which during the menu is THE LOADING SCREEN. That is the
  // ghost: not a frozen frame (the snapshot keeps moving), but a live frame
  // sampling textures nobody refreshed.
  //
  // skate3's discipline for "we cannot serve this" is to yield, and this is that
  // yield at texture granularity: hand back kInvalidSlot and the guest texture is
  // decoded from guest memory, which the emulated path DID write correctly.
  //
  // This is also why refusing the alias outright once corrupted the loading
  // screen's glyphs: back then it refused for surfaces we HAD rendered, whose
  // guest memory we never wrote. The freshness test refuses only the opposite
  // case, so the decode is the right answer exactly when it is taken.
  const auto srv = g_surface_srv.find(surface);
  if (srv != g_surface_srv.end() && SurfaceIsFresh(surface)) {
    return srv->second;
  }
  return dpour_tex::kInvalidSlot;
}

// AddUnusedXeResource has handed us a resource the guest is retiring. Only an
// object we actually hold a target for is one of ours - the queue that reaches
// this is every retiring resource in the game, most of them textures and
// vertex-buffer orphans that the other two modules claim.
//
// Queued, not acted on: the registry belongs to the render thread, and this runs
// on the guest's.
void RetireSurface(std::uint32_t resource_or_surface) {
  if (!Enabled() || resource_or_surface == 0) {
    return;
  }
  g_retire_calls.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_reg_mutex);
  const std::uint32_t surface = resource_or_surface;
  if (g_surface_meta.find(surface) == g_surface_meta.end()) {
    return;  // not a surface of ours - textures and buffers share this queue
  }
  g_retire_matched.fetch_add(1, std::memory_order_relaxed);
  g_surface_meta.erase(surface);
  g_pending_surface_retire.push_back(surface);
}

void OnTargetableSurfaceCreated(const std::uint8_t* base, std::uint32_t sret,
                                std::uint32_t usage_a, std::uint32_t usage_b,
                                std::uint32_t resolve_texture, std::uint32_t size_x,
                                std::uint32_t size_y, std::uint32_t pixel_format) {
  if (!Enabled() || base == nullptr) {
    return;
  }
  char name[48];
  if (!ReadGuestWideAscii(base, usage_a, name, sizeof(name)) &&
      !ReadGuestWideAscii(base, usage_b, name, sizeof(name))) {
    std::snprintf(name, sizeof(name), "<none %08x/%08x>", usage_a, usage_b);
  }
  std::uint32_t surface = 0;
  if (sret >= 0x10000u && sret < 0xC0000000u) {
    std::uint32_t v;
    std::memcpy(&v, base + sret, 4);
    surface = _byteswap_ulong(v);
  }
  // A handful of one-time creations; logging them all is what verifies both
  // the argument layout and the actual usage names this game uses.
  REXLOG_INFO("[native-scene] targetable surface {:08x} usage \"{}\" {}x{} pf={}", surface, name,
              size_x, size_y, pixel_format);
  if (surface != 0 && size_x >= 1 && size_x <= 4096 && size_y >= 1 && size_y <= 4096) {
    const bool depth = std::strstr(name, "Depth") != nullptr;
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    SurfaceMeta m{size_x, size_y, depth};
    std::snprintf(m.name, sizeof(m.name), "%s", name);
    m.dxgi = DxgiFromPixelFormat(pixel_format);
    g_surface_meta[surface] = m;
    // THE SURFACE GETS ITS TEXTURE HERE, exactly as CreateSurface does
    // (video.cpp:3184): texture, view and descriptor index allocated at
    // creation, for every surface, unconditionally. Not when a draw first lands
    // in it, not when a resolve first points at it - at birth. That is the
    // property that makes "sample a surface" always answerable in the
    // references, and its absence is what served white here: measured 28.07,
    // 15 of 54 surfaces had a texture, and the scene's own resolve destination
    // was among the 39 that did not.
    //
    // Depth surfaces take the reference's other branch (DEPTH_TARGET) and are
    // transient there by its own account (video.cpp:3572); our registry target
    // is a colour target, so they are not queued for one.
    if (!depth) {
      g_pending_surface_targets.push_back(surface);
    }
  }
  // The engine's own names (SceneRenderTargets.cpp:1042/1058): the scene colour
  // surface is created as "DefaultColor", with "DefaultColorRaw" and
  // "DefaultColorFixedPoint" aliasing the same EDRAM for raw/fixed-point
  // passes. Any of the three being bound means the scene is being drawn.
  if (std::strncmp(name, "DefaultColor", 12) == 0 && surface != 0) {
    // A level load RECREATES the whole DefaultColor family. The old fill-empty
    // logic could never record the new objects once the slots were taken - so
    // after the first level load the scene was never marked again (gameplay
    // captured ~30 draws/frame and the native frame went black). The primary
    // alias always leads the recreated family: wipe and start over on it.
    if (std::strcmp(name, "DefaultColor") == 0) {
      for (auto& slot : g_scene_color_aliases) {
        slot.store(0, std::memory_order_relaxed);
      }
      g_scene_color_aliases[0].store(surface, std::memory_order_relaxed);
      static std::atomic<std::uint32_t> generations{0};
      const std::uint32_t gen = generations.fetch_add(1, std::memory_order_relaxed);
      if (gen > 0) {
        REXLOG_INFO("[native-scene] DefaultColor recreated (generation {}): alias slots reset",
                    gen + 1);
      }
    } else {
      for (auto& slot : g_scene_color_aliases) {
        const std::uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == surface) {
          break;
        }
        if (cur == 0) {
          slot.store(surface, std::memory_order_relaxed);
          break;
        }
      }
    }
    g_scene_color_surface.store(surface, std::memory_order_relaxed);
    // Only the primary alias carries the resolve texture we care about.
    if (std::strcmp(name, "DefaultColor") == 0 && resolve_texture != 0) {
      g_scene_resolve_texture.store(resolve_texture, std::memory_order_relaxed);
    }
  } else if (std::strcmp(name, "DefaultDepth") == 0 && surface != 0) {
    g_scene_depth_surface.store(surface, std::memory_order_relaxed);
  }
}


void NoteViewProj(const std::uint8_t* base, std::uint32_t matrix_guest) {
  if (!Enabled() || !VsProbeEnabled() || base == nullptr || !GuestAddrPlausible(matrix_guest) ||
      !ObjectReadable(base, matrix_guest, 64)) {
    return;
  }
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadBEFloat(base + matrix_guest + i * 4);
    if (!std::isfinite(m[i])) {
      return;
    }
  }
  std::memcpy(g_probe_vp, m, sizeof(m));
  g_have_probe_vp = true;
}

void SetViewport(std::uint32_t min_x, std::uint32_t min_y, std::uint32_t max_x,
                 std::uint32_t max_y) {
  if (!Enabled() || max_x <= min_x || max_y <= min_y) {
    return;
  }
  const std::uint32_t w = max_x - min_x;
  const std::uint32_t h = max_y - min_y;
  if (w == 0 || h == 0 || w > 4096 || h > 4096) {
    return;
  }
  g_view_w.store(w, std::memory_order_relaxed);
  g_view_h.store(h, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pass_current < kMaxPasses) {
    g_passes[g_pass_current].view_w = w;
    g_passes[g_pass_current].view_h = h;
  }
}

namespace {

// One photograph of the pixel constant bank, capped so a probe run stays
// readable. `dev` is the guest's own big-endian register file (null when the
// device shadow is off), `staged` is the host-order bytes about to be uploaded,
// read AFTER the bias override so the line says what the shader will actually
// see rather than what the game asked for.
void ProbePixelBank(const std::uint8_t* dev, const std::uint8_t* staged, std::uint32_t bytes,
                    std::uint64_t ps_hash) {
  static std::uint32_t printed = 0;
  if (printed >= 16) {
    return;
  }
  ++printed;

  float dev_c0[4]{};
  std::uint32_t nonzero = 0;
  std::uint32_t top_reg = 0;
  if (dev != nullptr) {
    for (int i = 0; i < 4; ++i) {
      dev_c0[i] = LoadBEFloat(dev + i * 4);
    }
    // Whether the bank address is right at all: a register file the game has
    // been writing to for minutes cannot be all zeroes, and if it is, every
    // material constant we upload is zero and the black frame needs no further
    // explanation.
    for (std::uint32_t r = 0; r < dpour_consts::kRegisters; ++r) {
      for (int c = 0; c < 4; ++c) {
        if (LoadBEFloat(dev + (r * 4 + c) * 4) != 0.0f) {
          ++nonzero;
          top_reg = r;
        }
      }
    }
  }

  float hook_c0[4]{};
  dpour_consts::CopyPixelBank(hook_c0, sizeof(hook_c0));

  float up_c0[4]{};
  if (staged != nullptr && bytes >= 16) {
    std::memcpy(up_c0, staged, sizeof(up_c0));
  }

  REXLOG_INFO(
      "[c0probe] ps {:#x} | device bank {}: c0 ({:.4f} {:.4f} {:.4f} {:.4f}), "
      "{} non-zero floats of {}, top register {} | hook c0 ({:.4f} {:.4f} {:.4f} {:.4f}) "
      "| UPLOADING c0 ({:.4f} {:.4f} {:.4f} {:.4f}) in {} bytes",
      ps_hash, dev != nullptr ? "live" : "ABSENT", dev_c0[0], dev_c0[1], dev_c0[2], dev_c0[3],
      nonzero, dpour_consts::kRegisters * 4u, top_reg, hook_c0[0], hook_c0[1], hook_c0[2],
      hook_c0[3], up_c0[0], up_c0[1], up_c0[2], up_c0[3], bytes);
}

// The constant snapshot shared by regular and UP draws: the float banks (cached
// per revision so a run of draws with unchanged constants shares one upload),
// and the per-draw shared block (texture table, samplers, bool/loop banks).
bool FillDrawConstants(const std::uint8_t* base, const DrawDesc& d,
                       const dpour_state::Pipeline& state,
                       const dpour_pipeline::DeclLayout* layout, SceneDraw& out) {
  // A bank the game has not written to yet still needs an address to bind, and
  // a zero-byte allocation is not one.
  const std::uint32_t vs_bytes = dpour_consts::VertexBankBytes() > 0
                                     ? dpour_consts::VertexBankBytes()
                                     : 256u;
  const std::uint32_t ps_bytes = dpour_consts::PixelBankBytes() > 0
                                     ? dpour_consts::PixelBankBytes()
                                     : 256u;
  // The device's own register file, when it is available, in preference to the
  // shadowed setters. See downpour_native_constants.h: the XDK inlines most
  // constant writes straight into the device, so the setter shadow is only a
  // partial view - its pixel bank never saw a register above 33, and the
  // materials multiplied by the ones it missed came out as exactly zero.
  //
  // Keyed on CONTENT rather than on a write counter: the device file has no
  // revision, and copying four kilobytes per draw would be both slow and
  // pointless when consecutive draws share a material. The comparison is over
  // the guest's own big-endian bytes, so it costs a memcmp and no conversion.
  const std::uint8_t* dev_vs =
      dpour_consts::DeviceBanksEnabled() ? dpour_consts::DeviceVertexBank(base) : nullptr;
  const std::uint8_t* dev_ps =
      dpour_consts::DeviceBanksEnabled() ? dpour_consts::DevicePixelBank(base) : nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    static std::uint8_t vs_raw[dpour_consts::kDeviceBankBytes];
    static std::uint8_t ps_raw[dpour_consts::kDeviceBankBytes];
    static bool vs_raw_valid = false;
    static bool ps_raw_valid = false;

    const std::uint64_t vs_rev = dpour_consts::VertexRevision();
    const bool vs_changed =
        dev_vs != nullptr
            ? (!vs_raw_valid || std::memcmp(vs_raw, dev_vs, sizeof(vs_raw)) != 0)
            : (vs_rev != g_vs_rev_cached);
    if (vs_changed || g_vs_cb_cached == kNoStage) {
      const std::uint32_t bytes = dev_vs != nullptr ? dpour_consts::kDeviceBankBytes : vs_bytes;
      std::uint32_t handle = kNoStage;
      std::uint8_t* cpu = g_arenas[g_stage_arena_index].Alloc(bytes, handle);
      if (cpu == nullptr) {
        g_no_constants.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (dev_vs != nullptr) {
        std::memcpy(vs_raw, dev_vs, sizeof(vs_raw));
        vs_raw_valid = true;
        SwapBankToHost(cpu, dev_vs, bytes);
      } else {
        dpour_consts::CopyVertexBank(cpu, bytes);
      }
      g_vs_cb_cached = handle;
      g_vs_cb_bytes = bytes;
      g_vs_rev_cached = vs_rev;
    }
    out.cb_vertex = g_vs_cb_cached;
    out.cb_vertex_bytes = g_vs_cb_bytes;

    if (d.ps != nullptr) {
      const std::uint64_t ps_rev = dpour_consts::PixelRevision();
      const bool ps_changed =
          dev_ps != nullptr
              ? (!ps_raw_valid || std::memcmp(ps_raw, dev_ps, sizeof(ps_raw)) != 0)
              : (ps_rev != g_ps_rev_cached);
      if (ps_changed || g_ps_cb_cached == kNoStage) {
        const std::uint32_t bytes = dev_ps != nullptr ? dpour_consts::kDeviceBankBytes : ps_bytes;
        std::uint32_t handle = kNoStage;
        std::uint8_t* cpu = g_arenas[g_stage_arena_index].Alloc(bytes, handle);
        if (cpu == nullptr) {
          g_no_constants.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (dev_ps != nullptr) {
          std::memcpy(ps_raw, dev_ps, sizeof(ps_raw));
          ps_raw_valid = true;
          SwapBankToHost(cpu, dev_ps, bytes);
        } else {
          dpour_consts::CopyPixelBank(cpu, bytes);
        }
        // PSR_ColorBiasFactor (Engine/Inc/RHI.h:452) is float4 register 0.
        if (!KeepColorBias() && bytes >= 16) {
          float* c0 = reinterpret_cast<float*>(cpu);
          c0[0] = PixelC0Value();
          c0[1] = 0.0f;
          c0[2] = 0.0f;
          c0[3] = 0.0f;
        }
        if (C0ProbeEnabled()) {
          ProbePixelBank(dev_ps, cpu, bytes, d.ps->ucode_hash);
        }
        g_ps_cb_cached = handle;
        g_ps_cb_bytes = bytes;
        g_ps_rev_cached = ps_rev;
      }
      out.cb_pixel = g_ps_cb_cached;
      out.cb_pixel_bytes = g_ps_cb_bytes;
    } else {
      out.cb_pixel = g_vs_cb_cached;  // never read; the root CBV still needs an address
      out.cb_pixel_bytes = g_vs_cb_bytes;
    }
  }

  std::uint32_t shared_handle = kNoStage;
  std::uint8_t* shared_cpu = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    shared_cpu = g_arenas[g_stage_arena_index].Alloc(dpour_pipeline::kSharedConstantsSize, shared_handle);
  }
  if (shared_cpu == nullptr) {
    g_no_constants.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  {
    auto* sc = reinterpret_cast<dpour_pipeline::SharedConstants*>(shared_cpu);
    std::memset(sc, 0, sizeof(*sc));
    // Resolving all sixteen texture slots costs a cache lookup each; between two
    // draws of the same material nothing has changed, and the bound-slot key
    // says so exactly.
    static thread_local std::uint64_t tex_key = 0;
    static thread_local std::uint32_t tex_cached[32]{};
    const std::uint64_t key_now = dpour_tex::SamplerSlotsKey();
    if (key_now != tex_key) {
      dpour_tex::FillSamplerTable(base, tex_cached, 32);
      tex_key = key_now;
    }
    std::memcpy(sc->tex2d_index, tex_cached, sizeof(sc->tex2d_index));
    // Every view type indexes the same bindless heap, so a slot means the same
    // thing whichever of the four spaces reads it.
    std::memcpy(sc->tex3d_index, tex_cached, sizeof(sc->tex3d_index));
    std::memcpy(sc->texcube_index, tex_cached, sizeof(sc->texcube_index));
    // Cheap enough to redo every draw (sixteen relaxed loads), and the filter or
    // address mode can change without any texture being rebound.
    dpour_pipeline::FillSamplerIndices(sc->sampler_index, 32);
    sc->swapped_texcoords = dpour_pipeline::DeclSwappedTexcoords(layout);
    sc->normal_mode = static_cast<std::uint32_t>(dpour_pipeline::DeclNormalMode(layout));
    const std::uint32_t w = g_view_w.load(std::memory_order_relaxed);
    const std::uint32_t h = g_view_h.load(std::memory_order_relaxed);
    sc->half_pixel_offset_x = 1.0f / static_cast<float>(w);
    sc->half_pixel_offset_y = -1.0f / static_cast<float>(h);
    sc->alpha_test = state.alpha_test ? 1u : 0u;
    sc->alpha_threshold = state.alpha_ref;
    // Bool/loop banks straight from the device shadow. A shader whose loop
    // exit tests b{N} needs the real bit or it spins to the 1024-iteration
    // guard; a shader branching on b{N} needs it to pick the right material
    // path at all.
    static_assert(sizeof(sc->bools) == sizeof(state.bools), "bool bank size");
    static_assert(sizeof(sc->loops) == sizeof(state.loops), "loop bank size");
    std::memcpy(sc->bools, state.bools, sizeof(sc->bools));
    std::memcpy(sc->loops, state.loops, sizeof(sc->loops));
  }
  out.cb_shared = shared_handle;
  return true;
}

// One UP draw whose state is snapshotted but whose bytes the game is still
// writing. Guest-render-thread only (the same thread runs Begin, the inlined
// End and every event that closes this), so no lock guards the slot itself.
struct PendingUP {
  bool active = false;
  const std::uint8_t* base = nullptr;  // stable for the process lifetime
  SceneDraw draw;              // complete except the geometry bytes
  std::uint8_t* vtx_dst = nullptr;   // ring slice the bytes get swapped into
  std::uint32_t vtx_guest = 0;
  std::uint32_t vtx_bytes = 0;
  std::uint8_t* idx_dst = nullptr;
  std::uint32_t idx_guest = 0;
  std::uint32_t idx_count = 0;       // guest indices (pre-expansion)
  bool idx_32bit = false;
  bool expand_quads = false;         // QUADLIST: 4 guest entries -> 6 ours
};
PendingUP g_up_pending;

// Copies the now-complete bytes and files the draw into its pass. Runs on the
// guest render thread at the next captured event after Begin - any draw, the
// next Begin, a render-target switch, or the end of the frame. The spans are
// probed directly (not through the memoised ObjectReadable, which keys on the
// address alone and would let a 20-byte probe vouch for a kilobyte span).
void CloseUPPending() {
  if (!g_up_pending.active) {
    return;
  }
  PendingUP& p = g_up_pending;
  p.active = false;
  const std::uint8_t* base = p.base;
  if (base == nullptr || !SpanReadable(base + p.vtx_guest, p.vtx_bytes)) {
    g_up_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // UE3's UP vertices are all 4-byte fields (float positions and UVs, packed
  // colours), the same convention the captured-buffer path uploads with.
  {
    const std::uint8_t* src = base + p.vtx_guest;
    std::uint8_t* dst = p.vtx_dst;
    for (std::uint32_t i = 0; i < p.vtx_bytes; i += 4) {
      const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(src + i);
      *reinterpret_cast<std::uint32_t*>(dst + i) = _byteswap_ulong(v);
    }
  }
  if (p.idx_guest != 0) {
    const std::uint32_t elem = p.idx_32bit ? 4u : 2u;
    if (!SpanReadable(base + p.idx_guest, p.idx_count * elem)) {
      g_up_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const std::uint8_t* src = base + p.idx_guest;
    if (p.expand_quads) {
      // Each 4 consecutive guest entries describe one quad; emit two triangles
      // with the winding the quad order implies.
      const std::uint32_t quads = p.idx_count / 4u;
      if (p.idx_32bit) {
        auto* dst = reinterpret_cast<std::uint32_t*>(p.idx_dst);
        for (std::uint32_t q = 0; q < quads; ++q) {
          std::uint32_t v[4];
          for (std::uint32_t k = 0; k < 4; ++k) {
            v[k] = _byteswap_ulong(*reinterpret_cast<const std::uint32_t*>(src + (q * 4 + k) * 4));
          }
          *dst++ = v[0]; *dst++ = v[1]; *dst++ = v[2];
          *dst++ = v[0]; *dst++ = v[2]; *dst++ = v[3];
        }
      } else {
        auto* dst = reinterpret_cast<std::uint16_t*>(p.idx_dst);
        for (std::uint32_t q = 0; q < quads; ++q) {
          std::uint16_t v[4];
          for (std::uint32_t k = 0; k < 4; ++k) {
            v[k] = _byteswap_ushort(*reinterpret_cast<const std::uint16_t*>(src + (q * 4 + k) * 2));
          }
          *dst++ = v[0]; *dst++ = v[1]; *dst++ = v[2];
          *dst++ = v[0]; *dst++ = v[2]; *dst++ = v[3];
        }
      }
    } else if (p.idx_32bit) {
      auto* dst = reinterpret_cast<std::uint32_t*>(p.idx_dst);
      for (std::uint32_t i = 0; i < p.idx_count; ++i) {
        dst[i] = _byteswap_ulong(*reinterpret_cast<const std::uint32_t*>(src + i * 4));
      }
    } else {
      auto* dst = reinterpret_cast<std::uint16_t*>(p.idx_dst);
      for (std::uint32_t i = 0; i < p.idx_count; ++i) {
        dst[i] = _byteswap_ushort(*reinterpret_cast<const std::uint16_t*>(src + i * 2));
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_staging.size() >= kMaxDrawsPerFrame) {
      g_overflow.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_staging.push_back(p.draw);
  }
  g_up_captured.fetch_add(1, std::memory_order_relaxed);
  g_drawn.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool CaptureDraw(const std::uint8_t* base, const DrawDesc& d) {
  if (!Enabled() || base == nullptr) {
    return false;
  }
  CloseUPPending();
  g_seen.fetch_add(1, std::memory_order_relaxed);
  const std::int64_t started = TickNow();
  struct Charge {
    std::int64_t started;
    ~Charge() {
      g_capture_ticks.fetch_add(static_cast<std::uint64_t>(TickNow() - started),
                                std::memory_order_relaxed);
    }
  } charge{started};

  // Count every draw against its pass - that is what decides which pass is the
  // scene - but resolve only the pass we actually render. The counting has to
  // happen for all of them or the choice would be self-fulfilling.
  DXGI_FORMAT pass_rtv_format = kColorFormat;
  bool pass_no_depth = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pass_current < kMaxPasses) {
      ++g_passes[g_pass_current].draws;
      // The format of the target this draw's pass is bound to, latched at
      // DeviceSetRenderTarget from the surface's own record - the PSO below
      // must name it or D3D12 refuses the pipeline/target pair at replay.
      pass_rtv_format = g_passes[g_pass_current].rtv_format;
      pass_no_depth = g_passes[g_pass_current].no_depth;
    }
    // ONE TARGET MEANS ONE FORMAT. A pipeline that names a render-target format
    // the bound target does not have is a draw the hardware DISCARDS IN
    // SILENCE - no error, no debug-layer message unless it is switched on. We
    // have already lost a day to the depth half of this rule (a PSO naming a
    // DSV while none was bound turned every menu black); the colour half is the
    // same rule and it is why flat mode came out black while the log happily
    // reported submitting draws of 101550 indices. Every draw's PSO carried its
    // ORIGINAL pass surface's format, and flat mode binds one target of ours.
    if (FlatSceneMode()) {
      pass_rtv_format = kColorFormat;
      pass_no_depth = false;
    }
    // Reference shape: EVERY pass is captured and rendered into its own native
    // target. The old "resolve only the voted pass" filter is gone - it was
    // the reason shadows, post-processing and the UI never existed natively.
  }

  if (d.vs == nullptr) {
    g_no_shader.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ID3D12Device* device = g_device.load(std::memory_order_relaxed);
  if (device == nullptr) {
    g_no_device.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  D3D12_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  if (!Topology(d.prim_type, topology, topology_type)) {
    g_no_topology.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const dpour_decl::Layout& decl = dpour_decl::Resolve(base, d.decl_guest);
  const dpour_pipeline::DeclLayout* layout = dpour_pipeline::GetDeclLayout(decl);
  if (layout == nullptr) {
    g_no_layout.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  dpour_state::Pipeline state;
  if (!dpour_state::Read(base, state)) {
    g_no_device.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (g_invert_z.load(std::memory_order_relaxed) < 0) {
    g_invert_z.store(dpour_state::InvertedDepth(base) ? 1 : 0, std::memory_order_relaxed);
  }

  // --- pipeline ---------------------------------------------------------------
  dpour_pipeline::PsoKey key{};
  key.vs_hash = d.vs->ucode_hash;
  key.ps_hash = d.ps != nullptr ? d.ps->ucode_hash : 0;
  key.decl_hash = dpour_pipeline::DeclHash(layout);
  // UE3's depth prepass binds no pixel shader. Reproducing it as a depth-only
  // pipeline is not a special case bolted on here - it is what the game does,
  // and it is what fills our depth buffer so the base pass tests against the
  // same occlusion the guest sees.
  key.rtv_format = d.ps != nullptr ? static_cast<std::uint32_t>(pass_rtv_format) : 0;
  // A pass the replay binds without a DSV must not name one in the PSO: a
  // pipeline that declares a depth format while no depth buffer is bound is a
  // draw the hardware silently discards. That was every menu and UI quad, the
  // moment the back buffer became the presenter's image.
  if (pass_no_depth) {
    key.dsv_format = 0;
    key.depth_enable = 0;
    key.depth_write = 0;
  } else {
    key.dsv_format = kDepthFormat;
    key.depth_enable = state.depth_enable ? 1 : 0;
    key.depth_write = state.depth_write ? 1 : 0;
  }
  key.depth_func = static_cast<std::uint8_t>(state.depth_func);
  // DPOUR_NR_DRAW_NOCULL: draw both faces. If a scene that renders nothing
  // starts rendering with this on, the winding or the cull field is wrong -
  // which looks identical to "the shaders write black" from the outside.
  key.cull_mode = NoCull() ? static_cast<std::uint8_t>(D3D12_CULL_MODE_NONE)
                           : static_cast<std::uint8_t>(state.cull_mode);
  key.blend_enable = state.blend_enable ? 1 : 0;
  key.src_blend = static_cast<std::uint8_t>(state.src_blend);
  key.dst_blend = static_cast<std::uint8_t>(state.dst_blend);
  key.blend_op = static_cast<std::uint8_t>(state.blend_op);
  key.topology = static_cast<std::uint8_t>(topology_type);
  key.color_write_mask = ColorWriteMaskOn()
                             ? static_cast<std::uint8_t>(state.color_write_mask)
                             : static_cast<std::uint8_t>(D3D12_COLOR_WRITE_ENABLE_ALL);
  key.front_ccw = state.front_ccw ? 1 : 0;
  key.src_blend_alpha = static_cast<std::uint8_t>(state.src_blend_alpha);
  key.dst_blend_alpha = static_cast<std::uint8_t>(state.dst_blend_alpha);
  key.blend_op_alpha = static_cast<std::uint8_t>(state.blend_op_alpha);
  const std::uint32_t stream_mask = dpour_pipeline::DeclStreamMask(layout);
  // The stream shadow only follows the four streams the RHI binds. A declaration
  // reading beyond them would leave an input slot unbound, which reads garbage
  // vertices rather than failing - so the draw goes back to the guest instead.
  if ((stream_mask >> kMaxStreams) != 0) {
    g_no_stream.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (std::uint32_t s = 0; s < kMaxStreams && s < dpour_pipeline::kMaxStreams; ++s) {
    key.strides[s] = static_cast<std::uint16_t>(d.stride[s]);
  }

  ID3D12PipelineState* pso = dpour_pipeline::GetPso(device, key, d.vs, d.ps, layout);
  if (pso == nullptr) {
    g_no_pso.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  SceneDraw out;
  out.pso = pso;
  out.topology = topology;
  out.indexed = d.indexed;
  out.vs_hash = key.vs_hash;
  out.ps_hash = key.ps_hash;

  // --- geometry ---------------------------------------------------------------
  // The exact vertex range comes from the RHI call itself
  // (RHIDrawIndexedPrimitive's MinIndex + NumVertices, XeD3DCommands.cpp:855) -
  // no scanning of index data to work out what a section touches.
  // RHIDrawIndexedPrimitive's MinIndex/NumVertices are NOT dependable here: on
  // most of Downpour's draws they arrive as nonsense (over a million vertices
  // for a section of a few hundred), which is consistent with the caller not
  // setting every argument register on this path. So the range is treated as a
  // hint that may be absent, and the buffer's own size is what actually sizes
  // the upload. Rejecting on the hint alone threw away 51377 of the world's
  // draws per session while the data needed to draw them was available.
  const std::uint32_t range_top =
      d.indexed ? (d.min_index + d.num_vertices) : (d.start_vertex + d.vertex_count);
  const bool range_usable = range_top != 0 && range_top <= (1u << 20);
  const std::uint32_t top_vertex = range_usable ? range_top : 0;

  for (std::uint32_t s = 0; s < kMaxStreams; ++s) {
    if ((stream_mask & (1u << s)) == 0) {
      continue;
    }
    // A stride of ZERO is a legal binding, not a missing one: it means every
    // vertex reads the same element, which is how UE3 feeds per-instance and
    // per-object data through a vertex stream. Treating it as "not bound" is
    // what dropped 20592 of the world's draws a session - the log said
    // "strides=12,24,0,8" for a declaration using all four streams, and stream 2
    // was bound perfectly well.
    //
    // What genuinely cannot be drawn is a stream with no buffer behind it.
    if (d.vb_d3d[s] == 0 && d.vb_guest[s] == 0) {
      // This is the single biggest reason draws are dropped, and two rounds of
      // reasoning about it were wrong, so it reports itself instead.
      if (LogSubmissions()) {
        static std::atomic<std::uint32_t> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 12) {
          REXLOG_INFO(
              "[native-scene] stream {} unbound: mask={:#x} strides={},{},{},{} "
              "rhi_vb={:#x},{:#x},{:#x},{:#x} d3d_vb={:#x},{:#x},{:#x},{:#x} decl={:#x} "
              "elements={}",
              s, stream_mask, d.stride[0], d.stride[1], d.stride[2], d.stride[3], d.vb_guest[0],
              d.vb_guest[1], d.vb_guest[2], d.vb_guest[3], d.vb_d3d[0], d.vb_d3d[1], d.vb_d3d[2],
              d.vb_d3d[3], d.decl_guest, decl.element_count);
          for (std::uint32_t e = 0; e < decl.element_count && e < 8; ++e) {
            REXLOG_INFO("[native-scene]   element {}: stream={} offset={} type={} usage={}.{}", e,
                        decl.elements[e].stream, decl.elements[e].offset, decl.elements[e].type,
                        decl.elements[e].usage, decl.elements[e].usage_index);
          }
        }
      }
      g_no_stream.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // THE API ALREADY ANSWERED THIS. When the stream was paired - the D3D object
    // came out of RHISetStreamSource for this same stream - the bound buffer is
    // the RHI object, and FXeGPUResource::BaseAddress at +12 is where its data
    // lives, because AllocVertexBuffer put it there
    // (XeD3DVertexBuffer.cpp:63 XGOffsetResourceAddress(Resource, BaseAddress)).
    //
    // This is what owning the resource would buy, obtained without owning it:
    // the reference reads buffer->buffer and buffer->dataSize off its own struct
    // and never looks at a fetch constant. The fetch is consulted below only for
    // a SIZE, and is allowed to disagree without costing the draw - it cannot
    // contradict the game's own field about where the data is.
    BufferFetch fetch{};
    std::uint8_t fail = kVbFailNone;
    const bool have_fetch = ResolveBuffer(base, d.vb_guest[s], d.vb_d3d[s], fetch, &fail);
    std::uint32_t data_addr = 0;
    if (d.vb_paired[s] != 0 && GuestAddrPlausible(d.vb_guest[s]) &&
        ObjectReadable(base, d.vb_guest[s], 20)) {
      const std::uint32_t ba = LoadBE32(base + d.vb_guest[s] + 12);
      if (GuestDataAddrPlausible(ba)) {
        data_addr = ba;
      }
    }
    if (data_addr == 0) {
      // Unpaired: a raw device bind, which is FGPUMemMove's defrag and nothing
      // else we want. Without the pairing there is no object to believe, so the
      // fetch remains the only evidence - and if it does not agree with an RHI
      // object, the draw is not ours to reproduce.
      if (!have_fetch) {
        g_size_unknown.fetch_add(1, std::memory_order_relaxed);
        g_no_vbobj.fetch_add(1, std::memory_order_relaxed);
        NoteVbObjFail(base, s, d.vb_guest[s], d.vb_d3d[s], d.stride[s], fail);
        return false;
      }
      data_addr = fetch.addr;
    }
    // A size from the fetch counts only when it describes THIS buffer.
    if (!have_fetch || fetch.addr != data_addr) {
      fetch.size = 0;
    }
    fetch.addr = data_addr;
    g_size_known.fetch_add(1, std::memory_order_relaxed);
    // The RHI object carries the usage flags that say whether the game repacks
    // this buffer; when the draw only reached us through the device it is
    // treated as static, which is what cooked geometry is.
    std::uint32_t usage = 0;
    if (GuestAddrPlausible(d.vb_guest[s]) && ObjectReadable(base, d.vb_guest[s], 20)) {
      usage = LoadBE32(base + d.vb_guest[s] + 16);
    }
    const std::uint32_t drawn =
        range_usable ? (d.stream_offset[s] + (top_vertex + 1u) * d.stride[s]) : 0u;
    const std::uint32_t need = fetch.size > drawn ? fetch.size : drawn;
    dpour_vbuf::View view;
    if (!dpour_vbuf::AcquireBuffer(device, base, fetch.addr, need, false, usage, view) ||
        view.size_bytes <= d.stream_offset[s]) {
      g_no_vbdata.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Bound at the stream index, NOT packed in order: the input layout's
    // InputSlot is the guest's own stream number, so a declaration that reads
    // only stream 1 must leave slot 0 empty. Packing them would feed every
    // attribute from the wrong buffer.
    D3D12_VERTEX_BUFFER_VIEW& v = out.vbv[s];
    v.BufferLocation = view.gpu_address + d.stream_offset[s];
    v.SizeInBytes = view.size_bytes - d.stream_offset[s];
    v.StrideInBytes = d.stride[s];
    if (s + 1 > out.vbv_count) {
      out.vbv_count = s + 1;
    }
  }
  if (out.vbv_count == 0) {
    g_no_stream.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  if (d.indexed) {
    if (!GuestAddrPlausible(d.ib_guest)) {
      g_no_ibobj.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!ObjectReadable(base, d.ib_guest, 20)) {
      g_no_ibobj.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const std::uint32_t usage = LoadBE32(base + d.ib_guest + 16);
    // The index range is the one the device call gives directly, so it is always
    // usable; the buffer's own size only ever extends it.
    const std::uint32_t drawn = (d.start_index + d.index_count) * 2u;
    BufferFetch fetch;
    const bool have = ResolveBuffer(base, d.ib_guest, 0, fetch);
    const std::uint32_t data = have ? fetch.addr : LoadBE32(base + d.ib_guest + 12);
    const std::uint32_t need = (have && fetch.size > drawn) ? fetch.size : drawn;
    dpour_vbuf::View view;
    if (!dpour_vbuf::AcquireBuffer(device, base, data, need, true, usage, view)) {
      g_no_ibdata.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    out.ibv.BufferLocation = view.gpu_address;
    out.ibv.SizeInBytes = view.size_bytes;
    out.ibv.Format = DXGI_FORMAT_R16_UINT;
    out.count = d.index_count;
    out.start = d.start_index;
  } else {
    out.count = d.vertex_count;
    out.start = d.start_vertex;
  }

  if (!FillDrawConstants(base, d, state, layout, out)) {
    return false;
  }

  // Set under the lock, acted on after it: see the note at the assignment.
  bool probe_now = false;
  std::uint32_t probe_color = 0;
  std::uint32_t probe_depth = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_staging.size() >= kMaxDrawsPerFrame) {
      g_overflow.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // THE DRAW BELONGS TO THE TARGET THE GAME BOUND. Full stop - that is the
    // whole rule, and it is the reference's rule. What used to follow here (the
    // engine-driven admission that moved SDPG_World draws into a reserved scene
    // slot) is deleted: it decided which pass "is" the frame, and no reference
    // has such a decision because none can be right.
    out.pass = g_pass_current;
    g_staging.push_back(out);
  }
  if (probe_now) {
    ProbeVertexTransform(base, d, decl, probe_color, probe_depth, state);
  }
  g_drawn.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Defined here, below CloseUPPending, because that is where the pending slot
// lives. The API-level hooks call Begin then End back to back: the caller's
// buffer is already full, so there is nothing to wait for.
void CaptureUPEnd() { CloseUPPending(); }

void CaptureUPBegin(const std::uint8_t* base, const UPDraw& u) {
  if (!Enabled() || base == nullptr || NoUPCapture()) {
    return;
  }
  // A Begin can only follow the previous pair's (inlined) End, so whatever is
  // pending is complete.
  CloseUPPending();
  g_up_seen.fetch_add(1, std::memory_order_relaxed);
  if (g_up_frame_count.fetch_add(1, std::memory_order_relaxed) >= MaxUPPerFrame()) {
    g_up_capped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  std::uint32_t pass_now;
  DXGI_FORMAT pass_rtv_format = kColorFormat;
  bool pass_no_depth = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pass_now = g_pass_current;
    if (g_pass_current < kMaxPasses) {
      ++g_passes[g_pass_current].draws;
      pass_rtv_format = g_passes[g_pass_current].rtv_format;
      pass_no_depth = g_passes[g_pass_current].no_depth;
    }
  }
  // One target, one format - see the note in CaptureDraw.
  if (FlatSceneMode()) {
    pass_rtv_format = kColorFormat;
    pass_no_depth = false;
  }

  const DrawDesc& d = u.d;
  if (d.vs == nullptr) {
    g_no_shader.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ID3D12Device* device = g_device.load(std::memory_order_relaxed);
  if (device == nullptr) {
    g_no_device.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // The device caps the stride at 255 dwords (it stores stride>>2 in a byte),
  // and UP vertices are 4-byte fields throughout - anything else is not a UP
  // draw we understand.
  const std::uint32_t vtx_bytes = u.vtx_count * u.vtx_stride;
  if (u.vtx_count == 0 || u.vtx_stride == 0 || u.vtx_stride > 1020u ||
      (u.vtx_stride & 3u) != 0 || vtx_bytes > (4u << 20) ||
      !GuestAddrPlausible(u.vtx_guest)) {
    g_up_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // The UI draws QUADLISTs (GetD3DPrimitiveType maps PT_QuadList to 13), which
  // D3D12 does not have: each quad becomes two triangles through generated
  // indices, in the winding the quad's own vertex order implies.
  const bool quads = d.prim_type == 13u;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  if (quads) {
    topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  } else if (!Topology(d.prim_type, topology, topology_type)) {
    g_no_topology.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const dpour_decl::Layout& decl = dpour_decl::Resolve(base, d.decl_guest);
  const dpour_pipeline::DeclLayout* layout = dpour_pipeline::GetDeclLayout(decl);
  if (layout == nullptr) {
    g_no_layout.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // UP vertices arrive as one interleaved blob on stream 0; a declaration
  // reading any other stream does not describe this draw.
  const std::uint32_t stream_mask = dpour_pipeline::DeclStreamMask(layout);
  if ((stream_mask & ~1u) != 0) {
    g_no_stream.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  dpour_state::Pipeline state;
  if (!dpour_state::Read(base, state)) {
    g_no_device.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  dpour_pipeline::PsoKey key{};
  key.vs_hash = d.vs->ucode_hash;
  key.ps_hash = d.ps != nullptr ? d.ps->ucode_hash : 0;
  key.decl_hash = dpour_pipeline::DeclHash(layout);
  key.rtv_format = d.ps != nullptr ? static_cast<std::uint32_t>(pass_rtv_format) : 0;
  // Same rule as the indexed path: no DSV bound means no DSV named.
  if (pass_no_depth) {
    key.dsv_format = 0;
    key.depth_enable = 0;
    key.depth_write = 0;
  } else {
    key.dsv_format = kDepthFormat;
    key.depth_enable = state.depth_enable ? 1 : 0;
    key.depth_write = state.depth_write ? 1 : 0;
  }
  key.depth_func = static_cast<std::uint8_t>(state.depth_func);
  // DPOUR_NR_DRAW_NOCULL: draw both faces. If a scene that renders nothing
  // starts rendering with this on, the winding or the cull field is wrong -
  // which looks identical to "the shaders write black" from the outside.
  key.cull_mode = NoCull() ? static_cast<std::uint8_t>(D3D12_CULL_MODE_NONE)
                           : static_cast<std::uint8_t>(state.cull_mode);
  key.blend_enable = state.blend_enable ? 1 : 0;
  key.src_blend = static_cast<std::uint8_t>(state.src_blend);
  key.dst_blend = static_cast<std::uint8_t>(state.dst_blend);
  key.blend_op = static_cast<std::uint8_t>(state.blend_op);
  key.topology = static_cast<std::uint8_t>(topology_type);
  key.color_write_mask = ColorWriteMaskOn()
                             ? static_cast<std::uint8_t>(state.color_write_mask)
                             : static_cast<std::uint8_t>(D3D12_COLOR_WRITE_ENABLE_ALL);
  key.front_ccw = state.front_ccw ? 1 : 0;
  key.src_blend_alpha = static_cast<std::uint8_t>(state.src_blend_alpha);
  key.dst_blend_alpha = static_cast<std::uint8_t>(state.dst_blend_alpha);
  key.blend_op_alpha = static_cast<std::uint8_t>(state.blend_op_alpha);
  key.strides[0] = static_cast<std::uint16_t>(u.vtx_stride);

  ID3D12PipelineState* pso = dpour_pipeline::GetPso(device, key, d.vs, d.ps, layout);
  if (pso == nullptr) {
    g_no_pso.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  SceneDraw out;
  out.pso = pso;
  out.topology = topology;
  out.vs_hash = key.vs_hash;
  out.ps_hash = key.ps_hash;
  out.pass = pass_now;

  if (!FillDrawConstants(base, d, state, layout, out)) {
    return;
  }

  // Staged, like everything else the snapshot carries. The GPU vertex buffer is
  // created in the replay out of that frame's allocator.
  std::uint32_t vtx_handle = kNoStage;
  std::uint8_t* vtx_cpu = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    vtx_cpu = g_arenas[g_stage_arena_index].Alloc(vtx_bytes, vtx_handle);
  }
  if (vtx_cpu == nullptr) {
    g_no_vbdata.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  out.up_vtx = vtx_handle;
  out.up_vtx_bytes = vtx_bytes;
  out.vbv[0].SizeInBytes = vtx_bytes;
  out.vbv[0].StrideInBytes = u.vtx_stride;
  out.vbv_count = 1;

  PendingUP p;
  p.base = base;
  p.vtx_dst = vtx_cpu;
  p.vtx_guest = u.vtx_guest;
  p.vtx_bytes = vtx_bytes;

  const bool indexed_in = u.idx_guest != 0 && u.idx_count > 0;
  if (indexed_in || quads) {
    std::uint32_t issue_count;  // indices the native draw will issue
    if (quads) {
      const std::uint32_t entries = indexed_in ? u.idx_count : u.vtx_count;
      issue_count = (entries / 4u) * 6u;
    } else {
      issue_count = u.idx_count;
    }
    // A non-indexed quad list gets its indices generated over the vertex range;
    // they may need 32 bits only if the range does.
    bool fmt32 = indexed_in ? u.idx_32bit : (u.vtx_count > 0xFFFFu);
    const std::uint32_t issue_bytes = issue_count * (fmt32 ? 4u : 2u);
    if (issue_count == 0 || issue_count > (1u << 22) ||
        (indexed_in && !GuestAddrPlausible(u.idx_guest))) {
      g_up_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::uint32_t idx_handle = kNoStage;
    std::uint8_t* idx_cpu = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      idx_cpu = g_arenas[g_stage_arena_index].Alloc(issue_bytes, idx_handle);
    }
    if (idx_cpu == nullptr) {
      g_no_ibdata.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    out.up_idx = idx_handle;
    out.up_idx_bytes = issue_bytes;
    out.ibv.SizeInBytes = issue_bytes;
    out.ibv.Format = fmt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    out.indexed = true;
    out.count = issue_count;
    out.start = 0;
    out.base_vertex = u.base_vertex;
    if (quads && !indexed_in) {
      // These depend on nothing the game is still writing - fill them now.
      const std::uint32_t nq = u.vtx_count / 4u;
      if (fmt32) {
        auto* dst = reinterpret_cast<std::uint32_t*>(idx_cpu);
        for (std::uint32_t q = 0; q < nq; ++q) {
          const std::uint32_t v = q * 4u;
          *dst++ = v; *dst++ = v + 1; *dst++ = v + 2;
          *dst++ = v; *dst++ = v + 2; *dst++ = v + 3;
        }
      } else {
        auto* dst = reinterpret_cast<std::uint16_t*>(idx_cpu);
        for (std::uint32_t q = 0; q < nq; ++q) {
          const std::uint16_t v = static_cast<std::uint16_t>(q * 4u);
          *dst++ = v; *dst++ = v + 1; *dst++ = v + 2;
          *dst++ = v; *dst++ = v + 2; *dst++ = v + 3;
        }
      }
    } else {
      p.idx_dst = idx_cpu;
      p.idx_guest = u.idx_guest;
      p.idx_count = u.idx_count;
      p.idx_32bit = u.idx_32bit;
      p.expand_quads = quads;
    }
  } else {
    out.indexed = false;
    out.count = u.vtx_count;
    out.start = 0;
  }

  p.draw = out;
  p.active = true;
  g_up_pending = p;
}

namespace {

// Post-mortem for the DEVICE_HUNG hunt: the SDK's presenter aborts the process
// the moment it notices the loss, before the command processor's own DRED
// report can run - every crash log so far ends with nothing. This watchdog
// polls GetDeviceRemovedReason from its own thread and, on a loss, dumps the
// DRED auto-breadcrumbs (which command list got how far, which op hung) and
// the page-fault VA straight into our log, racing the presenter's abort.
// Costs one sleepy thread; only started when the draw path is enabled anyway.
void StartDeviceWatchdog(ID3D12Device* device) {
  static std::atomic<bool> started{false};
  if (started.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  // Holds its own reference: the watchdog outlives the renderer's shutdown,
  // and polling a destroyed device is a crash at exit (seen as SIGSEGV after
  // "Execution complete"). The process exiting with this thread parked is
  // fine; poking freed memory is not.
  Microsoft::WRL::ComPtr<ID3D12Device> guard(device);
  std::thread([device = std::move(guard)] {
    for (;;) {
      ::Sleep(250);
      const HRESULT reason = device->GetDeviceRemovedReason();
      if (SUCCEEDED(reason)) {
        continue;
      }
      REXLOG_ERROR("[native-scene] WATCHDOG: device removed, reason {:#010x}",
                   static_cast<std::uint32_t>(reason));
      Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
      if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
          for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode; n != nullptr;
               n = n->pNext) {
            if (n->pLastBreadcrumbValue == nullptr || n->pCommandHistory == nullptr) {
              continue;
            }
            const std::uint32_t done = *n->pLastBreadcrumbValue;
            // Fully completed or never started lists are not the hang.
            if (done == 0 || done >= n->BreadcrumbCount) {
              continue;
            }
            const wchar_t* list_name = n->pCommandListDebugNameW;
            REXLOG_ERROR("[native-scene] WATCHDOG: list {} hung at op {} of {}",
                         list_name != nullptr ? "named" : "unnamed", done, n->BreadcrumbCount);
            const std::uint32_t lo = done > 6 ? done - 6 : 0;
            const std::uint32_t hi =
                done + 3 < n->BreadcrumbCount ? done + 3 : n->BreadcrumbCount;
            for (std::uint32_t i = lo; i < hi; ++i) {
              REXLOG_ERROR("[native-scene]   op[{}] type {}{}", i,
                           static_cast<int>(n->pCommandHistory[i]), i == done ? "  <-- HERE" : "");
            }
          }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA != 0) {
          REXLOG_ERROR("[native-scene] WATCHDOG: page fault VA {:#018x}",
                       static_cast<std::uint64_t>(fault.PageFaultVA));
          // Print EVERY node, named or not. The previous version only printed
          // nodes carrying an ANSI name - our resources had no names at all, so
          // the whole fault dump came out as a bare address and said nothing.
          // The names come from SetName() at creation (dpour.vb / dpour.ib /
          // dpour.upload / dpour.tex / dpour.rt.*), which is what turns "a page
          // fault somewhere" into "this resource class was freed too early".
          const auto dump_node = [](const char* what, const D3D12_DRED_ALLOCATION_NODE* a) {
            char name[128] = "<unnamed>";
            if (a->ObjectNameA != nullptr) {
              std::snprintf(name, sizeof(name), "%s", a->ObjectNameA);
            } else if (a->ObjectNameW != nullptr) {
              std::snprintf(name, sizeof(name), "%ls", a->ObjectNameW);
            }
            REXLOG_ERROR("[native-scene]   {}: {} (type {})", what, name,
                         static_cast<int>(a->AllocationType));
          };
          for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadExistingAllocationNode;
               a != nullptr; a = a->pNext) {
            dump_node("existing allocation", a);
          }
          for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadRecentFreedAllocationNode;
               a != nullptr; a = a->pNext) {
            dump_node("RECENTLY FREED", a);
          }
        }
      }
      return;
    }
  }).detach();
}

// Composite `src` into the presenter's guest output image, at whatever size it
// asks for. This is the skate3 arrangement: render into context.guest_output as
// a render target and hand it back in kGuestOutput state
// (skate3_native_scene_gpu.cpp:11546). No intermediate texture, and above all
// no "are the two the same size" test - that test is what a resolution_scale of
// 2 failed on every single frame, yielding a perfectly healthy native frame to
// the emulator with nothing in the log to say so.
bool CompositeToGuestOutput(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* dl,
                            ID3D12Resource* out_res, std::uint32_t out_w, std::uint32_t out_h,
                            ID3D12Resource* src) {
  if (device == nullptr || dl == nullptr || out_res == nullptr || src == nullptr || out_w == 0 ||
      out_h == 0 || g_guestout_failed.load(std::memory_order_relaxed) || !g_comp_pso_guestout ||
      !g_comp_root || !g_srv_heap || !g_depth) {
    return false;
  }
  if (!g_guestout_rtv_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    h.NumDescriptors = kPassFrames;
    if (FAILED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&g_guestout_rtv_heap)))) {
      // Sticky, the way skate3 marks every creation failure (g_r.failed):
      // a path that cannot build its resources does not get to retry them once
      // per frame forever. It yields to the emulated output and stays there.
      REXLOG_ERROR("[native-scene] guest-output RTV heap creation failed");
      g_guestout_failed.store(true, std::memory_order_relaxed);
      return false;
    }
  }
  // Ring slot of its own: the composite runs on repeat callbacks too, where the
  // replay counter does not move, so keying the descriptors on that counter
  // would rewrite descriptors a still-recording submission is pointing at.
  static std::uint32_t comp_index = 0;
  const std::uint32_t ring = comp_index++ % kPassFrames;

  const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_guestout_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(ring) * rtv_inc;
  D3D12_RENDER_TARGET_VIEW_DESC rv{};
  rv.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(out_res, &rv, rtv);

  const UINT srv_inc =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT comp_slot = ring * 2u;
  D3D12_CPU_DESCRIPTOR_HANDLE comp_cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
  comp_cpu.ptr += static_cast<SIZE_T>(comp_slot) * srv_inc;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = kColorFormat;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src, &sv, comp_cpu);
  comp_cpu.ptr += srv_inc;
  sv.Format = kDepthSrvFormat;
  device->CreateShaderResourceView(g_depth.Get(), &sv, comp_cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE comp_gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
  comp_gpu.ptr += static_cast<UINT64>(comp_slot) * srv_inc;

  // The image arrives in kGuestOutputInternalState and must be returned to it.
  D3D12_RESOURCE_BARRIER to_rt{};
  to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_rt.Transition.pResource = out_res;
  to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_rt.Transition.StateBefore = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
  to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  dl->D3DResourceBarrier(1, &to_rt);

  dl->D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
  dl->SetDescriptorHeaps(g_srv_heap.Get(), nullptr);
  dl->D3DSetGraphicsRootSignature(g_comp_root.Get());
  dl->D3DSetGraphicsRootDescriptorTable(0, comp_gpu);
  struct {
    float depth_clear;
    std::uint32_t opaque;
    std::uint32_t depth_view;
    float amplify;
  } comp{g_depth_clear, 1u /* the replacement frame is always opaque */,
         dpour_pipeline::FlatShadingEnabled() ? 1u : 0u, AmplifyFactor()};
  dl->D3DSetGraphicsRoot32BitConstants(1, 4, &comp, 0);
  dl->D3DSetPipelineState(g_comp_pso_guestout.Get());
  const D3D12_VIEWPORT gvp{0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h),
                           0.0f, 1.0f};
  const D3D12_RECT gsc{0, 0, static_cast<LONG>(out_w), static_cast<LONG>(out_h)};
  dl->RSSetViewport(gvp);
  dl->RSSetScissorRect(gsc);
  dl->D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dl->D3DIASetVertexBuffers(0, 0, nullptr);
  dl->D3DIASetIndexBuffer(nullptr);
  // DPOUR_NR_DRAW_GOTEST: paint the replacement frame a flat colour and skip the
  // composite entirely. It separates "the guest-output replacement reaches the
  // screen" from "the composite produces black" - two failures that look
  // identical and had already cost several runs of guessing between them.
  static const bool go_test = EnvFlag("DPOUR_NR_DRAW_GOTEST");
  if (go_test) {
    const float flat[4] = {0.35f, 0.0f, 0.5f, 1.0f};
    dl->D3DClearRenderTargetView(rtv, flat, 0, nullptr);
  } else {
    dl->D3DDrawInstanced(3, 1, 0, 0);
  }

  D3D12_RESOURCE_BARRIER back = to_rt;
  back.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  back.Transition.StateAfter = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
  dl->D3DResourceBarrier(1, &back);
  return true;
}

}  // namespace

// The presenter's per-present entry. It no longer draws anything: the replay
// happens inside the guest-output callback, on the command processor's own
// list. What stays here is what only the presenter can give us - the D3D12
// device - plus registering that callback once.
void Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd,
            std::uint32_t width, std::uint32_t height) {
  if (!Enabled() || device == nullptr) {
    return;
  }
  (void)queue;
  g_device.store(device, std::memory_order_relaxed);
  StartDeviceWatchdog(device);
  static std::atomic<bool> registered{false};
  if (!registered.exchange(true, std::memory_order_relaxed)) {
    rex::graphics::SetNativeGuestOutputRenderer(&GuestOutputRenderCallback, nullptr);
    REXLOG_INFO("[native-scene] guest-output renderer registered (skate3-sdk hook)");
  }
  dpour_tex::EnsureHeap(device);
  dpour_pipeline::Init(device);
  (void)cmd;
  (void)width;
  (void)height;
}

// The replay, recorded onto the command processor's deferred list. `cmd` is the
// presenter's own list and is non-null ONLY on the legacy overlay path; from
// the guest-output callback it is null and the function returns after the
// composite. Returns true when the composite went into `out_res`.
bool RenderRecorded(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* dl,
                    ID3D12GraphicsCommandList* cmd, std::uint32_t width, std::uint32_t height,
                    ID3D12Resource* out_res, std::uint64_t submission, std::uint64_t completed) {
  if (!Enabled() || device == nullptr || dl == nullptr) {
    return false;
  }
  // Sticky failure first, exactly where skate3 puts it (EnsurePipeline opens
  // with `if (g_r.failed) return false;`). Once any resource this path needs
  // could not be built, the emulated output is the frame - permanently, not
  // until the next retry.
  if (g_reg_failed || g_guestout_failed.load(std::memory_order_relaxed)) {
    return false;
  }
  g_device.store(device, std::memory_order_relaxed);
  StartDeviceWatchdog(device);
  if (GuestOutMode()) {
    static std::atomic<bool> registered{false};
    if (!registered.exchange(true, std::memory_order_relaxed)) {
      rex::graphics::SetNativeGuestOutputRenderer(&GuestOutputRenderCallback, nullptr);
      REXLOG_INFO("[native-scene] guest-output renderer registered (skate3-sdk hook)");
    }
  }
  dpour_tex::EnsureHeap(device);
  if (!dpour_pipeline::Init(device)) {
    return false;
  }

  // The replay's upload memory for this submission.
  dpour_pipeline::BeginReplay(device, submission, completed);

  const std::int64_t render_started = TickNow();
  struct RenderCharge {
    std::int64_t started;
    ~RenderCharge() {
      g_render_ticks.fetch_add(static_cast<std::uint64_t>(TickNow() - started),
                               std::memory_order_relaxed);
      g_render_frames.fetch_add(1, std::memory_order_relaxed);
    }
  } render_charge{render_started};

  // (The readback/injection pipeline is retired - the reference-style target
  // registry below replaces it. The worker machinery stays compiled but idle.)

  static thread_local std::vector<SceneDraw> items;
  static thread_local std::vector<PendingResolve> resolves;
  const CpuArena* arena = nullptr;
  PassSlot passes_snap[kMaxPasses];
  std::uint64_t items_frame = 0;
  static thread_local std::vector<PendingClear> clears;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    items = g_published;
    resolves = g_resolve_published;
    clears = g_clear_published;
    arena = &g_arenas[g_published_arena_index];
    std::memcpy(passes_snap, g_published_passes, sizeof(passes_snap));
    items_frame = g_published_frame;
  }
  if (items.empty()) {
    // Title screen, menu, loading, movie: nothing of the scene this frame, so
    // leave the frame exactly as the game drew it.
    return false;
  }
  // ...and the same question asked properly (see YieldWithoutSceneOn): a frame
  // with draws in it is not a frame with a SCENE in it. The intro movies have
  // draws and no scene, and publishing them is what puts white on screen.
  // ...but NOT when we own the device. Yielding means "let the emulated frame
  // through", and in own-device mode there is no emulated frame to let through -
  // the guest's draws never reached the GPU. Yielding there would show nothing.
  if (YieldWithoutSceneOn() && !OwnDeviceMode()) {
    bool scene_drew = false;
    for (std::uint32_t p = 0; p < kMaxPasses; ++p) {
      if (passes_snap[p].draws != 0 && IsSceneColorSurface(passes_snap[p].color_object)) {
        scene_drew = true;
        break;
      }
    }
    if (!scene_drew) {
      static std::atomic<std::uint64_t> yielded{0};
      const std::uint64_t n = yielded.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == 1 || n == 600 || n == 6000) {
        REXLOG_INFO("[native-scene] yielding to the emulated frame x{}: {} draws captured but "
                    "none in the game's scene surface",
                    n, items.size());
      }
      return false;
    }
  }
  // EVERY CALLBACK REPLAYS IN FULL, the way skate3's RenderScene does - and it
  // is now safe to, which it was not before.
  //
  // The snapshot no longer holds GPU addresses. Its constants and its
  // user-primitive geometry are BYTES in the staging arena, and each replay
  // copies them into that frame's upload allocator and binds the address it
  // gets back. A second replay of the same snapshot therefore reads the same
  // bytes, not whatever has since moved into an address recycled underneath it.
  //
  // The whole cast of workarounds that stood here goes with it: one replay per
  // published frame, declining the callbacks in between, and the invention that
  // followed - re-compositing the target the last replay left, which froze the
  // picture and painted a loading frame over the menu.
  //
  // The per-target clear keys on the REPLAY, not the published frame, so a
  // second replay starts from a clean target rather than drawing on top of the
  // first.
  static std::uint64_t replay_index = 0;
  ++replay_index;

  // Targets are created only once there is something to draw, and not before:
  // their depth clear value depends on whether the game runs a reversed depth
  // buffer, which is not known until a draw has been captured. Creating them on
  // the first frame instead baked in the wrong end of the depth range and every
  // later comparison failed.
  // FLAT MODE SIZES THE TARGET BY THE OUTPUT, NOT BY A VOTE. g_published_w/h is
  // the winning pass's viewport, and measured 28.07 that vote picked a 512x16
  // utility pass: the whole frame was rendered into a sixteen-pixel-tall strip
  // and blitted up to the screen, which is the smeared horizontal band the user
  // photographed. skate3 has no such quantity - its scene target is
  // guest_output sized (skate3_native_scene_gpu.cpp:3993 EnsureOutputSizedTargets)
  // because the frame is the frame.
  const bool flat_mode = FlatSceneMode();
  const std::uint32_t tw =
      flat_mode ? width : g_published_w.load(std::memory_order_relaxed);
  const std::uint32_t th =
      flat_mode ? height : g_published_h.load(std::memory_order_relaxed);
  if (!EnsureTargets(device, tw, th) || !EnsureComposite(device)) {
    return false;
  }

  if (!g_logged_first.exchange(true)) {
    REXLOG_INFO(
        "[native-scene] FIRST NATIVE FRAME FROM THE GAME'S OWN SHADERS - {} draws, {}x{}, "
        "translated DXIL + guest vertex declarations + guest constants",
        items.size(), tw, th);
  }
  // How many draws actually reach the screen each frame - the number that was
  // missing while everything else looked healthy.
  {
    static std::atomic<std::uint64_t> n{0};
    const std::uint64_t i = n.fetch_add(1, std::memory_order_relaxed);
    if (i < 3 || (i % 600) == 0) {
      REXLOG_INFO("[native-scene] drawing {} draws this frame at {}x{}", items.size(), tw, th);
    }
  }
  // IS THE SNAPSHOT STILL MOVING? A frozen picture and a correctly-replayed one
  // are the same log line above: both say "drawing N draws". The published guest
  // frame is what separates them - if it stops advancing while the callbacks
  // keep coming, we are replaying a dead snapshot, and the screen shows the
  // frame the game drew before it stopped publishing. That is precisely what
  // "the menu shows the previous frame" looks like from in here.
  {
    static std::uint64_t last_seen = ~0ull;
    static std::uint32_t repeats = 0;
    if (items_frame == last_seen) {
      ++repeats;
      if (repeats == 120 || repeats == 600 || repeats == 3000) {
        REXLOG_WARN("[native-scene] SNAPSHOT FROZEN: guest frame {} replayed {} times - the game "
                    "has published nothing new ({} draws held)",
                    items_frame, repeats, items.size());
      }
    } else {
      if (repeats >= 120) {
        REXLOG_INFO("[native-scene] snapshot moving again: frame {} after {} repeats of {}",
                    items_frame, repeats, last_seen);
      }
      repeats = 0;
      last_seen = items_frame;
    }
  }

  // NO COMMAND LIST OF OUR OWN, NO SUBMISSION OF OUR OWN.
  //
  // Everything below is recorded onto the command processor's deferred list -
  // the same list the rest of the frame is recorded on, submitted once by the
  // command processor. What used to stand here (our own allocator ring, our own
  // ID3D12GraphicsCommandList, our own ExecuteCommandLists + Signal on the
  // runtime's queue) was a SECOND timeline running beside it, which no
  // reference has: skate3 records into the callback's list on this very SDK,
  // UnleashedRecomp and MarathonRecomp into their one render thread's list.
  // Two timelines mean two independent notions of "when is this resource free",
  // and that is the whole use-after-free / device-hang class.
  const std::uint32_t slot = g_pass_index % kPassFrames;

  // Uploads first: every buffer and texture a draw refers to was staged this
  // frame and must be copied before the draws that read it.
  dpour_vbuf::Flush(device, dl);
  dpour_tex::Flush(device, dl);

  if (!EnsureRegHeaps(device)) {
    return false;
  }
  // EVERY SURFACE A RESOLVE POINTS AT GETS ITS TEXTURE HERE - the reference's
  // CreateSurface, moved to the first moment we have a device. Until this
  // existed, a surface only had an SRV once a draw had landed in it, so the
  // scene's own resolve destination sampled white on every frame that drew the
  // world into a pass we had not yet built a target for.
  // Retirements first: a surface destroyed and one created in the same frame
  // must give its slot up before the new one asks for one, or a registry that
  // is exactly full stays full for no reason.
  {
    std::vector<std::uint32_t> retiring;
    {
      std::lock_guard<std::mutex> lk(g_reg_mutex);
      retiring.swap(g_pending_surface_retire);
    }
    for (const std::uint32_t key : retiring) {
      RetireRegTarget(key);
    }
  }
  {
    std::vector<std::uint32_t> pending;
    {
      std::lock_guard<std::mutex> lk(g_reg_mutex);
      pending.swap(g_pending_surface_targets);
    }
    for (const std::uint32_t key : pending) {
      std::uint32_t sw = 0, sh = 0;
      DXGI_FORMAT sfmt = kColorFormat;
      bool is_depth = false;
      {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        const auto meta = g_surface_meta.find(key);
        if (meta == g_surface_meta.end()) {
          continue;  // never saw it created; its size is not ours to invent
        }
        sw = meta->second.w;
        sh = meta->second.h;
        is_depth = meta->second.is_depth;
        if (SurfaceFormatOn() && meta->second.dxgi != DXGI_FORMAT_UNKNOWN) {
          sfmt = meta->second.dxgi;
        }
      }
      // Depth resolves are never sampled as colour, and the reference does not
      // copy them at all ("depth stencil textures in this game are guaranteed
      // to be transient", video.cpp:3572).
      if (is_depth || sw == 0 || sh == 0) {
        continue;
      }
      GetOrCreateRegTarget(device, key, sw, sh, true, sfmt);
    }
  }
  // The scene target's bindless SRV, so textures the game resolves the scene
  // into sample OUR scene. Recreated in place whenever g_color is rebuilt.
  {
    static std::uint32_t scene_bindless = dpour_tex::kInvalidSlot;
    static ID3D12Resource* scene_srv_res = nullptr;
    if (scene_srv_res != g_color.Get()) {
      if (scene_bindless == dpour_tex::kInvalidSlot) {
        scene_bindless = dpour_tex::AllocDescriptorSlot();
      }
      if (scene_bindless != dpour_tex::kInvalidSlot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = kColorFormat;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh{};
        sh.ptr = static_cast<std::size_t>(dpour_tex::CpuHandleAt(scene_bindless));
        device->CreateShaderResourceView(g_color.Get(), &sv, sh);
        scene_srv_res = g_color.Get();
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        for (const auto& slot : g_scene_color_aliases) {
          const std::uint32_t alias = slot.load(std::memory_order_relaxed);
          if (alias == 0) {
            break;
          }
          g_surface_srv[alias] = scene_bindless;
        }
      }
    }
  }

  // Depth stays owned by the scene pass for the whole list (registry targets
  // carry their own depth, never sampled yet).
  D3D12_RESOURCE_BARRIER depth_to_write{};
  depth_to_write.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  depth_to_write.Transition.pResource = g_depth.Get();
  depth_to_write.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  depth_to_write.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  depth_to_write.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  dl->D3DResourceBarrier(1, &depth_to_write);

  // DPOUR_NR_DRAW_CLEARTINT: clear the targets to something that is obviously
  // not the game, to separate "our target is black" from "the composite is not
  // showing our target". A black screen is the same picture either way, and
  // guessing between the two has already cost several runs.
  static const bool clear_tint = EnvFlag("DPOUR_NR_DRAW_CLEARTINT");
  const float clear_color[4] = {clear_tint ? 0.25f : 0.0f, 0.0f, clear_tint ? 0.35f : 0.0f,
                                clear_tint ? 1.0f : 0.0f};

  ID3D12DescriptorHeap* heaps[2] = {dpour_tex::Heap(), dpour_pipeline::SamplerHeap()};
  if (heaps[0] == nullptr || heaps[1] == nullptr) {
    return false;
  }
  dl->SetDescriptorHeaps(heaps[0], heaps[1]);
  dl->D3DSetGraphicsRootSignature(dpour_pipeline::RootSignature());
  dpour_pipeline::BindDescriptorTables(dl, dpour_tex::GpuHandleStart());

  // THE STATE LIVES ON THE TARGET. This is UnleashedRecomp's AddBarrier
  // (video.cpp:753) with its one essential property: it does NOTHING when the
  // resource is already in the requested state.
  //
  //     if (texture->layout != layout) { g_barrierMap[...] = layout; texture->layout = layout; }
  //
  // Ours hardcoded every StateBefore and wrote NativeTarget::color_state without
  // ever reading it back, so a target already in PIXEL_SHADER_RESOURCE was told
  // it was in RENDER_TARGET. A transition whose StateBefore does not match the
  // resource's actual state is not a hint the runtime corrects - it is undefined
  // behaviour, and what the driver does with the contents afterwards is its own
  // business. Making the recorded barrier follow the tracked state removes the
  // whole class rather than one instance of it.
  const auto transition_target = [&](NativeTarget& t, D3D12_RESOURCE_STATES to) {
    if (t.color_state == to) {
      return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.color.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.color_state;
    b.Transition.StateAfter = to;
    dl->D3DResourceBarrier(1, &b);
    t.color_state = to;
  };

  // --- the ordered multi-target replay (the references' command queue) -------
  const std::uint32_t rtv_stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const std::uint32_t dsv_stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  NativeTarget* current_reg = nullptr;
  std::uint32_t current_pass = 0xFFFFFFFFu;
  // The handles of whatever is bound right now, so a captured Clear can hit the
  // same image ProcClear hits: the currently bound one.
  D3D12_CPU_DESCRIPTOR_HANDLE cur_rtvh{};
  D3D12_CPU_DESCRIPTOR_HANDLE cur_dsvh{};
  bool cur_has_rtv = false;
  bool cur_has_dsv = false;
  ID3D12Resource* last_target_res = nullptr;  // what the composite should show
  // THE BACK BUFFER IS THE PRESENTED TEXTURE - the reference arrangement.
  // UnleashedRecomp assigns g_backBuffer->texture = g_swapChain->getTexture(...)
  // (video.cpp:1597), so the game's own final composition draws straight into
  // what will be shown and Present does nothing but present (:2791). Rendering
  // the back-buffer pass into a private target and compositing it afterwards is
  // our own invention, and it is exactly where the image was being lost: the
  // composite's source held the game's two UberPostProcess quads, which SAMPLE
  // textures rather than carry pixels, so a missing link showed as white.
  bool guestout_is_bound = false;
  bool guestout_ever_bound = false;
  const auto transition_guest_output = [&](D3D12_RESOURCE_STATES from,
                                           D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out_res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    dl->D3DResourceBarrier(1, &b);
  };
  // Bind the presenter's image as the render target - the reference's
  // BeginCommandList acquires the swapchain texture FIRST and the back buffer
  // is simply that texture (video.cpp:1557/1597); it is not something that gets
  // bound only if a back-buffer draw happens to exist. A replay that never
  // reached this bind returned false, the presenter fell back to decoding guest
  // memory the Xenos never rasterised, and the screen was WHITE on every frame
  // whose snapshot lacked a back-buffer item - which measurement says is most
  // of them. Bound at replay start, the frame has a black floor and whatever
  // back-buffer draws exist land on it.
  const auto bind_guest_output_direct = [&]() -> bool {
    if (out_res == nullptr || g_guestout_failed.load(std::memory_order_relaxed)) {
      return false;
    }
    if (!g_guestout_rtv_heap) {
      D3D12_DESCRIPTOR_HEAP_DESC hd{};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      hd.NumDescriptors = kPassFrames;
      if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_guestout_rtv_heap)))) {
        REXLOG_ERROR("[native-scene] guest-output RTV heap creation failed");
        g_guestout_failed.store(true, std::memory_order_relaxed);
        return false;
      }
    }
    static std::uint32_t bb_ring = 0;
    const std::uint32_t ring = bb_ring++ % kPassFrames;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvh = g_guestout_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtvh.ptr += static_cast<SIZE_T>(ring) * rtv_stride;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(out_res, &rv, rtvh);
    if (!guestout_is_bound) {
      transition_guest_output(rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
      guestout_is_bound = true;
    }
    if (!guestout_ever_bound) {
      // Once per replay, not once per bind: the guest's own Clear is dropped in
      // this mode, so the image starts from ours.
      dl->D3DClearRenderTargetView(rtvh, clear_color, 0, nullptr);
      guestout_ever_bound = true;
    }
    // No depth: the game's final composition is full-screen quads, and the
    // console binds its back buffer with a null depth surface too
    // (XeD3DViewport.cpp:85, RHISetRenderTarget(GD3DBackBuffer, NULL)).
    dl->D3DOMSetRenderTargets(1, &rtvh, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                            0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    dl->RSSetViewport(vp);
    dl->RSSetScissorRect(sc);
    cur_rtvh = rtvh;
    cur_has_rtv = true;
    cur_has_dsv = false;
    return true;
  };
  const auto close_current = [&]() {
    if (current_reg != nullptr) {
      transition_target(*current_reg, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      current_reg = nullptr;
    }
  };
  // Which full-size target actually RECEIVED the frame. "The last one bound"
  // chose empty post/utility targets over the one with the content (a black
  // screen with the world sitting perfectly rendered in another target);
  // draw-count is the honest signal.
  struct TargetScore {
    ID3D12Resource* res = nullptr;
    std::uint32_t w = 0;
    std::uint32_t key = 0;
    std::uint32_t draws = 0;
  };
  std::vector<TargetScore> target_scores;
  ID3D12Resource* current_score_res = nullptr;
  const auto score_target = [&](ID3D12Resource* res, std::uint32_t w, std::uint32_t key) {
    current_score_res = res;
    for (auto& t : target_scores) {
      if (t.res == res) {
        return;
      }
    }
    target_scores.push_back(TargetScore{res, w, key, 0});
  };
  const auto score_draw = [&]() {
    for (auto& t : target_scores) {
      if (t.res == current_score_res) {
        ++t.draws;
        return;
      }
    }
  };
  const auto bind_pass = [&](std::uint32_t pass) -> bool {
    close_current();
    // THE SHARED "SCENE" TARGET IS GONE. What stood here bound g_color for
    // kScenePassSlot - one target that every draw of the MSAA block was funnelled
    // into. It is the third architecture neither reference has: Unleashed and
    // Marathon give every guest surface its own native target, skate3 draws the
    // engine's stream into one target it owns end to end, and NEITHER answers
    // "which pass is the scene", because neither ever asks. g_color survives only
    // as the scene-injection readback source.
    if (pass >= kMaxPasses) {
      return false;  // the overflow sentinel: draw nowhere rather than into slot 0
    }
    const PassSlot& ps = passes_snap[pass];
    // The back-buffer pass - the game's own final composition (post-process
    // quads sampling the scene resolve through the registry links, then the
    // UI). Identified by the colour OBJECT matching GD3DBackBuffer at bind
    // time (see the SetRenderTarget hook); a null colour+depth bind means the
    // same thing. It never passes through RHICreateTargetableSurface, so it
    // gets a reserved registry key - sized by the GAME's viewport, because the
    // vertices drawn here are in guest screen coordinates and the composite
    // upscales to the output anyway. A null COLOUR with a real depth surface
    // is a depth-only pass (shadows), which has nothing native to draw into.
    // One EDRAM tile range, one native target: every DefaultColor alias draws
    // into the primary's target (see EdramAliasOn for the measurement).
    std::uint32_t target_key = CanonicalSurface(ps.color_object);
    std::uint32_t w = ps.view_w, h = ps.view_h;
    // The viewport is a fallback, not a size: it moves within a frame. Only the
    // two branches below that read a real surface record set this.
    bool size_is_authoritative = false;
    // Only an actual bind of GD3DBackBuffer is the back buffer. A NULL colour
    // bind is not "the back buffer by implication" - it is no render target.
    const bool is_bb = ps.is_backbuffer;
    // The surface's own colour format when the create hook recorded one and the
    // mode is on; kColorFormat otherwise (and always for the back buffer, whose
    // console format is A8R8G8B8 but which receives our HDR-ranged draws until
    // the composite retires everywhere).
    DXGI_FORMAT target_format = kColorFormat;
    if (is_bb) {
      if (!BackbufferPassEnabled()) {
        return false;
      }
      // Bind the presenter's own image and draw the game's composition into it,
      // the way the references do. Only under own-device: with the emulated
      // pipeline still running, its frame and ours would fight over the same
      // texture.
      if (OwnDeviceMode() && bind_guest_output_direct()) {
        current_reg = nullptr;
        score_target(out_res, width, kBackbufferKey);
        return true;
      }
      target_key = kBackbufferKey;
      // The guest back buffer is a fixed 1280x720 - the PASS viewport is not
      // its size (the game clips UI work to small rects on the same surface,
      // and whichever pass binds first would otherwise freeze a 128x128
      // viewport into the target for the whole session).
      w = kDefaultWidth;
      h = kDefaultHeight;
      size_is_authoritative = true;
    } else if (target_key == 0) {
      return false;  // depth-only or unidentified pass: nothing to draw into yet
    } else {
      std::lock_guard<std::mutex> lk(g_reg_mutex);
      const auto meta = g_surface_meta.find(target_key);
      if (meta != g_surface_meta.end()) {
        w = meta->second.w;
        h = meta->second.h;
        size_is_authoritative = true;
        if (SurfaceFormatOn() && meta->second.dxgi != DXGI_FORMAT_UNKNOWN) {
          target_format = meta->second.dxgi;
        }
      }
    }
    NativeTarget* t =
        GetOrCreateRegTarget(device, target_key, w, h, size_is_authoritative, target_format);
    if (t == nullptr) {
      return false;
    }
    transition_target(*t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvh = g_reg_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtvh.ptr += static_cast<std::size_t>(t->rtv_index) * rtv_stride;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvh = g_reg_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dsvh.ptr += static_cast<std::size_t>(t->dsv_index) * dsv_stride;
    if (t->cleared_frame != replay_index) {
      dl->D3DClearRenderTargetView(rtvh, clear_color, 0, nullptr);
      dl->D3DClearDepthStencilView(dsvh, D3D12_CLEAR_FLAG_DEPTH, g_depth_clear, 0, 0, nullptr);
      t->cleared_frame = replay_index;
    }
    dl->D3DOMSetRenderTargets(1, &rtvh, FALSE, &dsvh);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(t->w), static_cast<float>(t->h),
                            0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(t->w), static_cast<LONG>(t->h)};
    dl->RSSetViewport(vp);
    dl->RSSetScissorRect(sc);
    cur_rtvh = rtvh;
    cur_dsvh = dsvh;
    cur_has_rtv = true;
    cur_has_dsv = true;
    current_reg = t;
    // This surface now holds content we drew, as of this guest frame. What reads
    // it later (the alias path in RtBackedSrvSlot) uses that to tell "our target"
    // from "our empty target".
    {
      std::lock_guard<std::mutex> lk(g_reg_mutex);
      g_surface_drawn_frame[target_key] = items_frame;
    }
    // Downsample/utility passes must not become "the frame": only a full-size
    // target may represent it for the composite.
    if (t->w >= 1024) {
      last_target_res = t->color.Get();
    }
    score_target(t->color.Get(), t->w, target_key);
    return true;
  };

  // Occlusion query around the whole replay: pixels that survived every test.
  if (!g_query_heap) {
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = kPassFrames;
    if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_query_heap)))) {
      D3D12_HEAP_PROPERTIES rp{};
      rp.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = sizeof(std::uint64_t) * kPassFrames;
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.Format = DXGI_FORMAT_UNKNOWN;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_query_readback)))) {
        g_query_heap.Reset();
      } else {
        g_query_readback->SetName(L"dpour.occlusion");
      }
    }
  }
  const bool query_on = g_query_heap && g_query_readback;
  if (query_on) {
    dl->D3DBeginQuery(g_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
  }

  ID3D12PipelineState* last_pso = nullptr;
  D3D12_PRIMITIVE_TOPOLOGY last_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  std::uint64_t last_vs_cb = 0;
  std::uint64_t last_ps_cb = 0;
  std::uint64_t last_sh_cb = 0;
  // The rest of DirtyStates (video.cpp:195): a binding is re-issued only when
  // its value actually differs. UE3 draws long runs of primitives off one
  // stream set, so most of these collapse to nothing.
  D3D12_VERTEX_BUFFER_VIEW last_vbv[kMaxStreams]{};
  std::uint32_t last_vbv_count = 0;
  D3D12_INDEX_BUFFER_VIEW last_ibv{};
  const auto vbv_same = [&](const D3D12_VERTEX_BUFFER_VIEW* v, std::uint32_t n) {
    if (n != last_vbv_count) {
      return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      if (v[i].BufferLocation != last_vbv[i].BufferLocation ||
          v[i].SizeInBytes != last_vbv[i].SizeInBytes ||
          v[i].StrideInBytes != last_vbv[i].StrideInBytes) {
        return false;
      }
    }
    return true;
  };
  const bool no_draw = NoDraw();

  // Staged handle -> GPU address for this replay. One upload per distinct blob
  // however many draws reference it.
  static thread_local std::unordered_map<std::uint32_t, std::uint64_t> staged_gpu;
  staged_gpu.clear();
  const auto ResolveStaged = [&](std::uint32_t handle, std::uint32_t bytes) -> std::uint64_t {
    if (handle == kNoStage || bytes == 0) {
      return 0;
    }
    const auto found = staged_gpu.find(handle);
    if (found != staged_gpu.end()) {
      return found->second;
    }
    const std::uint8_t* src = arena != nullptr ? arena->At(handle) : nullptr;
    if (src == nullptr) {
      return 0;
    }
    const dpour_pipeline::ConstantAlloc a = dpour_pipeline::AllocReplayConstants(bytes);
    if (a.cpu == nullptr) {
      return 0;
    }
    std::memcpy(a.cpu, src, bytes);
    staged_gpu.emplace(handle, a.gpu);
    return a.gpu;
  };

  // THE RESOLVES, RUN WHERE THE GAME RAN THEM.
  //
  // Each was stamped with the draw count at the moment RHICopyToResolveTarget
  // fired, so replaying them at the same point puts the copy between the draws
  // that produced the surface and the draws that sample the result - which is
  // the whole reason a resolve exists and the reason an end-of-frame copy would
  // be wrong.
  std::size_t resolve_next = 0;
  std::uint32_t resolves_run = 0;
  const auto run_resolves_upto = [&](std::uint32_t draw_index) {
    if (!ResolveCopyEnabled()) {
      return;
    }
    while (resolve_next < resolves.size() && resolves[resolve_next].draw_index <= draw_index) {
      const PendingResolve pr = resolves[resolve_next++];
      // The set, not the list: the same destination resolved twice before this
      // cutoff is copied once, from its last source - what a std::set of pending
      // surfaces gives the reference for free.
      //
      // WITHIN THE BATCH ONLY. Scanning the whole rest of the frame dropped a
      // copy that a draw between the two resolves was about to sample, and left
      // it reading the previous frame's copy instead.
      bool superseded = false;
      for (std::size_t later = resolve_next; later < resolves.size(); ++later) {
        if (resolves[later].draw_index > draw_index) {
          break;
        }
        if (resolves[later].d3d_tex == pr.d3d_tex) {
          superseded = true;
          break;
        }
      }
      if (superseded) {
        continue;
      }
      // Depth resolves are not copied by the reference (video.cpp:3572 and :3582,
      // "Depth stencil textures in this game are guaranteed to be transient"), and
      // ours reach here only to be skipped for want of a target anyway - so this
      // is a no-op that removes noise, not a behaviour change.
      //
      // DEFAULT OFF regardless. It went out in the same build as a white screen,
      // and "it should be a no-op" is exactly the kind of claim that has to be
      // measured rather than asserted. DPOUR_NR_SKIP_DEPTH_RESOLVE=1 to enable.
      if (SkipDepthResolves()) {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        const auto meta = g_surface_meta.find(pr.surface);
        if (meta != g_surface_meta.end() && meta->second.is_depth) {
          continue;
        }
      }
      const auto src_it = g_reg_targets.find(pr.surface);
      if (src_it == g_reg_targets.end() || src_it->second.srv_slot == dpour_tex::kInvalidSlot) {
        // NAME THEM. "12 of 17" says a third of the copies do not happen; it does
        // not say which surfaces or why, and that is the difference between a
        // number and a lead. One line per surface, ever.
        //
        // Counted, not just announced: a surface skipped once during boot (before
        // anything had been drawn into it yet) and a surface skipped on EVERY
        // frame are completely different findings, and "log the first time" cannot
        // tell them apart. The repeats at 200 and 2000 are what separates them.
        static std::unordered_map<std::uint32_t, std::uint32_t> misses;
        std::uint32_t& n = misses[pr.surface];
        ++n;
        if (misses.size() < 64 && (n == 1 || n == 200 || n == 2000)) {
          std::uint32_t sw = 0, sh = 0;
          char sname[24] = "?";
          {
            std::lock_guard<std::mutex> lk(g_reg_mutex);
            const auto meta = g_surface_meta.find(pr.surface);
            if (meta != g_surface_meta.end()) {
              sw = meta->second.w;
              sh = meta->second.h;
              if (meta->second.name[0] != '\0') {
                std::snprintf(sname, sizeof(sname), "%s", meta->second.name);
              }
            }
          }
          REXLOG_WARN("[native-scene] resolve WITHOUT SOURCE x{}: \"{}\" surface {:#x} ({}x{}) -> "
                      "tex {:#x} - no native target, so nothing we drew reaches that texture",
                      n, sname, pr.surface, sw, sh, pr.d3d_tex);
        }
        continue;  // nothing native has been drawn into that surface yet
      }
      NativeTarget& src = src_it->second;
      if (!EnsureResolvePipeline(device)) {
        return;  // sticky: it will not work later either
      }
      ResolveTexture* dst = EnsureResolveTexture(device, pr.d3d_tex, src.w, src.h);
      if (dst == nullptr) {
        continue;
      }
      // The source has to be readable. If it is the target currently bound, this
      // is exactly the case the alias could never handle - close it first. The
      // transition after it is a no-op unless something else left it writable.
      if (current_reg == &src) {
        close_current();
        current_pass = 0xFFFFFFFFu;
      }
      transition_target(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      D3D12_RESOURCE_BARRIER to_rt{};
      to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      to_rt.Transition.pResource = dst->tex.Get();
      to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      dl->D3DResourceBarrier(1, &to_rt);

      const std::uint32_t stride =
          device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      D3D12_CPU_DESCRIPTOR_HANDLE rtvh = g_resolve_rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtvh.ptr += static_cast<std::size_t>(dst->rtv_index) * stride;
      dl->D3DOMSetRenderTargets(1, &rtvh, FALSE, nullptr);
      dl->D3DSetGraphicsRootSignature(g_resolve_root.Get());
      D3D12_GPU_DESCRIPTOR_HANDLE srcgpu{};
      srcgpu.ptr = dpour_tex::GpuHandleAt(src.srv_slot);
      dl->D3DSetGraphicsRootDescriptorTable(0, srcgpu);
      // Nothing to undo when nothing multiplied: with PSR_ColorBiasFactor forced
      // to 1 the shaders write unbiased colour, so the copy is a straight copy.
      // (The game's own resolve passes D3DRESOLVE_EXPONENTBIAS(-bias) here,
      // XeD3DRenderTarget.cpp:519, which is what this mirrors when the bias is
      // left in place.)
      const float scale = KeepColorBias() ? std::ldexp(1.0f, -pr.bias) : 1.0f;
      const float consts[4] = {scale, 0.0f, 0.0f, 0.0f};
      dl->D3DSetGraphicsRoot32BitConstants(1, 4, consts, 0);
      dl->D3DSetPipelineState(g_resolve_pso.Get());
      const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dst->w), static_cast<float>(dst->h),
                              0.0f, 1.0f};
      const D3D12_RECT sc{0, 0, static_cast<LONG>(dst->w), static_cast<LONG>(dst->h)};
      dl->RSSetViewport(vp);
      dl->RSSetScissorRect(sc);
      dl->D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      dl->D3DIASetVertexBuffers(0, 0, nullptr);
      dl->D3DIASetIndexBuffer(nullptr);
      dl->D3DDrawInstanced(3, 1, 0, 0);

      D3D12_RESOURCE_BARRIER back = to_rt;
      back.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      back.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      dl->D3DResourceBarrier(1, &back);
      ++resolves_run;
      // This destination now holds this frame's content. Anything that samples it
      // later is entitled to our copy; anything sampling a destination we did NOT
      // refresh gets the guest texture instead.
      {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        g_resolve_copy_frame[pr.d3d_tex] = items_frame;
      }

      // Hand the list back to the game's own pipeline. Setting a root signature
      // drops every root argument with it, so the next draw has to re-bind the
      // target, the PSO and the topology as if nothing were current.
      dl->D3DSetGraphicsRootSignature(dpour_pipeline::RootSignature());
      dpour_pipeline::BindDescriptorTables(dl, dpour_tex::GpuHandleStart());
      last_pso = nullptr;
      last_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
      last_vs_cb = 0;
      last_ps_cb = 0;
      last_sh_cb = 0;
      last_vbv_count = 0;
      last_ibv = D3D12_INDEX_BUFFER_VIEW{};
      current_pass = 0xFFFFFFFFu;
    }
  };
  bool pass_ok = false;
  // The captured Clears, run at their recorded positions - ProcClear's job in
  // the reference. A clear for a pass other than the bound one binds that pass
  // first; the next item's own pass check rebinds whatever it needs.
  std::size_t clear_next = 0;
  const auto run_clears_upto = [&](std::uint32_t draw_index) {
    while (clear_next < clears.size() && clears[clear_next].draw_index <= draw_index) {
      const PendingClear pc = clears[clear_next++];
      if (pc.pass != current_pass) {
        current_pass = pc.pass;
        pass_ok = bind_pass(current_pass);
      }
      if (!pass_ok) {
        continue;
      }
      if (pc.clear_color && cur_has_rtv) {
        dl->D3DClearRenderTargetView(cur_rtvh, pc.rgba, 0, nullptr);
      }
      if (pc.clear_depth && cur_has_dsv) {
        // The game's own RHIClear applies the GInvertZ flip inside; the captured
        // value is engine-side, so the flip is mirrored here where our depth
        // convention is decided.
        const float d = g_invert_z.load(std::memory_order_relaxed) == 1 ? 1.0f - pc.depth
                                                                        : pc.depth;
        dl->D3DClearDepthStencilView(cur_dsvh, D3D12_CLEAR_FLAG_DEPTH, d, 0, 0, nullptr);
      }
    }
  };
  std::uint32_t issued = 0;
  std::uint32_t scene_issued = 0;
  std::uint32_t scene_issued_shaded = 0;
  // Only frames with a real scene are worth listing; a menu frame would spend
  // the budget on nothing.
  static std::uint32_t scene_list_frames = 0;
  const std::uint32_t max_draws = MaxDrawsPerFrame();
  // EVERY pass is replayed, each into its own native target. Replaying only a
  // chosen "scene" was the invention: the game's frame is built out of many
  // passes feeding each other (shadow maps, the base pass, the post-process
  // chain), and dropping all but one leaves the survivor reading targets that
  // were never rendered.
  //
  // Under own-device the presenter's image is bound and cleared FIRST, before
  // any item - the reference acquires the swapchain texture at frame start
  // (BeginCommandList, video.cpp:1557) and everything else happens on top of
  // it. The first item's own pass bind replaces it immediately; what this buys
  // is that a replay whose snapshot carries no back-buffer item still OWNS the
  // frame (black, plus whatever did draw) instead of returning false and
  // letting the presenter show guest memory the Xenos never rasterised - which
  // was the standing white screen.
  // THE FLAT SCENE: one target, every draw, submission order (skate3's model).
  // Bound once here and never switched, so the item loop below skips the pass
  // machinery entirely.
  const bool flat = flat_mode;
  if (flat) {
    D3D12_CPU_DESCRIPTOR_HANDLE frtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE fdsv = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    // DPOUR_NR_FLAT_PROBE=1: clear the scene target to a colour nothing in this
    // game produces. It answers, in one run, the question every measurement so
    // far has left open - does ANYTHING written into this target reach the
    // screen? A magenta screen means the target, the blit, the presenter and the
    // swapchain are all sound and the draws are the problem; a black screen
    // means the chain is broken downstream of the draws and every conclusion
    // drawn from draw counters has been about the wrong half of the pipeline.
    static const bool probe = EnvOn("DPOUR_NR_FLAT_PROBE");
    if (probe) {
      const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
      dl->D3DClearRenderTargetView(frtv, magenta, 0, nullptr);
    } else {
      dl->D3DClearRenderTargetView(frtv, clear_color, 0, nullptr);
    }
    dl->D3DClearDepthStencilView(fdsv, D3D12_CLEAR_FLAG_DEPTH, g_depth_clear, 0, 0, nullptr);
    dl->D3DOMSetRenderTargets(1, &frtv, FALSE, &fdsv);
    const D3D12_VIEWPORT fvp{0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th),
                             0.0f, 1.0f};
    const D3D12_RECT fsc{0, 0, static_cast<LONG>(tw), static_cast<LONG>(th)};
    dl->RSSetViewport(fvp);
    dl->RSSetScissorRect(fsc);
    cur_rtvh = frtv;
    cur_dsvh = fdsv;
    cur_has_rtv = true;
    cur_has_dsv = true;
    pass_ok = true;
  } else if (OwnDeviceMode()) {
    bind_guest_output_direct();
  }
  for (std::size_t item_index = 0; item_index < items.size(); ++item_index) {
    const SceneDraw& it = items[item_index];
    if (issued >= max_draws) {
      break;
    }
    // The game's own Clears, at their recorded positions (ProcClear's job).
    // In flat mode they would wipe the one target mid-frame, since every pass's
    // clear now lands on the same surface - so the single clear above is the
    // frame's only one.
    if (!flat) {
      run_clears_upto(static_cast<std::uint32_t>(item_index));
    }
    // FLAT COLOUR, PER-PASS DEPTH.
    //
    // One colour target is the point; one DEPTH buffer across every pass is a
    // mistake I made building it. The game renders shadow maps, reflections and
    // downsample passes from other viewpoints entirely, and flattening them
    // onto a single depth buffer lets whichever drew first occlude the world.
    // Measured: 52.7M pixels passed all tests over 572 frames, i.e. the draws
    // DO rasterise - but only ~7k-92k pixels per frame survive, 0.2% of a
    // 2560x1440 screen. That is not a missing image, that is an image being
    // depth-killed by geometry from a different camera.
    //
    // Clearing depth when the guest's pass changes restores each pass's own
    // depth semantics without reintroducing a pass TABLE, a vote, or a target
    // registry: we only watch the number change.
    if (flat && it.pass != current_pass) {
      current_pass = it.pass;
      dl->D3DClearDepthStencilView(cur_dsvh, D3D12_CLEAR_FLAG_DEPTH, g_depth_clear, 0, 0, nullptr);
    }
    if (!flat && it.pass != current_pass) {
      // THE COPY GOES HERE, WHERE THE REFERENCE PUTS IT.
      //
      // Unleashed does not batch its stretch-rect copies at the end of the
      // frame. FlushRenderStateForRenderThread (video.cpp:4462) and ProcClear
      // (:3680) both ask PopulateBarriersForStretchRect whether the surface
      // about to be written has destination textures pending, and if so run
      // ExecutePendingStretchRectCommands right then - before the draw. The
      // Present-time pass (:3565) is only the sweep for whatever is still
      // pending when the frame ends.
      //
      // Ours ran every copy after every draw. That is a full frame of latency on
      // each one: the composition quads sampled the PREVIOUS frame's copy, which
      // is why the loading screen alternated between correct and a grey or white
      // wash - the same frame, sampling a texture that was one frame behind or
      // had never been written at all.
      //
      // A pass boundary is the cheap version of the same rule: it is where a
      // different surface starts being written, so it is where anything that
      // sampled the old one must already be satisfied. A dozen calls a frame,
      // not one per draw - the per-draw version was measured at 14 FPS.
      run_resolves_upto(static_cast<std::uint32_t>(item_index));
      current_pass = it.pass;
      pass_ok = bind_pass(current_pass);
    }
    if (!pass_ok) {
      continue;
    }
    ++issued;
    // Draws that can paint versus draws that cannot. A frame made entirely of
    // depth-prepass draws writes no colour at all - which looks exactly like
    // broken shaders. Counted over the WHOLE frame now: there is no reserved
    // scene slot left to count separately, and the number was only ever a
    // sanity check on "did anything shaded get through".
    ++scene_issued;
    if (it.ps_hash != 0) {
      ++scene_issued_shaded;
    }
    // DPOUR_NR_DRAW_SCENELIST: the frame's draws in submission order, with the
    // pass each one belongs to. A full-screen quad is unmistakable here: six
    // indices among neighbours of hundreds.
    if (SceneListEnabled() && scene_list_frames < 2 && scene_issued <= 80) {
      REXLOG_INFO("[native-scene] draw[{:3}] pass={} {} count={} start={} vs={:#x} ps={:#x}",
                  scene_issued, it.pass, it.indexed ? "indexed" : "linear", it.count, it.start,
                  it.vs_hash, it.ps_hash);
    }
    score_draw();
    if (it.pso != last_pso) {
      dl->D3DSetPipelineState(it.pso);
      last_pso = it.pso;
    }
    if (it.topology != last_topology) {
      dl->D3DIASetPrimitiveTopology(it.topology);
      last_topology = it.topology;
    }
    // THE GPU ADDRESS IS BORN HERE, not at capture. Staged bytes are copied into
    // this frame's upload allocator and bound - FlushRenderStateForRenderThread
    // (video.cpp:4520) does exactly this, and for the same reason: the address
    // must belong to the frame that is being submitted, not to one that has
    // already been recycled.
    //
    // Only when it CHANGES, which is the reference's dirty-state rule. Draws
    // sharing a bank share one upload and one binding, so the common case costs
    // nothing at all - and it removes three root-descriptor writes per draw.
    const std::uint64_t vs_gpu = ResolveStaged(it.cb_vertex, it.cb_vertex_bytes);
    const std::uint64_t ps_gpu = ResolveStaged(it.cb_pixel, it.cb_pixel_bytes);
    const std::uint64_t sh_gpu =
        ResolveStaged(it.cb_shared, dpour_pipeline::kSharedConstantsSize);
    if (vs_gpu == 0 || ps_gpu == 0 || sh_gpu == 0) {
      continue;  // the frame allocator refused; nothing to bind this draw to
    }
    if (vs_gpu != last_vs_cb) {
      dl->D3DSetGraphicsRootConstantBufferView(dpour_pipeline::kRootVertexConstants, vs_gpu);
      last_vs_cb = vs_gpu;
    }
    if (ps_gpu != last_ps_cb) {
      dl->D3DSetGraphicsRootConstantBufferView(dpour_pipeline::kRootPixelConstants, ps_gpu);
      last_ps_cb = ps_gpu;
    }
    if (sh_gpu != last_sh_cb) {
      dl->D3DSetGraphicsRootConstantBufferView(dpour_pipeline::kRootSharedConstants, sh_gpu);
      last_sh_cb = sh_gpu;
    }
    // One line the first time a given shader pair is actually submitted. A GPU
    // hang kills the process without saying what it was executing, so the last
    // line in the log before the device is removed IS the identification.
    if (LogSubmissions()) {
      static thread_local std::unordered_map<std::uint64_t, bool> announced;
      const std::uint64_t combo = it.vs_hash * 1099511628211ull ^ it.ps_hash;
      if (announced.emplace(combo, true).second) {
        REXLOG_INFO(
            "[native-scene] submitting vs={:#x} ps={:#x} {} count={} start={} topo={} streams={}",
            it.vs_hash, it.ps_hash, it.indexed ? "indexed" : "linear", it.count, it.start,
            static_cast<int>(it.topology), it.vbv_count);
      }
    }
    // User-primitive geometry becomes a GPU buffer here, in the frame that draws
    // it - never earlier.
    D3D12_VERTEX_BUFFER_VIEW vbv[kMaxStreams];
    std::memcpy(vbv, it.vbv, sizeof(vbv));
    D3D12_INDEX_BUFFER_VIEW ibv = it.ibv;
    if (it.up_vtx != kNoStage) {
      const std::uint64_t gpu = ResolveStaged(it.up_vtx, it.up_vtx_bytes);
      if (gpu == 0) {
        continue;
      }
      vbv[0].BufferLocation = gpu;
    }
    if (it.up_idx != kNoStage) {
      const std::uint64_t gpu = ResolveStaged(it.up_idx, it.up_idx_bytes);
      if (gpu == 0) {
        continue;
      }
      ibv.BufferLocation = gpu;
    }
    if (!vbv_same(vbv, it.vbv_count)) {
      dl->D3DIASetVertexBuffers(0, it.vbv_count, vbv);
      std::memcpy(last_vbv, vbv, sizeof(last_vbv));
      last_vbv_count = it.vbv_count;
    }
    if (it.indexed) {
      if (ibv.BufferLocation != last_ibv.BufferLocation ||
          ibv.SizeInBytes != last_ibv.SizeInBytes || ibv.Format != last_ibv.Format) {
        dl->D3DIASetIndexBuffer(&ibv);
        last_ibv = ibv;
      }
      if (!no_draw) {
        dl->D3DDrawIndexedInstanced(it.count, 1, it.start, it.base_vertex, 0);
      }
    } else if (!no_draw) {
      dl->D3DDrawInstanced(it.count, 1, it.start, 0);
    }
  }

  // ONCE PER FRAME, AT THE END, ONE COPY PER DESTINATION.
  //
  // This is ProcExecutePendingStretchRectCommands (video.cpp:3565): the resolve
  // itself records nothing but a link (ProcStretchRect, :3265), the alias serves
  // every sample taken during the frame (ProcSetTexture -> SetSurface, :3807),
  // and the real copies run in one batch out of a SET of pending surfaces - so
  // each destination is written exactly once no matter how often the game
  // resolved into it.
  //
  // Running a full-screen copy at every resolve event instead, in the middle of
  // the replay, with a forced pass rebind after each, cost fourteen of them per
  // frame and took gameplay to 14 FPS.
  run_clears_upto(0xFFFFFFFFu);
  run_resolves_upto(0xFFFFFFFFu);
  if (ResolveCopyEnabled()) {
    static std::atomic<std::uint64_t> n{0};
    const std::uint64_t i = n.fetch_add(1, std::memory_order_relaxed);
    if (i < 3 || (i % 600) == 0) {
      REXLOG_INFO("[native-scene] resolve copies this frame: {} of {} queued", resolves_run,
                  resolves.size());
    }
  }
  g_scene_issued.store(scene_issued, std::memory_order_relaxed);
  g_scene_issued_shaded.store(scene_issued_shaded, std::memory_order_relaxed);
  if (SceneListEnabled() && scene_issued > 40) {
    ++scene_list_frames;
  }
  if (query_on) {
    dl->D3DEndQuery(g_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
    dl->D3DResolveQueryData(g_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot, 1,
                         g_query_readback.Get(), sizeof(std::uint64_t) * slot);
  }

  // Close whatever target is still open, then hand depth back for sampling.
  close_current();
  D3D12_RESOURCE_BARRIER depth_to_srv = depth_to_write;
  depth_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  depth_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  dl->D3DResourceBarrier(1, &depth_to_srv);

  // THE FRAME IS THE BACK BUFFER. Not a vote, not a score, not the target with
  // the most draws - the one surface the game resolves to the front buffer.
  //
  // This is the reference's answer, and it is structural rather than clever: in
  // UnleashedRecomp the guest's back buffer literally IS the swapchain image
  // (video.cpp:1597 g_backBuffer->texture = g_swapChain->getTexture(...)), so
  // whatever the game composes there is the frame by construction. Downpour
  // does the same thing through XePerformSwap, which resolves GD3DBackBuffer to
  // the front buffer (XeD3DDevice.cpp:426).
  //
  // Everything that used to stand here - the engine-driven scene target, the
  // back-buffer preference, the draw-count vote among full-size targets - was
  // three ways of guessing at a question the game already answers.
  // THE BACK BUFFER, AND ONLY IF THE GAME DREW INTO IT THIS FRAME.
  //
  // Serving it unconditionally - on the grounds that UnleashedRecomp's back
  // buffer IS the swapchain image and therefore never needs qualifying - put a
  // WHITE SCREEN over the logos, which had been rendering correctly through the
  // emulated path. The reasoning was wrong, not just the result: Unleashed has
  // no such condition because it has no CHOICE to make, the guest's back buffer
  // is the output by construction. We do have the choice every frame, and the
  // reference that shares it is skate3, which yields explicitly and often
  // (YieldForMenus, YieldForPhotoDisplay, YieldForCasEditor, the warmup gate).
  // "The game drew into its back buffer this frame" is that yield for us.
  {
    const TargetScore* best = nullptr;
    for (const auto& t : target_scores) {
      if (t.key == kBackbufferKey && t.draws > 0) {
        best = &t;
        break;
      }
    }
    if (best != nullptr) {
      last_target_res = best->res;
      g_comp_source_key.store(best->key, std::memory_order_relaxed);
      g_comp_source_draws.store(best->draws, std::memory_order_relaxed);
    } else {
      g_comp_source_key.store(0, std::memory_order_relaxed);
      g_comp_source_draws.store(0, std::memory_order_relaxed);
    }
  }

  // Guest-output replacement path: composite straight into the presenter's own
  // image. Nothing is painted onto the presenter's list in this mode - a frame
  // we decline shows the clean emulated output.
  bool guestout_drawn = false;
  // NO DRAW-COUNT THRESHOLD. The condition is structural: did the game compose
  // into its back buffer this frame.
  //
  // What stood here was "at least 16 draws", a number of our own choosing, and
  // on the loading screen the frames hover at 18-21 - so the ones that dipped
  // under it were handed to the emulator and the picture jumped between the two
  // renderers. skate3 declines for structural reasons it can name (menus, the
  // photo viewer, the CAS editor, the warmup gate); it never counts draws and
  // compares against a constant. Neither should we: "the game drew into its back
  // buffer" is a fact about the frame, "sixteen" is a guess about it.
  // FLAT MODE: the one target IS the frame. Hand it to the blit, which is the
  // whole of our "post chain" - skate3 runs a real one here (resolve, SSAO,
  // bloom, tonemap) and writes the result to guest_output the same way.
  if (flat && g_color) {
    D3D12_RESOURCE_BARRIER to_srv{};
    to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_srv.Transition.pResource = g_color.Get();
    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    dl->D3DResourceBarrier(1, &to_srv);
    guestout_drawn = CompositeToGuestOutput(device, dl, out_res, width, height, g_color.Get());
    D3D12_RESOURCE_BARRIER back = to_srv;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    back.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dl->D3DResourceBarrier(1, &back);
    // PROBE STAGE 2: write magenta into the PRESENTER'S OWN IMAGE with nothing
    // but a clear - no composite pipeline, no shader, no descriptor heap, no
    // sampling. This is the most primitive write the callback can make. If the
    // screen is still black after it, then nothing this callback records ever
    // reaches the display and every draw-side measurement so far has been
    // answering a question about the wrong half of the pipeline.
    static const bool probe_out = EnvOn("DPOUR_NR_FLAT_PROBE");
    if (probe_out && bind_guest_output_direct()) {
      const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
      dl->D3DClearRenderTargetView(cur_rtvh, magenta, 0, nullptr);
      transition_guest_output(D3D12_RESOURCE_STATE_RENDER_TARGET,
                              rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
      guestout_is_bound = false;
      guestout_drawn = true;
    }
    static std::atomic<std::uint64_t> n{0};
    const std::uint64_t i = n.fetch_add(1, std::memory_order_relaxed);
    if (i == 0 || (i % 600) == 0) {
      REXLOG_INFO("[native-scene] FLAT: {} draws into one {}x{} target -> guest output ({})",
                  issued, tw, th, guestout_drawn ? "shown" : "BLIT FAILED");
    }
  } else if (guestout_is_bound) {
    // The game's composition went straight into the presenter's image, so there
    // is nothing left to composite - only the state to hand back. This is the
    // branch the references have (and the one below is the branch they do not).
    transition_guest_output(D3D12_RESOURCE_STATE_RENDER_TARGET,
                            rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
    guestout_is_bound = false;
    guestout_drawn = true;
    // A counter, not a one-shot line: the one-shot version could not answer
    // "how many frames took this path", which was exactly the question the last
    // white-screen session needed answered.
    g_guestout_direct_frames.fetch_add(1, std::memory_order_relaxed);
  } else if (GuestOutMode() && last_target_res != nullptr) {
    guestout_drawn = CompositeToGuestOutput(device, dl, out_res, width, height, last_target_res);
  }

  // TWO CONSUMERS, TWO UNITS. Collapsing them into one broke both, one at a
  // time, and each failure was visible on screen:
  //
  // 1. THE CONSTANT RING AND THE OCCLUSION READBACK ask "has the GPU finished
  //    the submission that wrote this slot", and the honest answer is the
  //    command processor's own pair (native_rhi.h:545). Answering it with a
  //    private counter minus kPassFrames is not just imprecise, it is
  //    STRUCTURALLY DEAD: there are kPassFrames slots and they always hold the
  //    kPassFrames most recent values, so `fence <= counter - kPassFrames` is
  //    never true for any of them. g_gpu_completed stayed 0 forever,
  //    pipeline::BeginFrame never rotated a slice (it only rotates once
  //    `completed` passes the frame that wrote it), the current slice filled to
  //    its full 24 MB and every draw after that got no constants at all. Then it
  //    latches: no constants -> no draws -> nothing published -> this tail never
  //    runs -> completed never moves. One frame of it and the native path is
  //    gone for the session, which is exactly what "все через емулятор пішло"
  //    looked like.
  //
  // 2. UPLOAD RETIREMENT counts in REPLAYS. RetireUploads holds a buffer retired
  //    during capture for `submitted + 8`, and that 8 was measured in replays -
  //    roughly guest frames, the unit the staging -> published -> replay window
  //    is counted in. Fed the command processor's counter, which ticks many
  //    times per guest frame, the same 8 became a fraction of a frame: vertex
  //    data freed while it was still being read, which is why the loading
  //    screen's text came back with its first glyphs as garbage.
  g_alloc_fence[slot] = submission;
  g_alloc_frame[slot] = items_frame;
  ++g_pass_index;
  // Highest guest frame whose submission the GPU has finished. Anything older
  // than this is safe for the constant ring to reuse.
  UpdateGpuCompleted(completed);
  // RETIREMENT COUNTS IN GUEST FRAMES. Not replays, not submissions.
  //
  // The slack inside RetireUploads is described in its own words as covering
  // "the staging -> published -> replay window", and "during a loading stall the
  // published snapshot referencing it is replayed for many frames without a new
  // publish". Both of those are measured in GUEST FRAMES, so that is the unit to
  // hand it - and it is the only one of the three that does not move when we
  // change how often we replay or how often the command processor submits.
  //
  // Every other unit tried here failed on screen. Command-processor submissions
  // tick many times per guest frame, so +8 became a fraction of a frame and the
  // loading screen's glyphs came back as garbage. A private replay counter was
  // stable only while the replay ran once per published frame; the moment the
  // replay correctly began running on every callback, it took the same fall.
  //
  // g_gpu_completed is already exactly "the newest guest frame whose submission
  // the GPU has finished", derived from the command processor's real counters.
  dpour_vbuf::RetireUploads(g_gpu_completed.load(std::memory_order_relaxed), items_frame);
  dpour_tex::RetireUploads(g_gpu_completed.load(std::memory_order_relaxed), items_frame);

  // Read back an occlusion result the GPU has certainly finished with. Any slot
  // whose fence completed is safe; the newest such slot is the freshest answer.
  if (query_on) {
    for (std::uint32_t i = 0; i < kPassFrames; ++i) {
      if (g_alloc_fence[i] == 0 || g_alloc_fence[i] > completed) {
        continue;
      }
      const D3D12_RANGE rr{sizeof(std::uint64_t) * i, sizeof(std::uint64_t) * (i + 1)};
      void* mapped = nullptr;
      if (SUCCEEDED(g_query_readback->Map(0, &rr, &mapped)) && mapped != nullptr) {
        std::uint64_t pixels = 0;
        std::memcpy(&pixels, static_cast<const std::uint8_t*>(mapped) + rr.Begin, 8);
        const D3D12_RANGE none{0, 0};
        g_query_readback->Unmap(0, &none);
        g_scene_pixels.store(pixels, std::memory_order_relaxed);
        g_scene_pixel_frames.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
  }

  if (GuestOutMode()) {
    // Replacement mode: nothing is painted onto the presenter's list. The
    // composite already went into the presenter's own image, so "did we draw"
    // is the whole answer the callback needs.
    return guestout_drawn;
  }

  // Below is the OVERLAY path: it paints onto the presenter's own list, which
  // only exists when the presenter called us. Recorded from the guest-output
  // callback there is no such list (and none is wanted - the frame was already
  // composited above), so this is where that call returns.
  if (cmd == nullptr) {
    return guestout_drawn;
  }

  // The composite shows the target that received the most draws this frame.
  // Per-frame descriptor pair, same race note as the guest-output path.
  D3D12_GPU_DESCRIPTOR_HANDLE overlay_gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
  if (last_target_res != nullptr) {
    const UINT srv_inc =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT comp_slot = static_cast<UINT>(g_pass_index % kPassFrames) * 2u;
    D3D12_CPU_DESCRIPTOR_HANDLE comp_cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    comp_cpu.ptr += static_cast<SIZE_T>(comp_slot) * srv_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kColorFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(last_target_res, &sv, comp_cpu);
    comp_cpu.ptr += srv_inc;
    sv.Format = kDepthSrvFormat;
    device->CreateShaderResourceView(g_depth.Get(), &sv, comp_cpu);
    overlay_gpu.ptr += static_cast<UINT64>(comp_slot) * srv_inc;
  }
  // Composite onto the frame, on the presenter's list with the backbuffer bound.
  ID3D12DescriptorHeap* comp_heaps[] = {g_srv_heap.Get()};
  cmd->SetDescriptorHeaps(1, comp_heaps);
  cmd->SetGraphicsRootSignature(g_comp_root.Get());
  cmd->SetGraphicsRootDescriptorTable(0, overlay_gpu);
  struct {
    float depth_clear;
    std::uint32_t opaque;
    std::uint32_t depth_view;
    float amplify;
  } comp{g_depth_clear, NativeOnly() ? 1u : 0u,
         dpour_pipeline::FlatShadingEnabled() ? 1u : 0u, AmplifyFactor()};
  cmd->SetGraphicsRoot32BitConstants(1, 4, &comp, 0);
  cmd->SetPipelineState(g_comp_pso.Get());
  const D3D12_VIEWPORT bvp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                           0.0f, 1.0f};
  const D3D12_RECT bsc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  cmd->RSSetViewports(1, &bvp);
  cmd->RSSetScissorRects(1, &bsc);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 0, nullptr);
  cmd->IASetIndexBuffer(nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
  return guestout_drawn;
}

void LogStats() {
  std::uint32_t passes = 0;
  std::uint32_t best_draws = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    passes = g_pass_count;
    if (g_published_pass < kMaxPasses) {
      best_draws = g_passes[g_published_pass].draws;
    }
  }
  const std::uint64_t frames = g_capture_frames.exchange(0, std::memory_order_relaxed);
  const std::uint64_t ticks = g_capture_ticks.exchange(0, std::memory_order_relaxed);
  {
    // The pass distribution, so "we reproduce most draws but publish almost
    // none" can be seen rather than deduced.
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[384];
    int n = std::snprintf(line, sizeof(line), "passes:");
    for (std::uint32_t i = 0; i < g_pass_count && n > 0 && n < 300; ++i) {
      n += std::snprintf(line + n, sizeof(line) - n, " %s%s%#x=%u@%ux%u",
                         i == g_published_pass ? "*" : "",
                         g_passes[i].is_backbuffer ? "bb:" : "", g_passes[i].color,
                         g_passes[i].draws, g_passes[i].view_w, g_passes[i].view_h);
    }
    REXLOG_INFO("[native-scene] {}", line);
  }
  const std::uint64_t rframes = g_render_frames.exchange(0, std::memory_order_relaxed);
  const std::uint64_t rticks = g_render_ticks.exchange(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    REXLOG_INFO("[native-scene] registry: {} surfaces known, {} texture links, {} SRV-backed | "
                "retire calls {}, matched {}",
                g_surface_meta.size(), g_texture_link.size(), g_surface_srv.size(),
                g_retire_calls.load(std::memory_order_relaxed),
                g_retire_matched.load(std::memory_order_relaxed));
  }
  REXLOG_INFO("[native-scene] UP draws: {} begun, {} captured, {} dropped, {} over the frame cap",
              g_up_seen.load(std::memory_order_relaxed),
              g_up_captured.load(std::memory_order_relaxed),
              g_up_dropped.load(std::memory_order_relaxed),
              g_up_capped.load(std::memory_order_relaxed));
  REXLOG_INFO("[native-scene] composite source: {:#x} with {} draws | direct-to-output frames: {}",
              g_comp_source_key.load(std::memory_order_relaxed),
              g_comp_source_draws.load(std::memory_order_relaxed),
              g_guestout_direct_frames.load(std::memory_order_relaxed));
  // The numbers that say whether any of this reaches the screen, and whether
  // what reaches it can paint at all.
  REXLOG_INFO(
      "[native-scene] occlusion: {} pixels passed ({} frames measured) | scene target got {} "
      "draws, {} of them with a pixel shader",
      g_scene_pixels.load(std::memory_order_relaxed),
      g_scene_pixel_frames.load(std::memory_order_relaxed),
      g_scene_issued.load(std::memory_order_relaxed),
      g_scene_issued_shaded.load(std::memory_order_relaxed));
  if (dpour_ue3::Enabled()) {
    // D3D12_COMPARISON_FUNC: 1 NEVER 2 LESS 3 EQUAL 4 LESS_EQUAL 5 GREATER
    // 6 NOT_EQUAL 7 GREATER_EQUAL 8 ALWAYS. A base pass that tests EQUAL and
    // writes no depth is the signature that makes the prepass mandatory.
    REXLOG_INFO(
        "[native-scene] scene depth: func never={} less={} equal={} lessEq={} greater={} "
        "notEq={} greaterEq={} always={} | {} draws wrote depth | prepass draws admitted {} | "
        "scene depth surface {:#x}",
        g_scene_depth_func[1].load(std::memory_order_relaxed),
        g_scene_depth_func[2].load(std::memory_order_relaxed),
        g_scene_depth_func[3].load(std::memory_order_relaxed),
        g_scene_depth_func[4].load(std::memory_order_relaxed),
        g_scene_depth_func[5].load(std::memory_order_relaxed),
        g_scene_depth_func[6].load(std::memory_order_relaxed),
        g_scene_depth_func[7].load(std::memory_order_relaxed),
        g_scene_depth_func[8].load(std::memory_order_relaxed),
        g_scene_depth_writes.load(std::memory_order_relaxed), g_scene_prepass_draws,
        g_scene_depth_object);
    REXLOG_INFO("[native-scene] rt-alias samples refused (texture == bound target): {}",
                g_rt_alias_refused.load(std::memory_order_relaxed));
  }
  REXLOG_INFO(
      "[native-scene] capture {:.2f} ms/frame ({} frames), render {:.2f} ms/frame ({} frames) | "
      "buffer size known {} / unknown {}",
      frames != 0 ? TicksToMs(ticks) / static_cast<double>(frames) : 0.0, frames,
      rframes != 0 ? TicksToMs(rticks) / static_cast<double>(rframes) : 0.0, rframes,
      g_size_known.load(std::memory_order_relaxed),
      g_size_unknown.load(std::memory_order_relaxed));
  REXLOG_INFO(
      "[native-scene] {} draws seen, {} reproduced ({} in the published pass of {}) | dropped: "
      "{} other passes, {} no shader, {} no layout, {} no pipeline, {} topology, {} vertex range, "
      "{} streams, {} vb object, {} vb data, {} ib object, {} ib data, {} constants, {} no "
      "device, {} over the frame limit",
      g_seen.load(std::memory_order_relaxed), g_drawn.load(std::memory_order_relaxed), best_draws,
      passes, g_other_pass.load(std::memory_order_relaxed),
      g_no_shader.load(std::memory_order_relaxed),
      g_no_layout.load(std::memory_order_relaxed), g_no_pso.load(std::memory_order_relaxed),
      g_no_topology.load(std::memory_order_relaxed), g_no_range.load(std::memory_order_relaxed),
      g_no_stream.load(std::memory_order_relaxed), g_no_vbobj.load(std::memory_order_relaxed),
      g_no_vbdata.load(std::memory_order_relaxed), g_no_ibobj.load(std::memory_order_relaxed),
      g_no_ibdata.load(std::memory_order_relaxed),
      g_no_constants.load(std::memory_order_relaxed), g_no_device.load(std::memory_order_relaxed),
      g_overflow.load(std::memory_order_relaxed));
  if (g_no_vbobj.load(std::memory_order_relaxed) != 0) {
    REXLOG_INFO("[native-scene] vb-object failures by reason: {} d3d unreadable, {} no key + "
                "learned miss, {} fetch/BaseAddress mismatch",
                g_vbfail_reason[kVbFailD3dBad].load(std::memory_order_relaxed),
                g_vbfail_reason[kVbFailNoKey].load(std::memory_order_relaxed),
                g_vbfail_reason[kVbFailMismatch].load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> lock(g_vbfail_mutex);
    for (const auto& e : g_vbfail) {
      if (e.count == 0) {
        continue;
      }
      // Decode the +24 fetch the way ResolveBuffer would, so the line itself
      // says which gate refused it: addr==0 means the header was never given an
      // address, size==0 means the dword pair is not a fetch constant at all,
      // and a plausible addr different from known_addr is a genuinely stale
      // shadow pair.
      const std::uint32_t f_addr = e.raw0 & 0xFFFFFFFCu;
      const std::uint64_t f_size = static_cast<std::uint64_t>((e.raw1 >> 2) & 0xFFFFFFu) * 4ull;
      REXLOG_INFO(
          "[native-scene]   vb-fail x{}: reason={} stream={} stride={} rhi={:#x} d3d={:#x} "
          "BaseAddress={:#x} fetch@24 raw={:#x},{:#x} -> addr={:#x} size={}",
          e.count, e.reason, e.stream, e.stride, e.rhi, e.d3d, e.known_addr, e.raw0, e.raw1,
          f_addr, f_size);
    }
  }
  dpour_state::LogStats();
  dpour_vbuf::LogStats();
  dpour_pipeline::LogStats();
}

}  // namespace dpour_scene
// === END DPOUR MIGRATION 2026-07-25 ===
