# The ROM make builds. build.sh builds it inside build/ -
# build/wolf3d.z64 for Wolfenstein 3D, build/spear.z64 for Spear of Destiny -
# and copies it out under the name people see: "Wolfenstein 3D.z64" or "Spear
# of Destiny.z64", or whatever --name asked for. make cannot have a space in
# the name of a file it builds, and the copy can. (The ROM records the
# program's file name, wolf3d.elf.stripped say, so the internal name is kept
# per game rather than taken from --name: the ROM is then the same whatever
# it is called.) A cartridge is given its SRAM by the ROM's name, so the two
# games having names of their own is what keeps their saves and high scores
# apart (src/save.c).
ROM ?= wolf3d.z64
ROM_NAME = $(basename $(ROM))

all: $(ROM)
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

# n64.mk compiles $(BUILD_DIR)/<path>.o from <path>.c, so object paths
# mirror source paths.
OBJS = $(BUILD_DIR)/src/main.o \
       $(BUILD_DIR)/src/n64audio.o \
       $(BUILD_DIR)/src/adlib.o \
       $(BUILD_DIR)/src/pcspeaker.o \
       $(BUILD_DIR)/src/automap.o \
       $(BUILD_DIR)/src/assets.o \
       $(BUILD_DIR)/src/level.o \
       $(BUILD_DIR)/src/doors.o \
       $(BUILD_DIR)/src/player.o \
       $(BUILD_DIR)/src/render.o \
       $(BUILD_DIR)/src/statics.o \
       $(BUILD_DIR)/src/status.o \
       $(BUILD_DIR)/src/sound.o \
       $(BUILD_DIR)/src/actors.o \
       $(BUILD_DIR)/src/intermission.o \
       $(BUILD_DIR)/src/text.o \
       $(BUILD_DIR)/src/game.o \
       $(BUILD_DIR)/src/menu.o \
       $(BUILD_DIR)/src/controls.o \
       $(BUILD_DIR)/src/save.o \
       $(BUILD_DIR)/src/n64save.o \
       $(BUILD_DIR)/src/n64prof.o

# The ROM's header names the game the assets are from: the blob's version,
# the big-endian word at byte 10 of assets/wolf3d.dat, is 2 for Spear of
# Destiny (tools/extract_assets.py). Read here rather than passed in, so no
# script has to hand make a quoted title with a space in it.
ifeq ($(shell od -An -tu1 -j11 -N1 assets/wolf3d.dat 2>/dev/null | tr -d ' '),2)
$(ROM): N64_ROM_TITLE = "Spear of Destiny"
else
$(ROM): N64_ROM_TITLE = "Wolfenstein 3D"
endif
# The saved games live in cartridge SRAM; see src/n64save.c.
$(ROM): N64_ROM_SAVETYPE = sram256k

# make BENCH=1 builds the profiling benchmark instead of the game (n64prof.c).
ifeq ($(BENCH),1)
CFLAGS += -DWOLF_BENCH
endif

# BENCH changes what main.c and n64prof.c compile to, so they rebuild when it
# changes - a stamp, as the audio flags have below - or switching from the
# benchmark back to the game would leave benchmark code in the game.
BENCH_KIND := bench-$(if $(filter 1,$(BENCH)),1,0)
$(BUILD_DIR)/bench-flags.stamp: $(if $(filter $(BENCH_KIND),$(shell cat $(BUILD_DIR)/bench-flags.stamp 2>/dev/null)),,FORCE)
	@mkdir -p $(dir $@)
	echo $(BENCH_KIND) > $@
$(BUILD_DIR)/src/main.o $(BUILD_DIR)/src/n64prof.o: $(BUILD_DIR)/bench-flags.stamp

# n64.mk builds %.z64 from $(BUILD_DIR)/%.elf, so the ELF is named after
# the ROM as well.
$(BUILD_DIR)/$(ROM_NAME).elf: $(OBJS)

# ---------------------------------------------------------------------------
# Audio. tools/extract_assets.py leaves the raw inputs in assets/audio; host
# programs (the container's own gcc, not the N64 one) turn the OPL parts
# into something the N64 can play, and audioconv64 packs it all into the
# ROM's filesystem. The digitized sounds are converted as they are.
#
# Music is sequenced by default: tools/audio/imf2xm.c turns each song into
# an XM module of recorded instruments, played by xm64player - about 1.6 MB
# for all of it. MUSIC=wav renders the songs to VADPCM wav64 instead,
# exact but about 8 MB.
#
# AdLib effects are synthesized on the N64 by default (src/adlib.c), from
# their original AUDIOT chunks - about 11 KB. SFX=wav renders them to
# uncompressed wav64 instead, about 5 MB. src/n64audio.c follows both
# switches.
# ---------------------------------------------------------------------------

MUSIC ?= xm
SFX   ?= synth
AUDIO_IN := $(wildcard assets/audio/*)
N64_MKDFS_ROOT = $(BUILD_DIR)/filesystem

ifeq ($(MUSIC),wav)
CFLAGS += -DWOLF_MUSIC_WAV
endif
ifeq ($(SFX),wav)
CFLAGS += -DWOLF_SFX_WAV
endif

$(BUILD_DIR)/render_audio: tools/audio/render_audio.c tools/audio/nuked-opl3/opl3.c
	@mkdir -p $(dir $@)
	@echo "    [HOSTCC] $@"
	gcc -O2 -Wall -o $@ $^ -lm

$(BUILD_DIR)/imf2xm: tools/audio/imf2xm.c tools/audio/nuked-opl3/opl3.c
	@mkdir -p $(dir $@)
	@echo "    [HOSTCC] $@"
	gcc -O2 -Wall -o $@ $^ -lm

# The stamp remembers which kinds it built, so switching rebuilds.
AUDIO_KIND := $(MUSIC)-$(SFX)

# The flags above change what n64audio.c compiles to, so it rebuilds when
# they change - a stamp of its own, as the program is built without the
# game's data for the web builder (see program, below).
$(BUILD_DIR)/audio-flags.stamp: $(if $(filter $(AUDIO_KIND),$(shell cat $(BUILD_DIR)/audio-flags.stamp 2>/dev/null)),,FORCE)
	@mkdir -p $(dir $@)
	echo $(AUDIO_KIND) > $@
$(BUILD_DIR)/src/n64audio.o: $(BUILD_DIR)/audio-flags.stamp

$(BUILD_DIR)/audio.stamp: $(BUILD_DIR)/render_audio $(BUILD_DIR)/imf2xm $(AUDIO_IN) \
                          $(if $(filter $(AUDIO_KIND),$(shell cat $(BUILD_DIR)/audio.stamp 2>/dev/null)),,FORCE)
	@echo "    [AUDIO] rendering and converting ($(MUSIC) music, $(SFX) effects)"
	rm -rf $(BUILD_DIR)/wav $(BUILD_DIR)/xm $(N64_MKDFS_ROOT)
	mkdir -p $(BUILD_DIR)/wav/sfx $(N64_MKDFS_ROOT)/music $(N64_MKDFS_ROOT)/sfx
	$(BUILD_DIR)/render_audio assets/audio $(BUILD_DIR)/wav
ifneq ($(SFX),wav)
	rm -f $(BUILD_DIR)/wav/sfx/al*.wav
	cp assets/audio/al*.adl $(N64_MKDFS_ROOT)/sfx/
endif
	cp assets/audio/pc*.pcs $(N64_MKDFS_ROOT)/sfx/	# the PC Speaker's, synthesized as ever
	cp assets/audio/*.wav $(BUILD_DIR)/wav/sfx/
ifeq ($(MUSIC),wav)
	$(N64_AUDIOCONV) --wav-compress 1 --wav-loop true \
		-o $(N64_MKDFS_ROOT)/music $(BUILD_DIR)/wav/music >/dev/null
else
	$(BUILD_DIR)/imf2xm assets/audio $(BUILD_DIR)/xm
	$(N64_AUDIOCONV) -o $(N64_MKDFS_ROOT)/music $(BUILD_DIR)/xm >/dev/null
endif
	$(N64_AUDIOCONV) --wav-compress 0 \
		-o $(N64_MKDFS_ROOT)/sfx $(BUILD_DIR)/wav/sfx >/dev/null
	echo $(AUDIO_KIND) > $@

FORCE:
.PHONY: FORCE

# The game's data goes in the ROM's filesystem, not the program, so that
# the program is the same whatever game it is built for: wolf3d.dat, which
# the game reads into RAM at boot, and wolfbig.dat, the maps and the
# full-screen pictures, which it reads as it needs them (see
# tools/extract_assets.py). After the audio, whose rule empties the
# filesystem directory.
$(N64_MKDFS_ROOT)/%.dat: assets/%.dat $(BUILD_DIR)/audio.stamp
	cp $< $@

$(BUILD_DIR)/wolf3d.dfs: $(BUILD_DIR)/audio.stamp \
                         $(N64_MKDFS_ROOT)/wolf3d.dat $(N64_MKDFS_ROOT)/wolfbig.dat
$(ROM): $(BUILD_DIR)/wolf3d.dfs

# The program alone, which needs none of the game's data: what the web
# builder carries (tools/web), which adds the filesystem in the player's
# browser. These are the steps n64.mk's %.z64 rule takes before it
# puts the ROM together, leaving $(BUILD_DIR)/$(ROM_NAME).elf.stripped,
# compressed, and its symbols, .elf.sym.
# n64.mk sets the N64 compiler for what a %.z64 depends on; this does the
# same for what program depends on.
program: CC=$(N64_CC)
program: CXX=$(N64_CXX)
program: AS=$(N64_AS)
program: LD=$(N64_LD)
program: CFLAGS+=$(N64_CFLAGS)
program: CXXFLAGS+=$(N64_CXXFLAGS)
program: ASFLAGS+=$(N64_ASFLAGS)
program: RSPASFLAGS+=$(N64_RSPASFLAGS)
program: LDFLAGS+=$(N64_LDFLAGS)
program: $(BUILD_DIR)/$(ROM_NAME).elf
	$(N64_SYM) $< $<.sym
	cp $< $<.stripped
	$(N64_STRIP) -s $<.stripped
	$(N64_ELFCOMPRESS) -o $(dir $<) -c $(N64_ROM_ELFCOMPRESS) $<.stripped
.PHONY: program

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d) $(wildcard $(BUILD_DIR)/src/*.d)
