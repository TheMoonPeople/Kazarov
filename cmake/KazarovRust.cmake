# Import the shared Rust extractor, desktop setup and C++ bridge once per build.
include_guard(GLOBAL)
include(FetchContent)
if(DEFINED ENV{KZ_CORROSION_SOURCE_DIR} AND
   EXISTS "$ENV{KZ_CORROSION_SOURCE_DIR}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_CORROSION "$ENV{KZ_CORROSION_SOURCE_DIR}" CACHE PATH
        "Pinned Corrosion source bundled by the host toolchain" FORCE)
endif()
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(Rust_CARGO_TARGET x86_64-pc-windows-gnullvm CACHE STRING "Rust target for llvm-mingw" FORCE)
endif()
FetchContent_Declare(corrosion
    URL https://github.com/corrosion-rs/corrosion/archive/refs/tags/v0.6.1.zip
    URL_HASH SHA256=5c1c5c6ab7196fe4466321c14e9698ec2d0a977c89fd9f1639a37fcffdf604f2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(CORROSION_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(corrosion)

get_filename_component(KZ_RUST_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../Cargo.toml" ABSOLUTE)
corrosion_import_crate(MANIFEST_PATH ${KZ_RUST_MANIFEST} CRATES kazarov_core)
find_program(KZ_FFMPEG_EXECUTABLE NAMES ffmpeg.exe ffmpeg HINTS "${KZ_FFMPEG_DIR}/bin")
if(KZ_FFMPEG_EXECUTABLE)
    corrosion_set_env_vars(kazarov_core "KAZAROV_FFMPEG=${KZ_FFMPEG_EXECUTABLE}")
endif()
corrosion_add_cxxbridge(kazarov_bridge CRATE kazarov_core FILES rust/lib.rs)

# Corrosion imports Rust's standard native libraries, but not the GUI crates'
# additional OS dependencies when their static archive is linked from C++.
if(WIN32)
    target_link_libraries(kazarov_core INTERFACE imm32 uxtheme opengl32 dwmapi)
elseif(APPLE)
    target_link_libraries(kazarov_core INTERFACE
        "-framework AppKit" "-framework Carbon" "-framework CoreFoundation" "-framework CoreGraphics"
        "-framework CoreVideo" "-framework Foundation" "-framework IOKit"
        "-framework OpenGL" "-framework QuartzCore" "-framework UniformTypeIdentifiers" objc)
endif()
