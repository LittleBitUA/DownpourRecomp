// downpour - Native Render: pipeline state for the game's OWN shaders
//
// === DPOUR MIGRATION 2026-07-25: real shaders need a real binding contract ===
//
// Everything here exists to satisfy exactly one thing: the binding layout that
// XenosRecomp bakes into every translated shader (XenosRecomp/shader_common.h).
// That layout is not a choice we get to make - it is compiled into the DXIL:
//
//   b0 space4  VertexShaderConstants   float4 [256]      4096 bytes
//   b1 space4  PixelShaderConstants    float4 [256]      4096 bytes
//   b2 space4  SharedConstants         see kShared* below  704 bytes
//   t0 space0  Texture2D[]         unbounded, dynamically indexed
//   t0 space1  Texture3D[]         same heap, different view type
//   t0 space2  TextureCube[]       same heap
//   t0 space6  Texture1D[]         same heap
//   s0 space3  SamplerState[]      unbounded, sampler heap
//
// Vertices reach the shader the way UnleashedRecomp does it, NOT through any
// fetch emulation of ours: the translated vertex shader declares one input per
// vertex element the container describes, and the guest's vertex declaration is
// turned into a matching host input layout (see UnleashedRecomp's
// ConvertDeclType / CreateVertexDeclarationWithoutAddRef). Guest vertex data is
// byte-swapped dword-wise once, when the game unlocks the buffer; the two cases
// that swap does not cover - 16-bit texcoords and packed normals - are fixed up
// inside the shader through g_SwappedTexcoords and g_R11G11B10Normal.
//
// An earlier revision of this file had the shaders fetch their own vertices out
// of a ByteAddressBuffer. That was built on the belief that Downpour's shader
// containers carry no vertex element table, which measurement disproved: 218 of
// the game's vertex shaders have one, and the containers that do not are the
// XDK's video-decoder shaders, which are not the game's at all.
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/graphics/d3d12/deferred_command_list.h>

struct ID3D12Device;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;

namespace dpour_shadercache {
struct Shader;
}
namespace dpour_decl {
struct Layout;
}

namespace dpour_pipeline {

// --- host input layout, built from the guest's vertex declaration -----------
//
// The mapping is not guessed anywhere: the game's own source gives it. From
// Development/Src/Xenon/XeD3DDrv/Src/XeD3DVertexDeclaration.cpp:36-47 the guest
// converts its EVertexElementType to D3DDECLTYPE like this -
//
//   VET_Float1..4   -> FLOAT1..4          VET_Color   -> D3DCOLOR
//   VET_PackedNormal-> UBYTE4  (NOT N!)   VET_Short2  -> SHORT2
//   VET_UByte4      -> UBYTE4             VET_Short2N -> SHORT2N
//   VET_UByte4N     -> UBYTE4N            VET_Half2   -> FLOAT16_2
//   VET_Pos3N       -> HEND3N   ("signed 11 11 10", Engine/Inc/RHI.h:259)
//
// and Engine/Inc/RHI.h:245 / :263 give the enum values used on the wire.
//
// How each element is then bound follows UnleashedRecomp: NORMAL/TANGENT/
// BINORMAL are declared uint4 by the translator, so they are bound as UINT
// formats and unpacked inside the shader; 16-bit texcoord pairs come out of the
// dword byte-swap in the wrong order, so their semantic index is recorded in a
// mask the shader uses to swap them back.
// The mode written into SharedConstants IS the guest's EVertexElementType for
// the normal/tangent/binormal element, so the shader decodes exactly what the
// declaration said. 0 means "no such element", which the shader treats as raw
// float bits.
enum NormalMode : std::uint32_t {
  kNormalRawFloatBits = 0,
};

// Opaque, owned and cached by this module. Built once per distinct declaration.
struct DeclLayout;

// Builds (or returns the cached) host input layout for a guest declaration.
// Returns nullptr when the declaration describes nothing we can bind.
const DeclLayout* GetDeclLayout(const dpour_decl::Layout& decl);

std::uint64_t DeclHash(const DeclLayout* layout);
// Bit per vertex stream the layout reads, so a draw binds exactly the streams
// the shader's inputs come from and no others.
std::uint32_t DeclStreamMask(const DeclLayout* layout);
// Bit per TEXCOORD semantic index whose two 16-bit halves need swapping back.
std::uint32_t DeclSwappedTexcoords(const DeclLayout* layout);
NormalMode DeclNormalMode(const DeclLayout* layout);


// Root parameter slots, in the order the root signature declares them.
//
// The SRV spaces get one root parameter each rather than one table with several
// ranges, because D3D12 only allows an unbounded range as the LAST range of a
// table (nothing can be appended after something of unknown size). They are all
// set to the same GPU handle - the one bindless SRV heap - so a descriptor index
// means the same thing whichever view type reads it.
enum RootSlot : std::uint32_t {
  kRootVertexConstants = 0,  // b0 space4, root CBV
  kRootPixelConstants = 1,   // b1 space4, root CBV
  kRootSharedConstants = 2,  // b2 space4, root CBV
  kRootTexture2D = 3,        // t0 space0
  kRootTexture3D = 4,        // t0 space1
  kRootTextureCube = 5,      // t0 space2
  kRootTexture1D = 6,        // t0 space6
  kRootSamplers = 7,         // s0 space3
  kRootSlotCount = 8,
};

// Binds all six descriptor tables to the bindless heaps. Call once per command
// list after SetGraphicsRootSignature/SetDescriptorHeaps; the tables never
// change afterwards, only the indices inside the constant buffers do.
void BindDescriptorTables(rex::graphics::d3d12::DeferredCommandList* cmd,
                          std::uint64_t srv_heap_gpu_start);

// Byte offsets inside the b2 SharedConstants buffer. These mirror the
// packoffset()s the recompiler emits; the buffer is a flat 704 bytes.
constexpr std::uint32_t kSharedTex2DIdx = 0;     // uint[32] - descriptor index per sampler slot
constexpr std::uint32_t kSharedTex3DIdx = 128;   // uint[32]
constexpr std::uint32_t kSharedTexCubeIdx = 256; // uint[32]
constexpr std::uint32_t kSharedSampIdx = 384;    // uint[32]
constexpr std::uint32_t kSharedBools = 512;      // uint[8]  - b0..b255 bitfield
constexpr std::uint32_t kSharedLoops = 544;      // uint[32] - loop registers
constexpr std::uint32_t kSharedSwappedTexcoords = 672;
constexpr std::uint32_t kSharedHalfPixelOffset = 676;  // float2
constexpr std::uint32_t kSharedAlphaThreshold = 684;   // float
constexpr std::uint32_t kSharedAlphaTest = 688;
constexpr std::uint32_t kSharedR11G11B10Normal = 692;
constexpr std::uint32_t kSharedConstantsSize = 704;

constexpr std::uint32_t kVertexConstantsSize = 256 * 16;
constexpr std::uint32_t kPixelConstantsSize = 256 * 16;

// The b2 buffer, laid out field for field as the recompiler declares it. Fill
// one of these and memcpy it - the offsets above are the same numbers, kept
// separately only so a mismatch is caught by a static_assert rather than by a
// shader reading the wrong slot.
struct SharedConstants {
  std::uint32_t tex2d_index[32];    // descriptor heap index per sampler slot
  std::uint32_t tex3d_index[32];
  std::uint32_t texcube_index[32];
  std::uint32_t sampler_index[32];  // index into the sampler heap
  std::uint32_t bools[8];           // b0..b255
  std::uint32_t loops[32];          // count | start<<8 | step<<16, per loop register
  std::uint32_t swapped_texcoords;  // bit per TEXCOORD semantic index
  float half_pixel_offset_x;        // 1 / target width   (the X360 pixel centre rule)
  float half_pixel_offset_y;        // -1 / target height
  float alpha_threshold;
  std::uint32_t alpha_test;
  std::uint32_t normal_mode;  // g_R11G11B10Normal, see NormalMode
  std::uint32_t pad[2];
};
static_assert(sizeof(SharedConstants) == kSharedConstantsSize, "b2 layout must match the shaders");
static_assert(offsetof(SharedConstants, sampler_index) == kSharedSampIdx, "b2 layout drift");
static_assert(offsetof(SharedConstants, bools) == kSharedBools, "b2 layout drift");
static_assert(offsetof(SharedConstants, loops) == kSharedLoops, "b2 layout drift");
static_assert(offsetof(SharedConstants, swapped_texcoords) == kSharedSwappedTexcoords,
              "b2 layout drift");
static_assert(offsetof(SharedConstants, alpha_test) == kSharedAlphaTest, "b2 layout drift");
static_assert(offsetof(SharedConstants, normal_mode) == kSharedR11G11B10Normal,
              "b2 layout drift");

// Everything a PSO depends on besides the two shaders. Kept POD and compared
// bytewise, so any field added here automatically participates in the cache key.
// The declaration hash and the per-stream strides are part of the key for the
// same reason they are in UnleashedRecomp's: they decide the input layout, so
// two draws that agree on both shaders but not on the vertex layout are two
// different pipelines.
constexpr std::uint32_t kMaxStreams = 16;

struct PsoKey {
  std::uint64_t vs_hash;
  std::uint64_t ps_hash;
  std::uint64_t decl_hash;
  std::uint32_t rtv_format;  // DXGI_FORMAT
  std::uint32_t dsv_format;  // DXGI_FORMAT, 0 = no depth
  std::uint8_t depth_enable;
  std::uint8_t depth_write;
  std::uint8_t depth_func;  // D3D12_COMPARISON_FUNC
  std::uint8_t cull_mode;   // D3D12_CULL_MODE
  std::uint8_t front_ccw;   // which winding the guest calls front
  std::uint8_t blend_enable;
  std::uint8_t src_blend;   // D3D12_BLEND
  std::uint8_t dst_blend;   // D3D12_BLEND
  std::uint8_t blend_op;    // D3D12_BLEND_OP
  // The Xbox 360 blend register carries separate alpha factors and the game does
  // use them (RHISetBlendState packs AlphaSourceBlendFactor / AlphaBlendOperation
  // / AlphaDestBlendFactor into bits 16-28), so they belong in the key rather
  // than being approximated by the colour ones.
  std::uint8_t src_blend_alpha;
  std::uint8_t dst_blend_alpha;
  std::uint8_t blend_op_alpha;
  std::uint8_t topology;    // D3D12_PRIMITIVE_TOPOLOGY_TYPE
  std::uint8_t color_write_mask;
  // Explicit tail padding to the struct's natural 8-byte alignment, so that
  // hashing and comparing all sizeof(PsoKey) bytes never reads uninitialised
  // compiler padding. Always build keys as `PsoKey k{};`.
  std::uint8_t pad[2];
  std::uint16_t strides[kMaxStreams];
};
static_assert(sizeof(PsoKey) == 80, "PsoKey must stay padding-free for bytewise hashing");

// DPOUR_NR_DRAW_FLAT: draw with a flat-colour pixel shader instead of the
// game's own. It answers the one question a black picture cannot: whether the
// geometry, the transform and the depth state are right, with the material taken
// out of the equation entirely. A lit-but-black scene and a wrongly-transformed
// scene look identical until this is turned on.
bool FlatShadingEnabled();

// Creates the root signature and the sampler heap. Safe to call repeatedly.
bool Init(ID3D12Device* device);

ID3D12RootSignature* RootSignature();
ID3D12DescriptorHeap* SamplerHeap();

// Index into the sampler heap for a filter/address combination. `filter` is
// 0 point, 1 linear, 2 anisotropic; the address modes are 0 wrap, 1 clamp,
// 2 mirror. Out-of-range values clamp to the nearest valid one.
std::uint32_t SamplerIndex(std::uint32_t filter, std::uint32_t address_u,
                           std::uint32_t address_v);

// Device SetSamplerState(dev, Sampler, D3DSAMPLERSTATETYPE, DWORD value).
// The game reaches it from RHISetSamplerState, which sets MAG/MIN/MIP filter
// and ADDRESSU/V/W per texture slot (XeD3DCommands.cpp:313-317).
void SetGuestSamplerState(std::uint32_t sampler, std::uint32_t state, std::uint32_t value);

// Sampler-heap index per guest sampler slot, for SharedConstants::sampler_index.
void FillSamplerIndices(std::uint32_t* out, std::uint32_t count);

// Returns the PSO for this key, creating and caching it on first use. Returns
// nullptr if either shader is missing or creation fails (which is cached too,
// so a broken combination is not retried every frame).
//
// `layout` is only read on a cache miss, because the key already identifies it
// through decl_hash and the strides.
ID3D12PipelineState* GetPso(ID3D12Device* device, const PsoKey& key,
                            const dpour_shadercache::Shader* vs,
                            const dpour_shadercache::Shader* ps,
                            const DeclLayout* layout);

// --- per-frame constant upload ring ----------------------------------------
//
// Constants change per draw, so they cannot live in a single buffer. This is a
// plain linear allocator over a persistently mapped upload heap, reset once the
// GPU is known to be done with a frame's slice.
struct ConstantAlloc {
  void* cpu;              // write here
  std::uint64_t gpu;      // D3D12_GPU_VIRTUAL_ADDRESS for SetGraphicsRootConstantBufferView
};

// `frame` is the render-thread frame counter; `completed` is the highest frame
// the GPU has finished. Older slices become reusable.
void BeginFrame(ID3D12Device* device, std::uint64_t frame, std::uint64_t completed);

// 256-byte aligned as D3D12 requires for CBVs. Returns {nullptr, 0} when the
// ring is exhausted for this frame - callers must skip the draw, not stall.
ConstantAlloc AllocConstants(std::uint32_t bytes);

// The REPLAY's own upload memory, on the render thread.
//
// It cannot share the capture allocator: that one rotates per GUEST frame, on
// the guest thread, which runs ahead - so a replay would be writing into the
// allocator of a frame the guest has already moved past, and its data would be
// reset out from under the submission still reading it. This one rotates on the
// command processor's SUBMISSION counters, which is precisely how long the data
// has to live. (UnleashedRecomp has one allocator per frame and resets it after
// that frame's fence; ours is the same rule with the submission as the unit.)
void BeginReplay(ID3D12Device* device, std::uint64_t submission, std::uint64_t completed);
ConstantAlloc AllocReplayConstants(std::uint32_t bytes);

// --- dry run ----------------------------------------------------------------
//
// Builds everything a draw would need - input layout from the declaration, PSO
// from the two translated shaders - and then throws it away without drawing.
//
// This is the one check worth doing before any pixel changes: if D3D12 refuses
// to create a pipeline from the game's own translated shaders against our root
// signature, nothing further can work, and the failure says exactly which of
// the two sides disagrees. Gated by DPOUR_NR_SHADERS_DRYRUN=1.
bool DryRunEnabled();
void DryRunDraw(const dpour_decl::Layout& decl, const dpour_shadercache::Shader* vs,
                const dpour_shadercache::Shader* ps, const std::uint16_t* strides,
                std::uint32_t stride_count);
void DryRunReport();

void LogStats();

}  // namespace dpour_pipeline
// === END DPOUR MIGRATION 2026-07-25 ===
