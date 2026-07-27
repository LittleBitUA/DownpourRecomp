// downpour - Native Render: guest shader-container harvester (Phase 3 shaders)
//
// === DPOUR MIGRATION 2026-07-25: native-render shader harvest ===
//
// WHY: to render the game natively we need the game's own shaders as DXIL. Our
// XenosRecomp fork (E:\XboxDP\XenosRecomp) already translates Xenos shader
// containers -> HLSL -> DXIL and builds a cache keyed by a *normalized* ucode
// hash (normalized because the X360 D3D runtime rewrites vfetch fields at bind
// time). It scans files for containers — but Downpour's cooked UE3 packages are
// LZO-compressed on disk (magic 0xC1832A9E), so nothing is findable offline.
//
// The containers DO exist, decompressed, in guest RAM once a level is loaded.
// This module walks the guest address space, finds every Xenos shader container
// (ShaderContainer::flags & 0xFFFFFF00 == 0x102A1100, all fields big-endian —
// exactly the layout XenosRecomp expects), and writes each unique one to disk.
// Feed that directory to XenosRecomp to get the DXIL cache.
//
// Completely passive: read-only scanning, writes only to its own dump folder,
// and it is gated by env DPOUR_NR_DUMP_SHADERS (default OFF).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_shader_dump.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <rex/logging.h>

namespace dpour_shaders {
namespace {

constexpr std::uint32_t kGuestScanEnd = 0xC0000000u;
constexpr std::uint32_t kContainerMagic = 0x102A1100u;
constexpr std::uint32_t kMaxContainerBytes = 0x100000u;  // 1 MB, as XenosRecomp

std::unordered_set<std::uint64_t> g_seen;
std::uint64_t g_frames = 0;
std::uint32_t g_scans_done = 0;
std::uint64_t g_dumped = 0;

inline std::uint32_t BE32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// FNV-1a over the container bytes; only used to avoid dumping duplicates.
std::uint64_t HashBytes(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::filesystem::path DumpDir() {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::filesystem::path dir = std::filesystem::path(exe).parent_path() / "shader_dump";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// Validate a candidate the same way XenosRecomp's scanner does, so anything we
// dump is something it will actually accept.
bool ValidContainer(const std::uint8_t* p, std::size_t avail, std::uint32_t& size_out) {
  if (avail < 36) {
    return false;
  }
  const std::uint32_t flags = BE32(p + 0);
  if ((flags & 0xFFFFFF00u) != kContainerMagic) {
    return false;
  }
  const std::uint32_t virtual_size = BE32(p + 4);
  const std::uint32_t physical_size = BE32(p + 8);
  const std::uint32_t ct_off = BE32(p + 16);
  const std::uint32_t dt_off = BE32(p + 20);
  const std::uint32_t sh_off = BE32(p + 24);
  const std::uint32_t field1c = BE32(p + 28);
  const std::uint32_t field20 = BE32(p + 32);

  const std::uint64_t data_size =
      static_cast<std::uint64_t>(virtual_size) + static_cast<std::uint64_t>(physical_size);
  if (field1c != 0 || field20 != 0) {
    return false;
  }
  if (data_size > avail || data_size > kMaxContainerBytes || virtual_size < 36) {
    return false;
  }
  if (ct_off < 4 || ct_off + 8 > data_size) {
    return false;
  }
  if (dt_off != 0 && dt_off >= data_size) {
    return false;
  }
  if (sh_off < 36 || sh_off + 24 > data_size) {
    return false;
  }
  // Shader sub-header: physicalOffset @+0, size @+4 (both big-endian).
  const std::uint32_t sh_phys = BE32(p + sh_off + 0);
  const std::uint32_t sh_size = BE32(p + sh_off + 4);
  if (sh_size < 12) {
    return false;
  }
  if (static_cast<std::uint64_t>(virtual_size) + sh_phys + 12 > data_size) {
    return false;
  }
  size_out = static_cast<std::uint32_t>(data_size);
  return true;
}

void ScanOnce(const std::uint8_t* base) {
  const std::filesystem::path dir = DumpDir();
  std::uint64_t found_this_scan = 0;
  std::uint32_t addr = 0x1000;

  while (addr < kGuestScanEnd) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base + addr, &mbi, sizeof(mbi)) == 0) {
      break;
    }
    const auto region_base = reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress);
    const std::size_t region_size = mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
                          (mbi.Protect & PAGE_NOACCESS) == 0;
    if (readable && region_base >= base) {
      const std::size_t off_in_region =
          static_cast<std::size_t>((base + addr) - region_base);
      const std::uint8_t* p = base + addr;
      std::size_t remaining = region_size - off_in_region;
      // Scan 4-byte aligned words for the container magic.
      while (remaining >= 36) {
        if ((BE32(p) & 0xFFFFFF00u) == kContainerMagic) {
          std::uint32_t size = 0;
          if (ValidContainer(p, remaining, size)) {
            const std::uint64_t h = HashBytes(p, size);
            if (g_seen.insert(h).second) {
              char name[64];
              std::snprintf(name, sizeof(name), "%016llX.bin",
                            static_cast<unsigned long long>(h));
              const std::filesystem::path out = dir / name;
              if (FILE* f = _wfopen(out.wstring().c_str(), L"wb")) {
                std::fwrite(p, 1, size, f);
                std::fclose(f);
                ++g_dumped;
                ++found_this_scan;
              }
            }
            p += size;
            remaining -= size;
            continue;
          }
        }
        p += 4;
        remaining -= 4;
      }
    }
    const std::uint64_t next =
        static_cast<std::uint64_t>(addr) + (region_size - ((base + addr) - region_base));
    if (next <= addr) {
      break;
    }
    addr = (next >= kGuestScanEnd) ? kGuestScanEnd : static_cast<std::uint32_t>(next);
  }

  ++g_scans_done;
  REXLOG_INFO("[shader-dump] scan #{} complete: {} new containers ({} total) -> {}", g_scans_done,
              found_this_scan, g_dumped, dir.string());
}

// --- bound-shader-state harvest ---------------------------------------------
//
// Why the RAM scan is not enough (established from the game's own source,
// Development/Src/Xenon/XeD3DDrv/Src/XeD3DShaders.cpp):
//
//   FXeVertexShader::FXeVertexShader(const void* Code)
//     XGGetMicrocodeShaderParts(Code, &parts);
//     VertexShader = (IDirect3DVertexShader9*) new char[parts.cbCachedPartSize];
//     XGSetVertexShaderHeader(VertexShader, parts.cbCachedPartSize, &parts);
//     BaseAddress = appPhysicalAlloc(Align(parts.cbPhysicalPartSize, ...));
//     appMemcpy(BaseAddress, parts.pPhysicalPart, parts.cbPhysicalPartSize);
//     XGRegisterVertexShader(VertexShader, BaseAddress);
//
// So the container is split in two the moment it is created, and the source
// TArray<BYTE> is freed. The cached part IS the D3D shader object - it still
// begins with the container header (flags 0x102A11xx) and carries the constant
// table; the microcode lives in its own physical allocation. Stitching
// [cached part][physical part] back together reproduces exactly the container
// layout XenosRecomp parses.
//
// And the CLONE matters: RHICreateBoundShaderState does
//   NewCachedShader->VertexShader = CloneVertexShader(InVertexShader);
//   XeD3DVertexShader->Bind(0, InVertexDeclaration, InStreamStrides, ps);
// IDirect3DVertexShader9::Bind is what rewrites the vfetch instructions for the
// bound vertex declaration and stream strides. The original shader's microcode
// still carries the compiler's placeholders (const_index 31 / sel 2), so only
// the clone tells us the real vertex layout.

constexpr std::size_t kMaxShaderBytes = 0x40000u;  // 256 KB, far above any real one

std::unordered_map<std::uint32_t, std::uint64_t> g_obj_hash;  // guest object -> dump hash
std::unordered_set<std::uint64_t> g_seen_binds;

// Containers captured at construction time, keyed by the FXeVertexShader* /
// FXePixelShader*. The microcode inside is the UNPATCHED copy; the live,
// declaration-patched microcode is read back from BaseAddress at bind time.
std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> g_containers;
std::unordered_map<std::uint32_t, UcodeRange> g_ucode_range;
std::mutex g_shader_mutex;
std::uint64_t g_bss_dumped = 0;
std::uint64_t g_bss_calls = 0;
int g_resource_offset = -1;  // byte offset of FXeGPUResource::Resource, discovered once
FILE* g_manifest = nullptr;

inline bool AddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

// Every pointer here comes out of an arbitrary guest object field, so nothing
// may be dereferenced without checking it is actually committed - reading a
// plausible-looking but unmapped address took the game down on the first
// harvest run.
bool SpanReadable(const std::uint8_t* p, std::size_t n) {
  const std::uint8_t* end = p + n;
  const std::uint8_t* cur = p;
  for (int guard = 0; guard < 32 && cur < end; ++guard) {
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
  return cur >= end;
}

inline bool GuestReadable(const std::uint8_t* base, std::uint32_t addr, std::size_t n) {
  return AddrPlausible(addr) && SpanReadable(base + addr, n);
}

inline std::uint32_t ReadBE32(const std::uint8_t* base, std::uint32_t addr) {
  std::uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline std::uint32_t ReadBE32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// A candidate container header: 36 bytes, magic in the top 24 bits of flags,
// sane sizes. Same predicate XenosRecomp's scanner uses.
// Take the container recorded at construction time and splice in the microcode
// as it is RIGHT NOW - which for a bound clone is the version
// IDirect3DVertexShader9::Bind rewrote for the vertex declaration. Returns the
// dump hash, or 0 if nothing could be produced.
std::uint64_t DumpShaderObject(const std::uint8_t* base, std::uint32_t obj, const char* kind) {
  if (!AddrPlausible(obj) || !GuestReadable(base, obj, 20)) {
    return 0;
  }

  std::vector<std::uint8_t> buf;
  {
    std::lock_guard<std::mutex> lock(g_shader_mutex);
    auto it = g_containers.find(obj);
    if (it != g_containers.end()) {
      buf = it->second;
    }
  }
  if (buf.size() < 36) {
    // Created before the harvester was on, or the constructor hooks missed it.
    // There is nothing to rebuild from the object itself: the D3D shader object
    // does NOT carry the container header (verified against live memory).
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("[shader-harvest] {} obj {:#x}: no container recorded at creation", kind, obj);
    }
    return 0;
  }

  // FXeGPUResource: [vptr][vbptr][Resource][BaseAddress][UsageFlags] - the
  // layout both constructors write, confirmed against live objects.
  const std::uint32_t ucode_addr = ReadBE32(base, obj + 12);
  const std::uint32_t virtual_size = ReadBE32(buf.data() + 4);
  const std::uint32_t physical_size = ReadBE32(buf.data() + 8);
  if (static_cast<std::size_t>(virtual_size) + physical_size <= buf.size() &&
      GuestReadable(base, ucode_addr, physical_size)) {
    std::memcpy(buf.data() + virtual_size, base + ucode_addr, physical_size);
    buf.resize(static_cast<std::size_t>(virtual_size) + physical_size);
  }

  const std::uint64_t h = HashBytes(buf.data(), buf.size());
  if (!g_seen.insert(h).second) {
    return h;
  }
  const std::filesystem::path dir = DumpDir();
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(h));
  if (FILE* f = _wfopen((dir / name).wstring().c_str(), L"wb")) {
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    ++g_bss_dumped;
    ++g_dumped;
  }
  if (g_manifest != nullptr) {
    std::fprintf(g_manifest, "%s %016llX obj=%08X ucode=%08X size=%u\n", kind,
                 static_cast<unsigned long long>(h), obj, ucode_addr,
                 static_cast<unsigned>(buf.size()));
    std::fflush(g_manifest);
  }
  return h;
}

}  // namespace

void OnShaderCreated(const std::uint8_t* base, std::uint32_t shader_object,
                     std::uint32_t code_guest) {
  if (base == nullptr || !AddrPlausible(shader_object) || !GuestReadable(base, code_guest, 36)) {
    return;
  }
  const std::uint32_t flags = ReadBE32(base, code_guest + 0);
  if ((flags & 0xFFFFFF00u) != kContainerMagic) {
    return;
  }
  const std::uint32_t virtual_size = ReadBE32(base, code_guest + 4);
  const std::uint32_t physical_size = ReadBE32(base, code_guest + 8);
  const std::uint64_t total = static_cast<std::uint64_t>(virtual_size) + physical_size;
  if (virtual_size < 36 || physical_size < 12 || total > kMaxContainerBytes ||
      !GuestReadable(base, code_guest, static_cast<std::size_t>(total))) {
    return;
  }

  // The shader sub-header says where inside the physical part the microcode
  // starts and how long it is. XenosRecomp hashes exactly that range, so the
  // runtime has to record it here - BaseAddress alone is not enough.
  const std::uint32_t shader_offset = ReadBE32(base, code_guest + 24);
  UcodeRange range;
  if (shader_offset >= 36 && shader_offset + 8 <= virtual_size) {
    range.offset = ReadBE32(base, code_guest + shader_offset + 0);
    range.size = ReadBE32(base, code_guest + shader_offset + 4);
  }
  // The sub-header's size may legally overshoot the physical part, and the
  // offline side clamps it to what is actually there. Clamp identically, or the
  // two hash different numbers of bytes for exactly those shaders.
  if (range.offset < physical_size) {
    const std::uint32_t available = physical_size - range.offset;
    if (range.size > available) {
      range.size = available;
    }
  } else {
    range.offset = 0;
    range.size = physical_size;
  }
  if (range.size < 12) {
    range.offset = 0;
    range.size = physical_size;
  }

  std::lock_guard<std::mutex> lock(g_shader_mutex);
  g_ucode_range[shader_object] = range;
  if (DumpEnabled()) {
    auto& stored = g_containers[shader_object];
    stored.assign(base + code_guest, base + code_guest + total);
  }
}

bool UcodeRangeFor(std::uint32_t shader_object, UcodeRange& out) {
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_ucode_range.find(shader_object);
  if (it == g_ucode_range.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void OnBoundShaderState(const std::uint8_t* base, std::uint32_t bss) {
  if (!DumpEnabled() || base == nullptr || !AddrPlausible(bss)) {
    return;
  }
  ++g_bss_calls;
  if (!GuestReadable(base, bss, 16)) {
    return;
  }

  // FBoundShaderStateRHIRef (XeD3DShaders.h): VertexDeclaration @0,
  // FCachedVertexShader* @4, OriginalVertexShader @8, PixelShader @12.
  const std::uint32_t cached = ReadBE32(base, bss + 4);
  const std::uint32_t orig_vs = ReadBE32(base, bss + 8);
  const std::uint32_t ps = ReadBE32(base, bss + 12);

  // FCachedVertexShader { FVertexShaderRHIRef VertexShader; UINT CacheRefCount; }
  // and TXeGPUResourceRef is a single pointer, so +0 is the patched clone.
  const std::uint32_t patched_vs = GuestReadable(base, cached, 4) ? ReadBE32(base, cached + 0) : 0;

  if (g_manifest == nullptr) {
    const std::filesystem::path out = DumpDir() / "manifest.txt";
    g_manifest = _wfopen(out.wstring().c_str(), L"a");
  }

  // Dedupe on the guest object pointers first: the same bound state is set
  // thousands of times per frame and stitching is not free.
  auto hash_of = [&](std::uint32_t obj, const char* kind) -> std::uint64_t {
    if (obj == 0) {
      return 0;
    }
    auto it = g_obj_hash.find(obj);
    if (it != g_obj_hash.end()) {
      return it->second;
    }
    const std::uint64_t h = DumpShaderObject(base, obj, kind);
    g_obj_hash.emplace(obj, h);
    return h;
  };

  // Only the ORIGINAL vertex shader is harvested, not the declaration-patched
  // clone. On the UnleashedRecomp path the declaration is expressed by the host
  // input layout and the PSO key, so a shader is one object however many
  // declarations it is bound with - and the original's microcode is the one its
  // own vertex element table describes. Dumping clones as well produced a cache
  // entry per (shader x declaration) pair, which is what made a live run miss
  // 390 lookups against 53 hits.
  (void)patched_vs;
  const std::uint64_t h_orig = hash_of(orig_vs, "vs-orig");
  const std::uint64_t h_ps = hash_of(ps, "ps");

  // Record which (declaration, vertex shader, pixel shader) triples the game
  // actually binds. That is the PSO key on this path, so the manifest doubles
  // as the list of pipelines a level needs.
  const std::uint32_t decl = ReadBE32(base, bss + 0);
  const std::uint64_t bind_key = (std::uint64_t(decl) << 32) ^ (h_orig * 1099511628211ull) ^
                                 (h_ps * 1469598103934665603ull);
  if (g_seen_binds.insert(bind_key).second && g_manifest != nullptr) {
    std::fprintf(g_manifest, "bind decl=%08X vs=%016llX ps=%016llX\n", decl,
                 static_cast<unsigned long long>(h_orig),
                 static_cast<unsigned long long>(h_ps));
    std::fflush(g_manifest);
  }

  if ((g_bss_calls % 20000) == 0) {
    REXLOG_INFO("[shader-harvest] {} bound-state calls, {} unique shaders, {} containers written",
                g_bss_calls, g_obj_hash.size(), g_bss_dumped);
  }
}

bool DumpEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR_DUMP_SHADERS");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

// The brute-force RAM scan predates the constructor hooks and is now strictly
// worse than them: measured over a full harvest run it found 68 containers on
// the first pass (all of them XDK video-decoder shaders, not the game's) and
// zero on every pass after, while each pass walked all of guest memory on the
// render thread - the 685 ms, 230 ms and 125 ms frame spikes in that run line
// up exactly with its three timestamps. Off unless explicitly asked for.
bool RamScanEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DPOUR_NR_DUMP_RAMSCAN");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return DumpEnabled() && enabled;
}

void MaybeHarvest(const std::uint8_t* base) {
  if (!RamScanEnabled() || base == nullptr) {
    return;
  }
  const std::uint64_t frame = ++g_frames;
  // A few milestones: after boot, after the title settles, then periodically so
  // streamed-in level shaders get picked up too.
  const bool milestone = frame == 300 || frame == 1200 || (frame % 3600) == 0;
  if (!milestone) {
    return;
  }
  ScanOnce(base);
}

}  // namespace dpour_shaders
// === END DPOUR MIGRATION 2026-07-25 ===
