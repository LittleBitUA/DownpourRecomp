// downpour - Native Render: guest shader-container harvester (Phase 5 shaders)
//
// === DPOUR MIGRATION 2026-07-25: native-render shader harvest ===
// UE3's cooked packages are LZO-compressed, so the Xenos shader containers can
// only be reached AFTER the game decompresses and creates them. Two harvesters
// live here:
//
//   * MaybeHarvest()       - the original blind RAM scan. It only ever finds
//                            containers that sit contiguously in memory, which
//                            in practice means the XDK's own WMV/VC-1 decoder
//                            shaders (static XEX data). UE3's own shaders are
//                            NOT findable this way - see below.
//   * OnBoundShaderState() - the real one. FXeVertexShader splits the container
//                            into a "cached part" (the D3D shader object, which
//                            still starts with the container header + constant
//                            table) and a "physical part" (the microcode, in
//                            its own physical allocation), so the container
//                            never exists contiguously again. This walks the
//                            bound shader state exactly as the game's own code
//                            does and stitches the two halves back together.
//
// Both are gated by env DPOUR_NR_DUMP_SHADERS (default OFF).
#pragma once

#include <cstdint>

namespace dpour_shaders {

bool DumpEnabled();

// The RAM scan additionally needs DPOUR_NR_DUMP_RAMSCAN=1. It walks all of
// guest memory on the render thread, which costs whole frames, and the
// constructor hooks below have made it redundant - it now only ever finds the
// XDK video-decoder shaders it always found.
bool RamScanEnabled();

// Called once per presented frame; performs a scan at a few frame milestones
// (early, after streaming settles) and writes any newly seen containers to
// <exe dir>/shader_dump/<hash>.bin.
void MaybeHarvest(const std::uint8_t* base);

// Called from the RHISetBoundShaderState hook with the guest address of the
// FBoundShaderStateRHIRef. Reconstructs and dumps the full containers of the
// bound vertex shader (both the original and the declaration-patched clone the
// game actually draws with) and of the pixel shader.
void OnBoundShaderState(const std::uint8_t* base, std::uint32_t bss);

// Called from the FXeVertexShader / FXePixelShader constructor hooks
// (sub_829CFB50 / sub_829CFD30) with `this` and the complete shader container
// the game was handed. This is the only place the container exists in one
// piece: the constructor immediately splits it into the D3D object (which does
// NOT keep the container header - verified against live memory) and a separate
// physical allocation for the microcode.
void OnShaderCreated(const std::uint8_t* base, std::uint32_t shader_object,
                     std::uint32_t code_guest);

// Where a shader object's microcode lives relative to its BaseAddress, and how
// many bytes of it identify the shader.
//
// Both numbers come from the container's shader sub-header, and BOTH matter:
// the offline translator hashes the microcode at
// (virtualSize + physicalOffset) for sub_header.size bytes, and BaseAddress is
// the start of the physical part - i.e. container offset virtualSize. Hashing
// from BaseAddress with the container's physicalSize instead, which is what an
// earlier version did, only agrees where physicalOffset happens to be 0 and the
// two sizes happen to match: a live run resolved 36 shaders out of 367 that way.
struct UcodeRange {
  std::uint32_t offset = 0;  // added to BaseAddress
  std::uint32_t size = 0;
};

// Returns false for objects whose creation we never saw.
bool UcodeRangeFor(std::uint32_t shader_object, UcodeRange& out);

}  // namespace dpour_shaders
// === END DPOUR MIGRATION 2026-07-25 ===
