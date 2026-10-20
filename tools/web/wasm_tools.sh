#!/bin/bash
# The programs the web builder runs, compiled to WebAssembly inside the
# n64-libdragon-web image (Dockerfile.emscripten): libdragon's audioconv64,
# mkdfs, n64tool and ed64romconfig, as its tools Makefile builds them, and
# this project's imf2xm. Each is an ES module exporting a factory; the page
# makes a fresh instance for every run, puts the input files in its memory
# file system, and calls its main (web/build.mjs). /src is the project; the
# modules go in $1.
set -e
OUT=${1:-/src/build/web/tools}
mkdir -p "$OUT"
rm -f "$OUT"/*.mjs "$OUT"/*.wasm
LD=/libdragon/tools

# audioconv64 works out how much memory each song's context needs on the
# N64 from its own structures' sizes. Those are a 64-bit machine's in
# libdragon's builds, at least the N64's; WebAssembly is 32-bit, and its
# figure comes out 8 bytes a song short of the N64's (the N64's context
# holds a file handle for streaming that the tools' does not). libxm adds
# 256 bytes for the same reason when the tools are built for Windows, and
# this adds them for WebAssembly too - in this container's copy only. If
# libdragon's line changes, this stops rather than build without it.
grep -q $'^\t#if _WIN32$' /libdragon/src/audio/libxm/load.c ||
    { echo "libxm's load.c no longer has its '#if _WIN32' margin"; exit 1; }
sed -i $'s/^\t#if _WIN32$/\t#if _WIN32 || __EMSCRIPTEN__/' /libdragon/src/audio/libxm/load.c

# A fresh instance a run, its main called by hand; a big stack, as the tools
# were written for a desktop's; memory that grows with the songs
FLAGS="-O2 -std=gnu11 -Wno-unused-result -sMODULARIZE=1 -sEXPORT_ES6=1
       -sENVIRONMENT=web,worker,node -sINVOKE_RUN=0 -sEXIT_RUNTIME=1
       -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608 -sFORCE_FILESYSTEM=1
       -sEXPORTED_RUNTIME_METHODS=FS,callMain"

build() {   # name, sources...
    local name=$1; shift
    echo "    [WASM] $name"
    emcc $FLAGS -I/libdragon/include -o "$OUT/$name.mjs" "$@" -lm
}

build audioconv64   $LD/audioconv64/audioconv64.c
build mkdfs         $LD/mkdfs/mkdfs.c
build n64tool       $LD/n64tool.c
build ed64romconfig $LD/ed64romconfig.c
build imf2xm        /src/tools/audio/imf2xm.c /src/tools/audio/nuked-opl3/opl3.c

# compiled as the image's root; on Linux, given back to whoever ran build.sh
if [ -n "$HOST_OWNER" ]; then
    chown -R "$HOST_OWNER" "$OUT"
fi
