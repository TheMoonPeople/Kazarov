#!/usr/bin/env bash
# Sends a build to the Homebrew Menu's netloader (Y) and prints what the game logs until it exits
# (docs/SWITCH.md, "Testing from a computer"). SWITCH_ADDRESS is the Switch's IPv4 address
# (nxlink broadcasts for it without one); the image is port/switch/build.sh's.
#   port/switch/run.sh [--nro FILE] [--path PATH] [-- game arguments...]
set -euo pipefail

root=${KZ_SWITCH_ROOT:-$PWD}
root=$(cd "$root" && pwd)
builder=${BUILDER:-docker}
nro=${SWITCH_NRO:-$root/port/build/switch/game.nro}
path=${SWITCH_INSTALL_PATH:-game/game.nro}
while [ $# -gt 0 ]; do
    case $1 in
        --nro) nro=$(realpath "$2"); shift 2 ;;
        --path) path=$2; shift 2 ;;
        --) shift; break ;;
        *) echo "usage: run.sh [--nro FILE] [--path PATH] [-- game arguments...]" >&2; exit 2 ;;
    esac
done
address=()
[ -n "${SWITCH_ADDRESS:-}" ] && address=(-a "$SWITCH_ADDRESS")
user=(--user "$(id -u):$(id -g)")
[ "$builder" = podman ] && user=(--userns=keep-id)
dir=$(dirname "$nro")
# No --args without arguments: nxlink would pass one empty one, which the game takes for a bad option.
args=()
[ $# -gt 0 ] && args=(--args "$*")
# The host's network, for nxlink's broadcast and for the game's connection back to its log server.
# stdbuf: nxlink's output is the game's log, line by line even into a file.
exec "$builder" run --rm --network host "${user[@]}" -v "$dir:$dir:z" "${KZ_SWITCH_IMAGE:-kazarov-switch:latest}" \
    stdbuf -oL -eL nxlink "${address[@]}" -r 5 -p "$path" -s "$nro" "${args[@]}"
