# The shared PC port in <kazarov>/src/cpp: the gfx, audio and platform libraries, the sce sources a
# consumer compiles, and the shared GoogleTest cases.
#
# Host code never sees a game header. sce sources are compiled by the consumer with its own
# ps2/include/sce on the path and its port.h force-included. The game defines SettingsGet() and
# SettingsChange(const HostSettings &), the definitions behind platform/settings.hpp.

include_guard(GLOBAL)

get_filename_component(KZ_PORT_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../include" ABSOLUTE)
get_filename_component(KZ_PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src/cpp" ABSOLUTE)

# Takes the file from the first KZ_PORT_VENDOR_DIRS entry that holds it. Otherwise it is fetched at its
# pinned hash, which is what a build without a vendor copy does; the Flatpak build has no network.
function(_kz_port_vendor out_dir name url sha256)
    foreach(dir IN LISTS KZ_PORT_VENDOR_DIRS)
        if(EXISTS "${dir}/${name}")
            set(${out_dir} "${dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(dir "${CMAKE_BINARY_DIR}/kz_port/vendor")
    file(DOWNLOAD ${url} "${dir}/${name}" EXPECTED_HASH SHA256=${sha256})
    set(${out_dir} "${dir}" PARENT_SCOPE)
endfunction()

function(_kz_port_headers_target)
    if(NOT TARGET kz_port_headers)
        add_library(kz_port_headers INTERFACE)
        target_include_directories(kz_port_headers INTERFACE ${KZ_PORT_INCLUDE_ROOT})
    endif()
endfunction()

function(kz_port_host_library target)
    if(NOT TARGET SDL3::SDL3)
        find_package(SDL3 REQUIRED CONFIG)
    endif()
    if(NOT TARGET Vulkan::Vulkan)
        if(ANDROID)
            # The Android NDK ships the Vulkan headers and dispatch loader, but not
            # CMake's Vulkan SDK package or a versioned loader library.
            find_library(KZ_VULKAN_LIBRARY vulkan REQUIRED)
            add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
            set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION "${KZ_VULKAN_LIBRARY}")
        else()
            find_package(Vulkan 1.4 REQUIRED)
        endif()
    endif()
    if(NOT GLSLANG_VALIDATOR)
        find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)
    endif()
    if(NOT TARGET nlohmann_json::nlohmann_json)
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
            FIND_PACKAGE_ARGS CONFIG)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()

    _kz_port_vendor(dr_wav_dir dr_wav.h
        https://raw.githubusercontent.com/mackron/dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_wav.h
        03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493)
    _kz_port_vendor(dr_flac_dir dr_flac.h
        https://raw.githubusercontent.com/mackron/dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_flac.h
        111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c)
    _kz_port_vendor(stb_vorbis_dir stb_vorbis.c
        https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c
        4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636)
    _kz_port_vendor(stb_truetype_dir stb_truetype.h
        https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h
        ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab)
    _kz_port_vendor(stb_image_dir stb_image.h
        https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
        594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
    set(vendor_dirs ${dr_wav_dir} ${dr_flac_dir} ${stb_vorbis_dir} ${stb_truetype_dir} ${stb_image_dir})
    list(REMOVE_DUPLICATES vendor_dirs)

    set(generated_dir "${CMAKE_BINARY_DIR}/kz_port/generated")
    file(GLOB shader_sources CONFIGURE_DEPENDS ${KZ_PORT_ROOT}/gfx/shaders/*.vert ${KZ_PORT_ROOT}/gfx/shaders/*.frag)
    set(shader_headers "")
    get_property(shaders_defined GLOBAL PROPERTY KZ_PORT_SHADERS_DEFINED)
    if(NOT shaders_defined)
        # A second host library in the same project would declare the same outputs.
        set_property(GLOBAL PROPERTY KZ_PORT_SHADERS_DEFINED ON)
    endif()
    foreach(shader IN LISTS shader_sources)
        get_filename_component(shader_name ${shader} NAME)
        string(REPLACE "." "_" shader_symbol ${shader_name})
        set(header "${generated_dir}/shaders/${shader_name}.spv.hpp")
        list(APPEND shader_headers ${header})
        if(NOT shaders_defined)
            add_custom_command(OUTPUT ${header}
                COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 --quiet --vn ${shader_symbol}
                    --depfile ${header}.d -o ${header} ${shader}
                DEPENDS ${shader}
                DEPFILE ${header}.d
                COMMENT "Compiling ${shader_name} to SPIR-V")
        endif()
    endforeach()

    file(GLOB host_sources CONFIGURE_DEPENDS
        ${KZ_PORT_ROOT}/gfx/*.cpp ${KZ_PORT_ROOT}/audio/*.cpp ${KZ_PORT_ROOT}/platform/*.cpp)
    option(KZ_PORT_BUILD_TIM2 "Build the legacy TIM2 decoder into the host library" ON)
    if(NOT KZ_PORT_BUILD_TIM2)
        list(FILTER host_sources EXCLUDE REGEX "/gfx/tim2\\.cpp$")
    endif()

    _kz_port_headers_target()
    add_library(${target} OBJECT ${host_sources} ${shader_headers})
    target_include_directories(${target} PRIVATE ${KZ_PORT_INCLUDE_ROOT} ${generated_dir} ${vendor_dirs})
    target_link_libraries(${target} PRIVATE SDL3::SDL3 Vulkan::Vulkan nlohmann_json::nlohmann_json)
    # The game's replacement units include "gfx/gfx.hpp", so the include root is part of this library's interface.
    target_link_libraries(${target} PUBLIC kz_port_headers)
    if(WIN32)
        # platform/game_icon.cpp's shortcut beside the executable (IShellLinkW).
        target_link_libraries(${target} PUBLIC ole32 uuid)
    endif()
    target_compile_options(${target} PRIVATE -Wall -ffunction-sections -fdata-sections)
    # arm64 clang fuses a*b+c into one rounding and x86-64 has no FMA in its baseline. The logical-to-pixel
    # mapping floors such sums, so a fused one lands a whole pixel off on arm64 alone.
    target_compile_options(${target} PRIVATE -ffp-contract=off)
endfunction()

function(kz_port_sce_sources out_var)
    cmake_parse_arguments(KZ_SCE "" "" "LIBS" ${ARGN})
    set(result "")
    foreach(family IN LISTS KZ_SCE_LIBS)
        set(found FALSE)
        foreach(candidate "${KZ_PORT_ROOT}/sce/${family}.cpp" "${KZ_PORT_ROOT}/sce/stubs/${family}.cpp")
            if(EXISTS "${candidate}")
                list(APPEND result "${candidate}")
                set(found TRUE)
            endif()
        endforeach()
        if(NOT found)
            message(FATAL_ERROR "kz_port_sce_sources: no sce/ or sce/stubs/ source for SDK family '${family}'")
        endif()
    endforeach()
    list(APPEND result "${KZ_PORT_ROOT}/sce/unimplemented.cpp")
    set(${out_var} ${result} PARENT_SCOPE)
endfunction()

function(kz_port_tests target)
    _kz_port_headers_target()
    file(GLOB tests CONFIGURE_DEPENDS ${KZ_PORT_ROOT}/tests/*_test.cpp)
    target_sources(${target} PRIVATE ${tests})
    target_include_directories(${target} PRIVATE ${KZ_PORT_INCLUDE_ROOT}/tests)
    target_link_libraries(${target} PRIVATE kz_port_headers)
endfunction()
