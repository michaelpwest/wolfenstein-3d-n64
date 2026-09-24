/* status.c - the real Wolfenstein status bar.
 *
 * STATUSBARPIC and the little pics that go on top of it come out of
 * VGAGRAPH; everything here is the DOS game's own layout, from
 * DrawFace/DrawHealth/DrawLives/DrawScore/DrawAmmo/DrawKeys/DrawWeapon in
 * WL_AGENT.C and DrawLevel in WL_GAME.C. StatusDrawPic() takes x in
 * eight-pixel columns and y in pixels from the top of the bar, so the
 * coordinates below are the originals unchanged.
 *
 * The bar is composed into statusbuf rather than drawn straight to the
 * screen: libdragon's display buffers are uncached, and the RDP blits this
 * alongside the 3D view.
 */
#include <stdio.h>
#include <string.h>
#include "wolf.h"

uint16_t statusbuf[STATUS_H][SCREEN_W] __attribute__((aligned(16)));

static int  facecount;
static int  over_face;                      /* a pic stamped over the face */
static int  facetimes;                      /* demo frames to hold the grin */
static int  funnyticount;                   /* Spear: tics without moving */

static void draw_pic(int cx, int y, int picnum)
{
    int w, h;
    const uint8_t *src = assets_pic(picnum, &w, &h);
    int x = cx * 8;

    if (!src || x < 0 || y < 0 || x + w > SCREEN_W || y + h > STATUS_H)
        return;

    for (int yy = 0; yy < h; yy++) {
        uint16_t *dst = &statusbuf[y + yy][x];
        for (int xx = 0; xx < w; xx++)
            dst[xx] = wolf_palette[src[yy * w + xx]];
    }
}

/* LatchNumber(): right justified, padded on the left with N_BLANKPIC. */
static void latch_number(int cx, int y, int width, int32_t number)
{
    char str[16];
    int length, c;

    snprintf(str, sizeof str, "%ld", (long)number);
    length = (int)strlen(str);

    while (length < width) {
        draw_pic(cx, y, PIC_BLANK);
        cx++;
        width--;
    }

    c = length <= width ? 0 : length - width;
    for (; c < length; c++, cx++)
        draw_pic(cx, y, PIC_N0 + (str[c] - '0'));
}

/* DrawFace(): three frames per health band, and a dead face - or, in
 * Spear of Destiny's god mode, its god faces. The bar is redrawn every frame
 * here, but the DOS game only drew the face when something called
 * DrawFace(), and a picture stamped over it lives between those calls: BJ's
 * grin when he picks up the gatling gun, and Spear of Destiny's wide eyes
 * at a big hit and his yawn when left standing. */
int status_face_pic(void)
{
    if (over_face)
        return over_face;
    if (gamestate.health <= 0) {
        /* the mutant face, if Dr. Schabbs's syringe did it - not in Spear of Destiny,
         * which has no such face */
        if (last_attacker_class == needleobj && wolf_version != WV_SPEAR
            && assets_has_pic(PIC_MUTANTBJ))
            return PIC_MUTANTBJ;
        return PIC_FACE + 21;                /* FACE8APIC */
    }
    if (wolf_version == WV_SPEAR && opt_god_mode && !dos_exact)
        return PIC_GODFACE1 + gamestate.faceframe;
    return PIC_FACE + 3 * ((100 - gamestate.health) / 16) + gamestate.faceframe;
}

void face_gatling(void)
{
    over_face = PIC_GOTGATLING;
    facecount = 0;
    facetimes = 38;
}

/* Spear of Destiny's TakeDamage(): "make BJ's eyes bug if major damage" */
void face_ouch(void)
{
    over_face = PIC_BJOUCH;
    facecount = 0;
}

/* Spear of Destiny's PlayLoop(): "make funny face if BJ doesn't move for
 * awhile" - thirty seconds without Thrust() moving him */
void face_funny(int tics)
{
    if (wolf_version != WV_SPEAR)
        return;
    funnyticount += tics;
    if (funnyticount > 30 * 70) {
        funnyticount = 0;
        over_face = PIC_BJWAITING1 + (us_rndt() & 1);
        facecount = 0;
    }
}

void face_moved(void)
{
    funnyticount = 0;
}

int face_funny_count(void)
{
    return funnyticount;
}

void set_face_funny_count(int count)
{
    funnyticount = count;
}

/* Anything that calls DrawFace(): damage, healing, dying, a new game. */
void face_redraw(void)
{
    over_face = 0;
}

void face_reset(void)
{
    facecount = 0;
    facetimes = 0;
    funnyticount = 0;
}

int face_count(void)
{
    return facecount;
}

void set_face_count(int count)
{
    facecount = count;
}

/* UpdateFace(): BJ glances about at an irregular rate, except while the
 * gatling-gun pickup sound plays - which, like every SD_SoundPlaying()
 * test, only sees the AdLib voice. With a digitized pickup sound the grin
 * just lasts until the next glance. */
void update_face(int tics)
{
    /* The random numbers this draws make a demo depend on how long the
     * pickup sound ran on the PC that recorded it (one reason the second
     * demo rarely played right even in DOS). During a demo the wait is a
     * fixed 38 frames instead, as Wolf4SDL settled on. */
    if (dos_exact) {
        if (facetimes > 0) {
            facetimes--;
            return;
        }
    } else if (sd_sound_playing() == GETGATLINGSND)
        return;

    facecount += tics;
    if (facecount > us_rndt()) {
        gamestate.faceframe = us_rndt() >> 6;
        if (gamestate.faceframe == 3)
            gamestate.faceframe = 1;
        facecount = 0;
        face_redraw();
    }
}

/* ------------------------------------------------------------------ */
/* Frame-rate readout - not part of the DOS game                       */
/* ------------------------------------------------------------------ */

/* 3x5 glyphs, one row per nibble, high bit on the left. */
static const uint8_t font3x5[][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
    { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    { 7, 4, 6, 4, 4 },                       /* F */
    { 7, 5, 7, 4, 4 },                       /* P */
    { 3, 4, 2, 1, 6 },                       /* S */
};

static void glyph(int x, int y, int g, uint16_t color)
{
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (font3x5[g][row] & (4 >> col))
                *view_pixel(x + col, y + row) = color;
}

void hud_draw_fps(int fps)
{
    const uint16_t yellow = 0xffc1, shadow = 0x0001;    /* RGBA5551 */
    signed char text[8];                     /* glyph indices, -1 for a gap */
    int n = 0;

    if (fps > 99) fps = 99;
    if (fps < 0)  fps = 0;
    if (fps >= 10)
        text[n++] = (signed char)(fps / 10);
    text[n++] = (signed char)(fps % 10);
    text[n++] = -1;                          /* a space */
    text[n++] = 10;                          /* F */
    text[n++] = 11;                          /* P */
    text[n++] = 12;                          /* S */

    /* A drop shadow one pixel down and right keeps it legible over a
     * bright wall as well as the dark ceiling. The inset clears the few
     * columns the VI border (and a TV's overscan) hides on the left. It
     * stays in the screen's corner whatever the view size; the border is
     * not redrawn every frame, so the part of it under the widest readout
     * ("99 FPS" and its shadow) is put back first. */
    const int left = 8, top = 6;

    if (view_x || view_y)
        view_border_restore(left, top, 6 * 4 + 1, 5 + 1);
    for (int pass = 0; pass < 2; pass++) {
        int x = left + !pass, y = top + !pass;
        for (int i = 0; i < n; i++, x += 4)
            if (text[i] >= 0)
                glyph(x, y, text[i], pass ? yellow : shadow);
    }
}

/* ------------------------------------------------------------------ */

/* Composing the bar is 12 800 palette lookups and stores, and the DOS game
 * only ever drew the parts that changed; here the whole bar is redrawn, but
 * only when something on it has changed - including the palette, which the
 * damage and bonus flashes swap - or something else has used its buffer. */
static bool status_valid;

void status_invalidate(void)
{
    status_valid = false;
}

void status_fill(int color)
{
    const uint16_t c = wolf_palette[color & 0xff];
    for (int y = 0; y < STATUS_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            statusbuf[y][x] = c;
    status_valid = false;
}

void status_draw(void)
{
    const int weapon = gamestate.weapon < 0 ? gamestate.chosenweapon
                                            : gamestate.weapon;
    const uintptr_t now[9] = {
        (uintptr_t)wolf_palette, (uintptr_t)gamestate.score,
        (uintptr_t)current_level, (uintptr_t)gamestate.lives,
        (uintptr_t)gamestate.health, (uintptr_t)gamestate.ammo,
        (uintptr_t)status_face_pic(), (uintptr_t)(gamestate.keys & 3),
        (uintptr_t)weapon
    };
    static uintptr_t drawn[9];

    if (status_valid && !memcmp(now, drawn, sizeof now))
        return;
    memcpy(drawn, now, sizeof now);
    status_valid = true;

    PROF(PROF_STATUS);
    draw_pic(0, 0, PIC_STATUSBAR);

    latch_number(2,  16, 2, level_mapon(current_level) + 1);    /* mapon+1 */
    latch_number(6,  16, 6, gamestate.score);
    latch_number(14, 16, 1, gamestate.lives);
    latch_number(21, 16, 3, gamestate.health);
    latch_number(27, 16, 2, gamestate.ammo);

    draw_pic(17, 4, status_face_pic());
    draw_pic(30, 4,  (gamestate.keys & 1) ? PIC_GOLDKEY : PIC_NOKEY);
    draw_pic(30, 20, (gamestate.keys & 2) ? PIC_SILVERKEY : PIC_NOKEY);
    /* While dying the weapon is -1; DrawWeapon() is not called then, so
     * the bar keeps showing the last one. */
    draw_pic(32, 8,  PIC_KNIFE + weapon);
    PROF(PROF_LOGIC);
}
