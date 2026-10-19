cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    project(kazarov NONE)
endif()

# Entry point of the Kazarov build rules.
#
# Set these before including it (paths are relative to CMAKE_SOURCE_DIR):
#   KZ_MWCC, KZ_MWLD       compiler and linker executables
#   KZ_LIB_INCLUDE_DIRS    list, the compiler's system-header search path
#   KZ_MWCC_RUNNER         list, the runner for the compiler (default wibo)
#   KZ_AS_FLAGS            list, extra flags for every assembly
# Optional: KZ_AS, KZ_OBJCOPY, KZ_OBJDUMP, KZ_READELF, KZ_WIBO, MIPS_TOOL_PREFIX,
# and for overlays KZ_OVERLAYS, KZ_OVERLAY_ORIGIN, KZ_OVERLAY_HEADER_SIZE,
# KZ_OVERLAY_FILE_ALIGN (see KazarovOverlays.cmake).

set(KZ_ROOT "${CMAKE_CURRENT_LIST_DIR}")

include("${KZ_ROOT}/cmake/KazarovToolchain.cmake")
include("${KZ_ROOT}/cmake/KazarovObjects.cmake")
include("${KZ_ROOT}/cmake/KazarovSetup.cmake")
include("${KZ_ROOT}/cmake/KazarovOverlays.cmake")
include("${KZ_ROOT}/cmake/KazarovContext.cmake")
