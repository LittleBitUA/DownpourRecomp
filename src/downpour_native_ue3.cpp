// downpour - Native Render: the ENGINE-level scene stream (UE3)
//
// === DPOUR MIGRATION 2026-07-26: data-driven layer, the skate3recomp approach ===
//
// THE EVIDENCE FOR THE LAYOUT BELOW.
//
// sub_828194E8 is the only function in the binary that calls
// RHIDrawIndexedPrimitive (sub_829CA0E8, downpour_recomp.43.cpp:5852), and its
// body matches FMeshDrawingPolicy::DrawMesh from the game's own source
// (Engine/Src/... , FMeshElement declared at Scene.h:599) field for field:
//
//   lwz  r11,208(r4)          the BITFIELD word
//   rlwinm r10,r11,0,0,0      -> bit 0  = UseDynamicData      (branch)
//   rlwinm r3,r11,8,29,31     -> bits 5-7 = Type              (passed as PrimitiveType)
//   lwz  r7,16(r4)            DynamicIndexData - null picks the non-indexed UP call
//   lwz  r4,184(r4)           MinVertexIndex
//   lwz  r11,188(r31)         MaxVertexIndex
//   subf r11,r4,r11 ; addi r5,r11,1     -> NumVertices = Max - Min + 1
//   lwz  r6,180(r31)          NumPrimitives
//   lwz  r9,8(r31)            DynamicVertexData
//   lhz  r10,12(r31)          DynamicVertexStride
//   lhz  r8,14(r31)           DynamicIndexStride
//   bl   0x829cae50           RHIDrawIndexedPrimitiveUP
//
// The two FMatrix members are 16-byte aligned (MS_ALIGN(16) on FMatrix), which
// is what puts LocalToWorld at +48 rather than +36 and every scalar after
// WorldToLocal at the offsets the disassembly reads.
//
// Why this layer exists at all: the D3D-level capture cannot tell the world
// from the HUD, a shadow pass from the base pass, or a mesh's transform from
// its vertex data. The engine can, and it says so once per mesh, right here.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_ue3.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>

namespace dpour_ue3 {
namespace {

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

inline std::uint32_t ReadBE32(const std::uint8_t* base, std::uint32_t addr) {
  std::uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return _byteswap_ulong(raw);
}

inline std::uint16_t ReadBE16(const std::uint8_t* base, std::uint32_t addr) {
  std::uint16_t raw;
  std::memcpy(&raw, base + addr, 2);
  return _byteswap_ushort(raw);
}

inline float ReadBEFloat(const std::uint8_t* base, std::uint32_t addr) {
  const std::uint32_t bits = ReadBE32(base, addr);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// FMeshElement, Scene.h:599. Offsets verified against the recompiled
// FMeshDrawingPolicy::DrawMesh - see the file header.
constexpr std::uint32_t kOffIndexBuffer = 0;
constexpr std::uint32_t kOffVertexFactory = 4;
constexpr std::uint32_t kOffDynVertexData = 8;
constexpr std::uint32_t kOffDynVertexStride = 12;
constexpr std::uint32_t kOffDynIndexStride = 14;
constexpr std::uint32_t kOffDynIndexData = 16;
constexpr std::uint32_t kOffMaterialProxy = 20;
constexpr std::uint32_t kOffLCI = 24;
constexpr std::uint32_t kOffDecalState = 28;
constexpr std::uint32_t kOffLocalToWorld = 48;
constexpr std::uint32_t kOffFirstIndex = 176;
constexpr std::uint32_t kOffNumPrimitives = 180;
constexpr std::uint32_t kOffMinVertexIndex = 184;
constexpr std::uint32_t kOffMaxVertexIndex = 188;
constexpr std::uint32_t kOffLODIndex = 192;
constexpr std::uint32_t kOffFlags = 208;
constexpr std::uint32_t kMeshElementSize = 216;

std::mutex g_mutex;
std::vector<MeshItem> g_frame;   // this frame's stream, in submission order
std::vector<MeshItem> g_last;    // the completed frame
// The biggest mesh of the last frame that HAD one. Menus and loading screens
// submit nothing, so a sample taken from "the last frame" is usually empty
// exactly when the stats line is printed - and the transform is the one field
// that must be eyeballed before anything is built on top of it.
MeshItem g_sample{};
bool g_have_sample = false;
std::uint32_t g_sample_frame_meshes = 0;

// The mesh being submitted right now (see CurrentMesh in the header). Guest
// render thread only, hence plain variables rather than atomics.
MeshItem g_current{};
bool g_have_current = false;

// Counters. The first question this layer has to answer is simply "does the
// engine hand us the world at all" - the D3D view saw about thirty draws a
// frame in gameplay and could not say whether the rest was missing or never
// issued.
std::atomic<std::uint64_t> g_meshes{0};
std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_rejected{0};
std::atomic<std::uint32_t> g_max_per_frame{0};
std::atomic<std::uint64_t> g_dynamic{0};
std::atomic<std::uint64_t> g_dpg[8] = {};

// A mesh element is a stack or heap temporary the engine fills in per draw, so
// the pointer is only sane while we are inside DrawMesh. A page probe on the
// first use of an address range is cheap insurance against a stale pointer
// taking the process down.
bool SpanReadable(const std::uint8_t* p, std::size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const auto* region_end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return static_cast<std::size_t>(region_end - p) >= bytes;
}

}  // namespace

bool Enabled() {
  static const bool on = EnvOn("DPOUR_NR_UE3");
  return on;
}

void OnDrawMesh(const std::uint8_t* base, std::uint32_t mesh_guest) {
  if (!Enabled() || base == nullptr || !GuestAddrPlausible(mesh_guest)) {
    return;
  }
  if (!SpanReadable(base + mesh_guest, kMeshElementSize)) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  MeshItem m;
  m.index_buffer = ReadBE32(base, mesh_guest + kOffIndexBuffer);
  m.vertex_factory = ReadBE32(base, mesh_guest + kOffVertexFactory);
  m.dyn_vertex_data = ReadBE32(base, mesh_guest + kOffDynVertexData);
  m.dyn_vertex_stride = ReadBE16(base, mesh_guest + kOffDynVertexStride);
  m.dyn_index_stride = ReadBE16(base, mesh_guest + kOffDynIndexStride);
  m.dyn_index_data = ReadBE32(base, mesh_guest + kOffDynIndexData);
  m.material_proxy = ReadBE32(base, mesh_guest + kOffMaterialProxy);
  m.lci = ReadBE32(base, mesh_guest + kOffLCI);
  m.decal_state = ReadBE32(base, mesh_guest + kOffDecalState);
  for (int i = 0; i < 16; ++i) {
    m.local_to_world[i] = ReadBEFloat(base, mesh_guest + kOffLocalToWorld + i * 4u);
  }
  m.first_index = ReadBE32(base, mesh_guest + kOffFirstIndex);
  m.num_primitives = ReadBE32(base, mesh_guest + kOffNumPrimitives);
  m.min_vertex = ReadBE32(base, mesh_guest + kOffMinVertexIndex);
  m.max_vertex = ReadBE32(base, mesh_guest + kOffMaxVertexIndex);
  m.lod_index = static_cast<std::int8_t>(base[mesh_guest + kOffLODIndex]);
  m.flags = ReadBE32(base, mesh_guest + kOffFlags);

  g_meshes.fetch_add(1, std::memory_order_relaxed);
  if (m.use_dynamic_data()) {
    g_dynamic.fetch_add(1, std::memory_order_relaxed);
  }
  g_dpg[m.depth_priority_group() & 7u].fetch_add(1, std::memory_order_relaxed);

  // Published for the draw that is about to happen (see CurrentMesh). Written
  // and read on the guest render thread only.
  g_current = m;
  g_have_current = true;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_frame.size() < 8192) {  // a frame that large is a runaway, not a scene
    g_frame.push_back(m);
  }
}

const MeshItem* CurrentMesh() { return g_have_current ? &g_current : nullptr; }

void BeginFrame() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_frame.clear();
}

void EndFrame() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::uint32_t n = static_cast<std::uint32_t>(g_frame.size());
  std::uint32_t prev = g_max_per_frame.load(std::memory_order_relaxed);
  while (n > prev && !g_max_per_frame.compare_exchange_weak(prev, n)) {
  }
  if (!g_frame.empty()) {
    const MeshItem* best = nullptr;
    for (const MeshItem& m : g_frame) {
      if (m.material_proxy == 0 || m.index_buffer == 0) {
        continue;  // want a real buffered world mesh, not a dynamic UI quad
      }
      if (best == nullptr || m.num_primitives > best->num_primitives) {
        best = &m;
      }
    }
    if (best != nullptr) {
      g_sample = *best;
      g_have_sample = true;
      g_sample_frame_meshes = n;
    }
  }
  g_last.swap(g_frame);
  g_frames.fetch_add(1, std::memory_order_relaxed);
}

void LogStats() {
  if (!Enabled()) {
    return;
  }
  std::size_t last_n = 0;
  std::size_t with_material = 0;
  std::size_t with_lightmap = 0;
  std::size_t indexed = 0;
  std::uint64_t triangles = 0;
  std::unordered_set<std::uint32_t> materials;
  std::unordered_set<std::uint32_t> factories;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    last_n = g_last.size();
    for (const MeshItem& m : g_last) {
      if (m.material_proxy != 0) {
        ++with_material;
        materials.insert(m.material_proxy);
      }
      if (m.lci != 0) {
        ++with_lightmap;
      }
      if (m.index_buffer != 0) {
        ++indexed;
      }
      if (m.vertex_factory != 0) {
        factories.insert(m.vertex_factory);
      }
      triangles += m.num_primitives;
    }
  }
  const std::uint64_t frames = g_frames.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[native-ue3] engine stream: {} meshes over {} frames (peak {}/frame, last frame {}: {} "
      "with material, {} lightmapped, {} indexed, {} triangles, {} distinct materials, {} vertex "
      "factories), {} dynamic, {} unreadable",
      g_meshes.load(std::memory_order_relaxed), frames,
      g_max_per_frame.load(std::memory_order_relaxed), last_n, with_material, with_lightmap,
      indexed, triangles, materials.size(), factories.size(),
      g_dynamic.load(std::memory_order_relaxed), g_rejected.load(std::memory_order_relaxed));
  REXLOG_INFO(
      "[native-ue3] depth priority groups: bg={} world={} foreground={} other={}/{}/{}/{}/{}",
      g_dpg[0].load(std::memory_order_relaxed), g_dpg[1].load(std::memory_order_relaxed),
      g_dpg[2].load(std::memory_order_relaxed), g_dpg[3].load(std::memory_order_relaxed),
      g_dpg[4].load(std::memory_order_relaxed), g_dpg[5].load(std::memory_order_relaxed),
      g_dpg[6].load(std::memory_order_relaxed), g_dpg[7].load(std::memory_order_relaxed));
  // One representative mesh, so the transform can be eyeballed against where
  // the player is standing - a matrix of zeroes or of nonsense would mean the
  // offset is wrong, and nothing downstream could be trusted. Taken from the
  // last frame that actually had geometry, since the stats line usually lands
  // on a menu frame.
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_have_sample) {
    const MeshItem& m = g_sample;
    REXLOG_INFO(
        "[native-ue3] sample mesh (from a {}-mesh frame): {} tris, ib={:#x} vf={:#x} mat={:#x} "
        "lci={:#x} dpg={} type={} lod={} verts {}..{} first={} | translation ({:.1f}, {:.1f}, "
        "{:.1f}) scale-row0 ({:.3f}, {:.3f}, {:.3f})",
        g_sample_frame_meshes, m.num_primitives, m.index_buffer, m.vertex_factory,
        m.material_proxy, m.lci, m.depth_priority_group(), m.prim_type(), m.lod_index,
        m.min_vertex, m.max_vertex, m.first_index, m.local_to_world[12], m.local_to_world[13],
        m.local_to_world[14], m.local_to_world[0], m.local_to_world[1], m.local_to_world[2]);
  }
}

}  // namespace dpour_ue3
// === END DPOUR MIGRATION 2026-07-26 ===
