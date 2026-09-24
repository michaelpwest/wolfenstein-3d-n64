/* level.c - turn a map into the tables the game runs on.
 *
 * This is SetupGameLevel() and the part of ScanInfoPlane() that places the
 * player, from WL_GAME.C, with the actors and statics left out.
 */
#include <string.h>
#include "wolf.h"

uint8_t  tilemap[MAPSIZE][MAPSIZE];
uint8_t  blockmap[MAPSIZE][MAPSIZE];
uint16_t plane0[MAPSIZE][MAPSIZE];
uint16_t plane1[MAPSIZE][MAPSIZE];
int      current_level;

/* vgaCeiling[] from WL_DRAW.C: Wolfenstein's six episodes, then Spear of
 * Destiny's floors. */
static const uint8_t wolf_ceiling[60] = {
    0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0xbf,
    0x4e, 0x4e, 0x4e, 0x1d, 0x8d, 0x4e, 0x1d, 0x2d, 0x1d, 0x8d,
    0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x2d, 0xdd, 0x1d, 0x1d, 0x98,
    0x1d, 0x9d, 0x2d, 0xdd, 0xdd, 0x9d, 0x2d, 0x4d, 0x1d, 0xdd,
    0x7d, 0x1d, 0x2d, 0x2d, 0xdd, 0xd7, 0x1d, 0x1d, 0x1d, 0x2d,
    0x1d, 0x1d, 0x1d, 0x1d, 0xdd, 0xdd, 0x7d, 0xdd, 0xdd, 0xdd,
};
static const uint8_t spear_ceiling[21] = {
    0x6f, 0x4f, 0x1d, 0xde, 0xdf, 0x2e, 0x7f, 0x9e, 0xae, 0x7f,
    0x1d, 0xde, 0xdf, 0xde, 0xdf, 0xde, 0xe1, 0xdc, 0x2e, 0x1d, 0xdc,
};

int level_ceiling_color(int level)
{
    if (wolf_version == WV_SPEAR)
        return spear_ceiling[level % 21];
    return wolf_ceiling[level % 60];
}

void level_load(int level)
{
    static uint8_t ambushmap[MAPSIZE][MAPSIZE];
    int x, y;

    current_level = level;

    /* US_InitRndT(): a demo starts the random table from the top so that
     * it replays as recorded; a game carries on where it was. */
    if (dos_exact)
        us_init_rndt();
    save_level_loading();

    /* Per-floor counters. Health, ammo, score and lives carry over; keys do
     * not, which is what GameLoop() does when a floor is completed. */
    gamestate.treasurecount = gamestate.treasuretotal = 0;
    gamestate.secretcount = gamestate.secrettotal = 0;
    gamestate.killcount = gamestate.killtotal = 0;
    gamestate.timecount = 0;
    gamestate.keys = 0;
    gamestate.victoryflag = false;
    face_reset();                           /* PlayLoop() */
    pushwall_init();
    actors_init();
    clear_palette_shifts();
    automap_clear();                        /* nothing of it seen yet */

    assets_load_level(level, &plane0[0][0], &plane1[0][0]);
    memset(tilemap, 0, sizeof(tilemap));
    memset(blockmap, 0, sizeof(blockmap));

    /* Walls. Anything below AREATILE is solid; everything else is floor. */
    for (y = 0; y < MAPSIZE; y++) {
        for (x = 0; x < MAPSIZE; x++) {
            unsigned tile = plane0[y][x];
            if (tile < AREATILE) {
                tilemap[y][x]  = (uint8_t)tile;
                blockmap[y][x] = 1;
            }
        }
    }

    /* Doors. Even tiles are vertical (the slab sits on a north-south line
     * and slides north), odd ones horizontal. */
    doors_init();
    for (y = 0; y < MAPSIZE; y++) {
        for (x = 0; x < MAPSIZE; x++) {
            unsigned tile = plane0[y][x];
            if (tile >= 90 && tile <= 101)
                spawn_door(x, y, !(tile & 1),
                           (tile & 1) ? (tile - 91) / 2 : (tile - 90) / 2);
        }
    }

    /* Ambush markers are a hint for the actor AI, not a wall; the DOS code
     * erases them and inherits an area number from a neighbour. It does
     * this after ScanInfoPlane() but only where nothing else has claimed
     * the tile, so doing it first and then letting scenery block comes out
     * the same and needs no "was it me?" test. A standing enemy placed on
     * one waits in ambush: it will not react to noise, only to seeing the
     * player, so the markers are remembered for SpawnStand(). */
    memset(ambushmap, 0, sizeof(ambushmap));
    for (y = 0; y < MAPSIZE; y++) {
        for (x = 0; x < MAPSIZE; x++) {
            unsigned tile;
            if (plane0[y][x] != AMBUSHTILE)
                continue;
            ambushmap[y][x] = 1;
            tilemap[y][x]  = 0;
            blockmap[y][x] = 0;
            tile = AREATILE;
            if (x + 1 < MAPSIZE && plane0[y][x + 1] >= AREATILE)
                tile = plane0[y][x + 1];
            if (y > 0 && plane0[y - 1][x] >= AREATILE)
                tile = plane0[y - 1][x];
            if (y + 1 < MAPSIZE && plane0[y + 1][x] >= AREATILE)
                tile = plane0[y + 1][x];
            if (x > 0 && plane0[y][x - 1] >= AREATILE)
                tile = plane0[y][x - 1];
            plane0[y][x] = tile;
        }
    }

    /* ScanInfoPlane(): the player start (19..22 for north/east/south/west),
     * the scenery (23..74), the push walls, and the enemies (108 on, and
     * Spear of Destiny's spectres and Angel of Death at 106 and 107). */
    statics_init();
    player_spawn(MAPSIZE / 2, MAPSIZE / 2, 0);
    for (y = 0; y < MAPSIZE; y++) {
        for (x = 0; x < MAPSIZE; x++) {
            unsigned tile = plane1[y][x];
            if (tile >= 19 && tile <= 22)
                player_spawn(x, y, tile - 19);
            else if (tile >= 23 && tile <= 74)
                spawn_static(x, y, tile - 23);
            else if (tile == PUSHABLETILE)
                gamestate.secrettotal++;
            else if (tile >= 106)
                spawn_map_actor(tile, x, y, ambushmap[y][x]);
        }
    }
    areas_init();                           /* InitAreas(), in SpawnPlayer */
    save_level_loaded();                    /* what a save is compared with */

    sd_level_start(level);
}
