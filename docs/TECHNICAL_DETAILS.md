# Wolfenstein 3D for Nintendo 64: technical details

How the port is built, how it works, and where and why it follows or departs
from id Software's source. For what the project is, what you need, and how
to build and run it, see [README.md](../README.md).

## Building, in detail

### ROM names

The ROM is named after the game in full, as most ROMs are: `Wolfenstein
3D.z64` for the shareware and the full game (3.0 MB and 6.0 MB), `Spear of
Destiny.z64` for Spear of Destiny (6.4 MB), unless `-Name` asks for another.

make cannot build a file whose name has a space in it, so it builds the ROM
inside `build\` under a plain name - `build\wolf3d.z64` or `build\spear.z64`,
by the game extracted - and the script copies it out under the full name or
`-Name`'s. (Its ELF is `build\build\wolf3d.elf`, where libdragon's rule for
a ROM at `build/wolf3d.z64` expects it.) The plain name stays per game
rather than following `-Name` because the ROM records its program's file
name, `wolf3d.elf.stripped`, in its table of contents: this way a ROM is the
same byte for byte whatever it is called.

The two games are named apart because a cartridge is given its SRAM by the name
of the ROM - emulators and flash carts keep the saves in a file beside it -
so two games under one name would share a cartridge, its ten save slots and
its high score table. They are kept apart inside the cartridge as well (see
Saved games and High scores), so nothing breaks if you do name them the
same; they just share the room.

### What extraction writes

Extracting writes `assets\wolf3d.dat`, `assets\wolfbig.dat` and the raw
sounds and songs in `assets\audio\`, but it leaves a file untouched when the
bytes it was about to write are the bytes already there. The build takes its
cue from those files' timestamps - the songs and effects go through an OPL
emulator and the converter, about a minute of it, and the blob decides
whether the ROM is linked again - so extracting the same game twice, or
coming back to it from the other game, does not pay for work whose answer
has not changed. Files that belong to a game no longer extracted are swept
out of `assets\audio\`, so the folder holds one game's and no leftovers.

Naming a folder with `-Data` always extracts from it again; without it,
`-Rom` uses whatever was extracted last, and `-Assets` extracts again from
`gamedata\`. `-Clean` runs `make clean`, which removes `build\` and the ROMs
but not `assets\`, so it does not extract again unless `-Assets` or `-Data`
is given as well.

### The palette and the sign-on screen

The palette and the sign-on screen are not in the data files: they were
linked into the executable, which id packed with LZEXE. `tools\lzexe.py`
unpacks it (Spear of Destiny's has had LZEXE's signature wiped, so it is
recognized by LZEXE's own unpacking code instead) and the extractor finds
both inside - the palette by its sixteen EGA colors, the sign-on screen by a
hash of some of its rows, so no pixels of it are kept here.

- **The palette is required.** Without the executable (or with a version
  where it is not found), id's source release can stand in: `-Source`
  pointing at the `WOLFSRC` folder of
  [id-Software/wolf3d](https://github.com/id-Software/wolf3d) takes it from
  `OBJ\GAMEPAL.OBJ` instead. (That is Wolfenstein's; Spear of Destiny's
  differs in two colors.)
- **The sign-on screen is optional.** It only ever comes from the
  executable; if that is missing, or is a version it is not found in, the
  game simply starts at the PG-13 screen.

### Tables from id's source

The tables id's source would supply (sound and song numbers, the digitized
sound map, each floor's song, the sprite and pic lists, the stereo tables -
Wolfenstein's and Spear of Destiny's) are carried in `tools\wolf_tables.py`;
given `-Source`, every asset build parses the source again and checks they
agree. `src\sprites.h`, the game's sprite numbers, is generated from the same
tables (`--write-tables` rewrites both), and every extraction checks it still
matches.

`-Source` (`--source` for `build.sh` and the extractor) is a maintainer's
option, and is left out of the help screens, the README and the build window
on purpose: a build needs only the game's files and its executable. It
still works: `.\build.ps1 -Assets -Source
D:\WOLFSRC` extracts again with the tables checked against the source, and
with the source's `OBJ\GAMEPAL.OBJ` standing in for a missing palette.

### The toolchain

The toolchain is libdragon, in a Docker image called `n64-libdragon`:
`tools\Dockerfile.libdragon` adds the library and its tools to the official
toolchain image, at the libdragon ref its `LIBDRAGON_REF` argument names
(`trunk` by default). `build.ps1` builds it the first time it is not there
(it compiles libdragon, so that first build takes several minutes); to
build it by hand:

    docker build -f tools\Dockerfile.libdragon -t n64-libdragon tools

The ROM's name is passed to `make` as `ROM=`. The header's title is not
passed at all: the Makefile reads the blob's version word (byte 10 of
`assets\wolf3d.dat`) itself and sets `N64_ROM_TITLE`, so the title cannot
disagree with the data - and no script has to hand `make` a quoted title
with a space in it, which Windows PowerShell 5.1 cannot pass intact (see
below). The scripts take the default name from the same word.

### build.sh, for macOS, Linux and Git Bash

`build.sh` does what `build.ps1` does, option for option (`--rom`,
`--data`, `--name` and so on), with a few differences that matter off
Windows - and one that matters on it:

- It runs the container with `--user` set to you, so that on Linux what the
  build writes into the project - `build/` and the ROM - is yours and not
  root's, and with `HOME=/tmp`, as the image has no home for that user.
  Under rootless Docker, whose root already is you, it leaves `--user` off.
  (That case has not been tried.)
- It finds `python3`, or `python` if that is Python 3.
- In Git Bash (or MSYS2, or Cygwin) Docker is a Windows program, and MSYS
  rewrites any argument that looks like a Unix path into a Windows one
  before passing it on - which turned the container's `-w /src` into
  `C:/Program Files/Git/src`. So for Docker alone, `build.sh` and
  `tools/sim.sh` switch that off (`MSYS_NO_PATHCONV`, and MSYS2's
  `MSYS2_ARG_CONV_EXCL`) and give it the project folder as a Windows path
  from `cygpath -m`. Python, a Windows program too, still gets the
  rewriting, which it needs to open the files. There is no Unix owner to
  get right on Windows, so `--user` is left off there, as `build.ps1` does.
- It is written for bash 3.2, the version macOS ships, so it avoids
  associative arrays, `${var,,}`, `mapfile` and `set -u`, which trips over
  an empty array in that version. It has been run under bash 3.2 and, in
  full, on Ubuntu and in Git Bash, where its ROMs are byte for byte
  `build.ps1`'s.

libdragon's table of contents near the start of the ROM (`TOC0`) records
the program's file name - `wolf3d.elf.stripped`, say. That name is make's
own, which follows the game and not `--name` (see ROM names), so builds of
the same game compare byte for byte whatever they are called.

Both scripts, and the extractor, find the game's files whatever the case of
their names: DOS named them in capitals, but not every copy keeps them that
way, and Linux tells `VSWAP.WL1` from `vswap.wl1`. `.gitattributes` keeps
the shell scripts' line endings Unix ones, which bash needs, even in a
checkout on Windows.

### The build window

`build-gui.ps1` - opened by `build-gui.cmd` in Windows PowerShell, with no
console left behind - is a Windows Forms window over `build.ps1`, not a
second build. It runs `build.ps1` with the options chosen, under the same
PowerShell as the window, and shows its output.

- **One check.** Its lights and `build.ps1`'s help screen both come from
  `Get-BuildRequirements` in `tools\requirements.ps1`, which gives each
  requirement a state (ok, missing, later - needed, but made by the first
  build - or unknown), whether a build needs it now, a line for the help
  screen and a shorter one for the window. Asking Docker can take seconds,
  so the window runs the check in a runspace of its own and collects it on
  a timer. It checks again when a folder or the name changes, when the
  window is activated, after a build, and every five seconds while Docker is
  not running.
- **The output.** The build is a child process whose output a small C#
  class (`Wolf3dBuildGui.Runner`) queues a line at a time; the window's
  timer empties the queue into the output box. A PowerShell script block
  cannot run on the threads .NET reads a process's output on, so the queue
  is what crosses between them.
- **Stop** ends the build's process tree (`taskkill /T`) and then its
  container, which would otherwise carry on compiling into `build\`.
  `build.ps1` and `tools\sim.ps1` label their containers with the project
  folder (`wolf3d-n64.project=`), so Stop ends this project's and no other's.
- **Remembered choices** go to `build-gui.json` beside it, written as UTF-8
  so that either PowerShell reads the other's. It holds your own folders, so
  `.gitignore` keeps it out. A game folder that is the project's own
  `gamedata` is not passed as `-Data`, which would extract it again on
  every build.
- **Which game is extracted.** The game-data line reads the blob's version
  (the first 12 bytes of `assets\wolf3d.dat` are enough) and names the
  game. When the chosen folder holds another one, `build.ps1` without
  `-Assets` or `-Data` would build the game already extracted - the
  gamedata folder is not passed as `-Data` - so the window adds `-Assets`
  itself and says so in the output, and the line reads "switching to" it.
  `build.ps1`'s help screen names the same case and points at `-Assets`.
- **Scaling.** The window is laid out at 96 dots per inch and scaled to the
  screen's by hand. Windows Forms' own scaling, on a screen at 200%, both
  scaled the output box and stretched it with the window it is anchored to,
  leaving its bottom half below the window.
- It needs a single-threaded apartment for its dialogs. Windows PowerShell
  and PowerShell 7 both give it one; anything that does not is handed to
  Windows PowerShell.

It was tested by driving the real window from outside - UI Automation to
find it, Windows messages to click and type - through a ready state, a
folder without the game, Docker unreachable, a build, a clean build
stopped part way (the container gone with it), a full rebuild whose ROM
matched the reference, the checks with all four demos in sync, remembered
choices across a restart, and opening it from PowerShell 7.

### Windows PowerShell 5.1

The scripts run under Windows PowerShell 5.1, which every Windows has, as
well as PowerShell 7. Two things in 5.1 needed care:

- **Native programs' stderr.** With `$ErrorActionPreference = "Stop"`, 5.1
  ends the script when a native program writes to stderr while it is
  redirected. Docker does that when an image is missing and when it is not
  running - exactly when `build.ps1` should explain rather than stop - and
  all through building its image. Docker and Python therefore run through
  `Invoke-Native` (`tools\requirements.ps1`), which switches that off for
  the call; the exit code says how it went.
- **Quoted arguments.** 5.1 cannot pass a native program an argument that
  holds both quotes and a space intact, so `N64_ROM_TITLE="Wolfenstein 3D"`
  reached the ROM tool in pieces and linking a ROM failed. The Makefile now
  reads the title from the blob (see The toolchain), and the scripts pass
  only `ROM=`.

### Audio build options

The music is sequenced and the AdLib effects synthesized by default (see
"Sound and music"). Inside the container, `make MUSIC=wav` and `make SFX=wav`
bring back the older rendered versions - music exact but about 6 MB more,
effects about 5 MB more. To check the effects synthesizer against Nuked-OPL3:

    gcc -O2 -o build/adlib_check tools/audio/adlib_check.c src/adlib.c tools/audio/nuked-opl3/opl3.c
    ./build/adlib_check assets/audio

## Emulators

The ROM is written for the console, not for any emulator, and only
emulators that emulate the hardware itself run it reliably.

**Why.** Commercial N64 games were all built with Nintendo's development
kit: Nintendo's boot code (IPL3), its libultra runtime, and its RSP
microcode for graphics and sound. Emulators were developed by testing
against those games, so many save effort by recognizing Nintendo's code and
imitating it instead of emulating the chips: they identify the boot code by
its checksum and fake what it does, recognize the graphics and audio
microcode and reimplement it (high-level emulation), and implement the CPU
only as far as libultra needs. This port is built with libdragon, which has
its own boot loader, its own runtime and its own microcode - the audio mixer
and `rdpq` - and drives the RDP with raw command lists. None of that is
Nintendo's code, so it falls outside those shortcuts. The few commercial
games that wrote their own microcode (Factor 5's, *World Driver
Championship*) had the same trouble on those emulators for years.

libdragon's own README puts it plainly: "At the moment, the only emulator
that accurately emulates the hardware (and does not just focus on playing
old classics) is Ares." It recommends ares's Homebrew mode, which adds
checks for things that would break on a console.

**What was tried**, with this port and with libdragon's own examples:

- **ares v148** runs it. It is what the port was developed and checked on.
- **simple64 v2024.12.1** runs it at 60 fps, but at random - after
  seconds or after many minutes of the attract loop, menus or play - it
  stops. Usually libdragon's crash screen says `RSP CRASH | rsp_queue |
  rspq_syncpoint_wait`, "wait loop timed out (200 ms)", with the `rsp_rdpq`
  overlay current, the RSP at PC 0x258, `SP_STATUS` showing SIG1 and
  `DP_STATUS` 0xa8 with `DP_CURRENT == DP_END`. SIG1 is libdragon's
  `SP_STATUS_SIG_RDPSYNCFULL`: `RDPQCmd_SyncFull` sets it when it sends a
  SYNC_FULL, and only the CPU's RDP interrupt handler (`__rdpq_interrupt`)
  clears it. So the RDP had run everything it was given, but the interrupt
  for the last SYNC_FULL never reached the CPU, and the RSP waits at the
  next SYNC_FULL for an acknowledgement that cannot come. Once the picture
  just froze at 0 fps with no crash screen - a wait with no timeout, most
  likely the audio queue waiting on an AI interrupt, which would be the same
  kind of lost interrupt. It is not the game's code: a 60-line libdragon
  program doing only this port's frame - the 40 strip uploads and flipped
  rectangles, a copy-mode blit, `rdpq_detach_wait()`, `display_show()`, the
  mixer topped up - stopped with the identical crash screen under simple64
  and ran three minutes clean under ares. It is rare and irregular enough
  that runs of several minutes, with 16 syncs a frame or with the window
  losing focus, did not bring it back on demand. (libdragon's `rdpqdemo`
  is no help as a comparison: at this libdragon it uses the FPU inside
  interrupt handlers, which libdragon now traps, so it crashes with a CPU
  exception before it can show anything.)
- **Mupen64Plus 2.6.0** never produces a frame - not even the first VI
  interrupt - under the dynamic recompiler or the pure interpreter, with the
  HLE or the cxd4 RSP, and with rice or glide64mk2 including their
  frame-buffer options. It cannot identify the CIC from libdragon's boot code
  (its checksum, `000000844C074690`, matches none of the ten it knows, so it
  assumes 6102), but that is not the cause: libdragon's own `vtest` built
  from the last libdragon before its own IPL3 (`a226d57e7`, October 2024),
  with Nintendo's 6102 boot code and valid header checksums, does not reach
  a first frame either, though it runs at 60 fps on simple64. It stalls in
  libdragon's startup, before anything is drawn.
- **Project64 1.7** never runs a cartridge's boot code: it fakes the retail
  boot by copying the ROM from `0x1000` to `0x80000400` and jumping there.
  In a libdragon ROM what sits at `0x1000` is the rest of libdragon's boot
  loader, written to run from RSP memory (it puts its stack at
  `0xA4001FF0`), and the emulator stops with "Executing from non-mapped
  space". libdragon's own trunk example stops the same way; the pre-IPL3
  build gets past the boot and then trips a breakpoint inside the
  emulator's interpreter.
- **Project64 3.0.1** says "Emulation started" and shows a black screen.
  libdragon's own examples - `helloworld`, `vtest`, `spriteanim` and
  `fontdemo`, built from the same libdragon - do exactly the same, where
  ares runs all four.

libdragon also leaves the header's two checksums at zero and its country
code empty, since its boot code, not Nintendo's, is what runs; none of the
emulators tried objected to either.

So an older emulator is not a matter of a different boot block. A ROM for
them would have to leave libdragon's runtime, its RSP audio mixer and
`rdpq` behind - a new platform layer, with the music mixed on the CPU at a
cost to the frame rate - and carry Nintendo's own boot code, which is
copyrighted.

## Game flow

The game starts with the sign-on screen. Its picture's own "One moment..."
shows for half a second - on a PC it stayed up while the game loaded - and then
the memory and hardware readout lights up and
the screen waits for a button - A, B, Z or Start - where it says "Press a
button" (Spear of Destiny's asks for none, and goes on after three seconds),
then the PG-13 screen and the attract
loop; A, B, Z or Start on the title, credits, high scores or a demo opens the
main menu. In the menus,
up and down move the gun cursor, A or Start selects, and B goes back. B at
the main menu during a game returns to it. In a yes/no box, A is yes and B is no.
Outside a game - at start-up, after a game over, from the demos - the gun
starts on New Game (the shareware DOS menu started it on Read This! and then
left it where it was last); during a game it stays where it was left.

Using the elevator switch shows the end-of-floor stats screen and then goes on
to the next floor - or, from an episode's hidden elevator, to its secret
floor 10, and back afterwards to the floor after the one it was found on.
On the stats screen, A, Z or Start skips the count, and a second press
leaves. An episode ends on its ninth floor: with the boss dead, at the exit
tile (Hans Grosse, Gretel Grosse) or with the death cam's replay of the
boss's death (the others). On the victory screen any of A, Z or Start moves
on. On the end-of-episode text pages, D-pad left and right turn pages, A
goes to the next page (or leaves from the last), and B or Start leaves, to
the high scores and then the title. The same keys read "Read This!". Spear
of Destiny's floors run one after another, with two secret ones, and end
with the Angel of Death; its ending is a run of pictures, each left with a
button.

## How it is put together

    tools\extract_assets.py    game files  ->  assets\wolf3d.dat, wolfbig.dat, audio\
    tools\wolf_tables.py       tables from id's source the extractor needs
    tools\lzexe.py             unpacks the executable for its palette and sign-on
    tools\Dockerfile.libdragon the n64-libdragon toolchain image
    src\wolf.h                 the game's shared declarations
    src\sprites.h              the sprite numbers (generated; see above)
    src\blob.S                 .incbin of that blob
    src\assets.c               header parsing, and reading wolfbig.dat
    src\level.c                SetupGameLevel + ScanInfoPlane
    src\doors.c                the DOORS section of WL_ACT1.C
    src\statics.c              the STATICS section of WL_ACT1.C
    src\actors.c               WL_STATE.C, WL_ACT2.C: every enemy, the death cam
    src\player.c               ControlMovement / Thrust / ClipMove / Cmd_Use
    src\render.c               the ray caster and DrawScaleds
    src\status.c               the status bar and the FPS readout
    src\game.c                 GameLoop / PlayLoop / DemoLoop, a frame at a time
    src\intermission.c         LevelCompleted, Get Psyched, Victory
    src\menu.c                 US_ControlPanel and the menus, Extras included
    src\controls.c             PollControls: the pad as Customize controls sets it
    src\automap.c              the map L shows (not in the DOS game)
    src\text.c                 the fonts and the text pages (WL_TEXT.C)
    src\sound.c                SD_PlaySound and its rules
    src\save.c                 SaveTheGame / LoadTheGame and the ten slots
    src\adlib.c, adlib.h       the OPL2 channel the AdLib effects play on
    src\n64audio.c             libdragon mixer playback of the audio
    src\n64save.c              cartridge SRAM
    src\n64prof.c, n64prof.h   the profiler overlay and the benchmark ROM
    src\main.c                 display, reading the controller, frame loop
    build.ps1, build.sh        the build, on Windows / on macOS and Linux
    build-gui.ps1, .cmd        the build window, on Windows
    tools\requirements.ps1     what a build needs, for build.ps1 and the window
    tools\sim.c                the host build of the game's checks
                               (a third argument, readme, takes
                               candidates for the README's floor 1
                               screenshots instead)
    tools\sim.ps1, sim.sh      build and run them (Windows / macOS, Linux)
    tools\sim_run.sh           ...inside the container, for both
    tools\sheet.py             the sim's contact sheet
    tools\audio\imf2xm.c       AdLib music -> XM modules, at build time
    tools\audio\render_audio.c AdLib music and effects -> WAV (kept only for
                               MUSIC=wav / SFX=wav)
    tools\audio\adlib_check.c  src\adlib.c against Nuked-OPL3
    tools\audio\nuked-opl3\    Nuked-OPL3, the OPL emulator (LGPL)

`extract_assets.py` reads the container formats the DOS game used:
`GAMEMAPS` (Carmack-compressed, then RLEW-compressed, per `ID_CA.C`),
`VSWAP` (64x64 CI8 wall pages stored column major, and the compiled
sprites after them), `VGAGRAPH` (Huffman-coded artwork), and the executable
(LZEXE-packed; `tools\lzexe.py` unpacks it) for the 768-byte VGA palette and
the sign-on screen, which were compiled into the program. It writes two
big-endian files:

- **`wolf3d.dat`**, 1.0 MB for the shareware and 1.3-1.4 MB for the others,
  which the game uses in place - it never parses it into other structures.
  It is linked into the program, and libdragon compresses the program in
  the ROM and unpacks it into RAM at boot, so it costs its full size in RAM:
  the walls, the sprites, the pics the menus and screens draw, the sound
  tables, the fonts, the articles and the demos.
- **`wolfbig.dat`**, 0.4 MB for the shareware and about 1.2 MB for the
  others: what is only needed now and then, kept in the ROM's filesystem and
  read when it is - each floor's two map planes, 16 KB a floor, read as the
  floor loads, and the full-screen pictures (the sign-on, the title, the
  credits, Spear of Destiny's end screens), read into one of two 64 KB
  buffers as a screen shows them. With 60 floors and more artwork, the full
  game and Spear of Destiny would not fit the console's 4 MB otherwise.

The rest of the ROM is the audio filesystem: 1.8 MB for the shareware, about
4 MB for the others, which have more songs.

The three games number their sprites, sounds and songs differently. The
blob uses one numbering for all of them - Wolfenstein's sprite and sound
lists, with Spear of Destiny's own added after them, each version's placed
by name - and the game plays songs by what they are for (the title, the
menus, the stats screen...) rather than by number. Whatever a version does
not have is simply empty: the shareware's missing sprites, Spear of
Destiny's absent "Read This!".

### Scenery

Plane 1 values 23..74 name an entry in `statinfo[]`: which sprite to draw and
whether it is furniture you cannot walk through (`statics.c`). Blocking pieces
go into the same `blockmap` the walls and doors use, so a table stops you the
way the DOS game's `actorat` did.

The sprites are id's compiled shapes, not bitmaps: each column is a list of
vertical runs, so transparency is structural and the renderer skips gaps
instead of color-keying them. `extract_assets.py` decodes them into the same
shape with 32-bit fields, and one thing there is easy to get wrong - a run's
`newstart` field is `pixel_base - starty` biased into a 16-bit word, so it is
frequently "negative" and has to be added modulo 65536, the way the DOS
scaler's segment arithmetic did it.

Drawing follows `DrawScaleds()`: the ray caster marks every tile a ray passed
through in `spotvis`, only scenery in a marked tile is considered, and each
piece is projected, sorted far to near, and drawn column by column - skipping
any column where the wall in front is nearer. Sprites scale exactly like wall
faces, so they carry the same 1.2 vertical stretch and come out 64 pixels wide
for every 77 tall.

### Sound and music

The DOS game, set to AdLib and Sound Blaster Pro, had three voices: FM music,
one FM sound effect at a time, and one digitized sound at a time on the DAC.
The port keeps that shape.

The N64 has no FM chip, so the AdLib parts go through an OPL emulator
(Nuked-OPL3) - the music at build time, the effects as they play:

- **Music** is `SDL_ALService()`'s register stream at 700 Hz, sequenced
  into XM modules by `tools\audio\imf2xm.c` and played by libdragon's
  xm64player on eight mixer channels, samples streamed off the cartridge.
  The songs use the OPL2 like a MIDI synth - one note per channel, pitches on
  equal temperament, no pitch bends, nothing changed mid-note but the
  envelope - so each distinct patch becomes an XM instrument with a short
  recorded sample per octave (0.2 s of attack, then a seamless loop of whole
  waveform periods, with the carrier's envelope divided out). The envelope
  goes in the volume column instead: for every note the converter runs the
  patch through the emulator with that note's own key-on and key-off times,
  so short notes that ring out on a slow release sound as they did. Vibrato
  and tremolo (7 cents and 1 dB here) are dropped. The shareware's eleven
  songs come to 1.6 MB, against 8 MB rendered (the full game's 27 and Spear
  of Destiny's 24 to about 3.7 MB); played back on the host, each one's
  loudness is within 0.81-1.18 of the chip's and tracks it 50 ms by 50 ms
  with a correlation of 0.86-0.99 (`imf2xm --check`). Notes can move by up
  to 5 ms, the length of an XM row at the fastest tempo. The floors use
  `songs[]` from `WL_PLAY.C` - each version's own list; the shareware's is
  GETTHEM, SEARCHN, POW, SUSPENSE, repeated, then WARMARCH for the boss
  floor and CORNER for the secret one.

  With `MUSIC=wav` each song is instead rendered by
  `tools\audio\render_audio.c` at 22 kHz, VADPCM-compressed and streamed on
  one channel. Each song is played twice and the second pass kept, so the
  loop starts from the chip state the song leaves behind and wraps cleanly.
- **AdLib effects** are `SDL_ALPlaySound()` - the instrument on channel 0,
  then one F-number byte per 140 Hz tick - and they are **synthesized on the
  N64** (`src\adlib.c`) from their original AUDIOT chunks, 11 KB for all 87,
  where rendering them took 5 MB. They don't suit samples the way the music
  does: most slide in pitch every tick, often over several octaves, and the
  longest have a modulator attack lasting seconds, so the timbre never
  settles. So the ROM drives an emulated OPL2 channel the way `ID_SD.C` drove
  the real one, and the chip keeps its state from one effect to the next as
  it did - an effect that is cut off or stopped rings out on its release.
  `adlib.c` is Nuked-OPL3 cut down to what those writes reach (two operators
  in series, no feedback, OPL2 waveforms, no rhythm mode), so it is under
  Nuked's LGPL 2.1+. It gives Nuked's samples exactly: `tools\audio\adlib_check.c`
  compares every 49716 Hz sample over all 87 effects, alone and cutting each
  other off (37 million samples, no differences), and the ROM's output hashes
  the same as the host's. The mixer takes every second sample, 24858 Hz.
  Emulated naively it cost 22% of the CPU while an effect sounds; working out
  register-derived values on writes, and skipping the envelope logic on
  samples where it provably changes nothing, brought that to about 5%.
  Nothing runs once an effect has fallen 54 dB (the level the rendered
  effects' tails used to be cut at). `SFX=wav` renders them to wav64 instead.
- **Digitized sounds** are the 8-bit, 7042 Hz recordings in `VSWAP`,
  converted as they are - the shareware's 20, the full game's 46, Spear of
  Destiny's 40.

`sound.c` is `SD_PlaySound()`: a sound goes to the DAC if `wolfdigimap` has a
recording for it, otherwise to the FM voice, and replaces what is playing
there only if its priority is at least as high. Door sounds are placed with
`lefttable`/`righttable` and re-panned every frame as you move, the way
`UpdateSoundLoc()` drove the SB Pro mixer. Every sound the game's actions make
in DOS is wired in: doors, locked doors, push walls, pickups, weapons, the
elevator (the game waits for its sound, as `SD_WaitSoundDone()` did), bumping
walls, and "nothing to use here".

Things that look wrong and are not:

- **The shipped data does not match `AUDIOWL1.H`.** That header declares 69
  sounds; the shareware files use `AUDIOWL6.H`'s 87-sound layout (288 chunks,
  and `wolfdigimap` names `YEAHSND`). So the extractor's tables in
  `tools\wolf_tables.py` - the enums, the digitized map, the song list and
  the stereo tables - were generated from the source text rather than typed
  in, and are checked against it whenever `-Source` is given.
- **The shareware has only 20 of the 46 digitized recordings.** The list in
  `VSWAP.WL1` describes all of them, but only the "upload version" ones have
  data, so sounds such as the level-done jingle play on the FM voice - as
  they did in the shareware release. (The full game has them all.)
- **Spear of Destiny has its own sound list.** Its sounds are numbered
  differently, and 22 of Wolfenstein's - mostly its bosses' voices - are not
  there; the game asks for sounds by Wolfenstein's numbers with Spear of
  Destiny's own added after, and the blob maps each to the data's, or to
  nothing.
- **Grinding along a wall repeats the bump sound every tic.** `HITWALLSND` is
  sound number 0, and `SD_SoundPlaying()` returns the playing sound's number,
  so while it plays it reads as "nothing playing" and `ClipMove()` starts it
  again. That is what the DOS code does.

The mix is a judgement call. Rendered straight, the FM parts come out far
quieter than the digitized sounds at their 8-bit full scale, so the renderer
lifts music 2.5x (as much as the loudest song takes without clipping) and FM
effects 3x - one gain per kind, so songs keep their relative levels.

Otherwise audio costs little frame rate: the mixer and VADPCM decoding run on
the RSP (the XM player's per-row work on the CPU is small), and
the queue is topped up after each frame's blits, when the RSP is idle. The
queue is three 1/25 s buffers, which keeps a sound's delay to about a tenth of
a second.

### Enemies

`src\actors.c` is `WL_STATE.C` and `WL_ACT2.C`, all of it, built as the DOS
game was for Wolfenstein or (`#ifdef SPEAR`) for Spear of Destiny, which here
the data decides: guards, officers, SS, mutants and dogs, standing and
patrolling, the dead-guard decoration, every boss and their projectiles. The
state tables are the DOS ones frame for frame, and `DoActor()`,
`SightPlayer()`, `CheckLine()`, `T_Chase()`, `T_Path()`, `T_Shoot()`,
`T_Bite()`, `SelectChaseDir()`/`SelectDodgeDir()`/`SelectRunDir()`,
`DamageActor()`, `KillActor()` and the bosses' own routines follow the
originals line by line. So enemies:

- notice you by sight (within a view cone, after a random reaction delay) or
  by hearing a gun anywhere in a connected room - unless they were placed in
  ambush, in which case only sight wakes them;
- open doors, patrol along the arrows on the map, dodge while in your line of
  fire, and shoot with the original odds - worse at range, worse again if you
  are running and they are on screen;
- take double damage before they are alerted, drop a clip (or a machine gun,
  from an SS if you have none), and give 100 (guard), 200 (dog), 400
  (officer), 500 (SS), 700 (mutant) or 5000 (a boss) points;
- follow `CalcRotate()` for which of their eight views you see.

The bosses are the DOS ones too. Hans and Gretel Grosse shoot and drop the
key to the exit; Dr. Schabbs throws syringes and Otto Giftmacher and General
Fettgesicht rockets, all three backing off when you close within four tiles;
the fake Hitlers breathe fire; Mecha Hitler's suit falls apart into Hitler,
who runs at you. Spear of Destiny's Trans Grosse, Barnacle Wilhelm, the
Ubermutant and the Death Knight drop keys; the Angel of Death fires sparks
three at a time and then has to catch her breath; spectres fade when shot and
rise again. The Pac-Man ghosts on episode three's secret floor cannot be shot
and hurt at a touch. A boss's last words last as long as their recording
does, as `SpawnSchabbs()` and its fellows arranged by setting the state's
length.

Projectiles, their smoke and explosions are actors that leave the list when
they are done, so the actor list is a real list as `objlist` was in DOS: new
actors go on the end, `DoActor()` runs down it, and a slot freed is the next
one given out. That last matters, as DOS's `actorat` could keep pointing at
a slot after its projectile had left it, and what the next actor put there
does the same here.

**The death cam.** Dr. Schabbs, Hitler, Otto Giftmacher and General
Fettgesicht end their episodes with it, as `A_StartDeathCam()` does: when the
boss has finished dying, a pause, the view fizzles out to "Let's see that
again!", and the player is put back where they fired the last shot from,
turned to the boss and backed off clear of walls, and watches the death
again before the victory screen.

Rooms matter. Each floor tile carries a room number, and an open door joins
the rooms either side (`areaconnect`). Enemies in rooms not joined to yours
do not think at all until they are seen, and cannot hear you. That is also
what makes door sounds and enemy noise local.

Getting hit flashes the screen red, and a pickup white, by switching between
the ten palettes `InitRedShifts()` builds - precomputed by the extractor. At
zero health, `Died()` plays out: the view swings round to face the killer,
dissolves to red with `FizzleFade()`'s pixel order, waits, and then either
restarts the floor with a life gone (health, ammo and weapons reset, score
back to what it was when the floor began) or, on the last life, fades out to
the high scores and then the title.

One deliberate fix to the original: no moonwalking. `SpawnNewObj()` starts
an actor's first state at a random tic count, and `DoActor()` treats a count
of 0 as a state that never ends. So about one patrolling guard, SS or dog
in fifteen walked its route stuck on one frame, until something changed its
state. BJ's victory run froze the same way. A zero count now becomes a full
state; the random number is still drawn, so the rest of the random sequence
is unchanged. The demos keep the original behavior (see below).

The difficulty chosen under New Game decides which enemies exist (each has
three copies on the map, for the setting that adds it) and the bosses' hit
points. "Can I play, Daddy?" also quarters the damage you take.

Faithful quirks, kept on purpose:

- **Doors are see-through once a quarter open.** `CheckLine()` compares an
  intercept that still carries the tile number against the door position.
- **A blocked nearest target means a miss.** `GunAttack()` means to fall back
  to the next target when a wall is in the way, but never resets its distance,
  so it cannot find one.
- **Ghosts open doors** and count towards the kill ratio, though they cannot
  be killed, so episode three's secret floor never reaches 100% kills.
- **Dr. Schabbs and Hitler cry out twice**: `KillActor()` calls
  `A_DeathScream()` itself, and their first death frame calls it again.

Where Wolf4SDL, the reference the demos were checked against (see "The title
and the demos"), changed the DOS code - Hans and Gretel facing no way at
first, no second death scream - the port keeps id's source.

One deliberate change: `T_Shoot()` decides whether you are running by
comparing `thrustspeed` - the distance moved that frame - against 6000. At
70 fps a frame is a tic and walking comes to 5250; at the N64's two or three
tics a frame, walking would count as running. It is compared per tic here.

**The sprites are looked up by their enum numbers.** `VSWAP.WL1` is laid out
by the full game's sprite list and simply leaves the chunks it does not ship
empty - mutants, officers, ghosts and the later bosses - so `SPR_GRD_S_1` is
chunk 50 of the sprites in the shareware file as in the registered one.
Spear of Destiny's list is its own, so the extractor places its sprites by
name, under Wolfenstein's numbers where they have one and after them where
they do not.

Thinking costs little: all the game logic, enemies included, is under
0.3 ms a frame (see Profiling).

### Weapons

`player_weapon()` is `Cmd_Fire()`, `T_Attack()` and `CheckWeaponChange()`
from `WL_AGENT.C`, driven by the same `attackinfo[]` table: four frames of 6
tics each, with the shot, stab or loop-back happening as a frame is left. That
gives the original rates without any special cases - one pistol shot per
press (holding does nothing), a machine-gun shot every 12 tics held, the
gatling gun twice as often. Run dry and the knife comes out at the end of the
attack; pick up ammo and the weapon you had chosen comes back. You cannot
change weapons or use doors mid-attack, as in DOS.

Two adaptations:

- **Choosing a weapon.** The DOS game picks one directly with keys 1-4; the
  pad has no four spare buttons, so Wpn + and Wpn - (C-up and C-down, unless
  customized) step through the weapons you own. With no ammo there is no
  choice, same as the original.
- **The controls moved.** By default Z is the trigger, so it fires (as it
  does in most N64 shooters), and run moved from Z to B. Customize controls
  can put either anywhere.

The weapon sprites are `SPR_KNIFEREADY` on, the last twenty sprites in the
file. They are drawn like
`SimpleScaleShape(viewwidth/2, shape, viewheight+1)`: 161 VGA pixels each
way, which is 161 columns by 193 rows here, standing on the bottom of the
view.

### Picking things up

`GetBonus()` gives what the DOS game gives: 8 rounds for a clip, 4 for the
small one, 25 health for a first aid kit, 10 for food, 4 for dog food, 100 /
500 / 1000 / 5000 points for a cross / chalice / bible / crown, a weapon and 6
rounds for the machine gun and the gatling gun, and a full heal, 25 rounds and
an extra life for the one-up. An extra life also arrives every 40 000 points.
Items you cannot use are left where they are - walk over food at full health
and it stays on the floor.

Two things about this are worth knowing before changing it:


- **The grab happens inside the renderer**, in `collect_scaleds`, because
  `TransformTile()` is what decides an item is close enough and `DrawScaleds()`
  calls `GetBonus()` right there. The test is in view space: within one tile
  ahead and half a tile to either side. That is why you have to be facing
  roughly towards something to pick it up, and why an item taken this frame is
  not drawn.
- **Keys reset when you change floor**, health, ammo, score and lives do not.
  `GameLoop()` clears `gamestate.keys` on `ex_completed`, so `level_load()`
  does the same.

### The status bar

`STATUSBARPIC` and the pics that go on top of it - the digits, BJ's face, the
key slots and the weapon - come out of `VGAGRAPH.WL1`, and `status.c` places
them at the DOS game's own coordinates (`DrawFace`, `DrawHealth`, `DrawLives`,
`DrawScore`, `DrawAmmo`, `DrawKeys`, `DrawWeapon` in `WL_AGENT.C`, `DrawLevel`
in `WL_GAME.C`). `StatusDrawPic()` takes x in eight-pixel columns and y in
pixels from the top of the bar, so those numbers are unchanged. BJ glances
around on `UpdateFace()`'s timing, driven by the same 256-byte `rndtable` from
`ID_US_A.ASM`.

The face follows health in bands of 16 points, three glances a band, with
`FACE8APIC` once health reaches 0 - or, in Wolfenstein 3D, the mutant face,
`MUTANTBJPIC`, when the last hit was Dr. Schabbs's syringe (`DrawFace()` checks
`LastAttacker`; a syringe hit you survive shows the usual face). The class is
taken when the hit lands, since the syringe itself is gone by then. That pic
is the last of the fixed list in the blob, so older extracted data, which
lacks it, still loads and shows the plain dead face. One detail needs care because this port
redraws the whole bar whenever anything on it changes, while the DOS game only
drew the face when something called `DrawFace()`. Picking up the gatling gun stamps BJ's grin
(`GOTGATLINGPIC`) over the face. The grin stays until the next `DrawFace()`:
taking damage, healing, dying, or the next random glance. `UpdateFace()` does
not glance while the pickup sound is still playing on the AdLib voice. So
`status.c` keeps the picture stamped over the face, with exactly those exits.
A grin taken just before an elevator lasts into the next floor, as it did in
DOS, because nothing redraws the face between floors. Spear of Destiny
stamps two more there: BJ's wide eyes at a hit of more than 30 points, and
his yawn after thirty seconds without moving - both of which reset the
glance's timer, so they are in the demos' reckoning too - and with Extras'
God Mode on it shows its god mode faces, as its `DrawFace()` did.

**`GFXV_WL1.H` does not describe the data that shipped.** The header declares
136 pics; `VGAGRAPH.WL1` holds 144, and every chunk from the status bar
onwards sits eight later than the header says. Rather than hard-code the
shift, the extractor finds the status bar by its size - it is the only 320x40
pic in the file - and locates the rest by their offsets from it, asserting the
dimensions the drawing code expects. Reading `VGAGRAPH` needs `VGADICT.WL1`
(255 Huffman nodes, head node 254, bits LSB first) and `VGAHEAD.WL1`
(three-byte offsets); chunk 0 is `STRUCTPIC`, a width/height pair per pic.

The bar is 320x40 and is shown at its own size, which is why the 3D view is
200 rows rather than the 192 that would match the DOS game's share of a 4:3
screen exactly. The eight-row difference shows a sliver more floor and
ceiling; the wall scale does not change, because it follows from the
horizontal field of view and the pixel shape rather than from the view height.

The real bar has nowhere to put a frame rate, so with Extras' Show FPS on
(it is off to begin with) `hud_draw_fps()` stamps one into the top-left
corner of the screen instead - a 3x5 font with a drop
shadow, inset from the edge because the VI border and a TV's overscan hide the
first few columns. It stays there when Change View shrinks the view, over the
border; the border is only drawn when it changes, so the readout first puts
back the border under itself (`view_border_restore()`).

Everything under `src\` except the N64 side - `main.c`, `n64audio.c`,
`n64save.c` and `n64prof.c` - is free of libdragon, which is what makes the
frame capture below possible. (`adlib.c` is libdragon-free too; the host
checks it with `tools\audio\adlib_check.c` rather than in the sim.)

### The renderer

`AsmRefresh()` in `WL_DR_A.ASM` walks the grid with tangent tables;
`render.c` does the same walk as a fixed-point DDA, which lands on the same
intercepts and is much easier to follow. Rays are not unit length - each is
the view vector plus the camera plane times the column's offset - so the
distance the DDA accumulates is already the perpendicular distance and there
is no fisheye to correct.

Everything that decides what a column *looks* like is the original logic:
which of a tile's two texture pages a face uses (`horizwall[]`/`vertwall[]`),
which way the texture runs, the door-side texture on jamb tiles, and a door's
texture offset by `doorposition[]`.

The view is 320x200 above the 40-pixel status bar (see below for why not 192),
and walls are scaled by 262 pixels per tile at one tile - `CalcProjection()`'s
218.75 times the 1.2 that square N64 pixels need to match 320x200 VGA ones.
The field of view is the original 72.4 degrees.

### Push walls

Secret walls are marked 98 (`PUSHABLETILE`) on the object plane. Pressing A
while facing one runs `PushWall()`: the wall claims the tile beyond it and
starts sliding, one tile every 128 tics (a little under two seconds), until
it hits something or has gone as far as it goes. Only one moves at a time, as
in DOS, and a pushed wall is never pushable again. `gamestate` counts secrets
found against the floor's total, ready for an intermission screen.

Two faithful details:

- **How far it travels.** `MovePWalls()` stops once `pwallstate` passes 256,
  and it adds a whole frame's tics at once, so an unobstructed wall moves
  three tiles when the count lands exactly on 256 and two when it skips over.
  On a fast PC at 70 fps it always lands, so that is what the game was
  designed around. `move_pwalls()` steps one tic at a time, which gives that
  behavior at any N64 frame rate instead of varying with it.
- **How it is drawn.** The sliding tile is marked with both high bits set
  (`0xc0 | tile`), which no door or jamb can produce. The ray caster moves the
  face the ray crosses back by `pwallpos`/64 of a tile along the ray's own
  step axis - `horizpushwall`/`vertpushwall` in `WL_DR_A.ASM` - rather than
  along the push direction. Seen head-on, which is nearly always, that is
  exact. Seen side-on, the block's side is drawn in the wrong place, just as it
  is in the DOS game.

Two things in the code look arbitrary and are not:

- The 3D view is drawn into `colbuf`, a column-major buffer shown on its side
  (see Profiling), because walls, scenery and the hands are all drawn a
  column at a time. `viewbuf`, for the screens in between, keeps
  `VIEW_STRIDE` at 328, not 320: from when the columns went there too, a
  320-pixel row is 40 cache lines and lands every column on the same 32 sets
  of the VR4300's data cache, and 328 is 41 lines - odd, so rows spread over
  every set.
- The view buffer is ordinary cached memory and the RDP reads RDRAM, so
  `data_cache_hit_writeback` has to run before the blit. libdragon's own
  display buffers are uncached, which is why the CPU does not draw into them
  directly: 64 000 uncached 16-bit stores a frame (the 320x200 view) would
  cost more than the render.

About 42-47 fps under ares; see Profiling for where the time goes.

The status bar is composed into its own buffer and blitted by the RDP next to
the view, for the same reason the view is: libdragon's display buffers are
uncached, so the CPU should not be drawing into them a pixel at a time.

### The end-of-floor screen

`intermission.c` is `LevelCompleted()` from `WL_INTER.C`: BJ breathing in the
corner, the floor's time against par, and the par-time bonus (500 points a
second under par), then the kill, secret and treasure ratios counted up with
the tally beeps. Each 100% ratio adds 10 000 points, and 0% gets the sad
sound. The secret floor gets a flat 15 000 instead - and so do Spear of
Destiny's boss floors, whose screen says which boss was defeated. The par
times are each episode's own, and the floor number is the floor's in its
episode. It plays the level-end song, and the points reach the status bar
under it as they are awarded.

The DOS function is a run of nested loops that each wait for the current beep
to finish. Here it is a small state machine that takes one step per tic and
holds while a sound plays, so the pacing is the same. The 160-row DOS layout
is drawn 20 rows down in the 200-row view, on the same background color.

The glyphs are the `LEVELEND` lump of `VGAGRAPH.WL1`. That lump sits 43 chunks
before `STATUSBARPIC`, which matches `GFXV_WL6.H` rather than the shareware
header. The time on each floor is counted in tics during play, as
`gamestate.TimeCount` was.

### Between floors: fades, "Get Psyched!" and the death fizzle

`game.c` is `GameLoop()` and `PlayLoop()`. Each DOS blocking loop (a fade,
`IN_UserInput()`, `Died()`) is a phase that gets one frame's tics at a time,
in `WL_GAME.C`'s order:

- **A new floor** fades "Get Psyched!" in, waits 70 tics (a button skips it)
  and fades out. The first view then fizzles in, invisibly because the screen
  is black, and the screen fades in.
- **The elevator** waits for its sound, then the music stops and the screen
  fades out. The stats screen fades in, and fades out again when dismissed.
- **A death** turns to face the killer and dissolves the view to red. With a
  life left, the floor restarts and the new view fizzles in over the red.
  With none left, the screen fades out to the high scores.
- **Pause** (L and R together) stamps `PAUSEDPIC` and holds the music in
  place, as `CheckKeys()` did, with the sign in the middle of the view as in
  DOS - but over the map it goes across the top of the screen instead, as
  Doom's does over its automap, so as not to hide the map. Carrying on is
  Doom's way too: DOS carried on at any key (`IN_Ack()`), and here only
  L + R again does, so a stray press cannot. Start still opens the menu, and Back
  to Game from there resumes play. Paused from the map, play returns to the
  map. L alone is the map (the Map input, unless customized), so that L + R
  is not also a map the map goes on its release, and not at all when L and
  R were down together meanwhile.

A fade is `VL_FadeOut()`/`VL_FadeIn()`: 30 steps (10 for the menus and text
pages) that move every palette entry toward a color and back. That color is
black, or dark red between menu screens (`MenuFadeOut()`). Moving every pixel
of the finished frame the same fraction of the way is the same arithmetic.
`main.c` does
this on copies of the two buffers, so the stats screen keeps the pixels it
draws a piece at a time. The DOS fades block the game, so nothing is rendered
under one and the cost does not land on play. (Blending black over the frame
on the RDP was tried first. On ares, the screen stayed black after the fade
ended.)

### Victory

In the first and fifth episodes, the ninth floor's exit tile (`EXITTILE` on
the object plane) runs `VictoryTile()`:

1. BJ is spawned on the tile south of the player and runs north with the
   original `s_bjrun`/`s_bjjump` states and sprites. (Spawned actors never
   start frozen on one frame; see Enemies.)
2. `VictorySpin()` turns the camera to face south and backs it five tiles
   north. Doors stop moving, enemies stop chasing, and damage is ignored.
3. BJ yells mid-jump. When `T_BJDone()` fires, the screen fades to
   `Victory()`: "you win!", total time, and average ratios over floors 1-8,
   with the `URAHERO` song. The full game adds its time verification code,
   three letters worked out from the total time, beside it.
4. The screen fades to `EndText()`, the episode's article. The shareware
   build's is the last chunk in `VGAGRAPH.WL1`.

The other episodes end with the death cam (see Enemies), which leads to the
same `Victory()`. Spear of Destiny's floors end with the Angel of Death:
the screen fades slowly to a dark teal, BJ collapses in four pictures to the
`XTHEEND` song, the averages come up - over all twenty floors, divided by
the fourteen with a tally, as `Victory()` did - and then `EndSpear()`'s nine
screens, each faded in with a palette of its own, the second with its two
captions. Those pictures are the only ones in any of the games not drawn in
the game's palette; the extractor stores their palettes with them.

`text.c` ports `ShowArticle()` and `PageLayout()`: the `^P`, `^E`, `^C`,
`^G`, `^L`, `^B` and `^>` commands, word wrap around pictures, and the page
footer, drawn with `STARTFONT`. The extractor rewrites the article's pic
numbers to indices in the blob. The page is 320x200, the whole VGA screen, so
it is centered on black without the status bar. After the text come the
high scores, as after a game over.

### High scores

`CheckHighScore()` runs when a game ends, after the last life (End Game
included) or after the end text. A score goes into the seven-row table above
the first row it beats, or above a row it ties if it reached a later floor.
The last row drops off. The table fades in with the `ROSTER` song. A score
that did not make it leaves the table up for `IN_UserInput(500)`, about
seven seconds, or until a button. Either way the game then goes back to the
title.

Wolfenstein and Spear of Destiny keep a table each. DOS never had to think
about it - the table lived in the config file, `CONFIG.WL6` or `CONFIG.SOD`,
one per game - and here the two ROMs have names of their own, so a cartridge
each. But one can still find the other's: a ROM built before this had one
name for both games, and `-Name` can still give them one. So the two tables
sit side by side in the last kilobyte, each with a mark of its own, and a
game reads and writes only its own; the saves are kept apart the same way,
and a shared cartridge costs only the room. The scores of one would make
no sense in the other in any case: the column beside the name is an episode
and a floor in Wolfenstein and a floor out of 21 in Spear. The settings -
view size, sound, the Extras switches, the customized controls - are shared
on purpose: they mean the same thing in both games.

A score that made it takes a name where its row is, as `US_LineInput()`
took one: up to 57 characters or 100 pixels, with the blinking I-bar
cursor. The pad has no keyboard, so a strip at the foot of the screen, in
the style of the menus' one, reads LETTER, A ADD, B ERASE, START DONE:

- **Up/Down** steps the last letter through space, A-Z, a-z, 0-9 and
  `.-'!&`. Held, it repeats.
- **A** (or right) adds a letter.
- **B** (or left) erases the last one. Held, it repeats. On an empty name, a
  fresh press of B is Esc, which leaves the name empty.
- **Start** finishes.

DOS kept the table in its config file. Here it is written to the cartridge
SRAM as soon as the name is in, with a CRC-32 of its own: Wolfenstein's in
the last 512 bytes, Spear of Destiny's in the 512 below them; the saves
never reach that far. Wolfenstein's block ends with the rest of
the config file's settings - the view size and the three sound settings -
and the port's Control and Extras settings, and just before those, in a
small block of its own, Customize controls' inputs - all written whenever
Change View, the Sound menu, Control or Extras changes one, whichever game
is running. A cartridge that has never held a game's table shows id's
default one. The shareware build prints the floor alone in the
Level column; the full game prints "E1/L" and the like before it. Spear of
Destiny's table is a picture of its own, in the menu font, with the Spear in
the Level column for a game won.

### The menus

`menu.c` is `US_ControlPanel()` and the parts of `WL_MENU.C` behind it.
Layouts, colors, strings, sounds and the menu song (`WONDERIN`) are the
DOS ones. It has the flickering gun cursor with its half-step between
neighboring items, the `Message()` and `Confirm()` boxes, and dark-red fades
between screens. `HandleMenu()` and the rest were blocking loops; they are
states here, advanced a frame at a time.

- **New Game** asks for the episode, then the difficulty, with BJ's face for
  each. With the shareware, episodes 2-6 are listed in green and answer with
  the "Please select Read This!" box; the full game has all six. Spear of
  Destiny has no episodes, and goes straight to the difficulty. During a
  game it first asks whether to abandon the current one.
- **Sound** has the three DOS groups - sound effects, digitized sound and
  music - each with two choices, None or Nintendo 64 Audio. DOS listed the PC's
  devices there (PC Speaker, AdLib/Sound Blaster, Disney Sound Source); the
  N64 has one sound system, the RCP's audio mixed on the RSP, so the device
  rows are replaced by it and the first two boxes are a row shorter. With
  digitized sound off, the recorded sounds fall back to their AdLib versions
  as in DOS; with effects off, only the recordings play.
- **Read This!** shows the shareware's help article, its 41 pages, with the
  same text-page code as the ending. Its keyboard instructions are the
  original text. It is the shareware's alone, as in DOS: the full game as
  GT Interactive released it (`GOODTIMES` in id's `VERSION.H`) and Spear of
  Destiny build their menus without it, and the items after it move up a
  place in a box a row shorter. (The full game's data still has a help
  article, from the earlier releases, but nothing shows it.)
- **End Game** and **Back to Game** replace View Scores and Back to Demo
  during a game. End Game plays out as a death with no lives left, as
  `CP_EndGame()` arranged, and then goes to the high scores and the title.
  With no attacker to face, the player does not turn; the dissolve starts at
  once. (`CP_EndGame()` never set `killerobj`, so the DOS `Died()` swung
  toward a stale pointer - a former killer's slot, or with none, whatever lay
  at the start of the data segment - which gave a turn in an arbitrary
  direction.)
- **View Scores** shows `DrawHighScores()` over the `ROSTER` song.
- **Back to Demo** (outside a game) returns to the title and the attract loop.
- **Load Game** and **Save Game** are the DOS screens: ten slots, the gun,
  the overwrite question, the name typed into the slot and the turning disk.
  Save Game is only active in a game, as in DOS. See Saved games below.
- **Change View** sizes the 3D view. See Change View below.
- **Control** is the DOS screen - its title, window and lights. DOS's rows
  enabled the mouse, the joystick, the second joystick port and the Gravis
  GamePad and set the mouse's sensitivity, none of which means anything
  with one N64 pad. Instead there are:
  - **Always Run** has a light, as the DOS enable rows did, and A turns it
    on or off. When it is on, the player runs all the time and the Run
    button (B) walks instead.
  - **Stick Sensitivity** opens a screen like DOS's Mouse Sensitivity: the
    same slider with ten notches between Slow and Fast. Left or up slows it,
    right or down speeds it up (held, a notch every 10 tics), A keeps the
    setting and B puts the old one back. It scales everything the stick
    does, as DOS's scaled both of the mouse's axes (`PollControls()`), from
    half to 1.4 times the D-pad's speed; the middle notch, the default, is
    the D-pad's speed. Turning and strafing (with the Strafe button, R,
    held, or through Str. L and Str. R) can go past the D-pad's speed at
    the faster settings; walking forward and back stops at it, so a faster
    setting only reaches full speed with less lean. The D-pad itself is
    never scaled, as DOS never scaled its keys.
  - **Customize controls** is the DOS screen with one section, **Nintendo
    64 Controller**, where DOS had the mouse, the joystick and the keyboard.
    It has three rows, each an action's label over the input it has:

    | Row | Actions | Defaults |
    | --- | --- | --- |
    | buttons | Run, Open, Fire, Strafe, Map | B, A, Z, R, L |
    | C buttons | Str. L, Str. R, Wpn +, Wpn - (strafe left and right, next and previous weapon) | C-Left, C-Right, C-Up, C-Down |
    | D-pad | Left, Right, Frwd, Bkwd | Left, Right, Up, Down |

    The first is the DOS joystick row with **Map** added, since the DOS
    game had no map; the last is the DOS keyboard's move row, with its
    labels. Like DOS's keys, any of the 13 inputs can take any of the 13
    actions, whatever row it is in - Fire on C-Down, say, or Frwd on A. Start
    is always the menu. The D-pad's directions are named without a "D-", as
    they are the stick's too.

    **The stick shares the D-pad's inputs**: each of its four directions is
    the D-pad direction's input, not one of its own, so whatever Up does,
    pushing the D-pad or the stick up does it. Where that is a move - Left, Right,
    Frwd, Bkwd, Str. L or Str. R - the stick moves in proportion to how far
    it leans, as it always did (a dead zone of 8, full at 72); anything
    else counts as pressed once the stick is past 40, the menus' halfway
    mark. So with the defaults the stick turns and walks, and with Fire
    moved to Up, pushing the stick up fires. When choosing an input,
    pushing the stick picks the D-pad direction.

    The gun picks a row, as DOS's picked a device. A boxes the row's first
    action; left and right move the box round the row, and A on an action
    flashes a "?" in it, as DOS did, until an input is pressed for it -
    any button but Start, the D-pad or the stick. The action that had the
    pressed input gets this one's old input in exchange. DOS left that
    action with none, which was fine with a keyboard, but with only a pad it
    could leave no way to open a door, or to walk. Pressing the input the
    action has already leaves it as it was, as Esc did; B leaves the row.
    While a row is being edited, the strip at the foot of the screen has its
    arrows pointing sideways, as the box moves (see below).

    DOS's columns were 60 pixels apart. Here each row's columns are spread
    evenly across the window by their widths. In the four-action rows a
    column is wide enough for any name in the menu font - C-Right is the
    widest, at 63 pixels - which puts them 69 apart. Five columns that wide
    do not fit the first row, so its columns are narrower. Every name is in
    the menu font - with the default controls, all of them - except in one
    case: C-Left, C-Right or C-Down given to Run, Open, Fire, Strafe or Map
    is too wide for its column, so that one name is written in the small
    font instead.

    The menus keep A and B and the D-pad, as DOS menus kept Enter, Esc and
    the arrows, and the screens that take Z (the stats screen, the victory
    screen) take the Fire button. The map still opens on its button's
    release, and not when that release ends an L + R pause. L + R always
    pauses; with L or R given another action, that action also happens as
    L + R is pressed to pause.

    **Reset controls**, in a window under the rows, where the gun reaches
    it the same way, asks in a DOS-style yes/no box (A = yes, B = no) and
    then puts all 13 back as the table above has them. Always Run and Stick
    Sensitivity, on the Control screen, are left as they are.

    The inputs are saved in a 16-byte block of their own, just before the
    other settings (see High scores); a cartridge without it keeps the
    defaults.

  All three are saved to the cartridge at once, with the other settings
  (see High scores).
- **Extras**, under Control, replaces Quit, since a console has nothing to
  quit to. It is not a DOS screen, but it is laid out like Sound: rows under
  section titles. The switches have a light that is lit when they are on,
  and A turns one on or off; the other cheats have none, and A does them
  once.
  - *Debug:* **Show FPS** is the frame-rate readout in the corner; **Show
    Profiler** is the profiler's overlay (see Profiling).
  - *Cheats:* the DOS game's own, from `CheckKeys()` and the Tab debug keys
    of `DebugKeys()`, which needed `-goobers` on the command line:
    - **God Mode** (Tab-G), a switch: a hit still flashes red but takes no
      health. It is ignored during the attract demos, which must play as
      recorded.
    - **No Clip**, a switch too: walk through walls, as Tab-N did in Spear
      of Destiny (Wolfenstein itself never had it). A blocked move goes
      ahead anyway, but not onto the map's outer two tiles, as in
      `ClipMove()`. Inside a wall the player stays in the room last walked
      in (DOS worked out a nonsense one there), so doors and enemies there
      go on hearing them. Ignored in the demos, as God Mode is.
    - **Health, Ammo & Keys** is M-L-I, the code anyone could type, named
      after its own message: all health, 99 rounds, both
      keys and the gatling gun - and, as in DOS, the score back to 0 and
      42 000 tics (ten minutes) on the floor's time, with DOS's message
      about the high score.
    - **Free Items** (Tab-I): 100 000 points (and the extra lives they
      bring), all health, the next weapon up and 50 rounds.
    - **Warp to Floor** (Tab-W): asks which floor of the episode, 1-10
      (Spear of Destiny's 1-21), starting at the one you are on - left and
      right (or up and down) choose, A goes, B cancels - then starts it as
      `ex_warped` did: the score back to what it was when the floor began,
      and "Get Psyched!". Keys go too, as on any new floor here.
    - **End Floor** (Tab-E): ends the floor as the elevator would, keys
      dropped and the stats screen, then on to the next floor. An episode's
      floor 9 is the exception: it ends in the victory, not an elevator, and
      `LevelCompleted()` gives any floor past 8 the secret floor's
      "completed" screen and bonus, which is what Tab-E showed there in DOS.
      So on floor 9 End Floor goes to the victory screen, as BJ's run
      would, then the end text and the high scores. The same goes for Spear
      of Destiny's last floor, where DOS went on to a floor that is not
      there. (The averages there come from the stats screens, so floors
      skipped by warping count as 0%, as in DOS.)

    The four one-offs are only for a game in progress, and shown inactive
    otherwise, as Save Game is. The other Tab keys are debugging tools.

  The four switches start off, and each change is saved to the cartridge
  at once, with the Sound and Change View settings (see High scores), so
  they survive power-off - God Mode and No Clip included.
  The section titles are drawn over the MUSIC title's bar in the same blocky
  letters (`draw_title()` in `menu.c`): D, E, U, G, C, T and S are copied
  from the Sound titles, and B, H and A drawn to match, as no picture in the
  data has them.

The CONTROLS lump in the shipped `VGAGRAPH.WL1` is one chunk later than
`GFXV_WL1.H` says. The README lump before it has an extra picture, an
advert for Spear of Destiny.

Spear of Destiny's menus are its own, as `WL_MENU.H` has them under
`#ifdef SPEAR`: blues where Wolfenstein's are reds, a picture behind every
screen instead of a flat color, fades through dark blue, the skill question
as a picture, and no "Read This!".

The strip at the foot of the menus, `C_MOUSELBACKPIC`, showed the PC's keys:
the arrows for Move, an Enter sign for Select and ESC for Back. The
extractor (`pad_footer()`) swaps the Enter sign and ESC for A and B - letters
taken from the picture's own BACK, in the shade it signs keys in - and
spaces the words again, so it reads Move, A Select, B Back. The arrows
stand for the D-pad and the stick as they are. For choosing along a line -
the Stick Sensitivity slider, a row being edited in Customize controls and
Warp to Floor's number - there is a second copy (`sideways_footer()`) whose
up-and-down arrows are redrawn pointing left and right, in the same shades.
And for typing a name, a third (`typing_footer()`), 144 pixels wide to fit
Start too: up and down LETTER, A ADD, B ERASE, START DONE. Its frame is the
original's stretched, and its letters the original's own but for R, D and
N, which it never had and are drawn to match.

None of those shades are written down: `footer_shades()` reads them off the
strip as it shipped, row by row, because the two games do not agree.
Wolfenstein signs its keys in pink and writes its words in red; Spear of
Destiny signs in white and writes in white fading to grey, and outlines its
arrows. So Spear's three strips come out in Spear's white, as its own does.
Its backdrop is marbled rather than flat, too, so where the typing strip is
made wider the rows above and below the frame are slid along instead of
having a column repeated through them, which would have drawn as a smear.

### The map

L during play (the Map input, unless customized) shows a map of the floor,
as much of it as the player has seen; the DOS game had none. As with Doom's
automap, the game does not stop for it: the player can walk, fight and be
shot with it up, every button does what it does in play, and L hides it
again. Start still opens the menu, and
anything that ends play - a death, the elevator, the victory - takes the map
down with it.

Under the map every frame is played and drawn exactly as usual, the view
included, and the map is then drawn over it. That matters here because
several things happen as the view is drawn, as they did in DOS: items are
picked up as the renderer passes over them, the gun takes its aim from it,
and it is what marks the map. So nothing plays differently with the map up,
and the map keeps filling in while it is shown. It costs about what a normal
frame costs, plus the map.

What counts as seen is what has been on screen. Each frame the ray caster
marks every tile a ray reaches, the wall it stops at included - the same
marks that decide which sprites are drawn - and `automap.c` keeps them for
the floor. So a room behind a shut door stays dark until the door is open
and the player looks through, and a secret push wall is drawn as the wall it
seems to be until it moves. A new floor starts with nothing seen, and the
marks are saved with the game.

The map is drawn over the whole view area, whatever the view size, with the
status bar left below. It is framed round what has been seen and the player,
with four tiles to spare, at up to six pixels a tile: early on a room or two
fills the screen, and it widens as more is found. Walls are drawn in the
average color of their own texture, doors as a bar across their tile (gold
and silver for the locked ones), explored floor dark gray, and the player as
a red dot with a yellow line the way they face. The floor number goes in the
bottom-left margin when there is room, and the FPS readout, when it is on,
stays in its top-left corner.

### Saved games

The saves live on the cartridge, in 256 Kbit (32 KB) of battery-backed SRAM,
the kind Ocarina of Time used. The ROM header asks for it, so emulators and
flash carts provide it. `n64save.c` sets the PI bus timing SRAM needs, reads
and writes the whole 32 KB with DMA, and reads each write back to check it.
On a cartridge with no SRAM that check fails, and the game says there is no
room instead of pretending to save.

`save.c` follows `SaveTheGame()` and `LoadTheGame()`. DOS wrote a memory
dump of about 40 KB: gamestate, the level ratios, the maps, every actor, the
statics, doors and push wall. Loading ran `SetupGameLevel()`, read it all
back over the fresh floor and went on through "Get Psyched!". The port does
the same.

Because loading rebuilds the floor first, a save only needs what has changed
since the floor loaded:

- the doors that are not shut, and the items taken or dropped;
- the walls pushed, as changes to the maps;
- each actor, stored as the XOR of its record with how it started, so what
  did not change is zeros;
- the player, gamestate, the level ratios and the random index;
- the map's record of what has been seen, a bit a tile (see The map). It
  comes last, and a save made before the map loads with none of it seen.

The whole save is then packed with a small LZSS compressor. Actors start on
a random tic of their first state, so a save also keeps the random index the
floor loaded with, and loading rebuilds the same floor. Actor states are
stored by number, not by pointer.

A save is 0.3-2.6 KB in play. A floor cleared of every enemy and item comes
to under 2 KB, so ten saves use under two thirds of the SRAM. If the ten
ever stop fitting, Save Game says there is not enough space, as DOS did for
a full disk. Each slot has a CRC-32, and a damaged save shows as empty.

Actors are saved by their slot in the list (see Enemies), with the free
slots in the order they will be given out, so a floor with rockets in the
air comes back as it was. A save names the game it is from: a Spear of
Destiny build shows Wolfenstein's saves as empty slots, and the other way
round - they stay on the cartridge unless saved over - while the shareware's
and the full game's load in either. Saves made before the full game and
Spear of Destiny were added still load.

A save is named in its slot, as `US_LineInput()` took the name in DOS: up to
31 characters or 121 pixels, typed with the same buttons as a high score
name (see High scores). As in DOS, an empty slot starts with a blank name,
and overwriting a save starts from its old name. B on an empty name is Esc:
nothing is saved, and the gun is back on the slots.

### Change View

`CP_ChangeView()`: the screen shows the play border at the size being
chosen, over the view's area, with the instructions on gray where the status
bar goes. Up or right grows the view and down or left shrinks it, on the
D-pad or the stick, with the wall-bump sound. Any of the four held keeps
stepping, one size every 10 tics (a seventh of a second), as `TicDelay(10)`
allowed. A (or Start) accepts, shows "Thinking..." if the size changed, and
fades back to the main menu; B keeps the old size.

A DOS view was 16 pixels wide per size step and 8 VGA rows tall, in the 160
rows above the status bar; `SetViewSize()` then worked everything out from
the width. This view area is 320x200, so a view here is 16 * size wide and
10 * size tall, the port's own shape, and size 20 - the default - fills it.
The menu goes from 4 to 20 (DOS stopped at 19, one step short of its full
view). From the width, as `CalcProjection()` did, come the wall and sprite
scale, the center column, the hands (`viewheight + 1` VGA pixels) and the
aim: `centerx` and `shootdelta`, so a smaller view aims with fewer pixels of
slop, as in DOS. The demos keep their own 304-pixel aim at any size.

Round the view, `DrawPlayBorder()`'s gray (color 127) with its bevel is drawn
into the column buffer only when it needs it: after a size change, or when
something (the paused sign, a menu, the profiler) has drawn over it. The
death dissolve covers the view and not the border, as `FizzleFade()` did.
The frame-rate counter stays in the screen's top-left corner, over the border
(see "The status bar").

A smaller view costs less, as it did on a 386: under ares, floor 1 reaches
the 60 fps cap at size 15, against about 47 at size 20.

### The sign-on screen

Before anything else, `InitGame()` shows the sign-on screen: `SignonScreen()`
puts up a 320x200 picture, `IntroScreen()` lights the "Available Memory" bars
and the hardware boxes, and `FinishSignon()` prints "Press a key", waits, and
prints "Working..." while the game finishes loading. The title song starts
and `PG13()` fades the screen out. Until "Press a key", the bottom line was
the picture's own "One moment...", up for as long as the PC took to start.
The N64 has nothing to load there, so the bare picture, "One moment..."
and all, is shown for half a second, with buttons ignored as `IN_Ack()` was
not yet listening; then the memory bars and hardware lights come on, and the
prompt says "Press a button". Spear of Destiny's `FinishSignon()` asks for
no key: its lights stay up for three seconds, and the game goes on. It skips
the half second too - with no prompt to wait for, it would only be time on
top of the three it asks for - so its lights are on from the first frame.

`IntroScreen()` puts the bars and boxes at fixed coordinates, one set for
both games: Spear of Destiny's picture has its slots in the same places as
Wolfenstein's, and only the bars' colors differ.

The picture is not in `VGAGRAPH`: it was linked into the EXE, as plain
rows (`VL_MungePic()` only weaves it into VGA planes at run time).
`extract_assets.py` unpacks the game's executable and takes it from there,
so it is that release's own screen - the shareware's with the Apogee logo,
and the full game's and Spear of Destiny's their own. (id's source release
has one too, in `WOLFSRC\OBJ\SIGNON.OBJ`, but it is the GT Interactive
edition's, with that logo instead, so it is not used.) It is optional: if
the executable is missing or the picture is not found in it, the game
starts as `DemoLoop()` does, with the title song and `PG13()` fading in
from black.

The readout is the N64's (`platform_machine()` in `main.c`): the
console's 4 MB fills the main memory column, and the EMS column too: EMS
has no N64 counterpart, and the console's own memory stands in for it, as
the game never runs short. An Expansion Pak's 4 MB shows as XMS. A
controller lights Joystick and the N64 mouse Mouse. Both AdLib and Sound
Blaster are lit: the effects run on an emulated AdLib chip and the recordings
on the Sound Blaster's voice. That departs from `IntroScreen()`, which lit
AdLib only without a Sound Blaster, whose own FM chip stood in for one. It is
redrawn every frame, so a controller plugged in late still lights its box.

### The title and the demos

`DemoLoop()` runs after the sign-on screen and `PG13()`: the title for 15 seconds, the
credits and the high scores for 10 each, then the next of the four demos, and
round again. A button on any of them fades out to the main menu. The title
plays `NAZI_NOR` (Spear of Destiny's, `XTOWER2`, over its title drawn in two
halves with a palette of its own), and a demo plays its floor's song.

The demos are four chunks of `VGAGRAPH`, recorded on "I am Death
incarnate!": the shareware's on floors 1, 3, 5 and 7, the full game's on
floors 8 and 2 of episode four, 4 of episode five and 7 of episode six
(the officers and mutants among them), Spear of Destiny's on its floors 2,
4, 6 and 13. Each is a list
of three-byte samples, one per four tics, holding the buttons and the mouse
movement. `PlayDemo()` feeds them to the same `PlayLoop()` a game uses, so a
demo only plays out as recorded if every calculation matches the DOS game.
The random table has to be in step, and so do movement, collision, every
actor's decisions and even what the renderer counts as visible. The smallest
difference - one degree of turning, one rounding - and BJ walks into a wall
and the rest of the demo goes wrong.

Normal play keeps the smoother N64 versions of a few of those calculations,
so a demo switches the game into `dos_exact` mode:

- **Frames** are always four tics, and the random index resets when the
  floor loads (`US_InitRndT()`).
- **Turning and thrust** use whole degrees and the DOS 16.16 sine table with
  `FixedByFrac()`'s sign-magnitude multiply.
- **The view** is `CalcProjection()`'s 304-pixel one, so the aim test in
  `T_Shoot()` and the player's `GunAttack()` see the same screen positions.
- **Visibility** comes from a port of `WallRefresh()`'s ray walk over 304
  columns. Enemies wake when their tile is marked, and that walk marks the
  tiles it passes, not the one it hits.
- **Push walls** move a whole frame's tics at once, and the gatling grin
  lasts 38 frames.
- **No moonwalking fix**: the DOS random draws have to land on the same
  frames.

`SPR_DEMO`, the "DEMO" sprite, is drawn over the view while one plays. A demo
ends when its samples run out, BJ dies (the shareware's and Spear of
Destiny's all end that way, and two of the full game's) or an elevator is
used, and the loop goes on to the title.

Some DOS behaviors the demos exposed are now used in normal play too. The
camera sits `FOCALLENGTH` behind the player. Items are grabbed before the
too-close check. `T_Attack()` moves before it attacks. Use repeats on push
walls while Open (A) is held. A door will not close until it is fully open.

The demos were checked frame by frame against Wolf4SDL, a source port of the
DOS code. It was built as each of the three games with a trace of the
player's position, angle, health, ammo, kills, random index and a checksum
of every door and actor, and compared with the same trace from the
simulator. All twelve demos match on every frame. (Spear of Destiny's second
needed its wide-eyed face: a big hit resets the glance timer, which shifts
when the next random number is drawn - see "The status bar".)

### Profiling

`n64prof.c` times the stretches of a frame on the N64 itself. The game
marks where each stretch starts with `PROF()` (logic, background, rays, wall
columns, collect, sprites, weapon, status bar, present, wait, audio, and the
overlay itself), and each
mark reads the CPU's COUNT register. In the game, turn on Extras' Show
Profiler to see the per-frame averages over the view.

`make BENCH=1` builds a benchmark ROM instead of the game (`ROM=` names it,
as `build.ps1 -Name` does). It starts with two
test cards. The same one-pixel checkerboard is shown once from rows and once
from columns, and every frame the RDP draws is read back and compared with
the picture, pixel for pixel. Then it turns on the spot at the start of
floors 1, 3, 5 and 9 (hardest setting, enemies awake), stands at point blank
to a guard, faces a wall, and turns on floor 1 again with the view at sizes
15 and 10. Every scene runs twice, probed and unprobed, and it ends on a
table of the results and the test cards' pixel counts. (The tables below were
measured with the earlier scene list, floors 1, 2, 3, 5, 7 and 9 at full
size; the latest run gave 44-49 fps for those it still has, and 59.8, the
cap, for floor 1 at sizes 15 and 10.) Under ares v148:

| fps, unprobed | floor 1 | floor 2 | floor 3 | floor 5 | floor 7 | floor 9 | guard | wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| first profile | 32.1 | 30.8 | 33.2 | 30.6 | 33.7 | 32.9 | 33.0 | 35.9 |
| status bar, ceiling and floor, palette pointer | 39.1 | 37.8 | 39.2 | 37.3 | 39.1 | 39.7 | 36.9 | 40.4 |
| **and the view drawn in columns** | **44.8** | **42.9** | **44.9** | **41.7** | **43.9** | **45.0** | **42.7** | **46.5** |
| worst frame (ms), first profile | 32.6 | 37.6 | 37.3 | 37.8 | 37.7 | 36.8 | 30.7 | 28.3 |
| worst frame (ms), now | 23.2 | 25.5 | 25.5 | 26.1 | 25.5 | 24.8 | 23.9 | 22.1 |

Where a frame goes now, in ms:

| | floor 1 | floor 2 | floor 3 | floor 5 | floor 7 | floor 9 | guard | wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| columns (with ceiling and floor) | 16.7 | 17.2 | 16.4 | 16.9 | 16.0 | 16.7 | 15.2 | 15.2 |
| present | 1.9 | 1.9 | 1.9 | 1.9 | 1.9 | 1.9 | 1.9 | 1.9 |
| rays | 1.8 | 1.8 | 1.9 | 1.8 | 2.1 | 1.8 | 2.0 | 2.0 |
| sprites | 0.1 | 0.2 | 0.0 | 0.9 | 1.0 | 0.3 | 2.8 | 0.8 |
| audio | 0.6 | 0.6 | 0.6 | 0.6 | 0.7 | 0.6 | 0.6 | 0.6 |
| weapon | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 | 0.6 |
| collect | 0.3 | 0.6 | 0.5 | 0.7 | 0.4 | 0.0 | 0.1 | 0.3 |
| logic | 0.2 | 0.2 | 0.2 | 0.3 | 0.2 | 0.0 | 0.0 | 0.0 |
| status bar | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

What changed, each leaving every frame the simulator captures byte for byte
the same:

- **The status bar** is composed only when something on it changes, the
  palette included (4.4 ms saved).
- **The ceiling and floor** are drawn with each wall column, above and below
  the wall, instead of filling the whole view first and drawing walls over
  most of it. Only about 1.5-2 ms of the 7.5 ms fill was saved: written down
  a row-major buffer, a column cost more per pixel than the fill's 64-bit
  row writes.
- **The column loops** keep the palette pointer in a local instead of loading
  the global again for every pixel.
- **The 3D view is drawn in columns.** Walls, scenery and the hands are all
  drawn a column at a time, and in `viewbuf` each pixel of a column is 656
  bytes past the last, on a cache line of its own. `render_view()` draws
  into `colbuf` instead, 320 columns of 200 pixels each, so a column is one
  run of memory. The RDP's `TEXTURE_RECTANGLE_FLIP` shows it on its side: it
  steps the texture's S coordinate down the screen and T across, which is
  exactly a transpose. That command does not work in copy mode, so the view
  goes up in standard mode, point-sampled, in strips of 8 columns: one load
  into texture memory is at most 4096 bytes, and a 200-texel row takes 512.
  A turned `rdpq_tex_blit()` (two triangles) was tried first. The readback
  found 44% of its pixels a texel off, while the flipped rectangle has
  matched on every frame. `view_columns` says which buffer holds the
  picture. The frame rate, the paused sign and the profiler draw into either
  through `view_pixel()`; fizzles and fades turn the view back into rows with
  `view_to_rows()` first, and the menus, text and stats screens stay in rows.

ares does not model the VR4300's data cache exactly, and the renderer's
memory writes are where a real console differs most, so treat these as
proportions rather than console timings.

### Checking it without hardware

`.\build.ps1 -Sim` (or `./build.sh --sim`) compiles the same libdragon-free `src\` files with the container's host
gcc and plays a fixed script: the start of each floor, a look at scenery on the
first three, then walking onto pickups and printing what each one gave, a
check that every gold door stays shut without its key and opens with it,
pushing every secret wall on every floor and checking where each came to rest,
enemies, sound, both kinds of elevator and the end-of-floor tally's points,
BJ's face (health bands, the gatling grin and what ends it), walking into
the first door on floor 1 and opening it, how controls.c reads the pad (the
stick in proportion and as a press, following the D-pad's inputs when they
move), and finally the game flow. That
last part starts at the sign-on screen (its half second unlit and deaf to
buttons, then the lights and the prompt) and drives the menus through New Game
(including the unavailable episode box and backing out of the difficulty), a
new game's fades, a death and respawn, pause (also from the map), the map,
the Sound menu, Read This!, Back to Game, Extras (the switches - No Clip
walked through a wall and not without it - and in a
game the one-off cheats: Health, Ammo & Keys (M-L-I), Free Items, a warp and End Floor through the
stats screen), Control (Always Run, the
sensitivity slider kept and given up, inputs given to other actions in
Customize controls, across rows too, and Reset controls answered both ways), Change View, Save Game
(a name typed, the overwrite question both ways, and a save given up) and
Load Game, End Game (a score too low
for the table, and its wait), and the whole victory sequence through both
text pages, then a name typed into the high score table, which must still be
there once the cartridge is read again. The attract loop runs
through once with all four demos, and each demo's end is checked against the
DOS result. Before the game flow, every floor is played roughly on the
hardest setting, with doors, push walls, pickups and fighting. Each floor is
saved, played on for 400 tics, loaded, and played through the same 400 tics
again. The state after loading and all 400 tics must match exactly. A
damaged slot is checked too. It writes a PPM
per captured frame plus a contact sheet:

    build\sim\sheet.png

That script is the shareware's, and knows its floors. With the full game or
Spear of Destiny extracted, the sim runs checks of its own instead: the
sign-on, PG-13, title, credits, high scores and menus as the attract loop
and New Game show them; every floor's start; every class of enemy on every
floor, and a look at each; every boss fought to the end through the game's
own flow - the keys they drop, the death cam and its replay, the victory
screen, the end text or Spear of Destiny's end screens and the high scores;
Spear of Destiny's Spear, taken and carried to the last floor; rockets,
syringes, flames and sparks fired for half a minute, with a save made while
one is in the air coming back exactly; a death by Dr. Schabbs's syringe and
the mutant face, and the plain one for any other killer; which floors each secret elevator
leads to and back from; the stats screen part way through and on a secret
floor; the same rough play, save and reload on every floor; a save of the
other game shown as empty and kept; and the four demos against Wolf4SDL's
ends.

This is where the renderer and the door state machine were actually verified;
the ROM itself was checked by running it under ares.
