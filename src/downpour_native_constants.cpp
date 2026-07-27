// downpour - Native Render: the guest's shader constant banks
//
// === DPOUR MIGRATION 2026-07-25: constants for the game's own shaders ===
// See downpour_native_constants.h for how the two setters were identified.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_constants.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <windows.h>

#include <rex/logging.h>

namespace dpour_consts {
namespace {

struct Bank {
  float values[kRegisters][4]{};
  std::atomic<std::uint64_t> revision{0};
  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint32_t> high_water{0};  // highest register written + 1
  std::mutex mutex;
};

Bank g_vertex;
Bank g_pixel;
std::atomic<std::uint64_t> g_rejected{0};

inline bool AddrPlausible(std::uint32_t a) { return a >= 0x1000u && a < 0xC0000000u; }

bool SpanReadable(const std::uint8_t* p, std::size_t n) {
  const std::uint8_t* end = p + n;
  const std::uint8_t* cur = p;
  for (int guard = 0; guard < 16 && cur < end; ++guard) {
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

// VirtualQuery is a system call, and this function is the hottest hook in the
// whole renderer: the game sets shader parameters hundreds of thousands of times
// a session, several times per draw. Probing the page on every one of them cost
// far more than everything the native path does put together, and it did not
// show up in the draw path's own timing because it happens outside it.
//
// UE3 hands the same few staging buffers to the RHI over and over
// (GetPaddedShaderParameterValue), so remembering the answer per address makes
// this free after the first call. Both threads that can reach here are guest
// render threads, so the memo is per-thread and needs no lock.
bool SpanReadableMemo(const std::uint8_t* p, std::size_t n) {
  struct Entry {
    const std::uint8_t* base;
    std::size_t size;
    bool ok;
  };
  static thread_local Entry last{nullptr, 0, false};
  if (p == last.base && n <= last.size) {
    return last.ok;
  }
  const bool ok = SpanReadable(p, n);
  last = Entry{p, n, ok};
  return ok;
}

inline float LoadBEFloat(const std::uint8_t* p) {
  std::uint32_t bits;
  std::memcpy(&bits, p, 4);
  bits = _byteswap_ulong(bits);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void Store(Bank& bank, const std::uint8_t* base, std::uint32_t start_register,
           std::uint32_t data_guest, std::uint32_t vec4_count) {
  if (base == nullptr || vec4_count == 0 || start_register >= kRegisters ||
      !AddrPlausible(data_guest)) {
    return;
  }
  // A write that runs off the end of the bank is clamped rather than dropped:
  // the guest is allowed to set a range that the shader only partly reads.
  const std::uint32_t count = (start_register + vec4_count > kRegisters)
                                  ? (kRegisters - start_register)
                                  : vec4_count;
  const std::uint8_t* p = base + data_guest;
  if (!SpanReadableMemo(p, static_cast<std::size_t>(count) * 16)) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(bank.mutex);
    for (std::uint32_t r = 0; r < count; ++r) {
      for (std::uint32_t c = 0; c < 4; ++c) {
        bank.values[start_register + r][c] = LoadBEFloat(p + (r * 4 + c) * 4);
      }
    }
  }

  const std::uint32_t top = start_register + count;
  std::uint32_t seen = bank.high_water.load(std::memory_order_relaxed);
  while (top > seen && !bank.high_water.compare_exchange_weak(seen, top)) {
  }
  bank.writes.fetch_add(1, std::memory_order_relaxed);
  bank.revision.fetch_add(1, std::memory_order_relaxed);
}

void Copy(Bank& bank, void* dst, std::uint32_t bytes) {
  if (dst == nullptr || bytes == 0) {
    return;
  }
  if (bytes > kBankBytes) {
    bytes = kBankBytes;
  }
  std::lock_guard<std::mutex> lock(bank.mutex);
  std::memcpy(dst, bank.values, bytes);
}

std::uint32_t BankBytes(const Bank& bank) {
  const std::uint32_t regs = bank.high_water.load(std::memory_order_relaxed);
  return (regs >= kRegisters ? kRegisters : regs) * 16u;
}

}  // namespace

void SetVertexConstants(const std::uint8_t* base, std::uint32_t start_register,
                        std::uint32_t data_guest, std::uint32_t vec4_count) {
  Store(g_vertex, base, start_register, data_guest, vec4_count);
}

void SetPixelConstants(const std::uint8_t* base, std::uint32_t start_register,
                       std::uint32_t data_guest, std::uint32_t vec4_count) {
  Store(g_pixel, base, start_register, data_guest, vec4_count);
}

void CopyVertexBank(void* dst, std::uint32_t bytes) { Copy(g_vertex, dst, bytes); }
void CopyPixelBank(void* dst, std::uint32_t bytes) { Copy(g_pixel, dst, bytes); }

std::uint32_t VertexBankBytes() { return BankBytes(g_vertex); }
std::uint32_t PixelBankBytes() { return BankBytes(g_pixel); }

std::uint64_t VertexRevision() { return g_vertex.revision.load(std::memory_order_relaxed); }
std::uint64_t PixelRevision() { return g_pixel.revision.load(std::memory_order_relaxed); }

namespace {
// GDirect3DDevice - the master key for everything the XDK inlines into the
// device (see reference_downpour_rhi_address_map).
constexpr std::uint32_t kGDirect3DDevice = 0x83790010u;
constexpr std::uint32_t kDeviceVsFloatBase = 1920u;
constexpr std::uint32_t kDevicePsFloatBase = 6016u;

const std::uint8_t* DeviceBank(const std::uint8_t* base, std::uint32_t offset) {
  if (base == nullptr) {
    return nullptr;
  }
  std::uint32_t raw;
  std::memcpy(&raw, base + kGDirect3DDevice, 4);
  const std::uint32_t device = _byteswap_ulong(raw);
  if (device < 0x1000u || device >= 0xC0000000u) {
    return nullptr;
  }
  return base + device + offset;
}
}  // namespace

bool DeviceBanksEnabled() {
  static const bool hook_only = [] {
    const char* v = std::getenv("DPOUR_NR_CONSTS_HOOKONLY");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return !hook_only;
}

const std::uint8_t* DeviceVertexBank(const std::uint8_t* base) {
  return DeviceBank(base, kDeviceVsFloatBase);
}

const std::uint8_t* DevicePixelBank(const std::uint8_t* base) {
  return DeviceBank(base, kDevicePsFloatBase);
}

void LogStats() {
  REXLOG_INFO("[native-consts] vs {} writes (top register {}), ps {} writes (top {}), {} rejected",
              g_vertex.writes.load(std::memory_order_relaxed),
              g_vertex.high_water.load(std::memory_order_relaxed),
              g_pixel.writes.load(std::memory_order_relaxed),
              g_pixel.high_water.load(std::memory_order_relaxed),
              g_rejected.load(std::memory_order_relaxed));
}

}  // namespace dpour_consts
// === END DPOUR MIGRATION 2026-07-25 ===
