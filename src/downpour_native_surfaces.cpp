// downpour - Native Render: the surface layer, written from the game's own source
//
// See downpour_native_surfaces.h for what this reproduces and why. Every table
// below is a transcription; where a line deviates from the game it says so.

#include "downpour_native_surfaces.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <rex/logging.h>

namespace dpour_surfaces {
namespace {

// EDRAM geometry, from the game's own comment (XeD3DRenderTarget.cpp:744):
// "10 MByte EDRAM, 5120 (160x32) bytes per tile => 2048 tiles". A tile is 16
// rows tall, so its width in pixels falls out of the pixel size: 80 pixels at 4
// bytes, 40 at 8, 160 at 2.
constexpr std::uint32_t kTileBytes = 5120;
constexpr std::uint32_t kTileRows = 16;
constexpr std::uint32_t kEdramTiles = 2048;  // GPU_EDRAM_TILES

std::mutex g_mutex;
std::unordered_map<std::uint32_t, Surface> g_surfaces;
// The DefaultColor family's primary. A level load recreates the family, and the
// primary always leads it (SceneRenderTargets.cpp:1041 before :1046 and :1052),
// so registering "DefaultColor" starts a new generation.
std::uint32_t g_family_primary = 0;
std::uint32_t g_screen_w = 1280;
std::uint32_t g_screen_h = 720;
// "SurfaceA|SurfaceB" pairs already reported as overlapping, so the log carries
// each distinct collision once instead of once per object.
std::set<std::string> g_reported_pairs;

bool NameIs(const char* a, const char* b) { return _stricmp(a, b) == 0; }

// The size in bytes of one pixel of the surface D3D format the game would pick
// for this EPixelFormat - XeGetRenderTargetFormat (XeD3DRenderTarget.cpp:20)
// followed by that D3DFORMAT's own size. PF_G8 is here because the game says
// out loud that it cannot render to it: "can only render to 32 bit formats on
// 360" (:46), and substitutes A8R8G8B8.
std::uint32_t RenderTargetBytesPerPixel(std::uint32_t pf) {
  switch (pf) {
    case kPF_FloatRGBA:       // D3DFMT_A16B16G16R16F
    case kPF_A16B16G16R16:    // D3DFMT_A16B16G16R16_EDRAM
    case kPF_G32R32F:
      return 8;
    case kPF_A32B32G32R32F:
      return 16;
    case kPF_R16F:
    case kPF_R16F_FILTER:
    case kPF_G16:
      return 2;
    default:
      // A8R8G8B8, G8 (widened), FloatRGB (A2B10G10R10F_EDRAM), A2B10G10R10,
      // G16R16(_EDRAM), G16R16F, R32F, and the whole depth family (D24S8,
      // D24X8, D24FS8, D32) are all four bytes.
      return 4;
  }
}

}  // namespace

std::uint32_t SurfaceTiles(std::uint32_t w, std::uint32_t h, std::uint32_t pixel_format) {
  if (w == 0 || h == 0) {
    return 0;
  }
  const std::uint32_t bpp = RenderTargetBytesPerPixel(pixel_format);
  const std::uint32_t tile_w = kTileBytes / (kTileRows * bpp);
  const std::uint32_t tiles_x = (w + tile_w - 1) / tile_w;
  const std::uint32_t tiles_y = (h + kTileRows - 1) / kTileRows;
  return tiles_x * tiles_y;
}

void NoteScreenSize(std::uint32_t w, std::uint32_t h) {
  if (w < 64 || h < 64 || w > 4096 || h > 4096) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_screen_w == w && g_screen_h == h) {
    return;
  }
  g_screen_w = w;
  g_screen_h = h;
}

// XeEDRAMOffset (XeD3DRenderTarget.cpp:751), transcribed. The one liberty taken
// is GScreenWidth/GScreenHeight: the game reads its own globals, we take the
// size of DefaultDepth, which is the surface those globals size. ShadowDepthZSize
// is computed in the original and never used; it is left out.
std::uint32_t EdramOffset(const char* name, std::uint32_t tiles) {
  std::uint32_t screen_w, screen_h;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    screen_w = g_screen_w;
    screen_h = g_screen_h;
  }
  const std::uint32_t default_depth = SurfaceTiles(screen_w, screen_h, kPF_DepthStencil);
  const std::uint32_t default_color = SurfaceTiles(screen_w, screen_h, kPF_A8R8G8B8);
  const std::uint32_t small_depth = SurfaceTiles(screen_w / 2, screen_h / 2, kPF_DepthStencil);
  const std::uint32_t small_color = SurfaceTiles(screen_w / 2, screen_h / 2, kPF_A8R8G8B8);

  std::uint32_t offset;
  if (NameIs(name, "DefaultDepth") || NameIs(name, "DominantShadowDepthZ") ||
      NameIs(name, "DoFBlurBuffer")) {
    // Place in the beginning of EDRAM.
    offset = 0;
  } else if (NameIs(name, "SmallDepth")) {
    // Place the quarter-sized depth buffer after SceneDepth and SceneColor.
    offset = default_depth + default_color;
  } else if (NameIs(name, "ShadowDepthZ") || NameIs(name, "ShadowVariance") ||
             NameIs(name, "ShadowDepthRT")) {
    // Co-exist with SceneDepth and SceneColor. Overlap SmallZ + Free.
    offset = default_depth + default_color;
  } else if (NameIs(name, "FogBuffer")) {
    // Co-exist with SceneDepth, SceneColor, SmallDepth and AmbientOcclusion.
    offset = default_depth + default_color + small_depth + small_color;
  } else if (NameIs(name, "FilterColor") || NameIs(name, "FilterColor2") ||
             NameIs(name, "FogFrontfacesIntegralAccumulationRT") ||
             NameIs(name, "FogBackfacesIntegralAccumulationRT") ||
             NameIs(name, "TranslucencyBuffer") || NameIs(name, "TranslucencyBuffer2") ||
             NameIs(name, "TranslucencyShadowDepthZ") || NameIs(name, "AmbientOcclusion") ||
             NameIs(name, "LUTBlend") || NameIs(name, "AOHistory") ||
             NameIs(name, "VelocityBuffer")) {
    // The 4th, "Free" region: co-exist with SceneDepth, SceneColor, SmallDepth.
    offset = default_depth + default_color + small_depth;
  } else {
    // By default, co-exist with SceneDepth only (overlap SceneColor).
    offset = default_depth;
  }

  // The game checks this with check(); an overflowing offset here means our
  // screen size is not the one the layout was computed against, which is worth
  // saying once rather than asserting on.
  if (offset + tiles > kEdramTiles) {
    static bool said = false;
    if (!said) {
      said = true;
      REXLOG_WARN("[surfaces] EDRAM layout overflows for \"{}\": offset {} + {} tiles > {} "
                  "(screen {}x{}) - the layout is being computed against the wrong screen size",
                  name, offset, tiles, kEdramTiles, screen_w, screen_h);
    }
  }
  return offset;
}

// XeGetRenderTargetColorExpBias (XeD3DRenderTarget.cpp:94) keys on the PAIR
// (surface format, resolve texture format), so the surface alone answers it only
// where both branches agree:
//
//   A2B10G10R10F_EDRAM -> FloatRGB texture   : 3   (the scene, written x8)
//   A2B10G10R10F_EDRAM -> A2B10G10R10        : 5   (DefaultColorFixedPoint)
//   A16B16G16R16_EDRAM -> A16B16G16R16       : 5
//   anything else                            : 0
//
// so only the formats that CANNOT reach a biased branch answer it here.
//
// PF_FloatRGB is ambiguous: DefaultColor and DefaultColorFixedPoint are both
// created with it and differ only in the texture they resolve into
// (SceneRenderTargets.cpp:1041 vs :1052) - 3 and 5.
//
// PF_A16B16G16R16 is ambiguous for the same reason, which THIS CROSS-CHECK
// FOUND, 29.07: it warned on TranslucencyBuffer, where the game had stored 0 and
// the table said 5. The game was right. Its surface format is PF_A16B16G16R16
// but its texture is PF_FloatRGBA (SceneRenderTargets.cpp:1148-1150), i.e.
// A16B16G16R16F, and the branch wants the fixed-point A16B16G16R16 - which
// FilterColor does use, and which correctly reads 5. The surface format alone
// never decided this one; assuming it did was the bug.
//
// The authoritative value is always the one the game itself computed and stored
// in FXeSurfaceInfo at +20. This function exists only to say when our reading of
// that struct has drifted, so it must stay silent wherever it cannot be certain.
std::int32_t ExpectedBias(std::uint32_t pixel_format) {
  switch (pixel_format) {
    case kPF_FloatRGB:        // 3 into a float texture, 5 into a fixed-point one
    case kPF_A16B16G16R16:    // 5 into A16B16G16R16, 0 into A16B16G16R16F
      return -1;
    default:
      // Every remaining format misses all three branches of
      // XeGetRenderTargetColorExpBias whatever it resolves into.
      return 0;
  }
}

// FXeSurfaceInfo::IsOverlapping (XeD3DRenderTarget.h:41), transcribed.
bool Overlaps(const Surface& a, const Surface& b) {
  return (b.edram_offset >= a.edram_offset &&
          b.edram_offset < a.edram_offset + a.edram_size && b.edram_size > 0) ||
         (a.edram_offset >= b.edram_offset &&
          a.edram_offset < b.edram_offset + b.edram_size && a.edram_size > 0);
}

void Reset() {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_surfaces.clear();
  g_reported_pairs.clear();
  g_family_primary = 0;
}

bool Register(std::uint32_t object, const char* name, std::uint32_t w, std::uint32_t h,
              std::uint32_t pixel_format, std::uint32_t resolve_texture) {
  if (object == 0 || name == nullptr || w == 0 || h == 0 || w > 4096 || h > 4096) {
    return false;
  }

  // DefaultDepth is what GScreenWidth/GScreenHeight size, and it is created
  // before every colour surface whose offset depends on it
  // (SceneRenderTargets.cpp:995 ahead of :1021 and :1041).
  if (NameIs(name, "DefaultDepth")) {
    NoteScreenSize(w, h);
  }

  Surface s;
  s.object = object;
  std::snprintf(s.name, sizeof(s.name), "%s", name);
  s.w = w;
  s.h = h;
  s.pixel_format = pixel_format;
  s.resolve_texture = resolve_texture;
  // The game's own test, and the same one the reference makes: it does not copy
  // depth resolves at all ("Depth stencil textures in this game are guaranteed
  // to be transient", video.cpp:3572). XeIsDepthFormat keys on the D3DFORMAT;
  // the name is what we have, and the engine names every one of them.
  s.is_depth = pixel_format == kPF_DepthStencil || pixel_format == kPF_ShadowDepth ||
               pixel_format == kPF_FilteredShadowDepth || pixel_format == kPF_D24 ||
               std::strstr(name, "Depth") != nullptr;
  s.edram_size = SurfaceTiles(w, h, pixel_format);
  s.edram_offset = EdramOffset(name, s.edram_size);
  s.expected_bias = ExpectedBias(pixel_format);

  std::lock_guard<std::mutex> lk(g_mutex);
  // THE ONE FAMILY THAT SHARES LIVE CONTENT. SceneColor, SceneColorRaw and
  // SceneColorFixedPoint are three views of one SceneColorMemoryBuffer
  // (SceneRenderTargets.cpp:1038-1053) over one EDRAM range, and the engine
  // reads the scene back through whichever of them suits the pass. Everything
  // else that shares a range only reuses it in time, and the engine moves the
  // content itself.
  if (NameIs(name, "DefaultColor")) {
    g_family_primary = object;  // a level load recreates the family, primary first
    s.group = object;
  } else if (std::strncmp(name, "DefaultColor", 12) == 0 && g_family_primary != 0) {
    s.group = g_family_primary;
  } else {
    s.group = object;
  }

  g_surfaces.insert_or_assign(object, s);
  REXLOG_INFO("[surfaces] \"{}\" {:#x} {}x{} pf={} EDRAM [{}..{}) {} tiles, bias {}, group {:#x}",
              s.name, object, w, h, pixel_format, s.edram_offset, s.edram_offset + s.edram_size,
              s.edram_size, s.expected_bias, s.group);

  // Name the collisions, so that a surface pair the game reuses in a way this
  // layer did not expect shows up as a line here instead of as a wrong picture
  // three passes later. Overlap is NORMAL - the layout is built out of it - so
  // this is INFO.
  //
  // ONCE PER PAIR OF NAMES. Measured on the first run: 780 lines, almost all of
  // them AuxColor against another AuxColor, because the game creates about
  // twenty of those at different sizes and every one collided with every
  // earlier one. Same name is the same buffer used again at another size, which
  // is not the finding; a DIFFERENT name on the same tiles is.
  for (const auto& [other_obj, other] : g_surfaces) {
    if (other_obj == object || other.is_depth != s.is_depth || other.group == s.group ||
        NameIs(other.name, s.name) || !Overlaps(s, other)) {
      continue;
    }
    char pair[72];
    std::snprintf(pair, sizeof(pair), "%s|%s", s.name, other.name);
    if (!g_reported_pairs.insert(pair).second) {
      continue;
    }
    REXLOG_INFO("[surfaces]   overlaps \"{}\" {:#x} [{}..{}) - separate targets here; the "
                "engine moves content between them itself",
                other.name, other_obj, other.edram_offset, other.edram_offset + other.edram_size);
  }
  return true;
}

void Unregister(std::uint32_t object) {
  std::lock_guard<std::mutex> lk(g_mutex);
  const auto it = g_surfaces.find(object);
  if (it == g_surfaces.end()) {
    return;
  }
  if (g_family_primary == object) {
    g_family_primary = 0;
  }
  g_surfaces.erase(it);
}

bool Find(std::uint32_t object, Surface& out) {
  std::lock_guard<std::mutex> lk(g_mutex);
  const auto it = g_surfaces.find(object);
  if (it == g_surfaces.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void ListSurfaces(std::vector<Surface>& out) {
  std::lock_guard<std::mutex> lk(g_mutex);
  out.reserve(out.size() + g_surfaces.size());
  for (const auto& [object, s] : g_surfaces) {
    out.push_back(s);
  }
}

std::uint32_t FamilyPrimary() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_family_primary;
}

void FamilyMembers(std::vector<std::uint32_t>& out) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_family_primary == 0) {
    return;
  }
  for (const auto& [object, s] : g_surfaces) {
    if (s.group == g_family_primary) {
      out.push_back(object);
    }
  }
}

std::uint32_t GroupOf(std::uint32_t object) {
  if (object == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  const auto it = g_surfaces.find(object);
  return it == g_surfaces.end() ? object : it->second.group;
}

}  // namespace dpour_surfaces
