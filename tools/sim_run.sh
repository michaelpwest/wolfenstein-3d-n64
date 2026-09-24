#!/usr/bin/env bash
# Inside the n64-libdragon container, from the project folder: build the
# game for the host with the container's own gcc, and run the capture
# script. tools/sim.ps1 (Windows) and tools/sim.sh (macOS, Linux) both run
# this, so the list of sources is kept in one place.
set -e
gcc -O2 -g -Wall -Wextra -std=gnu11 -o build/sim/sim \
    tools/sim.c src/assets.c src/level.c src/doors.c src/player.c \
    src/render.c src/statics.c src/status.c src/sound.c src/actors.c \
    src/intermission.c src/text.c src/game.c src/menu.c src/controls.c \
    src/save.c src/automap.c -lm
./build/sim/sim assets/wolf3d.dat build/sim
