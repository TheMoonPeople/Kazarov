#!/usr/bin/env bash
# Builds the Nintendo Switch homebrew port (docs/SWITCH.md) with Kazarov tools/switch/Containerfile
# describes, building that image first when this checkout's Containerfile has no image yet.
# Docker by default; BUILDER can select another OCI-compatible runtime. The arguments go to the
# build (targets, -v); `port/switch/build.sh shell` opens a shell in the image instead.
set -euo pipefail

root=${KZ_SWITCH_ROOT:-$PWD}
root=$(cd "$root" && pwd)
shared=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
builder=${BUILDER:-docker}
jobs=${JOBS:-4}
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "switch: JOBS must be a positive integer: $jobs" >&2
    exit 2
fi
[ -f "$root/tools/kazarov/include/kazarov/port.h" ] || git -C "$root" submodule update --init tools/kazarov

preset=${SWITCH_PRESET:-switch}
containerfile=$shared/Containerfile
name=kazarov-switch
if [ "$preset" != switch ]; then
    echo "unsupported Switch preset: $preset" >&2
    exit 2
fi

# Include every patch in the tag so editing one rebuilds the image.
tag=$({
    cat "$containerfile" "$shared"/build-mesa.sh \
        "$shared"/bindgen-switch-wrapper.sh \
        "$shared"/rustc-switch-wrapper.sh \
        "$shared"/bindgen-atomic-shim.h \
        "$shared"/switch_cross_file.txt \
        "$shared"/switch_native_file.txt \
        "$shared"/../ffmpeg/kz-ffmpeg \
        "$shared"/patches/*.patch
} | sha256sum | cut -c1-12)
image=${KZ_SWITCH_IMAGE:-$name:$tag}
if ! "$builder" image inspect "$image" > /dev/null 2>&1; then
    if [ -n "${KZ_SWITCH_IMAGE:-}" ]; then
        echo "switch: requested image does not exist: $image" >&2
        exit 2
    fi
    echo "switch: building $image (the first build takes a while)" >&2
    context=$(mktemp -d -t kazarov-switch-context-XXXXXXXX)
    trap 'rm -rf "$context"' EXIT
    cp -a "$shared/." "$context/"
    cp "$shared/../ffmpeg/kz-ffmpeg" "$context/kz-ffmpeg"
    "$builder" build --build-arg "BUILD_JOBS=$jobs" -t "$image" -t "$name:latest" -f "$context/Containerfile" "$context"
    rm -rf "$context"
    trap - EXIT
fi

# The build runs as the caller, so the tree it writes stays theirs.
if [ "$builder" = podman ]; then
    user=(--userns=keep-id)
else
    user=(--user "$(id -u):$(id -g)")
fi
interactive=()
[ -t 0 ] && interactive=(-it)
run=("$builder" run --rm "${interactive[@]}" "${user[@]}" -e HOME=/tmp -e "PRESET=$preset" -e "CLEAN=${CLEAN:-}" -e "JOBS=$jobs" -e "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-$jobs}" -e "CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-$jobs}" -v "$root:/work:z" -w /work "$image")

if [ "${1:-}" = shell ]; then
    shift
    exec "${run[@]}" bash "$@"
fi

# A container run without --user (for example an interactive `docker run`) leaves root-owned files
# in the build tree. CMake then cannot regenerate them and fails with unrelated errors such as
# "Target nlohmann_json::nlohmann_json not found", so remove them as root (chown is not honoured
# on Windows drives); the build recreates them.
builddir=$root/port/build/$preset
if [ "$builder" = docker ] && [ -d "$builddir" ] && [ -n "$(find "$builddir" ! -user "$(id -u)" -print -quit)" ]; then
    echo "switch: removing files in port/build/$preset owned by another user" >&2
    "$builder" run --rm -v "$root:/work:z" -w /work "$image" \
        find "port/build/$preset" ! -user "$(id -u)" -delete
fi

"${run[@]}" bash -c '
set -euo pipefail
if [ -n "$CLEAN" ] && [ "$CLEAN" != 0 ]; then
    rm -rf "port/build/$PRESET"
fi
echo "switch: configuring"
cmake --preset "$PRESET" > /dev/null
build_args=()
if [ -n "$JOBS" ]; then
    build_args+=(--parallel "$JOBS")
fi
exec cmake --build --preset "$PRESET" "${build_args[@]}" "$@"
' build "$@"

echo "switch: NVK compiles shaders at startup and saves a cache for later launches" >&2
