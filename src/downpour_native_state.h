// downpour - Native Render: the guest's pipeline state, read where it really is
//
// === DPOUR MIGRATION 2026-07-25: draw path - render state ===
//
// UnleashedRecomp gets depth/blend/cull by hooking one guest function per render
// state: on Xbox 360 the XDK's D3DDevice_SetRenderState is not a method but a
// per-state function pointer inside the device, and Unleashed - which replaces
// the device outright - simply fills that table with its own functions
// (UnleashedRecomp/gpu/video.h:318-337 GuestRenderState, video.cpp:2130).
//
// Downpour's XDK build inlines it instead: SetRenderState compiles to direct
// stores into the device's Xenos register shadow. Searching for a SetRenderState
// function therefore finds nothing, and the game's own source says why every
// piece of state must still be there - all of it goes through that one call:
//
//   XeD3DCommands.cpp:598 RHISetDepthState   -> ZENABLE, ZWRITEENABLE, ZFUNC
//   XeD3DCommands.cpp:627 RHISetBlendState   -> ALPHABLENDENABLE, ALPHATESTENABLE,
//                                               ALPHAFUNC, ALPHAREF + SetBlendState
//   XeD3DState.cpp:223    RHISetRasterizerState -> FILLMODE, CULLMODE
//
// So this module reads the shadow. That is not a shortcut around the reference
// approach, it IS the reference approach for a game whose state setters were
// inlined: the same values, taken from the same place the hardware takes them,
// one dword load per draw instead of a hook per state.
//
// The offsets were not guessed. They were read out of the recompiled code:
//
//   sub_829C9680 (RHISetDepthState)  writes dev+10548 bit1/bit2/bits4-6
//   sub_829C9990 (RHISetBlendState)  writes dev+10556 bit3 and bits0-2,
//                                    dev+10500 as a float, and calls
//   sub_82D1B000 (SetBlendState)     which stores the packed value at
//                                    dev+(TargetIndex ? TargetIndex+2645 : 2638)*4
//                                    = dev+10552 for target 0
//   sub_82D181D0 (ALPHABLENDENABLE)  writes 0x00010001 - ONE/ZERO/ADD - into
//                                    dev+10552/10584/10588/10592 when blending
//                                    is turned off, which is what proves those
//                                    four are RB_BLENDCONTROL0..3
//   sub_82D1F0B0 (DrawIndexedVertices) passes &dev[10548] to the packet builder,
//                                    which is what proves the block is the
//                                    contiguous Xenos register shadow, four
//                                    bytes per register from RB_DEPTHCONTROL.
#pragma once

#include <cstdint>

namespace dpour_state {

// One draw's worth of fixed-function state, already in D3D12 terms.
struct Pipeline {
  bool depth_enable = false;
  bool depth_write = false;
  std::uint32_t depth_func = 8;  // D3D12_COMPARISON_FUNC_ALWAYS

  std::uint32_t cull_mode = 1;  // D3D12_CULL_MODE_NONE
  bool front_ccw = false;

  bool blend_enable = false;
  std::uint32_t src_blend = 2;  // D3D12_BLEND_ONE
  std::uint32_t dst_blend = 1;  // D3D12_BLEND_ZERO
  std::uint32_t blend_op = 1;   // D3D12_BLEND_OP_ADD
  std::uint32_t src_blend_alpha = 2;
  std::uint32_t dst_blend_alpha = 1;
  std::uint32_t blend_op_alpha = 1;

  bool alpha_test = false;
  float alpha_ref = 0.0f;

  // RB_COLOR_MASK for render target 0, in D3D12's own bit order (RED 1, GREEN 2,
  // BLUE 4, ALPHA 8). 0xF unless the game turned colour writes off - which it
  // does for the depth prepass, for occlusion work and for the studio's depth
  // capture. Defaulting to "write everything" is what makes a prepass draw paint
  // over the target it was only supposed to fill the depth of.
  std::uint32_t color_write_mask = 0xF;

  // Shader constant banks the translated shaders read through SharedConstants:
  // bool bank 0x4900-0x4907 (bit N = b{N}; VS uses 0-127, PS 128-255) and loop
  // bank 0x4908-0x4927. Their shadow sits right after the PS float shadow in
  // the device: VS floats live at dev+(reg+120)*16 (sub_82D1D790) and PS floats
  // at dev+(reg+376)*16 (sub_82D1D868), which ends at dev+10112. Feeding zeros
  // is not cosmetic: a shader whose loop exit tests b{N} spins forever on the
  // GPU and removes the device.
  std::uint32_t bools[8] = {};
  std::uint32_t loops[32] = {};
};

// Reads the state the guest has set for the draw about to happen. Returns false
// only when the device pointer itself is not readable yet.
bool Read(const std::uint8_t* base, Pipeline& out);

// The game renders with a reversed depth buffer when its GInvertZ is set, in
// which case the depth target clears to 0 and the comparisons the register
// carries are already the flipped ones (ToggleCompareFunction, sub_829C9628).
// Read from the same global that function reads: guest 0x83BAB330.
bool InvertedDepth(const std::uint8_t* base);

// Inverted depth as OBSERVED, not as read from a global whose address is in
// doubt: whichever comparison direction the game's own draws actually use.
// GREATER/GEQUAL means near is 1 and the buffer clears to 0; LESS/LEQUAL means
// the usual way round. Guessing this wrong makes every pixel fail the depth
// test, which looks exactly like "the shading is broken".
// Returns false until enough draws have been seen to be sure.
bool ObservedInvertedDepth();

// One line summarising the distinct state combinations seen, so a wrong field
// shows up as a suspicious value rather than as a silently wrong picture.
void LogStats();

}  // namespace dpour_state
// === END DPOUR MIGRATION 2026-07-25 ===
