# Bundle only redistributable program dependencies; never game data or disc files.
include_guard(GLOBAL)

# Build events must be attached in the directory that creates the executable.
function(kz_windows_runtime_post_build target)
    if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        return()
    endif()
    get_target_property(attached ${target} KZ_WINDOWS_RUNTIME_POST_BUILD)
    if(attached)
        return()
    endif()
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(KZ_LLVM_READOBJ NAMES llvm-readobj
        HINTS "${compiler_bin}" NO_DEFAULT_PATH REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/build/windows-import-library.py"
            --runtime-executable "$<TARGET_FILE:${target}>"
            --compiler-bin "${compiler_bin}" --readobj "${KZ_LLVM_READOBJ}"
        COMMENT "Bundling the compiler runtime imported by ${target}"
        VERBATIM)
    set_property(TARGET ${target} PROPERTY KZ_WINDOWS_RUNTIME_POST_BUILD TRUE)
endfunction()

function(kz_windows_runtime target)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(KZ_RUNTIME "INSTALL" "" "EXECUTABLES" ${ARGN})
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(rust_bin "${Rust_COMPILER}" DIRECTORY)
    find_file(KZ_LIBCXX_DLL libc++.dll HINTS "${compiler_bin}"
        "${compiler_bin}/../x86_64-w64-mingw32/bin" NO_DEFAULT_PATH REQUIRED)
    find_file(KZ_PTHREAD_DLL libwinpthread-1.dll HINTS "${compiler_bin}"
        "${compiler_bin}/../x86_64-w64-mingw32/bin" NO_DEFAULT_PATH REQUIRED)
    # Rust's unwind runtime also serves the C++ code; do not overwrite it with an older copy.
    find_file(KZ_UNWIND_DLL libunwind.dll HINTS "${rust_bin}" NO_DEFAULT_PATH REQUIRED)

    set(KZ_VULKAN_RUNTIME_DIR "" CACHE PATH "Directory containing the redistributable Vulkan loader")
    if(NOT EXISTS "${KZ_VULKAN_RUNTIME_DIR}/vulkan-1.dll")
        include(FetchContent)
        FetchContent_Declare(kz_vulkan_runtime
            URL https://sdk.lunarg.com/sdk/download/1.4.363.0/windows/VulkanRT-X64-1.4.363.0-Components.zip
            URL_HASH SHA256=a25a927aa8b9f0371048f1861cf88ac3b9bc9b1fb332c42d897c8ab32695769a
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
        FetchContent_MakeAvailable(kz_vulkan_runtime)
        find_file(KZ_VULKAN_LOADER vulkan-1.dll
            PATHS "${kz_vulkan_runtime_SOURCE_DIR}/x64"
                "${kz_vulkan_runtime_SOURCE_DIR}/VulkanRT-X64-1.4.363.0-Components/x64"
            NO_DEFAULT_PATH REQUIRED)
    else()
        set(KZ_VULKAN_LOADER "${KZ_VULKAN_RUNTIME_DIR}/vulkan-1.dll")
    endif()
    get_property(ffmpeg GLOBAL PROPERTY KZ_FFMPEG_RUNTIME_DLLS)
    if(NOT ffmpeg)
        message(FATAL_ERROR "kz_windows_runtime must follow kz_ffmpeg initialization")
    endif()
    set(runtime "$<TARGET_FILE:SDL3::SDL3>" "${KZ_LIBCXX_DLL}" "${KZ_PTHREAD_DLL}"
        "${KZ_UNWIND_DLL}" "${KZ_VULKAN_LOADER}" ${ffmpeg})
    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        find_program(KZ_LLVM_READOBJ NAMES llvm-readobj
            HINTS "${compiler_bin}" NO_DEFAULT_PATH REQUIRED)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        # File-name components avoid CMP0112's dependency on the executable itself.
        cmake_policy(PUSH)
        cmake_policy(SET CMP0112 NEW)
        set(preflight "${target}_kz_runtime_preflight")
        add_custom_target(${preflight}
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/build/windows-import-library.py"
                --runtime-executable "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>"
                --skip-missing-executable
                --compiler-bin "${compiler_bin}" --readobj "${KZ_LLVM_READOBJ}"
            VERBATIM)
        add_dependencies(${target} ${preflight})
        cmake_policy(POP)
        get_target_property(target_source_dir ${target} SOURCE_DIR)
        if(target_source_dir STREQUAL CMAKE_CURRENT_SOURCE_DIR)
            kz_windows_runtime_post_build(${target})
        else()
            get_target_property(attached ${target} KZ_WINDOWS_RUNTIME_POST_BUILD)
            if(NOT attached)
                message(FATAL_ERROR
                    "Call kz_windows_runtime_post_build(${target}) in ${target_source_dir} before kz_windows_runtime")
            endif()
        endif()
    endif()
    # A dependency target restores missing DLLs even when the executable needs no relink. Each copy is a
    # declared output, so an up-to-date build runs nothing.
    get_target_property(runtime_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT runtime_dir)
        get_target_property(runtime_dir ${target} BINARY_DIR)
    endif()
    string(MD5 directory_id "${runtime_dir}")
    set(copy_target "kz_runtime_${directory_id}")
    if(NOT TARGET ${copy_target})
        set(copies "")
        foreach(path IN LISTS runtime)
            # OUTPUT cannot name a target's file, so an imported DLL is copied from its configured location.
            if(path MATCHES "^\\$<TARGET_FILE:(.+)>$")
                get_target_property(path ${CMAKE_MATCH_1} LOCATION)
            endif()
            get_filename_component(name "${path}" NAME)
            set(copy "${runtime_dir}/${name}")
            add_custom_command(OUTPUT "${copy}"
                COMMAND ${CMAKE_COMMAND} -E copy "${path}" "${copy}"
                DEPENDS "${path}"
                VERBATIM)
            list(APPEND copies "${copy}")
        endforeach()
        add_custom_target(${copy_target} DEPENDS ${copies})
    endif()
    add_dependencies(${target} ${copy_target})
    if(KZ_RUNTIME_INSTALL)
        install(FILES ${runtime} DESTINATION ${CMAKE_INSTALL_BINDIR})
        set(names "$<TARGET_FILE_NAME:${target}>\n$<TARGET_FILE_NAME:SDL3::SDL3>\n")
        foreach(executable IN LISTS KZ_RUNTIME_EXECUTABLES)
            string(APPEND names "$<TARGET_FILE_NAME:${executable}>\n")
        endforeach()
        foreach(path "${KZ_LIBCXX_DLL}" "${KZ_PTHREAD_DLL}" "${KZ_UNWIND_DLL}" "${KZ_VULKAN_LOADER}" ${ffmpeg})
            get_filename_component(name "${path}" NAME)
            string(APPEND names "${name}\n")
        endforeach()
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/windows-runtime-files.txt" CONTENT "${names}")
    endif()
endfunction()
