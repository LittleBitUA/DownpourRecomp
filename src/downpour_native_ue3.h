// downpour - Native Render: the ENGINE-level scene stream (UE3)
//
// === DPOUR MIGRATION 2026-07-26: data-driven layer, the skate3recomp approach ===
//
// Everything else in this renderer watches the D3D layer: which buffer was
// bound, which surface was targeted, which shader was set. That view is
// faithful but blind - it knows a triangle list was drawn and nothing about
// what it IS, which is why choosing "the pass that becomes the frame" has been
// guesswork.
//
// This layer watches the GAME instead. skate3recomp's native renderer consumes
// its engine's own per-frame mesh stream and walks the guest structures; the
// exact analogue in UE3 is FMeshDrawingPolicy::DrawMesh, through which EVERY
// mesh the renderer submits passes exactly once, carrying its transform, its
// material and its priority group.
//
//   sub_828194E8 = FMeshDrawingPolicy::DrawMesh(const FMeshElement& Mesh)
//                  r4 = &Mesh
//
// It is the only caller of RHIDrawIndexedPrimitive in the entire binary
// (downpour_recomp.43.cpp:5852), which is what proves the "exactly once".
//
// The layout below is NOT guessed: it is Scene.h:599 from the game's own
// source, checked against the recompiled function instruction by instruction
// (see downpour_native_ue3.cpp for the evidence).
#pragma once

#include <cstdint>

namespace dpour_ue3 {

// One mesh as the engine submitted it. Guest pointers stay guest pointers -
// this is an observation of the game's frame, not a host resource.
struct MeshItem {
  std::uint32_t index_buffer = 0;     // FIndexBuffer*     (+0)
  std::uint32_t vertex_factory = 0;   // FVertexFactory*   (+4)
  std::uint32_t dyn_vertex_data = 0;  // user memory       (+8)
  std::uint32_t dyn_index_data = 0;   // user memory       (+16)
  std::uint32_t material_proxy = 0;   // FMaterialRenderProxy* (+20)
  std::uint32_t lci = 0;              // FLightCacheInterface* (+24) - lightmaps
  std::uint32_t decal_state = 0;      // FDecalState*      (+28)
  float local_to_world[16] = {};      // FMatrix           (+48)
  std::uint32_t first_index = 0;      // (+176)
  std::uint32_t num_primitives = 0;   // (+180)
  std::uint32_t min_vertex = 0;       // (+184)
  std::uint32_t max_vertex = 0;       // (+188)
  std::int32_t lod_index = 0;         // (+192)
  std::uint16_t dyn_vertex_stride = 0;  // (+12)
  std::uint16_t dyn_index_stride = 0;   // (+14)
  std::uint32_t flags = 0;            // raw bitfield word (+208)

  // Bitfield accessors. UE3 packs BITFIELDs from the most significant bit down
  // on this compiler, which the disassembly confirms: `rlwinm r10,r11,0,0,0`
  // isolates bit 0 as UseDynamicData, and `rlwinm r3,r11,8,29,31` lifts the
  // three-bit primitive type out of bits 5-7.
  bool use_dynamic_data() const { return (flags & 0x80000000u) != 0; }
  bool reverse_culling() const { return (flags & 0x40000000u) != 0; }
  bool disable_backface_culling() const { return (flags & 0x20000000u) != 0; }
  bool cast_shadow() const { return (flags & 0x10000000u) != 0; }
  bool wireframe() const { return (flags & 0x08000000u) != 0; }
  std::uint32_t prim_type() const { return (flags >> 24) & 0x7u; }      // bits 5-7
  std::uint32_t particle_type() const { return (flags >> 22) & 0x3u; }  // bits 8-9
  // SDPG_UnrealEdBackground/World/Foreground - which tells the world apart
  // from the first-person/HUD geometry without looking at a render target.
  std::uint32_t depth_priority_group() const { return (flags >> 19) & 0x7u; }
};

// DPOUR_NR_UE3 - default OFF, like every other visual switch here.
bool Enabled();

// The hook body: FMeshDrawingPolicy::DrawMesh, before the engine's own draw.
void OnDrawMesh(const std::uint8_t* base, std::uint32_t mesh_guest);

// The mesh the engine is submitting RIGHT NOW, or null.
//
// This is what fuses the two views of the frame. DrawMesh is the immediate
// caller of RHIDrawIndexedPrimitive - nothing else happens in between - so the
// very next draw the D3D layer captures IS this mesh. The D3D side already
// knows the buffers, the translated shaders, the textures and the pipeline
// state perfectly; what it could never recover is what the mesh MEANS. Reading
// it here costs one pointer load per draw and gives every captured draw the
// engine's own answer: world or foreground, which material, which lightmap,
// where it stands.
//
// Guest render thread only - the same thread runs DrawMesh and the draw that
// follows it, so no lock is needed and none is taken.
const MeshItem* CurrentMesh();

// Frame boundaries, driven from the same RHI viewport hooks as everything else.
void BeginFrame();
void EndFrame();

// What the engine handed us this frame, in one line.
void LogStats();

}  // namespace dpour_ue3
// === END DPOUR MIGRATION 2026-07-26 ===
