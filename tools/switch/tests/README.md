# Native Switch runtime regression tests

These tests use the same indexed ZIP reader as the Switch runtime. A native C++23 compiler, zlib and Zstandard development libraries are required. Python uses only its standard library and loads the system Zstandard library with `ctypes` for independent comparison.

From the Kazarov checkout:

```sh
c++ -std=c++23 -O2 -Iinclude tools/switch/tests/archive_probe.cpp tools/switch/src/zip_archive.cpp -lzstd -lz -o /tmp/kazarov-archive-probe
python3 tools/switch/tests/archive_regression.py --probe /tmp/kazarov-archive-probe
python3 tools/switch/tests/archive_regression.py --probe /tmp/kazarov-archive-probe --archive /path/to/data.zip
```

The fixtures exercise stored, deflated and Zstandard files, empty deflate/Zstandard streams, UTF-8 names, directories and a ZIP64 archive with 65,536 entries. Corrupted CRCs, inconsistent local names/methods, truncation and invalid trailers must fail. The optional archive check compares selected real assets against Python's ZIP index and independent Zstandard decompression. Timing output describes the host and storage used for that run; it does not establish console performance.

For the complete native suite, use a C++26 compiler and configure this directory as a standalone CMake project:

```sh
cmake -S tools/switch/tests -B /tmp/kazarov-switch-tests -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build /tmp/kazarov-switch-tests
ctest --test-dir /tmp/kazarov-switch-tests --output-on-failure
```

The exit-registry test compiles the real runtime C source against an ABI fixture matching newlib's full 64-bit table layout. It checks more than 32 callbacks in LIFO order, DSO filtering and repeated finalization, plain/status/C++ callback slot reuse, registration during callbacks, nested finalization, a callback waiting for a registering thread, and 1,024 registrations across eight threads. Linux additionally injects an allocation failure and verifies that existing callbacks survive and further registrations recover. The source also compiles against the Switch toolchain's actual headers; the fixture does not replace those headers in the game build.
