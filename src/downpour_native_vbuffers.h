// downpour - Native Render: host vertex / index buffers
//
// === DPOUR MIGRATION 2026-07-25: the UnleashedRecomp buffer model ===
//
// UnleashedRecomp replaces the guest's buffer objects outright: CreateVertexBuffer
// makes a HOST buffer, Lock hands the game a scratch allocation, and Unlock
// byte-swaps the whole thing dword-wise into the host buffer
// (video.cpp UnlockBuffer<uint32_t>, hooks sub_82BE6B98 / sub_82BE6BE8).
//
// Downpour reaches the same place from the other end. Its RHILockVertexBuffer
// returns a pointer straight into the buffer's physical allocation and
// RHIUnlockVertexBuffer is a bare IDirect3DVertexBuffer9::Unlock with nothing in
// it (Development/Src/Xenon/XeD3DDrv/Src/XeD3DVertexBuffer.cpp:77-118), and
// cooked geometry is never locked at all - AllocVertexBuffer just points
// BaseAddress at the resource array (:29-38). So the swap happens on first use
// in a draw instead, which is the same logical moment: "the guest's bytes are
// final, take a host copy and swap them".
//
// The dword swap is what makes 32-bit fields (floats, D3DCOLOR) and packed
// dwords land correctly. It leaves 16-bit pairs (Short2, Half2) in the wrong
// order, which is exactly what the shader's g_SwappedTexcoords fixes up - the
// same division of labour the reference uses.
//
// Buffers are cached per guest allocation, not per drawn range: a UE3 static
// mesh is one buffer drawn as dozens of sections, and keying on (address, range)
// made a separate host copy of the same megabytes for every section.
#pragma once

#include <cstdint>

#include <rex/graphics/d3d12/deferred_command_list.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace dpour_vbuf {

// A host buffer ready to be bound. `gpu_address` is the START of the buffer, so
// the caller adds the guest's own stream offset to it.
struct View {
  std::uint64_t gpu_address = 0;
  std::uint32_t size_bytes = 0;
};

// Host copy of the guest buffer at `data_guest`, at least `need_bytes` long,
// byte-swapped (per dword for vertices, per index for 16-bit indices).
//
// `usage_flags` is the guest's own FXeGPUResource::UsageFlags (+16), whose
// RUF_Dynamic|RUF_Volatile bits say the game repacks the buffer, in which case
// the host copy is refreshed once per frame instead of cached forever
// (Engine/Inc/RHI.h:473-485).
//
// The copy is recorded by Flush() onto the same command list that then draws, so
// the buffer is usable in the frame it was acquired.
bool AcquireBuffer(ID3D12Device* device, const std::uint8_t* base, std::uint32_t data_guest,
                   std::uint32_t need_bytes, bool is_index, std::uint32_t usage_flags, View& out);

// Frame boundary on the guest render thread: resets the per-frame upload budget
// and tells dynamic buffers a new frame started.
void BeginFrame(std::uint64_t frame);

// Render thread: record every staged copy. MUST run on the command list before
// the draws that reference the buffers.
// Records onto the command processor's deferred list - the SAME list the rest
// of the frame is recorded on. A second, independent command list submitted
// beside it is exactly the parallel timeline the references do not have.
void Flush(ID3D12Device* device, rex::graphics::d3d12::DeferredCommandList* cmd);
void RetireUploads(std::uint64_t completed_fence, std::uint64_t submitted_fence);
void LogStats();

}  // namespace dpour_vbuf
// === END DPOUR MIGRATION 2026-07-25 ===
