// downpour - Native Render: guest Xenos texture -> native D3D12 texture cache
//
// === DPOUR MIGRATION 2026-07-25: native-render Phase 4 (textures) ===
//
// The game binds its textures through the UE3 RHI (RHISetSamplerState). Behind
// that RHI object sits an Xbox 360 D3DBaseTexture, whose header IS the Xenos
// texture fetch constant: base address, format, size, tiling and swizzle. This
// module reads that header, un-tiles + un-endian-swaps the payload out of guest
// memory ONCE per texture, and uploads it into a real D3D12 texture, handing
// back a slot in a shader-visible SRV heap.
//
// No Xenia GPU emulation is involved: this is a data-format decoder (the same
// maths the hardware does), feeding our own native pipeline.
#pragma once

#include <cstdint>

#include <rex/graphics/d3d12/deferred_command_list.h>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;

namespace dpour_tex {

// Slot 0 of the SRV heap is always a 1x1 opaque white texture, so a draw whose
// texture we could not decode still renders (untextured) instead of sampling
// garbage.
constexpr std::uint32_t kWhiteSlot = 0;
constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

// --- guest render thread ----------------------------------------------------
// RHISetSamplerState: remember which guest texture is bound to a sampler slot.
void SetSampler(std::uint32_t sampler_index, std::uint32_t texture_rhi_guest);
// Pick the guest texture that carries the material's base colour out of the
// currently bound samplers. UE3 binds a material's textures across several
// sampler registers (lightmaps, masks, normal maps, LUTs), and which register
// holds the diffuse depends on the material, so we score the candidates:
// block-compressed colour formats win over masks/normal maps, larger wins over
// smaller, lower sampler index breaks ties. DPOUR_NR_TEXSLOT forces one slot.
std::uint32_t BoundBaseTexture(const std::uint8_t* base);
// Called after every guest draw. What was (re)bound immediately before a draw
// is the per-mesh data: for UE3 that is the directional lightmap PAIR, while a
// material's own textures stay bound across many meshes. That difference is
// what lets us tell a lightmap atlas from a base colour map.
void NextDraw();
// Diagnostics (DPOUR_NR_TEXSURVEY): log the whole sampler table for the draw
// about to happen and dump every bound texture to a slot-tagged BMP.
void SurveyDraw(const std::uint8_t* base, std::uint32_t tag);
// Decode `texture_rhi_guest` if it has not been seen yet and return its SRV
// slot. Returns kWhiteSlot when the texture cannot be decoded.
std::uint32_t Acquire(const std::uint8_t* base, std::uint32_t texture_rhi_guest);
// The GPU base address out of the texture's Xenos fetch constant, or 0.
//
// THIS IS THE ALIAS KEY. UE3 deliberately overlaps render-target textures in one
// buffer - SceneRenderTargets.cpp:1074 creates LightAttenuationMemoryBuffer and
// then points SIX textures at it (LightAttenuation, FilterColor, an object
// shadow buffer, FogFrontfacesIntegralAccumulation, AOInput, AOOutput) with the
// comment "so we can share it with other render target textures". On the console
// they are literally the same bytes: RHICreateSharedTexture2D ends with
// XGOffsetResourceAddress(Resource, SharedMemory->BaseAddress)
// (XeD3DTexture.cpp:1246), so every one of them carries the SAME base address.
//
// Two textures with one base address must therefore be ONE native resource, or a
// resolve into the first is invisible to a sample of the second - which is
// exactly what left the menu showing the loading screen: the final quad samples
// SceneColorLDR, and SceneColorLDR *is* RenderTargets[LightAttenuation]
// (SceneRenderTargets.h:210).
std::uint32_t GuestBaseAddress(const std::uint8_t* base, std::uint32_t texture_guest);
// Reset the per-frame decode budget.
void BeginFrame();

// --- render thread ----------------------------------------------------------
bool EnsureHeap(ID3D12Device* device);
// Upload everything decoded since the last flush onto `cmd` (which must be
// executed before any draw that references the new slots).
// Records onto the command processor's deferred list - the SAME list the rest
// of the frame is recorded on. A second, independent command list submitted
// beside it is exactly the parallel timeline the references do not have.
void Flush(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* cmd);
// Retire upload staging buffers whose copies have completed.
void RetireUploads(std::uint64_t completed_fence, std::uint64_t submitted_fence);

// The guest freed the allocation at `addr` (AddUnusedXeResource). Every cache
// entry decoded from that memory is dropped so it cannot be served again -
// the DestructResource role from UnleashedRecomp (video.cpp:2157), applied to
// the texture cache.
void InvalidateGuestAddress(std::uint32_t addr);
ID3D12DescriptorHeap* Heap();
std::uint64_t GpuHandleAt(std::uint32_t slot);
// Shared bindless-heap allocation, used by the vertex-stream module too: the
// translated shaders read textures and vertex data out of the same heap.
// Returns kInvalidSlot when the heap is full.
std::uint32_t AllocDescriptorSlot();
std::uint64_t CpuHandleAt(std::uint32_t slot);
std::uint64_t GpuHandleStart();
// Descriptor index per guest sampler slot, written into the shaders'
// SharedConstants (g_Tex2DIdx). Unbound or undecodable slots get kWhiteSlot.
void FillSamplerTable(const std::uint8_t* base, std::uint32_t* out, std::uint32_t count);
// Cheap identity of what is bound right now, so a per-draw caller can tell that
// nothing changed and skip re-resolving all sixteen slots. Never 0 when
// anything is bound.
std::uint64_t SamplerSlotsKey();
// True when the slot holds a DXT1 texture. DXT1's alpha is 1-bit punch-through,
// which is exactly how UE3 masks chain-link fences, foliage and grates - so
// those draws want an alpha test, while formats whose alpha carries gloss data
// must not be clipped.
bool SlotIsCutout(std::uint32_t slot);
void LogStats();

// --- scene injection (the write path) ---------------------------------------
// Encodes an RGBA16F image into a guest texture's own layout - format, tiling
// and endian exactly as its fetch constant describes - so the bytes can be
// dropped into guest memory in place of the game's own resolve. Handles the
// scene-colour formats only: 7e3 (k_2_10_10_10_FLOAT_EDRAM=63), 16F x4 (32)
// and 8888 (6/50). Returns false, logging the reason once, otherwise.
struct EncodedSurface {
  std::uint32_t guest_dest = 0;  // guest address the bytes belong at
  std::uint32_t bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  bool tiled = false;
};
bool EncodeColorForGuestTexture(const std::uint8_t* base, std::uint32_t texture_rhi_guest,
                                const std::uint8_t* src_rgba16f, std::uint32_t src_pitch_bytes,
                                std::uint32_t src_w, std::uint32_t src_h,
                                std::vector<std::uint8_t>& out, EncodedSurface& desc);

// Finds the destination texture's fetch constant by scanning the
// FSurfaceRHIRef the resolve call carries: every pointer-sized member and one
// or two indirections behind each, validated by the EXACT expected dimensions.
// Guess-free by construction. Returns the guest address of the fetch constant,
// or 0. One-time cost; cache the result.
std::uint32_t LocateResolveTextureFetch(const std::uint8_t* base, std::uint32_t surface_ref_guest,
                                        std::uint32_t expect_w, std::uint32_t expect_h);

// Encode straight from a known fetch-constant address (found by the locator).
bool EncodeColorForGuestFetch(const std::uint8_t* base, std::uint32_t fetch_addr,
                              const std::uint8_t* src_rgba16f, std::uint32_t src_pitch_bytes,
                              std::uint32_t src_w, std::uint32_t src_h,
                              std::vector<std::uint8_t>& out, EncodedSurface& desc);

}  // namespace dpour_tex
// === END DPOUR MIGRATION 2026-07-25 ===
