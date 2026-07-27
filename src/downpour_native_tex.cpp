// downpour - Native Render: guest Xenos texture -> native D3D12 texture cache
//
// === DPOUR MIGRATION 2026-07-25: native-render Phase 4 (textures) ===
//
// Pipeline for one texture, executed exactly once per unique guest texture:
//
//   FXeTexture (RHI object)
//     -> D3DBaseTexture header  (found by scanning for a valid fetch constant)
//     -> Xenos texture fetch constant: base/mip address, format, w/h, tiling,
//        swizzle, endianness, resident mip range
//     -> read guest payload level by level, un-tile (Xenos 32x32-block swizzle),
//        un-endian-swap
//     -> DXGI format (BC1/2/3/4/5 map 1:1; uncompressed formats are expanded)
//     -> D3D12 DEFAULT-heap texture WITH ITS MIP CHAIN + SRV in a shader-visible
//        heap
//
// Everything after that is a descriptor-table bind in the hot path.
//
// Revert: delete this file + downpour_native_tex.h and their CMakeLists line.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_tex.h"

#include "downpour_native_scene.h"  // RtBackedSrvSlot - RT-linked textures

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <rex/logging.h>

using Microsoft::WRL::ComPtr;

namespace dpour_tex {
namespace {

// ---------------------------------------------------------------- tuning ---
constexpr std::uint32_t kHeapSize = 16384;  // SRV descriptors
constexpr std::size_t kMaxTextureBytes = 768u * 1024u * 1024u;
constexpr std::uint32_t kMaxDecodesPerFrame = 16;  // spread warm-up over frames
constexpr std::uint32_t kMaxDim = 8192;
constexpr std::uint32_t kMaxMips = 14;
constexpr std::uint32_t kSubresourceAlign = 4096;  // xenos::kTextureSubresourceAlignmentBytes
constexpr std::uint32_t kTileWH = 32;              // xenos::kTextureTileWidthHeight

// --------------------------------------------------------- Xenos formats ---
enum : std::uint32_t {
  kFmt_8 = 2,
  kFmt_1_5_5_5 = 3,
  kFmt_5_6_5 = 4,
  kFmt_6_5_5 = 5,
  kFmt_8_8_8_8 = 6,
  kFmt_2_10_10_10 = 7,
  kFmt_8_A = 8,
  kFmt_8_B = 9,
  kFmt_8_8 = 10,
  kFmt_8_8_8_8_A = 14,
  kFmt_4_4_4_4 = 15,
  kFmt_DXT1 = 18,
  kFmt_DXT2_3 = 19,
  kFmt_DXT4_5 = 20,
  kFmt_DXN = 49,
  kFmt_8_8_8_8_AS_16_16_16_16 = 50,
  kFmt_DXT1_AS_16_16_16_16 = 51,
  kFmt_DXT2_3_AS_16_16_16_16 = 52,
  kFmt_DXT4_5_AS_16_16_16_16 = 53,
  kFmt_DXT3A = 58,
  kFmt_DXT5A = 59,
  kFmt_CTX1 = 60,
  kFmt_DXT3A_AS_1_1_1_1 = 61,
};

struct FormatInfo {
  DXGI_FORMAT dxgi;
  std::uint32_t block_w;
  std::uint32_t block_h;
  std::uint32_t block_bytes;
  bool expand;  // needs CPU expansion into `dxgi` rather than a raw block copy
};

bool DescribeFormat(std::uint32_t xfmt, FormatInfo& out) {
  switch (xfmt) {
    // Block-compressed: layouts match D3D once the endian swap is undone.
    case kFmt_DXT1:
    case kFmt_DXT1_AS_16_16_16_16:
      out = {DXGI_FORMAT_BC1_UNORM, 4, 4, 8, false};
      return true;
    case kFmt_DXT2_3:
    case kFmt_DXT2_3_AS_16_16_16_16:
      out = {DXGI_FORMAT_BC2_UNORM, 4, 4, 16, false};
      return true;
    case kFmt_DXT4_5:
    case kFmt_DXT4_5_AS_16_16_16_16:
      out = {DXGI_FORMAT_BC3_UNORM, 4, 4, 16, false};
      return true;
    case kFmt_DXT3A:
    case kFmt_DXT5A:
    case kFmt_DXT3A_AS_1_1_1_1:
      out = {DXGI_FORMAT_BC4_UNORM, 4, 4, 8, false};
      return true;
    case kFmt_DXN:
      out = {DXGI_FORMAT_BC5_UNORM, 4, 4, 16, false};
      return true;
    // Uncompressed: expanded to RGBA8 on the CPU (handles the swizzle too).
    case kFmt_8_8_8_8:
    case kFmt_8_8_8_8_A:
    case kFmt_8_8_8_8_AS_16_16_16_16:
    case kFmt_2_10_10_10:
      out = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, true};
      return true;
    case kFmt_5_6_5:
    case kFmt_1_5_5_5:
    case kFmt_4_4_4_4:
    case kFmt_6_5_5:
    case kFmt_8_8:
      out = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, true};
      return true;
    case kFmt_8:
    case kFmt_8_A:
    case kFmt_8_B:
      out = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, true};
      return true;
    default:
      return false;
  }
}

// ------------------------------------------------------ guest memory I/O ---
inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

// VirtualQuery is a syscall and this runs on the hot render thread, so keep a
// small cache of contiguous readable ranges we have already proven.
constexpr int kRangeCacheSize = 8;
thread_local const std::uint8_t* g_range_begin[kRangeCacheSize] = {};
thread_local const std::uint8_t* g_range_end[kRangeCacheSize] = {};
thread_local int g_range_next = 0;

bool SpanReadable(const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  const std::uint8_t* e = b + n;
  for (int i = 0; i < kRangeCacheSize; ++i) {
    if (g_range_begin[i] != nullptr && b >= g_range_begin[i] && e <= g_range_end[i]) {
      return true;
    }
  }
  const std::uint8_t* cur = b;
  const std::uint8_t* covered_from = nullptr;
  for (int guard = 0; guard < 512 && cur < e; ++guard) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) {
      return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
      return false;
    }
    const auto* rb = static_cast<const std::uint8_t*>(mbi.BaseAddress);
    if (covered_from == nullptr) {
      covered_from = rb;
    }
    cur = rb + mbi.RegionSize;
  }
  if (cur < e) {
    return false;
  }
  g_range_begin[g_range_next] = covered_from;
  g_range_end[g_range_next] = cur;
  g_range_next = (g_range_next + 1) % kRangeCacheSize;
  return true;
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) {
  std::uint32_t raw;
  std::memcpy(&raw, p, 4);
  return _byteswap_ulong(raw);
}

inline std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
inline std::uint32_t Log2Floor(std::uint32_t v) {
  std::uint32_t r = 0;
  while (v > 1u) {
    v >>= 1;
    ++r;
  }
  return r;
}
inline std::uint32_t Log2Ceil(std::uint32_t v) {
  std::uint32_t r = Log2Floor(v);
  return (1u << r) < v ? r + 1u : r;
}
inline std::uint32_t NextPow2(std::uint32_t v) { return 1u << Log2Ceil(std::max(1u, v)); }

// ------------------------------------------------------- fetch constant ----
struct Fetch {
  std::uint32_t d[6];

  std::uint32_t type() const { return d[0] & 0x3u; }
  std::uint32_t pitch_div32() const { return (d[0] >> 22) & 0x1FFu; }
  bool tiled() const { return ((d[0] >> 31) & 1u) != 0; }
  std::uint32_t format() const { return d[1] & 0x3Fu; }
  std::uint32_t endian() const { return (d[1] >> 6) & 0x3u; }
  std::uint32_t base_address() const { return ((d[1] >> 12) & 0xFFFFFu) << 12; }
  std::uint32_t width() const { return (d[2] & 0x1FFFu) + 1u; }
  std::uint32_t height() const { return ((d[2] >> 13) & 0x1FFFu) + 1u; }
  std::uint32_t swizzle() const { return (d[3] >> 1) & 0xFFFu; }
  std::uint32_t mip_min_level() const { return (d[4] >> 2) & 0xFu; }
  std::uint32_t mip_max_level() const { return (d[4] >> 6) & 0xFu; }
  std::uint32_t dimension() const { return (d[5] >> 9) & 0x3u; }
  bool packed_mips() const { return ((d[5] >> 11) & 1u) != 0; }
  std::uint32_t mip_address() const { return ((d[5] >> 12) & 0xFFFFFu) << 12; }
};

bool LooksLikeTextureFetch(const Fetch& f) {
  if (f.type() != 2u) {  // FetchConstantType::kTexture
    return false;
  }
  if (f.dimension() != 1u) {  // k2D only
    return false;
  }
  const std::uint32_t w = f.width();
  const std::uint32_t h = f.height();
  if (w < 1u || h < 1u || w > kMaxDim || h > kMaxDim) {
    return false;
  }
  if (!GuestAddrPlausible(f.base_address())) {
    return false;
  }
  FormatInfo info{};
  if (!DescribeFormat(f.format(), info)) {
    return false;
  }
  if (f.pitch_div32() == 0u || f.pitch_div32() * 32u < w) {
    return false;
  }
  return true;
}

bool ReadFetchAt(const std::uint8_t* base, std::uint32_t guest_addr, Fetch& out) {
  if (!GuestAddrPlausible(guest_addr)) {
    return false;
  }
  const std::uint8_t* p = base + guest_addr;
  if (!SpanReadable(p, 24)) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    out.d[i] = LoadBE32(p + i * 4);
  }
  return true;
}

// Discovered once: where the fetch constant sits relative to the RHI object.
// kind 0 = inside the FXeTexture itself, 1 = behind the pointer at +8,
// 2 = behind the pointer at +12.
std::atomic<std::int32_t> g_fetch_kind{-1};
std::atomic<std::uint32_t> g_fetch_offset{0};

bool ResolveFetch(const std::uint8_t* base, std::uint32_t tex_rhi, Fetch& out) {
  const std::int32_t kind = g_fetch_kind.load(std::memory_order_acquire);
  if (kind >= 0) {
    const std::uint32_t off = g_fetch_offset.load(std::memory_order_relaxed);
    std::uint32_t root = tex_rhi;
    if (kind != 0) {
      const std::uint8_t* p = base + tex_rhi + (kind == 1 ? 8u : 12u);
      if (!SpanReadable(p, 4)) {
        return false;
      }
      root = LoadBE32(p);
      if (!GuestAddrPlausible(root)) {
        return false;
      }
    }
    return ReadFetchAt(base, root + off, out) && LooksLikeTextureFetch(out);
  }

  const std::uint32_t roots[3] = {0u, 8u, 12u};
  for (int k = 1; k <= 2; ++k) {
    const std::uint8_t* pp = base + tex_rhi + roots[k];
    if (!SpanReadable(pp, 4)) {
      continue;
    }
    const std::uint32_t root = LoadBE32(pp);
    if (!GuestAddrPlausible(root)) {
      continue;
    }
    for (std::uint32_t off = 0; off <= 0x60; off += 4) {
      if (ReadFetchAt(base, root + off, out) && LooksLikeTextureFetch(out)) {
        g_fetch_offset.store(off, std::memory_order_relaxed);
        g_fetch_kind.store(k, std::memory_order_release);
        REXLOG_INFO(
            "[native-tex] fetch constant located: RHI+{} -> D3DBaseTexture+{} "
            "(first texture {}x{} fmt={} tiled={} endian={})",
            roots[k], off, out.width(), out.height(), out.format(), out.tiled(), out.endian());
        return true;
      }
    }
  }
  for (std::uint32_t off = 0; off <= 0x80; off += 4) {
    if (ReadFetchAt(base, tex_rhi + off, out) && LooksLikeTextureFetch(out)) {
      g_fetch_offset.store(off, std::memory_order_relaxed);
      g_fetch_kind.store(0, std::memory_order_release);
      REXLOG_INFO("[native-tex] fetch constant located inline at RHI+{} ({}x{} fmt={} tiled={})",
                  off, out.width(), out.height(), out.format(), out.tiled());
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------------- untiling --
// Xenos 2D tiled address (x, y and pitch in BLOCKS). Same maths the hardware
// and XGRAPHICS::TileSurface use.
std::int32_t TiledOffset2D(std::int32_t x, std::int32_t y, std::uint32_t pitch,
                           std::uint32_t bpb_log2) {
  pitch = AlignUp(pitch, kTileWH);
  const std::int32_t macro = ((x >> 5) + (y >> 5) * std::int32_t(pitch >> 5)) << (bpb_log2 + 7);
  const std::int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  const std::int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Undo the endian swap the GPU applies when fetching. After this the bytes are
// exactly what a PC would store.
void UnswapEndian(std::uint8_t* p, std::size_t bytes, std::uint32_t endian) {
  switch (endian) {
    case 1:  // 8in16
      for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        std::swap(p[i], p[i + 1]);
      }
      break;
    case 2:  // 8in32
      for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(p[i], p[i + 3]);
        std::swap(p[i + 1], p[i + 2]);
      }
      break;
    case 3:  // 16in32
      for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(p[i], p[i + 2]);
        std::swap(p[i + 1], p[i + 3]);
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------- format expansion --
inline std::uint8_t Expand5(std::uint32_t v) {
  return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}
inline std::uint8_t Expand6(std::uint32_t v) {
  return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}
inline std::uint8_t Expand4(std::uint32_t v) { return static_cast<std::uint8_t>((v << 4) | v); }

void ExpandTexel(const std::uint8_t* src, std::uint32_t xfmt, std::uint32_t swizzle,
                 std::uint8_t* dst) {
  std::uint8_t c[6];  // X, Y, Z, W, 0, 1
  c[4] = 0;
  c[5] = 255;
  switch (xfmt) {
    case kFmt_8_8_8_8:
    case kFmt_8_8_8_8_A:
    case kFmt_8_8_8_8_AS_16_16_16_16:
      c[0] = src[0];
      c[1] = src[1];
      c[2] = src[2];
      c[3] = src[3];
      break;
    case kFmt_2_10_10_10: {
      std::uint32_t v;
      std::memcpy(&v, src, 4);
      c[0] = static_cast<std::uint8_t>((v & 0x3FF) >> 2);
      c[1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) >> 2);
      c[2] = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) >> 2);
      c[3] = static_cast<std::uint8_t>(((v >> 30) & 0x3) * 85);
      break;
    }
    case kFmt_5_6_5: {
      std::uint16_t v;
      std::memcpy(&v, src, 2);
      c[0] = Expand5(v & 0x1F);
      c[1] = Expand6((v >> 5) & 0x3F);
      c[2] = Expand5((v >> 11) & 0x1F);
      c[3] = 255;
      break;
    }
    case kFmt_1_5_5_5: {
      std::uint16_t v;
      std::memcpy(&v, src, 2);
      c[0] = Expand5(v & 0x1F);
      c[1] = Expand5((v >> 5) & 0x1F);
      c[2] = Expand5((v >> 10) & 0x1F);
      c[3] = ((v >> 15) & 1) ? 255 : 0;
      break;
    }
    case kFmt_6_5_5: {
      std::uint16_t v;
      std::memcpy(&v, src, 2);
      c[0] = static_cast<std::uint8_t>(((v & 0x3F) << 2) | ((v & 0x3F) >> 4));
      c[1] = Expand5((v >> 6) & 0x1F);
      c[2] = Expand5((v >> 11) & 0x1F);
      c[3] = 255;
      break;
    }
    case kFmt_4_4_4_4: {
      std::uint16_t v;
      std::memcpy(&v, src, 2);
      c[0] = Expand4(v & 0xF);
      c[1] = Expand4((v >> 4) & 0xF);
      c[2] = Expand4((v >> 8) & 0xF);
      c[3] = Expand4((v >> 12) & 0xF);
      break;
    }
    case kFmt_8_8:
      c[0] = src[0];
      c[1] = src[1];
      c[2] = 0;
      c[3] = 255;
      break;
    default:  // k_8 / k_8_A / k_8_B: single component replicated
      c[0] = c[1] = c[2] = c[3] = src[0];
      break;
  }
  dst[0] = c[std::min<std::uint32_t>(swizzle & 7u, 5u)];
  dst[1] = c[std::min<std::uint32_t>((swizzle >> 3) & 7u, 5u)];
  dst[2] = c[std::min<std::uint32_t>((swizzle >> 6) & 7u, 5u)];
  dst[3] = c[std::min<std::uint32_t>((swizzle >> 9) & 7u, 5u)];
}

// ------------------------------------------------------------ the cache ----
struct MipDesc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;  // bytes per row of blocks in our buffer
  std::uint32_t rows = 0;       // rows of blocks
  std::size_t offset = 0;       // into Pending::data
};

struct Pending {
  std::uint32_t slot = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::vector<MipDesc> levels;
  std::vector<std::uint8_t> data;
};

struct Entry {
  std::uint32_t slot = kInvalidSlot;
  ComPtr<ID3D12Resource> texture;
  // Where the pixels lived in guest memory (fetch.base_address(), 4KB-aligned),
  // kept so a guest free (AddUnusedXeResource) can find and drop every entry
  // decoded from that allocation. The XOR-mixed cache key cannot be searched by
  // address, so the invalidation walks - the cache is bounded by kHeapSize.
  std::uint32_t guest_base = 0;
};

struct RetiringUpload {
  ComPtr<ID3D12Resource> buffer;
  std::uint64_t fence = 0;
};

std::mutex g_mutex;
std::unordered_map<std::uint64_t, Entry> g_cache;
std::vector<Pending> g_pending;
std::deque<RetiringUpload> g_retiring;
std::size_t g_bytes = 0;
std::uint32_t g_next_slot = 1;  // 0 is the white fallback
std::uint32_t g_decodes_this_frame = 0;

ComPtr<ID3D12DescriptorHeap> g_heap;
ComPtr<ID3D12Resource> g_white;
std::uint32_t g_srv_stride = 0;
D3D12_CPU_DESCRIPTOR_HANDLE g_heap_cpu{};
D3D12_GPU_DESCRIPTOR_HANDLE g_heap_gpu{};
bool g_heap_failed = false;

std::atomic<std::uint32_t> g_sampler_slots[16] = {};
// Bit i set == sampler i was bound since the last draw.
std::atomic<std::uint32_t> g_fresh_mask{0};
std::atomic<std::uint64_t> g_decoded{0};
std::atomic<std::uint64_t> g_rejected{0};
// Entries dropped because the guest freed their memory (AddUnusedXeResource).
std::atomic<std::uint64_t> g_invalidated_by_free{0};
std::atomic<std::uint32_t> g_logged_details{0};
std::uint64_t g_frame = 0;
std::uint32_t g_last_pick_slot = 0;
bool g_slot_cutout[kHeapSize] = {};

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// -1 = auto-pick (default); >= 0 = forced sampler index via DPOUR_NR_TEXSLOT.
std::int32_t ForcedSamplerIndex() {
  static const std::int32_t idx = [] {
    const char* v = std::getenv("DPOUR_NR_TEXSLOT");
    if (v != nullptr && v[0] != '\0') {
      const int parsed = std::atoi(v);
      if (parsed >= 0 && parsed < 16) {
        return static_cast<std::int32_t>(parsed);
      }
    }
    return -1;
  }();
  return idx;
}

// --------------------------------------------------- base-texture picking --
// Cheap per-RHI-pointer cache of "what kind of texture is this", so scoring the
// 16 sampler slots on every material change costs no guest memory walks.
struct TexMeta {
  std::uint32_t rhi = 0;
  std::uint32_t score = 0;
  std::uint16_t w = 0;
  std::uint16_t h = 0;
  std::uint8_t fmt = 0;
  bool valid = false;
  bool normal = false;
  // Set once this texture has been seen bound as one half of a UE3 directional
  // lightmap pair. Lightmap atlas pages are reused all over a level, so a
  // single sighting is enough to keep them out of every later draw.
  bool lightmap = false;
};
constexpr std::size_t kMetaSlots = 8192;  // power of two
TexMeta g_meta[kMetaSlots];

// A tangent-space normal map is overwhelmingly lavender: B high, R and G both
// near the middle and close to each other. Reading the DXT colour endpoints is
// enough to see that - no need to decode the blocks - and it identifies normal
// maps by CONTENT, which is the only thing that survives materials laying their
// samplers out differently.
bool LooksLikeNormalMap(const std::uint8_t* base, const Fetch& f, const FormatInfo& info) {
  if (info.expand || (info.dxgi != DXGI_FORMAT_BC1_UNORM && info.dxgi != DXGI_FORMAT_BC2_UNORM &&
                      info.dxgi != DXGI_FORMAT_BC3_UNORM)) {
    return false;
  }
  const std::uint8_t* src = base + f.base_address();
  constexpr std::size_t kSpan = 8192;
  if (!SpanReadable(src, kSpan)) {
    return false;
  }
  const std::uint32_t bb = info.block_bytes;
  const std::uint32_t color_off = (bb == 16u) ? 8u : 0u;
  double sr = 0.0, sg = 0.0, sb = 0.0;
  int n = 0;
  for (std::size_t off = 0; off + bb <= kSpan; off += bb) {
    std::uint8_t blk[4];
    std::memcpy(blk, src + off + color_off, 4);
    UnswapEndian(blk, 4, f.endian());
    std::uint16_t c[2];
    std::memcpy(&c[0], blk + 0, 2);
    std::memcpy(&c[1], blk + 2, 2);
    for (int k = 0; k < 2; ++k) {
      sr += ((c[k] >> 11) & 0x1F) / 31.0;
      sg += ((c[k] >> 5) & 0x3F) / 63.0;
      sb += (c[k] & 0x1F) / 31.0;
      ++n;
    }
  }
  if (n == 0) {
    return false;
  }
  const double r = sr / n, g = sg / n, b = sb / n;
  return b > 0.58 && b > r + 0.12 && b > g + 0.12 && std::fabs(r - g) < 0.22 && r > 0.22 &&
         r < 0.78;
}

// Texture "class" used to pick a material's base colour map.
//   2 = block-compressed colour map (what a diffuse almost always is)
//   1 = plain RGBA colour map
//   0 = ramp / mask / normal map / too small to be a base colour
std::uint32_t ScoreTexture(const Fetch& f) {
  const std::uint32_t w = f.width();
  const std::uint32_t h = f.height();
  // Gradient LUTs (512x1) and tiny ramps are never the base colour.
  if (w < 16u || h < 16u) {
    return 0;
  }
  // A shipped diffuse map always has a mip chain. A large texture without one
  // is a render target the game is sampling back (scene colour, reflections,
  // monitors) - decoding those as if they were artwork is where the garish
  // yellow/magenta panels came from.
  if (f.mip_max_level() == 0u && (w > 256u || h > 256u)) {
    return 0;
  }
  switch (f.format()) {
    case kFmt_DXT1:
    case kFmt_DXT1_AS_16_16_16_16:
    case kFmt_DXT2_3:
    case kFmt_DXT2_3_AS_16_16_16_16:
    case kFmt_DXT4_5:
    case kFmt_DXT4_5_AS_16_16_16_16:
      return 2;
    case kFmt_8_8_8_8:
    case kFmt_8_8_8_8_A:
    case kFmt_8_8_8_8_AS_16_16_16_16:
      return 1;
    default:
      // Normal maps (DXN), lightmaps and single-channel masks: never a base
      // colour.
      return 0;
  }
}

TexMeta& MetaFor(const std::uint8_t* base, std::uint32_t rhi) {
  static TexMeta null_meta;
  if (rhi == 0 || !GuestAddrPlausible(rhi)) {
    null_meta = TexMeta{};
    return null_meta;
  }
  const std::size_t i = (rhi * 2654435761u) & (kMetaSlots - 1);
  TexMeta& m = g_meta[i];
  if (m.valid && m.rhi == rhi) {
    return m;
  }
  Fetch f{};
  m = TexMeta{};
  m.rhi = rhi;
  m.valid = true;
  if (ResolveFetch(base, rhi, f)) {
    m.score = ScoreTexture(f);
    m.w = static_cast<std::uint16_t>(std::min<std::uint32_t>(f.width(), 0xFFFFu));
    m.h = static_cast<std::uint16_t>(std::min<std::uint32_t>(f.height(), 0xFFFFu));
    m.fmt = static_cast<std::uint8_t>(f.format());
    FormatInfo info{};
    if (m.score != 0 && DescribeFormat(f.format(), info) && LooksLikeNormalMap(base, f, info)) {
      m.score = 0;  // a normal map is never the base colour
      m.normal = true;
    }
  }
  return m;
}

// ---------------------------------------------------------- BC dump (dbg) --
// Only used by DPOUR_NR_TEXDUMP: decodes a level into RGBA and writes a BMP so
// the decode can be verified against the real artwork.
void DecodeBC(const std::uint8_t* src, std::uint32_t blocks_w, std::uint32_t blocks_h,
              std::uint32_t row_pitch, bool bc3, std::vector<std::uint8_t>& rgba) {
  const std::uint32_t w = blocks_w * 4, h = blocks_h * 4;
  rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
  const std::uint32_t block_bytes = bc3 ? 16u : 8u;
  for (std::uint32_t by = 0; by < blocks_h; ++by) {
    for (std::uint32_t bx = 0; bx < blocks_w; ++bx) {
      const std::uint8_t* b = src + static_cast<std::size_t>(by) * row_pitch + bx * block_bytes;
      const std::uint8_t* cb = bc3 ? b + 8 : b;
      std::uint16_t c0, c1;
      std::memcpy(&c0, cb + 0, 2);
      std::memcpy(&c1, cb + 2, 2);
      std::uint32_t bits;
      std::memcpy(&bits, cb + 4, 4);
      std::uint8_t pal[4][3];
      auto unpack = [](std::uint16_t c, std::uint8_t* o) {
        o[0] = Expand5((c >> 11) & 0x1F);
        o[1] = Expand6((c >> 5) & 0x3F);
        o[2] = Expand5(c & 0x1F);
      };
      unpack(c0, pal[0]);
      unpack(c1, pal[1]);
      for (int i = 0; i < 3; ++i) {
        if (c0 > c1 || bc3) {
          pal[2][i] = static_cast<std::uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
          pal[3][i] = static_cast<std::uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
        } else {
          pal[2][i] = static_cast<std::uint8_t>((pal[0][i] + pal[1][i]) / 2);
          pal[3][i] = 0;
        }
      }
      for (std::uint32_t py = 0; py < 4; ++py) {
        for (std::uint32_t px = 0; px < 4; ++px) {
          const std::uint32_t idx = (bits >> (2 * (py * 4 + px))) & 3u;
          const std::size_t o =
              ((static_cast<std::size_t>(by) * 4 + py) * w + bx * 4 + px) * 4;
          rgba[o + 0] = pal[idx][0];
          rgba[o + 1] = pal[idx][1];
          rgba[o + 2] = pal[idx][2];
          rgba[o + 3] = 255;
        }
      }
    }
  }
}

void WriteBmp(const char* path, const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) {
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    return;
  }
  const std::uint32_t row = w * 3;
  const std::uint32_t pad = (4 - (row % 4)) % 4;
  const std::uint32_t data_size = (row + pad) * h;
  std::uint8_t hdr[54] = {};
  hdr[0] = 'B';
  hdr[1] = 'M';
  const std::uint32_t file_size = 54 + data_size;
  std::memcpy(hdr + 2, &file_size, 4);
  const std::uint32_t off = 54;
  std::memcpy(hdr + 10, &off, 4);
  const std::uint32_t dib = 40;
  std::memcpy(hdr + 14, &dib, 4);
  std::memcpy(hdr + 18, &w, 4);
  std::memcpy(hdr + 22, &h, 4);
  const std::uint16_t planes = 1, bpp = 24;
  std::memcpy(hdr + 26, &planes, 2);
  std::memcpy(hdr + 28, &bpp, 2);
  std::memcpy(hdr + 34, &data_size, 4);
  std::fwrite(hdr, 1, 54, f);
  std::vector<std::uint8_t> line(row + pad, 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* s = rgba + static_cast<std::size_t>(h - 1 - y) * w * 4;
    for (std::uint32_t x = 0; x < w; ++x) {
      line[x * 3 + 0] = s[x * 4 + 2];
      line[x * 3 + 1] = s[x * 4 + 1];
      line[x * 3 + 2] = s[x * 4 + 0];
    }
    std::fwrite(line.data(), 1, line.size(), f);
  }
  std::fclose(f);
}

// -------------------------------------------------------- D3D12 helpers ----
ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* device, DXGI_FORMAT fmt, std::uint32_t w,
                                   std::uint32_t h, std::uint32_t mips) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(mips);
  desc.Format = fmt;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  ComPtr<ID3D12Resource> res;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&res)))) {
    return nullptr;
  }
  res->SetName(L"dpour.texture");  // see the naming note in downpour_native_vbuffers
  return res;
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::size_t size) {
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
  res->SetName(L"dpour.tex.upload");
  return res;
}

// ----------------------------------------------------------- level decode --
// Reads one guest mip level (already located) into `out`, un-tiling and
// un-swapping. Returns false if the guest memory is not readable.
bool DecodeLevel(const std::uint8_t* base, std::uint32_t guest_addr, std::uint32_t level_w,
                 std::uint32_t level_h, std::uint32_t pitch_blocks, bool tiled,
                 std::uint32_t endian, std::uint32_t xfmt, std::uint32_t swizzle,
                 const FormatInfo& info, Pending& pending) {
  const std::uint32_t blocks_w = (level_w + info.block_w - 1u) / info.block_w;
  const std::uint32_t blocks_h = (level_h + info.block_h - 1u) / info.block_h;
  const std::uint32_t bpb_log2 = Log2Floor(info.block_bytes);

  std::size_t src_span;
  if (tiled) {
    src_span = static_cast<std::size_t>(AlignUp(pitch_blocks, kTileWH)) *
                   AlignUp(blocks_h, kTileWH) * info.block_bytes +
               0x1000u;
  } else {
    src_span = static_cast<std::size_t>(pitch_blocks) * blocks_h * info.block_bytes;
  }
  if (src_span == 0 || src_span > (256u << 20)) {
    return false;
  }
  const std::uint8_t* src = base + guest_addr;
  if (!SpanReadable(src, src_span)) {
    return false;
  }

  MipDesc mip;
  mip.width = level_w;
  mip.height = level_h;
  mip.offset = pending.data.size();

  if (!info.expand) {
    mip.row_pitch = blocks_w * info.block_bytes;
    mip.rows = blocks_h;
    pending.data.resize(mip.offset + static_cast<std::size_t>(mip.row_pitch) * blocks_h);
    std::uint8_t* dst_base = pending.data.data() + mip.offset;
    for (std::uint32_t by = 0; by < blocks_h; ++by) {
      std::uint8_t* dst_row = dst_base + static_cast<std::size_t>(by) * mip.row_pitch;
      for (std::uint32_t bx = 0; bx < blocks_w; ++bx) {
        std::size_t src_off;
        if (tiled) {
          src_off = static_cast<std::size_t>(static_cast<std::uint32_t>(
              TiledOffset2D(static_cast<std::int32_t>(bx), static_cast<std::int32_t>(by),
                            pitch_blocks, bpb_log2)));
        } else {
          src_off = (static_cast<std::size_t>(by) * pitch_blocks + bx) * info.block_bytes;
        }
        if (src_off + info.block_bytes > src_span) {
          continue;
        }
        std::memcpy(dst_row + static_cast<std::size_t>(bx) * info.block_bytes, src + src_off,
                    info.block_bytes);
      }
    }
    UnswapEndian(dst_base, static_cast<std::size_t>(mip.row_pitch) * blocks_h, endian);
  } else {
    mip.row_pitch = level_w * 4u;
    mip.rows = level_h;
    pending.data.resize(mip.offset + static_cast<std::size_t>(mip.row_pitch) * level_h);
    std::uint8_t* dst_base = pending.data.data() + mip.offset;
    std::uint8_t texel[16];
    for (std::uint32_t y = 0; y < level_h; ++y) {
      std::uint8_t* dst_row = dst_base + static_cast<std::size_t>(y) * mip.row_pitch;
      for (std::uint32_t x = 0; x < level_w; ++x) {
        std::size_t src_off;
        if (tiled) {
          src_off = static_cast<std::size_t>(static_cast<std::uint32_t>(
              TiledOffset2D(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                            pitch_blocks, bpb_log2)));
        } else {
          src_off = (static_cast<std::size_t>(y) * pitch_blocks + x) * info.block_bytes;
        }
        if (src_off + info.block_bytes > src_span) {
          std::memset(dst_row + static_cast<std::size_t>(x) * 4u, 0, 4);
          continue;
        }
        std::memcpy(texel, src + src_off, info.block_bytes);
        UnswapEndian(texel, info.block_bytes, endian);
        ExpandTexel(texel, xfmt, swizzle, dst_row + static_cast<std::size_t>(x) * 4u);
      }
    }
  }
  pending.levels.push_back(mip);
  return true;
}

// Diagnostics only: decode a texture's base level and write it out so the
// sampler-slot -> artwork mapping can be read off the real images.
void DumpTexture(const std::uint8_t* base, const Fetch& fetch, const char* name) {
  FormatInfo info{};
  if (!DescribeFormat(fetch.format(), info) || info.expand) {
    return;
  }
  if (info.dxgi != DXGI_FORMAT_BC1_UNORM && info.dxgi != DXGI_FORMAT_BC3_UNORM) {
    return;
  }
  Pending p;
  const std::uint32_t pitch_blocks =
      std::max<std::uint32_t>(1u, (fetch.pitch_div32() * 32u) / info.block_w);
  if (!DecodeLevel(base, fetch.base_address(), fetch.width(), fetch.height(), pitch_blocks,
                   fetch.tiled(), fetch.endian(), fetch.format(), fetch.swizzle(), info, p) ||
      p.levels.empty()) {
    return;
  }
  const MipDesc& m = p.levels[0];
  const std::uint32_t blocks_w = m.row_pitch / info.block_bytes;
  std::vector<std::uint8_t> rgba;
  DecodeBC(p.data.data() + m.offset, blocks_w, m.rows, m.row_pitch,
           info.dxgi == DXGI_FORMAT_BC3_UNORM, rgba);
  WriteBmp(name, rgba.data(), blocks_w * 4, m.rows * 4);
}

}  // namespace

// ---------------------------------------------------------------------------
void SetSampler(std::uint32_t sampler_index, std::uint32_t texture_rhi_guest) {
  if (sampler_index < 16u) {
    g_sampler_slots[sampler_index].store(texture_rhi_guest, std::memory_order_relaxed);
    g_fresh_mask.fetch_or(1u << sampler_index, std::memory_order_relaxed);
  }
}

void NextDraw() { g_fresh_mask.store(0, std::memory_order_relaxed); }

void SurveyDraw(const std::uint8_t* base, std::uint32_t tag) {
  if (base == nullptr) {
    return;
  }
  const std::uint32_t fresh = g_fresh_mask.load(std::memory_order_relaxed);
  char line[640];
  int n = std::snprintf(line, sizeof(line), "draw%u pick=s%u fresh=%#x |", tag, g_last_pick_slot,
                        fresh);
  for (std::uint32_t i = 0; i < 16 && n > 0 && n < static_cast<int>(sizeof(line)) - 40; ++i) {
    const std::uint32_t rhi = g_sampler_slots[i].load(std::memory_order_relaxed);
    if (rhi == 0) {
      continue;
    }
    Fetch f{};
    if (!ResolveFetch(base, rhi, f)) {
      n += std::snprintf(line + n, sizeof(line) - n, " s%u=?", i);
      continue;
    }
    n += std::snprintf(line + n, sizeof(line) - n, " s%u=%ux%u/f%u%s", i, f.width(), f.height(),
                       f.format(), ((fresh >> i) & 1u) ? "*" : "");
    char name[128];
    std::snprintf(name, sizeof(name), "draw%u_s%02u_%ux%u_f%u.bmp", tag, i, f.width(), f.height(),
                  f.format());
    DumpTexture(base, f, name);
  }
  REXLOG_INFO("[native-tex] {}", line);
}

std::uint32_t BoundBaseTexture(const std::uint8_t* base) {
  const std::int32_t forced = ForcedSamplerIndex();
  if (forced >= 0) {
    return g_sampler_slots[forced].load(std::memory_order_relaxed);
  }
  if (base == nullptr) {
    return 0;
  }
  static std::uint32_t last_hash = 0;
  static std::uint32_t last_pick = 0;

  // Two rules, learned from the game's real sampler tables:
  //
  //  * A UE3 directional lightmap is TWO textures of identical size and format
  //    bound to ADJACENT samplers, and it is the only thing rebound between the
  //    draws of one material (they are per-mesh atlas pages). Exactly-two-
  //    adjacent is therefore a precise signature - and since atlas pages are
  //    shared across a level, one sighting is enough to blacklist them for
  //    good. Without this the world ends up painted with lightmap charts.
  //  * Normal maps are caught by content in MetaFor (lavender DXT endpoints).
  //
  // The pick itself scans ALL bound samplers, not just the freshly rebound
  // ones: a material's own textures stay bound across many meshes, so looking
  // only at what changed since the last draw usually shows nothing but the
  // lightmap pair.
  const std::uint32_t fresh = g_fresh_mask.load(std::memory_order_relaxed);
  std::uint32_t slots[16];
  std::uint32_t hash = 0;
  for (std::uint32_t i = 0; i < 16; ++i) {
    slots[i] = g_sampler_slots[i].load(std::memory_order_relaxed);
    hash = hash * 31u + slots[i];
  }
  if (fresh != 0 && __popcnt(fresh) == 2) {
    for (std::uint32_t i = 0; i + 1 < 16; ++i) {
      if (fresh != (3u << i)) {
        continue;
      }
      TexMeta& a = MetaFor(base, slots[i]);
      TexMeta& b = MetaFor(base, slots[i + 1]);
      if (a.score != 0 && b.score != 0 && a.rhi != b.rhi && a.w == b.w && a.h == b.h &&
          a.fmt == b.fmt) {
        a.lightmap = true;
        b.lightmap = true;
      }
      break;
    }
  }

  if (hash == last_hash) {
    return last_pick;
  }

  // The material compiler assigns sampler registers in sampling order, so the
  // base colour is the first colour map that is neither lightmap nor normal.
  std::uint32_t best = 0;
  std::uint32_t best_score = 0;
  std::uint32_t best_slot = 0;
  for (std::uint32_t i = 0; i < 16; ++i) {
    const TexMeta& m = MetaFor(base, slots[i]);
    if (m.lightmap) {
      continue;
    }
    if (m.score > best_score) {
      best_score = m.score;
      best = slots[i];
      best_slot = i;
      if (m.score == 2) {
        break;
      }
    }
  }

  g_last_pick_slot = best_slot;
  last_hash = hash;
  last_pick = best;
  return best;
}

void BeginFrame() {
  g_decodes_this_frame = 0;
  ++g_frame;
}

std::uint32_t GuestBaseAddress(const std::uint8_t* base, std::uint32_t texture_guest) {
  if (base == nullptr || !GuestAddrPlausible(texture_guest)) {
    return 0;
  }
  Fetch fetch{};
  if (!ResolveFetch(base, texture_guest, fetch)) {
    return 0;
  }
  return fetch.base_address();
}

std::uint32_t Acquire(const std::uint8_t* base, std::uint32_t texture_rhi_guest) {
  if (base == nullptr || !GuestAddrPlausible(texture_rhi_guest)) {
    return kWhiteSlot;
  }

  // The fetch constant is resolved BEFORE the render-target lookup now, because
  // its base address is what identifies the texture's memory - and memory, not
  // the object, is what the engine shares between render targets.
  Fetch fetch{};
  if (!ResolveFetch(base, texture_rhi_guest, fetch)) {
    return kWhiteSlot;
  }

  // Reference behaviour first: a texture the game resolves a render target
  // into is served by that target's own SRV - our rendered image, no decode.
  {
    const std::uint32_t rt_slot =
        dpour_scene::RtBackedSrvSlot(texture_rhi_guest, fetch.base_address());
    if (rt_slot != kInvalidSlot) {
      return rt_slot;
    }
  }

  FormatInfo info{};
  if (!DescribeFormat(fetch.format(), info)) {
    return kWhiteSlot;
  }

  // Identity of the actual pixels, not of the RHI wrapper.
  const std::uint64_t key = (static_cast<std::uint64_t>(fetch.base_address()) << 24) ^
                            (static_cast<std::uint64_t>(fetch.d[1]) << 8) ^
                            static_cast<std::uint64_t>(fetch.d[2]);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
      return it->second.slot;
    }
    if (g_next_slot >= kHeapSize || g_bytes >= kMaxTextureBytes) {
      return kWhiteSlot;
    }
    if (g_decodes_this_frame >= kMaxDecodesPerFrame) {
      return kWhiteSlot;  // try again next frame; keeps warm-up smooth
    }
  }

  const std::uint32_t width = fetch.width();
  const std::uint32_t height = fetch.height();
  const std::uint32_t min_level = fetch.mip_min_level();
  std::uint32_t max_level = fetch.mip_max_level();
  // Mips at or beyond the packed tail live inside the previous level's padding;
  // we stop before it rather than decoding the tail layout.
  const std::uint32_t packed_level =
      fetch.packed_mips() ? (Log2Ceil(std::min(width, height)) > 4u
                                 ? Log2Ceil(std::min(width, height)) - 4u
                                 : 0u)
                          : 0xFFFFFFFFu;
  if (packed_level != 0xFFFFFFFFu && packed_level > 0u) {
    max_level = std::min(max_level, packed_level - 1u);
  } else if (packed_level == 0u) {
    max_level = 0u;
  }
  max_level = std::min(max_level, kMaxMips - 1u);

  Pending pending;
  pending.format = info.dxgi;

  const std::uint32_t base_pitch_blocks =
      std::max<std::uint32_t>(1u, (fetch.pitch_div32() * 32u) / info.block_w);

  bool ok = false;
  if (min_level == 0u) {
    ok = DecodeLevel(base, fetch.base_address(), width, height, base_pitch_blocks, fetch.tiled(),
                     fetch.endian(), fetch.format(), fetch.swizzle(), info, pending);
  }
  if (!ok && min_level == 0u) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    return kWhiteSlot;
  }

  // Mip levels 1..max_level live under mip_address, each aligned to 4 KB, with
  // a pitch derived from the power-of-two rounded base size.
  if (GuestAddrPlausible(fetch.mip_address()) && max_level >= 1u) {
    std::uint32_t offset_bytes = 0;
    for (std::uint32_t level = 1; level <= max_level; ++level) {
      const std::uint32_t lw = std::max(NextPow2(width) >> level, 1u);
      const std::uint32_t lh = std::max(NextPow2(height) >> level, 1u);
      const std::uint32_t pitch_blocks =
          AlignUp(AlignUp(lw, info.block_w) / info.block_w, kTileWH);
      const std::uint32_t rows_blocks = AlignUp(AlignUp(lh, info.block_h) / info.block_h, kTileWH);
      const std::uint32_t slice_bytes =
          AlignUp(pitch_blocks * info.block_bytes * rows_blocks, kSubresourceAlign);
      // Only append the level we are actually missing (levels are contiguous).
      if (static_cast<std::uint32_t>(pending.levels.size()) == level) {
        const std::uint32_t real_w = std::max(width >> level, 1u);
        const std::uint32_t real_h = std::max(height >> level, 1u);
        if (!DecodeLevel(base, fetch.mip_address() + offset_bytes, real_w, real_h, pitch_blocks,
                         fetch.tiled(), fetch.endian(), fetch.format(), fetch.swizzle(), info,
                         pending)) {
          break;  // keep whatever we already have
        }
      }
      offset_bytes += slice_bytes;
    }
  }

  if (pending.levels.empty()) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    return kWhiteSlot;
  }

  // Diagnostics + optional dump of the decoded base level.
  const std::uint32_t nlog = g_logged_details.fetch_add(1, std::memory_order_relaxed);
  if (nlog < 24) {
    REXLOG_INFO(
        "[native-tex] #{} {}x{} fmt={} endian={} tiled={} pitch32={} swz={:#x} mips={}..{} "
        "levels={} base={:#x} mip={:#x}",
        nlog, width, height, fetch.format(), fetch.endian(), fetch.tiled(), fetch.pitch_div32(),
        fetch.swizzle(), min_level, max_level, pending.levels.size(), fetch.base_address(),
        fetch.mip_address());
    static const bool dump = EnvOn("DPOUR_NR_TEXDUMP");
    if (dump && !info.expand &&
        (info.dxgi == DXGI_FORMAT_BC1_UNORM || info.dxgi == DXGI_FORMAT_BC3_UNORM)) {
      std::vector<std::uint8_t> rgba;
      const MipDesc& m = pending.levels[0];
      DecodeBC(pending.data.data() + m.offset, m.row_pitch / info.block_bytes, m.rows, m.row_pitch,
               info.dxgi == DXGI_FORMAT_BC3_UNORM, rgba);
      char path[256];
      std::snprintf(path, sizeof(path), "texdump_%02u_%ux%u_f%u_e%u.bmp", nlog, width, height,
                    fetch.format(), fetch.endian());
      WriteBmp(path, rgba.data(), (m.row_pitch / info.block_bytes) * 4, m.rows * 4);
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_cache.find(key);
  if (it != g_cache.end()) {
    return it->second.slot;  // raced with another decode
  }
  if (g_next_slot >= kHeapSize) {
    return kWhiteSlot;
  }
  pending.slot = g_next_slot++;
  g_slot_cutout[pending.slot] = (info.dxgi == DXGI_FORMAT_BC1_UNORM);
  g_bytes += pending.data.size();
  ++g_decodes_this_frame;
  g_decoded.fetch_add(1, std::memory_order_relaxed);
  Entry entry;
  entry.slot = pending.slot;
  entry.guest_base = fetch.base_address();
  const std::uint32_t slot = pending.slot;
  g_cache.emplace(key, std::move(entry));
  g_pending.push_back(std::move(pending));
  return slot;
}

bool EnsureHeap(ID3D12Device* device) {
  if (g_heap) {
    return true;
  }
  if (g_heap_failed || device == nullptr) {
    return false;
  }
  g_heap_failed = true;

  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = kHeapSize;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_heap)))) {
    REXLOG_ERROR("[native-tex] SRV heap creation failed");
    return false;
  }
  g_srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g_heap_cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
  g_heap_gpu = g_heap->GetGPUDescriptorHandleForHeapStart();

  g_white = CreateTex2D(device, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1);
  if (!g_white) {
    REXLOG_ERROR("[native-tex] white fallback texture failed");
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  for (std::uint32_t i = 0; i < kHeapSize; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap_cpu;
    h.ptr += static_cast<SIZE_T>(i) * g_srv_stride;
    device->CreateShaderResourceView(g_white.Get(), &srv, h);
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Pending white;
    white.slot = kWhiteSlot;
    white.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    white.data = {0xFF, 0xFF, 0xFF, 0xFF};
    MipDesc m;
    m.width = m.height = 1;
    m.row_pitch = 4;
    m.rows = 1;
    m.offset = 0;
    white.levels.push_back(m);
    g_pending.push_back(std::move(white));
  }

  g_heap_failed = false;
  REXLOG_INFO("[native-tex] SRV heap ready ({} descriptors, base sampler = {})", kHeapSize,
              ForcedSamplerIndex() < 0 ? -1 : ForcedSamplerIndex());
  return true;
}

void Flush(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* cmd) {
  if (device == nullptr || cmd == nullptr || !g_heap) {
    return;
  }
  std::vector<Pending> work;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.empty()) {
      return;
    }
    work.swap(g_pending);
  }

  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  barriers.reserve(work.size());

  for (Pending& p : work) {
    const std::uint32_t mips = static_cast<std::uint32_t>(p.levels.size());
    if (mips == 0) {
      continue;
    }
    ComPtr<ID3D12Resource> tex;
    if (p.slot == kWhiteSlot) {
      tex = g_white;
    } else {
      tex = CreateTex2D(device, p.format, p.levels[0].width, p.levels[0].height, mips);
    }
    if (!tex) {
      continue;
    }

    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[kMaxMips]{};
    UINT num_rows[kMaxMips]{};
    UINT64 row_sizes[kMaxMips]{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, mips, 0, fps, num_rows, row_sizes, &total);

    ComPtr<ID3D12Resource> upload = CreateUploadBuffer(device, static_cast<std::size_t>(total));
    if (!upload) {
      continue;
    }
    std::uint8_t* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped))) || mapped == nullptr) {
      continue;
    }
    for (std::uint32_t l = 0; l < mips; ++l) {
      const MipDesc& m = p.levels[l];
      const std::uint32_t copy_rows = std::min<std::uint32_t>(m.rows, num_rows[l]);
      const std::size_t copy_bytes =
          std::min<std::size_t>(m.row_pitch, static_cast<std::size_t>(row_sizes[l]));
      for (std::uint32_t r = 0; r < copy_rows; ++r) {
        std::memcpy(mapped + fps[l].Offset + static_cast<std::size_t>(r) * fps[l].Footprint.RowPitch,
                    p.data.data() + m.offset + static_cast<std::size_t>(r) * m.row_pitch,
                    copy_bytes);
      }
    }
    upload->Unmap(0, nullptr);

    for (std::uint32_t l = 0; l < mips; ++l) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = tex.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = l;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = upload.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fps[l];
      cmd->D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers.push_back(b);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = p.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mips;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap_cpu;
    h.ptr += static_cast<SIZE_T>(p.slot) * g_srv_stride;
    device->CreateShaderResourceView(tex.Get(), &srv, h);

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& kv : g_cache) {
      if (kv.second.slot == p.slot) {
        kv.second.texture = tex;
        break;
      }
    }
    g_retiring.push_back(RetiringUpload{upload, 0});
  }

  if (!barriers.empty()) {
    cmd->D3DResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
  }
}

void InvalidateGuestAddress(std::uint32_t addr) {
  if (addr == 0) {
    return;
  }
  // AddUnusedXeResource hands over the allocation's base; texture fetches store
  // a 4KB-aligned base too, so the match is on the page-aligned address.
  const std::uint32_t page = addr & ~0xFFFu;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_cache.begin(); it != g_cache.end();) {
    if (it->second.guest_base == page && it->second.guest_base != 0) {
      // The D3D12 texture must NOT die here: a replay in flight may still
      // sample its descriptor. Erasing the entry released the ComPtr on the
      // spot, and the first content transition after that TDR'd the device -
      // the exact use-after-free the reference avoids by parking retired
      // resources until the GPU is done (g_tempResources). Park it in
      // g_retiring with fence 0; RetireUploads stamps it submitted+8, the same
      // slack every other retirement here gets.
      if (it->second.texture) {
        g_retiring.push_back(RetiringUpload{std::move(it->second.texture), 0});
      }
      // The SRV heap slot is NOT reclaimed - the allocator is monotonic. That
      // leaks one slot per invalidated texture, which is the price of never
      // serving pixels from memory the guest has already handed to something
      // else. The heap holds kHeapSize slots; the stats line reports the count
      // so a session that ever exhausts it names the culprit instead of
      // silently going white.
      it = g_cache.erase(it);
      g_invalidated_by_free.fetch_add(1, std::memory_order_relaxed);
    } else {
      ++it;
    }
  }
}

void RetireUploads(std::uint64_t completed_fence, std::uint64_t submitted_fence) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& r : g_retiring) {
    if (r.fence == 0) {
      // Same slack as dpour_vbuf::RetireUploads: never free on the fence of
      // the CURRENT submission, references can live in later ones (see the
      // page-fault post-mortem there).
      r.fence = submitted_fence + 8;
    }
  }
  while (!g_retiring.empty() && g_retiring.front().fence != 0 &&
         g_retiring.front().fence <= completed_fence) {
    g_retiring.pop_front();
  }
}

bool SlotIsCutout(std::uint32_t slot) {
  return slot < kHeapSize && g_slot_cutout[slot];
}

ID3D12DescriptorHeap* Heap() { return g_heap.Get(); }

// === DPOUR MIGRATION 2026-07-25: shared bindless heap ===
// The translated shaders index one descriptor heap for every view type - the
// vertex streams they fetch from are just another SRV in it (t0,space5). So the
// slot allocator has to be shared rather than duplicated, otherwise two modules
// would hand out the same index and silently overwrite each other's views.
std::uint32_t AllocDescriptorSlot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_heap || g_next_slot >= kHeapSize) {
    return kInvalidSlot;
  }
  return g_next_slot++;
}

std::uint64_t CpuHandleAt(std::uint32_t slot) {
  if (!g_heap || slot >= kHeapSize) {
    return 0;
  }
  return g_heap_cpu.ptr + static_cast<std::uint64_t>(slot) * g_srv_stride;
}

std::uint64_t GpuHandleStart() { return g_heap ? g_heap_gpu.ptr : 0; }

// Descriptor index per guest sampler slot, for the shaders' SharedConstants.
//
// This is the whole of the texture binding on the translated-shader path: the
// shader reads g_Tex2DIdx[slot] and indexes the bindless heap with it, so there
// is no per-draw descriptor table to rebind and no guessing which sampler holds
// the base colour - the shader already knows, because the game's own microcode
// says so.
void FillSamplerTable(const std::uint8_t* base, std::uint32_t* out, std::uint32_t count) {
  if (out == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    out[i] = kWhiteSlot;
  }
  // DPOUR_NR_TEX_WHITE: bind the white 1x1 to every slot. The scene's pixel
  // shaders were measured running and writing rgb exactly zero with a live
  // alpha - the signature of a texture input that samples as black (a lightmap
  // at zero multiplies the whole material away). With every sampler forced
  // white, a scene that stays black cannot blame its textures.
  static const bool force_white = [] {
    const char* v = std::getenv("DPOUR_NR_TEX_WHITE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  if (base == nullptr || force_white) {
    return;
  }
  const std::uint32_t n = (count < 16u) ? count : 16u;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t rhi = g_sampler_slots[i].load(std::memory_order_relaxed);
    if (rhi == 0) {
      continue;
    }
    const std::uint32_t slot = Acquire(base, rhi);
    out[i] = (slot == kInvalidSlot) ? kWhiteSlot : slot;
  }
}

std::uint64_t SamplerSlotsKey() {
  std::uint64_t h = 1469598103934665603ull;
  for (std::uint32_t i = 0; i < 16u; ++i) {
    h ^= g_sampler_slots[i].load(std::memory_order_relaxed);
    h *= 1099511628211ull;
  }
  return h;
}
// === END DPOUR MIGRATION 2026-07-25 ===

std::uint64_t GpuHandleAt(std::uint32_t slot) {
  if (!g_heap || slot >= kHeapSize) {
    return g_heap ? g_heap_gpu.ptr : 0;
  }
  return g_heap_gpu.ptr + static_cast<std::uint64_t>(slot) * g_srv_stride;
}

void LogStats() {
  std::lock_guard<std::mutex> lock(g_mutex);
  REXLOG_INFO("[native-tex] textures={} ({} MB) rejected={} slots_used={} freed_by_guest={}",
              g_decoded.load(std::memory_order_relaxed), g_bytes / (1024 * 1024),
              g_rejected.load(std::memory_order_relaxed), g_next_slot,
              g_invalidated_by_free.load(std::memory_order_relaxed));
}

// ------------------------------------------------- scene injection (write) --
namespace {

// IEEE half -> float, bit-exact including denormals.
float HalfToFloat(std::uint16_t h) {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1Fu;
  std::uint32_t man = h & 0x3FFu;
  std::uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((man & 0x400u) == 0) {
        man <<= 1;
        --exp;
      }
      man &= 0x3FFu;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// float -> Xenos 7e3 (bias 124, 7-bit mantissa). The exact mirror of the SDK's
// Float7e3To32 (float bits = ((e7e3+124)<<23)|(m<<16..)), built with the same
// structure as its Float32To20e4: saturate at (max_exp+1)<<23 minus half an
// LSB, denormalize below the smallest normal, round to nearest even.
std::uint32_t Float32To7e3(float f32) {
  if (!(f32 > 0.0f)) {
    return 0;  // negative, -0 and NaN all clamp to zero, as the hardware does
  }
  std::uint32_t u;
  std::memcpy(&u, &f32, 4);
  if (u >= 0x41FF8000u) {
    return 0x3FFu;
  }
  if (u < 0x3E800000u) {
    const std::uint32_t shift = std::min<std::uint32_t>(125u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
  } else {
    u += 0xC2000000u;  // rebias: subtract 124 << 23
  }
  u += 0x7FFFu + ((u >> 16) & 1u);
  return (u >> 16) & 0x3FFu;
}

// One-time lookup tables: half bits -> encoded channel. 921k texels per frame
// would otherwise spend a float conversion per channel.
const std::uint16_t* Half7e3Lut() {
  static const std::vector<std::uint16_t>& lut = *[] {
    auto* t = new std::vector<std::uint16_t>(65536);
    for (std::uint32_t i = 0; i < 65536; ++i) {
      (*t)[i] = static_cast<std::uint16_t>(Float32To7e3(HalfToFloat(static_cast<std::uint16_t>(i))));
    }
    return t;
  }();
  return lut.data();
}

const std::uint8_t* HalfUnormLut() {
  static const std::vector<std::uint8_t>& lut = *[] {
    auto* t = new std::vector<std::uint8_t>(65536);
    for (std::uint32_t i = 0; i < 65536; ++i) {
      const float f = HalfToFloat(static_cast<std::uint16_t>(i));
      (*t)[i] = f <= 0.0f ? 0 : (f >= 1.0f ? 255 : static_cast<std::uint8_t>(f * 255.0f + 0.5f));
    }
    return t;
  }();
  return lut.data();
}

bool EncodeFromFetchImpl(const Fetch& f, const std::uint8_t* src_rgba16f,
                         std::uint32_t src_pitch_bytes, std::uint32_t src_w, std::uint32_t src_h,
                         std::vector<std::uint8_t>& out, EncodedSurface& desc);

}  // namespace

bool EncodeColorForGuestTexture(const std::uint8_t* base, std::uint32_t texture_rhi_guest,
                                const std::uint8_t* src_rgba16f, std::uint32_t src_pitch_bytes,
                                std::uint32_t src_w, std::uint32_t src_h,
                                std::vector<std::uint8_t>& out, EncodedSurface& desc) {
  static std::atomic<bool> logged_reason{false};
  // The regular resolver rejects render-target formats it cannot SAMPLE-decode
  // (7e3 has no DescribeFormat entry), so read the fetch constant through the
  // already-discovered layout and validate leniently here.
  const std::int32_t kind = g_fetch_kind.load(std::memory_order_acquire);
  if (kind < 0 || base == nullptr || src_rgba16f == nullptr) {
    return false;  // no texture has been decoded yet, so the offset is unknown
  }
  const std::uint32_t fetch_off = g_fetch_offset.load(std::memory_order_relaxed);
  const auto fetch_from = [&](std::uint32_t rhi, Fetch& f) {
    std::uint32_t root = rhi;
    if (kind != 0) {
      const std::uint8_t* pp = base + rhi + (kind == 1 ? 8u : 12u);
      if (!SpanReadable(pp, 4)) {
        return false;
      }
      root = LoadBE32(pp);
      if (!GuestAddrPlausible(root)) {
        return false;
      }
    }
    if (!ReadFetchAt(base, root + fetch_off, f)) {
      return false;
    }
    return f.type() == 2u && f.dimension() == 1u && GuestAddrPlausible(f.base_address()) &&
           f.pitch_div32() != 0;
  };
  Fetch f{};
  bool got = fetch_from(texture_rhi_guest, f);
  if (!got) {
    // The RHI hooks receive REFERENCES (a pointer to the TRefCountPtr), not the
    // FXeTexture2D itself - the same lesson the surface identity taught. One
    // dereference and retry covers whichever convention this pointer follows.
    const std::uint8_t* pp = base + texture_rhi_guest;
    if (GuestAddrPlausible(texture_rhi_guest) && SpanReadable(pp, 4)) {
      const std::uint32_t inner = LoadBE32(pp);
      if (GuestAddrPlausible(inner)) {
        got = fetch_from(inner, f);
        if (got && !logged_reason.exchange(true)) {
          REXLOG_INFO("[native-inject] resolve texture pointer needed one dereference ({:#x} -> {:#x})",
                      texture_rhi_guest, inner);
        }
      }
    }
  }
  if (!got) {
    if (!logged_reason.exchange(true)) {
      REXLOG_INFO("[native-inject] scene texture fetch not usable (type={} dim={} base={:#x})",
                  f.type(), f.dimension(), f.base_address());
    }
    return false;
  }
  return EncodeFromFetchImpl(f, src_rgba16f, src_pitch_bytes, src_w, src_h, out, desc);
}

namespace {
bool EncodeFromFetchImpl(const Fetch& f, const std::uint8_t* src_rgba16f,
                         std::uint32_t src_pitch_bytes, std::uint32_t src_w, std::uint32_t src_h,
                         std::vector<std::uint8_t>& out, EncodedSurface& desc) {
  static std::atomic<bool> logged_reason{false};
  const std::uint32_t fmt = f.format();
  std::uint32_t bpb_log2;
  if (fmt == 63u) {  // k_2_10_10_10_FLOAT_EDRAM - the 7e3 HDR scene colour
    bpb_log2 = 2;
  } else if (fmt == 32u) {  // k_16_16_16_16_FLOAT
    bpb_log2 = 3;
  } else if (fmt == kFmt_8_8_8_8 || fmt == kFmt_8_8_8_8_AS_16_16_16_16) {
    bpb_log2 = 2;
  } else {
    if (!logged_reason.exchange(true)) {
      REXLOG_INFO("[native-inject] scene texture format {} not encodable", fmt);
    }
    return false;
  }
  const std::uint32_t bpb = 1u << bpb_log2;
  const std::uint32_t pitch = f.pitch_div32() * 32u;
  const std::uint32_t tex_h = f.height();
  const std::uint32_t w = std::min(f.width(), src_w);
  const std::uint32_t h = std::min(tex_h, src_h);
  const std::uint32_t rows = f.tiled() ? AlignUp(tex_h, kTileWH) : tex_h;
  const std::size_t total = static_cast<std::size_t>(AlignUp(pitch, kTileWH)) * rows * bpb;
  if (w == 0 || h == 0 || total == 0 || total > 64u * 1024u * 1024u) {
    return false;
  }
  out.assign(total, 0);
  const std::uint16_t* lut7e3 = fmt == 63u ? Half7e3Lut() : nullptr;
  const std::uint8_t* lut8 = (fmt == kFmt_8_8_8_8 || fmt == kFmt_8_8_8_8_AS_16_16_16_16)
                                 ? HalfUnormLut()
                                 : nullptr;
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t* srow = src_rgba16f + static_cast<std::size_t>(y) * src_pitch_bytes;
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint16_t hf[4];
      std::memcpy(hf, srow + static_cast<std::size_t>(x) * 8u, 8);
      const std::size_t off =
          f.tiled() ? static_cast<std::size_t>(static_cast<std::uint32_t>(TiledOffset2D(
                          static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), pitch,
                          bpb_log2)))
                    : (static_cast<std::size_t>(y) * pitch + x) * bpb;
      if (off + bpb > total) {
        continue;
      }
      std::uint8_t* d = out.data() + off;
      if (lut7e3 != nullptr) {
        const std::uint32_t v = static_cast<std::uint32_t>(lut7e3[hf[0]]) |
                                (static_cast<std::uint32_t>(lut7e3[hf[1]]) << 10) |
                                (static_cast<std::uint32_t>(lut7e3[hf[2]]) << 20) | (0x3u << 30);
        std::memcpy(d, &v, 4);
      } else if (fmt == 32u) {
        std::memcpy(d, hf, 8);
      } else {
        const std::uint8_t px[4] = {lut8[hf[0]], lut8[hf[1]], lut8[hf[2]], lut8[hf[3]]};
        std::memcpy(d, px, 4);
      }
    }
  }
  // The GPU un-swaps on fetch; the swap is an involution, so writing means
  // applying the very same transform the reader undoes.
  if (f.endian() != 0) {
    UnswapEndian(out.data(), out.size(), f.endian());
  }
  desc.guest_dest = f.base_address();
  desc.bytes = static_cast<std::uint32_t>(total);
  desc.width = f.width();
  desc.height = tex_h;
  desc.format = fmt;
  desc.tiled = f.tiled();
  return true;
}

}  // namespace

std::uint32_t LocateResolveTextureFetch(const std::uint8_t* base, std::uint32_t surface_ref_guest,
                                        std::uint32_t expect_w, std::uint32_t expect_h) {
  if (base == nullptr || !GuestAddrPlausible(surface_ref_guest)) {
    return 0;
  }
  // A texture fetch constant with EXACTLY the scene's dimensions cannot be
  // mistaken for anything else, so scan: every pointer-sized member of the
  // FSurfaceRHIRef, then one and two indirections behind each, each level
  // swept across a small offset window.
  const auto valid = [&](const Fetch& f) {
    return f.type() == 2u && f.dimension() == 1u && f.width() == expect_w &&
           f.height() == expect_h && GuestAddrPlausible(f.base_address()) &&
           f.pitch_div32() * 32u >= expect_w;
  };
  const auto scan_object = [&](std::uint32_t obj, std::uint32_t& found) {
    if (!GuestAddrPlausible(obj)) {
      return false;
    }
    for (std::uint32_t off = 0; off <= 0x60; off += 4) {
      Fetch f{};
      if (ReadFetchAt(base, obj + off, f) && valid(f)) {
        found = obj + off;
        return true;
      }
    }
    return false;
  };
  for (std::uint32_t member = 0; member <= 0x1C; member += 4) {
    const std::uint8_t* pm = base + surface_ref_guest + member;
    if (!SpanReadable(pm, 4)) {
      continue;
    }
    const std::uint32_t level1 = LoadBE32(pm);
    if (!GuestAddrPlausible(level1)) {
      continue;
    }
    std::uint32_t found = 0;
    if (scan_object(level1, found)) {
      REXLOG_INFO("[native-inject] scene texture fetch located: ref+{} -> obj -> fetch @{:#x}",
                  member, found);
      return found;
    }
    for (std::uint32_t inner = 0; inner <= 0x10; inner += 4) {
      const std::uint8_t* pi = base + level1 + inner;
      if (!SpanReadable(pi, 4)) {
        continue;
      }
      if (scan_object(LoadBE32(pi), found)) {
        REXLOG_INFO(
            "[native-inject] scene texture fetch located: ref+{} -> obj+{} -> obj -> fetch @{:#x}",
            member, inner, found);
        return found;
      }
    }
  }
  return 0;
}

bool EncodeColorForGuestFetch(const std::uint8_t* base, std::uint32_t fetch_addr,
                              const std::uint8_t* src_rgba16f, std::uint32_t src_pitch_bytes,
                              std::uint32_t src_w, std::uint32_t src_h,
                              std::vector<std::uint8_t>& out, EncodedSurface& desc) {
  if (base == nullptr || fetch_addr == 0 || src_rgba16f == nullptr) {
    return false;
  }
  Fetch f{};
  if (!ReadFetchAt(base, fetch_addr, f) || f.type() != 2u || f.dimension() != 1u ||
      !GuestAddrPlausible(f.base_address()) || f.pitch_div32() == 0) {
    return false;
  }
  return EncodeFromFetchImpl(f, src_rgba16f, src_pitch_bytes, src_w, src_h, out, desc);
}

}  // namespace dpour_tex
// === END DPOUR MIGRATION 2026-07-25 ===
