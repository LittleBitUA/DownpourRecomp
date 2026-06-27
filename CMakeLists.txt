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
endif()
