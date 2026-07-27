// downpour - Native Render: translated-shader cache
//
// === DPOUR MIGRATION 2026-07-25: real game shaders, not an approximation ===
//
// The hand-written "diffuse x lambert" shader the native renderer used until now
// cannot reproduce UE3: a material's look comes from its own compiled shader,
// which computes UVs (scale/pan/layer blending), masks, lightmap UV1 scale-bias
// and so on. This module is the runtime half of the pipeline that replaces it:
//
//   1. downpour_shader_dump.cpp stitches every bound shader back into a Xenos
//      container and writes it out            (env DPOUR_NR_DUMP_SHADERS=1)
//   2. XenosRecomp translates those containers to DXIL offline, emitting
//      <name>.bin (blobs) + <name>.idx (index keyed by microcode hash)
//   3. this module loads that pair and hands the renderer the DXIL for whatever
//      the game just bound
//
// The lookup key is an FNV-1a hash of the RAW big-endian microcode as it sits in
// the shader object's physical allocation, which is exactly what both sides see:
// no normalization, no reflection, nothing to guess.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dpour_shadercache {

// One Xenos vertex fetch instruction's static operands, read out of the
// microcode by the translator. `stride_dwords` is the vertex stride of the
// stream this slot reads and `offset_dwords` the element's position inside the
// vertex, which is what lets the runtime decide WHICH bound stream feeds the
// slot instead of assuming an order.
//
// Two things here were established by dumping real tables, not assumed:
//   * slots count DOWN from 95 (the first declaration element is slot 95), so
//     an ascending element->slot mapping would have read every vertex from the
//     wrong offsets;
//   * a slot with stride_dwords == 0 belongs to UNPATCHED microcode - the
//     compiler's placeholder that IDirect3DVertexShader9::Bind rewrites. Those
//     appear only in the original shaders, never in the bound clones we draw
//     with, and must never be fed to the GPU.
struct VertexFetchDesc {
  std::uint32_t slot;
  std::uint32_t stride_dwords;
  std::uint32_t offset_dwords;
  std::uint32_t format;
};

struct Shader {
  const std::uint8_t* dxil = nullptr;
  std::uint32_t dxil_size = 0;
  std::uint32_t spec_constants = 0;
  bool pixel_shader = false;
  std::uint64_t ucode_hash = 0;
  const VertexFetchDesc* fetches = nullptr;
  std::uint32_t fetch_count = 0;
};

// Loads <exe dir>/dpour_shaders.{idx,bin} once. Returns false when the cache is
// absent, which is the normal state until a harvest+translate cycle has run.
bool Load();

// True when a cache was loaded and has entries.
bool Available();

// Look a shader up by the hash of its microcode.
const Shader* Find(std::uint64_t ucode_hash);

// Resolve a guest FXeVertexShader*/FXePixelShader* to its translated shader.
// Walks the same fields the game's own code does (FXeGPUResource::Resource for
// the container header, ::BaseAddress for the microcode), hashes the microcode
// and looks it up. Results are memoised per guest object, including misses.
const Shader* ForGuestShader(const std::uint8_t* base, std::uint32_t guest_object);

// Hit/miss counters for the log line printed once a second.
void ReportStats();

}  // namespace dpour_shadercache
// === END DPOUR MIGRATION 2026-07-25 ===
