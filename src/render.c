/* render.c - the ray caster.
 *
 * AsmRefresh() in WL_DR_A.ASM walks the grid with tangent tables; this does
 * the same walk as a fixed-point DDA, which is easier to follow and lands on
 * the same intercepts. Everything that decides what a column *looks* like -
 * which of the two textures a wall face uses, which way the texture runs,
 * where a door's texture starts, when a jamb shows the door-side art - is
 * the logic from HitVertWall/HitHorizWall/HitVertDoor/HitHorizDoor.
 *
 * Rays are not unit length: they are the view vector plus the camera plane
 * times the column's offset, so the distance the DDA accumulates is already
 * the perpendicular distance and there is no fisheye to divide out.
 */
#include <string.h>
#include "wolf.h"

/* tan(half the horizontal field of view). CalcProjection() in WL_MAIN.C:
 * halfview * VIEWGLOBAL / viewwidth / (FOCALLENGTH + MINDIST)
 * = 160 * 65536 / 320 / 0xaf00, about 0.7314 - a 72.4 degree view. */
#define PLANE_LEN   47935

/* FOCALLENGTH in WL_MAIN.C. ThreeDRefresh() puts the eye this far behind the
 * player's position, and draws, tests visibility and grabs pickups from
 * there - so a wall the player is touching is still 0xaf00 away. */
#define FOCALLENGTH 0x5700

/* The view, as Change View sizes it (see set_view_size()), and what
 * CalcProjection() works out from its width: scale, halfview * facedist /
 * (VIEWGLOBAL/2) truncated to a long, and centerx, viewwidth/2 - 1, which
 * TransformActor() places sprites with and GunAttack() aims by; the pixel
 * height of a wall a tile away, the scale times the 1.2 square pixels need;
 * and the hands, SimpleScaleShape()'s viewheight+1 VGA pixels. The initial
 * values are the full 320x200 view's. */
int  view_size = VIEW_SIZE_MAX;
int  view_width = VIEW_W, view_height = VIEW_H, view_x, view_y;
bool view_border_dirty = true;

static int proj_scale  = 218;
static int proj_center = VIEW_W / 2 - 1;
static int scale_px    = SCALE_PX;
static int weapon_w    = 161, weapon_h = 193;

#define PROJ_SCALE   proj_scale
#define PROJ_CENTER  proj_center

/* The same for the 304-pixel view (view size 19) the demos are checked
 * against. GunAttack() aims by where an actor lands in the view, so during
 * a demo ob->viewx is worked out for that width, while sprites are still
 * drawn across all 320 columns. */
#define DEMO_VIEW_W       304
#define DEMO_PROJ_SCALE   207               /* 152 * 0xaf00 / 0x8000   */
#define DEMO_PROJ_CENTER  (DEMO_VIEW_W / 2 - 1)

bool dos_exact;
void (*prof_mark)(int section);

/* A ray that is almost parallel to one axis would want an unbounded step
 * along it. 2^26 is 1024 tiles: further than the map is wide, so the axis
 * is simply never stepped, and the accumulator still cannot overflow. */
#define DDA_FAR     (1 << 26)

#define MAX_STEPS   192

uint16_t viewbuf[VIEW_H][VIEW_STRIDE] __attribute__((aligned(16)));
uint16_t colbuf[VIEW_W][VIEW_H] __attribute__((aligned(16)));
bool     view_columns;

void view_to_rows(void)
{
    if (!view_columns)
        return;
    for (int x = 0; x < VIEW_W; x++) {
        const uint16_t *src = colbuf[x];
        for (int y = 0; y < VIEW_H; y++)
            viewbuf[y][x] = src[y];
    }
    view_columns = false;
}

fixed sintab[FINEANGLES + ANG90];

uint8_t spotvis[MAPSIZE][MAPSIZE];

/* Perpendicular distance to the wall in each column - the DOS renderer's
 * wallheight[], which DrawScaleds() uses the same way to decide whether a
 * sprite column is in front of the wall behind it. */
static fixed walldist[VIEW_W];

/* Scenery in view this frame, sorted far to near before drawing. */
typedef struct {
    fixed depth;                /* perpendicular distance      */
    int   centerx;              /* screen column of its middle */
    int   shapenum;
} visobj_t;

static visobj_t vis[MAXSTATS + MAXACTORS];
static int      numvis;

int32_t dos_sintable[360 + 90 + 1];

/* Tables for the demo visibility pass below: finetangent[] and a 304-pixel
 * view's pixelangle[], built as CalcProjection() and BuildTables() do. */
#define DEMO_ANG90   900
static int32_t finetangent[DEMO_ANG90];
static int16_t pixelangle[DEMO_VIEW_W];

void build_tables(void)
{
    /* BuildTables() computes these with sin(); the N64 has an FPU and this
     * runs once, so there is no reason to do it any other way. */
    for (int i = 0; i < FINEANGLES + ANG90; i++) {
        double a = (double)i * (2.0 * 3.14159265358979323846 / FINEANGLES);
        sintab[i] = (fixed)(__builtin_sin(a) * 65536.0);
    }

    /* The DOS game's own table, for demo playback: whole degrees, built with
     * its single-precision angle stepping and its PI. Kept as ordinary
     * signed numbers, with exactly one at 90 and 270 degrees - the way
     * Wolf4SDL holds it, whose demo playback is known to stay in sync. */
    {
        const double PI_DOS = 3.141592657;
        float angle = 0, anglestep = (float)(PI_DOS / 2 / 90);
        for (int i = 0; i < 90; i++) {
            const int32_t value = (int32_t)(65536L * __builtin_sin(angle));
            dos_sintable[i] = dos_sintable[i + 360] = dos_sintable[180 - i] = value;
            dos_sintable[360 - i] = dos_sintable[180 + i] = -value;
            angle += anglestep;
        }
        dos_sintable[90]  = dos_sintable[450] = 65536;
        dos_sintable[270] = -65536;
    }

    {
        const double PI_DOS = 3.141592657;
        const float radtoint = (float)(3600 / 2 / PI_DOS);
        const double facedist = FOCALLENGTH + MINDIST;

        for (int i = 0; i < DEMO_ANG90 / 2; i++) {
            const double tang = __builtin_tan((i + 0.5) / radtoint);
            finetangent[i] = (int32_t)(tang * 65536);
            finetangent[DEMO_ANG90 - 1 - i] = (int32_t)((1 / tang) * 65536);
        }
        for (int i = 0; i < DEMO_VIEW_W / 2; i++) {
            const double tang = (int32_t)i * 0x10000 / DEMO_VIEW_W / facedist;
            const float angle = (float)__builtin_atan(tang);
            const int intang = (int)(angle * radtoint);
            pixelangle[DEMO_VIEW_W / 2 - 1 - i] = (int16_t)intang;
            pixelangle[DEMO_VIEW_W / 2 + i] = (int16_t)-intang;
        }
    }
}

/* FixedByFrac() from WL_DRAW.C, as Thrust() uses it: the magnitude of `a`
 * times the fraction's low word, rounded toward zero, with the sign put
 * back. A whole 1.0 counts as 0xffff, as the sign-magnitude table had it. */
fixed fixed_by_frac(fixed a, int32_t b)
{
    bool negative = false;
    int64_t result;

    if (b == 65536)
        b = 65535;
    else if (b == -65536)
        b = 65535, negative = true;
    else if (b < 0)
        b = -b, negative = true;
    if (a < 0) {
        a = -a;
        negative = !negative;
    }
    result = ((int64_t)a * b) >> 16;
    return (fixed)(negative ? -result : result);
}

/* The view's cosine and sine for this frame, and multiplication by them:
 * the port's fine table normally; while a demo plays, the whole-degree
 * table and a rounded multiply, as the demo-synchronized reference does. */
static fixed view_cos, view_sin;
static int32_t view_dcos, view_dsin;

static fixed mul_round(fixed a, int32_t b)
{
    return (fixed)(((int64_t)a * b + 0x8000) >> 16);
}

static fixed mul_cos(fixed a)
{
    return dos_exact ? mul_round(a, view_dcos) : fixmul(a, view_cos);
}

static fixed mul_sin(fixed a)
{
    return dos_exact ? mul_round(a, view_dsin) : fixmul(a, view_sin);
}

/* ------------------------------------------------------------------ */

/* One column of the view, top to bottom: ceiling, a 64-pixel texture column
 * scaled to `height`, floor. VGAClearScreen() filled the whole view with the
 * two colors and the walls then covered most of it; the same pixels come
 * out of writing each one once. A height of 0 is a ray that hit nothing,
 * ceiling and floor meeting in the middle. */
static void draw_column(int x, const uint8_t *tex, int height,
                        uint16_t ceiling_px, uint16_t floor_px)
{
    const uint16_t *palette = wolf_palette;
    const int vh = view_height;
    const int top = (vh - height) >> 1;
    int y0 = top < 0 ? 0 : top;
    int y1 = top + height;
    uint16_t *dst = &colbuf[view_x + x][view_y];
    int y = 0;

    if (y1 > vh)
        y1 = vh;
    if (height <= 0)
        y0 = y1 = vh / 2;

    for (; y < y0; y++)
        *dst++ = ceiling_px;

    if (y0 < y1) {
        const uint32_t step = (64u << 16) / (uint32_t)height;
        uint32_t texpos = (uint32_t)(y0 - top) * step;
        for (; y < y1; y++) {
            *dst++ = palette[tex[(texpos >> 16) & 63]];
            texpos += step;
        }
    }

    for (; y < vh; y++)
        *dst++ = floor_px;
}

/* ------------------------------------------------------------------ */
/* Scenery                                                             */
/* ------------------------------------------------------------------ */

/* One column of a sprite, drawn from its vertical runs so the gaps cost
 * nothing. `top` and `height` are the sprite's full 64-pixel extent, the
 * same as a wall at this distance. */
static void draw_sprite_column(int x, const uint32_t *rec, const uint8_t *pix,
                               int tx, int top, int height)
{
    const uint16_t *palette = wolf_palette;
    uint32_t colword = rec[1 + tx];
    const uint32_t *run;
    uint32_t step;
    int n;

    if (!colword)
        return;                              /* transparent all the way down */

    n    = (int)(colword >> 24);             /* number of runs in this column */
    run  = (const uint32_t *)(wolf_sprites + (colword & 0xffffff));
    step = (64u << 16) / (uint32_t)height;

    while (n--) {
        uint32_t r = *run++;
        int starty = SPRITE_RUN_START(r);
        int len    = SPRITE_RUN_LEN(r);
        const uint8_t *src = pix + SPRITE_RUN_PIX(r);
        int y0 = top + starty * height / 64;
        int y1 = top + (starty + len) * height / 64;
        uint16_t *dst;

        if (y0 < 0)           y0 = 0;
        if (y1 > view_height) y1 = view_height;
        if (y0 >= y1)
            continue;                        /* the run is off the view */
        dst = &colbuf[view_x + x][view_y + y0];

        for (int y = y0; y < y1; y++) {
            int row = (int)(((uint32_t)(y - top) * step) >> 16) - starty;
            if (row < 0)    row = 0;
            if (row >= len) row = len - 1;
            *dst++ = palette[src[row]];
        }
    }
}

static void draw_scaleds(void)
{
    /* Insertion sort, far to near, the way DrawScaleds() orders by height.
     * A level never has more than a few dozen pieces of scenery in view. */
    for (int i = 1; i < numvis; i++) {
        int j = i;
        while (j > 0 && vis[j - 1].depth < vis[j].depth) {
            visobj_t t = vis[j - 1];
            vis[j - 1] = vis[j];
            vis[j] = t;
            j--;
        }
    }

    for (int i = 0; i < numvis; i++) {
        fixed depth = vis[i].depth;
        /* Sprites are 64x64 like wall faces, so they scale the same way:
         * the same pixel height, and a width that is narrower by the 1.2
         * the vertical scale carries for VGA's tall pixels. */
        int height = (scale_px << 16) / depth;
        int width  = (int)(((int64_t)(view_width / 2) << 16) / fixmul(depth, PLANE_LEN));
        int x0, x1, top;
        uint32_t txstep, txpos;
        const uint32_t *rec;
        const uint8_t *pix;

        if (height <= 0 || width <= 0)
            continue;

        x0  = vis[i].centerx - width / 2;
        x1  = x0 + width;
        top = (view_height - height) >> 1;

        rec = assets_sprite(vis[i].shapenum);
        if (!rec)
            continue;
        pix = wolf_sprites + rec[0];

        txstep = (64u << 16) / (uint32_t)width;
        txpos  = 0;
        if (x0 < 0) {
            txpos = (uint32_t)(-x0) * txstep;
            x0 = 0;
        }
        if (x1 > view_width)
            x1 = view_width;

        for (int x = x0; x < x1; x++, txpos += txstep) {
            if (depth >= walldist[x])
                continue;                    /* behind the wall here */
            draw_sprite_column(x, rec, pix, (txpos >> 16) & 63, top, height);
        }
    }
}

/* DrawPlayerWeapon(): SimpleScaleShape(viewwidth/2, shape, viewheight+1),
 * the 64x64 sprite scaled to viewheight+1 VGA pixels each way and standing
 * on the bottom of the view. In the full view that is 161 VGA columns, 161
 * columns here, and 161 VGA rows, 1.2x that, 193 rows. No wall test - the
 * hands are always in front. */
#define WEAPON_W  weapon_w
#define WEAPON_H  weapon_h

static void draw_player_weapon(void)
{
    const uint32_t *rec;
    const uint8_t *pix;
    int x0 = view_width / 2 - WEAPON_W / 2;
    int top = view_height - WEAPON_H;
    uint32_t txstep = (64u << 16) / WEAPON_W, txpos = 0;

    if (gamestate.weapon < 0)
        return;                              /* Died() takes it away */
    if (gamestate.victoryflag)
        return;                              /* the camera is watching BJ */
    rec = assets_sprite(SPR_KNIFEREADY + gamestate.weapon * WEAPON_FRAMES
                        + gamestate.weaponframe);
    if (!rec)
        return;
    pix = wolf_sprites + rec[0];

    for (int x = x0; x < x0 + WEAPON_W; x++, txpos += txstep)
        draw_sprite_column(x, rec, pix, (txpos >> 16) & 63, top, WEAPON_H);

    /* "DEMO" over the hands while a recording plays */
    if (dos_exact && (rec = assets_sprite(SPR_DEMO)) != NULL) {
        pix = wolf_sprites + rec[0];
        txpos = 0;
        for (int x = x0; x < x0 + WEAPON_W; x++, txpos += txstep)
            draw_sprite_column(x, rec, pix, (txpos >> 16) & 63, top, WEAPON_H);
    }
}

/* Project every piece of scenery the ray caster marked as visible, from
 * the eye at posx,posy: TransformTile() and TransformActor(). */
static void collect_scaleds(fixed posx, fixed posy)
{
    numvis = 0;

    for (int i = 0; i < numstats && numvis < MAXSTATS; i++) {
        statobj_t *s = &statobjlist[i];
        fixed relx, rely, depth, lateral;

        if (s->shapenum < 0)
            continue;                        /* already picked up */
        if (!spotvis[s->tiley][s->tilex])
            continue;

        relx = (((fixed)s->tilex << TILESHIFT) + TILEGLOBAL / 2) - posx;
        rely = (((fixed)s->tiley << TILESHIFT) + TILEGLOBAL / 2) - posy;

        /* TransformTile(): depth along the view axis, less 0x2000 for the
         * object's own size, and the distance to the right of it. */
        depth = mul_cos(relx) - mul_sin(rely) - 0x2000;
        lateral = mul_cos(rely) + mul_sin(relx);

        /* "See if it should be grabbed": within a tile ahead and half a
         * tile either side. DrawScaleds() does this here rather than in the
         * player code, and an item taken this frame is not drawn. The test
         * comes before the too-close one, even though the released
         * TransformTile() returns first: the shipped game grabbed items
         * that close - an item underfoot - and the recorded demos depend
         * on it. */
        if (depth < TILEGLOBAL
            && lateral > -TILEGLOBAL / 2 && lateral < TILEGLOBAL / 2
            && s->itemnumber > block) {
            get_bonus(s);
            continue;
        }

        if (depth < MINDIST)
            continue;                        /* behind the eye, or too close */

        vis[numvis].depth    = depth;
        vis[numvis].centerx  = PROJ_CENTER
                             + (int)((int64_t)lateral * PROJ_SCALE / depth);
        vis[numvis].shapenum = s->shapenum;
        numvis++;
    }

    /* Actors: DrawScaleds() counts one as visible if a ray reached its
     * tile, or any of the eight around it that is not solid - an actor
     * walking between tiles can show in the next one before its tilex/tiley
     * catches up. Seeing one wakes it (active) and marks it FL_VISABLE, which
     * is what GunAttack() aims at and what makes enemy fire easier to dodge. */
    for (int i = 0; i < numobjs; i++) {
        objtype *ob = actor_at(i);
        fixed relx, rely, nx, ny;
        bool seen = false;

        if (!ob->state->shapenum)
            continue;

        for (int dy = -1; dy <= 1 && !seen; dy++)
            for (int dx = -1; dx <= 1 && !seen; dx++) {
                int tx = ob->tilex + dx, ty = ob->tiley + dy;
                if (tx < 0 || ty < 0 || tx >= MAPSIZE || ty >= MAPSIZE)
                    continue;
                /* DrawScaleds() skips a neighbor that is a wall, which the
                 * port's ray caster marks; the demo pass never marks walls,
                 * and there a door tile it looked through counts too */
                if (spotvis[ty][tx] && (!(dx | dy) || !tilemap[ty][tx] || dos_exact))
                    seen = true;
            }
        if (!seen) {
            ob->flags &= ~FL_VISABLE;
            continue;
        }

        ob->active = true;

        /* TransformActor(): ACTORSIZE fudges the shape forward so that
         * half of it cannot poke into the wall behind. */
        relx = ob->x - posx;
        rely = ob->y - posy;
        nx = mul_cos(relx) - mul_sin(rely) - 0x4000;
        ny = mul_cos(rely) + mul_sin(relx);
        ob->transx = nx;

        if (nx < MINDIST) {
            ob->viewheight = 0;              /* too close to draw */
            continue;
        }
        ob->viewx = PROJ_CENTER + (int)((int64_t)ny * PROJ_SCALE / nx);
        if (dos_exact)
            ob->aimx = DEMO_PROJ_CENTER + (int)((int64_t)ny * DEMO_PROJ_SCALE / nx);
        else
            ob->aimx = ob->viewx;
        ob->viewheight = (scale_px << 16) / nx;

        if (numvis < MAXSTATS + MAXACTORS) {
            vis[numvis].depth    = nx;
            vis[numvis].centerx  = ob->viewx;
            vis[numvis].shapenum = actor_shape(ob);
            numvis++;
        }
        ob->flags |= FL_VISABLE;
    }
}

/* ------------------------------------------------------------------ */
/* FizzleFade: the view dissolves to red as Died() uses it, or into a  */
/* new frame as each floor starts                                      */
/* ------------------------------------------------------------------ */

/* ID_VH.C walks every pixel once in a random-looking order with a 17-bit
 * maximal LFSR (taps 0x12000): the low 8 bits are a row, the next 9 a
 * column, and anything off the view is skipped. The caller says how many
 * frames the 131071 steps are spread over - 70 for a death, 20 for a floor
 * fizzling in. */
static uint32_t fizzle_rnd;
static uint16_t fizzlebuf[VIEW_H][VIEW_STRIDE];     /* the frame to reveal */
static bool     fizzle_whole;               /* the border too, not the view */

void fizzle_start(void)
{
    view_to_rows();                         /* the dissolve works on rows */
    fizzle_rnd = 1;
    fizzle_whole = false;
}

/* The death cam's FizzleFade() of all 320x160 above the status bar, the
 * border with the view - here all 320x200 of it. */
void fizzle_start_whole(void)
{
    fizzle_start();
    fizzle_whole = true;
}

/* ThreeDRefresh() with fizzlein set: the frame is drawn off screen, and the
 * screen keeps what it had until the dissolve copies it across. */
void fizzle_in_start(void)
{
    static uint16_t old[VIEW_H][VIEW_STRIDE];

    view_to_rows();
    memcpy(old, viewbuf, sizeof old);
    render_view();
    view_to_rows();
    memcpy(fizzlebuf, viewbuf, sizeof fizzlebuf);
    memcpy(viewbuf, old, sizeof old);
    fizzle_rnd = 1;
    fizzle_whole = false;
}

static bool fizzle(int tics, int frames, const uint16_t (*src)[VIEW_STRIDE],
                   uint16_t color)
{
    int count = (int)(131071L * tics / frames) + 1;
    /* FizzleFade() covers the view, not the border round it - but for the
     * death cam's */
    const uint32_t x0 = fizzle_whole ? 0 : (uint32_t)view_x;
    const uint32_t y0 = fizzle_whole ? 0 : (uint32_t)view_y;
    const uint32_t w  = fizzle_whole ? VIEW_W : (uint32_t)view_width;
    const uint32_t h  = fizzle_whole ? VIEW_H : (uint32_t)view_height;

    while (count--) {
        uint32_t y = fizzle_rnd & 0xff;
        uint32_t x = (fizzle_rnd >> 8) & 0x1ff;
        uint32_t lsb = fizzle_rnd & 1;

        fizzle_rnd >>= 1;
        if (lsb)
            fizzle_rnd ^= 0x00012000;
        if (x < w && y < h)
            viewbuf[y0 + y][x0 + x] = src ? src[y0 + y][x0 + x] : color;

        if (fizzle_rnd == 1) {
            /* the one pixel the sequence cannot reach, and anything else */
            for (y = y0; y < y0 + h; y++)
                for (x = x0; x < x0 + w; x++)
                    viewbuf[y][x] = src ? src[y][x] : color;
            return true;
        }
    }
    return false;
}

bool fizzle_step(int tics, int frames, uint16_t color)
{
    return fizzle(tics, frames, NULL, color);
}

bool fizzle_in_step(int tics, int frames)
{
    return fizzle(tics, frames, (const uint16_t (*)[VIEW_STRIDE])fizzlebuf, 0);
}

/* ------------------------------------------------------------------ */
/* Pics and bars for the screens between floors                        */
/* ------------------------------------------------------------------ */

void view_pic(int x, int y, int picnum)
{
    int w, h;
    const uint8_t *src = assets_pic(picnum, &w, &h);
    /* a picture with a palette of its own (Spear of Destiny's title and end
     * screens) shows as VL_FadeIn() with that palette showed it */
    const uint16_t *palette = assets_pic_palette(picnum);

    if (!palette)
        palette = wolf_palette;
    x &= ~7;
    if (!src)
        return;
    view_border_dirty |= view_columns;      /* it may cover the border */
    for (int yy = 0; yy < h; yy++) {
        if (y + yy < 0 || y + yy >= VIEW_H)
            continue;
        for (int xx = 0; xx < w; xx++)
            if (x + xx >= 0 && x + xx < VIEW_W)
                *view_pixel(x + xx, y + yy) = palette[src[yy * w + xx]];
    }
}

static void fill(int x, int y, int w, int h, int color)
{
    const uint16_t c = wolf_palette[color & 0xff];

    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < VIEW_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < VIEW_W; xx++)
            *view_pixel(xx, yy) = c;
}

void view_bar(int x, int y, int w, int h, int color)
{
    view_border_dirty |= view_columns;
    fill(x, y, w, h, color);
}

/* ------------------------------------------------------------------ */
/* The view's size - SetViewSize(), NewViewSize() and DrawPlayBorder() */
/* ------------------------------------------------------------------ */

#define VIEWCOLOR  127

/* DOS views are 16n pixels wide and 8n VGA rows tall, in the 160 rows above
 * the status bar. This view area is 200 rows, so the port's views keep its
 * own shape instead: 16n wide and 10n tall, and size 20 fills it. */
void set_view_size(int size)
{
    if (size < VIEW_SIZE_MIN) size = VIEW_SIZE_MIN;
    if (size > VIEW_SIZE_MAX) size = VIEW_SIZE_MAX;

    view_size   = size;
    view_width  = size * 16;
    view_height = size * 10;
    view_x      = (VIEW_W - view_width) / 2;
    view_y      = (VIEW_H - view_height) / 2;

    /* CalcProjection(): everything scales with the half width */
    proj_center = view_width / 2 - 1;
    proj_scale  = (view_width / 2) * 0xaf00 / 0x8000;
    scale_px    = (view_width / 2) * 0xaf00 * 6 / (0x8000 * 5);
    weapon_w    = view_width / 2 + 1;
    weapon_h    = weapon_w * 6 / 5;

    view_border_dirty = true;
}

/* DrawPlayBorder() for a view of `size`: gray all round and a bevel - dark
 * above and left, light below and right - and the view itself black, unless
 * it is about to be drawn anyway. Only the strips round the view are filled,
 * so the full view has no border to draw. */
static const uint16_t *border_palette;     /* what the border was drawn in */

static void draw_border(int size, bool clear_view)
{
    const int w = size * 16, h = size * 10;
    const int xl = (VIEW_W - w) / 2, yl = (VIEW_H - h) / 2;

    border_palette = wolf_palette;
    fill(0, 0, VIEW_W, yl, VIEWCOLOR);
    fill(0, yl + h, VIEW_W, VIEW_H - yl - h, VIEWCOLOR);
    fill(0, yl, xl, h, VIEWCOLOR);
    fill(xl + w, yl, VIEW_W - xl - w, h, VIEWCOLOR);
    if (clear_view)
        fill(xl, yl, w, h, 0);
    fill(xl - 1, yl - 1, w + 2, 1, 0);          /* VWB_Hlin(xl-1, xl+w, yl-1, 0) */
    fill(xl - 1, yl + h, w + 2, 1, 125);
    fill(xl - 1, yl - 1, 1, h + 2, 0);          /* VWB_Vlin(yl-1, yl+h, xl-1, 0) */
    fill(xl + w, yl - 1, 1, h + 2, 125);
    fill(xl - 1, yl + h, 1, 1, 124);            /* VWB_Plot(xl-1, yl+h, 124) */
}

void draw_play_border(int size)
{
    draw_border(size, true);
}

/* Put the border back under a rectangle, pixel by pixel as draw_border()
 * leaves it, in the palette it was drawn in; the view's own pixels are left
 * alone. For something redrawn every frame over the border, which is not. */
void view_border_restore(int x, int y, int w, int h)
{
    const int xl = view_x, yl = view_y, xr = view_x + view_width,
              yr = view_y + view_height;

    if (view_border_dirty || !border_palette)
        return;                             /* all of it is redrawn anyway */
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < VIEW_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < VIEW_W; xx++) {
            const bool on_x = xx >= xl - 1 && xx <= xr;
            const bool on_y = yy >= yl - 1 && yy <= yr;
            int color = VIEWCOLOR;

            if (xx >= xl && xx < xr && yy >= yl && yy < yr)
                continue;                   /* the view */
            if (xx == xl - 1 && yy == yr)
                color = 124;
            else if ((xx == xr && on_y) || (yy == yr && on_x))
                color = 125;
            else if ((xx == xl - 1 && on_y) || (yy == yl - 1 && on_x))
                color = 0;
            *view_pixel(xx, yy) = border_palette[color];
        }
}

/* ------------------------------------------------------------------ */
/* Demo visibility: WallRefresh()'s ray walk, marking spotvis only     */
/* ------------------------------------------------------------------ */

/* The port's own ray caster decides what is visible closely enough for
 * play, but a recorded demo turns on exactly which tiles are marked: an
 * actor seen once is woken, and keeps thinking when it is out of sight.
 * During a demo spotvis is filled by this instead - the tangent-stepping
 * walk from WL_DR_A.ASM, as Wolf4SDL carries it in C, over the 304 columns
 * of the view the demos are checked against. It marks the tiles a ray
 * passes through, open door gaps included, but not the ones it stops on.
 * The tile encoding differs from the DOS one, so doors and the moving push
 * wall's two tiles are recognized here rather than by tilemap bits. */

#define DV_DOOR  0x100                      /* a door tile, door number below */
#define DV_PWALL 0x200                      /* one of the push wall's tiles   */

static int demo_tile(int x, int y)
{
    int t;

    if ((unsigned)x >= MAPSIZE || (unsigned)y >= MAPSIZE)
        return 1;                           /* off the map: a wall */
    if (pwallstate
        && ((x == pwallx && y == pwally)
            || (x == pwallx + (pwalldir == di_east) - (pwalldir == di_west)
                && y == pwally + (pwalldir == di_south) - (pwalldir == di_north))))
        return DV_PWALL;
    t = tilemap[y][x];
    if ((t & 0xc0) == 0x80)
        return DV_DOOR | (t & 0x3f);
    return t;
}

static int32_t imul(int32_t a, int32_t b)   /* int arithmetic, wrapping */
{
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

static void demo_visibility(fixed viewx, fixed viewy, int viewangle)
{
    const int DV_ANG180 = 1800, DV_ANG270 = 2700, DV_ANG360 = 3600;
    const int midangle = viewangle * 10;
    const int focaltx = viewx >> TILESHIFT, focalty = viewy >> TILESHIFT;
    const uint32_t xpartialdown = viewx & (TILEGLOBAL - 1);
    const uint32_t xpartialup   = xpartialdown ^ (TILEGLOBAL - 1);
    const uint32_t ypartialdown = viewy & (TILEGLOBAL - 1);
    const uint32_t ypartialup   = ypartialdown ^ (TILEGLOBAL - 1);

    for (int pixx = 0; pixx < DEMO_VIEW_W; pixx++) {
        int angle = midangle + pixelangle[pixx];
        int xtilestep, ytilestep, xtile, ytile, xinttile, yinttile, tilehit;
        int32_t xstep, ystep, xintercept, yintercept, xinttemp, yinttemp;
        uint32_t xpartial, ypartial;
        int pwallposnorm, pwallposinv, pwallposi;
        int guard;

        if (angle < 0)
            angle += DV_ANG360;
        if (angle >= DV_ANG360)
            angle -= DV_ANG360;

        if (angle < DEMO_ANG90) {
            xtilestep = 1;  ytilestep = -1;
            xstep = finetangent[DEMO_ANG90 - 1 - angle];
            ystep = -finetangent[angle];
            xpartial = xpartialup;  ypartial = ypartialdown;
        } else if (angle < DV_ANG180) {
            xtilestep = -1; ytilestep = -1;
            xstep = -finetangent[angle - DEMO_ANG90];
            ystep = -finetangent[DV_ANG180 - 1 - angle];
            xpartial = xpartialdown; ypartial = ypartialdown;
        } else if (angle < DV_ANG270) {
            xtilestep = -1; ytilestep = 1;
            xstep = -finetangent[DV_ANG270 - 1 - angle];
            ystep = finetangent[angle - DV_ANG180];
            xpartial = xpartialdown; ypartial = ypartialup;
        } else {
            xtilestep = 1;  ytilestep = 1;
            xstep = finetangent[angle - DV_ANG270];
            ystep = finetangent[DV_ANG360 - 1 - angle];
            xpartial = xpartialup;  ypartial = ypartialup;
        }

        yintercept = mul_round(ystep, (int32_t)xpartial) + viewy;
        yinttile = yintercept >> TILESHIFT;
        xtile = focaltx + xtilestep;

        xintercept = mul_round(xstep, (int32_t)ypartial) + viewx;
        xinttile = xintercept >> TILESHIFT;
        ytile = focalty + ytilestep;

        /* the player in the back tile of a moving push wall */
        if (demo_tile(focaltx, focalty) == DV_PWALL) {
            if ((pwalldir == di_east && xtilestep == 1) || (pwalldir == di_west && xtilestep == -1)) {
                yinttemp = yintercept - (imul(ystep, 64 - pwallpos) >> 6);
                if (yinttemp >> TILESHIFT == focalty)
                    continue;
            } else if ((pwalldir == di_south && ytilestep == 1) || (pwalldir == di_north && ytilestep == -1)) {
                xinttemp = xintercept - (imul(xstep, 64 - pwallpos) >> 6);
                if (xinttemp >> TILESHIFT == focaltx)
                    continue;
            }
        }

        for (guard = 0; guard < 512; guard++) {
            /* --- vertical walls --- */
            if ((xtile - xtilestep) == xinttile && (ytile - ytilestep) == yinttile)
                yinttile = ytile;

            if ((ytilestep == -1 && yinttile <= ytile) || (ytilestep == 1 && yinttile >= ytile))
                goto horizentry;
vertentry:
            tilehit = demo_tile(xtile, yinttile);
            if (tilehit) {
                if (tilehit & DV_DOOR) {
                    const int door = tilehit & 0x3f;
                    if (doorobjlist[door].action == dr_open)
                        goto passvert;
                    yinttemp = yintercept + (ystep >> 1);
                    if (yinttemp >> TILESHIFT != yinttile)
                        goto passvert;
                    if (doorobjlist[door].action != dr_closed
                        && (uint16_t)yinttemp < doorposition[door])
                        goto passvert;
                    break;                  /* HitVertDoor() */
                } else if (tilehit == DV_PWALL) {
                    if (pwalldir == di_west || pwalldir == di_east) {
                        if (pwalldir == di_west) {
                            pwallposnorm = 64 - pwallpos; pwallposinv = pwallpos;
                        } else {
                            pwallposnorm = pwallpos; pwallposinv = 64 - pwallpos;
                        }
                        if ((pwalldir == di_east && xtile == pwallx && yinttile == pwally)
                         || (pwalldir == di_west && !(xtile == pwallx && yinttile == pwally))) {
                            yinttemp = yintercept + (imul(ystep, pwallposnorm) >> 6);
                            if (yinttemp >> TILESHIFT != yinttile)
                                goto passvert;
                        } else {
                            yinttemp = yintercept + (imul(ystep, pwallposinv) >> 6);
                            if (yinttemp >> TILESHIFT != yinttile)
                                goto passvert;
                        }
                    } else {
                        pwallposi = pwalldir == di_north ? 64 - pwallpos : pwallpos;
                        if ((pwalldir == di_south && (uint16_t)yintercept < (pwallposi << 10))
                         || (pwalldir == di_north && (uint16_t)yintercept > (pwallposi << 10))) {
                            if (xtile == pwallx && yinttile == pwally
                                && ((pwalldir == di_south && (int32_t)(uint16_t)yintercept + ystep < (pwallposi << 10))
                                 || (pwalldir == di_north && (int32_t)(uint16_t)yintercept + ystep > (pwallposi << 10))))
                                goto passvert;
                        } else {
                            if (!(xtile == pwallx && yinttile == pwally)
                                && ((pwalldir == di_south && (int32_t)(uint16_t)yintercept + ystep > (pwallposi << 10))
                                 || (pwalldir == di_north && (int32_t)(uint16_t)yintercept + ystep < (pwallposi << 10))))
                                goto passvert;
                        }
                    }
                    break;                  /* HitVertWall() on the push wall */
                }
                break;                      /* HitVertWall() */
            }
passvert:
            if ((unsigned)xtile < MAPSIZE && (unsigned)yinttile < MAPSIZE)
                spotvis[yinttile][xtile] = 1;
            xtile += xtilestep;
            yintercept += ystep;
            yinttile = yintercept >> TILESHIFT;
            continue;

            /* --- horizontal walls --- */
horizentry_loop:
            if ((xtile - xtilestep) == xinttile && (ytile - ytilestep) == yinttile)
                xinttile = xtile;
            if ((xtilestep == -1 && xinttile <= xtile) || (xtilestep == 1 && xinttile >= xtile))
                goto vertentry;
horizentry:
            tilehit = demo_tile(xinttile, ytile);
            if (tilehit) {
                if (tilehit & DV_DOOR) {
                    const int door = tilehit & 0x3f;
                    if (doorobjlist[door].action == dr_open)
                        goto passhoriz;
                    xinttemp = xintercept + (xstep >> 1);
                    if (xinttemp >> TILESHIFT != xinttile)
                        goto passhoriz;
                    if (doorobjlist[door].action != dr_closed
                        && (uint16_t)xinttemp < doorposition[door])
                        goto passhoriz;
                    break;                  /* HitHorizDoor() */
                } else if (tilehit == DV_PWALL) {
                    if (pwalldir == di_north || pwalldir == di_south) {
                        if (pwalldir == di_north) {
                            pwallposnorm = 64 - pwallpos; pwallposinv = pwallpos;
                        } else {
                            pwallposnorm = pwallpos; pwallposinv = 64 - pwallpos;
                        }
                        if ((pwalldir == di_south && xinttile == pwallx && ytile == pwally)
                         || (pwalldir == di_north && !(xinttile == pwallx && ytile == pwally))) {
                            xinttemp = xintercept + (imul(xstep, pwallposnorm) >> 6);
                            if (xinttemp >> TILESHIFT != xinttile)
                                goto passhoriz;
                        } else {
                            xinttemp = xintercept + (imul(xstep, pwallposinv) >> 6);
                            if (xinttemp >> TILESHIFT != xinttile)
                                goto passhoriz;
                        }
                    } else {
                        pwallposi = pwalldir == di_west ? 64 - pwallpos : pwallpos;
                        if ((pwalldir == di_east && (uint16_t)xintercept < (pwallposi << 10))
                         || (pwalldir == di_west && (uint16_t)xintercept > (pwallposi << 10))) {
                            if (xinttile == pwallx && ytile == pwally
                                && ((pwalldir == di_east && (int32_t)(uint16_t)xintercept + xstep < (pwallposi << 10))
                                 || (pwalldir == di_west && (int32_t)(uint16_t)xintercept + xstep > (pwallposi << 10))))
                                goto passhoriz;
                        } else {
                            if (!(xinttile == pwallx && ytile == pwally)
                                && ((pwalldir == di_east && (int32_t)(uint16_t)xintercept + xstep > (pwallposi << 10))
                                 || (pwalldir == di_west && (int32_t)(uint16_t)xintercept + xstep < (pwallposi << 10))))
                                goto passhoriz;
                        }
                    }
                    break;                  /* HitHorizWall() on the push wall */
                }
                break;                      /* HitHorizWall() */
            }
passhoriz:
            if ((unsigned)xinttile < MAPSIZE && (unsigned)ytile < MAPSIZE)
                spotvis[ytile][xinttile] = 1;
            ytile += ytilestep;
            xintercept += xstep;
            xinttile = xintercept >> TILESHIFT;
            goto horizentry_loop;
        }
    }
}

/* ------------------------------------------------------------------ */

void render_view(void)
{
    const int   angle = player.angle;
    const fixed dirx =  fcos(angle);
    const fixed diry = -fsin(angle);
    fixed posx, posy;
    /* The camera plane points to the player's right: the view vector
     * turned a quarter turn, scaled to the half-width of the screen. */
    const fixed planex = -fixmul(diry, PLANE_LEN);
    const fixed planey =  fixmul(dirx, PLANE_LEN);

    /* CalcViewVariables(): the eye is FOCALLENGTH behind the player */
    view_cos  = fcos(angle);
    view_sin  = fsin(angle);
    view_dcos = dos_sintable[(angle / 10 + 90) % 360];
    view_dsin = dos_sintable[angle / 10];
    posx = player.x - mul_cos(FOCALLENGTH);
    posy = player.y + mul_sin(FOCALLENGTH);

    /* VGAClearScreen()'s two colors, drawn with each column */
    const uint16_t ceiling_px = wolf_palette[level_ceiling_color(current_level)];
    const uint16_t floor_px   = wolf_palette[0x19];

    PROF(PROF_BACKGROUND);
    view_columns = true;                    /* every view pixel is drawn */
    if (view_border_dirty) {
        draw_border(view_size, false);      /* and round it, when needed */
        view_border_dirty = false;
    }
    memset(spotvis, 0, sizeof(spotvis));
    /* AsmRefresh() never marks the tile the eye is in - only tiles its
     * rays step into - so an item there is only noticed once some ray
     * crosses it. Outside demos, mark it, as Wolf4SDL does, so that nothing
     * underfoot is missed; a demo was recorded without that. */
    if (!dos_exact)
        spotvis[posy >> TILESHIFT][posx >> TILESHIFT]
            = automap_seen[posy >> TILESHIFT][posx >> TILESHIFT] = 1;

    for (int x = 0; x < view_width; x++) {
        PROF(PROF_RAYS);
        /* -1 at the left edge of the view, +1 at the right */
        fixed camx = (fixed)((int64_t)(2 * x + 1 - view_width) * 65536 / view_width);
        fixed raydirx = dirx + fixmul(planex, camx);
        fixed raydiry = diry + fixmul(planey, camx);

        fixed ddx, ddy, sdx, sdy;
        int stepx, stepy, mapx, mapy;
        fixed dist = 0;
        int page = -1, texcol = 0;

        ddx = (raydirx > -64 && raydirx < 64)
            ? DDA_FAR : (fixed)((1LL << 32) / (raydirx < 0 ? -raydirx : raydirx));
        ddy = (raydiry > -64 && raydiry < 64)
            ? DDA_FAR : (fixed)((1LL << 32) / (raydiry < 0 ? -raydiry : raydiry));
        if (ddx > DDA_FAR) ddx = DDA_FAR;
        if (ddy > DDA_FAR) ddy = DDA_FAR;

        mapx = posx >> TILESHIFT;
        mapy = posy >> TILESHIFT;

        if (raydirx < 0) {
            stepx = -1;
            sdx = (fixed)(((int64_t)(posx & 0xffff) * ddx) >> 16);
        } else {
            stepx = 1;
            sdx = (fixed)(((int64_t)(0x10000 - (posx & 0xffff)) * ddx) >> 16);
        }
        if (raydiry < 0) {
            stepy = -1;
            sdy = (fixed)(((int64_t)(posy & 0xffff) * ddy) >> 16);
        } else {
            stepy = 1;
            sdy = (fixed)(((int64_t)(0x10000 - (posy & 0xffff)) * ddy) >> 16);
        }
        if (sdx > DDA_FAR) sdx = DDA_FAR;
        if (sdy > DDA_FAR) sdy = DDA_FAR;

        for (int steps = 0; steps < MAX_STEPS; steps++) {
            fixed enter, leave;
            int side, tile;

            if (sdx < sdy) {
                enter = sdx; sdx += ddx; mapx += stepx; side = 0;
            } else {
                enter = sdy; sdy += ddy; mapy += stepy; side = 1;
            }
            if ((unsigned)mapx >= MAPSIZE || (unsigned)mapy >= MAPSIZE)
                break;

            if (!dos_exact)
                spotvis[mapy][mapx] = automap_seen[mapy][mapx] = 1;

            tile = tilemap[mapy][mapx];
            if (!tile)
                continue;

            leave = sdx < sdy ? sdx : sdy;

            if ((tile & 0xc0) == 0xc0) {
                /* A push wall in motion: horizpushwall/vertpushwall in
                 * WL_DR_A.ASM. The face the ray is crossing is moved
                 * pwallpos/64 of a tile further along the ray's own step
                 * axis; if the ray still lands in this tile's span there,
                 * it hits, otherwise it has gone past the side of the block
                 * and carries on. As in the DOS game, the offset follows
                 * the axis the ray crossed rather than the direction of the
                 * push, which only shows when the moving block is seen side
                 * on - secret passages rarely allow that. */
                fixed dd;

                if (side == 1) {
                    fixed xint;
                    dd = enter + (fixed)(((int64_t)ddy * pwallpos) >> 6);
                    xint = posx + (fixed)(((int64_t)dd * raydirx) >> 16);
                    if ((xint >> TILESHIFT) != mapx)
                        continue;
                    texcol = (xint >> 10) & 63;
                    if (stepy > 0)
                        texcol = 63 - texcol;
                    page = ((tile & 0x3f) - 1) * 2;
                } else {
                    fixed yint;
                    dd = enter + (fixed)(((int64_t)ddx * pwallpos) >> 6);
                    yint = posy + (fixed)(((int64_t)dd * raydiry) >> 16);
                    if ((yint >> TILESHIFT) != mapy)
                        continue;
                    texcol = (yint >> 10) & 63;
                    if (stepx < 0)
                        texcol = 63 - texcol;
                    page = ((tile & 0x3f) - 1) * 2 + 1;
                }
                dist = dd;
                break;
            }

            if (tile & 0x80) {
                /* A door's slab is a thin wall down the middle of its tile.
                 * Find where the ray crosses that plane; if it crosses
                 * outside this tile, or in the gap the slab has slid back
                 * to, the ray carries on to whatever is behind. */
                int dn = tile & 0x7f;
                int pos = doorposition[dn];
                fixed dd;
                uint32_t frac;

                if (doorobjlist[dn].vertical) {
                    dd = sdx - (ddx >> 1);
                    if (dd < enter || dd >= leave)
                        continue;
                    frac = (uint32_t)(posy + (fixed)(((int64_t)dd * raydiry) >> 16))
                         & 0xffff;
                } else {
                    dd = sdy - (ddy >> 1);
                    if (dd < enter || dd >= leave)
                        continue;
                    frac = (uint32_t)(posx + (fixed)(((int64_t)dd * raydirx) >> 16))
                         & 0xffff;
                }
                if (frac < (uint32_t)pos)
                    continue;               /* through the open part */

                dist   = dd;
                texcol = ((frac - pos) >> 10) & 63;
                page   = door_page(dn) + (doorobjlist[dn].vertical ? 1 : 0);
                break;
            }

            /* Solid wall. horizwall[]/vertwall[] from SetupWalls(): each
             * map tile owns a pair of pages, the darker one for the faces
             * the ray meets crossing a north-south line. */
            dist = enter;
            if (side == 0) {
                fixed yint = posy + (fixed)(((int64_t)enter * raydiry) >> 16);
                texcol = (yint >> 10) & 63;
                if (stepx < 0)
                    texcol = 63 - texcol;
                if ((tile & 0x40) && (tilemap[mapy][mapx - stepx] & 0x80))
                    page = DOORWALL + 3;
                else
                    page = ((tile & 0x3f) - 1) * 2 + 1;
            } else {
                fixed xint = posx + (fixed)(((int64_t)enter * raydirx) >> 16);
                texcol = (xint >> 10) & 63;
                if (stepy > 0)
                    texcol = 63 - texcol;
                if ((tile & 0x40) && (tilemap[mapy - stepy][mapx] & 0x80))
                    page = DOORWALL + 2;
                else
                    page = ((tile & 0x3f) - 1) * 2;
            }
            break;
        }

        if (page < 0 || page >= wolf_wall_pages) {
            walldist[x] = DDA_FAR;          /* ray escaped: sky and floor */
            PROF(PROF_COLUMNS);
            draw_column(x, NULL, 0, ceiling_px, floor_px);
            continue;
        }

        if (dist < MINDIST)
            dist = MINDIST;
        walldist[x] = dist;

        PROF(PROF_COLUMNS);
        draw_column(x, wolf_walls + page * WALL_PAGE_SIZE + texcol * 64,
                    (scale_px << 16) / dist, ceiling_px, floor_px);
    }

    PROF(PROF_COLLECT);
    if (dos_exact)
        demo_visibility(posx, posy, angle / 10);
    collect_scaleds(posx, posy);
    PROF(PROF_SPRITES);
    draw_scaleds();
    PROF(PROF_WEAPON);
    draw_player_weapon();
    PROF(PROF_LOGIC);
}
