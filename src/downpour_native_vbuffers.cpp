// downpour - Native Render: host vertex / index buffers
//
// === DPOUR MIGRATION 2026-07-25: the UnleashedRecomp buffer model ===
// See downpour_native_vbuffers.h for why the swap happens here and not at an
// Unlock hook, and why buffers are cached per allocation rather than per range.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_vbuffers.h"

#include <atomic>
#include <chrono>
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

namespace dpour_vbuf {
namespace {

constexpr std::uint64_t kMaxResidentBytes = 768ull << 20;
constexpr std::uint32_t kMaxSingleBuffer = 48u << 20;
constexpr double kUploadBudgetMs = 3.0;
constexpr std::uint32_t kGrowGranularity = 64u << 10;
// Guest resource usage bits (Engine/Inc/RHI.h:473-485).
constexpr std::uint32_t kRufAnyDynamic = 0x06;
// A buffer the game repacks needs one host copy per frame in flight, so a frame
// still being read by the GPU is never overwritten.
constexpr std::uint32_t kDynamicSlots = 3;

struct Buffer {
  ComPtr<ID3D12Resource> slot[kDynamicSlots];
  std::uint64_t gpu[kDynamicSlots] = {};
  std::uint32_t size = 0;
  bool dynamic = false;
  std::uint64_t frame_uploaded = 0;  // last frame any slot was refreshed
  std::uint32_t current = 0;
  bool live = false;
  // A buffer whose guest memory cannot be read is not going to become readable,
  // and re-probing it costs a burst of VirtualQuery calls on EVERY draw that
  // references it, every frame. Giving up on it after a few tries is what keeps
  // an unreachable buffer from eating the frame - the same negative cache the
  // geometry path needs for the same reason.
  std::uint32_t failures = 0;
};
constexpr std::uint32_t kMaxFailures = 3;

struct Pending {
  ComPtr<ID3D12Resource> staging;
  ComPtr<ID3D12Resource> dest;
  std::uint32_t size = 0;
  bool is_index = false;
  // A buffer created for this copy is already in COPY_DEST; one being refreshed
  // sits in its read state and has to be transitioned back first. Transitioning
  // a resource into the state it is already in is a debug-layer error, so the
  // two cases must not be merged.
  bool first_upload = false;
};

struct Retiring {
  ComPtr<ID3D12Resource> resource;
  std::uint64_t fence = 0;
};

std::mutex g_mutex;
std::unordered_map<std::uint64_t, Buffer> g_buffers;
std::vector<Pending> g_pending;
std::deque<Retiring> g_retiring;

std::uint64_t g_bytes = 0;
std::uint64_t g_frame = 0;
std::atomic<std::uint64_t> g_uploaded{0};
std::atomic<std::uint64_t> g_refreshed{0};
std::atomic<std::uint64_t> g_rejected{0};
std::atomic<std::uint64_t> g_deferred{0};
// Buffers dropped because the guest freed their memory (AddUnusedXeResource).
std::atomic<std::uint64_t> g_invalidated{0};
double g_frame_budget_ms = 0.0;

// Why AcquireBuffer said no - one counter per exit, plus a census of the top
// offenders. The same method that cracked "vb object": the aggregate counter
// said 10520 per interval and named nothing.
enum : std::uint8_t {
  kAcqFailArgs = 1,     // need_bytes 0 / over kMaxSingleBuffer / bad pointer
  kAcqFailLatched = 2,  // b.failures reached kMaxFailures earlier - permanent
  kAcqFailBudget = 3,   // frame's upload budget spent and no live copy to serve
  kAcqFailSpan = 4,     // guest pages not committed at upload time
  kAcqFailCap = 5,      // kMaxResidentBytes exceeded (also latches)
  kAcqFailAlloc = 6,    // CreateBuffer failed
  kAcqFailStage = 7,    // StageCopy failed
};
std::atomic<std::uint64_t> g_fail_reason[8] = {};

struct AcqFailEntry {
  std::uint32_t addr = 0;
  std::uint32_t need = 0;
  std::uint8_t reason = 0;
  std::uint8_t failures = 0;
  bool is_index = false;
  std::uint64_t count = 0;
};
AcqFailEntry g_acqfail[12];  // guarded by g_mutex, like g_buffers
// For re-probing census entries at print time: the guest base is one constant
// mapping per process, remembered here so LogStats can ask "is this latched
// buffer's memory readable NOW?" - which is the direct test of whether the
// failure latch outlived the condition that set it.
const std::uint8_t* g_last_base = nullptr;

// Callers other than the args gate hold g_mutex already.
void NoteAcqFail(std::uint32_t addr, std::uint32_t need, bool is_index, std::uint8_t reason,
                 std::uint32_t failures) {
  g_fail_reason[reason < 8 ? reason : 0].fetch_add(1, std::memory_order_relaxed);
  AcqFailEntry* slot = nullptr;
  for (auto& e : g_acqfail) {
    if (e.count != 0 && e.addr == addr && e.is_index == is_index) {
      slot = &e;
      break;
    }
    if (slot == nullptr && e.count == 0) {
      slot = &e;
    }
  }
  if (slot == nullptr) {
    return;
  }
  slot->addr = addr;
  slot->need = need;
  slot->reason = reason;
  slot->failures = static_cast<std::uint8_t>(failures < 255 ? failures : 255);
  slot->is_index = is_index;
  ++slot->count;
}

// Guest pointers come out of D3D resource fields, so nothing may be read without
// confirming the pages are committed.
bool SpanReadable(const std::uint8_t* p, std::size_t n) {
  const std::uint8_t* end = p + n;
  const std::uint8_t* cur = p;
  for (int guard = 0; guard < 256 && cur < end; ++guard) {
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

// `preferred` bytes from `p`, trimmed to what is actually committed, but never
// below `minimum`. One VirtualQuery, so the rounding-up of a buffer size cannot
// walk off the end of a guest allocation that ends mid-region.
std::uint32_t CommittedExtent(const std::uint8_t* p, std::uint32_t minimum,
                              std::uint32_t preferred) {
  if (preferred <= minimum) {
    return minimum;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return minimum;
  }
  const auto* region_end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  const std::uint64_t avail = static_cast<std::uint64_t>(region_end - p);
  if (avail >= preferred) {
    return preferred;
  }
  return avail > minimum ? static_cast<std::uint32_t>(avail) : minimum;
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, std::uint32_t size,
                                    D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
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
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&res)))) {
    return nullptr;
  }
  // Named so a GPU page fault names its victim: DRED reports the faulting
  // allocation by object name, and unnamed resources reduced the whole
  // post-mortem to a bare virtual address.
  res->SetName(heap_type == D3D12_HEAP_TYPE_UPLOAD ? L"dpour.upload" : L"dpour.geometry");
  return res;
}

std::uint64_t MakeKey(std::uint32_t addr, bool is_index) {
  return (static_cast<std::uint64_t>(addr) << 1) | (is_index ? 1ull : 0ull);
}

// The one place the endian conversion happens, mirroring UnlockBuffer<T>.
void SwapCopy32(void* dst, const std::uint8_t* src, std::uint32_t bytes) {
  auto* d = static_cast<std::uint32_t*>(dst);
  const std::uint32_t words = bytes / 4;
  for (std::uint32_t i = 0; i < words; ++i) {
    std::uint32_t v;
    std::memcpy(&v, src + i * 4, 4);
    d[i] = _byteswap_ulong(v);
  }
  const std::uint32_t tail = bytes - words * 4;
  if (tail != 0) {
    std::memcpy(d + words, src + words * 4, tail);
  }
}

void SwapCopy16(void* dst, const std::uint8_t* src, std::uint32_t bytes) {
  auto* d = static_cast<std::uint16_t*>(dst);
  const std::uint32_t items = bytes / 2;
  for (std::uint32_t i = 0; i < items; ++i) {
    std::uint16_t v;
    std::memcpy(&v, src + i * 2, 2);
    d[i] = _byteswap_ushort(v);
  }
}

// Stages `size` bytes of guest data into a fresh upload buffer and queues the
// copy into `dest`. Caller holds g_mutex.
bool StageCopy(ID3D12Device* device, const std::uint8_t* src, ID3D12Resource* dest,
               std::uint32_t size, bool is_index, bool first_upload) {
  ComPtr<ID3D12Resource> staging =
      CreateBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!staging) {
    return false;
  }
  void* mapped = nullptr;
  const D3D12_RANGE no_read{0, 0};
  if (FAILED(staging->Map(0, &no_read, &mapped)) || mapped == nullptr) {
    return false;
  }
  if (is_index) {
    SwapCopy16(mapped, src, size);
  } else {
    SwapCopy32(mapped, src, size);
  }
  staging->Unmap(0, nullptr);

  Pending p;
  p.staging = std::move(staging);
  p.dest = dest;
  p.size = size;
  p.is_index = is_index;
  p.first_upload = first_upload;
  g_pending.push_back(std::move(p));
  return true;
}

}  // namespace

bool AcquireBuffer(ID3D12Device* device, const std::uint8_t* base, std::uint32_t data_guest,
                   std::uint32_t need_bytes, bool is_index, std::uint32_t usage_flags, View& out) {
  out = View{};
  if (device == nullptr || base == nullptr || data_guest < 0x1000u || need_bytes == 0 ||
      need_bytes > kMaxSingleBuffer) {
    g_fail_reason[kAcqFailArgs].fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const bool dynamic = (usage_flags & kRufAnyDynamic) != 0;
  const std::uint64_t key = MakeKey(data_guest, is_index);

  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_base = base;
  Buffer& b = g_buffers[key];
  if (b.failures >= kMaxFailures) {
    NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailLatched, b.failures);
    return false;
  }

  const bool needs_grow = !b.live || b.size < need_bytes;
  const bool needs_refresh = b.live && b.dynamic && b.frame_uploaded != g_frame;
  if (b.live && !needs_grow && !needs_refresh) {
    out.gpu_address = b.gpu[b.current];
    out.size_bytes = b.size;
    return true;
  }

  if (g_frame_budget_ms >= kUploadBudgetMs) {
    g_deferred.fetch_add(1, std::memory_order_relaxed);
    if (b.live) {  // stale is better than nothing: keep drawing last frame's copy
      out.gpu_address = b.gpu[b.current];
      out.size_bytes = b.size;
      return true;
    }
    NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailBudget, b.failures);
    return false;
  }

  // Never round up past what is actually committed: the rounding is ours, and a
  // guest allocation can end mid-page.
  const std::uint32_t rounded = (need_bytes + kGrowGranularity - 1) & ~(kGrowGranularity - 1);
  const std::uint32_t want =
      needs_grow ? CommittedExtent(base + data_guest, need_bytes, rounded) : b.size;
  // The tail may run past the allocation when the size was rounded up, so the
  // exact request is the fallback. Both answers come from one pair of probes -
  // asking three times (as this used to) tripled the syscall cost of the whole
  // renderer's hottest path.
  const bool whole_readable = SpanReadable(base + data_guest, want);
  // A refresh copies the buffer's existing size, so it needs the WHOLE span; a
  // first upload may fall back to exactly what the draw asked for, because the
  // rounding-up is ours, not the guest's.
  if (!whole_readable && (!needs_grow || !SpanReadable(base + data_guest, need_bytes))) {
    ++b.failures;
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailSpan, b.failures);
    return false;
  }
  const std::uint32_t size = whole_readable ? want : need_bytes;
  const auto started = std::chrono::steady_clock::now();

  const std::uint32_t slots = dynamic ? kDynamicSlots : 1u;
  if (needs_grow) {
    // Growing replaces every slot; the old ones may still be in flight.
    for (std::uint32_t i = 0; i < kDynamicSlots; ++i) {
      if (b.slot[i]) {
        Retiring r;
        r.resource = std::move(b.slot[i]);
        r.fence = 0;
        g_retiring.push_back(std::move(r));
        b.slot[i].Reset();
        b.gpu[i] = 0;
      }
    }
    // THE SLOTS ARE GONE AS OF HERE, so the record must stop claiming to have
    // them. Every exit below this point used to leave `live` set with null
    // slots and zero GPU addresses, and the fast path at the top of the next
    // call ("live, big enough, fresh") would then hand out
    // BufferLocation = 0 as a vertex buffer view - far worse than dropping the
    // draw. It survived only because the cap exit happened to latch the buffer
    // dead forever; removing that latch (12ecb42, reverted) exposed it.
    b.live = false;
    b.size = 0;
    if (g_bytes + static_cast<std::uint64_t>(size) * slots > kMaxResidentBytes) {
      g_rejected.fetch_add(1, std::memory_order_relaxed);
      b.failures = kMaxFailures;
      NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailCap, b.failures);
      return false;
    }
    for (std::uint32_t i = 0; i < slots; ++i) {
      b.slot[i] = CreateBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COPY_DEST);
      if (!b.slot[i]) {
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailAlloc, 0);
        g_buffers.erase(key);
        return false;
      }
      b.gpu[i] = b.slot[i]->GetGPUVirtualAddress();
    }
    b.size = size;
    b.dynamic = dynamic;
    b.current = 0;
    g_bytes += static_cast<std::uint64_t>(size) * slots;
    g_uploaded.fetch_add(1, std::memory_order_relaxed);
  } else {
    b.current = (b.current + 1u) % slots;
    g_refreshed.fetch_add(1, std::memory_order_relaxed);
  }

  if (!StageCopy(device, base + data_guest, b.slot[b.current].Get(), b.size, is_index,
                 needs_grow)) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    NoteAcqFail(data_guest, need_bytes, is_index, kAcqFailStage, b.failures);
    return false;
  }
  b.frame_uploaded = g_frame;
  b.live = true;
  g_frame_budget_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  out.gpu_address = b.gpu[b.current];
  out.size_bytes = b.size;
  return true;
}

void BeginFrame(std::uint64_t frame) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_frame_budget_ms = 0.0;
  g_frame = frame;
  // If nothing ever flushes (the native pass is off, or the window is minimised)
  // the staging list must not grow without bound.
  if (g_pending.size() > 4096) {
    g_pending.clear();
  }
}

void Flush(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* cmd) {
  (void)device;
  if (cmd == nullptr) {
    return;
  }
  std::vector<Pending> work;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    work.swap(g_pending);
  }
  if (work.empty()) {
    return;
  }

  std::vector<D3D12_RESOURCE_BARRIER> to_copy;
  std::vector<D3D12_RESOURCE_BARRIER> to_read;
  to_copy.reserve(work.size());
  to_read.reserve(work.size());
  for (Pending& p : work) {
    const D3D12_RESOURCE_STATES read_state = p.is_index
                                                 ? D3D12_RESOURCE_STATE_INDEX_BUFFER
                                                 : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = p.dest.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = read_state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    to_copy.push_back(b);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    to_read.push_back(b);
  }

  std::vector<D3D12_RESOURCE_BARRIER> pre;
  pre.reserve(to_copy.size());
  for (std::size_t i = 0; i < work.size(); ++i) {
    if (!work[i].first_upload) {
      pre.push_back(to_copy[i]);
    }
  }
  if (!pre.empty()) {
    cmd->D3DResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
  }
  for (Pending& p : work) {
    cmd->D3DCopyBufferRegion(p.dest.Get(), 0, p.staging.Get(), 0, p.size);
  }
  cmd->D3DResourceBarrier(static_cast<UINT>(to_read.size()), to_read.data());

  std::lock_guard<std::mutex> lock(g_mutex);
  for (Pending& p : work) {
    Retiring r;
    r.resource = std::move(p.staging);
    r.fence = 0;
    g_retiring.push_back(std::move(r));
  }
}

void InvalidateGuestAddress(std::uint32_t addr) {
  if (addr == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  // Both keys the address can appear under (vertex and index), erased outright.
  // The D3D12 resources inside still belong to in-flight frames, so they go
  // through g_retiring exactly like a dynamic-slot refresh does: fence 0 here,
  // and RetireUploads stamps it submitted+8 - the same slack that covers the
  // staging->published->replay window for every other retirement.
  for (const bool is_index : {false, true}) {
    auto it = g_buffers.find(MakeKey(addr, is_index));
    if (it == g_buffers.end()) {
      continue;
    }
    for (auto& slot : it->second.slot) {
      if (slot) {
        Retiring r;
        r.resource = std::move(slot);
        r.fence = 0;
        g_retiring.push_back(std::move(r));
      }
    }
    g_buffers.erase(it);
    g_invalidated.fetch_add(1, std::memory_order_relaxed);
  }
}

void RetireUploads(std::uint64_t completed_fence, std::uint64_t submitted_fence) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& r : g_retiring) {
    if (r.fence == 0) {
      // NOT submitted_fence itself: a buffer retired by a grow during CAPTURE
      // is still referenced by draws sitting in the staging list, which get
      // submitted one or more fences LATER - and during a loading stall the
      // published snapshot referencing it is replayed for many frames without
      // a new publish. Stamping the current fence freed such buffers while a
      // future submission still read their VBVs: a GPU page fault
      // (DRED: page fault VA inside a buffer address, all breadcrumb lists
      // complete), which is the DEVICE_HUNG that killed every content
      // transition. Eight submissions of slack cover the deepest
      // staging->published->replay window and cost only a few transient MB.
      r.fence = submitted_fence + 8;
    }
  }
  while (!g_retiring.empty() && g_retiring.front().fence != 0 &&
         g_retiring.front().fence <= completed_fence) {
    g_retiring.pop_front();
  }
}

void LogStats() {
  std::lock_guard<std::mutex> lock(g_mutex);
  REXLOG_INFO("[native-vbuf] {} buffers ({} MB), {} refreshes, {} rejected, {} deferred, "
              "{} invalidated by guest free",
              g_uploaded.load(std::memory_order_relaxed), g_bytes / (1024 * 1024),
              g_refreshed.load(std::memory_order_relaxed),
              g_rejected.load(std::memory_order_relaxed),
              g_deferred.load(std::memory_order_relaxed),
              g_invalidated.load(std::memory_order_relaxed));
  std::uint64_t total = 0;
  for (const auto& c : g_fail_reason) {
    total += c.load(std::memory_order_relaxed);
  }
  if (total != 0) {
    REXLOG_INFO("[native-vbuf] acquire failures: {} args, {} latched, {} budget w/o copy, "
                "{} pages unreadable, {} resident cap, {} alloc, {} stage",
                g_fail_reason[kAcqFailArgs].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailLatched].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailBudget].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailSpan].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailCap].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailAlloc].load(std::memory_order_relaxed),
                g_fail_reason[kAcqFailStage].load(std::memory_order_relaxed));
    for (const auto& e : g_acqfail) {
      if (e.count == 0) {
        continue;
      }
      // The census is re-probed HERE, not when the failure happened: a latched
      // entry whose memory reads fine at print time is a latch that outlived
      // the streaming window that set it - the exact question this exists to
      // answer.
      const bool readable_now =
          g_last_base != nullptr && SpanReadable(g_last_base + e.addr, e.need);
      REXLOG_INFO("[native-vbuf]   acq-fail x{}: reason={} addr={:#x} need={} {} failures={} "
                  "readable_now={}",
                  e.count, e.reason, e.addr, e.need, e.is_index ? "ib" : "vb", e.failures,
                  readable_now);
    }
  }
}

}  // namespace dpour_vbuf
// === END DPOUR MIGRATION 2026-07-25 ===
