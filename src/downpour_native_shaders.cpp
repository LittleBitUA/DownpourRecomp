// downpour - Native Render: translated-shader cache
//
// === DPOUR MIGRATION 2026-07-25: real game shaders, not an approximation ===
// See downpour_native_shaders.h for the pipeline this is the runtime half of.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_shaders.h"

#include "downpour_shader_dump.h"  // microcode sizes recorded at shader creation

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <rex/logging.h>

namespace dpour_shadercache {
namespace {

// Must match the writer in XenosRecomp/main.cpp.
#pragma pack(push, 1)
struct IdxEntry {
  std::uint64_t container_hash;
  std::uint64_t raw_ucode_hash;
  std::uint32_t dxil_offset;
  std::uint32_t dxil_size;
  std::uint32_t spec_constants;
  std::uint32_t flags;  // bit0 = pixel shader
  std::uint32_t fetch_offset;
  std::uint32_t fetch_count;
};
#pragma pack(pop)
static_assert(sizeof(IdxEntry) == 40, "index entry layout must match XenosRecomp");
static_assert(sizeof(VertexFetchDesc) == 16, "fetch record layout must match XenosRecomp");

std::vector<std::uint8_t> g_dxil;
std::vector<VertexFetchDesc> g_fetches;
std::unordered_map<std::uint64_t, Shader> g_by_ucode;
std::unordered_map<std::uint32_t, const Shader*> g_by_object;
std::mutex g_mutex;
bool g_loaded = false;
bool g_load_attempted = false;

// Split by stage: a miss means something different for each. Vertex misses
// point at the harvest (is the cache keyed on what we look up?), pixel misses
// at coverage (was that material ever drawn while harvesting?).
std::atomic<std::uint64_t> g_hits_vs{0};
std::atomic<std::uint64_t> g_hits_ps{0};
std::atomic<std::uint64_t> g_misses{0};
std::atomic<std::uint64_t> g_unreadable{0};

inline bool AddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

// The pointers walked below come out of guest object fields, so a plausible
// range check is not enough - the page has to actually be committed.
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

// The same trivial hash XenosRecomp uses, so there is no library to keep in
// sync between the two halves.
std::uint64_t Fnv1a(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::filesystem::path ExeDir() {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path();
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
  FILE* f = _wfopen(path.wstring().c_str(), L"rb");
  if (f == nullptr) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return read == out.size();
}

}  // namespace

bool Load() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_load_attempted) {
    return g_loaded;
  }
  g_load_attempted = true;

  // Default OFF. Loading the cache is invisible on its own - it only makes the
  // translated shaders available - so DPOUR_NR_SHADERS stays the flag for that,
  // and DPOUR_NR_DRAW (which does change the picture) implies it.
  auto env_on = [](const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  };
  if (!env_on("DPOUR_NR_SHADERS") && !env_on("DPOUR_NR_DRAW")) {
    return false;
  }

  const std::filesystem::path dir = ExeDir();
  std::vector<std::uint8_t> idx;
  if (!ReadFileBytes(dir / "dpour_shaders.idx", idx) ||
      !ReadFileBytes(dir / "dpour_shaders.bin", g_dxil)) {
    REXLOG_INFO("[shader-cache] no dpour_shaders.idx/.bin next to the exe - the native "
                "renderer will keep using its built-in shader");
    return false;
  }
  // Format 2 added the per-shader vertex fetch table. The magic is versioned so
  // an older file is rejected outright rather than read at the wrong stride.
  if (idx.size() < 16 || std::memcmp(idx.data(), "DPXRSHD2", 8) != 0) {
    REXLOG_INFO("[shader-cache] dpour_shaders.idx has a bad or outdated header "
                "(rebuild it with BUILD_SHADER_CACHE.bat)");
    return false;
  }

  std::uint32_t count = 0;
  std::uint32_t fetch_count = 0;
  std::memcpy(&count, idx.data() + 8, 4);
  std::memcpy(&fetch_count, idx.data() + 12, 4);
  const std::size_t entries_bytes = static_cast<std::size_t>(count) * sizeof(IdxEntry);
  const std::size_t fetch_bytes = static_cast<std::size_t>(fetch_count) * sizeof(VertexFetchDesc);
  if (16 + entries_bytes + fetch_bytes > idx.size()) {
    REXLOG_INFO("[shader-cache] dpour_shaders.idx is truncated ({} entries, {} fetch records)",
                count, fetch_count);
    return false;
  }

  g_fetches.resize(fetch_count);
  if (fetch_count != 0) {
    std::memcpy(g_fetches.data(), idx.data() + 16 + entries_bytes, fetch_bytes);
  }

  std::uint32_t vs = 0, ps = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    IdxEntry e{};
    std::memcpy(&e, idx.data() + 16 + i * sizeof(IdxEntry), sizeof(IdxEntry));
    if (e.dxil_size == 0 ||
        static_cast<std::size_t>(e.dxil_offset) + e.dxil_size > g_dxil.size()) {
      continue;
    }
    Shader s;
    s.dxil = g_dxil.data() + e.dxil_offset;
    s.dxil_size = e.dxil_size;
    s.spec_constants = e.spec_constants;
    s.pixel_shader = (e.flags & 1u) != 0;
    s.ucode_hash = e.raw_ucode_hash;
    if (static_cast<std::size_t>(e.fetch_offset) + e.fetch_count <= g_fetches.size()) {
      s.fetches = g_fetches.data() + e.fetch_offset;
      s.fetch_count = e.fetch_count;
    }
    g_by_ucode[e.raw_ucode_hash] = s;
    (s.pixel_shader ? ps : vs)++;
  }

  g_loaded = !g_by_ucode.empty();
  REXLOG_INFO("[shader-cache] loaded {} translated shaders ({} vs, {} ps, {} KB of DXIL, "
              "{} fetch slots)",
              g_by_ucode.size(), vs, ps, g_dxil.size() / 1024, fetch_count);
  return g_loaded;
}

bool Available() {
  // Loaded on first use: there is no init order to depend on, and the answer
  // never changes afterwards, so the mutex is taken exactly once.
  static const bool available = Load();
  return available;
}

const Shader* Find(std::uint64_t ucode_hash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_by_ucode.find(ucode_hash);
  return it == g_by_ucode.end() ? nullptr : &it->second;
}

const Shader* ForGuestShader(const std::uint8_t* base, std::uint32_t guest_object) {
  if (!g_loaded || base == nullptr || !AddrPlausible(guest_object)) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_by_object.find(guest_object);
    if (it != g_by_object.end()) {
      return it->second;
    }
  }

  // FXeGPUResource keeps the D3D object (whose bytes still begin with the
  // container header) in `Resource`, and the microcode in `BaseAddress` right
  // after it. Virtual inheritance makes the offset compiler-dependent, so find
  // it the same self-verifying way the harvester does.
  const Shader* found = nullptr;
  bool resolved_object = false;
  if (!GuestReadable(base, guest_object, 72)) {
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_by_object[guest_object] = nullptr;
    return nullptr;
  }
  // FXeGPUResource is [vptr][vbptr][Resource][BaseAddress][UsageFlags] - taken
  // from the constructors (sub_829CFB50 / sub_829CFD30) and confirmed against
  // live objects. The microcode size comes from the container the constructor
  // hook recorded; the D3D object itself does not carry it.
  const std::uint32_t ucode_addr = ReadBE32(base, guest_object + 12);
  dpour_shaders::UcodeRange range;
  const bool have_range = dpour_shaders::UcodeRangeFor(guest_object, range);
  const std::uint32_t hash_addr = ucode_addr + range.offset;
  const std::uint32_t ucode_size = range.size;
  if (have_range && ucode_size >= 12 && GuestReadable(base, hash_addr, ucode_size)) {
    found = Find(Fnv1a(base + hash_addr, ucode_size));
    resolved_object = true;
    if (found == nullptr) {
      g_misses.fetch_add(1, std::memory_order_relaxed);
    } else {
      (found->pixel_shader ? g_hits_ps : g_hits_vs).fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (!resolved_object) {
    // The object did not look like an FXeGPUResource at all - worth knowing,
    // because it means the field walk needs revisiting, not that a shader is
    // missing from the cache.
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  g_by_object[guest_object] = found;
  return found;
}

void ReportStats() {
  if (!g_loaded) {
    return;
  }
  static std::uint64_t last_total = 0;
  const std::uint64_t vs = g_hits_vs.load(std::memory_order_relaxed);
  const std::uint64_t ps = g_hits_ps.load(std::memory_order_relaxed);
  const std::uint64_t misses = g_misses.load(std::memory_order_relaxed);
  const std::uint64_t total = vs + ps + misses;
  if (total == last_total) {
    return;
  }
  last_total = total;
  REXLOG_INFO("[shader-cache] resolved {} vs + {} ps, {} not in the cache, {} objects unread", vs,
              ps, misses, g_unreadable.load(std::memory_order_relaxed));
}

}  // namespace dpour_shadercache
// === END DPOUR MIGRATION 2026-07-25 ===
