// downpour - Native Render: guest D3D vertex declaration reader
//
// === DPOUR MIGRATION 2026-07-25: native-render Phase 5 (all vertex formats) ===
//
// Until now geometry capture only understood ONE layout (UE3's dedicated float3
// position stream, stride 12). Everything else - BSP, terrain, foliage, decals,
// anything whose vertex factory packs position together with the rest - was
// dropped, which is why large parts of a level rendered as empty black.
//
// The guest already describes every layout precisely: each bound shader state
// carries a vertex declaration, i.e. a D3DVERTEXELEMENT9 array. This module
// finds that array in guest memory and turns it into the offsets we need, so
// the renderer stops guessing.
#pragma once

#include <cstdint>

namespace dpour_decl {

// D3DDECLTYPE values we can read.
enum : std::uint8_t {
  kFloat1 = 0,
  kFloat2 = 1,
  kFloat3 = 2,
  kFloat4 = 3,
  kD3DColor = 4,
  kUByte4 = 5,
  kShort2 = 6,
  kShort4 = 7,
  kUByte4N = 8,
  kShort2N = 9,
  kShort4N = 10,
  kUShort2N = 11,
  kUShort4N = 12,
  kUDec3 = 13,
  kDec3N = 14,
  kFloat16_2 = 15,
  kFloat16_4 = 16,
  kUnused = 17,
};

struct Element {
  std::uint8_t stream = 0xFF;
  std::uint8_t offset = 0;
  std::uint8_t type = kUnused;
  bool valid() const { return stream != 0xFF; }
};

// UE3's own element description (Engine/Inc/RHI.h FVertexElement), kept in the
// order the guest sorts it into before calling CreateVertexDeclaration - by
// stream, then offset. THAT order is what the X360 D3D runtime assigns Xenos
// vertex fetch slots in, so element i is fetch slot i.
struct FullElement {
  std::uint8_t stream = 0;
  std::uint8_t offset = 0;      // byte offset within the vertex
  std::uint8_t type = 0;        // VET_*
  std::uint8_t usage = 0;       // VEU_*
  std::uint8_t usage_index = 0;
};

constexpr std::uint32_t kMaxElements = 16;

struct Layout {
  Element position;
  Element normal;    // NORMAL, or TANGENT/BINORMAL as a fallback
  Element texcoord;  // TEXCOORD0
  bool skinned = false;  // has BLENDINDICES: needs bone matrices we do not apply
  bool valid = false;

  FullElement elements[kMaxElements];
  std::uint32_t element_count = 0;
};

// Resolve (and cache) the layout behind a guest vertex-declaration pointer.
// Returns a layout with valid == false when it cannot be parsed.
const Layout& Resolve(const std::uint8_t* base, std::uint32_t decl_guest);

// Called from the RHICreateVertexDeclaration hook (sub_829D40F8) once the guest
// has produced the IDirect3DVertexDeclaration9. `elements` is the guest address
// of the FVertexDeclarationElementList: 16 inline FVertexElement (16 bytes
// each) followed by the count at +256.
//
// This is the ONLY way to learn a layout: the D3DVERTEXELEMENT9 array the guest
// builds lives on the stack and is gone by the time anything draws, and the
// declaration object itself is compiled by the X360 driver. Scanning memory for
// it - which is what this module used to do - finds nothing.
void OnCreateDeclaration(const std::uint8_t* base, std::uint32_t decl_guest,
                         std::uint32_t elements_guest);

// Number of declarations captured so far (diagnostics).
std::uint32_t RegisteredCount();

// Read one element as up to 4 floats. Guest vertex data is big-endian.
void ReadElement(const std::uint8_t* p, std::uint8_t type, float out[4]);

// Bytes one element of this type occupies (0 when unsupported).
std::uint32_t ElementSize(std::uint8_t type);

}  // namespace dpour_decl
// === END DPOUR MIGRATION 2026-07-25 ===
