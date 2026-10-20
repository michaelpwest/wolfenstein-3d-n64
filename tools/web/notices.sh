#!/bin/bash
# The license texts of what the web builder's WebAssembly tools are made
# from, out of the n64-libdragon-web image, for build.sh --web to put in
# THIRD_PARTY_NOTICES.txt. Each goes to stdout behind a "=== name" line.
set -e
section() { echo "=== $1"; cat; echo; }

section "libdragon (audioconv64, mkdfs, n64tool, ed64romconfig)" < /libdragon/LICENSE.md
section "libsamplerate (compiled into audioconv64)" < /libdragon/tools/audioconv64/libsamplerate/COPYING
# the Opus codec's BSD license, as its sources carry it
sed -n '1,/\*\//p' /libdragon/src/audio/opus/celt.c | section "Opus (compiled into audioconv64)"
section "VADPCM encoder (compiled into audioconv64)" <<'EOF'
Copyright 2022 Dietrich Epp. Part of Skelly 64, licensed under the terms of
the Mozilla Public License, version 2.0 (https://mozilla.org/MPL/2.0/). The
source is in libdragon, tools/audioconv64/vadpcm:
https://github.com/DragonMinded/libdragon
EOF
section "dr_wav and dr_mp3 (compiled into audioconv64)" <<'EOF'
By David Reid; public domain (Unlicense), or MIT No Attribution.
EOF
section "Emscripten (the runtime compiled into each tool)" < /emsdk/upstream/emscripten/LICENSE
