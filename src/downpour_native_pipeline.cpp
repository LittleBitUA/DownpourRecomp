// downpour - Native Render: pipeline state for the game's OWN shaders
//
// === DPOUR MIGRATION 2026-07-25: real shaders need a real binding contract ===
// See downpour_native_pipeline.h for the binding layout and where it came from.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_pipeline.h"

#include "downpour_native_decl.h"
#include "downpour_native_shaders.h"
#include "downpour_native_tex.h"  // owns the bindless heap, and through it the device

#include <atomic>
#include <climits>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <rex/logging.h>

using Microsoft::WRL::ComPtr;

namespace dpour_pipeline {
namespace {

ComPtr<ID3D12RootSignature> g_root_sig;
ComPtr<ID3D12DescriptorHeap> g_sampler_heap;
std::uint64_t g_sampler_gpu_start = 0;
bool g_init_failed = false;
std::mutex g_mutex;

// filter (point/linear/aniso) x addressU (wrap/clamp/mirror) x addressV.
constexpr std::uint32_t kFilterCount = 3;
constexpr std::uint32_t kAddressCount = 3;
constexpr std::uint32_t kSamplerCount = kFilterCount * kAddressCount * kAddressCount;

D3D12_TEXTURE_ADDRESS_MODE AddressMode(std::uint32_t mode) {
  switch (mode) {
    case 1:
      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 2:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    default:
      return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  }
}

bool CreateSamplerHeap(ID3D12Device* device) {
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  desc.NumDescriptors = kSamplerCount;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_sampler_heap)))) {
    REXLOG_ERROR("[native-pipeline] sampler heap creation failed");
    return false;
  }

  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_sampler_heap->GetCPUDescriptorHandleForHeapStart();
  g_sampler_gpu_start = g_sampler_heap->GetGPUDescriptorHandleForHeapStart().ptr;

  for (std::uint32_t f = 0; f < kFilterCount; ++f) {
    for (std::uint32_t u = 0; u < kAddressCount; ++u) {
      for (std::uint32_t v = 0; v < kAddressCount; ++v) {
        D3D12_SAMPLER_DESC s{};
        switch (f) {
          case 0:
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            break;
          case 1:
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            break;
          default:
            s.Filter = D3D12_FILTER_ANISOTROPIC;
            break;
        }
        s.AddressU = AddressMode(u);
        s.AddressV = AddressMode(v);
        s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxAnisotropy = (f == 2) ? 8 : 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.MinLOD = 0.0f;
        s.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += static_cast<SIZE_T>(SamplerIndex(f, u, v)) * stride;
        device->CreateSampler(&s, h);
      }
    }
  }
  return true;
}

bool CreateRootSignature(ID3D12Device* device) {
  D3D12_ROOT_PARAMETER params[kRootSlotCount]{};

  // b0..b2 space4 as root descriptors: no descriptor heap traffic for the
  // constants, just a GPU virtual address per draw.
  const std::uint32_t cbv_registers[3] = {0, 1, 2};
  for (std::uint32_t i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = cbv_registers[i];
    params[i].Descriptor.RegisterSpace = 4;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // One unbounded range per space, one table per range - see the header.
  struct TableDesc {
    RootSlot slot;
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    std::uint32_t space;
  };
  const TableDesc tables[] = {
      {kRootTexture2D, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0},
      {kRootTexture3D, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
      {kRootTextureCube, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2},
      {kRootTexture1D, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6},
      {kRootSamplers, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3},
  };
  // Ranges must outlive serialization, so they live in a fixed array here.
  D3D12_DESCRIPTOR_RANGE ranges[std::size(tables)]{};
  for (std::size_t i = 0; i < std::size(tables); ++i) {
    ranges[i].RangeType = tables[i].type;
    ranges[i].NumDescriptors = UINT_MAX;  // unbounded
    ranges[i].BaseShaderRegister = 0;
    ranges[i].RegisterSpace = tables[i].space;
    ranges[i].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER& p = params[tables[i].slot];
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &ranges[i];
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = kRootSlotCount;
  desc.pParameters = params;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> err;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
  if (FAILED(hr)) {
    REXLOG_ERROR("[native-pipeline] SerializeRootSignature hr={:#010x} {}",
                 static_cast<std::uint32_t>(hr),
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_root_sig)))) {
    REXLOG_ERROR("[native-pipeline] CreateRootSignature failed");
    return false;
  }
  return true;
}

// --- host input layout ------------------------------------------------------

// UE3 EVertexElementType (Engine/Inc/RHI.h:245).
enum : std::uint8_t {
  kVetNone = 0,
  kVetFloat1 = 1,
  kVetFloat2 = 2,
  kVetFloat3 = 3,
  kVetFloat4 = 4,
  kVetPackedNormal = 5,
  kVetUByte4 = 6,
  kVetUByte4N = 7,
  kVetColor = 8,
  kVetShort2 = 9,
  kVetShort2N = 10,
  kVetHalf2 = 11,
  kVetPos3N = 12,
};

// UE3 EVertexElementUsage (Engine/Inc/RHI.h:263), in the order the semantic
// strings XenosRecomp emits expect (shader_recompiler.cpp USAGE_SEMANTICS).
const char* SemanticFor(std::uint8_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "TEXCOORD";
    case 2: return "BLENDWEIGHT";
    case 3: return "BLENDINDICES";
    case 4: return "NORMAL";
    case 5: return "TANGENT";
    case 6: return "BINORMAL";
    case 7: return "COLOR";
    default: return nullptr;
  }
}

DXGI_FORMAT FormatFor(std::uint8_t vet) {
  switch (vet) {
    case kVetFloat1: return DXGI_FORMAT_R32_FLOAT;
    case kVetFloat2: return DXGI_FORMAT_R32G32_FLOAT;
    case kVetFloat3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case kVetFloat4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case kVetPackedNormal:  // -> D3DDECLTYPE_UBYTE4, unsigned and NOT normalized
    case kVetUByte4: return DXGI_FORMAT_R8G8B8A8_UINT;
    case kVetUByte4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case kVetColor: return DXGI_FORMAT_B8G8R8A8_UNORM;  // D3DCOLOR is BGRA
    case kVetShort2: return DXGI_FORMAT_R16G16_SINT;
    case kVetShort2N: return DXGI_FORMAT_R16G16_SNORM;
    case kVetHalf2: return DXGI_FORMAT_R16G16_FLOAT;
    // HEND3N is one dword holding signed 11/11/10; nothing in DXGI matches, so
    // it goes in raw and the shader unpacks it - which is exactly the case
    // tfetchR11G11B10 exists for.
    case kVetPos3N: return DXGI_FORMAT_R32_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// NORMAL/TANGENT/BINORMAL inputs are declared uint4 by the translator, so they
// must be bound as UINT and decoded in the shader. The mode handed to the
// shader is the guest's element type itself, so nothing is lost on the way.
//
// Anything packed into a single dword is bound as one R32_UINT rather than as
// its "natural" two-component format: the vertex data was byte-swapped per
// dword, so the dword is in native order and the fields inside it are exactly
// where the hardware saw them - whereas a two-component format would hand the
// halves over in the swapped order.
DXGI_FORMAT NormalFormatFor(std::uint8_t vet, NormalMode& mode) {
  mode = static_cast<NormalMode>(vet);
  switch (vet) {
    case kVetFloat1: return DXGI_FORMAT_R32_UINT;
    case kVetFloat2: return DXGI_FORMAT_R32G32_UINT;
    case kVetFloat3: return DXGI_FORMAT_R32G32B32_UINT;
    case kVetFloat4: return DXGI_FORMAT_R32G32B32A32_UINT;
    case kVetPackedNormal:
    case kVetUByte4:
    case kVetUByte4N:
    case kVetColor: return DXGI_FORMAT_R8G8B8A8_UINT;
    case kVetShort2:
    case kVetShort2N:
    case kVetHalf2:
    case kVetPos3N: return DXGI_FORMAT_R32_UINT;
    default: mode = kNormalRawFloatBits; return DXGI_FORMAT_UNKNOWN;
  }
}

// Only formats whose components are 16 bits wide survive the dword-wise byte
// swap in the wrong order, so only those need swapping back in the shader.
bool NeedsTexcoordSwap(std::uint8_t vet) {
  return vet == kVetShort2 || vet == kVetShort2N || vet == kVetHalf2;
}

}  // namespace

struct DeclLayout {
  D3D12_INPUT_ELEMENT_DESC elements[dpour_decl::kMaxElements]{};
  std::uint32_t count = 0;
  std::uint64_t hash = 0;
  std::uint32_t swapped_texcoords = 0;
  NormalMode normal_mode = kNormalRawFloatBits;
};

namespace {

std::unordered_map<std::uint64_t, DeclLayout> g_decl_layouts;

// --- PSO cache --------------------------------------------------------------

struct KeyHash {
  std::size_t operator()(const PsoKey& k) const noexcept {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&k);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < sizeof(PsoKey); ++i) {
      h ^= p[i];
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};
struct KeyEq {
  bool operator()(const PsoKey& a, const PsoKey& b) const noexcept {
    return std::memcmp(&a, &b, sizeof(PsoKey)) == 0;
  }
};

// A null entry means "tried and failed" - kept so a bad combination costs one
// CreateGraphicsPipelineState, not one per frame forever.
std::unordered_map<PsoKey, ComPtr<ID3D12PipelineState>, KeyHash, KeyEq> g_psos;
std::atomic<std::uint64_t> g_pso_created{0};
std::atomic<std::uint64_t> g_pso_failed{0};

// --- constant ring ----------------------------------------------------------

constexpr std::uint32_t kRingSlices = 3;  // one per frame in flight
constexpr std::uint32_t kCbvAlign = 256;
// One block of the growing allocator. Sized so an ordinary frame needs a single
// one; the point is that a frame needing more simply GETS more.
constexpr std::uint32_t kBlockBytes = 24u << 20;

// AN ALLOCATOR THAT GROWS, NOT A RING THAT RUNS OUT.
//
// This is UnleashedRecomp's UploadAllocator (video.cpp:465). Its entire
// out-of-space handling is three lines:
//
//     if (offset + size > UploadBuffer::SIZE) { ++index; offset = 0; }
//     if (buffers.size() <= index) buffers.resize(index + 1);
//     if (buffer.buffer == nullptr) buffer.buffer = createBuffer(...);
//
// It cannot fail. A frame that needs more memory is given another block.
//
// What stood here was a FIXED three slices of 24 MB that returned nothing when
// full, and every draw that got nothing was silently dropped. Worse, it
// latched: no constants -> no draws -> nothing published -> the code that
// advances "the GPU has finished frame N" never ran -> no slice was ever freed
// again. One busy frame and the renderer was gone for the rest of the session
// while every log line still read healthy. A fixed budget for a quantity the
// game chooses is not a tuning mistake, it is the wrong shape.
struct UploadBlock {
  ComPtr<ID3D12Resource> buffer;
  std::uint8_t* cpu = nullptr;
  std::uint64_t gpu = 0;
};
struct GrowingAllocator {
  std::vector<UploadBlock> blocks;
  std::uint32_t index = 0;
  std::uint32_t offset = 0;
  std::uint64_t frame = 0;  // the guest frame this allocator last served

  void reset() {
    index = 0;
    offset = 0;
  }
};
GrowingAllocator g_alloc[kRingSlices];
std::uint32_t g_alloc_current = 0;
// The replay's own set, rotated on submissions rather than guest frames.
GrowingAllocator g_replay_alloc[kRingSlices];
std::uint32_t g_replay_current = 0;
bool g_ring_failed = false;
std::atomic<std::uint64_t> g_ring_exhausted{0};
std::atomic<std::uint64_t> g_blocks_live{0};
ID3D12Device* g_alloc_device = nullptr;

bool EnsureRing(ID3D12Device* device) {
  if (device == nullptr) {
    return false;
  }
  g_alloc_device = device;
  return true;
}

// Hands back a block, creating it on demand. Returns nullptr only when the
// device itself refuses the allocation, which is a real failure and is logged.
UploadBlock* BlockAt(GrowingAllocator& a, std::uint32_t i) {
  if (a.blocks.size() <= i) {
    a.blocks.resize(i + 1);
  }
  UploadBlock& b = a.blocks[i];
  if (b.buffer) {
    return &b;
  }
  if (g_alloc_device == nullptr) {
    return nullptr;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kBlockBytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(g_alloc_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&b.buffer)))) {
    if (!g_ring_failed) {
      REXLOG_ERROR("[native-pipeline] constant block allocation failed ({} MB)",
                   kBlockBytes >> 20);
      g_ring_failed = true;
    }
    return nullptr;
  }
  D3D12_RANGE none{0, 0};
  void* mapped = nullptr;
  if (FAILED(b.buffer->Map(0, &none, &mapped))) {
    REXLOG_ERROR("[native-pipeline] constant block map failed");
    b.buffer.Reset();
    return nullptr;
  }
  b.cpu = static_cast<std::uint8_t*>(mapped);
  b.gpu = b.buffer->GetGPUVirtualAddress();
  const std::uint64_t n = g_blocks_live.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_INFO("[native-pipeline] constant block #{} ({} MB, {} live)", i, kBlockBytes >> 20, n);
  return &b;
}

}  // namespace

const DeclLayout* GetDeclLayout(const dpour_decl::Layout& decl) {
  if (decl.element_count == 0) {
    return nullptr;
  }

  // Key on the element list itself, not on the guest declaration pointer: the
  // guest caches declarations by value, so two pointers can describe the same
  // layout and one pointer can be reused after a streaming load.
  std::uint64_t key = 1469598103934665603ull;
  for (std::uint32_t i = 0; i < decl.element_count && i < dpour_decl::kMaxElements; ++i) {
    const dpour_decl::FullElement& e = decl.elements[i];
    const std::uint8_t bytes[5] = {e.stream, e.offset, e.type, e.usage, e.usage_index};
    for (std::uint8_t b : bytes) {
      key ^= b;
      key *= 1099511628211ull;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_decl_layouts.find(key);
    if (it != g_decl_layouts.end()) {
      return it->second.count != 0 ? &it->second : nullptr;
    }
  }

  DeclLayout built;
  built.hash = key;
  for (std::uint32_t i = 0; i < decl.element_count && i < dpour_decl::kMaxElements; ++i) {
    const dpour_decl::FullElement& e = decl.elements[i];
    const char* semantic = SemanticFor(e.usage);
    if (semantic == nullptr || e.type == kVetNone) {
      continue;
    }

    DXGI_FORMAT format;
    if (e.usage == 4 || e.usage == 5 || e.usage == 6) {  // NORMAL / TANGENT / BINORMAL
      NormalMode mode = kNormalRawFloatBits;
      format = NormalFormatFor(e.type, mode);
      // A declaration mixes normal, tangent and binormal of one kind; if it ever
      // did not, the last one would win and the others would decode wrong, so
      // say so rather than silently pick.
      if (built.normal_mode != kNormalRawFloatBits && built.normal_mode != mode) {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
          REXLOG_INFO("[native-pipeline] declaration {:#x} mixes normal encodings ({} vs {})",
                      key, static_cast<std::uint32_t>(built.normal_mode),
                      static_cast<std::uint32_t>(mode));
        }
      }
      built.normal_mode = mode;
    } else {
      format = FormatFor(e.type);
      if (e.usage == 1 && NeedsTexcoordSwap(e.type)) {  // TEXCOORD
        built.swapped_texcoords |= 1u << (e.usage_index & 31);
      }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
      static std::atomic<std::uint32_t> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("[native-pipeline] no host format for VET {} ({} index {}) - element dropped",
                    e.type, semantic, e.usage_index);
      }
      continue;
    }

    D3D12_INPUT_ELEMENT_DESC& d = built.elements[built.count++];
    d.SemanticName = semantic;  // string literal, outlives everything
    d.SemanticIndex = e.usage_index;
    d.Format = format;
    d.InputSlot = e.stream;
    d.AlignedByteOffset = e.offset;
    d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    d.InstanceDataStepRate = 0;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto inserted = g_decl_layouts.emplace(key, built);
  DeclLayout& stored = inserted.first->second;
  return stored.count != 0 ? &stored : nullptr;
}

std::uint64_t DeclHash(const DeclLayout* layout) { return layout != nullptr ? layout->hash : 0; }

std::uint32_t DeclStreamMask(const DeclLayout* layout) {
  if (layout == nullptr) {
    return 0;
  }
  std::uint32_t mask = 0;
  for (std::uint32_t i = 0; i < layout->count; ++i) {
    mask |= 1u << (layout->elements[i].InputSlot & 31);
  }
  return mask;
}

std::uint32_t DeclSwappedTexcoords(const DeclLayout* layout) {
  return layout != nullptr ? layout->swapped_texcoords : 0;
}

NormalMode DeclNormalMode(const DeclLayout* layout) {
  return layout != nullptr ? layout->normal_mode : kNormalRawFloatBits;
}

namespace {

// What the guest last set per sampler slot, in D3D9 enum values.
struct GuestSampler {
  std::atomic<std::uint32_t> address_u{1};  // D3DTADDRESS_WRAP
  std::atomic<std::uint32_t> address_v{1};
  std::atomic<std::uint32_t> min_filter{2};  // D3DTEXF_LINEAR
};
GuestSampler g_guest_samplers[16];

// D3DTEXTUREADDRESS -> our 0 wrap / 1 clamp / 2 mirror.
std::uint32_t MapAddress(std::uint32_t d3d) {
  switch (d3d) {
    case 1: return 0;  // WRAP
    case 2: return 2;  // MIRROR
    case 3: return 1;  // CLAMP
    // BORDER and MIRRORONCE have no equivalent in the small fixed table; clamp
    // is the closest thing that cannot smear the opposite edge across a mesh.
    default: return 1;
  }
}

// D3DTEXTUREFILTERTYPE -> our 0 point / 1 linear / 2 anisotropic.
std::uint32_t MapFilter(std::uint32_t d3d) {
  switch (d3d) {
    case 0:  // NONE
    case 1: return 0;  // POINT
    case 3: return 2;  // ANISOTROPIC
    default: return 1;  // LINEAR
  }
}

}  // namespace

void SetGuestSamplerState(std::uint32_t sampler, std::uint32_t state, std::uint32_t value) {
  if (sampler >= 16u) {
    return;
  }
  switch (state) {
    case 1: g_guest_samplers[sampler].address_u.store(value, std::memory_order_relaxed); break;
    case 2: g_guest_samplers[sampler].address_v.store(value, std::memory_order_relaxed); break;
    // MAGFILTER (5) is set right before MINFILTER (6) and the two agree in
    // practice; the minification filter is the one that decides whether the
    // sampler is point, linear or anisotropic.
    case 6: g_guest_samplers[sampler].min_filter.store(value, std::memory_order_relaxed); break;
    default: break;
  }
}

void FillSamplerIndices(std::uint32_t* out, std::uint32_t count) {
  if (out == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t s = (i < 16u) ? i : 0u;
    out[i] = SamplerIndex(MapFilter(g_guest_samplers[s].min_filter.load(std::memory_order_relaxed)),
                          MapAddress(g_guest_samplers[s].address_u.load(std::memory_order_relaxed)),
                          MapAddress(g_guest_samplers[s].address_v.load(std::memory_order_relaxed)));
  }
}

std::uint32_t SamplerIndex(std::uint32_t filter, std::uint32_t address_u,
                           std::uint32_t address_v) {
  if (filter >= kFilterCount) {
    filter = kFilterCount - 1;
  }
  if (address_u >= kAddressCount) {
    address_u = 0;
  }
  if (address_v >= kAddressCount) {
    address_v = 0;
  }
  return (filter * kAddressCount + address_u) * kAddressCount + address_v;
}

bool FlatShadingEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("DPOUR_NR_DRAW_FLAT");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return on;
}

bool Init(ID3D12Device* device) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_root_sig && g_sampler_heap) {
    return true;
  }
  if (g_init_failed || device == nullptr) {
    return false;
  }
  g_init_failed = true;
  if (!CreateRootSignature(device) || !CreateSamplerHeap(device)) {
    g_root_sig.Reset();
    g_sampler_heap.Reset();
    return false;
  }
  g_init_failed = false;
  REXLOG_INFO("[native-pipeline] root signature + {} samplers ready", kSamplerCount);
  return true;
}

ID3D12RootSignature* RootSignature() { return g_root_sig.Get(); }
ID3D12DescriptorHeap* SamplerHeap() { return g_sampler_heap.Get(); }

void BindDescriptorTables(rex::graphics::d3d12::DeferredCommandList* cmd,
                          std::uint64_t srv_heap_gpu_start) {
  if (cmd == nullptr || srv_heap_gpu_start == 0 || g_sampler_gpu_start == 0) {
    return;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE srv{};
  srv.ptr = srv_heap_gpu_start;
  D3D12_GPU_DESCRIPTOR_HANDLE samp{};
  samp.ptr = g_sampler_gpu_start;

  // Every view type indexes the same heap from the same base, so a descriptor
  // index written into SharedConstants means one thing everywhere.
  cmd->D3DSetGraphicsRootDescriptorTable(kRootTexture2D, srv);
  cmd->D3DSetGraphicsRootDescriptorTable(kRootTexture3D, srv);
  cmd->D3DSetGraphicsRootDescriptorTable(kRootTextureCube, srv);
  cmd->D3DSetGraphicsRootDescriptorTable(kRootTexture1D, srv);
  cmd->D3DSetGraphicsRootDescriptorTable(kRootSamplers, samp);
}

ID3D12PipelineState* GetPso(ID3D12Device* device, const PsoKey& key,
                            const dpour_shadercache::Shader* vs,
                            const dpour_shadercache::Shader* ps, const DeclLayout* layout) {
  if (device == nullptr || vs == nullptr || !g_root_sig) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_psos.find(key);
  if (it != g_psos.end()) {
    return it->second.Get();
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = g_root_sig.Get();
  desc.VS.pShaderBytecode = vs->dxil;
  desc.VS.BytecodeLength = vs->dxil_size;
  if (ps != nullptr) {
    desc.PS.pShaderBytecode = ps->dxil;
    desc.PS.BytecodeLength = ps->dxil_size;
  }
  // The host input assembler feeds the shader, from a layout built out of the
  // guest's own vertex declaration - the UnleashedRecomp model.
  if (layout != nullptr) {
    desc.InputLayout.pInputElementDescs = layout->elements;
    desc.InputLayout.NumElements = layout->count;
  }
  desc.SampleMask = UINT_MAX;
  desc.PrimitiveTopologyType =
      static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(key.topology);
  desc.SampleDesc.Count = 1;

  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = static_cast<D3D12_CULL_MODE>(key.cull_mode);
  desc.RasterizerState.FrontCounterClockwise = key.front_ccw != 0 ? TRUE : FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;

  desc.DepthStencilState.DepthEnable = key.depth_enable != 0;
  desc.DepthStencilState.DepthWriteMask =
      key.depth_write != 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(key.depth_func);
  desc.DSVFormat = static_cast<DXGI_FORMAT>(key.dsv_format);

  D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
  rt.BlendEnable = key.blend_enable != 0;
  rt.SrcBlend = static_cast<D3D12_BLEND>(key.src_blend);
  rt.DestBlend = static_cast<D3D12_BLEND>(key.dst_blend);
  rt.BlendOp = static_cast<D3D12_BLEND_OP>(key.blend_op);
  rt.SrcBlendAlpha = key.src_blend_alpha != 0 ? static_cast<D3D12_BLEND>(key.src_blend_alpha)
                                              : D3D12_BLEND_ONE;
  rt.DestBlendAlpha = key.dst_blend_alpha != 0 ? static_cast<D3D12_BLEND>(key.dst_blend_alpha)
                                               : D3D12_BLEND_ZERO;
  rt.BlendOpAlpha = key.blend_op_alpha != 0 ? static_cast<D3D12_BLEND_OP>(key.blend_op_alpha)
                                            : D3D12_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = key.color_write_mask;

  desc.NumRenderTargets = key.rtv_format != 0 ? 1 : 0;
  desc.RTVFormats[0] = static_cast<DXGI_FORMAT>(key.rtv_format);

  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    // Cached as a null entry on purpose.
    g_psos.emplace(key, nullptr);
    const std::uint64_t n = g_pso_failed.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      REXLOG_INFO("[native-pipeline] PSO creation failed hr={:#010x} vs={:#x} ps={:#x} rt={} ds={}",
                  static_cast<std::uint32_t>(hr), key.vs_hash, key.ps_hash, key.rtv_format,
                  key.dsv_format);
    }
    return nullptr;
  }
  g_pso_created.fetch_add(1, std::memory_order_relaxed);
  auto inserted = g_psos.emplace(key, std::move(pso));
  return inserted.first->second.Get();
}

void BeginFrame(ID3D12Device* device, std::uint64_t frame, std::uint64_t completed) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!EnsureRing(device)) {
    return;
  }
  const std::uint32_t next = static_cast<std::uint32_t>(frame % kRingSlices);
  // Only reuse an allocator once the GPU is past the frame that last wrote it -
  // the reference's rule, where reset() happens after waitForCommandFence.
  // Staying on the current one is now merely wasteful rather than fatal: it
  // grows instead of running dry.
  if (g_alloc[next].frame != 0 && g_alloc[next].frame > completed) {
    return;
  }
  g_alloc_current = next;
  g_alloc[next].reset();
  g_alloc[next].frame = frame;
}

void BeginReplay(ID3D12Device* device, std::uint64_t submission, std::uint64_t completed) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!EnsureRing(device)) {
    return;
  }
  const std::uint32_t next = static_cast<std::uint32_t>(submission % kRingSlices);
  // Reuse only once the GPU is past the submission that last wrote it.
  if (g_replay_alloc[next].frame != 0 && g_replay_alloc[next].frame > completed) {
    return;
  }
  g_replay_current = next;
  g_replay_alloc[next].reset();
  g_replay_alloc[next].frame = submission;
}

ConstantAlloc AllocReplayConstants(std::uint32_t bytes) {
  ConstantAlloc out{nullptr, 0};
  if (bytes == 0) {
    return out;
  }
  const std::uint32_t aligned = (bytes + kCbvAlign - 1) & ~(kCbvAlign - 1);
  std::lock_guard<std::mutex> lock(g_mutex);
  constexpr std::uint32_t kTailGuard = 2 * 256 * 16;
  if (aligned + kTailGuard > kBlockBytes) {
    g_ring_exhausted.fetch_add(1, std::memory_order_relaxed);
    return out;
  }
  GrowingAllocator& a = g_replay_alloc[g_replay_current];
  if (a.offset + aligned + kTailGuard > kBlockBytes) {
    ++a.index;
    a.offset = 0;
  }
  UploadBlock* b = BlockAt(a, a.index);
  if (b == nullptr) {
    g_ring_exhausted.fetch_add(1, std::memory_order_relaxed);
    return out;
  }
  out.cpu = b->cpu + a.offset;
  out.gpu = b->gpu + a.offset;
  a.offset += aligned;
  return out;
}

ConstantAlloc AllocConstants(std::uint32_t bytes) {
  ConstantAlloc out{nullptr, 0};
  if (bytes == 0) {
    return out;
  }
  const std::uint32_t aligned = (bytes + kCbvAlign - 1) & ~(kCbvAlign - 1);

  std::lock_guard<std::mutex> lock(g_mutex);
  // A ROOT CBV carries no size, so a shader that indexes g_VertexConstants[255]
  // reads 4 KB from the address it is given whatever we allocated. Inside the
  // ring that is harmless - it reads the neighbouring draw's constants - but off
  // the END of the ring resource it is a GPU page fault, which shows up as a
  // device removal rather than as anything obviously wrong. Reserving a full
  // bank's worth of tail keeps every such read inside the allocation.
  constexpr std::uint32_t kTailGuard = 2 * 256 * 16;
  if (aligned + kTailGuard > kBlockBytes) {
    g_ring_exhausted.fetch_add(1, std::memory_order_relaxed);
    return out;  // one allocation larger than a whole block: a real failure
  }
  GrowingAllocator& a = g_alloc[g_alloc_current];
  if (a.offset + aligned + kTailGuard > kBlockBytes) {
    ++a.index;  // the reference's entire out-of-space handling
    a.offset = 0;
  }
  UploadBlock* b = BlockAt(a, a.index);
  if (b == nullptr) {
    g_ring_exhausted.fetch_add(1, std::memory_order_relaxed);
    return out;
  }
  out.cpu = b->cpu + a.offset;
  out.gpu = b->gpu + a.offset;
  a.offset += aligned;
  return out;
}

bool DryRunEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR_SHADERS_DRYRUN");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

namespace {
std::atomic<std::uint64_t> g_dry_ok{0};
std::atomic<std::uint64_t> g_dry_no_layout{0};
std::atomic<std::uint64_t> g_dry_no_pso{0};
std::atomic<std::uint64_t> g_dry_incomplete{0};
// Counted rather than returned silently: an earlier version just gave up when
// the device was not reachable, so a whole verification run reported "0
// pipelines built" with nothing to say why.
std::atomic<std::uint64_t> g_dry_no_device{0};
}  // namespace

void DryRunDraw(const dpour_decl::Layout& decl, const dpour_shadercache::Shader* vs,
                const dpour_shadercache::Shader* ps, const std::uint16_t* strides,
                std::uint32_t stride_count) {
  if (vs == nullptr || ps == nullptr) {
    g_dry_incomplete.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The texture module owns the bindless heap, and through it the device.
  ID3D12DescriptorHeap* heap = dpour_tex::Heap();
  if (heap == nullptr) {
    g_dry_no_device.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(heap->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    g_dry_no_device.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Init(device)) {
    g_dry_no_device.fetch_add(1, std::memory_order_relaxed);
    device->Release();
    return;
  }

  const DeclLayout* layout = GetDeclLayout(decl);
  if (layout == nullptr) {
    g_dry_no_layout.fetch_add(1, std::memory_order_relaxed);
    device->Release();
    return;
  }

  PsoKey key{};
  key.vs_hash = vs->ucode_hash;
  key.ps_hash = ps->ucode_hash;
  key.decl_hash = DeclHash(layout);
  // Plain opaque state: this only has to be a state D3D12 will accept, since
  // nothing is drawn. Real per-draw state comes with the draw path.
  key.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  key.dsv_format = DXGI_FORMAT_D32_FLOAT;
  key.depth_enable = 1;
  key.depth_write = 1;
  key.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  key.cull_mode = D3D12_CULL_MODE_BACK;
  key.blend_enable = 0;
  key.src_blend = D3D12_BLEND_ONE;
  key.dst_blend = D3D12_BLEND_ZERO;
  key.blend_op = D3D12_BLEND_OP_ADD;
  key.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  key.color_write_mask = D3D12_COLOR_WRITE_ENABLE_ALL;
  for (std::uint32_t i = 0; i < kMaxStreams && i < stride_count; ++i) {
    key.strides[i] = strides[i];
  }

  const bool ok = GetPso(device, key, vs, ps, layout) != nullptr;
  (ok ? g_dry_ok : g_dry_no_pso).fetch_add(1, std::memory_order_relaxed);
  device->Release();
}

void DryRunReport() {
  REXLOG_INFO("[native-dryrun] {} pipelines built, {} PSO failures, {} declarations unusable, "
              "{} draws without both shaders, {} with no device yet",
              g_dry_ok.load(std::memory_order_relaxed), g_dry_no_pso.load(std::memory_order_relaxed),
              g_dry_no_layout.load(std::memory_order_relaxed),
              g_dry_incomplete.load(std::memory_order_relaxed),
              g_dry_no_device.load(std::memory_order_relaxed));
}

void LogStats() {
  const std::uint64_t exhausted = g_ring_exhausted.load(std::memory_order_relaxed);
  std::uint32_t used_kb = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const GrowingAllocator& a = g_alloc[g_alloc_current];
    used_kb = (a.index * (kBlockBytes / 1024)) + a.offset / 1024;
  }
  REXLOG_INFO("[native-pipeline] {} PSOs, {} failed, {} KB of constants, {} blocks live{}",
              g_pso_created.load(std::memory_order_relaxed),
              g_pso_failed.load(std::memory_order_relaxed), used_kb,
              g_blocks_live.load(std::memory_order_relaxed),
              exhausted != 0 ? ", ALLOCATION REFUSED BY DEVICE" : "");
}

}  // namespace dpour_pipeline
// === END DPOUR MIGRATION 2026-07-25 ===
