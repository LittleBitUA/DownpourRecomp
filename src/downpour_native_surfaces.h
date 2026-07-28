// downpour - Native Render: the surface layer, written from the game's own source
//
// === DPOUR MIGRATION 2026-07-29: surface / EDRAM / resolve layer ==============
//
// Every number in this module comes out of the game's own renderer, not out of a
// reference recomp and not out of a guess. The four functions it reproduces are:
//
//   XeEDRAMOffset            XeD3DRenderTarget.cpp:751  where a surface lives
//   XGSurfaceSize            (XDK)                      how many tiles it takes
//   XeGetRenderTargetColorExpBias  XeD3DRenderTarget.cpp:94   its resolve divisor
//   FXeSurfaceInfo::IsOverlapping  XeD3DRenderTarget.h:41     whether two collide
//
// WHY THIS EXISTS AT ALL. The old layer decided identity by name prefix and
// picked one divisor for a whole family of surfaces. The game does neither: it
// gives every surface an EDRAM range and its own exponent bias, and it pairs
// that bias with the shader that writes into it -
//
//   RHISetRenderTarget:  GCurrentColorExpBias = surface.XeSurfaceInfo.ColorExpBias
//                        RHISetRenderTargetBias(2^bias)   -> PS c0
//                          (XeD3DCommands.cpp:714-716)
//   the shaders:         RETURN_COLOR(c) = c.rgb * SCENE_COLOR_BIAS_FACTOR.x
//                          (Common.usf:284-289, and c0 IS that constant)
//   RHICopyToResolveTarget: D3DRESOLVE_EXPONENTBIAS(-surface.ColorExpBias)
//                          (XeD3DRenderTarget.cpp:519)
//
// So the multiply on write and the divide on resolve are the SAME surface's
// bias, and they cancel exactly. Nothing has to be modelled, guessed or voted
// on - it only has to be done on both sides with the same number. Getting that
// number from the wrong surface is what made the picture too bright: measured
// 28.07, the DefaultColor family resolves through three surfaces whose biases
// are 3, 0 and 5 (divide by 8, 1 and 32), and one shared divisor cannot be right
// for all three.
//
// WHAT THE GAME SHARES, AND WHAT IT ONLY REUSES. A shared EDRAM offset means one
// memory at different TIMES, not one content at once: LightAttenuation sits on
// SceneColor's tiles because the scene has already been resolved out by then,
// and the engine puts it back itself (SaveSceneColorRaw / RestoreSceneColorRaw,
// SceneRenderTargets.cpp:494-523 - a resolve out and a full-screen quad back in,
// which are ordinary draws we already carry). The ONE place the same live
// content is read through several surfaces is the DefaultColor family, and the
// engine says so in its own allocation: SceneColor, SceneColorRaw and
// SceneColorFixedPoint are three RHICreateSharedTexture2D views of one
// SceneColorMemoryBuffer (SceneRenderTargets.cpp:1038-1053). That family shares
// a native target here. Nothing else does.
#pragma once

#include <cstdint>
#include <vector>

namespace dpour_surfaces {

// The game's EPixelFormat as it reaches RHICreateTargetableSurface. Only the
// values this game actually creates targetable surfaces with are named.
enum : std::uint32_t {
  kPF_Unknown = 0,
  kPF_A32B32G32R32F = 1,
  kPF_A8R8G8B8 = 2,
  kPF_G8 = 3,
  kPF_G16 = 4,
  kPF_DepthStencil = 11,
  kPF_ShadowDepth = 12,
  kPF_FilteredShadowDepth = 13,
  kPF_R32F = 14,
  kPF_G16R16 = 15,
  kPF_G16R16F = 16,
  kPF_G16R16F_FILTER = 17,
  kPF_G32R32F = 18,
  kPF_A2B10G10R10 = 19,
  kPF_A16B16G16R16 = 20,
  kPF_D24 = 21,
  kPF_R16F = 22,
  kPF_R16F_FILTER = 23,
  kPF_FloatRGB = 9,
  kPF_FloatRGBA = 10,
};

// One targetable surface, as the game created it.
struct Surface {
  std::uint32_t object = 0;           // guest IDirect3DSurface9*
  char name[32] = {};                 // the game's own UsageStr
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  std::uint32_t pixel_format = 0;     // EPixelFormat at creation
  std::uint32_t resolve_texture = 0;  // the RHI texture ref it resolves into
  bool is_depth = false;
  // Where the console would have put it. Tiles are 5120 bytes; EDRAM holds 2048.
  std::uint32_t edram_offset = 0;
  std::uint32_t edram_size = 0;
  // What XeGetRenderTargetColorExpBias would return for this surface, as far as
  // the surface's own format determines it. The authoritative value is the one
  // the game stored in FXeSurfaceInfo (+20), which is read at resolve time; this
  // is the cross-check that says when the two disagree and therefore when our
  // reading of the struct has drifted. -1 means "depends on the resolve
  // texture's format, which is not known from the surface alone".
  std::int32_t expected_bias = 0;
  // The surface whose native target this one draws into: itself, except for the
  // DefaultColor family, where all three share the primary's.
  std::uint32_t group = 0;
};

// Forget everything. A level load recreates the whole set.
void Reset();

// Record a surface the game just created. Returns FALSE if the arguments were
// not plausible. Safe to call from the guest thread.
bool Register(std::uint32_t object, const char* name, std::uint32_t w, std::uint32_t h,
              std::uint32_t pixel_format, std::uint32_t resolve_texture);

// Drop a surface the game retired.
void Unregister(std::uint32_t object);

// Copy out the record for a surface. Returns FALSE if there is none. By value on
// purpose: the table is shared with the guest thread, and a pointer into it
// would outlive the lock that made it safe to read.
bool Find(std::uint32_t object, Surface& out);

// Every surface currently on record, appended to `out`. For the F3 panel's
// surface picker: the game names them all, so the list is the game's own.
void ListSurfaces(std::vector<Surface>& out);

// The DefaultColor primary - the surface the whole family draws into - or 0
// before the game has created it.
std::uint32_t FamilyPrimary();

// Every surface currently in the DefaultColor family, primary included. Appends;
// leaves `out` untouched when there is no family yet.
void FamilyMembers(std::vector<std::uint32_t>& out);

// The native target key for a surface: itself, or the DefaultColor primary.
// Replaces CanonicalSurface - same answer, from the engine's own allocation
// rather than from a name prefix and a four-slot alias array.
std::uint32_t GroupOf(std::uint32_t object);

// TRUE when the two surfaces occupy overlapping EDRAM - the game's own test
// (FXeSurfaceInfo::IsOverlapping). The engine never calls it; we log with it, so
// that a surface pair the game reuses in a way we did not expect shows up by
// name instead of as a wrong picture.
bool Overlaps(const Surface& a, const Surface& b);

// Size of a surface in EDRAM tiles, XGSurfaceSize for the non-multisampled case.
std::uint32_t SurfaceTiles(std::uint32_t w, std::uint32_t h, std::uint32_t pixel_format);

// XeEDRAMOffset: the tile a surface with this usage name and size starts at.
std::uint32_t EdramOffset(const char* name, std::uint32_t tiles);

// XeGetRenderTargetColorExpBias, as far as the surface format alone decides it.
// Returns -1 when the resolve texture's format is what picks between two
// answers (PF_FloatRGB surfaces, which are 3 into a float texture and 5 into a
// fixed-point one).
std::int32_t ExpectedBias(std::uint32_t pixel_format);

// The screen size the EDRAM layout is computed against: GScreenWidth/Height in
// the game. Taken from DefaultDepth when it is created, 1280x720 until then.
void NoteScreenSize(std::uint32_t w, std::uint32_t h);

}  // namespace dpour_surfaces
