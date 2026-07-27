// downpour - Native Render: guest D3D vertex declaration reader
//
// === DPOUR MIGRATION 2026-07-25: native-render Phase 5 (all vertex formats) ===
//
// How the declaration is found: a D3DVERTEXELEMENT9 array always ends with
// D3DDECL_END, which on the big-endian guest is the byte pattern
//
//     00 FF 00 00 11 00 00 00
//     |Stream=0xFF| |Off=0| Type=UNUSED(17)
//
// That is distinctive enough to search for. From the terminator we walk
// backwards in 8-byte steps while the entries still look like elements, and
// accept the run when it starts with POSITION at stream 0. This keeps us
// independent of where exactly UE3's FXeVertexDeclaration / the D3D9 library
// choose to keep the array.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_decl.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include <rex/logging.h>

namespace dpour_decl {
namespace {

// D3DDECLUSAGE
enum : std::uint8_t {
  kUsagePosition = 0,
  kUsageBlendWeight = 1,
  kUsageBlendIndices = 2,
  kUsageNormal = 3,
  kUsageTexcoord = 5,
  kUsageTangent = 6,
  kUsageBinormal = 7,
  kUsageColor = 10,
};

inline bool GuestAddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

bool SpanReadable(const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  const std::uint8_t* e = b + n;
  const std::uint8_t* cur = b;
  for (int guard = 0; guard < 32 && cur < e; ++guard) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) {
      return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
      return false;
    }
    cur = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return cur >= e;
}

inline std::uint16_t LoadBE16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t LoadBE32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline float LoadBEFloat(const std::uint8_t* p) {
  const std::uint32_t v = LoadBE32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
float HalfToFloat(std::uint16_t h) {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1Fu;
  std::uint32_t man = h & 0x3FFu;
  std::uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {
      exp = 1;
      while ((man & 0x400u) == 0) {
        man <<= 1;
        --exp;
      }
      man &= 0x3FFu;
      bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp + 112u) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool ElementLooksSane(const std::uint8_t* e) {
  const std::uint16_t stream = LoadBE16(e + 0);
  const std::uint16_t offset = LoadBE16(e + 2);
  const std::uint8_t type = e[4];
  const std::uint8_t method = e[5];
  const std::uint8_t usage = e[6];
  const std::uint8_t usage_index = e[7];
  return stream < 8u && offset < 256u && type <= 60u && method <= 6u && usage <= 13u &&
         usage_index < 16u;
}

bool IsTerminator(const std::uint8_t* e) {
  return e[0] == 0x00 && e[1] == 0xFF && e[2] == 0x00 && e[3] == 0x00 && e[4] == kUnused &&
         e[5] == 0x00 && e[6] == 0x00 && e[7] == 0x00;
}

// Try to read an element array that ENDS at `end` (exclusive). Returns the
// number of elements, or 0.
std::uint32_t ParseBackwards(const std::uint8_t* end, Layout& out) {
  constexpr std::uint32_t kMaxElements = 24;
  std::uint32_t count = 0;
  const std::uint8_t* first = end;
  for (std::uint32_t i = 1; i <= kMaxElements; ++i) {
    const std::uint8_t* e = end - i * 8;
    if (!ElementLooksSane(e) || IsTerminator(e)) {
      break;
    }
    first = e;
    count = i;
  }
  if (count == 0) {
    return 0;
  }
  // A real declaration starts with POSITION in stream 0 at offset 0.
  if (LoadBE16(first + 0) != 0u || LoadBE16(first + 2) != 0u || first[6] != kUsagePosition) {
    return 0;
  }

  Layout layout;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* e = first + i * 8;
    Element elem;
    elem.stream = static_cast<std::uint8_t>(LoadBE16(e + 0));
    elem.offset = static_cast<std::uint8_t>(LoadBE16(e + 2));
    elem.type = e[4];
    const std::uint8_t usage = e[6];
    const std::uint8_t usage_index = e[7];
    switch (usage) {
      case kUsagePosition:
        if (usage_index == 0 && !layout.position.valid()) {
          layout.position = elem;
        }
        break;
      case kUsageNormal:
        if (usage_index == 0) {
          layout.normal = elem;
        }
        break;
      case kUsageTangent:
      case kUsageBinormal:
        if (!layout.normal.valid()) {
          layout.normal = elem;  // UE3 packs the basis; any axis beats none
        }
        break;
      case kUsageTexcoord:
        if (usage_index == 0 && !layout.texcoord.valid()) {
          layout.texcoord = elem;
        }
        break;
      case kUsageBlendIndices:
      case kUsageBlendWeight:
        layout.skinned = true;
        break;
      default:
        break;
    }
  }
  if (!layout.position.valid() || ElementSize(layout.position.type) == 0) {
    return 0;
  }
  layout.valid = true;
  out = layout;
  return count;
}

// Search a window of guest memory for a declaration terminator and parse the
// array that precedes it.
bool ScanRegion(const std::uint8_t* start, std::size_t bytes, Layout& out) {
  if (!SpanReadable(start, bytes)) {
    return false;
  }
  for (std::size_t off = 8; off + 8 <= bytes; off += 4) {
    const std::uint8_t* e = start + off;
    if (!IsTerminator(e)) {
      continue;
    }
    if (ParseBackwards(e, out) != 0) {
      return true;
    }
  }
  return false;
}

std::mutex g_mutex;
std::unordered_map<std::uint32_t, Layout> g_cache;
std::atomic<std::uint32_t> g_logged{0};

}  // namespace

std::uint32_t ElementSize(std::uint8_t type) {
  switch (type) {
    case kFloat1:
      return 4;
    case kFloat2:
      return 8;
    case kFloat3:
      return 12;
    case kFloat4:
      return 16;
    case kD3DColor:
    case kUByte4:
    case kUByte4N:
    case kShort2:
    case kShort2N:
    case kUShort2N:
    case kUDec3:
    case kDec3N:
    case kFloat16_2:
      return 4;
    case kShort4:
    case kShort4N:
    case kUShort4N:
    case kFloat16_4:
      return 8;
    default:
      return 0;
  }
}

void ReadElement(const std::uint8_t* p, std::uint8_t type, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  switch (type) {
    case kFloat4:
      out[3] = LoadBEFloat(p + 12);
      [[fallthrough]];
    case kFloat3:
      out[2] = LoadBEFloat(p + 8);
      [[fallthrough]];
    case kFloat2:
      out[1] = LoadBEFloat(p + 4);
      [[fallthrough]];
    case kFloat1:
      out[0] = LoadBEFloat(p + 0);
      break;
    case kFloat16_4:
      out[2] = HalfToFloat(LoadBE16(p + 4));
      out[3] = HalfToFloat(LoadBE16(p + 6));
      [[fallthrough]];
    case kFloat16_2:
      out[0] = HalfToFloat(LoadBE16(p + 0));
      out[1] = HalfToFloat(LoadBE16(p + 2));
      break;
    case kD3DColor:
    case kUByte4:
    case kUByte4N: {
      // Stored as a big-endian DWORD, so the component order reverses.
      const std::uint32_t v = LoadBE32(p);
      const float s = (type == kUByte4) ? 1.0f : (1.0f / 255.0f);
      out[0] = static_cast<float>((v >> 0) & 0xFF) * s;
      out[1] = static_cast<float>((v >> 8) & 0xFF) * s;
      out[2] = static_cast<float>((v >> 16) & 0xFF) * s;
      out[3] = static_cast<float>((v >> 24) & 0xFF) * s;
      break;
    }
    case kShort4:
    case kShort4N:
      out[2] = static_cast<float>(static_cast<std::int16_t>(LoadBE16(p + 4)));
      out[3] = static_cast<float>(static_cast<std::int16_t>(LoadBE16(p + 6)));
      [[fallthrough]];
    case kShort2:
    case kShort2N:
      out[0] = static_cast<float>(static_cast<std::int16_t>(LoadBE16(p + 0)));
      out[1] = static_cast<float>(static_cast<std::int16_t>(LoadBE16(p + 2)));
      if (type == kShort2N || type == kShort4N) {
        for (int i = 0; i < 4; ++i) {
          out[i] *= 1.0f / 32767.0f;
        }
      }
      break;
    case kUShort2N:
    case kUShort4N:
      out[0] = LoadBE16(p + 0) * (1.0f / 65535.0f);
      out[1] = LoadBE16(p + 2) * (1.0f / 65535.0f);
      if (type == kUShort4N) {
        out[2] = LoadBE16(p + 4) * (1.0f / 65535.0f);
        out[3] = LoadBE16(p + 6) * (1.0f / 65535.0f);
      }
      break;
    case kUDec3:
    case kDec3N: {
      const std::uint32_t v = LoadBE32(p);
      const std::uint32_t xs = (v >> 0) & 0x3FF;
      const std::uint32_t ys = (v >> 10) & 0x3FF;
      const std::uint32_t zs = (v >> 20) & 0x3FF;
      if (type == kUDec3) {
        out[0] = static_cast<float>(xs);
        out[1] = static_cast<float>(ys);
        out[2] = static_cast<float>(zs);
      } else {
        auto sn = [](std::uint32_t u) {
          const std::int32_t s = (u & 0x200) ? static_cast<std::int32_t>(u) - 1024 : static_cast<std::int32_t>(u);
          return static_cast<float>(s) / 511.0f;
        };
        out[0] = sn(xs);
        out[1] = sn(ys);
        out[2] = sn(zs);
      }
      break;
    }
    default:
      break;
  }
}

// === DPOUR MIGRATION 2026-07-25: declarations come from the RHI, not a scan ===
//
// RHICreateVertexDeclaration = sub_829D40F8 (confirmed against the game source:
// it builds an FVertexDeclarationKey on the stack from r4, looks it up in
// GVertexDeclarationCache and returns the IDirect3DVertexDeclaration9). The key
// constructor sub_829D3D28 reads StreamIndex at +0, Offset at +1 and switches
// on Type-1 over 12 cases - exactly FVertexElement and EVertexElementType.
void OnCreateDeclaration(const std::uint8_t* base, std::uint32_t decl_guest,
                         std::uint32_t elements_guest) {
  if (base == nullptr || !GuestAddrPlausible(decl_guest) || !GuestAddrPlausible(elements_guest)) {
    return;
  }
  // FVertexDeclarationElementList is a fixed array: 16 x FVertexElement (16
  // bytes) with the count stored right after it.
  if (!SpanReadable(base + elements_guest, 16 * 16 + 4)) {
    return;
  }
  const std::uint32_t count = LoadBE32(base + elements_guest + 16 * 16);
  if (count == 0 || count > kMaxElements) {
    return;
  }

  Layout layout;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* e = base + elements_guest + i * 16;
    FullElement fe;
    fe.stream = e[0];
    fe.offset = e[1];
    fe.type = e[2];
    fe.usage = e[3];
    fe.usage_index = e[4];
    layout.elements[layout.element_count++] = fe;
  }

  // The guest sorts by (Stream, Offset) before creating the declaration
  // (IMPLEMENT_COMPARE_CONSTREF in XeD3DVertexDeclaration.cpp), and that order
  // decides the fetch slots. Reproduce it exactly.
  std::sort(layout.elements, layout.elements + layout.element_count,
            [](const FullElement& a, const FullElement& b) {
              if (a.stream != b.stream) {
                return a.stream < b.stream;
              }
              return a.offset < b.offset;
            });

  // Fill in the convenience fields the geometry path uses, translating UE3's
  // element types to the D3DDECLTYPE values this module already understands.
  auto d3d_type = [](std::uint8_t vet) -> std::uint8_t {
    switch (vet) {
      case 1: return kFloat1;
      case 2: return kFloat2;
      case 3: return kFloat3;
      case 4: return kFloat4;
      case 5: return kUByte4;     // VET_PackedNormal
      case 6: return kUByte4;
      case 7: return kUByte4N;
      case 8: return kD3DColor;
      case 9: return kShort2;
      case 10: return kShort2N;
      case 11: return kFloat16_2;  // VET_Half2
      case 12: return kUDec3;      // VET_Pos3N -> HEND3N, closest we decode
      default: return kUnused;
    }
  };

  for (std::uint32_t i = 0; i < layout.element_count; ++i) {
    const FullElement& fe = layout.elements[i];
    Element el;
    el.stream = fe.stream;
    el.offset = fe.offset;
    el.type = d3d_type(fe.type);
    switch (fe.usage) {
      case 0:  // VEU_Position
        if (!layout.position.valid()) layout.position = el;
        break;
      case 1:  // VEU_TextureCoordinate
        if (fe.usage_index == 0 && !layout.texcoord.valid()) layout.texcoord = el;
        break;
      case 3:  // VEU_BlendIndices
        layout.skinned = true;
        break;
      case 4:  // VEU_Normal
        layout.normal = el;
        break;
      case 5:  // VEU_Tangent
      case 6:  // VEU_Binormal
        if (!layout.normal.valid()) layout.normal = el;
        break;
      default:
        break;
    }
  }
  layout.valid = layout.position.valid();

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache[decl_guest] = layout;
  }

  const std::uint32_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
  if (n < 24) {
    char line[512];
    int w = std::snprintf(line, sizeof(line), "decl %#x n=%u:", decl_guest, layout.element_count);
    for (std::uint32_t i = 0; i < layout.element_count && w > 0 && w < 460; ++i) {
      const FullElement& fe = layout.elements[i];
      w += std::snprintf(line + w, sizeof(line) - w, " [%u]s%u+%u t%u u%u.%u", i, fe.stream,
                         fe.offset, fe.type, fe.usage, fe.usage_index);
    }
    REXLOG_INFO("[native-decl] {}", line);
  }
}

std::uint32_t RegisteredCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<std::uint32_t>(g_cache.size());
}

const Layout& Resolve(const std::uint8_t* base, std::uint32_t decl_guest) {
  static const Layout kInvalid;
  if (base == nullptr || !GuestAddrPlausible(decl_guest)) {
    return kInvalid;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(decl_guest);
    if (it != g_cache.end()) {
      return it->second;
    }
  }

  Layout layout;
  bool found = ScanRegion(base + decl_guest, 0x200, layout);
  if (!found) {
    // The array is usually behind a pointer in the wrapper object.
    for (std::uint32_t off = 0; off <= 0x40 && !found; off += 4) {
      const std::uint8_t* pp = base + decl_guest + off;
      if (!SpanReadable(pp, 4)) {
        continue;
      }
      const std::uint32_t target = LoadBE32(pp);
      if (!GuestAddrPlausible(target)) {
        continue;
      }
      found = ScanRegion(base + target, 0x200, layout);
    }
  }

  if (!found) {
    // Dump what IS at the declaration so the search can be corrected instead of
    // guessed at.
    const std::uint32_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 && SpanReadable(base + decl_guest, 96)) {
      const std::uint8_t* d = base + decl_guest;
      char line[512];
      int w = std::snprintf(line, sizeof(line), "decl %#x NOT PARSED, bytes:", decl_guest);
      for (int i = 0; i < 96 && w > 0 && w < static_cast<int>(sizeof(line)) - 6; ++i) {
        w += std::snprintf(line + w, sizeof(line) - w, "%s%02X", (i % 8) == 0 ? " " : "", d[i]);
      }
      REXLOG_INFO("[native-decl] {}", line);
    }
  }

  if (found) {
    const std::uint32_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < 12) {
      REXLOG_INFO(
          "[native-decl] decl {:#x}: pos s{} +{} t{} | nrm s{} +{} t{} | uv s{} +{} t{} | "
          "skinned={}",
          decl_guest, layout.position.stream, layout.position.offset, layout.position.type,
          layout.normal.stream, layout.normal.offset, layout.normal.type, layout.texcoord.stream,
          layout.texcoord.offset, layout.texcoord.type, layout.skinned);
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto [it, inserted] = g_cache.emplace(decl_guest, layout);
  return it->second;
}

}  // namespace dpour_decl
// === END DPOUR MIGRATION 2026-07-25 ===
