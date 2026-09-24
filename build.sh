#!/usr/bin/env bash
# Build the ROM on macOS or Linux - or from Git Bash on Windows: "Wolfenstein
# 3D.z64", "Spear of Destiny.z64", or --name whatever you like. It does what
# build.ps1 does, with the same options spelled the Unix way.
#
#   ./build.sh                  # this help, and what the build needs
#   ./build.sh --rom            # make
#   ./build.sh --assets         # extract the assets again first
#   ./build.sh --clean          # make clean, then build
#   ./build.sh --sim            # host build + capture instead of the ROM
#   ./build.sh --rom --name x   # build x.z64 instead of the usual name
#
# The assets come from your own copy of the game - the shareware (*.WL1),
# the full game (*.WL6) or Spear of Destiny (*.SOD), with its WOLF3D.EXE or
# SPEAR.EXE - in gamedata/ here unless --data names another folder; naming
# one always extracts from it. See README.md.
#
# --source, left out of the help on purpose, is for maintaining the port: it
# points at the WOLFSRC folder of id's source release, which checks the
# tables in tools/wolf_tables.py and can stand in for the executable's
# palette. A build never needs it (see docs/TECHNICAL_DETAILS.md).
#
# Everything else runs in Docker: the n64-libdragon image, which this builds
# from tools/Dockerfile.libdragon the first time it is missing.
#
# Written for bash 3.2, the version macOS ships: no associative arrays,
# ${var,,}, mapfile or set -u (which trips over an empty array there).

root=$(cd "$(dirname "$0")" && pwd)

die() { printf '%s\n' "$*" >&2; exit 1; }

# Git Bash (or MSYS2, or Cygwin) on Windows: Docker is a Windows program
# there. It is given the project folder as a Windows path (C:/...), and
# MSYS is told not to rewrite the container's own paths - /src, /tmp - into
# Windows ones, as it would every argument that looks like a Unix path.
# Only for Docker: Python, a Windows program too, needs that rewriting to
# find the files.
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

rom=0 assets=0 clean=0 sim=0 help=0
data= src_dir= name=
while [ $# -gt 0 ]; do
    case $1 in
        --rom)      rom=1 ;;
        --assets)   assets=1 ;;
        --clean)    clean=1 ;;
        --sim)      sim=1 ;;
        -h|--help)  help=1 ;;
        --data|--source|--name)
            [ $# -ge 2 ] || die "$1 needs a value (see ./build.sh --help)"
            case $1 in
                --data)   data=$2 ;;
                --source) src_dir=$2 ;;
                --name)   name=$2 ;;
            esac
            shift ;;
        --data=*)   data=${1#*=} ;;
        --source=*) src_dir=${1#*=} ;;
        --name=*)   name=${1#*=} ;;
        *) die "unknown option: $1 (see ./build.sh --help)" ;;
    esac
    shift
done

python=
for p in python3 python; do
    if command -v "$p" >/dev/null 2>&1 &&
            "$p" -c 'import sys; sys.exit(sys.version_info[0] < 3)' 2>/dev/null; then
        python=$p
        break
    fi
done

# A file in a folder, whatever the case of its name: DOS named the game's
# files in capitals, but not every copy keeps them that way, and Linux tells
# VSWAP.WL1 from vswap.wl1. Prints the path, or fails if there is none.
find_ci() {
    local want f lower
    want=$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')
    for f in "$1"/*; do
        [ -e "$f" ] || continue
        lower=$(basename "$f" | tr '[:upper:]' '[:lower:]')
        if [ "$lower" = "$want" ]; then
            printf '%s\n' "$f"
            return 0
        fi
    done
    return 1
}

# The blob's version - 0 the shareware, 1 the full game, 2 Spear of
# Destiny - the big-endian word at byte 10 of assets/wolf3d.dat
# (tools/extract_assets.py). Nothing if the assets are not extracted yet.
blob_version() {
    [ -f "$root/assets/wolf3d.dat" ] &&
        od -An -tu1 -j11 -N1 "$root/assets/wolf3d.dat" | tr -d ' \n'
}

# What the ROM is called. Each game gets a name of its own, because a
# cartridge is given its SRAM by the name of the ROM: emulators and flash
# carts keep the saves beside it, so two games sharing a name would share a
# cartridge - and its ten save slots and its high score table (see
# src/save.c).
# Spaces are fine - make builds the ROM inside build/ under a name of its
# own (internal_rom), and this one only goes on the copy of it - but not a
# folder, nor anything Windows does not allow in a file name.
rom_name() {
    local n
    if [ -n "$name" ]; then
        n=$(printf '%s' "$name" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
        case $n in
            *.z64) ;;
            *)     n=$n.z64 ;;
        esac
        case $n in
            .z64 | */* | *\\* | *[:*?\"\<\>\|]* | *[[:cntrl:]]*)
                die "--name must be a file name, without a folder or any of \\ / : * ? \" < > | - not: $name" ;;
        esac
        printf '%s\n' "$n"
    elif [ "$(blob_version)" = 2 ]; then
        echo "Spear of Destiny.z64"
    else
        echo "Wolfenstein 3D.z64"
    fi
}

# The ROM as make builds it, inside build/, under the game's own name: make
# cannot build a file with a space in its name (see the Makefile).
internal_rom() {
    if [ "$(blob_version)" = 2 ]; then echo build/spear.z64; else echo build/wolf3d.z64; fi
}

# The port's version, kept in one place: PORT_VERSION in src/wolf.h
port_version() {
    local v
    v=$(sed -n 's/^#define[[:space:]]*PORT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$root/src/wolf.h" | head -n 1)
    printf '%s\n' "${v:-unknown}"
}

# A game's name, by its files' extension or the blob's version
game_label() {
    case $1 in
        WL1 | 0) echo "the shareware episode" ;;
        WL6 | 1) echo "the full game" ;;
        SOD | 2) echo "Spear of Destiny" ;;
    esac
}

# ---------------------------------------------------------------------------
# With nothing to do, say what there is to do, and whether it can be done.

mark() { if [ "$1" = 1 ]; then printf '  ok     '; else printf '  MISSING'; fi; }

show_help() {
    local data_dir exts e ext games label f found=0 exe_names exe=0 rom_file
    local docker_cmd=0 docker_up=0 image=0 got
    data_dir=${data:-$root/gamedata}

    # whichever of the three games is there: the one with a VSWAP
    exts=
    for e in WL1 WL6 SOD; do
        find_ci "$data_dir" "VSWAP.$e" >/dev/null && exts="$exts $e"
    done
    set -- $exts
    games=$#
    if [ $games -eq 1 ]; then ext=$1; else ext=WL1; fi
    label=$(game_label $ext)
    for f in VSWAP GAMEMAPS MAPHEAD VGAGRAPH VGADICT VGAHEAD AUDIOT AUDIOHED; do
        find_ci "$data_dir" "$f.$ext" >/dev/null && found=$((found + 1))
    done
    # Spear of Destiny's is SPEAR.EXE, or WOLF3D.EXE in some releases
    if [ "$ext" = SOD ]; then exe_names="SPEAR.EXE WOLF3D.EXE"; else exe_names=WOLF3D.EXE; fi
    for f in $exe_names; do
        find_ci "$data_dir" "$f" >/dev/null && exe=1
    done
    if command -v docker >/dev/null 2>&1; then
        docker_cmd=1
        if docker info >/dev/null 2>&1; then
            docker_up=1
            docker image inspect n64-libdragon >/dev/null 2>&1 && image=1
        fi
    fi

    printf '\nWolfenstein 3D for Nintendo 64 v%s - build script\n' "$(port_version)"
    cat <<'EOF'

  ./build.sh --rom      build the ROM (extracting the assets first,
                         if that has not been done)
  ./build.sh --assets    extract the assets from your game files again,
                         then build what that changed
  ./build.sh --clean     throw away build/ and the ROMs and compile the
                         lot again; the extracted assets are kept, so
                         add --assets to redo those as well
  ./build.sh --sim       build the game for this computer instead and run
                         its checks; frames go to build/sim, see sheet.png
  ./build.sh --help      this help screen

  --data <folder>        where your game files are - the shareware, the
                         full game or Spear of Destiny (default: gamedata/
                         here); naming a folder extracts from it again
  --name <name>          what to call the ROM, instead of Wolfenstein 3D.z64
                         or Spear of Destiny.z64 - in quotes if it has spaces.
                         Each game is named apart because a cartridge's
                         saves and high scores follow the ROM's name

Examples:

  ./build.sh --rom
      build what is in gamedata/ - Wolfenstein 3D.z64, or Spear of
      Destiny.z64 if those are Spear of Destiny's files

  ./build.sh --rom --data ~/games/SPEAR
      build from another folder instead. Naming one always extracts it
      again, so this is how to switch from one game to another

  ./build.sh --rom --data ~/games/WOLF6 --name "Wolfenstein 3D (full)"
      call it Wolfenstein 3D (full).z64, so that the full game has a
      cartridge of its own - its saves and its high scores - beside the
      shareware's

  ./build.sh --assets
      read the game files in gamedata/ again and build - after putting
      other files there, say

  ./build.sh --sim
      no ROM: build the game for this computer and run its checks instead

What the build needs:
EOF
    if [ $games -gt 1 ]; then
        printf '  MISSING more than one game in %s (%s): give each its own folder\n' \
            "$data_dir" "$(echo $exts | sed 's/ /, /g')"
    elif [ $games -eq 0 ]; then
        printf '  MISSING your game files in %s: none found (VSWAP.WL1, .WL6 or .SOD)\n' \
            "$data_dir"
    else
        printf '%s %s in %s: %s of 8 .%s files\n' \
            "$(mark $([ $found = 8 ] && echo 1))" "$label" "$data_dir" "$found" "$ext"
    fi
    printf '%s %s there (the palette and sign-on screen)\n' \
        "$(mark $exe)" "$(echo $exe_names | sed 's/ / or /')"
    printf '%s Python 3 (the asset extractor)\n' "$(mark $([ -n "$python" ] && echo 1))"
    printf '%s Docker, installed\n' "$(mark $docker_cmd)"
    printf '%s Docker, running and yours to use\n' "$(mark $docker_up)"
    if [ $docker_up = 1 ]; then
        if [ $image = 1 ]; then
            echo "  ok      the n64-libdragon image"
        else
            echo "  later   the n64-libdragon image (the first build makes it; it takes a while)"
        fi
    fi
    # which game is extracted; if the folder holds another, a build reads it
    # only with --assets or --data - until then it would build the old one
    got=$(blob_version)
    if [ ! -f "$root/assets/wolf3d.dat" ]; then
        echo "  later   game data not extracted yet (--rom or --assets does it)"
    elif [ -n "$(game_label "$got")" ] && [ $games -eq 1 ] &&
            [ "$(game_label "$got")" != "$label" ]; then
        if [ $assets = 1 ] || [ -n "$data" ]; then
            echo "  later   game data extracted from $(game_label "$got"); this build reads $label from the folder instead"
        else
            echo "  later   game data extracted from $(game_label "$got"), but the folder holds $label - --assets reads it"
        fi
    elif [ -n "$(game_label "$got")" ]; then
        echo "  ok      game data extracted (assets/wolf3d.dat): $(game_label "$got")"
    else
        echo "  ok      game data extracted (assets/wolf3d.dat)"
    fi
    echo
    # the game in the data folder decides the name, unless --name says
    if [ -n "$name" ]; then
        rom_file=$(rom_name) || return 1
    elif [ "$ext" = SOD ]; then
        rom_file="Spear of Destiny.z64"
    else
        rom_file="Wolfenstein 3D.z64"
    fi
    echo "The ROM will be $rom_file."
    echo
    echo "See README.md for the details."
    echo
}

if [ $help = 1 ] || [ $((rom + assets + clean + sim)) = 0 ]; then
    show_help
    exit
fi

# ---------------------------------------------------------------------------

if [ $assets = 1 ] || [ -n "$data" ] || [ ! -f "$root/assets/wolf3d.dat" ]; then
    [ -n "$python" ] ||
        die "Python 3 is needed to extract the game's data - install it and try again"
    args=()
    [ -n "$data" ]    && args=("${args[@]}" --data "$data")
    [ -n "$src_dir" ] && args=("${args[@]}" --source "$src_dir")
    "$python" "$root/tools/extract_assets.py" "${args[@]}" ||
        die "asset extraction failed"
fi

command -v docker >/dev/null 2>&1 ||
    die "Docker is not installed - see README.md"
docker info >/dev/null 2>&1 ||
    die "Docker is not running, or you may not use it - start it and try again (on Linux your user may need to be in the docker group)"
if ! docker image inspect n64-libdragon >/dev/null 2>&1; then
    echo "building the n64-libdragon image (once; it compiles libdragon, so it takes a while)"
    run_docker build -f "$host_root/tools/Dockerfile.libdragon" -t n64-libdragon \
        "$host_root/tools" ||
        die "building the n64-libdragon image failed"
fi

if [ $sim = 1 ]; then
    exec bash "$root/tools/sim.sh"
fi

# The container writes build/ and the ROM into the project: run it as you,
# or on Linux they would be root's (on macOS Docker Desktop sees to that
# anyway) - except under rootless Docker, whose root already is you, and on
# Windows, where files have no Unix owner to get wrong. HOME points
# somewhere writable, as the image has no home for your user.
user=(--user "$(id -u):$(id -g)")
if [ $windows = 1 ] ||
        docker info --format '{{json .SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    user=()
fi
make_in_docker() {
    run_docker run --rm "${user[@]}" -e HOME=/tmp -v "$host_root:/src" -w /src \
        n64-libdragon make "$@" ||
        die "make $* failed"
}

# make builds the ROM inside build/ under the game's own name, and it is
# copied out as the game's full name or --name's (rom_name above). The
# Makefile reads the game from the blob too, for the ROM header's title.
rom_file=$(rom_name) || exit 1
internal=$(internal_rom)

[ $clean = 1 ] && make_in_docker clean
make_in_docker "ROM=$internal"
cp "$root/$internal" "$root/$rom_file" || die "could not copy the ROM to $rom_file"

printf '%s  %s bytes\n' "$rom_file" "$(wc -c < "$root/$rom_file" | tr -d ' ')"
