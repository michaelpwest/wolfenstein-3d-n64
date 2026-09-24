#!/usr/bin/env python3
"""Build the N64 asset blobs from Wolfenstein 3D's data files.

Reads your copy of the game - the shareware episode (*.WL1), the full
six-episode game (*.WL6) or Spear of Destiny (*.SOD), whichever is in the
folder - and writes

  assets/wolf3d.dat   a big-endian blob that the ROM links in with .incbin
                      and uses in place: everything a floor needs
  assets/wolfbig.dat  what is only needed now and then - the maps and the
                      full-screen pictures - which goes into the ROM's
                      filesystem and is read when it is wanted, so that the
                      larger games still fit a 4 MB console's memory
  assets/audio/       the raw sounds and music

Two pieces of artwork the game needs are not in the data files: they were
linked into the DOS executable. They are read out of WOLF3D.EXE (or
SPEAR.EXE), next to the data files, once tools/lzexe.py has unpacked it: the
palette (required; id's source release can stand in with OBJ/GAMEPAL.OBJ)
and the sign-on screen (optional; if the executable does not have it, the
ROM starts at the PG-13 screen). The tables the source release supplies -
sound numbers, the digitized sound map, the song lists, the sprite and
graphics numbering and the stereo tables - are carried in
tools/wolf_tables.py, and checked against the source whenever it is there.

The game keeps one numbering of sprites, sounds and pictures whatever data
it is built from - the full game's, with Spear of Destiny's own ones after
it - and each version's data is placed into it by name.

    python tools/extract_assets.py [--data DIR] [--source DIR]
    python tools/extract_assets.py --write-tables --source DIR

--data defaults to gamedata/ in the project, where the game files are meant
to be put. --source (the WOLFSRC folder of id's source release) is only used
when it is given. --write-tables regenerates tools/wolf_tables.py from it.
"""

import hashlib
import os
import struct
import sys

import lzexe

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DATA = os.path.join(PROJECT, "gamedata")          # --data
WOLFSRC = None                                    # --source, optional
TABLES_PY = os.path.join(HERE, "wolf_tables.py")

# Which game the data is, by its files' extension, and the number the blob
# gives it (the game's wolf_version).
VERSIONS = {"WL1": 0, "WL6": 1, "SOD": 2}
EXT = None                                        # set by main()
VT = None                                         # that version's TABLES


def data_path(name):
    """A file in the data folder, whatever the case of its name. DOS named
    them in capitals, but not every copy of the game keeps them that way,
    and Linux tells VSWAP.WL1 from vswap.wl1. The exact name wins; failing
    that, the first file whose name matches but for case."""
    exact = os.path.join(DATA, name)
    if os.path.exists(exact):
        return exact
    try:
        for entry in sorted(os.listdir(DATA)):
            if entry.lower() == name.lower():
                return os.path.join(DATA, entry)
    except OSError:
        pass
    return exact


def fname(base):
    """A data file's path: VSWAP -> DATA/VSWAP.WL6, say."""
    return data_path("%s.%s" % (base, EXT))


def have_source(*parts):
    """A file from id's source release, if --source was given and has it."""
    return WOLFSRC is not None and os.path.exists(os.path.join(WOLFSRC, *parts))

MAPPLANES = 3
MAPSIZE = 64
NEARTAG = 0xA7
FARTAG = 0xA8

# Sprites, sounds and pictures are numbered the same way whatever the data:
# sprites and sounds by the full game's enums (WL_DEF.H, AUDIOWL6.H - the
# shareware's data is laid out by them too, with the registered-only
# sprites left empty), then Spear of Destiny's own ones after them. These
# are filled in from the tables by main().
GAME_SPRITES = None                     # name per internal number
GAME_SOUNDS = None


# --------------------------------------------------------------------------
# GAMEMAPS / MAPHEAD  (Carmackized, then RLEW compressed - see ID_CA.C)
# --------------------------------------------------------------------------

def carmack_expand(src, expanded_bytes):
    """CarmackExpand() from ID_CA.C. Works in words, emits `expanded_bytes`."""
    out = bytearray()
    length = expanded_bytes // 2
    i = 0
    while length > 0:
        ch = src[i] | (src[i + 1] << 8)
        i += 2
        chhigh = ch >> 8
        count = ch & 0xFF
        if chhigh == NEARTAG:
            if count == 0:
                # escaped literal: the tag byte is real data
                ch = (ch & 0xFF00) | src[i]
                i += 1
                out += struct.pack("<H", ch)
                length -= 1
            else:
                offset = src[i]
                i += 1
                copy = len(out) - offset * 2
                length -= count
                for _ in range(count):
                    out += out[copy:copy + 2]
                    copy += 2
        elif chhigh == FARTAG:
            if count == 0:
                ch = (ch & 0xFF00) | src[i]
                i += 1
                out += struct.pack("<H", ch)
                length -= 1
            else:
                offset = src[i] | (src[i + 1] << 8)
                i += 2
                copy = offset * 2
                length -= count
                for _ in range(count):
                    out += out[copy:copy + 2]
                    copy += 2
        else:
            out += struct.pack("<H", ch)
            length -= 1
    return bytes(out[:expanded_bytes])


def rlew_expand(src, expanded_bytes, tag):
    """CA_RLEWexpand() from ID_CA.C."""
    out = bytearray()
    i = 0
    while len(out) < expanded_bytes:
        value = src[i] | (src[i + 1] << 8)
        i += 2
        if value != tag:
            out += struct.pack("<H", value)
        else:
            count = src[i] | (src[i + 1] << 8)
            value = src[i + 2] | (src[i + 3] << 8)
            i += 4
            out += struct.pack("<H", value) * count
    return bytes(out[:expanded_bytes])


def load_maps():
    """Every map, in order: the shareware's ten, the full game's sixty (six
    episodes of ten), Spear of Destiny's 21."""
    with open(fname("MAPHEAD"), "rb") as f:
        head = f.read()
    rlew_tag = struct.unpack_from("<H", head, 0)[0]
    offsets = struct.unpack_from("<100i", head, 2)

    with open(fname("GAMEMAPS"), "rb") as f:
        maps = f.read()

    count = 0
    while count < 100 and offsets[count] > 0:
        count += 1
    if count == 0:
        raise SystemExit("no maps in %s" % fname("GAMEMAPS"))

    levels = []
    for n in range(count):
        pos = offsets[n]
        planestart = struct.unpack_from("<3i", maps, pos)
        planelength = struct.unpack_from("<3H", maps, pos + 12)
        width, height = struct.unpack_from("<2H", maps, pos + 18)
        name = maps[pos + 22:pos + 38].split(b"\0")[0].decode("latin-1")
        if (width, height) != (MAPSIZE, MAPSIZE):
            raise SystemExit("map %d is %dx%d, not 64x64" % (n, width, height))

        planes = []
        for p in range(MAPPLANES):
            comp = maps[planestart[p]:planestart[p] + planelength[p]]
            # Carmack layer: leading word is the RLEW stream's length in bytes
            expanded = struct.unpack_from("<H", comp, 0)[0]
            rlew = carmack_expand(comp[2:], expanded)
            # RLEW layer: its own leading word is the final length in bytes
            plane = rlew_expand(rlew[2:], MAPSIZE * MAPSIZE * 2, rlew_tag)
            planes.append(struct.unpack("<%dH" % (MAPSIZE * MAPSIZE), plane))
        levels.append({"name": name, "planes": planes})
        print("  map %2d  %-16s" % (n, name))
    return levels


# --------------------------------------------------------------------------
# VSWAP - wall pages are 64x64 CI8, stored column major
# --------------------------------------------------------------------------

def load_vswap():
    with open(fname("VSWAP"), "rb") as f:
        data = f.read()
    chunks, sprite_start, sound_start = struct.unpack_from("<3H", data, 0)
    offsets = struct.unpack_from("<%dI" % chunks, data, 6)
    lengths = struct.unpack_from("<%dH" % chunks, data, 6 + chunks * 4)
    print("  %d chunks, %d wall pages, sprites start at %d, sounds at %d"
          % (chunks, sprite_start, sprite_start, sound_start))

    def chunk(i):
        off, ln = offsets[i], lengths[i]
        return data[off:off + ln] if off else b""

    walls = []
    for i in range(sprite_start):
        page = chunk(i)
        walls.append(page if len(page) == 4096 else page.ljust(4096, b"\0"))

    # Every sprite the file has, under the game's number for its name. The
    # shareware file is laid out by the full game's enum and leaves the
    # ones it does not ship empty - mutants, officers, ghosts, the later
    # bosses - and those simply are not there.
    names = VT["SPRITES"]
    numsprites = sound_start - sprite_start
    if numsprites != len(names):
        raise SystemExit("VSWAP has %d sprites, WL_DEF.H's list for this"
                         " version %d" % (numsprites, len(names)))
    sprites = []
    for s in range(numsprites):
        data_s = chunk(sprite_start + s)
        if data_s:
            sprites.append((GAME_SPRITES.index(names[s]), data_s))
    return walls, (len(GAME_SPRITES), sprites), (data, sound_start, chunks, offsets, lengths)


# --------------------------------------------------------------------------
# Compiled sprites
#
# Each chunk is a t_compshape (WL_DEF.H): leftpix, rightpix, then one offset
# per populated column. A column's entry is a run of three-word posts -
# "end of segment pixel*2 (0 terminates line) / top of virtual line with
# segment in proper place / start of segment pixel*2", per the comment above
# SimpleScaleShape() - so the pixel for row y of a post is the byte at
# newstart + y from the start of the chunk.
#
# Decoding to explicit posts keeps the transparency structural: nothing has
# to be colour-keyed, and the renderer skips gaps instead of testing them.
# --------------------------------------------------------------------------

def decode_sprite(chunk):
    """-> (columns, pixels); columns[c] is a list of (starty, pixel_offset)."""
    leftpix, rightpix = struct.unpack_from("<2H", chunk, 0)
    if rightpix < leftpix or rightpix > 63:
        raise SystemExit("sprite has columns %d..%d" % (leftpix, rightpix))
    dataofs = struct.unpack_from("<%dH" % (rightpix - leftpix + 1), chunk, 4)

    pixels = bytearray()
    columns = [[] for _ in range(64)]
    for col in range(leftpix, rightpix + 1):
        off = dataofs[col - leftpix]
        while True:
            endy = struct.unpack_from("<H", chunk, off)[0]
            if endy == 0:
                break
            endy >>= 1
            newstart = struct.unpack_from("<H", chunk, off + 2)[0]
            starty = struct.unpack_from("<H", chunk, off + 4)[0] >> 1
            off += 6
            # newstart is pixel_base - starty biased into a 16-bit word, so
            # it is often "negative"; the DOS scaler added y to it inside a
            # segment and let it wrap. Do the same arithmetic.
            base = (newstart + starty) & 0xFFFF
            run = chunk[base:base + endy - starty]
            if len(run) != endy - starty:
                raise SystemExit("post runs off the end of the chunk")
            columns[col].append((starty, len(pixels), run))
            pixels += run
    return columns, bytes(pixels)


def pack_sprites(sprite_list):
    """Pack the decoded sprites into the blob's sprite section.

    The index has an entry for every sprite number in the game's enum, 0 for
    the ones not included, so the game looks sprites up by SPR_ value.
    Everything but the pixels is 32-bit, in one contiguous run, so that
    tools/sim can byte-swap the whole section header with a single pass.
    """
    count, sprites = sprite_list
    records, pixel_blocks, numbers = [], [], []
    for number, chunk in sprites:
        columns, pixels = decode_sprite(chunk)
        records.append(columns)
        pixel_blocks.append(pixels)
        numbers.append(number)

    # header: count, posts_off, pixels_off, pad, then one offset per number
    head_words = 4 + count
    # each record is pix_base + 64 column words, then its posts
    rec_words = [1 + 64 + sum(len(c) for c in cols) for cols in records]

    rec_off = []
    w = head_words
    for n in rec_words:
        rec_off.append(w * 4)
        w += n
    pixels_off = w * 4

    index = [0] * count
    for number, off in zip(numbers, rec_off):
        index[number] = off
    words = [count, head_words * 4, pixels_off, 0] + index
    pix_base = pixels_off
    for i, cols in enumerate(records):
        post_off = rec_off[i] + (1 + 64) * 4
        words.append(pix_base)
        colwords, posts = [], []
        for col in cols:
            if not col:
                colwords.append(0)
                continue
            # MIPS traps unaligned 32-bit loads and the renderer reads the
            # runs straight out of the blob, so this offset has to stay a
            # multiple of four.
            off = post_off + len(posts) * 4
            assert off % 4 == 0 and off < (1 << 24), off
            colwords.append((len(col) << 24) | off)
            for starty, pixofs, run in col:
                posts.append((starty << 24) | (len(run) << 16) | pixofs)
        words += colwords + posts
        pix_base += len(pixel_blocks[i])

    blob = struct.pack(">%dI" % len(words), *words) + b"".join(pixel_blocks)
    assert len(blob) == pixels_off + sum(len(p) for p in pixel_blocks)
    print("  %d sprites of the game's %d, %d bytes (%d of pixels)"
          % (len(records), count, len(blob), sum(len(p) for p in pixel_blocks)))
    return blob


# --------------------------------------------------------------------------
# VGAGRAPH - the menu and status bar artwork, Huffman coded
#
# VGADICT is 255 huffnodes (ID_CA.C's grhuffman), VGAHEAD a three-byte
# offset per chunk, and each chunk in VGAGRAPH opens with a longword giving
# its expanded size. Chunk 0 is STRUCTPIC: a {width, height} pair for every
# pic, indexed from STARTPICS.
#
# The full game's and Spear of Destiny's files are exactly as GFXV_WL6.H
# and GFXV_SOD.H say, so their pics are looked up by name. The shareware's
# is not: GFXV_WL1.H declares 136 pics, VGAGRAPH.WL1 holds 144, and every
# chunk from the status bar on sits eight later than the header says. So
# there the status bar is found by its size - the only 320x40 pic - and
# everything else by its offset from it, with the expected dimensions
# asserted.
# --------------------------------------------------------------------------

STARTPICS = 3

# offset from STATUSBARPIC, and the size the status bar code expects
PICS = ([("statusbar", 0, (320, 40))]
        + [(n, 5 + i, (48, 24)) for i, n in
           enumerate(("knife", "gun", "machinegun", "gatlinggun"))]
        + [(n, 9 + i, (8, 16)) for i, n in
           enumerate(("nokey", "goldkey", "silverkey", "blank"))]
        + [("n%d" % i, 13 + i, (8, 16)) for i in range(10)]
        + [("face%d" % i, 23 + i, (24, 32)) for i in range(22)]
        # The LEVELEND lump that LevelCompleted() draws with, just before the
        # status bar. Neither GFXV header matches the shipped file here
        # either; the order is GFXV_WL6.H's, which has the apostrophe that
        # the v1.4 Write() uses, and it ends one pic (L_BJWINSPIC) before
        # STATUSBARPIC. Every size is asserted.
        + [("l_guy", -43, (104, 88)), ("l_colon", -42, (8, 16))]
        + [("l_n%d" % i, -41 + i, (16, 16)) for i in range(10)]
        + [("l_percent", -31, (16, 16))]
        + [("l_%c" % (65 + i), -30 + i, (16, 16)) for i in range(26)]
        + [("l_expoint", -4, (8, 16)), ("l_apostrophe", -3, (8, 16)),
           ("l_guy2", -2, (104, 88))]
        # BJ's grin when he picks up the gatling gun, and the pic between
        # floors; these follow the faces as in both headers.
        + [("gotgatling", 45, (24, 32)), ("getpsyched", 48, (224, 48))]
        # The victory screen's BJ, the pic just before the status bar.
        + [("l_bjwins", -1, (88, 88))]
        # The frame ShowArticle() draws round a text page. The README lump at
        # the start of the file is where GFXV_WL1.H says, chunks 17..20.
        + [("h_topwindow", 17 - 98, (320, 8)), ("h_leftwindow", 18 - 98, (8, 192)),
           ("h_rightwindow", 19 - 98, (8, 192)),
           ("h_bottominfo", 20 - 98, (304, 24))]
        # PAUSEDPIC, after the gatling grin and the mutant BJ.
        + [("paused", 47, (64, 32))]
        # The CONTROLS lump the menus draw with. The shipped file has one more
        # README pic than GFXV_WL1.H (a Spear of Destiny advert, chunk 21), so
        # C_OPTIONSPIC is chunk 22, not 21; checked by rendering the chunks.
        + [("c_options", -76, (152, 48)), ("c_cursor1", -75, (24, 16)),
           ("c_cursor2", -74, (24, 16)), ("c_notselected", -73, (24, 8)),
           ("c_selected", -72, (24, 8)), ("c_fxtitle", -71, (136, 16)),
           ("c_digititle", -70, (136, 16)), ("c_musictitle", -69, (136, 16)),
           ("c_mouselback", -68, (104, 16))]
        + [("c_skill%d" % i, -67 + i, (24, 32)) for i in range(4)]
        + [("c_episode%d" % i, -56 + i, (48, 24)) for i in range(6)]
        # The attract loop: TITLEPIC, PG13PIC, CREDITSPIC and HIGHSCORESPIC
        # follow the status bar, and the high score column headings are in
        # the CONTROLS lump.
        + [("title", 1, (320, 200)), ("pg13", 2, (88, 64)),
           ("credits", 3, (320, 200)), ("highscores", 4, (224, 56))]
        + [("c_level", -48, (32, 8)), ("c_name", -47, (32, 8)),
           ("c_score", -46, (32, 8))]
        # Load Game and Save Game: the disk DrawLSAction() flips while it
        # works, and the two screen titles.
        + [("c_diskloading1", -62, (32, 32)), ("c_diskloading2", -61, (32, 32)),
           ("c_loadgame", -58, (216, 48)), ("c_savegame", -57, (216, 48))]
        # The Control and Customize screens' titles, after the disks.
        + [("c_control", -60, (144, 48)), ("c_customize", -59, (152, 48))]
        # Pics only the registered games have (no offset: the shareware has
        # none): the frame round the victory screen's time code, and Spear
        # of Destiny's face for a big hit...
        + [("c_timecode", None, None), ("bjouch", None, None)]
        # ...and the rest of Spear of Destiny's own: the menus' backdrop and
        # skill title, the high scores' Spear, BJ's collapse, the title in
        # two halves, the end screens (these last two with palettes of their
        # own: PIC_PALETTE), and god mode's faces.
        + [("c_backdrop", None, None), ("c_howtough", None, None),
           ("c_wonspear", None, None)]
        + [("bjcollapse%d" % i, None, None) for i in range(1, 5)]
        + [("title1", None, None), ("title2", None, None)]
        + [("endscreen%d" % i, None, None) for i in (11, 12, 3, 4, 5, 6, 7, 8, 9)]
        + [("godface%d" % i, None, None) for i in range(1, 4)]
        + [("bjwaiting%d" % i, None, None) for i in range(1, 3)])

# The same pics' names in GFXV_WL6.H and GFXV_SOD.H.
FACE_NAMES = ["FACE%d%sPIC" % (i // 3 + 1, "ABC"[i % 3]) for i in range(21)] \
             + ["FACE8APIC"]
PIC_GFX = dict(
    [("statusbar", "STATUSBARPIC"), ("knife", "KNIFEPIC"), ("gun", "GUNPIC"),
     ("machinegun", "MACHINEGUNPIC"), ("gatlinggun", "GATLINGGUNPIC"),
     ("nokey", "NOKEYPIC"), ("goldkey", "GOLDKEYPIC"),
     ("silverkey", "SILVERKEYPIC"), ("blank", "N_BLANKPIC")]
    + [("n%d" % i, "N_%dPIC" % i) for i in range(10)]
    + [("face%d" % i, FACE_NAMES[i]) for i in range(22)]
    + [("l_guy", "L_GUYPIC"), ("l_colon", "L_COLONPIC")]
    + [("l_n%d" % i, "L_NUM%dPIC" % i) for i in range(10)]
    + [("l_percent", "L_PERCENTPIC")]
    + [("l_%c" % (65 + i), "L_%cPIC" % (65 + i)) for i in range(26)]
    + [("l_expoint", "L_EXPOINTPIC"), ("l_apostrophe", "L_APOSTROPHEPIC"),
       ("l_guy2", "L_GUY2PIC"), ("gotgatling", "GOTGATLINGPIC"),
       ("getpsyched", "GETPSYCHEDPIC"), ("l_bjwins", "L_BJWINSPIC"),
       ("h_topwindow", "H_TOPWINDOWPIC"), ("h_leftwindow", "H_LEFTWINDOWPIC"),
       ("h_rightwindow", "H_RIGHTWINDOWPIC"),
       ("h_bottominfo", "H_BOTTOMINFOPIC"), ("paused", "PAUSEDPIC"),
       ("c_options", "C_OPTIONSPIC"), ("c_cursor1", "C_CURSOR1PIC"),
       ("c_cursor2", "C_CURSOR2PIC"), ("c_notselected", "C_NOTSELECTEDPIC"),
       ("c_selected", "C_SELECTEDPIC"), ("c_fxtitle", "C_FXTITLEPIC"),
       ("c_digititle", "C_DIGITITLEPIC"), ("c_musictitle", "C_MUSICTITLEPIC"),
       ("c_mouselback", "C_MOUSELBACKPIC")]
    + [("c_skill%d" % i, n) for i, n in enumerate(
        ("C_BABYMODEPIC", "C_EASYPIC", "C_NORMALPIC", "C_HARDPIC"))]
    + [("c_episode%d" % i, "C_EPISODE%dPIC" % (i + 1)) for i in range(6)]
    + [("title", "TITLEPIC"), ("pg13", "PG13PIC"), ("credits", "CREDITSPIC"),
       ("highscores", "HIGHSCORESPIC"), ("c_level", "C_LEVELPIC"),
       ("c_name", "C_NAMEPIC"), ("c_score", "C_SCOREPIC"),
       ("c_diskloading1", "C_DISKLOADING1PIC"),
       ("c_diskloading2", "C_DISKLOADING2PIC"),
       ("c_loadgame", "C_LOADGAMEPIC"), ("c_savegame", "C_SAVEGAMEPIC"),
       ("c_control", "C_CONTROLPIC"), ("c_customize", "C_CUSTOMIZEPIC"),
       ("c_timecode", "C_TIMECODEPIC"), ("bjouch", "BJOUCHPIC"),
       ("c_backdrop", "C_BACKDROPPIC"), ("c_howtough", "C_HOWTOUGHPIC"),
       ("c_wonspear", "C_WONSPEARPIC")]
    + [("bjcollapse%d" % i, "BJCOLLAPSE%dPIC" % i) for i in range(1, 5)]
    + [("title1", "TITLE1PIC"), ("title2", "TITLE2PIC")]
    + [("endscreen%d" % i, "ENDSCREEN%dPIC" % i) for i in (11, 12, 3, 4, 5, 6, 7, 8, 9)]
    + [("godface%d" % i, "GODMODEFACE%dPIC" % i) for i in range(1, 4)]
    + [("bjwaiting%d" % i, "BJWAITING%dPIC" % i) for i in range(1, 3)])
assert set(PIC_GFX) == {name for name, _, _ in PICS}

# The pics drawn with a palette of their own, and the VGAGRAPH chunk that
# holds it: Spear of Destiny's title, and each end screen, as EndSpear()
# fades them in. (The end screens are numbered as the fade order has them:
# the first shown is ENDSCREEN11PIC with END1PALETTE, the last ENDSCREEN12PIC
# with END2PALETTE.)
PIC_PALETTE = dict(
    [("title1", "TITLEPALETTE"), ("title2", "TITLEPALETTE"),
     ("endscreen11", "END1PALETTE"), ("endscreen12", "END2PALETTE")]
    + [("endscreen%d" % i, "END%dPALETTE" % i) for i in range(3, 10)])

# A pic this big is kept out of the memory-resident blob, in wolfbig.dat,
# and read in when a screen shows it: the full-screen title, credits and
# sign-on, and pictures of their kind.
BIG_PIC = 32000
# VGAGRAPH chunks 1 and 2 are STARTFONT and STARTFONT+1: the small font the
# text pages use and the larger one the menus use. The articles are the
# externs at the end of the file: T_HELPART, four demos, then T_ENDART1 last
# - the shareware build has no other episodes' articles after it.
STARTFONT = 1


def footer_shades(pic):
    """The colors the strip at the foot of the menus is written in, row by
    row of the five it writes on - read out of the picture rather than
    written down here, because the two games do not agree: Wolfenstein
    signs its keys in pink and its words in red, Spear of Destiny signs in
    white and writes in white fading to grey. Three shades come back: the
    keys' (taken from the Enter sign and ESC, either side of SELECT), the
    words' (from MOVE) and the arrow sign's, whose middle row is the gap
    between its two halves and so is left the keys'."""
    name, w, h, rows = pic

    def shade(spans):
        out = []
        for y in range(5):
            seen = [rows[(9 + y) * w + x] for x0, x1 in spans
                    for x in range(x0, x1 + 1) if rows[(9 + y) * w + x]]
            # a row's shade is the color most of it is in, so that the
            # darker pixels the edges are smoothed with do not count
            out.append(max(seen, key=lambda v: (seen.count(v), -seen.index(v)))
                       if seen else None)
        return out

    keys = shade([(33, 40), (67, 83)])
    words = shade([(14, 30)])
    arrows = [c if c is not None else keys[y]
              for y, c in enumerate(shade([(4, 12)]))]
    return tuple(keys), tuple(words), tuple(arrows)


def footer_arrows(pic):
    """Which columns the arrow sign stands in: 6..10 in Wolfenstein's
    strip, 5..11 in Spear's, which draws an outline around it."""
    name, w, h, rows = pic
    cols = [x for x in range(2, 14)
            for y in range(9, 14) if rows[y * w + x]]
    return min(cols), max(cols)


def pad_footer(pic, shades):
    """C_MOUSELBACKPIC, the strip at the foot of the menus, shows the PC's
    keys: arrows MOVE, an Enter sign SELECT, ESC BACK - in a 3x5 font, the
    keys in one shade and the words in another, on rows 9..13. The pad
    selects with A and goes back with B, so the Enter sign and ESC give way
    to an A and a B, taken from the picture's own BACK and turned the shade
    it signs keys in, and the right-hand words are spaced out again. The
    arrows and MOVE stay."""
    name, w, h, rows = pic
    if (w, h) != (104, 16):
        raise SystemExit("c_mouselback is %dx%d, expected 104x16" % (w, h))
    rows = bytearray(rows)
    keys = shades[0]                             # the keys' shade by row

    def cut(x0, x1, recolor=False):
        return [[(keys[y] if rows[(9 + y) * w + x] and recolor
                  else rows[(9 + y) * w + x]) for x in range(x0, x1 + 1)]
                for y in range(5)]

    letter_b, letter_a = cut(84, 86, True), cut(88, 90, True)
    select, back = cut(43, 66), cut(84, 99)
    for y in range(9, 14):                       # clear after MOVE
        for x in range(31, 102):
            rows[y * w + x] = 0
    # MOVE ends at 30; seven columns between groups, three from key to word
    x = 38
    for glyph, gap in ((letter_a, 3), (select, 7), (letter_b, 3), (back, 0)):
        for y in range(5):
            for i, v in enumerate(glyph[y]):
                rows[(9 + y) * w + x + i] = v
        x += len(glyph[0]) + gap
    return (name, w, h, bytes(rows))


def sideways_footer(pic, shades):
    """The same strip for choosing along a line - Warp to Floor's number -
    with its up-and-down arrows pointing left and right instead. The
    original sign is two triangles five wide and two tall; turned, two wide
    reads as a blob at this size, so these are three wide, a column apart
    and centered where the original is, each pixel in the shade of its row
    as the original's are."""
    name, w, h, rows = pic
    rows = bytearray(rows)
    shade = shades[2]
    sign = ("..#.#..",
            ".##.##.",
            "###.###",
            ".##.##.",
            "..#.#..")
    # the old sign's tips reach into the frame above and below it, at
    # column 8: those take their neighbors' colors
    rows[8 * w + 8] = rows[8 * w + 7]
    rows[14 * w + 8] = rows[14 * w + 7]
    for y in range(5):
        for x in range(4, 13):                   # the old sign out
            rows[(9 + y) * w + x] = 0
        for x, c in enumerate(sign[y]):
            if c == "#":
                rows[(9 + y) * w + 5 + x] = shade[y]
    return ("c_mouselback_lr", w, h, bytes(rows))


def typing_footer(pic, shades):
    """The strip for typing a name - a save's or a high score's - where the
    buttons mean something else: up and down step the letter, A adds it,
    B erases, Start is done. So it reads LETTER, A ADD, B ERASE, START DONE,
    wider than the original (144 pixels, not 104) to hold it all. The frame
    is the original's, stretched through the middle; the letters are its
    own 3x5 ones, save R, D and N, which it never needed and are drawn to
    match. Keys are in the shade it signs keys in, words in the one it
    writes words in, each by row as it shades them. It takes the strip as
    it shipped, so the letters are where they were: MOVE, SELECT, BACK."""
    name, w, h, rows = pic
    W = 144
    keys, words, _ = shades
    arrow0, arrow1 = footer_arrows(pic)

    def mask(x0, x1):
        return ["".join("#" if rows[(9 + y) * w + x] else "."
                        for x in range(x0, x1 + 1)) for y in range(5)]

    glyphs = {"L": mask(52, 54), "E": mask(28, 30), "T": mask(64, 66),
              "A": mask(88, 90), "S": mask(43, 46), "O": mask(20, 22),
              "B": mask(84, 86),
              "R": ["##.", "#.#", "##.", "#.#", "#.#"],
              "D": ["##.", "#.#", "#.#", "#.#", "##."],
              "N": ["#..#", "##.#", "#.##", "#..#", "#..#"]}

    # the frame, stretched: the column at the seam repeated to make up the
    # width, which keeps the bar under it in its bands. Above the frame and
    # below it is backdrop - flat in Wolfenstein, marbled in Spear of
    # Destiny, where a repeated column would draw as a smear - so there the
    # rest of the strip is slid along instead and the marbling goes on.
    out = bytearray(W * h)
    for y in range(h):
        frame = 8 <= y <= 14
        for x in range(W):
            if x < 60:
                ox = x
            elif x >= W - 44 or not frame:
                ox = x - (W - w)
            else:
                ox = 59
            out[y * W + x] = rows[y * w + ox]
    for y in range(9, 14):
        for x in range(2, W - 2):
            out[y * W + x] = rows[y * w + x] if arrow0 <= x <= arrow1 else 0

    x = 14                                   # after the arrows, as MOVE was
    for word, shade, gap in (("LETTER", words, 7), ("A", keys, 3),
                             ("ADD", words, 7), ("B", keys, 3),
                             ("ERASE", words, 7), ("START", keys, 3),
                             ("DONE", words, 0)):
        for i, ch in enumerate(word):
            g = glyphs[ch]
            for y in range(5):
                for gx, c in enumerate(g[y]):
                    if c == "#":
                        out[(9 + y) * W + x + gx] = shade[y]
            x += len(g[0]) + (1 if i < len(word) - 1 else 0)
        x += gap
    if x > W - 3:
        raise SystemExit("the typing strip's words run to %d of %d" % (x, W))
    return ("c_mouselback_typing", W, h, bytes(out))


def load_vgagraph():
    with open(fname("VGADICT"), "rb") as f:
        nodes = struct.unpack("<512H", f.read()[:1024])
    with open(fname("VGAHEAD"), "rb") as f:
        head = f.read()
    with open(fname("VGAGRAPH"), "rb") as f:
        gfx = f.read()

    def pos(c):
        if (c + 1) * 3 > len(head):
            return -1
        v = head[c * 3] | (head[c * 3 + 1] << 8) | (head[c * 3 + 2] << 16)
        return -1 if v == 0xFFFFFF else v

    def expand(src, length):
        """CAL_HuffExpand: bits LSB first, node 254 is the head."""
        out = bytearray()
        node, si, cur, mask = 254, 1, src[0], 1
        while len(out) < length:
            val = nodes[node * 2 + 1] if (cur & mask) else nodes[node * 2]
            if mask == 0x80:
                mask = 1
                cur = src[si] if si < len(src) else 0
                si += 1
            else:
                mask <<= 1
            if val < 256:
                out.append(val)
                node = 254
            else:
                node = val - 256
        return bytes(out)

    numchunks = len(head) // 3 - 1      # the final entry is the end of file

    def chunk(c):
        p = pos(c) if 0 <= c < numchunks else -1
        if p < 0:
            raise SystemExit("chunk %d is not in %s" % (c, fname("VGAGRAPH")))
        n = c + 1
        while pos(n) < 0:
            n += 1
        expanded = struct.unpack_from("<I", gfx, p)[0]
        return expand(gfx[p + 4:pos(n)], expanded)

    table = chunk(0)
    pictable = [struct.unpack_from("<2H", table, i * 4)
                for i in range(len(table) // 4)]

    graphics = VT["GRAPHICS"]
    if EXT == "WL1":
        bars = [i for i, (w, h) in enumerate(pictable) if (w, h) == (320, 40)]
        if len(bars) != 1:
            raise SystemExit("expected one 320x40 pic, found %d" % len(bars))
        base = bars[0] + STARTPICS
    else:
        base = graphics["STATUSBARPIC"]
    # The shareware's chunks after its README lump sit this far on from
    # where GFXV_WL6.H has them; the others' sit where their headers say.
    shift = base - graphics["STATUSBARPIC"]
    print("  %d pics; STATUSBARPIC is chunk %d (the header says %d)"
          % (len(pictable), base, graphics["STATUSBARPIC"]))

    def decode(name, c, want=None):
        w, h = pictable[c - STARTPICS]
        if want is not None and (w, h) != want:
            raise SystemExit("%s (chunk %d) is %dx%d, expected %dx%d"
                             % (name, c, w, h, want[0], want[1]))
        data = chunk(c)
        if len(data) != w * h:
            raise SystemExit("%s is %d bytes, expected %d"
                             % (name, len(data), w * h))
        # VL_MemToScreen() copies four VGA planes in turn, so plane p holds
        # the pixels whose x is p modulo 4. Unweave them into plain rows.
        bw = w // 4
        rows = bytearray(w * h)
        for y in range(h):
            for x in range(w):
                rows[y * w + x] = data[(x & 3) * bw * h + y * bw + (x >> 2)]
        return (name, w, h, bytes(rows))

    # Each version has what it has: a pic its data lacks (Spear of Destiny
    # has no episode pics, say) is an empty one, 0x0.
    out = []
    for name, rel, want in PICS:
        if EXT == "WL1":
            out.append(decode(name, base + rel, want) if rel is not None
                       else (name, 0, 0, b""))
        elif PIC_GFX[name] in graphics:
            # Spear of Destiny draws some of these at other sizes; the full
            # game's are the shareware's
            out.append(decode(name, graphics[PIC_GFX[name]],
                              want if EXT == "WL6" else None))
        else:
            out.append((name, 0, 0, b""))
    footer = [p for p in out if p[0] == "c_mouselback"][0]  # as it shipped
    if (footer[1], footer[2]) == (104, 16):
        shades = footer_shades(footer)   # the game's own, off the shipped strip
        out = [pad_footer(p, shades) if p is footer else p for p in out]
        out.append(sideways_footer(
            [p for p in out if p[0] == "c_mouselback"][0], shades))
        out.append(typing_footer(footer, shades))
    else:
        # another design of strip: it stands for all three
        out += [("c_mouselback_lr",) + footer[1:],
                ("c_mouselback_typing",) + footer[1:]]
    out.append(load_signon())          # PIC_SIGNON, before the articles' pics
    # PIC_MUTANTBJ: DrawFace()'s face for a player a syringe killed, between
    # the gatling grin and PAUSEDPIC. Last of the fixed pics, so data
    # extracted before it keeps every other pic's number; Spear of Destiny
    # has none.
    if EXT == "WL1":
        out.append(decode("mutantbj", base + 46, (24, 32)))
    elif "MUTANTBJPIC" in graphics:
        out.append(decode("mutantbj", graphics["MUTANTBJPIC"], (24, 32)))
    else:
        out.append(("mutantbj", 0, 0, b""))

    # the palettes some pics are drawn with, as 6-bit VGA triplets
    palettes = {}
    for name, pal in PIC_PALETTE.items():
        if pal in graphics and any(p[0] == name and p[1] for p in out):
            data = chunk(graphics[pal] + shift)
            if len(data) != 768:
                raise SystemExit("%s is %d bytes, expected 768" % (pal, len(data)))
            palettes[name] = data

    # The article names its pics by chunk number (^G y,x,chunk). Those pics
    # are appended after the fixed list and the numbers rewritten to their
    # index here, so the game needs no chunk table.
    picindex = {}

    def repl(m):
        c = int(m.group(4))
        if c not in picindex:
            picindex[c] = len(out)
            out.append(decode("article%d" % c, c))
        return b"^%s%s,%s,%d" % (m.group(1), m.group(2), m.group(3), picindex[c])

    def article(name):
        """T_HELPART, T_ENDARTn: a text article, or None if the data does
        not have one there."""
        if name not in graphics or not 0 <= graphics[name] + shift < numchunks:
            return None
        c = graphics[name] + shift
        text = chunk(c)
        if not text.lstrip().startswith(b"^P") or b"^E" not in text:
            return None
        text = text[:text.index(b"^E") + 2] + b"\r\n"
        text = re.sub(rb"\^([GgTt])(\d+),(\d+),(\d+)", repl, text)
        print("  %s is chunk %d, %d bytes, %d pages"
              % (name, c, len(text), text.upper().count(b"^P")))
        return text

    # "Read This!" (the shareware's, and the full game's, which the GT
    # edition hid), and the text after each episode.
    help_article = article("T_HELPART") or b""
    end_articles = [a for a in (article("T_ENDART%d" % i) for i in range(1, 7))
                    if a is not None]

    # T_DEMO0..3: a byte of map number, a word of total length, a spare
    # byte, then three bytes a frame.
    demos = []
    for i in range(4):
        d = chunk(graphics["T_DEMO%d" % i] + shift)
        length = struct.unpack_from("<H", d, 1)[0]
        if length > len(d) or (length - 4) % 3:
            raise SystemExit("demo %d has a bad length %d" % (i, length))
        demos.append(d[:length])
    print("  demos: floors %s, %s frames"
          % ([d[0] + 1 for d in demos], [(len(d) - 4) // 3 for d in demos]))

    fonts = [chunk(STARTFONT), chunk(STARTFONT + 1)]
    if struct.unpack_from("<H", fonts[0], 0)[0] != 10:
        raise SystemExit("STARTFONT is not the 10-pixel font")
    print("  fonts %s bytes, heights %s; article pics %s"
          % ([len(f) for f in fonts],
             [struct.unpack_from("<H", f, 0)[0] for f in fonts], sorted(picindex)))
    return out, fonts, help_article, end_articles, demos, palettes


# --------------------------------------------------------------------------
# WOLF3D.EXE - the palette and the sign-on screen were linked into the
# executable rather than shipped as data. id packed it with LZEXE, so it is
# unpacked (tools/lzexe.py) and searched.
# --------------------------------------------------------------------------

_exe_image = None


def exe_image():
    """The game executable's load module - WOLF3D.EXE, or Spear of
    Destiny's SPEAR.EXE (which some releases also call WOLF3D.EXE) - or
    None if there is none. One someone has already unpacked is searched as
    it is."""
    global _exe_image
    if _exe_image is None:
        _exe_image = b""
        order = ("SPEAR.EXE", "WOLF3D.EXE") if EXT == "SOD" else ("WOLF3D.EXE", "SPEAR.EXE")
        names = [n for n in order if os.path.exists(data_path(n))]
        if not names:
            print("  WOLF3D.EXE / SPEAR.EXE: not in %s" % DATA)
        else:
            with open(data_path(names[0]), "rb") as f:
                exe = f.read()
            try:
                _exe_image = lzexe.unpack(exe)
                print("  %s: unpacked, %d bytes" % (names[0], len(_exe_image)))
            except lzexe.NotLzexe as e:
                print("  %s: %s; searching it as it is" % (names[0], e))
                _exe_image = exe
    return _exe_image or None


# The sign-on picture is found by rows 92..107 - the memory columns and the
# hardware boxes - so that no pixels of it need be kept here: a hash of the
# band's first 32 bytes to find a candidate quickly, then of the whole band.
# Wolfenstein's (the shareware's and the registered game's are the same
# there) and Spear of Destiny's. Both pictures start on a 16-byte boundary,
# but every offset is tried, for other releases' sake.
#
# The two are laid out alike: the same slots for the memory bars and the
# same hardware boxes, which is why id's IntroScreen() has one set of
# numbers for both. (Spear's band was once hashed 970 bytes - ten pixels and
# three rows - into the picture, which found it that much too late: the
# bars then filled beside their slots, the left edge was cut off, and the
# last rows were whatever the executable held after it.)
SIGNON_BAND = (92, 108)
SIGNON_BANDS = [
    ("874f8c34fb3d58323812436d7168459b7761d0a4",
     "d4631df49f3de98adae228bcb0570faebe1090f2"),       # Wolfenstein 3D
    ("40978b371d85da18bd0649dc55e7c804c0a5f7a2",
     "edf3edd5ae809f60b8a71065662b596ee389e06e"),       # Spear of Destiny
]


def signon_from_exe():
    image = exe_image()
    if not image:
        return None
    lo, hi = SIGNON_BAND
    starts = {start for start, _ in SIGNON_BANDS}
    wholes = {whole for _, whole in SIGNON_BANDS}
    for off in range(0, len(image) - 64000 + 1):
        band = off + lo * 320
        if hashlib.sha1(image[band:band + 32]).hexdigest() in starts and \
                hashlib.sha1(image[band:off + hi * 320]).hexdigest() in wholes:
            print("  sign-on screen: 320x200 from the executable at 0x%x" % off)
            return ("signon", 320, 200, image[off:off + 64000])
    print("  sign-on screen: not found in the executable (another version?)")
    return None


def load_signon():
    """The sign-on screen. It is not in VGAGRAPH: SignonScreen() shows a
    320x200 picture linked into the EXE (the public _signon, alias
    introscn), and VL_MungePic() only weaves it into planes at run time, so
    the data is plain rows in the executable.

    Optional: when WOLF3D.EXE is not there, or is a version the picture is
    not found in, there is none, and an empty one tells the game to skip the
    screen. (The source release has one too, in SIGNON.OBJ, but it is the GT
    Interactive edition's rather than the shareware's, so it is not used.)"""
    found = signon_from_exe()
    if found:
        return found
    print("  no sign-on screen: the ROM will start at the PG-13 screen")
    return ("signon", 0, 0, b"")


def pack_pics(pics, big, palettes):
    """Same shape as the sprite section: 32-bit fields, then 8-bit pixels.
    A pic of BIG_PIC bytes or more goes on the end of `big` (wolfbig.dat)
    instead, and its offset word is that position with the top bit set.
    The header is the count, where the pixels start, how many palettes of
    their own the pics have, and how many are the game's fixed list (the
    articles' follow; data from before PIC_MUTANTBJ has 0 here, and the
    game counts one fewer); after the dimensions and offsets comes each
    pic's palette (0 for the game's, n for the nth of those), then the
    palettes, 256 RGBA5551 colors each, a word to a color."""
    count = len(pics)
    names = [name for name, _, _, _ in pics]
    own = []                                    # distinct palettes, in order
    for name in names:
        if name in palettes and palettes[name] not in own:
            own.append(palettes[name])
    pixels_off = (4 + count * 3 + len(own) * 256) * 4

    words = [count, pixels_off, len(own), names.index("mutantbj") + 1]
    words += [(pw << 16) | ph for _, pw, ph, _ in pics]
    offsets = []
    here, resident, moved = pixels_off, [], 0
    for _, _, _, pixels in pics:
        if len(pixels) >= BIG_PIC:
            offsets.append(0x80000000 | len(big))
            big += pixels
            moved += len(pixels)
        else:
            offsets.append(here)
            resident.append(pixels)
            here += len(pixels)
    words += offsets
    words += [own.index(palettes[n]) + 1 if n in palettes else 0 for n in names]
    for pal in own:
        words += struct.unpack(">256H", to_rgba5551(pal))

    blob = struct.pack(">%dI" % len(words), *words) + b"".join(resident)
    print("  %d pics, %d bytes, and %d bytes more in wolfbig.dat; %d palettes"
          " of their own" % (count, len(blob), moved, len(own)))
    return blob


# --------------------------------------------------------------------------
# Audio
#
# AUDIOHED.WL1 is a longword offset per chunk into AUDIOT.WL1. Like the
# graphics, the shipped data does not match the header in the source tree:
# AUDIOWL1.H declares 69 sounds and 234 chunks, but the file has 288 - the
# layout of AUDIOWL6.H, 87 sounds of PC, AdLib and (unused) digi chunks
# followed by 27 songs. wolfdigimap in WL_MAIN.C confirms it, naming
# YEAHSND, which only AUDIOWL6.H has. So the sound numbers, the digitized
# sound map, the per-floor song list and the stereo tables were read out of
# the source text rather than transcribed by hand: tools/wolf_tables.py is
# written by --write-tables from these parsers, and when the source is there
# every build parses it again and checks the two agree.
#
# Nothing is synthesized in this script. It writes the raw inputs to
# assets/audio - IMF music streams, AdLib sound chunks, and the digitized
# sounds as WAV - and tools/audio/render_audio.c turns the first two into
# PCM with an OPL emulator during the build.
# --------------------------------------------------------------------------

import re

AUDIO_DIR = os.path.join(PROJECT, "assets", "audio")
DIGI_RATE = 7042            # ID_SD.C: SB time constant 256 - 1000000/7000


def parse_enum(path, suffix):
    """NAME, // N lines -> {NAME: N}, for the names ending in suffix."""
    names = {}
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            m = re.match(r"\s*(\w+%s),\s*//\s*(\d+)" % suffix, line)
            if m:
                names[m.group(1)] = int(m.group(2))
    return names


def source_text(name):
    with open(os.path.join(WOLFSRC, name), "r", encoding="latin-1") as f:
        return f.read()


def preprocess(text, defines):
    """The lines of `text` a C compiler would see with `defines` set, for
    the #ifdef / #ifndef / #else / #endif the lists below use."""
    out, stack = [], []                  # stack: is each open branch live
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|else|endif)\b\s*(\w*)", line)
        if not m:
            if all(stack):
                out.append(line)
            continue
        what, name = m.groups()
        if what == "ifdef":
            stack.append(name in defines)
        elif what == "ifndef":
            stack.append(name not in defines)
        elif what == "else":
            stack[-1] = not stack[-1]
        else:
            stack.pop()
    return "\n".join(out)


def block(text, start, end):
    """The text from the line holding `start` up to `end` after it."""
    i = text.index(start)
    return text[i:text.index(end, i)]


def parse_digimap(defines):
    """wolfdigimap[] in WL_MAIN.C for one build: [(sound name, digi index)].
    The full list for Wolfenstein (the shareware's own recordings are its
    first part), or Spear of Destiny's."""
    body = preprocess(block(source_text("WL_MAIN.C"), "wolfdigimap[] =", "};"),
                      defines)
    return [(n, int(i)) for n, i in re.findall(r"(\w+SND),\s*(\d+)", body)]


def parse_songs(defines):
    """songs[] in WL_PLAY.C for one build, by name: Wolfenstein's six
    episodes of ten floors (the shareware plays the first ten), or Spear of
    Destiny's 21."""
    body = preprocess(block(source_text("WL_PLAY.C"), "int songs[]=", "};"),
                      defines)
    body = re.sub(r"//.*", "", body)
    return re.findall(r"\b(\w+_MUS)\b", body)


def parse_sprites(defines):
    """WL_DEF.H's sprite enum for one build, in order: index n is sprite n.
    (One entry, MACHINEGUNATK3, lacks the SPR_ its neighbors have.)"""
    body = preprocess(block(source_text("WL_DEF.H"), "SPR_DEMO,", "};"),
                      defines)
    body = re.sub(r"//.*", "", body)
    return re.findall(r"\b([A-Z][A-Z0-9_]*)\b", body)


def parse_graphics(name):
    """A GFXV_*.H graphicnums enum: {NAME: chunk}, counting on from any
    NAME=N."""
    body = block(source_text(name), "typedef enum", "ENUMEND")
    body = re.sub(r"//.*", "", body.split("{", 1)[1])
    chunks, n = {}, 0
    for name_, value in re.findall(r"\b([A-Z][A-Z0-9_]*)\b(?:\s*=\s*(\d+))?",
                                   body):
        n = int(value) if value else n
        chunks[name_] = n
        n += 1
    return chunks


def parse_pantables():
    """lefttable/righttable in WL_GAME.C, 15 rows of 30."""
    with open(os.path.join(WOLFSRC, "WL_GAME.C"), "r", encoding="latin-1") as f:
        text = f.read()
    tables = []
    for name in ("lefttable", "righttable"):
        start = text.index("byte %s[ATABLEMAX]" % name)
        body = text[text.index("{", start) + 1:text.index("};", start)]
        values = [int(v) for v in re.findall(r"\d+", body)]
        if len(values) != 15 * 30:
            raise SystemExit("%s has %d entries" % (name, len(values)))
        tables.append(values)
    return tables


def version_tables(audio_h, graphics_h, defines):
    """One version's tables: Wolfenstein's (which the shareware's data is
    laid out by too) or Spear of Destiny's."""
    audio_h = os.path.join(WOLFSRC, audio_h)
    return {
        "SOUNDS": parse_enum(audio_h, "SND"),
        "MUSIC": parse_enum(audio_h, "_MUS"),
        "DIGIMAP": parse_digimap(defines),
        "SONGS": parse_songs(defines),
        "SPRITES": parse_sprites(defines),
        "GRAPHICS": parse_graphics(graphics_h),
    }


def source_tables():
    """Everything tools/wolf_tables.py carries, parsed from id's source."""
    left, right = parse_pantables()
    return {
        "WL6": version_tables("AUDIOWL6.H", "GFXV_WL6.H", set()),
        "SOD": version_tables("AUDIOSOD.H", "GFXV_SOD.H", {"SPEAR"}),
        "LEFTTABLE": left,
        "RIGHTTABLE": right,
    }


def repo_tables():
    import wolf_tables
    return wolf_tables.TABLES


def write_tables(tables):
    import pprint
    lines = [
        '"""Tables from id Software\'s Wolfenstein 3D source release, for',
        "tools/extract_assets.py. Generated from the source by",
        "`extract_assets.py --write-tables`; do not edit by hand. The build",
        "parses the source again whenever it is present and checks these agree.",
        "",
        "TABLES['WL6'] is Wolfenstein's - the full game's, and the layout the",
        "shareware data really has - and TABLES['SOD'] Spear of Destiny's:",
        "",
        "  SOUNDS, MUSIC   the soundnames and musicnames enums in AUDIOWL6.H /",
        "                  AUDIOSOD.H",
        "  DIGIMAP         wolfdigimap[] in WL_MAIN.C, (sound, recording) pairs",
        "  SONGS           songs[] in WL_PLAY.C, a song per floor",
        "  SPRITES         WL_DEF.H's sprite enum, in order",
        "  GRAPHICS        GFXV_WL6.H / GFXV_SOD.H: each graphic's chunk",
        "",
        "and for both, LEFTTABLE and RIGHTTABLE, lefttable[] and righttable[] in",
        "WL_GAME.C: the SB Pro stereo attenuations, 15 rows of 30.",
        "",
        "id Software's code is used under the GPL grant described in README.md",
        '("License, and what not to redistribute")."""',
        "",
        "TABLES = " + pprint.pformat(tables, width=78, compact=True),
        ""]
    changed = write_file(TABLES_PY, "\n".join(lines).encode("utf-8"))
    print("%s %s" % ("wrote" if changed else "unchanged:", TABLES_PY))


def load_tables():
    """tools/wolf_tables.py, checked against id's source if it is there."""
    tables = repo_tables()
    if have_source("AUDIOWL6.H"):
        src = source_tables()
        if src != tables:
            bad = [k for k in tables if tables[k] != src[k]]
            raise SystemExit("tools/wolf_tables.py differs from the source in %s"
                             " - run with --write-tables" % ", ".join(bad))
        print("  tables: tools/wolf_tables.py agrees with the source")
    elif WOLFSRC is not None:
        raise SystemExit("--source %s has no AUDIOWL6.H: give the WOLFSRC folder"
                         " of id's source release" % WOLFSRC)
    else:
        print("  tables: tools/wolf_tables.py (no --source to check against)")
    return tables


TABLES = None                   # load_tables(), once main() has the paths


def write_file(path, data):
    """Write the file, unless it already holds exactly this. The build
    watches what this script writes and does the work again when it is
    newer: the songs and sound effects are rendered with an OPL emulator
    and converted, about a minute of it, and the blob decides whether the
    ROM is linked again. Extracting the same game a second time - after a
    change to a picture, say, or to switch back from the other game -
    leaves most of those files byte for byte as they were, so leaving
    their timestamps alone too saves that work. Returns whether it wrote."""
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    except OSError:
        pass                            # not there, or not readable: write it
    with open(path, "wb") as f:
        f.write(data)
    return True


def wav_bytes(rate, bits, samples):
    block = bits // 8
    return (b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate,
                                    rate * block, block, bits)
            + b"data" + struct.pack("<I", len(samples)) + samples)


# The songs the game plays by what they are for, in the order game.c and
# menu.c ask for them, by each version's names: "Read This!", the title
# (INTROSONG), the menus (MENUSONG), the high scores, the stats screen, the
# victory screen, Spear of Destiny's hidden id Software screen, and its
# ending, over BJ's collapse and the pictures after.
MUSIC_ROLES = {
    "WL6": ["CORNER_MUS", "NAZI_NOR_MUS", "WONDERIN_MUS", "ROSTER_MUS",
            "ENDLEVEL_MUS", "URAHERO_MUS", None, None],
    "SOD": [None, "XTOWER2_MUS", "WONDERIN_MUS", "XAWARD_MUS",
            "ENDLEVEL_MUS", "URAHERO_MUS", "XJAZNAZI_MUS", "XTHEEND_MUS"],
}


def load_audio(vswap, num_levels):
    sounds = VT["SOUNDS"]
    music = VT["MUSIC"]
    numsounds = len(sounds)
    STARTADLIB, STARTMUSIC = numsounds, numsounds * 3

    with open(fname("AUDIOHED"), "rb") as f:
        hed = f.read()
    with open(fname("AUDIOT"), "rb") as f:
        audiot = f.read()
    offs = struct.unpack("<%dI" % (len(hed) // 4), hed)
    if len(offs) - 1 != STARTMUSIC + len(music):
        raise SystemExit("%s has %d chunks, the audio header implies %d"
                         % (fname("AUDIOHED"), len(offs) - 1,
                            STARTMUSIC + len(music)))

    def chunk(c):
        return audiot[offs[c]:offs[c + 1]]

    # The folder holds what this run writes and nothing else, but the files
    # it has already are left alone where they match, so that the build
    # renders only what changed (write_file). The names written are kept to
    # sweep the rest away at the end.
    os.makedirs(AUDIO_DIR, exist_ok=True)
    written = set()

    def audio_file(name, data):
        written.add(name)
        write_file(os.path.join(AUDIO_DIR, name), data)

    # AdLib sound effects: SoundCommon (length, priority), Instrument, block,
    # then one byte per 140 Hz tick. The chunk is written as-is.
    info = []
    for s in range(numsounds):
        data = chunk(STARTADLIB + s)
        length, priority = struct.unpack_from("<IH", data, 0)
        audio_file("al%02d.adl" % s, data)
        info.append((priority, (length + 1) // 2))   # 140 Hz bytes -> 70 Hz tics

    # Music: a word of length in bytes, then (register, value, delay) at 700 Hz.
    songs = [music[n] for n in VT["SONGS"][:num_levels]]
    roles = [music[n] if n else 0xFFFFFFFF
             for n in MUSIC_ROLES["SOD" if EXT == "SOD" else "WL6"]]
    for m in sorted(set(songs) | {r for r in roles if r != 0xFFFFFFFF}):
        data = chunk(STARTMUSIC + m)
        length = struct.unpack_from("<H", data, 0)[0]
        if not length:
            raise SystemExit("song %d is empty in %s" % (m, fname("AUDIOT")))
        audio_file("m%02d.imf" % m, data[2:2 + length])

    # Digitized sounds: the last VSWAP chunk lists (first page, length) per
    # sound; the pages from sound_start on are 8-bit unsigned PCM.
    data, sound_start, chunks, offsets, lengths = vswap
    list_chunk = data[offsets[chunks - 1]:offsets[chunks - 1] + lengths[chunks - 1]]
    digi, page = [], sound_start
    for i in range(len(list_chunk) // 4):
        first, length = struct.unpack_from("<2H", list_chunk, i * 4)
        if page >= chunks - 1:
            break
        pcm = b"".join(data[offsets[sound_start + p]:offsets[sound_start + p]
                             + lengths[sound_start + p]]
                       for p in range(first, first + (length + 4095) // 4096))
        page += (length + 4095) // 4096
        # The shareware's list has all 46 of the full game's recordings,
        # but the file only carries the pages for the twenty in the "upload
        # version" part of wolfdigimap; the rest are empty chunks. Those
        # sounds fall back to AdLib, as they did in the shareware EXE.
        if len(pcm) < length:
            digi.append(0)
            continue
        audio_file("dg%02d.wav" % i, wav_bytes(DIGI_RATE, 8, pcm[:length]))
        digi.append((length * 70 + DIGI_RATE - 1) // DIGI_RATE)

    for old in os.listdir(AUDIO_DIR):   # another game's, or another part
        if old not in written:
            os.remove(os.path.join(AUDIO_DIR, old))

    digimap = [0xFFFFFFFF] * numsounds
    for name, d in VT["DIGIMAP"]:
        if d < len(digi) and digi[d]:
            digimap[sounds[name]] = d

    # The game's own sound numbers, and this data's sound for each - or
    # none, for a sound this version does not have.
    soundmap = [sounds.get(name, 0xFFFFFFFF) for name in GAME_SOUNDS]

    print("  %d sounds, %d of them digitized (%d of %d recordings present;"
          " some are shared), songs for floors %s"
          % (numsounds, sum(d != 0xFFFFFFFF for d in digimap),
             sum(1 for t in digi if t), len(digi), songs))
    return (info, digimap, digi, songs,
            [TABLES["LEFTTABLE"], TABLES["RIGHTTABLE"]], soundmap, roles)


def pack_sounds(info, digimap, digi, songs, pantables, soundmap, roles):
    """All 32-bit words, so tools/sim swaps the whole section in one pass;
    the first word is how many there are."""
    words = [0, len(info), len(digi), len(songs), len(soundmap), len(roles),
             0, 0]
    words += [(p << 16) | t for p, t in info]
    words += digimap
    words += digi
    words += songs
    words += pantables[0] + pantables[1]
    words += soundmap
    words += roles
    words[0] = len(words)
    return struct.pack(">%dI" % len(words), *words)


# --------------------------------------------------------------------------
# Palette - 768 bytes of 6-bit VGA components inside GAMEPAL.OBJ
# --------------------------------------------------------------------------

#   _gamepal's first sixteen entries are the EGA colors, which makes a
#   signature specific enough to find it inside the OMF record stream. (A
#   plain "768 bytes all under 64" scan matches two bytes early, on the tail
#   of the LEDATA record header.)
EGA16 = bytes([0, 0, 0, 0, 0, 42, 0, 42, 0, 0, 42, 42,
               42, 0, 0, 42, 0, 42, 42, 21, 0, 42, 42, 42,
               21, 21, 21, 21, 21, 63, 21, 63, 21, 21, 63, 63,
               63, 21, 21, 63, 21, 63, 63, 63, 21, 63, 63, 63])


def find_palette(data, name):
    start = data.find(EGA16)
    if start < 0 or start + 768 > len(data):
        return None
    window = data[start:start + 768]
    if max(window) >= 64:
        raise SystemExit("palette at 0x%x in %s has non-VGA components"
                         % (start, name))
    print("  palette at offset 0x%x in %s" % (start, name))
    return window


def load_palette():
    """gamepal, from the executable, or else GAMEPAL.OBJ in the source
    release (Wolfenstein's: Spear of Destiny's differs in two colors)."""
    image = exe_image()
    if image:
        found = find_palette(image, "the executable")
        if found:
            return found
    if not have_source("OBJ", "GAMEPAL.OBJ"):
        raise SystemExit("no palette: it is not in the data files but inside"
                         " the game's executable, which is %s; put the"
                         " game's WOLF3D.EXE (or SPEAR.EXE) in %s (id's"
                         " source release can stand in for it: see"
                         " docs/TECHNICAL_DETAILS.md)"
                         % ("missing" if not image else "not a version this"
                            " knows", DATA))
    path = os.path.join(WOLFSRC, "OBJ", "GAMEPAL.OBJ")
    with open(path, "rb") as f:
        found = find_palette(f.read(), "GAMEPAL.OBJ")
    if not found:
        raise SystemExit("no 768-byte VGA palette found in GAMEPAL.OBJ")
    return found


def palette_shifts(vga):
    """InitRedShifts() in WL_PLAY.C: the game palette, then the six red
    damage steps and three white bonus steps, in that order."""
    out = [bytes(vga)]
    for steps, target, count in ((8, (64, 0, 0), 6), (20, (64, 62, 0), 3)):
        for i in range(1, count + 1):
            shifted = bytearray(768)
            for j in range(768):
                base = vga[j]
                delta = (target[j % 3] - base) * i
                # C's division truncates toward zero; Python's // floors
                shifted[j] = base + (abs(delta) // steps) * (1 if delta >= 0 else -1)
            out.append(bytes(shifted))
    return out


def to_rgba5551(vga):
    """6-bit VGA triplets -> the RDRAM framebuffer's 16-bit RGBA5551."""
    out = bytearray()
    for i in range(256):
        # the shifted palettes can reach 64, one past the VGA DAC's range
        r, g, b = (min(v, 63) for v in vga[i * 3:i * 3 + 3])
        # 6 bits -> 8 bits -> 5 bits, the usual replicate-high-bits expansion
        r = ((r << 2) | (r >> 4)) >> 3
        g = ((g << 2) | (g >> 4)) >> 3
        b = ((b << 2) | (b >> 4)) >> 3
        out += struct.pack(">H", (r << 11) | (g << 6) | (b << 1) | 1)
    return bytes(out)


# --------------------------------------------------------------------------

def align8(blob):
    while len(blob) % 8:
        blob += b"\0"
    return blob


def detect_version():
    """WL1, WL6 or SOD: whichever game's VSWAP is in the data folder."""
    found = [e for e in VERSIONS if os.path.exists(data_path("VSWAP.%s" % e))]
    if not found:
        raise SystemExit("no VSWAP.WL1, VSWAP.WL6 or VSWAP.SOD in %s: copy"
                         " your copy of the game (its data files and"
                         " WOLF3D.EXE or SPEAR.EXE) there, or point --data at"
                         " it" % DATA)
    if len(found) > 1:
        raise SystemExit("%s holds more than one game (%s): give each its own"
                         " folder" % (DATA, ", ".join(found)))
    return found[0]


SPRITES_H = os.path.join(PROJECT, "src", "sprites.h")


def game_sprites(tables):
    """The game's sprite numbers: WL_DEF.H's for Wolfenstein, then Spear of
    Destiny's own."""
    wl6, sod = tables["WL6"]["SPRITES"], tables["SOD"]["SPRITES"]
    return wl6 + [n for n in sod if n not in wl6]


def sprites_header(names):
    """src/sprites.h: the game's sprite numbers as a C enum."""
    lines = ["/* sprites.h - the game's sprite numbers. Generated from",
             " * tools/wolf_tables.py by tools/extract_assets.py --write-tables;",
             " * do not edit. WL_DEF.H's enum for Wolfenstein, then the sprites",
             " * only Spear of Destiny has, in its order. */",
             "#ifndef SPRITES_H", "#define SPRITES_H", "", "enum {"]
    for i, n in enumerate(names):
        # id's own enum has MACHINEGUNATK3 without its prefix
        c = n if n.startswith("SPR_") else "SPR_" + n
        lines.append("    %s = %d," % (c, i))
    lines += ["    NUM_GAME_SPRITES = %d" % len(names), "};", "",
              "#endif /* SPRITES_H */", ""]
    return "\n".join(lines)


def main():
    global DATA, WOLFSRC, TABLES, EXT, VT, GAME_SPRITES, GAME_SOUNDS
    args = sys.argv[1:]
    write = "--write-tables" in args
    for flag in ("--data", "--source"):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                raise SystemExit("%s needs a folder" % flag)
            if flag == "--data":
                DATA = os.path.abspath(args[i + 1])
            else:
                WOLFSRC = os.path.abspath(args[i + 1])
    if write:
        if not have_source("AUDIOWL6.H"):
            raise SystemExit("--write-tables needs --source: the WOLFSRC folder"
                             " of id's source release")
        tables = source_tables()
        write_tables(tables)
        changed = write_file(
            SPRITES_H, sprites_header(game_sprites(tables)).encode("utf-8"))
        print("%s %s" % ("wrote" if changed else "unchanged:", SPRITES_H))
        return 0

    EXT = detect_version()
    print("%s: %s" % (DATA, {"WL1": "the shareware episode",
                             "WL6": "the full game, six episodes",
                             "SOD": "Spear of Destiny"}[EXT]))
    print("tables:")
    TABLES = load_tables()
    VT = TABLES["SOD" if EXT == "SOD" else "WL6"]
    wl6, sod = TABLES["WL6"], TABLES["SOD"]
    GAME_SPRITES = game_sprites(TABLES)
    with open(SPRITES_H) as f:
        if f.read() != sprites_header(GAME_SPRITES):
            raise SystemExit("src/sprites.h does not match tools/wolf_tables.py"
                             " - run with --write-tables")
    GAME_SOUNDS = sorted(wl6["SOUNDS"], key=wl6["SOUNDS"].get) + \
        [n for n in sorted(sod["SOUNDS"], key=sod["SOUNDS"].get)
         if n not in wl6["SOUNDS"]]

    print("maps:")
    levels = load_maps()
    print("vswap:")
    walls, sprite_chunks, vswap = load_vswap()
    sprites = pack_sprites(sprite_chunks)
    print("audio:")
    sounds = pack_sounds(*load_audio(vswap, len(levels)))

    # wolfbig.dat: every map's two planes as big-endian words, 16 KB a
    # floor from the start, then the big pics
    big = bytearray()
    for lvl in levels:
        for p in (0, 1):
            big += struct.pack(">%dH" % (MAPSIZE * MAPSIZE), *lvl["planes"][p])

    print("vgagraph:")
    pic_list, fonts, help_article, end_articles, demos, pic_palettes = load_vgagraph()
    pics = pack_pics(pic_list, big, pic_palettes)
    print("palette:")
    # ten palettes: normal, six red shifts, three white shifts
    palette = b"".join(to_rgba5551(p) for p in palette_shifts(load_palette()))

    # header is HEADER bytes; everything after it is 8-byte aligned
    HEADER = 48
    body = bytearray()

    pal_off = HEADER + len(body)
    body += palette
    body = bytearray(align8(bytes(body)))

    walls_off = HEADER + len(body)
    for page in walls:
        body += page
    body = bytearray(align8(bytes(body)))

    names_off = HEADER + len(body)            # the planes are in wolfbig.dat
    for lvl in levels:
        body += lvl["name"].encode("latin-1")[:15].ljust(16, b"\0")
    body = bytearray(align8(bytes(body)))

    sprites_off = HEADER + len(body)
    body += sprites
    body = bytearray(align8(bytes(body)))

    pics_off = HEADER + len(body)
    body += pics
    body = bytearray(align8(bytes(body)))

    sounds_off = HEADER + len(body)
    body += sounds
    body = bytearray(align8(bytes(body)))

    # Text: how many parts, and how many of them are end-of-episode
    # articles, then each part's big-endian length, then the parts: the
    # two font chunks exactly as VGAGRAPH has them (little-endian
    # fontstructs, read byte by byte), "Read This!" (empty if there is
    # none), the four demos, and the end articles; each article ends in a
    # NUL.
    text_off = HEADER + len(body)
    parts = fonts + [help_article + b"\0"] + demos \
        + [a + b"\0" for a in end_articles]
    body += struct.pack(">%dI" % (2 + len(parts)), len(parts),
                        len(end_articles), *[len(p) for p in parts])
    body += b"".join(parts)

    header = struct.pack(">4sHHHHIIIIIIIII",
                         b"WL64",
                         len(levels),
                         len(walls),
                         len(walls),       # sprite_start == wall page count
                         VERSIONS[EXT],
                         pal_off,
                         walls_off,
                         names_off,
                         HEADER + len(body),
                         sprites_off,
                         pics_off,
                         sounds_off, text_off, len(big))
    assert len(header) == HEADER, len(header)

    print("")
    for out, data in ((os.path.join(PROJECT, "assets", "wolf3d.dat"),
                       header + bytes(body)),
                      (os.path.join(PROJECT, "assets", "wolfbig.dat"), big)):
        changed = write_file(out, data)
        print("%s %s (%d bytes)" % ("wrote" if changed else "unchanged:",
                                    out, len(data)))


if __name__ == "__main__":
    sys.exit(main())
