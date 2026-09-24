/* doors.c - the sliding doors, ported from the DOORS section of WL_ACT1.C.
 *
 * doorposition[] is the leading edge of each door: 0 shut, 0xffff wide
 * open. The renderer reads it directly, and the door tile stays solid in
 * blockmap[] until it is all the way open, which is how the DOS game keeps
 * you from walking into a half-open door.
 *
 * Doors also join rooms: while one is open the rooms either side count as
 * one for sound and for which enemies can hear or see the player.
 */
#include <string.h>
#include "wolf.h"

#define OPENTICS  300           /* how long a door stays open, in tics */

doorobj_t doorobjlist[MAXDOORS];
uint16_t  doorposition[MAXDOORS];
int       doornum;

/* ------------------------------------------------------------------ */
/* Areas - areaconnect, ConnectAreas, InitAreas                       */
/* ------------------------------------------------------------------ */

uint8_t areaconnect[NUMAREAS][NUMAREAS];
bool           areabyplayer[NUMAREAS];

int area_at(int tilex, int tiley)
{
    int a;
    if (tilex < 0 || tiley < 0 || tilex >= MAPSIZE || tiley >= MAPSIZE)
        return 0;
    a = (int)plane0[tiley][tilex] - AREATILE;
    return a < 0 ? 0 : a >= NUMAREAS ? NUMAREAS - 1 : a;
}

static void recursive_connect(int areanumber)
{
    for (int i = 0; i < NUMAREAS; i++) {
        if (areaconnect[areanumber][i] && !areabyplayer[i]) {
            areabyplayer[i] = true;
            recursive_connect(i);
        }
    }
}

void connect_areas(void)
{
    memset(areabyplayer, 0, sizeof(areabyplayer));
    areabyplayer[player_area] = true;
    recursive_connect(player_area);
}

void areas_init(void)
{
    memset(areabyplayer, 0, sizeof(areabyplayer));
    areabyplayer[player_area] = true;
}

/* the two rooms a door joins: either side along its opening direction */
static void door_areas(int door, int *area1, int *area2)
{
    int x = doorobjlist[door].tilex, y = doorobjlist[door].tiley;
    if (doorobjlist[door].vertical) {
        *area1 = area_at(x + 1, y);
        *area2 = area_at(x - 1, y);
    } else {
        *area1 = area_at(x, y - 1);
        *area2 = area_at(x, y + 1);
    }
}

void doors_init(void)
{
    memset(doorobjlist, 0, sizeof(doorobjlist));
    memset(doorposition, 0, sizeof(doorposition));
    memset(areaconnect, 0, sizeof(areaconnect));
    memset(areabyplayer, 0, sizeof(areabyplayer));
    doornum = 0;
}

void spawn_door(int tilex, int tiley, bool vertical, int lock)
{
    doorobj_t *d;

    if (doornum == MAXDOORS)
        return;                 /* the DOS game quits here; just drop it */

    d = &doorobjlist[doornum];
    d->tilex    = tilex;
    d->tiley    = tiley;
    d->vertical = vertical;
    d->lock     = lock;
    d->action   = dr_closed;
    d->ticcount = 0;
    doorposition[doornum] = 0;

    tilemap[tiley][tilex]  = 0x80 | doornum;
    blockmap[tiley][tilex] = 1;

    /* The door tile itself has no area number, so it borrows a neighbour's,
     * and the two tiles flanking it are flagged as jambs so the renderer
     * draws the door-side texture on them. */
    if (vertical) {
        if (tilex > 0)
            plane0[tiley][tilex] = plane0[tiley][tilex - 1];
        if (tiley > 0)
            tilemap[tiley - 1][tilex] |= 0x40;
        if (tiley + 1 < MAPSIZE)
            tilemap[tiley + 1][tilex] |= 0x40;
    } else {
        if (tiley > 0)
            plane0[tiley][tilex] = plane0[tiley - 1][tilex];
        if (tilex > 0)
            tilemap[tiley][tilex - 1] |= 0x40;
        if (tilex + 1 < MAPSIZE)
            tilemap[tiley][tilex + 1] |= 0x40;
    }

    doornum++;
}

int door_page(int door)
{
    switch (doorobjlist[door].lock) {
    case dr_elevator: return DOORWALL + 4;
    case dr_normal:   return DOORWALL;
    default:          return DOORWALL + 6;      /* dr_lock1..dr_lock4 */
    }
}

/* ------------------------------------------------------------------ */

void open_door(int door)
{
    if (doorobjlist[door].action == dr_open)
        doorobjlist[door].ticcount = 0;         /* restart the open time */
    else
        doorobjlist[door].action = dr_opening;
}

static void close_door(int door)
{
    int tilex = doorobjlist[door].tilex;
    int tiley = doorobjlist[door].tiley;
    objtype *check;

    /* don't close on anything solid. In DOS the door itself is in actorat
     * until it is all the way open, so a door still opening cannot be
     * pushed shut; blockmap holds that here. */
    if (actorat[tiley][tilex] || blockmap[tiley][tilex])
        return;
    if (player.tilex == tilex && player.tiley == tiley)
        return;

    if (doorobjlist[door].vertical) {
        if (player.tiley == tiley
            && (((player.x + MINDIST) >> TILESHIFT) == tilex
             || ((player.x - MINDIST) >> TILESHIFT) == tilex))
            return;
        check = tilex > 0 ? actorat[tiley][tilex - 1] : NULL;
        if (check && ((check->x + MINDIST) >> TILESHIFT) == tilex)
            return;
        check = tilex + 1 < MAPSIZE ? actorat[tiley][tilex + 1] : NULL;
        if (check && ((check->x - MINDIST) >> TILESHIFT) == tilex)
            return;
    } else {
        if (player.tilex == tilex
            && (((player.y + MINDIST) >> TILESHIFT) == tiley
             || ((player.y - MINDIST) >> TILESHIFT) == tiley))
            return;
        check = tiley > 0 ? actorat[tiley - 1][tilex] : NULL;
        if (check && ((check->y + MINDIST) >> TILESHIFT) == tiley)
            return;
        check = tiley + 1 < MAPSIZE ? actorat[tiley + 1][tilex] : NULL;
        if (check && ((check->y - MINDIST) >> TILESHIFT) == tiley)
            return;
    }

    /* play the door sound if the door is in a room connected to the player */
    if (areabyplayer[area_at(tilex, tiley)])
        sd_play_sound_loc_tile(CLOSEDOORSND, tilex, tiley);

    doorobjlist[door].action = dr_closing;
    blockmap[tiley][tilex]   = 1;
}

void operate_door(int door)
{
    int lock = doorobjlist[door].lock;

    if (lock >= dr_lock1 && lock <= dr_lock4
        && !(gamestate.keys & (1 << (lock - dr_lock1)))) {
        sd_play_sound(NOWAYSND);                /* locked: need the key */
        return;
    }

    switch (doorobjlist[door].action) {
    case dr_closed:
    case dr_closing:
        open_door(door);
        break;
    case dr_open:
    case dr_opening:
        close_door(door);
        break;
    }
}

/* ------------------------------------------------------------------ */

static void door_opening(int door, int tics)
{
    int32_t position;

    /* DoorOpening(): the first tic of movement is when the areas connect
     * and the sound starts, placed at the door. */
    if (!doorposition[door]) {
        int area1, area2;
        door_areas(door, &area1, &area2);
        areaconnect[area1][area2]++;
        areaconnect[area2][area1]++;
        connect_areas();
        if (areabyplayer[area1])
            sd_play_sound_loc_tile(OPENDOORSND, doorobjlist[door].tilex,
                                   doorobjlist[door].tiley);
    }

    position = doorposition[door] + (tics << 10);

    if (position >= 0xffff) {
        position = 0xffff;
        doorobjlist[door].ticcount = 0;
        doorobjlist[door].action   = dr_open;
        blockmap[doorobjlist[door].tiley][doorobjlist[door].tilex] = 0;
    }
    doorposition[door] = (uint16_t)position;
}

static void door_closing(int door, int tics)
{
    int tilex = doorobjlist[door].tilex;
    int tiley = doorobjlist[door].tiley;
    int32_t position;

    /* Something got inside the doorway while it was shutting. */
    if (actorat[tiley][tilex]
        || (player.tilex == tilex && player.tiley == tiley)) {
        open_door(door);
        return;
    }

    position = (int32_t)doorposition[door] - (tics << 10);
    if (position <= 0) {
        int area1, area2;
        position = 0;
        doorobjlist[door].action = dr_closed;

        /* all the way closed, so disconnect the areas */
        door_areas(door, &area1, &area2);
        if (areaconnect[area1][area2]) areaconnect[area1][area2]--;
        if (areaconnect[area2][area1]) areaconnect[area2][area1]--;
        connect_areas();
    }
    doorposition[door] = (uint16_t)position;
}

void move_doors(int tics)
{
    int door;

    if (gamestate.victoryflag)              /* don't move doors during victory */
        return;

    for (door = 0; door < doornum; door++) {
        switch (doorobjlist[door].action) {
        case dr_open:
            doorobjlist[door].ticcount += tics;
            if (doorobjlist[door].ticcount >= OPENTICS)
                close_door(door);
            break;
        case dr_opening:
            door_opening(door, tics);
            break;
        case dr_closing:
            door_closing(door, tics);
            break;
        }
    }
}

/* ================================================================== */
/* Push walls - PushWall() and MovePWalls()                           */
/* ================================================================== */

int pwallstate;
int pwallpos;
int pwallx, pwally, pwalldir;

static const int dir_dx[4] = { 0, 1, 0, -1 };   /* north east south west */
static const int dir_dy[4] = { -1, 0, 1, 0 };

void pushwall_init(void)
{
    pwallstate = pwallpos = 0;
}

/* Claim the tile the wall is about to slide into: solid, and drawn as the
 * wall, exactly as PushWall()/MovePWalls() write actorat and tilemap. */
static bool claim_ahead(int x, int y, int dir, int tile)
{
    int nx = x + dir_dx[dir], ny = y + dir_dy[dir];

    if (nx < 0 || ny < 0 || nx >= MAPSIZE || ny >= MAPSIZE || blockmap[ny][nx])
        return false;
    tilemap[ny][nx]  = (uint8_t)tile;
    blockmap[ny][nx] = 1;
    return true;
}

bool push_wall(int checkx, int checky, int dir)
{
    int oldtile;

    if (pwallstate)
        return false;                       /* one at a time */

    oldtile = tilemap[checky][checkx] & 0x3f;
    if (!oldtile)
        return false;

    if (!claim_ahead(checkx, checky, dir, oldtile)) {
        sd_play_sound(NOWAYSND);
        return false;
    }

    gamestate.secretcount++;
    pwallx     = checkx;
    pwally     = checky;
    pwalldir   = dir;
    pwallstate = 1;
    pwallpos   = 0;
    tilemap[pwally][pwallx] = (uint8_t)(oldtile | 0xc0);
    plane1[pwally][pwallx]  = 0;            /* it cannot be pushed again */
    sd_play_sound(PUSHWALLSND);
    return true;
}

/* MovePWalls() for `tics` tics. The wall crosses into a new tile every 128
 * tics; each time, the tile it left becomes floor, and it carries on
 * unless something is in the way or pwallstate has passed 256. */
static void move_pwall_tics(int tics)
{
    int oldblock = pwallstate / 128;

    pwallstate += tics;

    if (pwallstate / 128 != oldblock) {
        int oldtile = tilemap[pwally][pwallx] & 0x3f;

        tilemap[pwally][pwallx]  = 0;
        blockmap[pwally][pwallx] = 0;
        plane0[pwally][pwallx]   = plane0[player.tiley][player.tilex];

        if (pwallstate > 256) {
            pwallstate = 0;                 /* pushed as far as it goes */
            return;
        }

        pwallx += dir_dx[pwalldir];
        pwally += dir_dy[pwalldir];
        if (!claim_ahead(pwallx, pwally, pwalldir, oldtile)) {
            pwallstate = 0;                 /* stopped by what is ahead */
            return;
        }
        tilemap[pwally][pwallx] = (uint8_t)(oldtile | 0xc0);
    }

    pwallpos = (pwallstate / 2) & 63;
}

/* The DOS code advances pwallstate by the whole frame's tics at once, so
 * how far a wall travels depends on whether the count lands exactly on
 * 256: at 70 fps, with a tic a frame, it always does and the wall moves
 * three tiles; at lower frame rates it sometimes moves two. Stepping one
 * tic at a time gives the fast-PC behavior whatever the N64 frame rate.
 * A demo takes the DOS path, a frame's tics at once, as it was recorded. */
void move_pwalls(int tics)
{
    if (dos_exact) {
        if (pwallstate)
            move_pwall_tics(tics);
        return;
    }
    while (tics-- > 0 && pwallstate)
        move_pwall_tics(1);
}
