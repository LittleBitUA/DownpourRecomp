# downpour - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(downpour LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === DPOUR MIGRATION 2026-08-01: CPU baseline (issue #11) =====================
#
# We were shipping -march=x86-64-v3, which MANDATES AVX2 + BMI2 + FMA. Measured
# on the shipped rexruntimerd.dll: 4898 AVX2-only instructions across 259
# distinct functions, including std::_Hash_vec::_Assign_grow,
# VirtualFileSystem::UnregisterSymbolicLink and XexModule::ApplyPatch - i.e.
# ordinary whole-program auto-vectorisation, not runtime-dispatched kernels. So
# it is a hard requirement, not a fast path.
#
# x86-64-v3 means Intel Haswell (2013) / AMD Excavator (2015) or newer. The
# reporter in issue #11 has an FX-8300 (Piledriver, 2012): it HAS AVX, and no
# AVX2. downpour.exe imports rexruntimerd.dll statically, so the bad
# instructions are reached during DLL init, before main() and before logging -
# which is exactly what was reported: 0xc0000142 (STATUS_DLL_INIT_FAILED) and a
# log folder that never appears.
#
# NOTE the thread on issue #11 calls this "AVX". That is wrong and it matters:
# the affected owners DO have AVX. The line is AVX2.
#
# Copied from skate3recomp (_ref_skate3/CMakeLists.txt:92-137), including the
# part that is easy to miss: Snappy runs its OWN feature detection and does not
# see directory-level compile options, so lowering -march alone still leaves
# AVX2/BMI2 code in the binary. Their comment says so verbatim.
#
# Default OFF = the compatible build, as in the reference.
option(DPOUR_ENABLE_X86_64_V3
    "Build x86_64 desktop binaries for x86-64-v3 CPUs (AVX2/BMI2)"
    OFF)

if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if(DPOUR_ENABLE_X86_64_V3)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64-v3)
            set(SNAPPY_REQUIRE_AVX2 ON CACHE BOOL
                "Match Snappy feature detection to the project x86-64-v3 target" FORCE)
            set(SNAPPY_HAVE_SSSE3 1 CACHE INTERNAL
                "x86-64-v3 requires SSSE3; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_X86_CRC32 1 CACHE INTERNAL
                "x86-64-v3 requires SSE4.2 CRC32; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_BMI2 1 CACHE INTERNAL
                "x86-64-v3 requires BMI2; Snappy checks do not see directory compile options" FORCE)
        elseif(MSVC)
            add_compile_options(/arch:AVX2)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64 -mtune=generic -mssse3)
        endif()
        set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL
            "Do not require AVX for the compatibility build" FORCE)
        set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL
            "Do not require AVX2 for the compatibility build" FORCE)
        set(SNAPPY_HAVE_SSSE3 0 CACHE INTERNAL
            "Keep Snappy on its scalar path for the compatibility build" FORCE)
        set(SNAPPY_HAVE_X86_CRC32 0 CACHE INTERNAL
            "Compatibility build must not require SSE4.2 CRC32" FORCE)
        set(SNAPPY_HAVE_BMI2 0 CACHE INTERNAL
            "Compatibility build must not require BMI2" FORCE)
    endif()
endif()

include(generated/rexglue.cmake)

# Sources
set(DOWNPOUR_SOURCES
    src/main.cpp
    src/downpour_title_update_installer.cpp
)

# dpour-fork 2026-08-08: the native render layer is RETIRED by owner decision.
# All of its sources and docs live in _archived_native_render/; the guard
# macro below keeps the app-side hooks compiled out. The game renders through
# the SDK's emulated path only.
add_compile_definitions(DPOUR_NO_NATIVE_RENDER=1)

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
