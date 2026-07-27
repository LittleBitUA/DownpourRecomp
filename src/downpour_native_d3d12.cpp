// downpour - Native Render: standalone D3D12 device self-test (Phase 1a)
//
// === DPOUR MIGRATION 2026-07-24: native-render Phase 1a (D3D12 bring-up) ===
//
// What this proves: that we can create and drive a real, native D3D12 device
// INSIDE the recompiled game process — the exact primitives every later
// native-render phase needs (device, direct queue, command allocator + list,
// a render target + RTV heap, a fence, and a GPU->CPU readback). It is the
// hardware foundation for replacing the Xenos GPU emulation with native draws.
//
// Deliberately ISOLATED and ZERO-RISK: this creates its OWN independent
// ID3D12Device (not the runtime's) and does everything OFFSCREEN. It never
// touches the runtime's device, command queue, swapchain, descriptor heaps or
// resource states, so it cannot destabilize the game's existing rendering.
//   * It clears a small offscreen render target to pure magenta (255,0,255),
//     copies it to a readback buffer, maps it, and verifies the top-left pixel
//     round-tripped exactly. Result is logged. Nothing is drawn on screen yet.
//
// The runtime cleanly exposes rex::ui::d3d12::D3D12Provider::GetDevice() /
// GetDirectQueue() (and the presenter owns the swapchain) — that is the path
// Phase 1b will reuse to put a native pixel on the real backbuffer without any
// cross-device interop. For now we stay isolated and safe.
//
// Gated by the same env var as the present hook (DPOUR_NR); runs once. Revert:
// delete this file + downpour_native_d3d12.h, drop them from CMakeLists.txt.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "downpour_native_d3d12.h"

#include <cstdint>
#include <mutex>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <rex/logging.h>

using Microsoft::WRL::ComPtr;

namespace dpour_nr {
namespace {

constexpr UINT kRtWidth = 16;
constexpr UINT kRtHeight = 16;

// Minimal HRESULT check that logs and bails out of the self-test.
#define DPOUR_HR(expr, what)                                             \
  do {                                                                   \
    const HRESULT _hr = (expr);                                          \
    if (FAILED(_hr)) {                                                   \
      REXLOG_ERROR("[native-d3d12] {} failed hr={:#010x}", (what),       \
                   static_cast<uint32_t>(_hr));                          \
      return;                                                            \
    }                                                                    \
  } while (0)

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}

void RunSelfTest() {
  REXLOG_INFO("[native-d3d12] Phase 1a bring-up: creating isolated native D3D12 device...");

  // 1. Device on the default adapter (independent from the runtime's device).
  ComPtr<ID3D12Device> device;
  DPOUR_HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
           "D3D12CreateDevice");

  // 2. Direct command queue.
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  DPOUR_HR(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

  // 3. Allocator + graphics command list.
  ComPtr<ID3D12CommandAllocator> allocator;
  DPOUR_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
           "CreateCommandAllocator");
  ComPtr<ID3D12GraphicsCommandList> cmd;
  DPOUR_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&cmd)),
           "CreateCommandList");

  // 4. Offscreen render target (magenta clear colour baked in as optimized clear).
  const D3D12_CLEAR_VALUE clear_value{DXGI_FORMAT_R8G8B8A8_UNORM, {1.0f, 0.0f, 1.0f, 1.0f}};
  D3D12_RESOURCE_DESC rt_desc{};
  rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rt_desc.Width = kRtWidth;
  rt_desc.Height = kRtHeight;
  rt_desc.DepthOrArraySize = 1;
  rt_desc.MipLevels = 1;
  rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rt_desc.SampleDesc.Count = 1;
  rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  const D3D12_HEAP_PROPERTIES default_heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> rt;
  DPOUR_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                           IID_PPV_ARGS(&rt)),
           "CreateCommittedResource(rt)");

  // 5. RTV heap + view.
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  DPOUR_HR(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
           "CreateDescriptorHeap(rtv)");
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(rt.Get(), nullptr, rtv);

  // 6. Readback buffer sized from the copyable footprint.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(&rt_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

  D3D12_RESOURCE_DESC rb_desc{};
  rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb_desc.Width = total_bytes;
  rb_desc.Height = 1;
  rb_desc.DepthOrArraySize = 1;
  rb_desc.MipLevels = 1;
  rb_desc.Format = DXGI_FORMAT_UNKNOWN;
  rb_desc.SampleDesc.Count = 1;
  rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  const D3D12_HEAP_PROPERTIES readback_heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12Resource> readback;
  DPOUR_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&readback)),
           "CreateCommittedResource(readback)");

  // 7. Record: clear -> transition RT to COPY_SOURCE -> copy into readback.
  const float clear_rgba[4] = {1.0f, 0.0f, 1.0f, 1.0f};
  cmd->ClearRenderTargetView(rtv, clear_rgba, 0, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = rt.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cmd->ResourceBarrier(1, &barrier);

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = rt.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  DPOUR_HR(cmd->Close(), "CommandList::Close");

  // 8. Submit + fence-wait for GPU completion.
  ID3D12CommandList* lists[] = {cmd.Get()};
  queue->ExecuteCommandLists(1, lists);

  ComPtr<ID3D12Fence> fence;
  DPOUR_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event == nullptr) {
    REXLOG_ERROR("[native-d3d12] CreateEvent failed");
    return;
  }
  DPOUR_HR(queue->Signal(fence.Get(), 1), "Queue::Signal");
  if (fence->GetCompletedValue() < 1) {
    DPOUR_HR(fence->SetEventOnCompletion(1, fence_event), "SetEventOnCompletion");
    WaitForSingleObject(fence_event, INFINITE);
  }
  CloseHandle(fence_event);

  // 9. Map the readback and verify the top-left texel round-tripped exactly.
  void* mapped = nullptr;
  const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
  DPOUR_HR(readback->Map(0, &read_range, &mapped), "Readback::Map");
  const auto* px = static_cast<const uint8_t*>(mapped);
  const uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
  const D3D12_RANGE no_write{0, 0};
  readback->Unmap(0, &no_write);

  const bool ok = (r == 255 && g == 0 && b == 255 && a == 255);
  if (ok) {
    REXLOG_INFO(
        "[native-d3d12] SELF-TEST PASS — native D3D12 device drove a clear+readback; "
        "top-left texel RGBA=({},{},{},{}) matches magenta. GPU foundation LIVE.",
        r, g, b, a);
  } else {
    REXLOG_ERROR(
        "[native-d3d12] SELF-TEST readback mismatch RGBA=({},{},{},{}) expected (255,0,255,255)",
        r, g, b, a);
  }
}

std::once_flag g_selftest_once;

}  // namespace

void RunD3D12SelfTestOnce() {
  std::call_once(g_selftest_once, RunSelfTest);
}

}  // namespace dpour_nr
// === END DPOUR MIGRATION 2026-07-24 ===
