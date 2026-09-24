"""Unpack a DOS executable compressed with LZEXE 0.91.

id shipped WOLF3D.EXE squeezed with Fabrice Bellard's LZEXE, and two pieces
of the game's artwork live only inside it: the VGA palette and the sign-on
screen. tools/extract_assets.py reads them out of the unpacked load module,
so a build needs nothing but the player's own copy of the game.

The format, as UNLZEXE (Mitugu Kurizono) documents it:

  - The MZ header says "LZ91" at 0x1C (though some copies had that mark
    wiped, so the decompressor's own opening bytes are checked too). CS:IP
    points at the decompressor,
    and CS:0 holds eight words: the real IP, CS, SP and SS, the compressed
    load module's size in paragraphs, how much the module grows, and the
    decompressor's size and checksum.
  - The compressed module sits just below the decompressor, from paragraph
    (header + CS - compressed size). It is one stream of 16-bit little-endian
    words of flag bits, taken low bit first, with the literal and offset
    bytes interleaved between them in the order they are needed:

      1                  a literal byte follows
      0 0 b b  byte      copy b+2 bytes from -256..-1 back (the byte - 256)
      0 1  lo hi         copy (hi & 7) + 2 bytes from a 13-bit distance,
                         lo | (hi & 0xF8) << 5, minus 8192; if hi & 7 is 0
                         the length is the next byte + 1 instead, where a
                         byte of 0 ends the stream and 1 only marks a
                         segment boundary

The relocation table that follows is not needed to read data out of the
image, so it is left alone.

    python tools/lzexe.py WOLF3D.EXE out.bin
"""

import struct
import sys


class NotLzexe(Exception):
    pass


# The first bytes of LZEXE 0.91's decompressor, at CS:IP. Some executables
# were shipped with the "LZ91" mark wiped - Spear of Destiny's among them -
# so the code itself is recognized too.
STUB_091 = bytes.fromhex("060e1f8b0e0c008bf14e89f78cdb031e0a008ec3fdf3a4")


def unpack(exe):
    """The load module of an LZEXE 0.91 executable, as bytes."""
    if len(exe) < 0x20 or exe[:2] != b"MZ":
        raise NotLzexe("not a DOS executable")
    header = struct.unpack_from("<16H", exe, 0)
    header_paras, ip, cs = header[4], header[10], header[11]
    entry = (header_paras + cs) * 16 + ip
    if exe[0x1C:0x20] != b"LZ91" \
            and exe[entry:entry + len(STUB_091)] != STUB_091:
        raise NotLzexe("not compressed with LZEXE 0.91")
    info = struct.unpack_from("<8H", exe, (header_paras + cs) * 16)
    pos = (header_paras + cs - info[4]) * 16
    out = bytearray()

    def byte():
        nonlocal pos
        if pos >= len(exe):
            raise NotLzexe("compressed data runs past the end of the file")
        value = exe[pos]
        pos += 1
        return value

    def word():
        return byte() | byte() << 8

    flags, left = word(), 16

    def bit():
        # the next flag word is read the moment the last bit is used, before
        # any byte that follows - the stream depends on that order
        nonlocal flags, left
        b = flags & 1
        left -= 1
        if left == 0:
            flags, left = word(), 16
        else:
            flags >>= 1
        return b

    while True:
        if bit():
            out.append(byte())
            continue
        if not bit():
            length = (bit() << 1 | bit()) + 2
            distance = byte() - 256
        else:
            lo, hi = byte(), byte()
            distance = (lo | (hi & 0xF8) << 5) - 8192
            length = (hi & 7) + 2
            if length == 2:
                length = byte()
                if length == 0:
                    break                   # the end of the module
                if length == 1:
                    continue                # a segment boundary
                length += 1
        start = len(out) + distance
        if start < 0:
            raise NotLzexe("a copy reaches back before the start")
        for i in range(length):             # may overlap what it writes
            out.append(out[start + i])
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: lzexe.py IN.EXE OUT.BIN")
    with open(sys.argv[1], "rb") as f:
        image = unpack(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(image)
    print("%d bytes" % len(image))
