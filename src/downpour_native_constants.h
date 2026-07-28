// downpour - Native Render: the guest's shader constant banks
//
// === DPOUR MIGRATION 2026-07-25: constants for the game's own shaders ===
//
// The translated shaders address constants by register number, not by name:
// Downpour's shader containers have no constant table at all
// (constantTableOffset == 0), so XenosRecomp emits the whole bank as
//   cbuffer VertexShaderConstants : register(b0, space4) { float4 g_VertexConstants[256]; }
//   cbuffer PixelShaderConstants  : register(b1, space4) { float4 g_PixelConstants[256];  }
// and every c[N] in the microcode becomes g_*Constants[N].
//
// So all the runtime has to do is shadow what the guest writes. Both setters are
// reached through one RHI function each, which the game's own source shows are
// otherwise identical (XeD3DCommands.cpp:400 RHISetVertexShaderParameter and
// :518 RHISetPixelShaderParameter - same body, different device method):
//
//   sub_829C92B0 -> sub_82D1D790  IDirect3DDevice9::SetVertexShaderConstantF
//   sub_829C93D0 -> sub_82D1D868  IDirect3DDevice9::SetPixelShaderConstantF
//
// The pixel one was identified statically: in the recompiled code the two RHI
// functions are structurally identical - the same five helper calls at the same
// offsets - and differ only in the device method they end with.
#pragma once

#include <cstdint>

namespace dpour_consts {

constexpr std::uint32_t kRegisters = 256;
constexpr std::uint32_t kBankBytes = kRegisters * 16;

// Device SetVertexShaderConstantF / SetPixelShaderConstantF
// (dev, StartRegister, const float* pData, Vector4fCount). Guest floats are
// big-endian; they are byte-swapped once here and stored host-side.
void SetVertexConstants(const std::uint8_t* base, std::uint32_t start_register,
                        std::uint32_t data_guest, std::uint32_t vec4_count);
void SetPixelConstants(const std::uint8_t* base, std::uint32_t start_register,
                       std::uint32_t data_guest, std::uint32_t vec4_count);

// Copy a bank into a constant-buffer allocation. `bytes` may be less than
// kBankBytes: the shaders read the bank through a ROOT CBV, which carries no
// size, so registers past the end simply read whatever follows in the ring - and
// a register the game never wrote is one no shader reads. Uploading only what
// the game has actually set turns ~8 KB of constants per draw into well under 2.
void CopyVertexBank(void* dst, std::uint32_t bytes);
void CopyPixelBank(void* dst, std::uint32_t bytes);

// Bytes worth uploading: (highest register the game has ever written + 1) * 16,
// rounded up to a float4.
std::uint32_t VertexBankBytes();
std::uint32_t PixelBankBytes();

// Bumped whenever the bank changes, so a draw can skip re-uploading constants
// that nothing has touched since the last one.
std::uint64_t VertexRevision();
std::uint64_t PixelRevision();

// THE DEVICE'S OWN CONSTANT SHADOW - the authoritative bank.
//
// Shadowing the setters is not enough for this build, and the exact shortfall is
// measured rather than guessed (DPOUR_NR_C0_PROBE, 2026-07-28): the setter shadow
// holds register 0 at ZERO for every draw while the device's own file holds 1, 8
// or 32 there. That register is PSR_ColorBiasFactor, which every pixel shader
// multiplies its rgb by (Common.usf:286), so a shadow that misses it turns the
// whole scene black. The XDK inlines constant writes straight into the device's
// register file at many call sites, exactly as it inlines SetRenderState (see
// downpour_native_state.h for the same lesson).
//
// (An earlier note here argued the shadow was broadly incomplete because it
// topped out at register 33. The device file tops out at 32, so the shadow was
// in fact nearly complete - it missed register 0 specifically. The conclusion
// was right for the wrong reason; the correction is kept because the reason is
// what tells you where else to look.)
//
// So read the file itself. From the recompiled commit path (sub_82D1E250):
//   VS float constants at device + 1920   (register N at +(N+120)*16)
//   PS float constants at device + 6016   (register N at +(N+376)*16)
// with GDirect3DDevice at guest 0x83790010. Values are big-endian floats, 256
// vec4 each. Returns null when the device is not up yet.
const std::uint8_t* DeviceVertexBank(const std::uint8_t* base);
const std::uint8_t* DevicePixelBank(const std::uint8_t* base);
constexpr std::uint32_t kDeviceBankBytes = 256 * 16;

// Whether the device shadow is used in place of the shadowed setters
// (kill switch: DPOUR_NR_CONSTS_HOOKONLY).
bool DeviceBanksEnabled();

void LogStats();

}  // namespace dpour_consts
// === END DPOUR MIGRATION 2026-07-25 ===
