/* automap.c - the map L shows, of the parts of the floor the player has seen.
 *
 * Not in the DOS game. What counts as seen is what has been on screen: the
 * ray caster marks every tile a ray reaches, the wall it stops at included,
 * each frame it draws (render.c), and those marks are kept here for the
 * floor. So a room behind a shut door stays dark until the door is open and
 * the player looks through, and a secret push wall is drawn as the wall it
 * appears to be until it is pushed. The marks go with a saved game.
 *
 * While it is up the game runs on, as Doom's automap does (game.c): each
 * frame is played and its view drawn as usual - the view is what picks up
 * items, aims the gun and marks the map - and then the map is drawn over
 * it, the whole 320x200 view area, with the status bar left below: every
 * seen wall in the average color of its own
 * texture, doors as a bar across the middle of their tile (gold and silver
 * for the locked ones), explored floor dark, and the player as a dot with a
 * line the way they face.
 */
#include <string.h>
#include "wolf.h"

uint8_t automap_seen[MAPSIZE][MAPSIZE];

#define RGB(r, g, b)  ((uint16_t)((r) << 11 | (g) << 6 | (b) << 1 | 1))

#define MAP_BACK     RGB(0, 0, 0)
#define MAP_FLOOR    RGB(5, 5, 6)
#define MAP_DOOR     RGB(2, 20, 20)             /* the door art's teal     */
#define MAP_GOLD     RGB(31, 25, 2)
#define MAP_SILVER   RGB(24, 26, 29)
#define MAP_LIFT     RGB(18, 18, 18)            /* the elevator's doors    */
#define MAP_PLAYER   RGB(31, 4, 4)
#define MAP_FACING   RGB(31, 31, 8)

void automap_clear(void)
{
    memset(automap_seen, 0, sizeof automap_seen);
}

int automap_count(void)
{
    int n = 0;
    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++)
            n += automap_seen[y][x] != 0;
    return n;
}

/* The average color of a wall page, in the game palette as it is without
 * any damage or bonus flash; worked out the first time each is needed. */
static uint16_t wall_color(int page)
{
    static uint16_t cache[256];
    static bool known[256];

    if (page < 0 || page >= wolf_wall_pages || page >= 256)
        return RGB(16, 16, 16);
    if (!known[page]) {
        const uint8_t *tex = wolf_walls + page * WALL_PAGE_SIZE;
        uint32_t r = 0, g = 0, b = 0;
        for (int i = 0; i < WALL_PAGE_SIZE; i++) {
            const uint16_t c = wolf_palettes[tex[i]];
            r += (c >> 11) & 31;
            g += (c >> 6) & 31;
            b += (c >> 1) & 31;
        }
        cache[page] = RGB(r / WALL_PAGE_SIZE, g / WALL_PAGE_SIZE, b / WALL_PAGE_SIZE);
        known[page] = true;
    }
    return cache[page];
}

/* Column by column: in play the view is kept on its side (colbuf), and it
 * is redrawn under the map every frame. */
static void box(int x, int y, int w, int h, uint16_t color)
{
    for (int xx = x < 0 ? 0 : x; xx < x + w && xx < VIEW_W; xx++)
        for (int yy = y < 0 ? 0 : y; yy < y + h && yy < VIEW_H; yy++)
            *view_pixel(xx, yy) = color;
}

void automap_draw(void)
{
    int x0 = MAPSIZE, y0 = MAPSIZE, x1 = 0, y1 = 0;
    int scale, left, top;

    /* The frame is what has been seen, and the player, with a few tiles to
     * spare: early on that is a room or two drawn large, and it widens as
     * more is found, down to the whole floor. Framing the floor's full
     * extent instead would keep every tile at three pixels from the start. */
    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++)
            if (automap_seen[y][x] || (x == player.tilex && y == player.tiley)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    x0 = x0 - 4 < 0 ? 0 : x0 - 4;
    y0 = y0 - 4 < 0 ? 0 : y0 - 4;
    x1 = x1 + 4 > MAPSIZE - 1 ? MAPSIZE - 1 : x1 + 4;
    y1 = y1 + 4 > MAPSIZE - 1 ? MAPSIZE - 1 : y1 + 4;
    scale = (VIEW_W - 8) / (x1 - x0 + 1);
    if ((VIEW_H - 8) / (y1 - y0 + 1) < scale)
        scale = (VIEW_H - 8) / (y1 - y0 + 1);
    if (scale > 6)
        scale = 6;
    if (scale < 1)
        scale = 1;
    left = (VIEW_W - (x1 - x0 + 1) * scale) / 2;
    top  = (VIEW_H - (y1 - y0 + 1) * scale) / 2;

    box(0, 0, VIEW_W, VIEW_H, MAP_BACK);
    view_border_dirty = true;               /* the border is drawn over */

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const int tile = tilemap[y][x];
            const int px = left + (x - x0) * scale, py = top + (y - y0) * scale;

            if (!automap_seen[y][x])
                continue;
            if ((tile & 0xc0) == 0xc0) {
                /* the push wall on the move, by its light face */
                box(px, py, scale, scale, wall_color(((tile & 0x3f) - 1) * 2));
            } else if (tile & 0x80) {
                const doorobj_t *door = &doorobjlist[tile & 0x7f];
                const int bar = scale >= 4 ? 2 : 1, mid = (scale - bar) / 2;
                const uint16_t c = door->lock == dr_lock1 ? MAP_GOLD
                                 : door->lock == dr_lock2 ? MAP_SILVER
                                 : door->lock == dr_elevator ? MAP_LIFT
                                 : MAP_DOOR;
                box(px, py, scale, scale, MAP_FLOOR);
                if (door->vertical)
                    box(px + mid, py, bar, scale, c);
                else
                    box(px, py + mid, scale, bar, c);
            } else if (tile) {
                /* a wall, by its light face */
                box(px, py, scale, scale, wall_color(((tile & 0x3f) - 1) * 2));
            } else {
                box(px, py, scale, scale, MAP_FLOOR);
            }
        }

    /* the player: a dot, and a line the way they face */
    {
        const int cx = left + (int)(((int64_t)(player.x - ((fixed)x0 << TILESHIFT)) * scale) >> TILESHIFT);
        const int cy = top + (int)(((int64_t)(player.y - ((fixed)y0 << TILESHIFT)) * scale) >> TILESHIFT);
        const int len = scale * 2 < 4 ? 4 : scale * 2;
        const fixed c = fcos(player.angle), s = fsin(player.angle);

        for (int i = 1; i <= len; i++)      /* +y is south, so sin goes up */
            box(cx + (int)(((int64_t)c * i) >> 16), cy - (int)(((int64_t)s * i) >> 16),
                1, 1, MAP_FACING);
        box(cx - 1, cy - 1, 3, 3, MAP_PLAYER);
    }

    /* which floor, in the margin when there is one - of the episode, as the
     * status bar counts them */
    if (left >= 56) {
        char name[16] = "Floor ";
        int n = level_mapon(current_level) + 1, i = 6;
        if (n >= 10)
            name[i++] = (char)('0' + n / 10);
        name[i++] = (char)('0' + n % 10);
        name[i] = 0;
        /* bottom left, away from the FPS readout's corner */
        font_draw(0, 6, VIEW_H - 16, name, 15);
    }
}
