#!/usr/bin/env bash
# Build the game for this computer and run the capture script - the macOS,
# Linux and Git Bash counterpart of sim.ps1, which ./build.sh --sim runs.
#
#   ./tools/sim.sh
#
# src/ has no N64 dependencies, so the same sources the ROM is built from
# compile with the container's host gcc (tools/sim_run.sh). The frames land
# in build/sim, and build/sim/sheet.png is the contact sheet.
#
# Written for bash 3.2, the version macOS ships.

root=$(cd "$(dirname "$0")/.." && pwd)

die() { printf '%s\n' "$*" >&2; exit 1; }

# Git Bash on Windows: see build.sh. Docker gets the project as a Windows
# path, and MSYS leaves its other paths alone; Python still needs them
# rewritten.
case $(uname -s) in
    MINGW* | MSYS* | CYGWIN*) windows=1; host_root=$(cygpath -m "$root") ;;
    *)                        windows=0; host_root=$root ;;
esac
run_docker() {
    if [ $windows = 1 ]; then
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' docker "$@"
    else
        docker "$@"
    fi
}

python=
for p in python3 python; do
    if command -v "$p" >/dev/null 2>&1 &&
            "$p" -c 'import sys; sys.exit(sys.version_info[0] < 3)' 2>/dev/null; then
        python=$p
        break
    fi
done
[ -n "$python" ] || die "Python 3 is needed - install it and try again"

if [ ! -f "$root/assets/wolf3d.dat" ]; then
    "$python" "$root/tools/extract_assets.py" || die "asset extraction failed"
fi

mkdir -p "$root/build/sim" || die "cannot make $root/build/sim"
rm -f "$root/build/sim/"*.ppm

# Run the container as you, so that what it writes into build/ is yours and
# not root's (on macOS Docker Desktop sees to that anyway) - except under
# rootless Docker, whose root already is you, and on Windows.
user=(--user "$(id -u):$(id -g)")
if [ $windows = 1 ] ||
        docker info --format '{{json .SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    user=()
fi
run_docker run --rm "${user[@]}" -e HOME=/tmp -v "$host_root:/src" -w /src \
    n64-libdragon bash tools/sim_run.sh ||
    die "host build or run failed"

"$python" "$root/tools/sheet.py" "$root/build/sim" 3 2
