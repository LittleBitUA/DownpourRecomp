// downpour - Native Render: the guest's pipeline state, read where it really is
//
// === DPOUR MIGRATION 2026-07-25: draw path - render state ===
// See downpour_native_state.h for where every offset below came from.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_state.h"

#include <atomic>
#include <cstring>

#include <windows.h>

#include <d3d12.h>

#include <rex/logging.h>

namespace dpour_state {
namespace {

// GDirect3DDevice, the master key for the whole RHI (see the address map).
constexpr std::uint32_t kGDirect3DDevice = 0x83790010;
// GInvertZ, read by ToggleCompareFunction (sub_829C9628: lis r11,-31877 ;
// lwz r11,-19664(r11)). -31877 as an unsigned halfword is 0x837B, so the base is
// 0x837B0000 and the global sits 19664 (0x4CD0) below it.
constexpr std::uint32_t kGInvertZ = 0x837AB330;

// Xenos register shadow, four bytes per register starting at RB_DEPTHCONTROL.
constexpr std::uint32_t kRegBase = 10548;
constexpr std::uint32_t kRbDepthControl = kRegBase + 0;   // 10548
constexpr std::uint32_t kRbBlendControl0 = kRegBase + 4;  // 10552
constexpr std::uint32_t kRbColorControl = kRegBase + 8;   // 10556
// RB_COLOR_MASK, four bits per render target, target 0 in the low nibble and in
// D3D9's own bit order (RED 1, GREEN 2, BLUE 4, ALPHA 8 - the same order D3D12
// uses). Read out of RHISetColorWriteMask (sub_829C9E30), which ends with
//   r10 = mask & 0xF
//   [dev+12292] = r10                       <- the requested mask, cached
//   [dev+10460] = (gate & r10) | ([dev+10460] & ~0xF)
// where `gate` is all-ones unless [dev+12816] is zero, in which case the low
// nibble is forced to 0. So dev+10460 is the EFFECTIVE mask and dev+12292 the
// requested one; the effective one is what the hardware rasterises with.
constexpr std::uint32_t kRbColorMask = 10460;
// Established by sampling both candidates over a whole session: dev+10564 never
// changes (0x00080000 across 38809 draws), while dev+10560 carries a two-bit
// field that takes exactly the three values a cull mode takes - and it is the
// register every draw function loads (sub_82D1F0B0: lwz r5,10560(r31)).
constexpr std::uint32_t kPaSuScModeCntl = kRegBase + 12;  // 10560
constexpr std::uint32_t kRegNeighbour = kRegBase + 16;    // 10564, sampled only
constexpr std::uint32_t kRbAlphaRef = 10500;              // float, D3DRS_ALPHAREF

// Bool/loop constant shadow. The PS float shadow ends at dev+(256+376)*16 =
// dev+10112 (see the header); the banks that follow it in the register file
// (0x4900 bools, 0x4908 loops) are assumed to follow it in the shadow too.
// This is the one offset in the module that is INFERRED rather than read out
// of the code, so Read() samples the window and LogStats() prints it - if the
// dump shows float bit patterns instead of bitfields, the offset is wrong.
constexpr std::uint32_t kBoolBank = 10112;            // 8 dwords, 256 bits
constexpr std::uint32_t kLoopBank = kBoolBank + 32;   // 32 dwords

inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

bool SpanReadable(const void* p, std::size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
      (mbi.Protect & PAGE_NOACCESS) != 0) {
    return false;
  }
  const auto* start = static_cast<const std::uint8_t*>(mbi.BaseAddress);
  return static_cast<const std::uint8_t*>(p) + n <= start + mbi.RegionSize;
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// Xenos blend factor -> D3D12_BLEND. The Xbox 360 D3DBLEND enum values ARE the
// hardware values (which is exactly why RHISetBlendState can pack them into the
// register with nothing but shifts - see sub_829C9990).
D3D12_BLEND BlendFactor(std::uint32_t x) {
  switch (x) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: return D3D12_BLEND_BLEND_FACTOR;
    case 13: return D3D12_BLEND_INV_BLEND_FACTOR;
    // CONSTANT_ALPHA has no separate D3D12 factor; the blend factor's alpha
    // channel carries it, so the same enum is the closest exact match.
    case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

// Alpha-channel factors may not name a colour channel in D3D12.
D3D12_BLEND BlendFactorAlpha(std::uint32_t x) {
  switch (x) {
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_ALPHA;
    case 9: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return BlendFactor(x);
  }
}

D3D12_BLEND_OP BlendOp(std::uint32_t x) {
  switch (x) {
    case 0: return D3D12_BLEND_OP_ADD;           // DST_PLUS_SRC
    case 1: return D3D12_BLEND_OP_SUBTRACT;      // SRC_MINUS_DST
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;  // DST_MINUS_SRC
    default: return D3D12_BLEND_OP_ADD;
  }
}

// Xenos compare functions are D3DCMP minus one: 0 NEVER .. 7 ALWAYS.
D3D12_COMPARISON_FUNC CompareFunc(std::uint32_t x) {
  return static_cast<D3D12_COMPARISON_FUNC>((x & 7u) + 1u);
}

// How many draws compare depth each way. D3D12_COMPARISON_FUNC: 3 = LESS,
// 4 = LESS_EQUAL, 5 = GREATER, 6 = NOT_EQUAL, 7 = GREATER_EQUAL.
std::atomic<std::uint64_t> g_depth_less{0};
std::atomic<std::uint64_t> g_depth_greater{0};

std::atomic<std::uint64_t> g_reads{0};
std::atomic<std::uint64_t> g_no_device{0};
// A tiny sample of the distinct raw register values, printed once. If a field
// were misplaced this is where it shows: the blend register would not be full of
// 0x00010001 (blending off) and the cull field would not be 0/1/2.
constexpr std::uint32_t kSampleSlots = 8;
std::atomic<std::uint32_t> g_sample_depth[kSampleSlots]{};
std::atomic<std::uint32_t> g_sample_blend[kSampleSlots]{};
std::atomic<std::uint32_t> g_sample_raster[kSampleSlots]{};
// The neighbouring register, sampled purely so the log can say which of the two
// carries the cull field: on a title screen everything is cull-none, so only a
// gameplay sample can tell them apart.
std::atomic<std::uint32_t> g_sample_clip[kSampleSlots]{};
// Colour-write-mask census (see the read below for why these are counters).
std::atomic<std::uint64_t> g_cw_full{0};
std::atomic<std::uint64_t> g_cw_zero{0};
std::atomic<std::uint64_t> g_cw_other{0};
// Bool/loop bank evidence: how often the bool bank was nonzero, plus samples.
std::atomic<std::uint64_t> g_bool_nonzero{0};
std::atomic<std::uint32_t> g_sample_bools[kSampleSlots]{};
std::atomic<std::uint32_t> g_sample_loops[kSampleSlots]{};
std::atomic<bool> g_window_dumped{false};

void Sample(std::atomic<std::uint32_t>* slots, std::uint32_t value) {
  for (std::uint32_t i = 0; i < kSampleSlots; ++i) {
    const std::uint32_t cur = slots[i].load(std::memory_order_relaxed);
    if (cur == value) {
      return;
    }
    if (cur == 0) {
      std::uint32_t expected = 0;
      if (slots[i].compare_exchange_strong(expected, value, std::memory_order_relaxed)) {
        return;
      }
      if (slots[i].load(std::memory_order_relaxed) == value) {
        return;
      }
    }
  }
}

}  // namespace

bool Read(const std::uint8_t* base, Pipeline& out) {
  out = Pipeline{};
  if (base == nullptr) {
    return false;
  }
  // Called once per draw - thousands of times a frame - so the device pointer is
  // resolved and page-checked once and then reused. GDirect3DDevice is created
  // during boot and never moves; if it ever did, the shadow it points at would
  // be a different device and the whole RHI address map would be wrong anyway.
  static const std::uint8_t* g_dev_cached = nullptr;
  static const std::uint8_t* g_base_cached = nullptr;
  const std::uint8_t* d = g_dev_cached;
  if (d == nullptr || base != g_base_cached) {
    if (!SpanReadable(base + kGDirect3DDevice, 4)) {
      g_no_device.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const std::uint32_t dev = LoadBE32(base + kGDirect3DDevice);
    if (!GuestAddrPlausible(dev) ||
        !SpanReadable(base + dev + kBoolBank, kRbAlphaRef + 128 - kBoolBank)) {
      g_no_device.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    d = base + dev;
    g_dev_cached = d;
    g_base_cached = base;
  }

  const std::uint32_t depth = LoadBE32(d + kRbDepthControl);
  const std::uint32_t blend = LoadBE32(d + kRbBlendControl0);
  const std::uint32_t color = LoadBE32(d + kRbColorControl);
  const std::uint32_t raster = LoadBE32(d + kPaSuScModeCntl);
  const std::uint32_t alpha_ref_bits = LoadBE32(d + kRbAlphaRef);
  // Counted, not Sample()d: Sample uses 0 as its empty-slot marker and a mask of
  // ZERO is the whole question here. UE3's depth prepass draws with colour writes
  // off, and Downpour's studio build always runs the masked-material lists in it
  // (SceneRendering.cpp:2625, the bDominantShadowsActive condition commented out)
  // - those draws DO bind a pixel shader for the alpha test, so "no pixel shader"
  // does not identify the prepass and this mask does.
  out.color_write_mask = LoadBE32(d + kRbColorMask) & 0xFu;
  if (out.color_write_mask == 0xFu) {
    g_cw_full.fetch_add(1, std::memory_order_relaxed);
  } else if (out.color_write_mask == 0u) {
    g_cw_zero.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_cw_other.fetch_add(1, std::memory_order_relaxed);
  }

  Sample(g_sample_depth, depth);
  Sample(g_sample_blend, blend);
  Sample(g_sample_raster, raster);
  Sample(g_sample_clip, LoadBE32(d + kRegNeighbour));
  g_reads.fetch_add(1, std::memory_order_relaxed);

  out.depth_enable = (depth & 0x2u) != 0;
  out.depth_write = (depth & 0x4u) != 0;
  out.depth_func = CompareFunc((depth >> 4) & 7u);
  if (out.depth_enable) {
    if (out.depth_func == D3D12_COMPARISON_FUNC_LESS ||
        out.depth_func == D3D12_COMPARISON_FUNC_LESS_EQUAL) {
      g_depth_less.fetch_add(1, std::memory_order_relaxed);
    } else if (out.depth_func == D3D12_COMPARISON_FUNC_GREATER ||
               out.depth_func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) {
      g_depth_greater.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // RB_BLENDCONTROL0: colour in bits 0-12, alpha in bits 16-28.
  const std::uint32_t src_c = blend & 0x1Fu;
  const std::uint32_t op_c = (blend >> 5) & 7u;
  const std::uint32_t dst_c = (blend >> 8) & 0x1Fu;
  const std::uint32_t src_a = (blend >> 16) & 0x1Fu;
  const std::uint32_t op_a = (blend >> 21) & 7u;
  const std::uint32_t dst_a = (blend >> 24) & 0x1Fu;
  out.src_blend = BlendFactor(src_c);
  out.dst_blend = BlendFactor(dst_c);
  out.blend_op = BlendOp(op_c);
  out.src_blend_alpha = BlendFactorAlpha(src_a);
  out.dst_blend_alpha = BlendFactorAlpha(dst_a);
  out.blend_op_alpha = BlendOp(op_a);
  // "Blending off" is not a flag on this hardware - the XDK writes ONE/ZERO/ADD
  // into the register instead (sub_82D181D0). Recognising that lets the PSO turn
  // the blend unit off rather than run an identity blend on every draw.
  const bool identity = (src_c == 1 && dst_c == 0 && op_c == 0);
  const bool identity_alpha = (src_a == 1 && dst_a == 0 && op_a == 0);
  out.blend_enable = !(identity && identity_alpha);

  // RB_COLORCONTROL: bits 0-2 ALPHA_FUNC, bit 3 ALPHA_TEST_ENABLE. The shaders
  // do the test themselves (XenosRecomp emits clip(oC0.w - g_AlphaThreshold)),
  // so only "on" and the reference value travel.
  out.alpha_test = (color & 0x8u) != 0;
  std::memcpy(&out.alpha_ref, &alpha_ref_bits, 4);
  if (!(out.alpha_ref >= 0.0f) || out.alpha_ref > 1.0f) {
    out.alpha_ref = 0.0f;  // never let a stale/NaN reference clip everything away
  }

  // PA_SU_SC_MODE_CNTL: bit0 CULL_FRONT, bit1 CULL_BACK, bit2 FACE (which
  // winding counts as front). D3DCULL_CW / D3DCULL_CCW are the bit patterns
  // themselves, which is why the XDK's cull setter (sub_82D18BF0) is a plain
  // "insert bits 0-5" with no translation.
  const bool cull_front = (raster & 0x1u) != 0;
  const bool cull_back = (raster & 0x2u) != 0;
  out.front_ccw = (raster & 0x4u) == 0;
  if (cull_front && cull_back) {
    out.cull_mode = D3D12_CULL_MODE_BACK;  // degenerate; keep something drawable
  } else if (cull_front) {
    out.cull_mode = D3D12_CULL_MODE_FRONT;
  } else if (cull_back) {
    out.cull_mode = D3D12_CULL_MODE_BACK;
  } else {
    out.cull_mode = D3D12_CULL_MODE_NONE;
  }

  for (std::uint32_t i = 0; i < 8; ++i) {
    out.bools[i] = LoadBE32(d + kBoolBank + 4 * i);
  }
  for (std::uint32_t i = 0; i < 32; ++i) {
    out.loops[i] = LoadBE32(d + kLoopBank + 4 * i);
  }
  if (out.bools[0] | out.bools[1] | out.bools[2] | out.bools[3] | out.bools[4] | out.bools[5] |
      out.bools[6] | out.bools[7]) {
    g_bool_nonzero.fetch_add(1, std::memory_order_relaxed);
  }
  Sample(g_sample_bools, out.bools[0]);
  Sample(g_sample_bools, out.bools[4]);
  Sample(g_sample_loops, out.loops[0]);
  Sample(g_sample_loops, out.loops[16]);
  // One raw dump of the whole inferred window, taken mid-session when the
  // scene is real. Floats read as 0x3F.. / 0xBF.. patterns, bools as sparse
  // bits, loop constants as small packed integers - one glance settles it.
  if (g_reads.load(std::memory_order_relaxed) == 20000 &&
      !g_window_dumped.exchange(true, std::memory_order_relaxed)) {
    char dump[1024];
    int p = 0;
    for (std::uint32_t off = kBoolBank; off < kBoolBank + 160 && p < 900; off += 4) {
      p += std::snprintf(dump + p, sizeof(dump) - p, " %08x", LoadBE32(d + off));
    }
    REXLOG_INFO("[native-state] bank window dev+{}..{}:{}", kBoolBank, kBoolBank + 160, dump);
  }
  return true;
}

bool ObservedInvertedDepth() {
  const std::uint64_t less = g_depth_less.load(std::memory_order_relaxed);
  const std::uint64_t greater = g_depth_greater.load(std::memory_order_relaxed);
  return (less + greater) >= 64 && greater > less;
}

bool InvertedDepth(const std::uint8_t* base) {
  if (base == nullptr || !SpanReadable(base + kGInvertZ, 4)) {
    return false;
  }
  return LoadBE32(base + kGInvertZ) != 0;
}

void LogStats() {
  char line[512];
  int n = std::snprintf(line, sizeof(line), "depth:");
  for (std::uint32_t i = 0; i < kSampleSlots; ++i) {
    const std::uint32_t v = g_sample_depth[i].load(std::memory_order_relaxed);
    if (v != 0 && n > 0 && n < 400) {
      n += std::snprintf(line + n, sizeof(line) - n, " %08x", v);
    }
  }
  if (n > 0 && n < 400) {
    n += std::snprintf(line + n, sizeof(line) - n, " | blend:");
  }
  for (std::uint32_t i = 0; i < kSampleSlots; ++i) {
    const std::uint32_t v = g_sample_blend[i].load(std::memory_order_relaxed);
    if (v != 0 && n > 0 && n < 440) {
      n += std::snprintf(line + n, sizeof(line) - n, " %08x", v);
    }
  }
  if (n > 0 && n < 440) {
    n += std::snprintf(line + n, sizeof(line) - n, " | raster:");
  }
  for (std::uint32_t i = 0; i < kSampleSlots; ++i) {
    const std::uint32_t v = g_sample_raster[i].load(std::memory_order_relaxed);
    if (v != 0 && n > 0 && n < 470) {
      n += std::snprintf(line + n, sizeof(line) - n, " %08x", v);
    }
  }
  if (n > 0 && n < 470) {
    n += std::snprintf(line + n, sizeof(line) - n, " | clip:");
  }
  for (std::uint32_t i = 0; i < kSampleSlots; ++i) {
    const std::uint32_t v = g_sample_clip[i].load(std::memory_order_relaxed);
    if (v != 0 && n > 0 && n < 500) {
      n += std::snprintf(line + n, sizeof(line) - n, " %08x", v);
    }
  }
  REXLOG_INFO("[native-state] colour write mask: {} full (RGBA), {} ZERO (depth-only), {} partial",
              g_cw_full.load(std::memory_order_relaxed), g_cw_zero.load(std::memory_order_relaxed),
              g_cw_other.load(std::memory_order_relaxed));
  REXLOG_INFO("[native-state] depth compares: {} less, {} greater -> {} depth",
              g_depth_less.load(std::memory_order_relaxed),
              g_depth_greater.load(std::memory_order_relaxed),
              ObservedInvertedDepth() ? "INVERTED" : "normal");
  REXLOG_INFO("[native-state] {} reads, {} without a device | {}",
              g_reads.load(std::memory_order_relaxed), g_no_device.load(std::memory_order_relaxed),
              line);
  char banks[256];
  int b = 0;
  for (std::uint32_t i = 0; i < kSampleSlots && b < 100; ++i) {
    const std::uint32_t v = g_sample_bools[i].load(std::memory_order_relaxed);
    if (v != 0) b += std::snprintf(banks + b, sizeof(banks) - b, " %08x", v);
  }
  b += std::snprintf(banks + b, sizeof(banks) - b, " | loops:");
  for (std::uint32_t i = 0; i < kSampleSlots && b < 220; ++i) {
    const std::uint32_t v = g_sample_loops[i].load(std::memory_order_relaxed);
    if (v != 0) b += std::snprintf(banks + b, sizeof(banks) - b, " %08x", v);
  }
  REXLOG_INFO("[native-state] bool bank nonzero on {} of {} reads | bools:{}",
              g_bool_nonzero.load(std::memory_order_relaxed),
              g_reads.load(std::memory_order_relaxed), banks);
}

}  // namespace dpour_state
// === END DPOUR MIGRATION 2026-07-25 ===
