/* assets.c - unpack the header of the blob.
 *
 * The blob is big-endian and already laid out the way the game wants it, so
 * this is only pointer arithmetic. Layout is documented in
 * tools/extract_assets.py. Nothing here is N64-specific: the ROM passes the
 * address of the linked-in copy, tools/sim passes a file it read. What is
 * only needed now and then - the maps and the full-screen pictures - is in
 * the second file, wolfbig.dat, read through platform_read_big().
 */
#include <string.h>
#include "wolf.h"

#define LEVEL_NAME_LEN  16
#define PLANE_BYTES     (MAPSIZE * MAPSIZE * 2)

int wolf_version;

const uint16_t *wolf_palette;
const uint16_t *wolf_palettes;

/* VL_SetPalette(), in effect: everything draws through wolf_palette. */
void set_palette_shift(int which)
{
    wolf_palette = wolf_palettes + 256 * which;
}
const uint8_t  *wolf_walls;
const uint8_t  *wolf_sprites;
int             wolf_wall_pages;
int             wolf_num_levels;
int             wolf_num_sprites;

static const uint8_t  *level_names;
static const uint32_t *sprite_index;

int             wolf_num_sounds, wolf_num_digi, wolf_num_game_sounds;
const uint32_t *wolf_sound_info;
const uint32_t *wolf_sound_digi;
const uint32_t *wolf_digi_tics;
const uint32_t *wolf_level_song;
const uint32_t *wolf_pantable;
const uint32_t *wolf_sound_map;
const uint32_t *wolf_music_roles;

const uint8_t *wolf_fonts[2];
const char    *wolf_help_article;
const char    *wolf_end_articles[6];
int            wolf_num_end_articles;
const uint8_t *wolf_demos[4];

static const uint8_t  *pics_base;
static const uint32_t *pic_dims;            /* (width << 16) | height */
static const uint32_t *pic_offs;            /* top bit: in wolfbig.dat */
static const uint8_t  *pic_pal;             /* 0 the game's, else n: words */
static int             num_pics;
static int             num_fixed_pics;      /* the rest are the articles' */

/* The palettes some pics have of their own (Spear of Destiny's title and
 * end screens), as the renderer wants them */
#define MAX_PIC_PALETTES 12
static uint16_t pic_palettes[MAX_PIC_PALETTES][256];
static int      num_pic_palettes;

static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

bool assets_init(const uint8_t *blob)
{
    if (memcmp(blob, "WL64", 4) != 0)
        return false;

    wolf_num_levels = be16(blob + 4);
    wolf_wall_pages = be16(blob + 6);
    wolf_version    = be16(blob + 10);
    wolf_palettes   = (const uint16_t *)(blob + be32(blob + 12));
    wolf_palette    = wolf_palettes;
    wolf_walls      = blob + be32(blob + 16);
    level_names     = blob + be32(blob + 20);
    wolf_sprites    = blob + be32(blob + 28);

    /* Sprite section: count, posts offset, pixels offset, pad, then one
     * record offset per sprite (0 for one this data lacks) - all relative
     * to wolf_sprites. */
    wolf_num_sprites = (int)be32(wolf_sprites);
    sprite_index     = (const uint32_t *)(wolf_sprites + 16);

    /* Pic section: count, pixels offset, how many palettes of their own,
     * how many are the fixed list (0 in data from before PIC_MUTANTBJ,
     * which ends a pic sooner), then a dimensions word, a pixel offset and a palette per
     * pic, then those palettes. Same 32-bit-fields-then-pixels shape as the
     * sprites, for the same byte-swapping reason - and read with be32()
     * here, so as to come out the same before tools/sim swaps them. */
    pics_base = blob + be32(blob + 32);
    num_pics  = (int)be32(pics_base);
    num_fixed_pics = (int)be32(pics_base + 12);
    if (num_fixed_pics == 0)                /* extracted before PIC_MUTANTBJ */
        num_fixed_pics = PIC_MUTANTBJ;
    if (num_fixed_pics > num_pics)
        return false;
    pic_dims  = (const uint32_t *)(pics_base + 16);
    pic_offs  = pic_dims + num_pics;
    pic_pal   = (const uint8_t *)(pic_offs + num_pics);
    num_pic_palettes = (int)be32(pics_base + 8);
    if (num_pic_palettes > MAX_PIC_PALETTES)
        return false;
    for (int p = 0; p < num_pic_palettes; p++)
        for (int i = 0; i < 256; i++)
            pic_palettes[p][i] = (uint16_t)be32(pic_pal + 4 * (num_pics + p * 256 + i));

    /* Sound section: its size in words and the counts - the data's sounds,
     * recordings, floors, the game's sounds, the song roles - then the
     * per-sound info and digitized map, digitized lengths, the song for
     * each floor, the two stereo tables, the game's sound numbers in this
     * data, and the songs by role - all 32-bit words. */
    {
        const uint8_t *snd = blob + be32(blob + 36);
        const uint32_t *w  = (const uint32_t *)(snd + 32);
        int nsongs  = (int)be32(snd + 12);

        wolf_num_sounds      = (int)be32(snd + 4);
        wolf_num_digi        = (int)be32(snd + 8);
        wolf_num_game_sounds = (int)be32(snd + 16);
        wolf_sound_info  = w;  w += wolf_num_sounds;
        wolf_sound_digi  = w;  w += wolf_num_sounds;
        wolf_digi_tics   = w;  w += wolf_num_digi;
        wolf_level_song  = w;  w += nsongs;
        wolf_pantable    = w;  w += 2 * 15 * 30;
        wolf_sound_map   = w;  w += wolf_num_game_sounds;
        wolf_music_roles = w;

        if (nsongs < wolf_num_levels || (int)be32(snd + 20) < MUSROLES)
            return false;
    }

    /* Text section: how many parts and how many of them are end articles,
     * each part's length, then the parts - the two fonts, "Read This!",
     * the four demos, the end articles - read as bytes so nothing here
     * needs swapping on the host. */
    {
        const uint8_t *text = blob + be32(blob + 40);
        int parts = (int)be32(text), ends = (int)be32(text + 4);
        const uint8_t *len = text + 8;
        const uint8_t *p   = len + 4 * parts;
        if (text == blob || parts != 7 + ends || ends > 6)
            return false;
        wolf_fonts[0]     = p;  p += be32(len);
        wolf_fonts[1]     = p;  p += be32(len + 4);
        wolf_help_article = (const char *)p;  p += be32(len + 8);
        for (int i = 0; i < 4; i++) {
            wolf_demos[i] = p;
            p += be32(len + 12 + 4 * i);
        }
        wolf_num_end_articles = ends;
        for (int i = 0; i < ends; i++) {
            wolf_end_articles[i] = (const char *)p;
            p += be32(len + 28 + 4 * i);
        }
    }

    return wolf_wall_pages >= 8 && wolf_num_levels > 0
        && wolf_num_sprites > 0 && num_fixed_pics >= PIC_MUTANTBJ
        && wolf_num_game_sounds >= NUM_GAME_SOUNDS;
}

/* A big pic is read into one of two buffers when it is asked for, and
 * kept there until the other is needed: the screens that show them - the
 * sign-on, the title and credits - draw them over and over. */
#define BIG_SLOTS  2
#define BIG_BYTES  (320 * 200)

static uint8_t big_buf[BIG_SLOTS][BIG_BYTES];
static int     big_pic[BIG_SLOTS] = { -1, -1 };
static int     big_next;

bool assets_has_pic(int n)
{
    return n >= 0 && n < num_fixed_pics && pic_dims[n] != 0;
}

const uint8_t *assets_pic(int n, int *w, int *h)
{
    uint32_t off;

    if (n < 0 || n >= num_pics) {
        *w = *h = 0;
        return 0;
    }
    *w = (int)(pic_dims[n] >> 16);
    *h = (int)(pic_dims[n] & 0xffff);
    off = pic_offs[n];
    if (!(off & 0x80000000u))
        return pics_base + off;

    for (int s = 0; s < BIG_SLOTS; s++)
        if (big_pic[s] == n)
            return big_buf[s];
    if (*w * *h > BIG_BYTES) {
        *w = *h = 0;
        return 0;
    }
    {
        const int s = big_next;
        big_next = (big_next + 1) % BIG_SLOTS;
        platform_read_big(off & 0x7fffffffu, big_buf[s], (uint32_t)(*w * *h));
        big_pic[s] = n;
        return big_buf[s];
    }
}

const uint16_t *assets_pic_palette(int n)
{
    int p;

    if (n < 0 || n >= num_pics)
        return NULL;
    /* a word per pic, read a byte at a time: tools/sim has swapped it */
    p = (int)pic_pal[4 * n + 3] | pic_pal[4 * n];
    return p >= 1 && p <= num_pic_palettes ? pic_palettes[p - 1] : NULL;
}

const uint32_t *assets_sprite(int n)
{
    if (n < 0 || n >= wolf_num_sprites || !sprite_index[n])
        return 0;
    return (const uint32_t *)(wolf_sprites + sprite_index[n]);
}

/* A floor's two planes, from wolfbig.dat: 16 KB a floor, big-endian words
 * read straight into the planes and put in the host's order there. */
void assets_load_level(int level, uint16_t *p0, uint16_t *p1)
{
    uint16_t *planes[2] = { p0, p1 };

    for (int p = 0; p < 2; p++) {
        uint8_t *bytes = (uint8_t *)planes[p];
        platform_read_big((uint32_t)(level * 2 + p) * PLANE_BYTES, bytes,
                          PLANE_BYTES);
        for (int i = 0; i < MAPSIZE * MAPSIZE; i++)
            planes[p][i] = be16(bytes + 2 * i);
    }
}

const char *assets_level_name(int level)
{
    return (const char *)(level_names + level * LEVEL_NAME_LEN);
}
