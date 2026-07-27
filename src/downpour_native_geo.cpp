// downpour - Native Render: native scene renderer (Phase 3/4)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 3 (real game geometry) ===
// === DPOUR MIGRATION 2026-07-25: Phase 4 - UVs, normals, textures, depth ===
//
// THE ARCHITECTURE (the Unleashed/Marathon model, not a per-frame hack):
//   * The FIRST time we see a mesh (a unique guest vertex-buffer / index-buffer
//     / section triple) we read it out of guest memory, byte-swap it, decode its
//     packed normals and half-float UVs, and upload it into REAL D3D12 buffers.
//     That happens ONCE per mesh, ever.
//   * The first time we see a texture we decode it out of guest memory (Xenos
//     tiling + endian + format) into a real D3D12 texture. Once per texture.
//   * Every frame after that, a draw costs: root constants + descriptor table +
//     bind + DrawIndexedInstanced. No guest reads, no byte-swapping, no CPU
//     transforms in the hot path.
//   * Rendering goes into OUR OWN colour + depth targets on OUR OWN command
//     list, submitted on the runtime's direct queue ahead of the presenter's
//     list, then composited over the frame. With DPOUR_NR_ONLY=1 the composite
//     is opaque - the emulated image is gone and what you see is 100% ours.
//
// Where the data comes from (all through the UE3 RHI hook boundary, no PM4):
//   RHISetViewParameters (sub_829C94F0)      -> the game's view-projection
//   SetVertexShaderConstantF (sub_82D1D790)  -> constant file, incl. LocalToWorld
//   RHISetStreamSource (sub_829C8AA8)        -> bound vertex buffers + strides
//   RHISetSamplerState (sub_829C8E68)        -> bound textures
//   DrawIndexedVertices (sub_82D1F0B0)       -> IB + base vertex + start + count
//   FXeGPUResource::BaseAddress (+12)        -> the actual payload, big-endian
//
// Safety: guest reads are mapping-checked (VirtualQuery, multi-region aware),
// positions validated finite, and uploads are bounded by a per-frame time
// budget and a total VRAM cap, so a heavy scene can never tank the frame rate.
//
// Gated by env DPOUR_NR_GEO (default OFF).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_geo.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <rex/logging.h>

#include "downpour_native_decl.h"
#include "downpour_native_tex.h"

using Microsoft::WRL::ComPtr;

namespace dpour_geo {
namespace {

// ---------------------------------------------------------------- tuning ---
constexpr std::uint32_t kMaxIndicesPerDraw = 300000;
constexpr std::uint32_t kMaxDrawItemsPerFrame = 8192;
constexpr std::size_t kMaxCachedBytes = 512u * 1024u * 1024u;
constexpr std::size_t kMaxCachedMeshes = 32768;
constexpr float kMaxWorldCoord = 1.0e6f;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kPassColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kPassDepthFormat = DXGI_FORMAT_D32_FLOAT;

// Native vertex: object-space position, unpacked normal, UV0.
struct NVertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};
static_assert(sizeof(NVertex) == 32, "vertex layout");

// ------------------------------------------------------ guest memory I/O ---
inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

bool SpanReadable(const void* p, std::size_t n) {
  static thread_local const std::uint8_t* cached_begin = nullptr;
  static thread_local const std::uint8_t* cached_end = nullptr;
  const auto* b = static_cast<const std::uint8_t*>(p);
  const std::uint8_t* e = b + n;
  if (cached_begin != nullptr && b >= cached_begin && e <= cached_end) {
    return true;
  }
  const std::uint8_t* cur = b;
  const std::uint8_t* covered_from = nullptr;
  for (int guard = 0; guard < 64 && cur < e; ++guard) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) {
      return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
      return false;
    }
    const auto* rb = static_cast<const std::uint8_t*>(mbi.BaseAddress);
    const std::uint8_t* re = rb + mbi.RegionSize;
    if (re <= cur) {
      return false;
    }
    if (covered_from == nullptr) {
      covered_from = rb;
    }
    cur = re;
  }
  if (cur < e) {
    return false;
  }
  cached_begin = covered_from;
  cached_end = cur;
  return true;
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) {
  std::uint32_t raw;
  std::memcpy(&raw, p, 4);
  return _byteswap_ulong(raw);
}
inline std::uint16_t LoadBE16(const std::uint8_t* p) {
  std::uint16_t raw;
  std::memcpy(&raw, p, 2);
  return _byteswap_ushort(raw);
}
inline float LoadBEFloat(const std::uint8_t* p) {
  const std::uint32_t v = LoadBE32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline bool CoordSane(float v) {
  return std::isfinite(v) && v > -kMaxWorldCoord && v < kMaxWorldCoord;
}

// IEEE half -> float (UE3 stores FVector2DHalf UVs for X360 static meshes).
float HalfToFloat(std::uint16_t h) {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1Fu;
  std::uint32_t man = h & 0x3FFu;
  if (exp == 0) {
    if (man == 0) {
      std::uint32_t bits = sign;
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }
    // Subnormal: normalise.
    exp = 1;
    while ((man & 0x400u) == 0) {
      man <<= 1;
      --exp;
    }
    man &= 0x3FFu;
    const std::uint32_t bits = sign | ((exp + 112u) << 23) | (man << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  if (exp == 31) {
    const std::uint32_t bits = sign | 0x7F800000u | (man << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  const std::uint32_t bits = sign | ((exp + 112u) << 23) | (man << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// FPackedNormal as stored big-endian: bytes are W, Z, Y, X.
inline void UnpackNormalBE(const std::uint8_t* p, float& x, float& y, float& z) {
  x = static_cast<float>(p[3]) / 127.5f - 1.0f;
  y = static_cast<float>(p[2]) / 127.5f - 1.0f;
  z = static_cast<float>(p[1]) / 127.5f - 1.0f;
}

// --------------------------------------------------------- frame budget ---
double g_qpc_to_ms = 0.0;
std::int64_t g_budget_ticks = 0;
std::int64_t g_frame_upload_ticks = 0;

double BudgetMsFromEnv() {
  const char* v = std::getenv("DPOUR_NR_GEO_MS");
  if (v != nullptr && v[0] != '\0') {
    const double parsed = std::atof(v);
    if (parsed > 0.05 && parsed < 50.0) {
      return parsed;
    }
  }
  return 3.0;
}

void InitTiming() {
  LARGE_INTEGER f{};
  QueryPerformanceFrequency(&f);
  g_qpc_to_ms = 1000.0 / static_cast<double>(f.QuadPart);
  g_budget_ticks =
      static_cast<std::int64_t>(BudgetMsFromEnv() / 1000.0 * static_cast<double>(f.QuadPart));
}
inline std::int64_t Now() {
  LARGE_INTEGER t{};
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// Largest world-space extent a single captured mesh may have, in UE units
// (1 unit == 1 cm). Anything past this is a bad transform or a sky dome, both
// of which would swallow the whole frame once depth testing is on.
float MaxWorldExtentFromEnv() {
  const char* v = std::getenv("DPOUR_NR_MAXEXT");
  if (v != nullptr && v[0] != '\0') {
    const double parsed = std::atof(v);
    if (parsed > 10.0) {
      return static_cast<float>(parsed);
    }
  }
  return 60000.0f;  // 600 m
}

// 0 = no culling, 1 = back faces (default), 2 = front faces.
int CullModeFromEnv() {
  const char* v = std::getenv("DPOUR_NR_CULL");
  if (v != nullptr && v[0] != '\0') {
    const int parsed = std::atoi(v);
    if (parsed >= 0 && parsed <= 2) {
      return parsed;
    }
  }
  return 1;
}

// Bit 0 = negate U, bit 1 = negate V, bit 2 = swap U/V.
// Default 1: the decoded texture comes out mirrored along X relative to the
// mesh's texture coordinates, and negating U puts every sign, poster and label
// in the world the right way round (verified against the game's own render).
int UvModeFromEnv() {
  const char* v = std::getenv("DPOUR_NR_UV");
  if (v != nullptr && v[0] != '\0') {
    const int parsed = std::atoi(v);
    if (parsed >= 0 && parsed <= 7) {
      return parsed;
    }
  }
  return 1;
}

std::uint32_t MinIndicesFromEnv() {
  const char* v = std::getenv("DPOUR_NR_MINIDX");
  if (v != nullptr && v[0] != '\0') {
    const int parsed = std::atoi(v);
    if (parsed >= 3 && parsed < 100000) {
      return static_cast<std::uint32_t>(parsed);
    }
  }
  return 30u;
}

// --------------------------------------------------------- native meshes ---
struct CachedMesh {
  ComPtr<ID3D12Resource> vb;
  ComPtr<ID3D12Resource> ib;
  D3D12_VERTEX_BUFFER_VIEW vbv{};
  D3D12_INDEX_BUFFER_VIEW ibv{};
  std::uint32_t index_count = 0;
  bool has_uv = false;
  float bmin[3] = {0, 0, 0};  // object-space bounds, for transform sanity checks
  float bmax[3] = {0, 0, 0};
};

struct DrawItem {
  const CachedMesh* mesh;
  std::uint32_t tex_slot;
  std::uint32_t view;  // which of the frame's views this draw belongs to
  float l2w[16];
};

std::mutex g_mutex;
std::unordered_map<std::uint64_t, CachedMesh> g_cache;
// Sections whose guest data we could not read (streamed out, transient scratch
// buffers, ...). Without this we would re-walk their guest memory EVERY frame,
// burn the whole upload budget on them and starve the sections that CAN be
// uploaded - which is exactly what capped the scene at a fraction of its
// geometry before.
std::unordered_map<std::uint64_t, std::uint32_t> g_failed;
constexpr std::uint32_t kMaxUploadRetries = 3;
std::size_t g_cached_bytes = 0;
std::vector<DrawItem> g_staging;
std::vector<DrawItem> g_published;
std::atomic<ID3D12Device*> g_device{nullptr};

// A frame contains several views: the player camera plus shadow-depth and
// reflection passes, each with its own view-projection. Rendering the frame's
// LAST matrix picked whichever view happened to finish last, which is why the
// native image sometimes showed the scene from an unrelated angle. Keep them
// all, count how many draws each one owns, and publish the busiest - that is
// the player camera by a wide margin.
constexpr std::uint32_t kMaxViews = 8;
struct ViewSlot {
  float m[16];
  std::uint32_t hash;
  std::uint32_t draws;
};
ViewSlot g_views[kMaxViews] = {};
std::uint32_t g_view_count = 0;
std::uint32_t g_view_current = 0;
float g_matrix_published[16] = {};
bool g_have_matrix = false;

// Shadowed guest vertex-shader constant file. Reserved (RHI.h): c0-c3 = view
// projection, c4 = view origin, c5 = pre-view translation, so UE3's vertex
// factories put LocalToWorld at c6.
constexpr std::uint32_t kVsConstCount = 256;
constexpr std::uint32_t kLocalToWorldReg = 6;
float g_vs_consts[kVsConstCount][4] = {};
bool g_vs_const_valid[kVsConstCount] = {};

// Per-frame dedup (UE3 redraws each mesh for depth prepass / base / shadows).
constexpr std::size_t kDedupSlots = 16384;
std::uint32_t g_dedup[kDedupSlots] = {};
bool DedupInsert(std::uint32_t key) {
  if (key == 0) {
    key = 1;
  }
  std::size_t i = key & (kDedupSlots - 1);
  for (int probe = 0; probe < 24; ++probe) {
    if (g_dedup[i] == 0) {
      g_dedup[i] = key;
      return true;
    }
    if (g_dedup[i] == key) {
      return false;
    }
    i = (i + 1) & (kDedupSlots - 1);
  }
  return true;
}

// Diagnostics
std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_draws_seen{0};
std::atomic<std::uint64_t> g_meshes_uploaded{0};
std::atomic<std::uint64_t> g_budget_stops{0};
std::atomic<std::uint64_t> g_uv_ok{0};
std::atomic<std::uint64_t> g_uv_fail{0};
std::atomic<std::uint64_t> g_oversized{0};
std::atomic<std::uint64_t> g_rej_stride{0};
std::atomic<std::uint64_t> g_rej_count{0};
std::atomic<std::uint64_t> g_rej_l2w{0};
std::atomic<std::uint64_t> g_used_fallback{0};
std::atomic<std::uint64_t> g_stride_hist[64] = {};
std::atomic<bool> g_logged_first{false};
std::atomic<bool> g_logged_matrix{false};

// ------------------------------------------------------------- rendering ---
// Scene pass: object-space position -> world -> clip, per-vertex normal from
// the mesh's packed tangent basis, base colour from the game's own texture.
constexpr char kGeoShaderSrc[] = R"HLSL(
cbuffer Xform : register(b0) {
  row_major float4x4 uWVP;
  row_major float4x4 uWorld;
  float4 uParams;   // x = has-uv flag, y = alpha-test flag, zw = unused
};
Texture2D    uTex : register(t0);
SamplerState uSmp : register(s0);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float3 wnrm : NORMAL; float2 uv : TEXCOORD0;
               float3 wpos : TEXCOORD1; };

VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos  = mul(float4(i.pos, 1.0), uWVP);        // UE3 convention: mul(v, M)
  o.wpos = mul(float4(i.pos, 1.0), uWorld).xyz;
  o.wnrm = mul(float4(i.nrm, 0.0), uWorld).xyz;
  o.uv   = i.uv;
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  float4 tex = uTex.Sample(uSmp, i.uv);
  // DXT1 alpha is 1-bit punch-through: that is how the game masks chain-link
  // fences, grates and foliage, so those draws get a real alpha test.
  clip(uParams.y > 0.5 ? tex.a - 0.5 : 1.0);
  float3 albedo = tex.rgb;
  // Fall back to a neutral surface colour when the mesh had no usable UVs.
  albedo = lerp(float3(0.60, 0.60, 0.62), albedo, uParams.x);

  float3 n = normalize(i.wnrm);
  if (dot(n, n) < 0.5) {
    n = normalize(cross(ddx(i.wpos), ddy(i.wpos)));
  }
  // Two-sided: the guest culls, we do not, so light whichever face we see.
  float3 key  = normalize(float3(0.30, 0.55, -0.78));
  float3 fill = normalize(float3(-0.55, 0.25, 0.70));
  float  d    = saturate(abs(dot(n, key))) * 0.65 + saturate(abs(dot(n, fill))) * 0.20 + 0.25;
  return float4(albedo * d, 1.0);
}

// Wireframe variant, for diagnosing capture without hiding the game image.
float4 PSWire(VSOut i) : SV_Target { return float4(0.15, 1.0, 0.35, 1.0); }
)HLSL";

// Composite: fullscreen triangle that puts our pass onto the frame.
constexpr char kCompositeShaderSrc[] = R"HLSL(
Texture2D    uTex : register(t0);
SamplerState uSmp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  VSOut o;
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
float4 PSMain(VSOut i) : SV_Target { return uTex.Sample(uSmp, i.uv); }
)HLSL";

ComPtr<ID3D12RootSignature> g_root_sig;
ComPtr<ID3D12PipelineState> g_pso;
ComPtr<ID3D12RootSignature> g_comp_root_sig;
ComPtr<ID3D12PipelineState> g_comp_pso;
bool g_pipeline_failed = false;

// Our own render pass targets + submission machinery.
constexpr std::uint32_t kPassFrames = 3;
ComPtr<ID3D12Resource> g_pass_color;
ComPtr<ID3D12Resource> g_pass_depth;
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
ComPtr<ID3D12DescriptorHeap> g_dsv_heap;
ComPtr<ID3D12DescriptorHeap> g_srv_heap;  // shader visible, for the composite
ComPtr<ID3D12CommandAllocator> g_allocators[kPassFrames];
ComPtr<ID3D12GraphicsCommandList> g_pass_list;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fence_event = nullptr;
std::uint64_t g_fence_value = 0;
std::uint64_t g_alloc_fence[kPassFrames] = {};
std::uint32_t g_pass_index = 0;
std::uint32_t g_pass_width = 0;
std::uint32_t g_pass_height = 0;
bool g_wireframe = false;

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, const void* data, std::size_t size) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> res;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&res)))) {
    return nullptr;
  }
  void* mapped = nullptr;
  const D3D12_RANGE no_read{0, 0};
  if (FAILED(res->Map(0, &no_read, &mapped)) || mapped == nullptr) {
    return nullptr;
  }
  std::memcpy(mapped, data, size);
  res->Unmap(0, nullptr);
  return res;
}

bool EnsurePipeline(ID3D12Device* device) {
  if (g_pso) {
    return true;
  }
  if (g_pipeline_failed || device == nullptr) {
    return false;
  }
  g_pipeline_failed = true;
  g_wireframe = EnvOn("DPOUR_NR_WIRE");

  if (!dpour_tex::EnsureHeap(device)) {
    REXLOG_ERROR("[native-geo] texture heap unavailable");
    return false;
  }

  ComPtr<ID3DBlob> vs, ps, err;
  HRESULT hr = D3DCompile(kGeoShaderSrc, sizeof(kGeoShaderSrc) - 1, "dpour_geo", nullptr, nullptr,
                          "VSMain", "vs_5_1", 0, 0, &vs, &err);
  if (FAILED(hr)) {
    REXLOG_ERROR("[native-geo] VS compile hr={:#010x} {}", static_cast<std::uint32_t>(hr),
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  err.Reset();
  hr = D3DCompile(kGeoShaderSrc, sizeof(kGeoShaderSrc) - 1, "dpour_geo", nullptr, nullptr,
                  g_wireframe ? "PSWire" : "PSMain", "ps_5_1", 0, 0, &ps, &err);
  if (FAILED(hr)) {
    REXLOG_ERROR("[native-geo] PS compile hr={:#010x} {}", static_cast<std::uint32_t>(hr),
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }

  // Root signature: transform constants + one texture table + a static sampler.
  D3D12_DESCRIPTOR_RANGE tex_range{};
  tex_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  tex_range.NumDescriptors = 1;
  tex_range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER root_params[2]{};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params[0].Constants.ShaderRegister = 0;
  root_params[0].Constants.Num32BitValues = 36;  // WVP + World + params
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[1].DescriptorTable.NumDescriptorRanges = 1;
  root_params[1].DescriptorTable.pDescriptorRanges = &tex_range;
  root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC geo_samp{};
  geo_samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  geo_samp.AddressU = geo_samp.AddressV = geo_samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  geo_samp.MaxLOD = D3D12_FLOAT32_MAX;
  geo_samp.ShaderRegister = 0;
  geo_samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.NumParameters = 2;
  rs_desc.pParameters = root_params;
  rs_desc.NumStaticSamplers = 1;
  rs_desc.pStaticSamplers = &geo_samp;
  rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rs_blob, rs_err;
  hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
  if (FAILED(hr)) {
    REXLOG_ERROR("[native-geo] SerializeRootSignature hr={:#010x} {}",
                 static_cast<std::uint32_t>(hr),
                 rs_err ? static_cast<const char*>(rs_err->GetBufferPointer()) : "");
    return false;
  }
  if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_root_sig)))) {
    REXLOG_ERROR("[native-geo] CreateRootSignature failed");
    return false;
  }

  const D3D12_INPUT_ELEMENT_DESC input_elems[3] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
       0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = g_root_sig.Get();
  pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso_desc.InputLayout = {input_elems, 3};
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.RasterizerState.FillMode =
      g_wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  // UE3 on X360 uses clockwise front faces, matching D3D12's default
  // (FrontCounterClockwise = FALSE). Culling halves the pixel work and, more
  // importantly, stops us drawing the inside of enclosing geometry.
  const int cull = CullModeFromEnv();
  pso_desc.RasterizerState.CullMode = (cull == 0)   ? D3D12_CULL_MODE_NONE
                                      : (cull == 1) ? D3D12_CULL_MODE_BACK
                                                    : D3D12_CULL_MODE_FRONT;
  pso_desc.RasterizerState.DepthClipEnable = TRUE;
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  // Real depth testing - this is what makes it a render, not an overlay.
  pso_desc.DepthStencilState.DepthEnable = TRUE;
  pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso_desc.DepthStencilState.StencilEnable = FALSE;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = kPassColorFormat;
  pso_desc.DSVFormat = kPassDepthFormat;
  pso_desc.SampleDesc.Count = 1;

  if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&g_pso)))) {
    REXLOG_ERROR("[native-geo] CreateGraphicsPipelineState failed");
    return false;
  }

  // ---- composite pipeline (fullscreen triangle over the backbuffer) ----
  ComPtr<ID3DBlob> cvs, cps;
  err.Reset();
  if (FAILED(D3DCompile(kCompositeShaderSrc, sizeof(kCompositeShaderSrc) - 1, "dpour_comp", nullptr,
                        nullptr, "VSMain", "vs_5_0", 0, 0, &cvs, &err))) {
    REXLOG_ERROR("[native-geo] composite VS compile failed {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  err.Reset();
  if (FAILED(D3DCompile(kCompositeShaderSrc, sizeof(kCompositeShaderSrc) - 1, "dpour_comp", nullptr,
                        nullptr, "PSMain", "ps_5_0", 0, 0, &cps, &err))) {
    REXLOG_ERROR("[native-geo] composite PS compile failed {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }

  D3D12_DESCRIPTOR_RANGE srv_range{};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 1;
  srv_range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER comp_param{};
  comp_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  comp_param.DescriptorTable.NumDescriptorRanges = 1;
  comp_param.DescriptorTable.pDescriptorRanges = &srv_range;
  comp_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.ShaderRegister = 0;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC comp_rs{};
  comp_rs.NumParameters = 1;
  comp_rs.pParameters = &comp_param;
  comp_rs.NumStaticSamplers = 1;
  comp_rs.pStaticSamplers = &samp;

  ComPtr<ID3DBlob> comp_blob, comp_err;
  if (FAILED(D3D12SerializeRootSignature(&comp_rs, D3D_ROOT_SIGNATURE_VERSION_1, &comp_blob,
                                         &comp_err)) ||
      FAILED(device->CreateRootSignature(0, comp_blob->GetBufferPointer(),
                                         comp_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_comp_root_sig)))) {
    REXLOG_ERROR("[native-geo] composite root signature failed");
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC comp_pso{};
  comp_pso.pRootSignature = g_comp_root_sig.Get();
  comp_pso.VS = {cvs->GetBufferPointer(), cvs->GetBufferSize()};
  comp_pso.PS = {cps->GetBufferPointer(), cps->GetBufferSize()};
  comp_pso.SampleMask = UINT_MAX;
  comp_pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  comp_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  // DPOUR_NR_ONLY: opaque - our frame REPLACES the emulated one.
  // Otherwise: alpha blend, so the native render sits over the game image.
  if (!NativeOnly()) {
    comp_pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    comp_pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    comp_pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    comp_pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    comp_pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    comp_pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    comp_pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  comp_pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  comp_pso.DepthStencilState.DepthEnable = FALSE;
  comp_pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  comp_pso.NumRenderTargets = 1;
  comp_pso.RTVFormats[0] = kBackBufferFormat;
  comp_pso.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&comp_pso, IID_PPV_ARGS(&g_comp_pso)))) {
    REXLOG_ERROR("[native-geo] composite PSO failed");
    return false;
  }

  // ---- descriptor heaps, command machinery ----
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
  dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv_desc.NumDescriptors = 1;
  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
  srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_desc.NumDescriptors = 1;
  srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap))) ||
      FAILED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&g_dsv_heap))) ||
      FAILED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap)))) {
    REXLOG_ERROR("[native-geo] descriptor heaps failed");
    return false;
  }
  for (std::uint32_t i = 0; i < kPassFrames; ++i) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g_allocators[i])))) {
      REXLOG_ERROR("[native-geo] command allocator failed");
      return false;
    }
  }
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(),
                                       nullptr, IID_PPV_ARGS(&g_pass_list)))) {
    REXLOG_ERROR("[native-geo] command list failed");
    return false;
  }
  g_pass_list->Close();
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
    REXLOG_ERROR("[native-geo] fence failed");
    return false;
  }
  g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (g_fence_event == nullptr) {
    return false;
  }

  g_pipeline_failed = false;
  REXLOG_INFO(
      "[native-geo] NATIVE RENDERER READY - {} pass with depth, textures ON, "
      "native_only={}, skip_guest_draws={}",
      g_wireframe ? "wireframe" : "textured", NativeOnly(), SkipGuestDraws());
  return true;
}

// (Re)create the pass targets when the output size changes.
bool EnsureTargets(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
  if (g_pass_color && g_pass_width == width && g_pass_height == height) {
    return true;
  }
  if (width == 0 || height == 0) {
    return false;
  }
  // Make sure nothing is still reading the old targets.
  if (g_fence && g_fence_value > 0 && g_fence->GetCompletedValue() < g_fence_value) {
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    WaitForSingleObject(g_fence_event, 2000);
  }
  g_pass_color.Reset();
  g_pass_depth.Reset();

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kPassColorFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const D3D12_CLEAR_VALUE color_clear{kPassColorFormat, {0.0f, 0.0f, 0.0f, 0.0f}};
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             &color_clear, IID_PPV_ARGS(&g_pass_color)))) {
    REXLOG_ERROR("[native-geo] pass colour target failed");
    return false;
  }

  D3D12_RESOURCE_DESC ddesc = desc;
  ddesc.Format = kPassDepthFormat;
  ddesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depth_clear{};
  depth_clear.Format = kPassDepthFormat;
  depth_clear.DepthStencil.Depth = 1.0f;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ddesc,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
                                             IID_PPV_ARGS(&g_pass_depth)))) {
    REXLOG_ERROR("[native-geo] pass depth target failed");
    return false;
  }

  device->CreateRenderTargetView(g_pass_color.Get(), nullptr,
                                 g_rtv_heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = kPassDepthFormat;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(g_pass_depth.Get(), &dsv,
                                 g_dsv_heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = kPassColorFormat;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(g_pass_color.Get(), &srv,
                                   g_srv_heap->GetCPUDescriptorHandleForHeapStart());

  g_pass_width = width;
  g_pass_height = height;
  REXLOG_INFO("[native-geo] pass targets {}x{}", width, height);
  return true;
}

// Upload one guest mesh section into native buffers, using the layout the game
// itself declared. Called once per section.
const CachedMesh* UploadMesh(ID3D12Device* device, const std::uint8_t* base, const DrawInputs& in,
                             const dpour_decl::Layout& layout, const std::uint32_t vb_data[kMaxStreams],
                             std::uint32_t ib_data, std::uint64_t key) {
  static thread_local std::vector<std::uint16_t> idx_le;
  static thread_local std::vector<NVertex> verts;

  const std::uint8_t* ip = base + ib_data + static_cast<std::size_t>(in.start_index) * 2u;
  if (!SpanReadable(ip, static_cast<std::size_t>(in.index_count) * 2u)) {
    return nullptr;
  }
  idx_le.clear();
  idx_le.resize(in.index_count);
  std::uint32_t min_idx = 0xFFFFFFFFu;
  std::uint32_t max_idx = 0;
  for (std::uint32_t i = 0; i < in.index_count; ++i) {
    const std::uint32_t idx = LoadBE16(ip + i * 2);
    idx_le[i] = static_cast<std::uint16_t>(idx);
    min_idx = std::min(min_idx, idx);
    max_idx = std::max(max_idx, idx);
  }
  if (min_idx > max_idx) {
    return nullptr;
  }
  for (std::uint32_t i = 0; i < in.index_count; ++i) {
    idx_le[i] = static_cast<std::uint16_t>(idx_le[i] - min_idx);
  }

  const std::uint32_t first_vertex = in.base_vertex + min_idx;
  const std::uint32_t vert_count = max_idx - min_idx + 1u;
  if (vert_count == 0 || vert_count > 65536u) {
    return nullptr;
  }

  // Resolve each attribute to a base pointer + stride, or null when the stream
  // it lives in is not bound.
  struct Attr {
    const std::uint8_t* base = nullptr;
    std::uint32_t stride = 0;
    std::uint8_t type = dpour_decl::kUnused;
  };
  auto bind = [&](const dpour_decl::Element& e) {
    Attr a;
    if (!e.valid() || e.stream >= kMaxStreams) {
      return a;
    }
    const std::uint32_t stride = in.stride[e.stream];
    const std::uint32_t data = vb_data[e.stream];
    const std::uint32_t size = dpour_decl::ElementSize(e.type);
    if (stride == 0 || data == 0 || size == 0 || e.offset + size > stride) {
      return a;
    }
    const std::uint8_t* p = base + data;
    if (!SpanReadable(p, static_cast<std::size_t>(first_vertex + vert_count) * stride)) {
      return a;
    }
    a.base = p + e.offset;
    a.stride = stride;
    a.type = e.type;
    return a;
  };

  const Attr pos = bind(layout.position);
  if (pos.base == nullptr) {
    return nullptr;
  }
  const Attr nrm = bind(layout.normal);
  const Attr uv = bind(layout.texcoord);
  if (uv.base != nullptr) {
    g_uv_ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_uv_fail.fetch_add(1, std::memory_order_relaxed);
  }

  static const int uv_mode = UvModeFromEnv();
  std::uint32_t uv_bad = 0;
  verts.clear();
  verts.resize(vert_count);
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  float tmp[4];
  for (std::uint32_t v = 0; v < vert_count; ++v) {
    NVertex& out = verts[v];
    const std::size_t vi = static_cast<std::size_t>(first_vertex + v);

    dpour_decl::ReadElement(pos.base + vi * pos.stride, pos.type, tmp);
    out.px = tmp[0];
    out.py = tmp[1];
    out.pz = tmp[2];
    if (!CoordSane(out.px) || !CoordSane(out.py) || !CoordSane(out.pz)) {
      // Do NOT collapse a bad vertex to the origin: every triangle sharing it
      // then stretches into a huge spike across the screen. Drop the section.
      return nullptr;
    }
    bmin[0] = std::min(bmin[0], out.px);
    bmin[1] = std::min(bmin[1], out.py);
    bmin[2] = std::min(bmin[2], out.pz);
    bmax[0] = std::max(bmax[0], out.px);
    bmax[1] = std::max(bmax[1], out.py);
    bmax[2] = std::max(bmax[2], out.pz);

    out.nx = out.ny = out.nz = 0.0f;
    if (nrm.base != nullptr) {
      dpour_decl::ReadElement(nrm.base + vi * nrm.stride, nrm.type, tmp);
      // Packed normals arrive as unsigned 0..1 and need biasing to -1..1.
      const bool packed = (nrm.type == dpour_decl::kUByte4N || nrm.type == dpour_decl::kD3DColor);
      out.nx = packed ? tmp[0] * 2.0f - 1.0f : tmp[0];
      out.ny = packed ? tmp[1] * 2.0f - 1.0f : tmp[1];
      out.nz = packed ? tmp[2] * 2.0f - 1.0f : tmp[2];
    }

    out.u = out.v = 0.0f;
    if (uv.base != nullptr) {
      dpour_decl::ReadElement(uv.base + vi * uv.stride, uv.type, tmp);
      out.u = tmp[0];
      out.v = tmp[1];
      if (uv_mode & 4) {
        std::swap(out.u, out.v);
      }
      if (uv_mode & 1) {
        out.u = -out.u;
      }
      if (uv_mode & 2) {
        out.v = -out.v;
      }
      if (!std::isfinite(out.u) || !std::isfinite(out.v) || std::fabs(out.u) > 256.0f ||
          std::fabs(out.v) > 256.0f) {
        out.u = out.v = 0.0f;
        ++uv_bad;
      }
    }
  }

  const std::size_t vb_bytes = verts.size() * sizeof(NVertex);
  const std::size_t ib_bytes = idx_le.size() * sizeof(std::uint16_t);

  CachedMesh mesh;
  mesh.vb = CreateUploadBuffer(device, verts.data(), vb_bytes);
  mesh.ib = CreateUploadBuffer(device, idx_le.data(), ib_bytes);
  if (!mesh.vb || !mesh.ib) {
    return nullptr;
  }
  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = static_cast<UINT>(vb_bytes);
  mesh.vbv.StrideInBytes = sizeof(NVertex);
  mesh.ibv.BufferLocation = mesh.ib->GetGPUVirtualAddress();
  mesh.ibv.SizeInBytes = static_cast<UINT>(ib_bytes);
  mesh.ibv.Format = DXGI_FORMAT_R16_UINT;
  mesh.index_count = in.index_count;
  // If a large share of the texture coordinates decoded to nonsense, the layout
  // guess was wrong for this factory. Showing a neutral surface is far better
  // than smearing a texture across it with garbage coordinates.
  mesh.has_uv = (uv.base != nullptr) && (uv_bad * 4u < vert_count);
  std::memcpy(mesh.bmin, bmin, sizeof(bmin));
  std::memcpy(mesh.bmax, bmax, sizeof(bmax));

  std::lock_guard<std::mutex> lock(g_mutex);
  g_cached_bytes += vb_bytes + ib_bytes;
  auto [it, inserted] = g_cache.emplace(key, std::move(mesh));
  g_meshes_uploaded.fetch_add(1, std::memory_order_relaxed);
  return &it->second;
}

// LocalToWorld from the shadowed constants, if that slot holds a sane affine
// FMatrix (UE3 is row-major with translation in row 3).
bool GetLocalToWorld(float out[16]) {
  for (std::uint32_t r = 0; r < 4; ++r) {
    if (!g_vs_const_valid[kLocalToWorldReg + r]) {
      return false;
    }
    for (int c = 0; c < 4; ++c) {
      const float v = g_vs_consts[kLocalToWorldReg + r][c];
      if (!std::isfinite(v) || v < -kMaxWorldCoord || v > kMaxWorldCoord) {
        return false;
      }
      out[r * 4 + c] = v;
    }
  }
  if (std::fabs(out[15] - 1.0f) > 1.0e-3f || std::fabs(out[3]) > 1.0e-3f ||
      std::fabs(out[7]) > 1.0e-3f || std::fabs(out[11]) > 1.0e-3f) {
    return false;
  }
  return true;
}

void MatMul4(const float a[16], const float b[16], float out[16]) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                       a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    }
  }
}

}  // namespace

bool Enabled() {
  static const bool enabled = [] {
    const bool e = EnvOn("DPOUR_NR_GEO");
    if (e) {
      InitTiming();
    }
    return e;
  }();
  return enabled;
}

bool NativeOnly() {
  static const bool v = EnvOn("DPOUR_NR_ONLY");
  return v;
}

bool SkipGuestDraws() {
  static const bool v = EnvOn("DPOUR_NR_SKIPGUEST");
  return v;
}

void SetViewProj(const std::uint8_t* base, std::uint32_t matrix_guest_addr) {
  if (!Enabled() || base == nullptr || !GuestAddrPlausible(matrix_guest_addr)) {
    return;
  }
  const std::uint8_t* p = base + matrix_guest_addr;
  if (!SpanReadable(p, 64)) {
    return;
  }
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadBEFloat(p + i * 4);
    if (!std::isfinite(m[i])) {
      return;
    }
  }
  std::uint32_t hash = 2166136261u;
  for (int i = 0; i < 16; ++i) {
    std::uint32_t bits;
    std::memcpy(&bits, &m[i], 4);
    hash = (hash ^ bits) * 16777619u;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  for (std::uint32_t i = 0; i < g_view_count; ++i) {
    if (g_views[i].hash == hash) {
      g_view_current = i;
      return;
    }
  }
  if (g_view_count < kMaxViews) {
    g_view_current = g_view_count++;
  } else {
    // Full: replace the least used slot rather than dropping the new view.
    std::uint32_t worst = 0;
    for (std::uint32_t i = 1; i < kMaxViews; ++i) {
      if (g_views[i].draws < g_views[worst].draws) {
        worst = i;
      }
    }
    g_view_current = worst;
  }
  std::memcpy(g_views[g_view_current].m, m, sizeof(m));
  g_views[g_view_current].hash = hash;
  g_views[g_view_current].draws = 0;
  if (!g_logged_matrix.exchange(true)) {
    REXLOG_INFO("[native-geo] view-projection matrix captured");
  }
}

void SetVSConstants(const std::uint8_t* base, std::uint32_t start_reg,
                    std::uint32_t data_guest_addr, std::uint32_t vec4_count) {
  if (!Enabled() || base == nullptr || vec4_count == 0 || start_reg >= kVsConstCount ||
      !GuestAddrPlausible(data_guest_addr)) {
    return;
  }
  const std::uint32_t count =
      (start_reg + vec4_count > kVsConstCount) ? (kVsConstCount - start_reg) : vec4_count;
  const std::uint8_t* p = base + data_guest_addr;
  if (!SpanReadable(p, static_cast<std::size_t>(count) * 16)) {
    return;
  }
  for (std::uint32_t r = 0; r < count; ++r) {
    for (int c = 0; c < 4; ++c) {
      g_vs_consts[start_reg + r][c] = LoadBEFloat(p + (r * 4 + c) * 4);
    }
    g_vs_const_valid[start_reg + r] = true;
  }
}

void BeginFrame() {
  if (!Enabled()) {
    return;
  }
  std::memset(g_dedup, 0, sizeof(g_dedup));
  g_frame_upload_ticks = 0;
  dpour_tex::BeginFrame();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_staging.clear();
  for (std::uint32_t i = 0; i < g_view_count; ++i) {
    g_views[i].draws = 0;
  }
}

void EndFrame() {
  if (!Enabled()) {
    return;
  }
  const std::uint64_t frame = g_frames.fetch_add(1) + 1;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if ((frame % 300) == 0) {
      REXLOG_INFO(
          "[native-geo] frame {}: draws_seen={} meshes={} ({} MB) uploads={} budget_stops={} "
          "uv_ok={} uv_fail={} oversized={} items={}",
          frame, g_draws_seen.load(std::memory_order_relaxed), g_cache.size(),
          g_cached_bytes / (1024 * 1024), g_meshes_uploaded.load(std::memory_order_relaxed),
          g_budget_stops.load(std::memory_order_relaxed), g_uv_ok.load(std::memory_order_relaxed),
          g_uv_fail.load(std::memory_order_relaxed), g_oversized.load(std::memory_order_relaxed),
          g_staging.size());
    }
    // Publish only the busiest view's draws, with that view's matrix: that is
    // the player camera, and it keeps shadow-pass copies of the same meshes out
    // of the frame.
    std::uint32_t best_view = 0;
    for (std::uint32_t i = 1; i < g_view_count; ++i) {
      if (g_views[i].draws > g_views[best_view].draws) {
        best_view = i;
      }
    }
    // Publish EVERY frame, including empty ones. Menus, loading screens and
    // movies contain no world geometry, and keeping the previous frame's list
    // alive there would leave a stale world on screen - or, with the opaque
    // composite, blank the whole UI to black.
    g_published.clear();
    if (!g_staging.empty() && g_views[best_view].draws > 0) {
      g_published.reserve(g_staging.size());
      for (const DrawItem& item : g_staging) {
        if (item.view == best_view) {
          g_published.push_back(item);
        }
      }
      std::memcpy(g_matrix_published, g_views[best_view].m, sizeof(g_matrix_published));
      g_have_matrix = true;
    }
    g_staging.clear();
  }
  if ((frame % 300) == 0) {
    dpour_tex::LogStats();
    char line[512];
    int n = std::snprintf(line, sizeof(line), "stride0 histogram:");
    for (int s = 0; s < 64 && n > 0 && n < static_cast<int>(sizeof(line)) - 24; ++s) {
      const std::uint64_t c = g_stride_hist[s].load(std::memory_order_relaxed);
      if (c != 0) {
        n += std::snprintf(line + n, sizeof(line) - n, " %d=%llu", s,
                           static_cast<unsigned long long>(c));
      }
    }
    REXLOG_INFO("[native-geo] rejects: layout={} count={} l2w={} oversized={} fallback={} | {}",
                g_rej_stride.load(std::memory_order_relaxed),
                g_rej_count.load(std::memory_order_relaxed),
                g_rej_l2w.load(std::memory_order_relaxed),
                g_oversized.load(std::memory_order_relaxed),
                g_used_fallback.load(std::memory_order_relaxed), line);
  }
}

bool CaptureDraw(const std::uint8_t* base, const DrawInputs& in) {
  if (!Enabled() || base == nullptr) {
    return false;
  }
  g_draws_seen.fetch_add(1, std::memory_order_relaxed);
  if (in.stride[0] < 64) {
    g_stride_hist[in.stride[0]].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_stride_hist[0].fetch_add(1, std::memory_order_relaxed);
  }

  static const std::uint32_t min_indices = MinIndicesFromEnv();
  if (in.index_count < min_indices || in.index_count > kMaxIndicesPerDraw ||
      !GuestAddrPlausible(in.ib_guest) || !GuestAddrPlausible(in.vb_guest[0])) {
    g_rej_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ID3D12Device* device = g_device.load(std::memory_order_relaxed);
  if (device == nullptr) {
    return false;  // renderer not up yet
  }

  // The game's own vertex declaration tells us where position, normal and UV
  // live - for EVERY vertex factory, not just the one with a separate float3
  // position stream.
  const dpour_decl::Layout& parsed = dpour_decl::Resolve(base, in.decl_guest);
  dpour_decl::Layout layout = parsed;
  if (!layout.valid) {
    // The declaration could not be read. Fall back to the layout that UE3 uses
    // for static meshes on this platform, which is the one that has been
    // rendering the world all along: a dedicated float3 position stream plus a
    // tangents+UV stream. Never regress to drawing nothing.
    layout = dpour_decl::Layout{};
    layout.position.stream = 0;
    layout.position.offset = 0;
    layout.position.type = dpour_decl::kFloat3;
    if (in.stride[0] == 12 && in.stride[1] >= 12) {
      // Two-stream static mesh: FPositionVertexBuffer + FStaticMeshVertexBuffer
      // (TangentX, TangentZ, then FVector2DHalf texture coordinates).
      layout.normal.stream = 1;
      layout.normal.offset = 4;
      layout.normal.type = dpour_decl::kUByte4N;
      layout.texcoord.stream = 1;
      layout.texcoord.offset = 8;
      layout.texcoord.type = dpour_decl::kFloat16_2;
    } else if (in.stride[0] >= 20 && in.stride[0] <= 48) {
      // Single-stream mesh: the vertex factory keeps position, the tangent
      // basis and the texture coordinates in one buffer, in declaration order
      // (position, tangents, then UVs) - the layout every UE3 vertex factory
      // that does not split the position stream produces.
      layout.normal.stream = 0;
      layout.normal.offset = 16;
      layout.normal.type = dpour_decl::kUByte4N;
      if (in.stride[0] >= 24) {
        layout.texcoord.stream = 0;
        layout.texcoord.offset = 20;
        layout.texcoord.type = dpour_decl::kFloat16_2;
      }
    } else {
      g_rej_stride.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    layout.valid = true;
    g_used_fallback.fetch_add(1, std::memory_order_relaxed);
  } else if (layout.skinned) {
    // Skinned meshes need bone matrices we do not apply yet; drawing them in
    // bind pose would look worse than leaving them to the guest renderer.
    g_rej_stride.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  float l2w[16];
  if (!GetLocalToWorld(l2w)) {
    g_rej_l2w.fetch_add(1, std::memory_order_relaxed);
    return false;  // no usable transform for this draw
  }

  const std::uint32_t tex_guest = dpour_tex::BoundBaseTexture(base);

  // Per-frame dedup, including the instance's world position.
  {
    std::uint32_t key = in.vb_guest[0] * 2654435761u ^ in.ib_guest * 2246822519u ^
                        in.index_count * 16777619u ^ in.start_index * 40503u;
    key ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(l2w[12])) * 1013904223u;
    key ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(l2w[13])) * 1664525u;
    key ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(l2w[14])) * 22695477u;
    if (!DedupInsert(key)) {
      return false;
    }
  }

  // Resolve the payload addresses (FXeGPUResource::BaseAddress at +12).
  const std::uint8_t* ib_res = base + in.ib_guest;
  if (!SpanReadable(ib_res, 16)) {
    return false;
  }
  const std::uint32_t ib_data = LoadBE32(ib_res + 12);
  if (!GuestAddrPlausible(ib_data)) {
    return false;
  }
  std::uint32_t vb_data[kMaxStreams] = {};
  for (std::uint32_t st = 0; st < kMaxStreams; ++st) {
    if (!GuestAddrPlausible(in.vb_guest[st])) {
      continue;
    }
    const std::uint8_t* res = base + in.vb_guest[st];
    if (!SpanReadable(res, 16)) {
      continue;
    }
    const std::uint32_t data = LoadBE32(res + 12);
    if (GuestAddrPlausible(data)) {
      vb_data[st] = data;
    }
  }
  if (vb_data[layout.position.stream] == 0) {
    return false;
  }

  const std::uint64_t mesh_key = (static_cast<std::uint64_t>(vb_data[0]) << 32) ^
                                 (static_cast<std::uint64_t>(ib_data) * 31u) ^
                                 (static_cast<std::uint64_t>(in.start_index) << 12) ^
                                 (static_cast<std::uint64_t>(in.base_vertex) << 3) ^
                                 in.index_count;

  const CachedMesh* mesh = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(mesh_key);
    if (it != g_cache.end()) {
      mesh = &it->second;
    } else {
      auto bad = g_failed.find(mesh_key);
      if (bad != g_failed.end() && bad->second >= kMaxUploadRetries) {
        return false;  // known-unreadable: never touch it again
      }
    }
  }

  if (mesh == nullptr) {
    if (g_frame_upload_ticks > g_budget_ticks) {
      g_budget_stops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_cached_bytes >= kMaxCachedBytes || g_cache.size() >= kMaxCachedMeshes) {
        return false;
      }
    }
    const std::int64_t t0 = Now();
    mesh = UploadMesh(device, base, in, layout, vb_data, ib_data, mesh_key);
    g_frame_upload_ticks += Now() - t0;
    if (mesh == nullptr) {
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_failed[mesh_key];
      return false;
    }
  }

  // Transform sanity: not every vertex factory keeps LocalToWorld at c6, so a
  // shadowed constant block can be misread as a matrix and blow a mesh up to
  // engulf the camera. Measure the world-space bounds and drop the absurd ones.
  {
    static const float max_extent = MaxWorldExtentFromEnv();
    float wmin[3] = {1e30f, 1e30f, 1e30f};
    float wmax[3] = {-1e30f, -1e30f, -1e30f};
    for (int corner = 0; corner < 8; ++corner) {
      const float p[3] = {(corner & 1) ? mesh->bmax[0] : mesh->bmin[0],
                          (corner & 2) ? mesh->bmax[1] : mesh->bmin[1],
                          (corner & 4) ? mesh->bmax[2] : mesh->bmin[2]};
      for (int c = 0; c < 3; ++c) {
        const float v =
            p[0] * l2w[0 * 4 + c] + p[1] * l2w[1 * 4 + c] + p[2] * l2w[2 * 4 + c] + l2w[3 * 4 + c];
        wmin[c] = std::min(wmin[c], v);
        wmax[c] = std::max(wmax[c], v);
      }
    }
    const float extent = std::max(wmax[0] - wmin[0], std::max(wmax[1] - wmin[1], wmax[2] - wmin[2]));
    if (!std::isfinite(extent) || extent > max_extent) {
      g_oversized.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }

  std::uint32_t tex_slot = dpour_tex::kWhiteSlot;
  if (mesh->has_uv && tex_guest != 0) {
    tex_slot = dpour_tex::Acquire(base, tex_guest);
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_staging.size() >= kMaxDrawItemsPerFrame) {
    return false;
  }
  DrawItem item;
  item.mesh = mesh;
  item.tex_slot = tex_slot;
  item.view = g_view_current;
  std::memcpy(item.l2w, l2w, sizeof(l2w));
  g_staging.push_back(item);
  if (g_view_current < kMaxViews) {
    ++g_views[g_view_current].draws;
  }
  return true;
}

void Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd,
            std::uint32_t width, std::uint32_t height) {
  if (!Enabled() || device == nullptr || queue == nullptr || cmd == nullptr) {
    return;
  }
  g_device.store(device, std::memory_order_relaxed);
  if (!EnsurePipeline(device) || !EnsureTargets(device, width, height)) {
    return;
  }

  static thread_local std::vector<DrawItem> items;
  float view_proj[16];
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have_matrix || g_published.empty()) {
      // Nothing of the world this frame (title, menu, loading, movie): leave the
      // frame exactly as the game drew it. Compositing here - opaquely, in
      // DPOUR_NR_ONLY - is what turned those screens black.
      return;
    }
    items = g_published;
    std::memcpy(view_proj, g_matrix_published, sizeof(view_proj));
  }

  if (!g_logged_first.exchange(true)) {
    REXLOG_INFO(
        "[native-geo] FIRST NATIVE FRAME - {} meshes drawn from native D3D12 buffers with depth "
        "and the game's own textures",
        items.size());
  }

  // ---- 1. our own render pass, on our own command list ------------------
  // Submitted on the presenter's queue while it is still recording, so the GPU
  // runs it before the composite that samples its result.
  const std::uint32_t slot = g_pass_index % kPassFrames;
  if (g_alloc_fence[slot] != 0 && g_fence->GetCompletedValue() < g_alloc_fence[slot]) {
    g_fence->SetEventOnCompletion(g_alloc_fence[slot], g_fence_event);
    WaitForSingleObject(g_fence_event, 1000);
  }
  if (FAILED(g_allocators[slot]->Reset()) ||
      FAILED(g_pass_list->Reset(g_allocators[slot].Get(), g_pso.Get()))) {
    return;
  }
  ID3D12GraphicsCommandList* pl = g_pass_list.Get();

  // Texture uploads first: they must complete before the draws that use them.
  // (legacy DPOUR_NR_GEO path: uploads are flushed by the scene path, which now
  // records onto the command processor's list; this wireframe mode predates it.)

  D3D12_RESOURCE_BARRIER to_rt{};
  to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_rt.Transition.pResource = g_pass_color.Get();
  to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  pl->ResourceBarrier(1, &to_rt);

  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
  // Alpha 0 where nothing is drawn, so the non-opaque composite leaves the rest
  // of the frame alone; opaque composite ignores alpha anyway.
  const float clear_color[4] = {0.02f, 0.02f, 0.03f, 0.0f};
  pl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
  pl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  pl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

  const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                          0.0f, 1.0f};
  const D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  pl->RSSetViewports(1, &vp);
  pl->RSSetScissorRects(1, &sc);
  pl->SetGraphicsRootSignature(g_root_sig.Get());
  pl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12DescriptorHeap* tex_heaps[] = {dpour_tex::Heap()};
  if (tex_heaps[0] != nullptr) {
    pl->SetDescriptorHeaps(1, tex_heaps);
  }

  float constants[36];
  std::uint32_t last_slot = 0xFFFFFFFFu;
  for (const DrawItem& item : items) {
    MatMul4(item.l2w, view_proj, constants);                  // WVP
    std::memcpy(constants + 16, item.l2w, sizeof(item.l2w));  // World
    constants[32] = item.mesh->has_uv ? 1.0f : 0.0f;
    constants[33] = (item.mesh->has_uv && dpour_tex::SlotIsCutout(item.tex_slot)) ? 1.0f : 0.0f;
    constants[34] = constants[35] = 0.0f;
    pl->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
    if (tex_heaps[0] != nullptr && item.tex_slot != last_slot) {
      D3D12_GPU_DESCRIPTOR_HANDLE h{};
      h.ptr = dpour_tex::GpuHandleAt(item.tex_slot);
      pl->SetGraphicsRootDescriptorTable(1, h);
      last_slot = item.tex_slot;
    }
    pl->IASetVertexBuffers(0, 1, &item.mesh->vbv);
    pl->IASetIndexBuffer(&item.mesh->ibv);
    pl->DrawIndexedInstanced(item.mesh->index_count, 1, 0, 0, 0);
  }

  D3D12_RESOURCE_BARRIER to_srv = to_rt;
  to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pl->ResourceBarrier(1, &to_srv);

  if (FAILED(pl->Close())) {
    return;
  }
  ID3D12CommandList* lists[] = {pl};
  queue->ExecuteCommandLists(1, lists);
  ++g_fence_value;
  queue->Signal(g_fence.Get(), g_fence_value);
  g_alloc_fence[slot] = g_fence_value;
  ++g_pass_index;
  dpour_tex::RetireUploads(g_fence->GetCompletedValue(), g_fence_value);

  // ---- 2. composite onto the frame (presenter's list, backbuffer bound) --
  ID3D12DescriptorHeap* heaps[] = {g_srv_heap.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(g_comp_root_sig.Get());
  cmd->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
  cmd->SetPipelineState(g_comp_pso.Get());
  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &sc);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 0, nullptr);
  cmd->IASetIndexBuffer(nullptr);
  cmd->DrawInstanced(3, 1, 0, 0);
}

}  // namespace dpour_geo
// === END DPOUR MIGRATION 2026-07-25 ===
