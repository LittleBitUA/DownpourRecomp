# downpour - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(downpour LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(DOWNPOUR_SOURCES
    src/main.cpp
    src/downpour_title_update_installer.cpp
)

# dpour-fork 2026-07-25: one switch that builds the game with NO native render
# at all - not merely with it disabled at runtime.
#
# Every native-render feature is already off unless its environment variable is
# set, but "off at runtime" is an argument, and an argument is not a measurement.
# Configuring with -DDPOUR_NO_NATIVE_RENDER=ON produces a binary that cannot
# contain any of it, so a frame-time comparison between the two settles whether
# this code costs anything at all.
option(DPOUR_NO_NATIVE_RENDER "Build without the native render layer entirely" OFF)
if(NOT DPOUR_NO_NATIVE_RENDER)
    list(APPEND DOWNPOUR_SOURCES
    # === DPOUR MIGRATION 2026-07-24: native-render Phase 0 hook harness +
    # Phase 1a isolated D3D12 device bring-up. Remove these lines + delete the
    # src/downpour_native_render.cpp / src/downpour_native_d3d12.cpp files to
    # fully revert.
    src/downpour_native_render.cpp
    src/downpour_native_d3d12.cpp
    # Phase 1b: on-screen native overlay (UIDrawer on the runtime's swapchain).
    src/downpour_native_overlay.cpp
    # Phase 2: RHI render-path hooks (log-only proof of resolved addresses).
    src/downpour_native_draws.cpp
    # Phase 3: capture the game's real geometry + draw it with native D3D12.
    src/downpour_native_geo.cpp
    # Phase 4: decode the game's Xenos textures into native D3D12 textures.
    src/downpour_native_tex.cpp
    # Phase 5: read the game's vertex declarations so every vertex factory
    # (BSP, terrain, foliage, decals) is captured, not just static meshes.
    src/downpour_native_decl.cpp
    # Phase 3 (shaders): harvest Xenos shader containers from guest RAM so
    # XenosRecomp can translate them to DXIL offline.
    src/downpour_shader_dump.cpp
    # Phase 5 (shaders): load those translated shaders back and pair them with
    # whatever the game binds - the game's own materials instead of ours.
    src/downpour_native_shaders.cpp
    # Phase 5 (shaders): the root signature / PSO / constant plumbing those
    # translated shaders require. The layout is dictated by the DXIL, not by us.
    src/downpour_native_pipeline.cpp
    # Phase 5 (shaders): host vertex/index buffers with the dword byte swap the
    # reference does at Unlock - see the header for why the moment differs here.
    src/downpour_native_vbuffers.cpp
    # Phase 5 (shaders): the guest's VS/PS constant banks, which the translated
    # shaders address by register number.
    src/downpour_native_constants.cpp
    # Phase 5 (draw path): the guest's pipeline state, read out of the Xenos
    # register shadow inside its D3D device - the XDK inlines SetRenderState, so
    # there is no function to hook and the shadow IS the state.
    src/downpour_native_state.cpp
    # Phase 5 (draw path): the game's own draws, replayed on our own D3D12
    # pipeline into our own targets. This is what all of the above was for.
    src/downpour_native_scene.cpp
    # Phase 6 (data-driven layer, the skate3recomp approach): the ENGINE's own
    # per-frame mesh stream, tapped at FMeshDrawingPolicy::DrawMesh. Carries
    # what the D3D layer cannot recover - transform, material, priority group.
    src/downpour_native_ue3.cpp
    )
endif()
if(DPOUR_NO_NATIVE_RENDER)
    add_compile_definitions(DPOUR_NO_NATIVE_RENDER=1)
endif()

# dpour-fork 2026-07-03: embed the unique game icon on Windows so
# downpour.exe shows the silhouette+doorway art in Explorer / taskbar /
# Alt-Tab (distinct from the launcher's Murphy portrait).
if(WIN32)
    list(APPEND DOWNPOUR_SOURCES src/downpour.rc)
endif()

if(WIN32)
    add_executable(downpour WIN32 ${DOWNPOUR_SOURCES})
else()
    add_executable(downpour ${DOWNPOUR_SOURCES})
endif()

rexglue_setup_target(downpour)

# Title-update installer wizard needs WinHTTP (download) and Common Dialogs
# (file picker) on Windows. On other platforms the installer uses curl and
# GTK, which the SDK already wires up.
if(WIN32)
    target_link_libraries(downpour PRIVATE winhttp comdlg32)
    # === DPOUR MIGRATION 2026-07-24: native-render Phase 1a/1b need D3D12/DXGI +
    # d3dcompiler (Phase 1b compiles the overlay shaders at runtime) ===
    target_link_libraries(downpour PRIVATE d3d12 dxgi d3dcompiler)
endif()
