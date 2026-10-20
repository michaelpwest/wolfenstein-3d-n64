#!/usr/bin/env bash
# Build Wolfenstein 3D for Nintendo 64 from your own copy of the game - on
# macOS, Linux, or Windows from Git Bash: the ROM ("Wolfenstein 3D.z64",
# "Spear of Destiny.z64", or --name whatever you like), or the web builder,
# a page that builds it in a browser.
#
#   ./build.sh                  # a menu that asks what to do, step by step
#   ./build.sh --rom            # build the ROM
#   ./build.sh --rom --name x   # ...as x.z64 instead of the usual name
#   ./build.sh --serve          # the web builder, at localhost:8064
#   ./build.sh --help           # the options, and what the build needs
#
# The game comes from your own copy - the shareware (*.WL1), the full game
# (*.WL6) or Spear of Destiny (*.SOD), with its WOLF3D.EXE or SPEAR.EXE - in
# gamedata/ here unless --data names another folder. See README.md.
#
# Building a ROM needs only Node.js. It is the web builder's own build
# (tools/web/app/build.mjs), run in Node by tools/web/build_node.mjs: the
# prebuilt program, which is the same for every game, and libdragon's tools
# compiled to WebAssembly. Those come with the web builder, in build/web -
# kept if it is this version's, or else fetched from this version's GitHub
# release (tools/web/web_tools.mjs fetches and serves it).
#
# Building from the source code - with changes to the port - takes Docker,
# which compiles it: the menu's third choice, which checks for Docker before
# anything else, or these:
#
#   ./build.sh --compile        # make: the ROM compiled from src/
#   ./build.sh --assets         # extract the assets again, then compile
#   ./build.sh --clean          # make clean, then compile
#   ./build.sh --sim            # host build + capture instead of the ROM
#   ./build.sh --bench          # make BENCH=1: the benchmark ROM
#   ./build.sh --music-wav      # make MUSIC=wav: the music rendered
#   ./build.sh --sfx-wav        # make SFX=wav: the AdLib effects rendered
#   ./build.sh --web            # make the web builder, and its zip for a release
#
# They run in the n64-libdragon image, which this builds from
# tools/Dockerfile.libdragon the first time it is missing, and --web in
# n64-libdragon-web too (tools/web/Dockerfile.emscripten). --source points
# at the WOLFSRC folder of id's source release, which checks the tables in
# tools/wolf_tables.py and can stand in for the executable's palette (see
# docs/TECHNICAL_DETAILS.md).
#
# Written for bash 3.2, the version macOS ships: no associative arrays,
# ${var,,}, mapfile or set -u (which trips over an empty array there).

# This script's folder. Double-clicked on Windows, it is handed its path as
# C:\...\build.sh, which dirname cannot take apart.
self=$0
case $self in
    *\\*) command -v cygpath >/dev/null 2>&1 && self=$(cygpath -u "$self") ;;
esac
root=$(cd "$(dirname "$self")" && pwd)

die() { printf '%s\n' "$*" >&2; exit 1; }

# Git Bash (or MSYS2, or Cygwin) on Windows: Docker is a Windows program
# there. It is given the project folder as a Windows path (C:/...), and
# MSYS is told not to rewrite the container's own paths - /src, /tmp - into
# Windows ones, as it would every argument that looks like a Unix path.
# Only for Docker: Node and Python, Windows programs too, need that
# rewriting to find the files.
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

rom=0 serve=0 help=0 open_browser=1
compile=0 assets=0 clean=0 sim=0 web=0 bench=0 music_wav=0 sfx_wav=0
data= src_dir= name= port=8064
if [ $# -eq 0 ]; then wizard=1; else wizard=0; fi
while [ $# -gt 0 ]; do
    case $1 in
        --rom)      rom=1 ;;
        --serve)    serve=1 ;;
        --no-open)  open_browser=0 ;;
        -h|--help)  help=1 ;;
        --compile)  compile=1 ;;
        --assets)   assets=1 ;;
        --clean)    clean=1 ;;
        --sim)      sim=1 ;;
        --web)      web=1 ;;
        --bench)     bench=1 ;;
        --music-wav) music_wav=1 ;;
        --sfx-wav)   sfx_wav=1 ;;
        --data|--source|--name|--port)
            [ $# -ge 2 ] || die "$1 needs a value (see ./build.sh --help)"
            case $1 in
                --data)   data=$2 ;;
                --source) src_dir=$2 ;;
                --name)   name=$2 ;;
                --port)   port=$2 ;;
            esac
            shift ;;
        --data=*)   data=${1#*=} ;;
        --source=*) src_dir=${1#*=} ;;
        --name=*)   name=${1#*=} ;;
        --port=*)   port=${1#*=} ;;
        *) die "unknown option: $1 (see ./build.sh --help)" ;;
    esac
    shift
done
# --source means extracting again, with the source's tables
[ -n "$src_dir" ] && assets=1

# Opened on its own - double-clicked, which runs it as the window's shell -
# the window closes the moment it ends: keep it open until Enter, so that
# what it did, or why it stopped, can be read. From a prompt it is a shell
# of its own, one level down, and the prompt comes back instead.
if [ "${SHLVL:-1}" -le 1 ] && [ -t 0 ] && [ -t 1 ]; then
    trap 'printf "\nPress Enter to close this window. "; read -r _' EXIT
fi

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

# Which games a folder holds, by their VSWAP files: WL1, WL6 and/or SOD
games_in() {
    local e exts=
    for e in WL1 WL6 SOD; do
        find_ci "$1" "VSWAP.$e" >/dev/null && exts="$exts $e"
    done
    printf '%s\n' "${exts# }"
}

# A message as a sentence: capital first, full stop last
sentence() {
    printf '%s%s.' "$(printf '%s' "${1:0:1}" | tr '[:lower:]' '[:upper:]')" "${1:1}"
}

# A, B and C
names_list() {
    case $# in
        0) ;;
        1) printf '%s' "$1" ;;
        *) local last=${!#} all=("$@"); unset "all[$(($# - 1))]"
           printf '%s and %s' "$(printf '%s, ' "${all[@]}" | sed 's/, $//')" "$last" ;;
    esac
}

# check_folder <folder>: true if it holds one game, whole - its eight data
# files and its executable - setting games to its kind; else false, with
# problem saying what is wrong and what to do, in the words of someone who
# has just picked the wrong thing
check_folder() {
    local d=$1 sub f exe labels missing=()
    problem= games=
    if [ -f "$d" ]; then
        case $d in
            *.[zZ][iI][pP]) problem="$d is a zip file: unzip it first, then use the folder it makes" ;;
            *)              problem="$d is a file: use the folder the game's files are in" ;;
        esac
        return 1
    fi
    if [ ! -d "$d" ]; then
        problem="there is no folder $d"
        return 1
    fi
    games=$(games_in "$d")
    case $games in
        "")
            # picked the folder around the game's, perhaps
            for sub in "$d"/*/; do
                sub=${sub%/}
                if [ -d "$sub" ] && [ -n "$(games_in "$sub")" ]; then
                    problem="the game is not in $d itself, but in its folder $(basename "$sub"): use $sub"
                    return 1
                fi
            done
            problem="no game in $d: it needs the game's files - VSWAP.WL1 for the shareware, VSWAP.WL6 for the full game or VSWAP.SOD for Spear of Destiny, and the files that go with it"
            return 1 ;;
        *" "*)
            labels=()
            for f in $games; do labels=("${labels[@]}" "$(game_label "$f")"); done
            problem="$d holds more than one game - $(names_list "${labels[@]}"): give each its own folder"
            return 1 ;;
    esac
    for f in VSWAP GAMEMAPS MAPHEAD VGAGRAPH VGADICT VGAHEAD AUDIOT AUDIOHED; do
        find_ci "$d" "$f.$games" >/dev/null || missing=("${missing[@]}" "$f.$games")
    done
    # Spear of Destiny's is SPEAR.EXE, or WOLF3D.EXE in some releases
    if [ "$games" = SOD ]; then
        find_ci "$d" SPEAR.EXE >/dev/null || find_ci "$d" WOLF3D.EXE >/dev/null || missing=("${missing[@]}" SPEAR.EXE)
    else
        find_ci "$d" WOLF3D.EXE >/dev/null || missing=("${missing[@]}" WOLF3D.EXE)
    fi
    if [ ${#missing[@]} -gt 0 ]; then
        problem="$(game_label "$games") in $d is missing $(names_list "${missing[@]}"): the folder needs all of the game's files"
        return 1
    fi
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
# src/save.c). Without --name, a compiled ROM is named by the game
# extracted; build_prebuilt names it by the game in the folder first.
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
        echo "Spear of Destiny$(rom_suffix).z64"
    else
        echo "Wolfenstein 3D$(rom_suffix).z64"
    fi
}

# What sets a benchmark or other-audio ROM's name apart from the game's, so
# that building one never replaces the ROM you play - nor shares its saves
rom_suffix() {
    if [ $bench = 1 ]; then
        echo " (benchmark)"
    elif [ $music_wav = 1 ] && [ $sfx_wav = 1 ]; then
        echo " (rendered audio)"
    elif [ $music_wav = 1 ]; then
        echo " (rendered music)"
    elif [ $sfx_wav = 1 ]; then
        echo " (rendered effects)"
    fi
}

# make's settings for those: the Makefile's BENCH, MUSIC and SFX
make_flags() {
    [ $bench = 1 ]     && printf '%s\n' BENCH=1
    [ $music_wav = 1 ] && printf '%s\n' MUSIC=wav
    [ $sfx_wav = 1 ]   && printf '%s\n' SFX=wav
    return 0
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

# Node.js 18 or later: what Pyodide needs, and fetch()
node_ok() {
    command -v node >/dev/null 2>&1 &&
        node -e 'process.exit(parseInt(process.versions.node) < 18 ? 1 : 0)' 2>/dev/null
}

need_node() {
    node_ok || die "Building needs Node.js, version 18 or later - install it from https://nodejs.org/ and try again"
}

# The version of the web builder in build/web, from its page: nothing if
# there is none
web_version() {
    sed -n 's/.*ROM Builder v\([^<]*\)<.*/\1/p' "$root/build/web/index.html" 2>/dev/null |
        head -n 1
}

# True when build/web is this version's web builder, whole
have_web_builder() {
    [ -f "$root/build/web/build.mjs" ] && [ -f "$root/build/web/program/wolf3d.elf.stripped" ] &&
        [ "$(web_version)" = "$(port_version)" ]
}

# Where this version's web builder is published: the zip --web makes,
# attached to the GitHub release. WOLF3D_WEB_URL stands in for it, to test.
release_url() {
    printf '%s\n' "${WOLF3D_WEB_URL:-https://github.com/michaelpwest/wolfenstein-3d-n64/releases/download/v$(port_version)/wolfenstein-3d-n64-web.zip}"
}

# ---------------------------------------------------------------------------
# What the build needs, and whether it is there

mark() { if [ "$1" = 1 ]; then printf '  ok     '; else printf '  MISSING'; fi; }

# show_requirements [tools]: everything, or with "tools" not the game files
# and the ROM's name - the menu asks for those next
show_requirements() {
    local data_dir exts ext games label f found=0 exe_names exe=0 rom_file only_tools=0
    [ "$1" = tools ] && only_tools=1
    data_dir=${data:-$root/gamedata}

    echo "What the build needs:"
    if [ $only_tools = 0 ]; then
        # whichever of the three games is there: the one with a VSWAP
        exts=$(games_in "$data_dir")
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
    fi
    if node_ok; then
        echo "  ok      Node.js"
    elif command -v node >/dev/null 2>&1; then
        echo "  MISSING Node.js 18 or later: this one is $(node --version 2>/dev/null) - https://nodejs.org/"
    else
        echo "  MISSING Node.js: https://nodejs.org/"
    fi
    echo
    [ $only_tools = 1 ] && return 0
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
}

# What building from the source code needs: true when it is all there. The
# menu's third choice asks this first, and again until it is.
show_source_requirements() {
    local ready=1 image=0
    echo "Building from the source code needs:"
    if [ -n "$python" ]; then
        echo "  ok      Python 3"
    else
        echo "  MISSING Python 3: https://www.python.org/"
        ready=0
    fi
    if ! command -v docker >/dev/null 2>&1; then
        echo "  MISSING Docker: install Docker Desktop, https://www.docker.com/products/docker-desktop/"
        [ "$(uname -s)" = Linux ] && echo "          (or Docker Engine)"
        ready=0
    elif ! docker info >/dev/null 2>&1; then
        echo "  MISSING Docker, running: start Docker Desktop, and wait until it says it is running"
        [ "$(uname -s)" = Linux ] &&
            echo "          (on Linux, your user may also need to be in the docker group)"
        ready=0
    else
        echo "  ok      Docker, running"
        docker image inspect n64-libdragon >/dev/null 2>&1 && image=1
        if [ $image = 1 ]; then
            echo "  ok      the compiler, set up in Docker"
        else
            echo "  later   the compiler: the first build sets it up in Docker (several minutes)"
        fi
    fi
    echo
    [ $ready = 1 ]
}

show_help() {
    printf '\nWolfenstein 3D for Nintendo 64 - ROM Builder v%s\n' "$(port_version)"
    cat <<'EOF'

  ./build.sh             a menu that asks what to do, step by step
  ./build.sh --rom       build the ROM
  ./build.sh --serve     serve the web builder - a page that builds the ROM
                         in your browser - at http://localhost:8064, and
                         open it there; Ctrl+C stops it
  ./build.sh --help      this help screen

  --data <folder>        where your game files are - the shareware, the
                         full game or Spear of Destiny (default: gamedata/
                         here)
  --name <name>          what to call the ROM, instead of Wolfenstein 3D.z64
                         or Spear of Destiny.z64 - in quotes if it has spaces.
                         Each game is named apart because a cartridge's
                         saves and high scores follow the ROM's name
  --port <number>        the port --serve uses, instead of 8064
  --no-open              --serve without opening the browser

Examples:

  ./build.sh --rom
      build what is in gamedata/ - Wolfenstein 3D.z64, or Spear of
      Destiny.z64 if those are Spear of Destiny's files

  ./build.sh --rom --data ~/games/SPEAR
      build from another folder instead

  ./build.sh --rom --data ~/games/WOLF6 --name "Wolfenstein 3D (full)"
      call it Wolfenstein 3D (full).z64, so that the full game has a
      cartridge of its own - its saves and its high scores - beside the
      shareware's

Building from the source code - with your own changes to the port. These
need Docker; the options above never do.

  ./build.sh --compile   compile the ROM from src/; --data and --name work
                         as with --rom
  ./build.sh --assets    read the game files in gamedata/ again, then
                         compile
  ./build.sh --clean     throw away everything compiled and compile it all
                         again; the game data already read is kept, so add
                         --assets to redo that too
  ./build.sh --sim       build the game for this computer instead and run
                         its checks; frames go to build/sim, see sheet.png
  ./build.sh --bench     compile the benchmark ROM instead of the game: it
                         times the renderer on a few floors and shows a
                         table of the results (... (benchmark).z64)
  ./build.sh --music-wav compile the ROM with the music rendered exactly,
                         rather than sequenced - about 6 MB more
  ./build.sh --sfx-wav   compile it with the AdLib sound effects rendered,
                         rather than synthesized on the N64 - about 5 MB more;
                         with either, the ROM's name says so unless --name
                         is given
  ./build.sh --web       make the web builder from src/: build/web, and
                         build/wolfenstein-3d-n64-web.zip for itch.io and for the
                         GitHub release, which --rom fetches it from (see
                         README.md); add --serve to try it

After --web, --rom builds from your own program, changes and all, rather
than the release's: it uses build/web when that is this version's.

EOF
    show_requirements
    show_source_requirements
    echo "See README.md for the details."
    echo
}

# ---------------------------------------------------------------------------
# The menu: ./build.sh with nothing after it

# Color for the title, where the terminal shows it - NO_COLOR turns it off,
# and CLICOLOR_FORCE on where the output is not a terminal
if { [ -t 1 ] || [ -n "${CLICOLOR_FORCE:-}" ]; } && [ -z "${NO_COLOR:-}" ]; then
    color=1 dim=$(printf '\033[2m') off=$(printf '\033[0m')
else
    color=0 dim= off=
fi

# The title's letters, a # for each block
banner_rows=(
    "#   #  ##  #    #### #### #   #  ### ##### #### ### #   #    ###  ###"
    "#   # #  # #    #    #    ##  # #      #   #     #  ##  #       # #  #"
    "# # # #  # #    ###  ###  # # #  ##    #   ###   #  # # #     ##  #  #"
    "## ## #  # #    #    #    #  ##    #   #   #     #  #  ##       # #  #"
    "#   #  ##  #### #    #### #   # ###    #   #### ### #   #    ###  ###"
)

# shade <half-row> <column>: sets shade to the 256-color number of that
# half-row of the 3D title, or to nothing for none. Each letter is lit from
# above - its face red, its top edge a shade lighter - with two half-rows
# of darkening red below it, its depth.
shade() {
    local r=$(($1 / 2))
    if [ $r -lt ${#banner_rows[@]} ] && [ "${banner_rows[$r]:$2:1}" = "#" ]; then
        if [ $(($1 % 2)) = 0 ] && { [ $r = 0 ] || [ "${banner_rows[$((r - 1))]:$2:1}" != "#" ]; }; then
            shade=203
        else
            shade=160
        fi
    elif [ $1 -ge 1 ] && r=$((($1 - 1) / 2)) && [ $r -lt ${#banner_rows[@]} ] &&
            [ "${banner_rows[$r]:$2:1}" = "#" ]; then
        shade=88
    elif [ $1 -ge 2 ] && r=$((($1 - 2) / 2)) && [ $r -lt ${#banner_rows[@]} ] &&
            [ "${banner_rows[$r]:$2:1}" = "#" ]; then
        shade=52
    else
        shade=
    fi
}

# The title in 3D: a terminal cell holds two half-rows, drawn as "▀" in the
# top one's color on the bottom one's, and the depth adds a row
banner_3d() {
    local row col top bot ch code last line width=0 r
    for r in "${banner_rows[@]}"; do [ ${#r} -gt $width ] && width=${#r}; done
    for ((row = 0; row <= ${#banner_rows[@]}; row++)); do
        line="  " last=
        for ((col = 0; col < width; col++)); do
            shade $((row * 2)) $col;     top=$shade
            shade $((row * 2 + 1)) $col; bot=$shade
            if [ -z "$top$bot" ]; then
                code="0" ch=" "
            elif [ "$top" = "$bot" ]; then
                code="0;38;5;$top" ch="█"
            elif [ -z "$bot" ]; then
                code="0;38;5;$top" ch="▀"
            elif [ -z "$top" ]; then
                code="0;38;5;$bot" ch="▄"
            else
                code="0;38;5;$top;48;5;$bot" ch="▀"
            fi
            [ "$code" = "$last" ] || line="$line"$'\033['"${code}m"
            line=$line$ch last=$code
        done
        printf '%s\033[0m\n' "${line%"${line##*[! ]}"}"
    done
}

# The same 3D title as an SVG, for the web builder's page: a square for each
# half-row of each column, those of one color in a row joined into one
# rectangle. The colors are the 256-color palette's that banner_3d uses.
title_svg() {
    local width=0 height r hr col run run_start fill
    for r in "${banner_rows[@]}"; do [ ${#r} -gt $width ] && width=${#r}; done
    height=$(((${#banner_rows[@]} + 1) * 2))
    printf '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" shape-rendering="crispEdges">\n' \
        $width $height
    printf '<title>Wolfenstein 3D</title>\n'
    for ((hr = 0; hr < height; hr++)); do
        run= run_start=0
        for ((col = 0; col <= width; col++)); do
            if [ $col -lt $width ]; then shade $hr $col; else shade=; fi
            [ "$shade" = "$run" ] && continue
            if [ -n "$run" ]; then
                case $run in
                    203) fill='#ff5f5f' ;;
                    160) fill='#d70000' ;;
                    88)  fill='#870000' ;;
                    52)  fill='#5f0000' ;;
                esac
                printf '<rect x="%d" y="%d" width="%d" height="1" fill="%s"/>\n' \
                    $run_start $hr $((col - run_start)) "$fill"
            fi
            run=$shade run_start=$col
        done
    done
    printf '</svg>\n'
}

banner() {
    printf '\n'
    if [ $color = 1 ]; then
        banner_3d
    else
        printf '  %s\n' "${banner_rows[@]}" | sed 's/#/█/g'
    fi
    # centered under the title's 70 columns
    printf '                     f o r   N i n t e n d o   6 4\n'
    printf '%s                            ROM Builder v%s%s\n\n' \
        "$dim" "$(port_version)" "$off"
}

# ask <variable> <question> [default]: a line of input, or the default for
# an empty one. The end of the input quits.
ask() {
    local reply
    if [ -n "$3" ]; then
        printf '%s [%s]: ' "$2" "$3"
    else
        printf '%s: ' "$2"
    fi
    if ! IFS= read -r reply; then
        echo
        exit 0
    fi
    [ -n "$reply" ] || reply=$3
    printf -v "$1" '%s' "$reply"
}

# yes_no <question> <default y or n>: true for yes
yes_no() {
    local answer
    while :; do
        if [ "$2" = y ]; then ask answer "$1 (Y/n)"; else ask answer "$1 (y/N)"; fi
        case $answer in
            "")        [ "$2" = y ]; return ;;
            y|Y|yes)   return 0 ;;
            n|N|no)    return 1 ;;
        esac
    done
}

# A folder as typed or pasted: what a paste brings along unseen taken off,
# quotes and a leading ~ undone, and on Windows a C:\... path made one this
# shell can use
clean_folder() {
    local d=$1
    # Spaces either side, and the marks some Windows copies hide in front of
    # a path - left-to-right and right-to-left marks, the left-to-right
    # embedding a folder's Properties > Security tab adds, a byte-order mark
    # - and no-break spaces: before a drive letter, any of them turns
    # C:\Games into a relative path, which cygpath makes " C:/Games".
    d=$(printf '%s' "$d" | LC_ALL=C sed \
        -e "s/$(printf '\342\200\252')//g" -e "s/$(printf '\342\200\216')//g" \
        -e "s/$(printf '\342\200\217')//g" -e "s/$(printf '\357\273\277')//g" \
        -e "s/$(printf '\302\240')/ /g" \
        -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    d=${d#\"}; d=${d%\"}; d=${d#\'}; d=${d%\'}
    case $d in
        "~")   d=$HOME ;;
        "~/"*) d=$HOME/${d#\~/} ;;
    esac
    if [ $windows = 1 ] && [ -n "$d" ]; then
        d=$(cygpath -u "$d" 2>/dev/null || printf '%s' "$d")
    fi
    printf '%s\n' "${d%/}"
}

# Each step of the menu below returns false for "back", to the menu before.

# ask_folder: where the game files are - sets data, and games to its kind
ask_folder() {
    local dir answer
    while :; do
        echo
        ask answer "Where are your game files?" "$root/gamedata"
        dir=$(clean_folder "$answer")
        check_folder "$dir" && break
        echo "  $(sentence "$problem")"
    done
    echo "  Found $(game_label "$games")."
    data=$dir
}

# ask_game: the folder, and what to call the ROM - sets name and rom_file too
ask_game() {
    local rom_default
    ask_folder
    if [ "$games" = SOD ]; then rom_default="Spear of Destiny"; else rom_default="Wolfenstein 3D"; fi
    rom_default="$rom_default$(rom_suffix).z64"
    echo
    echo "  Saves and high scores go with the ROM's name: give two copies of a game"
    echo "  different names to keep theirs apart."
    ask name "What should the ROM be called?" "$rom_default"
    rom_file=$(rom_name) || exit 1
    echo
}

# ask_port: and --serve with it
ask_port() {
    echo
    ask port "Which port should the web builder use?" "$port"
    serve=1
}

# 1: the ROM, from the ready-made program
wizard_rom() {
    if ! node_ok; then
        echo
        echo "  This needs Node.js, version 18 or later: install it from https://nodejs.org/,"
        echo "  then run this again."
        echo
        return 1
    fi
    ask_game
    yes_no "Build $rom_file from $(game_label "$games")?" y || { echo; return 1; }
    rom=1
}

# 2: the web builder, served here
wizard_web() {
    if ! node_ok; then
        echo
        echo "  This needs Node.js, version 18 or later: install it from https://nodejs.org/,"
        echo "  then run this again."
        echo
        return 1
    fi
    echo
    echo "  The web builder is a page that builds the ROM in your browser: choose"
    echo "  your game folder there, and download the ROM. Your files stay on"
    echo "  this computer."
    ask_port
}

# A ROM from the source code - the game, or with bench, music_wav or sfx_wav
# set, one of its variants: the game folder, the name, and a clean build if
# wanted
source_rom() {
    ask_game
    yes_no "Throw away everything compiled before, and compile it all again (only needed if a build went wrong)?" n &&
        clean=1
    yes_no "Compile $rom_file from $(game_label "$games")?" y || return 1
    compile=1
}

# 3: from the source code - Docker first, then what to build
wizard_source() {
    local choice
    echo
    echo "  This compiles the port from its source code, in src/ - the way to build"
    echo "  it with changes of your own. It takes Docker, which holds the compiler."
    echo
    until show_source_requirements; do
        echo "  r  Check again"
        echo "  b  Back"
        echo "  q  Quit"
        echo
        while :; do
            ask choice "Choose" r
            case $choice in
                r|R) break ;;
                b|B) echo; return 1 ;;
                q|Q) exit 0 ;;
                *)   echo "  Choose r, b or q." ;;
            esac
        done
        echo
    done
    while :; do
        bench=0 music_wav=0 sfx_wav=0 clean=0 serve=0
        echo "What would you like to build from the source code?"
        echo "  1  The ROM"
        echo "  2  The ROM with rendered audio - larger, to compare with the usual"
        echo "  3  The benchmark ROM - times the renderer, instead of the game"
        echo "  4  The web builder, and its zip - for a release, or itch.io"
        echo "  5  The automated checks"
        echo "  b  Back"
        echo
        ask choice "Choose" 1
        case $choice in
            1)
                source_rom && return 0
                echo ;;
            2)
                echo
                echo "  Usually the music is sequenced, and the AdLib sound effects are"
                echo "  synthesized on the N64, as the original sound card did. Either can be"
                echo "  rendered to recordings instead: exact, but larger."
                echo
                yes_no "Render the music (about 6 MB more)?" y && music_wav=1
                yes_no "Render the sound effects (about 5 MB more)?" n && sfx_wav=1
                if [ $music_wav = 0 ] && [ $sfx_wav = 0 ]; then
                    echo "  Neither, then: that is the usual ROM, choice 1."
                    echo
                    continue
                fi
                source_rom && return 0
                echo ;;
            3)
                echo
                echo "  The benchmark shows two test cards, turns on the spot on a few floors"
                echo "  and faces a guard and a wall, timing every frame, then ends on a table"
                echo "  of the results. It is a ROM of its own, with its own name."
                bench=1
                source_rom && return 0
                echo ;;
            4)
                if ! command -v npm >/dev/null 2>&1; then
                    echo
                    echo "  This also needs npm, which comes with Node.js: https://nodejs.org/"
                    echo
                    continue
                fi
                echo
                yes_no "Start the web builder once it is made, to try it?" y && ask_port
                echo
                yes_no "Make the web builder from the source code?" y || { echo; continue; }
                web=1
                return 0 ;;
            5)
                # they run on the game data read last; there must be some
                if [ ! -f "$root/assets/wolf3d.dat" ]; then
                    echo
                    echo "  The checks play the game, so they need its files first."
                    ask_folder
                fi
                echo
                yes_no "Run the automated checks?" y || { echo; continue; }
                sim=1
                return 0 ;;
            b|B) echo; return 1 ;;
            *)   echo "  Choose 1 to 5, or b."; echo ;;
        esac
    done
}

wizard() {
    local choice
    banner
    show_requirements tools
    while :; do
        echo "What would you like to do?"
        echo "  1  Build the ROM                        quickest; needs only Node.js"
        echo "  2  Build the ROM in your web browser    opens a page that builds it"
        echo "  3  Build from the source code           with your own changes; needs Docker"
        echo "  q  Quit"
        echo
        ask choice "Choose" 1
        case $choice in
            1)   wizard_rom && return ;;
            2)   wizard_web && return ;;
            3)   wizard_source && return ;;
            q|Q) exit 0 ;;
            *)   echo "  Choose 1, 2, 3 or q."; echo ;;
        esac
    done
}

if [ $help = 1 ]; then
    show_help
    exit
fi
if [ $wizard = 1 ]; then
    wizard
    echo
fi
variant=$((bench + music_wav + sfx_wav))
dev=$((compile + assets + clean + sim + web + variant))
# the web builder and the checks are the game's, always
if [ $variant -gt 0 ] && [ $((web + sim + serve)) -gt 0 ]; then
    die "--bench, --music-wav and --sfx-wav are for compiling a ROM, not for --web, --serve or --sim"
fi
if [ $((rom + serve + dev)) = 0 ]; then
    show_help
    exit
fi
[ -n "$data" ] && data=$(clean_folder "$data")

# ---------------------------------------------------------------------------
# The prebuilt web builder, and a ROM built from it

# build/web, as this version's web builder: kept if it already is - made
# here by --web, or fetched before - or else fetched from the release
get_web_builder() {
    local url
    have_web_builder && return 0
    need_node
    url=$(release_url)
    echo "downloading what the build needs, about 8 MB - only the first time"
    mkdir -p "$root/build" || die "cannot make $root/build"
    node "$root/tools/web/web_tools.mjs" fetch "$url" "$root/build/web" ||
        die "the download failed ($url) - check the internet connection and try again. A 404 means v$(port_version) has no release on GitHub yet: use a released version of the port."
    have_web_builder || die "what was fetched is not the builder for v$(port_version): $url"
}

# The ROM, by the web builder's build in Node: it reads the game folder
# itself each time, so nothing is extracted into assets/
build_prebuilt() {
    local dir games
    need_node
    dir=${data:-$root/gamedata}
    check_folder "$dir" || die "$(sentence "$problem")"
    if [ -z "$name" ]; then
        if [ "$games" = SOD ]; then name="Spear of Destiny"; else name="Wolfenstein 3D"; fi
    fi
    rom_file=$(rom_name) || exit 1
    get_web_builder
    echo "building $rom_file from $(game_label "$games")"
    # in a terminal, each step's progress on a line that keeps updating
    live=()
    [ -t 1 ] && live=(--progress)
    node "$root/tools/web/build_node.mjs" "${live[@]}" "$dir" "$root/$rom_file" "$root/build/web" ||
        die "The ROM was not built - see above."
    printf '%s  %s bytes\n' "$rom_file" "$(wc -c < "$root/$rom_file" | tr -d ' ')"
}

# The page, served to this computer only, until Ctrl+C. The server runs in
# the foreground and opens the browser itself once it is listening.
serve_web() {
    local open=
    case $port in
        '' | *[!0-9]*) die "--port must be a number, not: $port" ;;
    esac
    [ $open_browser = 1 ] && open=open
    # Ctrl+C ends both it and this script; any other end, it explains
    node "$root/tools/web/web_tools.mjs" serve "$root/build/web" "$port" $open ||
        die "the web builder stopped - --port picks another port, if that was the trouble"
}

# ---------------------------------------------------------------------------
# Working on the port: Docker, and the n64-libdragon image

# need_docker <what needs it>
need_docker() {
    command -v docker >/dev/null 2>&1 ||
        die "Docker is not installed, and $1 needs it: install Docker Desktop, https://www.docker.com/products/docker-desktop/"
    docker info >/dev/null 2>&1 ||
        die "Docker is not running, or you may not use it, and $1 needs it - start it and try again (on Linux your user may need to be in the docker group)"
}

# image <name> <Dockerfile> <context>: built, the first time it is missing
need_image() {
    if ! docker image inspect "$1" >/dev/null 2>&1; then
        echo "building the $1 image - this takes several minutes"
        run_docker build -f "$host_root/$2" -t "$1" "$host_root/$3" ||
            die "building the $1 image failed"
    fi
}

# The container writes build/ and the ROM into the project: run it as you,
# or on Linux they would be root's (on macOS Docker Desktop sees to that
# anyway) - except under rootless Docker, whose root already is you, and on
# Windows, where files have no Unix owner to get wrong. HOME points
# somewhere writable, as the image has no home for your user.
docker_user() {
    user=(--user "$(id -u):$(id -g)")
    if [ $windows = 1 ] ||
            docker info --format '{{json .SecurityOptions}}' 2>/dev/null | grep -q rootless; then
        user=()
    fi
}

make_in_docker() {
    run_docker run --rm "${user[@]}" -e HOME=/tmp -v "$host_root:/src" -w /src \
        n64-libdragon make "$@" ||
        die "make $* failed"
}

# ---------------------------------------------------------------------------
# Making the web builder (tools/web): build/web, and build/wolfenstein-3d-n64-web.zip
# for an HTML5 project on itch.io and for the GitHub release. Its build is
# tools/web/app/build.mjs, which --rom runs in Node; tools/web/build_node.mjs
# is also how its ROMs were checked against --compile's.

pyodide_version=314.0.7

make_web() {
    local out=$root/build/web deps=$root/build/web-deps f owner=()
    [ -n "$python" ] || die "Python 3 is needed to make the web builder - install it and try again"
    command -v npm >/dev/null 2>&1 ||
        die "npm (Node.js) is needed to make the web builder, to fetch Pyodide - install Node.js and try again"
    need_docker "--web"
    need_image n64-libdragon tools/Dockerfile.libdragon tools
    need_image n64-libdragon-web tools/web/Dockerfile.emscripten tools/web
    docker_user

    echo "the program"
    make_in_docker program ROM=build/wolf3d.z64

    rm -rf "$out"
    mkdir -p "$out/tools" "$out/pyodide" "$out/py" "$out/program" || die "cannot make $out"

    # the WebAssembly is compiled as the image's root, then given back to you
    echo "the tools, in WebAssembly"
    [ ${#user[@]} -gt 0 ] && owner=(-e "HOST_OWNER=$(id -u):$(id -g)")
    run_docker run --rm "${owner[@]}" -v "$host_root:/src" n64-libdragon-web \
        bash /src/tools/web/wasm_tools.sh /src/build/web/tools ||
        die "building the WebAssembly tools failed"

    # Pyodide's core: Python itself and its standard library - the extractor
    # needs nothing more
    if ! grep -q "\"version\": \"$pyodide_version\"" "$deps/package/package.json" 2>/dev/null; then
        echo "fetching Pyodide $pyodide_version"
        rm -rf "$deps"
        mkdir -p "$deps" || die "cannot make $deps"
        (cd "$deps" && npm pack "pyodide@$pyodide_version" --silent >/dev/null &&
            tar -xzf "pyodide-$pyodide_version.tgz") ||
            die "fetching Pyodide failed"
    fi
    for f in pyodide.mjs pyodide.asm.mjs pyodide.asm.wasm python_stdlib.zip pyodide-lock.json; do
        cp "$deps/package/$f" "$out/pyodide/" || die "Pyodide's $f is missing"
    done

    echo "the page"
    cp "$root/tools/extract_assets.py" "$root/tools/lzexe.py" "$root/tools/wolf_tables.py" \
        "$root/src/sprites.h" "$out/py/"
    cp "$root/build/build/wolf3d.elf.stripped" "$root/build/build/wolf3d.elf.sym" "$out/program/"
    cp "$root/tools/web/app/"* "$out/"
    sed "s/{{VERSION}}/$(port_version)/" "$root/tools/web/app/index.html" > "$out/index.html"
    title_svg > "$out/title.svg"
    web_notices > "$out/THIRD_PARTY_NOTICES.txt" || die "collecting the notices failed"

    # for itch.io: index.html at the top of the zip
    rm -f "$root/build/wolfenstein-3d-n64-web.zip"
    (cd "$out" && "$python" -m zipfile -c ../wolfenstein-3d-n64-web.zip *) ||
        die "making the zip failed"
    printf 'build/web  %s MB, and build/wolfenstein-3d-n64-web.zip for itch.io and the release\n' \
        "$(du -sk "$out" | awk '{ printf "%.1f", $1 / 1024 }')"
}

# THIRD_PARTY_NOTICES.txt: the license of everything the page is made of
web_notices() {
    local rule line
    rule=$(printf '%078d' 0 | tr 0 =)
    heading() { printf '%s\n%s\n%s\n' "$rule" "$1" "$rule"; }
    heading "Wolfenstein 3D for Nintendo 64 - web ROM builder v$(port_version)"
    cat <<'EOF'
The port, and this builder, are free software under the GNU General Public
License, version 2, below. The source code is at
https://github.com/michaelpwest/wolfenstein-3d-n64

The ROM is built in your browser from your own copy of the game: your files
are not uploaded anywhere, and none of id Software's data is in this page.

EOF
    cat "$root/COPYING"
    heading "Nuked-OPL3 (compiled into imf2xm)"
    printf '%s\n%s\n\n' \
        "Copyright (C) 2013-2020 Nuke.YKT. GNU Lesser General Public License, version" \
        "2.1 or later; the source is in the repository above, tools/audio/nuked-opl3."
    cat "$root/tools/audio/nuked-opl3/LICENSE"
    run_docker run --rm -v "$host_root:/src" n64-libdragon-web bash /src/tools/web/notices.sh |
        while IFS= read -r line; do
            case $line in
                "=== "*) heading "${line#=== }" ;;
                *)       printf '%s\n' "$line" ;;
            esac
        done
    heading "Pyodide $pyodide_version (Python in the browser, which runs the extractor)"
    cat <<'EOF'
Pyodide is distributed under the Mozilla Public License, version 2.0
(https://mozilla.org/MPL/2.0/); its source is at
https://github.com/pyodide/pyodide. It includes CPython, under the Python
Software Foundation License: https://docs.python.org/3/license.html
EOF
}

# A ROM compiled from src/: the asset extractor, then make in the container
compile_rom() {
    local internal
    if [ $assets = 1 ] || [ -n "$data" ] || [ ! -f "$root/assets/wolf3d.dat" ]; then
        [ -n "$python" ] ||
            die "Python 3 is needed to extract the game's data - install it and try again"
        check_folder "${data:-$root/gamedata}" || die "$(sentence "$problem")"
        args=()
        [ -n "$data" ]    && args=("${args[@]}" --data "$data")
        [ -n "$src_dir" ] && args=("${args[@]}" --source "$src_dir")
        "$python" "$root/tools/extract_assets.py" "${args[@]}" ||
            die "the game's files could not be read - see above"
    fi

    if [ $sim = 1 ]; then need_docker "--sim"; else need_docker "compiling"; fi
    need_image n64-libdragon tools/Dockerfile.libdragon tools

    if [ $sim = 1 ]; then
        exec bash "$root/tools/sim.sh"
    fi

    docker_user

    # make builds the ROM inside build/ under the game's own name, and it is
    # copied out as the game's full name or --name's (rom_name above). The
    # Makefile reads the game from the blob too, for the ROM header's title.
    rom_file=$(rom_name) || exit 1
    internal=$(internal_rom)

    [ $clean = 1 ] && make_in_docker clean
    make_in_docker "ROM=$internal" $(make_flags)
    cp "$root/$internal" "$root/$rom_file" || die "could not copy the ROM to $rom_file"

    printf '%s  %s bytes\n' "$rom_file" "$(wc -c < "$root/$rom_file" | tr -d ' ')"
}

# ---------------------------------------------------------------------------

if [ $web = 1 ] || [ $serve = 1 ]; then
    [ $web = 1 ] && make_web
    if [ $serve = 1 ]; then
        need_node
        get_web_builder
        serve_web
    fi
elif [ $dev -gt 0 ]; then
    compile_rom
else
    build_prebuilt
fi
