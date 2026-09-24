/* The game data, linked straight into the ROM image.
 *
 * assets/wolf3d.dat is built by tools/extract_assets.py out of your copy of
 * the game - the shareware, the full game or Spear of Destiny - the palette
 * and sign-on screen included (they come out of the executable). It is
 * 1.0 to 1.4 MB and the game uses it in place, never parsing it into other
 * structures; libdragon compresses it with the rest of the program in the
 * ROM and unpacks it into RAM at boot. The maps and the full-screen
 * pictures are in assets/wolfbig.dat, in the ROM's filesystem, and read
 * when they are needed. It is id's data: see "License, and what not to
 * redistribute" in README.md.
 */
    .section .rodata
    .balign 8
    .globl  wolf3d_dat
wolf3d_dat:
    .incbin "assets/wolf3d.dat"
    .globl  wolf3d_dat_end
wolf3d_dat_end:
    .balign 8
