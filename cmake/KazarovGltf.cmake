# Standard model loading shared by host applications and game ports.
include_guard(GLOBAL)
include(FetchContent)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
# The memory pool is built on std::pmr, which the system libc++ has only from iOS 17: below that the
# app would import a symbol dyld cannot find and be killed before main.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 17.0)
    set(FASTGLTF_DISABLE_CUSTOM_MEMORY_POOL ON CACHE BOOL "" FORCE)
endif()
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simdjson
    URL https://codeload.github.com/simdjson/simdjson/tar.gz/refs/tags/v4.6.11
    URL_HASH SHA256=61d948fc24f0d793829ad658058e7597d064988a89b4607ea02e401a82df98ff
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(simdjson)
FetchContent_Declare(fastgltf
    URL https://codeload.github.com/spnda/fastgltf/tar.gz/refs/tags/v0.9.1
    URL_HASH SHA256=d8705a496deb2831775baeef5009be36d24bf3d10f8d29ab016e977d0119c872
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(fastgltf)
