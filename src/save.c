/* save.c - SaveTheGame() and LoadTheGame() from WL_MAIN.C, and the ten
 * save slots, kept in the cartridge's battery-backed SRAM.
 *
 * The DOS save was a memory dump: gamestate, LevelRatios, tilemap, actorat,
 * areaconnect, areabyplayer, every actor, statobjlist, the doors and the
 * push wall, about 40 KB with this port's tables. Loading ran
 * SetupGameLevel() and then read all of it back over the fresh floor.
 *
 * Loading works the same way here, which is what makes the save small: a
 * floor rebuilt by level_load() is already almost everything, so a save
 * only records where the floor now differs from how it loaded - the doors
 * that are open, the items taken, the walls pushed and the actors that have
 * moved, fought or died - plus the player and gamestate. level_load() takes
 * a copy of the fresh floor to compare against, and a save keeps the random
 * index the floor was loaded with, so that loading rebuilds the same floor
 * (actors start on a random tic of their first state). Actors are written
 * XORed with how they started, and the whole save is packed with LZSS: a
 * floor cleared of every enemy and item still comes to about 1.4 KB.
 *
 * Every field is written byte by byte, big-endian, so tools/sim on a
 * little-endian host reads and writes the same saves as the N64.
 *
 * The SRAM holds a 16-byte header, a directory of ten slots (length, CRC-32
 * and name) and the saves packed one after another. A save rewrites the
 * whole 32 KB image, which the cartridge bus does in a moment; when the ten
 * no longer fit, the save is refused, as DOS refused one on a full disk.
 *
 * The last 1024 bytes are the rest of what DOS kept in its config file
 * (ReadConfig()/WriteConfig() in WL_MAIN.C): a high score table for each
 * game - DOS had CONFIG.WL6 and CONFIG.SOD, so they were never one table -
 * and in the final 16 bytes the view size, the sound settings and the
 * Control and Extras switches, with Customize controls' inputs in the 16
 * before those, each block with a CRC-32 of its own. When one does not
 * check out, the defaults stand. The settings are shared: they mean the
 * same thing in either game, and a player who has set the view size and
 * the controls to their liking wants them either way.
 */
#include <stdio.h>
#include <string.h>
#include "wolf.h"

#define MAGIC          "WOLF3DSV"
#define VERSION        1
#define HEADER_BYTES   16
#define DIR_BYTES      (4 + 4 + SAVE_NAME_LEN)
#define DATA_START     (HEADER_BYTES + SAVE_SLOTS * DIR_BYTES)
/* Two high score tables, 512 bytes each: Wolfenstein's in the last 512,
 * where the only table used to be, so that a cartridge written before
 * Spear of Destiny keeps its scores, and Spear's in the 512 below. DOS
 * kept the table in its config file, CONFIG.WL6 or CONFIG.SOD, so the two
 * games never shared one; here they can share a cartridge, and this is
 * what keeps them apart - as state_is_ours() keeps the saves apart. */
#define SCORES_BYTES   1024
#define DATA_END       (SAVE_MEDIUM_BYTES - SCORES_BYTES)
#define SCORES_MAGIC   "WOLF3DHS"
#define SCORES_MAGIC_SOD "SPEARDHS"
#define SCORE_BYTES    (HIGH_NAME_LEN + 1 + 4 + 2 + 2)
#define SCORES_TABLE   (12 + MAX_SCORES * SCORE_BYTES)
#define CONFIG_BYTES   16                       /* the last of the block */
#define CONFIG_START   (SAVE_MEDIUM_BYTES - CONFIG_BYTES)
#define CONFIG_MAGIC   "WLCF"
#define CONTROLS_BYTES 16                       /* just before the config */
#define CONTROLS_START (CONFIG_START - CONTROLS_BYTES)
#define CONTROLS_MAGIC "WLCT"
_Static_assert(CONTROLS_START >= SAVE_MEDIUM_BYTES - SCORES_BYTES / 2
                                 + SCORES_TABLE,
               "the controls block overlaps Wolfenstein's high score table");
_Static_assert(SCORES_TABLE <= SCORES_BYTES / 2,
               "Spear of Destiny's high score table overlaps Wolfenstein's");
_Static_assert(8 + CTL_PACKED_BYTES <= CONTROLS_BYTES,
               "the controls do not fit their block");

/* A save's own format. The first ones began with the floor number and had
 * no more actor than a shareware floor needs; the second begins with
 * STATE_MARK, its number and the game, so that Spear of Destiny and
 * Wolfenstein do not load each other's. */
#define STATE_MARK     0xff
#define STATE_V2       2
#define ACTOR_BYTES_V1 30
#define ACTOR_BYTES    35               /* a 16-bit state, temp1, angle */

/* Scores[] from ID_US_1.C, as a fresh install has them */
highscore_t high_scores[MAX_SCORES];

static const highscore_t default_scores[MAX_SCORES] = {
    { "id software-'92", 10000, 1, 0 },
    { "Adrian Carmack",  10000, 1, 0 },
    { "John Carmack",    10000, 1, 0 },
    { "Kevin Cloud",     10000, 1, 0 },
    { "Tom Hall",        10000, 1, 0 },
    { "John Romero",     10000, 1, 0 },
    { "Jay Wilbur",      10000, 1, 0 },
};

/* ------------------------------------------------------------------ */
/* Big-endian byte streams                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *p, *end;
    bool     overflow;
} writer_t;

typedef struct {
    const uint8_t *p, *end;
    bool           bad;
} reader_t;

static void put8(writer_t *w, int v)
{
    if (w->p < w->end)
        *w->p++ = (uint8_t)v;
    else
        w->overflow = true;
}

static void put16(writer_t *w, int v)
{
    put8(w, v >> 8);
    put8(w, v);
}

static void put32(writer_t *w, int32_t v)
{
    put16(w, (int)(v >> 16));
    put16(w, v);
}

static int get8(reader_t *r)
{
    if (r->p < r->end)
        return *r->p++;
    r->bad = true;
    return 0;
}

static int get16(reader_t *r)                   /* signed */
{
    int hi = get8(r);
    return (int16_t)((hi << 8) | get8(r));
}

static int getu16(reader_t *r)
{
    int hi = get8(r);
    return (hi << 8) | get8(r);
}

static int32_t get32(reader_t *r)
{
    uint32_t hi = (uint16_t)get16(r);
    return (int32_t)((hi << 16) | (uint16_t)get16(r));
}

/* LZSS. A flag byte covers the next eight items, bit set for a literal
 * byte, clear for a match: two bytes of a 12-bit distance back (1..4096)
 * and a 4-bit length (3..18). The search is brute force; a save is a few
 * kilobytes and made once, while the disk turns. */
static size_t lz_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0, flagpos = 0;
    int bit = 8;

    while (i < n) {
        size_t best = 0, bestd = 0;

        if (bit == 8) {
            if (o >= cap)
                return 0;
            flagpos = o++;
            out[flagpos] = 0;
            bit = 0;
        }
        for (size_t j = i > 4096 ? i - 4096 : 0; j < i; j++) {
            size_t len = 0;
            while (len < 18 && i + len < n && in[j + len] == in[i + len])
                len++;
            if (len > best) {
                best  = len;
                bestd = i - j;
                if (len == 18)
                    break;
            }
        }
        if (best >= 3) {
            if (o + 2 > cap)
                return 0;
            out[o++] = (uint8_t)((bestd - 1) >> 4);
            out[o++] = (uint8_t)(((bestd - 1) & 15) << 4 | (best - 3));
            i += best;
        } else {
            if (o >= cap)
                return 0;
            out[flagpos] |= (uint8_t)(1 << bit);
            out[o++] = in[i++];
        }
        bit++;
    }
    return o;
}

static size_t lz_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        const int flags = in[i++];
        for (int bit = 0; bit < 8 && i < n; bit++) {
            if (flags >> bit & 1) {
                if (o >= cap)
                    return 0;
                out[o++] = in[i++];
            } else {
                size_t d, len;
                if (i + 1 >= n)
                    return 0;
                d   = ((size_t)in[i] << 4 | in[i + 1] >> 4) + 1;
                len = (in[i + 1] & 15) + 3;
                i  += 2;
                if (d > o || o + len > cap)
                    return 0;
                while (len--) {
                    out[o] = out[o - d];
                    o++;
                }
            }
        }
    }
    return o;
}

static uint32_t crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xffffffff;
    while (n--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* The floor as it loaded                                              */
/* ------------------------------------------------------------------ */

static struct {
    uint8_t   tilemap[MAPSIZE][MAPSIZE];
    uint8_t   blockmap[MAPSIZE][MAPSIZE];
    uint16_t  plane0[MAPSIZE][MAPSIZE];
    uint16_t  plane1[MAPSIZE][MAPSIZE];
    uint8_t   actorat[MAPSIZE][MAPSIZE];        /* actor slot + 1, or 0 */
    statobj_t stats[MAXSTATS];
    int       numstats;
    objtype   actors[MAXACTORS];                /* in list order */
    int       numobjs;
    int       rndindex;                         /* before the floor loaded */
} base;

static int rnd_before_load;

/* actorat by slot: a slot keeps its actor for life, and actorat can still
 * name one whose actor has left the list, as in DOS */
static int actor_number(const objtype *ob)
{
    return ob ? (int)(ob - objlist) + 1 : 0;
}

/* The free slots as a floor with nothing removed has them: every slot not
 * in the list, lowest first. The save writes the real ones XORed with these,
 * which is all zeros until something has left the list. */
static void usual_free_slots(const uint8_t *order, int count, uint8_t *usual)
{
    uint8_t used[MAXACTORS] = { 0 };
    int n = 0;

    for (int i = 0; i < count; i++)
        if (order[i] < MAXACTORS)
            used[order[i]] = 1;
    for (int s = 0; s < MAXACTORS; s++)
        if (!used[s])
            usual[n++] = (uint8_t)s;
    while (n < MAXACTORS)
        usual[n++] = 0;
}

/* an actor, less what the renderer works out every frame; the first saves'
 * had neither temp1 nor the angle */
static void pack_actor(const objtype *ob, uint8_t *out, int bytes)
{
    writer_t w = { out, out + bytes, false };

    put8(&w, ob->active);
    put16(&w, ob->ticcount);
    put8(&w, ob->obclass);
    if (bytes > ACTOR_BYTES_V1 + 1)
        put16(&w, actor_state_number(ob->state));
    else
        put8(&w, actor_state_number(ob->state));
    put8(&w, ob->flags);
    put32(&w, ob->distance);
    put8(&w, ob->dir);
    put32(&w, ob->x);
    put32(&w, ob->y);
    put8(&w, ob->tilex);
    put8(&w, ob->tiley);
    put8(&w, ob->areanumber);
    put16(&w, ob->hitpoints);
    put32(&w, ob->speed);
    put16(&w, ob->temp2);
    if (bytes > ACTOR_BYTES_V1 + 1) {
        put16(&w, ob->temp1);
        put16(&w, ob->angle);
    }
}

static bool unpack_actor(const uint8_t *in, int bytes, objtype *ob)
{
    reader_t r = { in, in + bytes, false };

    memset(ob, 0, sizeof *ob);
    ob->active     = get8(&r);
    ob->ticcount   = get16(&r);
    ob->obclass    = get8(&r);
    ob->state      = actor_state(bytes > ACTOR_BYTES_V1 + 1 ? get16(&r) : get8(&r));
    ob->flags      = get8(&r);
    ob->distance   = get32(&r);
    ob->dir        = get8(&r);
    ob->x          = get32(&r);
    ob->y          = get32(&r);
    ob->tilex      = get8(&r);
    ob->tiley      = get8(&r);
    ob->areanumber = get8(&r);
    ob->hitpoints  = get16(&r);
    ob->speed      = get32(&r);
    ob->temp2      = get16(&r);
    if (bytes > ACTOR_BYTES_V1 + 1) {
        ob->temp1  = get16(&r);
        ob->angle  = get16(&r) % 360;
    }
    return ob->state != NULL && ob->tilex < MAPSIZE && ob->tiley < MAPSIZE
        && ob->areanumber < NUMAREAS;
}

/* level_load() has built a floor: remember it. The random index it started
 * from is noted by save_level_loading(). */
void save_level_loaded(void)
{
    memcpy(base.tilemap, tilemap, sizeof tilemap);
    memcpy(base.blockmap, blockmap, sizeof blockmap);
    memcpy(base.plane0, plane0, sizeof plane0);
    memcpy(base.plane1, plane1, sizeof plane1);
    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++)
            base.actorat[y][x] = (uint8_t)actor_number(actorat[y][x]);
    memcpy(base.stats, statobjlist, sizeof statobjlist);
    base.numstats = numstats;
    for (int i = 0; i < numobjs; i++)
        base.actors[i] = *actor_at(i);
    base.numobjs  = numobjs;
    base.rndindex = rnd_before_load;
}

void save_level_loading(void)
{
    rnd_before_load = us_rnd_index();
}

/* ------------------------------------------------------------------ */
/* SaveTheGame(): the state, as differences from the floor as loaded   */
/* ------------------------------------------------------------------ */

static void put_diff8(writer_t *w, const uint8_t *cur, const uint8_t *was)
{
    int count = 0;
    for (int i = 0; i < MAPSIZE * MAPSIZE; i++)
        count += cur[i] != was[i];
    put16(w, count);
    for (int i = 0; i < MAPSIZE * MAPSIZE; i++)
        if (cur[i] != was[i]) {
            put16(w, i);
            put8(w, cur[i]);
        }
}

static void put_diff16(writer_t *w, const uint16_t *cur, const uint16_t *was)
{
    int count = 0;
    for (int i = 0; i < MAPSIZE * MAPSIZE; i++)
        count += cur[i] != was[i];
    put16(w, count);
    for (int i = 0; i < MAPSIZE * MAPSIZE; i++)
        if (cur[i] != was[i]) {
            put16(w, i);
            put16(w, cur[i]);
        }
}

static bool same_static(const statobj_t *a, const statobj_t *b)
{
    return a->tilex == b->tilex && a->tiley == b->tiley
        && a->shapenum == b->shapenum && a->itemnumber == b->itemnumber;
}

static uint8_t raw[SAVE_MEDIUM_BYTES];          /* a save before packing */

static size_t write_state(uint8_t *buf, size_t size)
{
    writer_t w = { buf, buf + size, false };
    uint8_t  actorat_now[MAPSIZE][MAPSIZE];
    uint8_t  packed[ACTOR_BYTES], was[ACTOR_BYTES];
    uint8_t  slots[MAXACTORS];
    int      count, damage, bonus;

    put8(&w, STATE_MARK);
    put8(&w, STATE_V2);
    put8(&w, wolf_version == WV_SPEAR);

    /* how to rebuild the floor */
    put8(&w, current_level);
    put8(&w, gamestate.difficulty);
    put8(&w, base.rndindex);

    /* gamestate */
    put16(&w, gamestate.health);
    put16(&w, gamestate.ammo);
    put16(&w, gamestate.lives);
    put32(&w, gamestate.score);
    put32(&w, gamestate.oldscore);
    put16(&w, gamestate.killcount);
    put16(&w, gamestate.killtotal);
    put32(&w, gamestate.nextextra);
    put8(&w, gamestate.keys);
    put8(&w, gamestate.weapon);
    put8(&w, gamestate.bestweapon);
    put8(&w, gamestate.chosenweapon);
    put16(&w, gamestate.treasurecount);
    put16(&w, gamestate.treasuretotal);
    put16(&w, gamestate.secretcount);
    put16(&w, gamestate.secrettotal);
    put8(&w, gamestate.faceframe);
    put32(&w, gamestate.timecount);
    put32(&w, gamestate.killx);
    put32(&w, gamestate.killy);
    put8(&w, gamestate.victoryflag | deathcam << 1);
    put16(&w, face_funny_count());
    count = wolf_version == WV_SPEAR ? LEVEL_RATIOS : 8;    /* by mapon */
    put8(&w, count);
    for (int i = 0; i < count; i++) {
        put16(&w, level_ratios[i].kill);
        put16(&w, level_ratios[i].secret);
        put16(&w, level_ratios[i].treasure);
        put32(&w, level_ratios[i].time);
    }

    /* what only this port keeps apart from gamestate */
    put8(&w, us_rnd_index());
    put16(&w, face_count());
    palette_shift_counts(&damage, &bonus);
    put16(&w, damage);
    put16(&w, bonus);

    /* the player */
    put32(&w, player.x);
    put32(&w, player.y);
    put16(&w, player.angle);
    put16(&w, player.anglefrac);
    put8(&w, player.tilex);
    put8(&w, player.tiley);
    put8(&w, player_area);

    /* the maps */
    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++)
            actorat_now[y][x] = (uint8_t)actor_number(actorat[y][x]);
    put_diff8(&w, tilemap[0], base.tilemap[0]);
    put_diff8(&w, blockmap[0], base.blockmap[0]);
    put_diff16(&w, plane0[0], base.plane0[0]);
    put_diff16(&w, plane1[0], base.plane1[0]);
    put_diff8(&w, actorat_now[0], base.actorat[0]);

    /* the areas */
    count = 0;
    for (int i = 0; i < NUMAREAS * NUMAREAS; i++)
        count += areaconnect[i / NUMAREAS][i % NUMAREAS] != 0;
    put16(&w, count);
    for (int i = 0; i < NUMAREAS * NUMAREAS; i++)
        if (areaconnect[i / NUMAREAS][i % NUMAREAS]) {
            put8(&w, i / NUMAREAS);
            put8(&w, i % NUMAREAS);
            put8(&w, areaconnect[i / NUMAREAS][i % NUMAREAS]);
        }
    for (int i = 0; i < NUMAREAS; i += 8) {
        int bits = 0;
        for (int b = 0; b < 8 && i + b < NUMAREAS; b++)
            bits |= areabyplayer[i + b] << b;
        put8(&w, bits);
    }

    /* the doors that are not shut */
    count = 0;
    for (int i = 0; i < doornum; i++)
        count += doorobjlist[i].action != dr_closed || doorobjlist[i].ticcount
              || doorposition[i];
    put8(&w, count);
    for (int i = 0; i < doornum; i++)
        if (doorobjlist[i].action != dr_closed || doorobjlist[i].ticcount
            || doorposition[i]) {
            put8(&w, i);
            put8(&w, doorobjlist[i].action);
            put16(&w, doorobjlist[i].ticcount);
            put16(&w, doorposition[i]);
        }

    /* the push wall */
    put16(&w, pwallstate);
    put8(&w, pwallpos);
    put8(&w, pwallx);
    put8(&w, pwally);
    put8(&w, pwalldir);

    /* the statics that were taken, or dropped */
    put16(&w, numstats);
    count = 0;
    for (int i = 0; i < numstats; i++)
        count += i >= base.numstats || !same_static(&statobjlist[i], &base.stats[i]);
    put16(&w, count);
    for (int i = 0; i < numstats; i++)
        if (i >= base.numstats || !same_static(&statobjlist[i], &base.stats[i])) {
            put16(&w, i);
            put8(&w, statobjlist[i].tilex);
            put8(&w, statobjlist[i].tiley);
            put16(&w, statobjlist[i].shapenum);
            put8(&w, statobjlist[i].itemnumber);
        }

    /* every actor, in list order - its slot, then the actor XORed with
     * how the one in its place started: what did not change is zeros, and
     * the packing below all but removes it - then the free slots */
    put8(&w, numobjs);
    for (int i = 0; i < numobjs; i++) {
        put8(&w, obj_order[i] ^ i);         /* usually its place in the list */
        pack_actor(actor_at(i), packed, ACTOR_BYTES);
        memset(was, 0, sizeof was);
        if (i < base.numobjs)
            pack_actor(&base.actors[i], was, ACTOR_BYTES);
        for (int k = 0; k < ACTOR_BYTES; k++)
            put8(&w, packed[k] ^ was[k]);
    }
    count = actor_free_slots(slots);
    {
        uint8_t usual[MAXACTORS];
        usual_free_slots(obj_order, numobjs, usual);
        put8(&w, count);
        for (int i = 0; i < count; i++)
            put8(&w, slots[i] ^ usual[i]);
    }

    /* the map: a bit for every tile seen, eight tiles to a byte (the
     * packing makes little of the unseen runs). Saves made before the map
     * end just before this, and load with none of it seen. */
    {
        const uint8_t *seen = &automap_seen[0][0];  /* the 64x64 as one run */
        for (int i = 0; i < MAPSIZE * MAPSIZE; i += 8) {
            int bits = 0;
            for (int b = 0; b < 8; b++)
                bits |= (seen[i + b] != 0) << b;
            put8(&w, bits);
        }
    }

    return w.overflow ? 0 : (size_t)(w.p - buf);
}

/* What the saves have to share, after the header, the directory and the
 * two high score tables with the settings in them */
size_t save_space(void)
{
    return DATA_END - DATA_START;
}

size_t save_state(uint8_t *buf, size_t size)
{
    const size_t n = write_state(raw, sizeof raw);
    return n ? lz_pack(raw, n, buf, size) : 0;
}

/* ------------------------------------------------------------------ */
/* LoadTheGame()                                                       */
/* ------------------------------------------------------------------ */

static bool get_diff8(reader_t *r, uint8_t *map)
{
    int count = getu16(r);
    while (count-- > 0 && !r->bad) {
        int i = getu16(r);
        int v = get8(r);
        if (i >= MAPSIZE * MAPSIZE)
            return false;
        map[i] = (uint8_t)v;
    }
    return !r->bad;
}

static bool get_diff16(reader_t *r, uint16_t *map)
{
    int count = getu16(r);
    while (count-- > 0 && !r->bad) {
        int i = getu16(r);
        int v = getu16(r);
        if (i >= MAPSIZE * MAPSIZE)
            return false;
        map[i] = (uint16_t)v;
    }
    return !r->bad;
}

static bool restore_state(const uint8_t *packed, size_t packed_size)
{
    const size_t size = lz_unpack(packed, packed_size, raw, sizeof raw);
    reader_t r = { raw, raw + size, false };
    uint8_t  actorat_now[MAPSIZE][MAPSIZE];
    uint8_t  order[MAXACTORS], free_list[MAXACTORS];
    int      level, count, damage, rnd, facecount, nfree, ratios, flags = 0;
    int      funny = 0;
    bool     v2 = false;
    gametype gs = { 0 };

    /* the first saves are Wolfenstein's, and start with the floor */
    if (size && raw[0] == STATE_MARK) {
        get8(&r);
        if (get8(&r) != STATE_V2 || get8(&r) != (wolf_version == WV_SPEAR))
            return false;
        v2 = true;
    } else if (wolf_version == WV_SPEAR)
        return false;

    level         = get8(&r);
    gs.difficulty = get8(&r);
    rnd           = get8(&r);
    if (r.bad || level >= wolf_num_levels || gs.difficulty > gd_hard)
        return false;

    /* SetupGameLevel(), from the same random index as the first time */
    dos_exact = false;
    gamestate.difficulty = gs.difficulty;
    us_set_rnd_index(rnd);
    level_load(level);

    gs.health        = get16(&r);
    gs.ammo          = get16(&r);
    gs.lives         = get16(&r);
    gs.score         = get32(&r);
    gs.oldscore      = get32(&r);
    gs.killcount     = get16(&r);
    gs.killtotal     = get16(&r);
    gs.nextextra     = get32(&r);
    gs.keys          = get8(&r);
    gs.weapon        = (int8_t)get8(&r);
    gs.bestweapon    = (int8_t)get8(&r);
    gs.chosenweapon  = (int8_t)get8(&r);
    gs.treasurecount = get16(&r);
    gs.treasuretotal = get16(&r);
    gs.secretcount   = get16(&r);
    gs.secrettotal   = get16(&r);
    gs.faceframe     = get8(&r);
    gs.timecount     = get32(&r);
    ratios = 8;
    if (v2) {
        gs.killx       = get32(&r);
        gs.killy       = get32(&r);
        flags          = get8(&r);
        gs.victoryflag = flags & 1;
        funny          = get16(&r);
        ratios         = get8(&r);
        if (ratios > LEVEL_RATIOS)
            return false;
    }
    gamestate = gs;                     /* no attack under way */
    deathcam  = (flags & 2) != 0;
    memset(level_ratios, 0, sizeof level_ratios);
    for (int i = 0; i < ratios; i++) {
        level_ratios[i].kill     = get16(&r);
        level_ratios[i].secret   = get16(&r);
        level_ratios[i].treasure = get16(&r);
        level_ratios[i].time     = get32(&r);
    }

    us_set_rnd_index(get8(&r));
    facecount = get16(&r);
    set_face_count(facecount);
    set_face_funny_count(funny);
    damage = get16(&r);
    set_palette_shift_counts(damage, get16(&r));

    player.x         = get32(&r);
    player.y         = get32(&r);
    player.angle     = getu16(&r) % FINEANGLES;
    player.anglefrac = get16(&r);
    player.tilex     = get8(&r) & (MAPSIZE - 1);
    player.tiley     = get8(&r) & (MAPSIZE - 1);
    player_area      = get8(&r) % NUMAREAS;

    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++)
            actorat_now[y][x] = (uint8_t)actor_number(actorat[y][x]);
    if (!get_diff8(&r, tilemap[0]) || !get_diff8(&r, blockmap[0])
        || !get_diff16(&r, plane0[0]) || !get_diff16(&r, plane1[0])
        || !get_diff8(&r, actorat_now[0]))
        return false;

    memset(areaconnect, 0, sizeof areaconnect);
    count = getu16(&r);
    while (count-- > 0 && !r.bad) {
        int a = get8(&r), b = get8(&r), v = get8(&r);
        if (a >= NUMAREAS || b >= NUMAREAS)
            return false;
        areaconnect[a][b] = (uint8_t)v;
    }
    for (int i = 0; i < NUMAREAS; i += 8) {
        int bits = get8(&r);
        for (int b = 0; b < 8 && i + b < NUMAREAS; b++)
            areabyplayer[i + b] = (bits >> b) & 1;
    }

    count = get8(&r);
    while (count-- > 0 && !r.bad) {
        int i = get8(&r);
        if (i >= doornum)
            return false;
        doorobjlist[i].action   = get8(&r);
        doorobjlist[i].ticcount = get16(&r);
        doorposition[i]         = getu16(&r);
    }

    pwallstate = get16(&r);
    pwallpos   = get8(&r);
    pwallx     = get8(&r) & (MAPSIZE - 1);
    pwally     = get8(&r) & (MAPSIZE - 1);
    pwalldir   = get8(&r) & 3;

    numstats = getu16(&r);
    if (numstats > MAXSTATS)
        return false;
    count = getu16(&r);
    while (count-- > 0 && !r.bad) {
        int i = getu16(&r);
        if (i >= numstats)
            return false;
        statobjlist[i].tilex      = get8(&r);
        statobjlist[i].tiley      = get8(&r);
        statobjlist[i].shapenum   = get16(&r);
        statobjlist[i].itemnumber = get8(&r);
    }

    /* The actors. The first saves had them in slots 0 on, with the rest
     * free in order, as a floor without projectiles always has them. */
    {
        const int bytes = v2 ? ACTOR_BYTES : ACTOR_BYTES_V1;
        count = get8(&r);
        if (count > MAXACTORS)
            return false;
        for (int i = 0; i < count && !r.bad; i++) {
            uint8_t packed[ACTOR_BYTES], was[ACTOR_BYTES];
            objtype ob;
            order[i] = (uint8_t)(v2 ? get8(&r) ^ i : i);
            memset(was, 0, sizeof was);
            if (i < base.numobjs)
                pack_actor(&base.actors[i], was, bytes);
            for (int k = 0; k < bytes; k++)
                packed[k] = (uint8_t)(get8(&r) ^ was[k]);
            if (order[i] >= MAXACTORS || !unpack_actor(packed, bytes, &ob))
                return false;
            objlist[order[i]] = ob;
        }
        if (v2) {
            uint8_t usual[MAXACTORS];
            nfree = get8(&r);
            if (nfree > MAXACTORS)
                return false;
            usual_free_slots(order, count, usual);
            for (int i = 0; i < nfree; i++)
                free_list[i] = (uint8_t)(get8(&r) ^ usual[i]);
        } else {
            nfree = MAXACTORS - count;
            for (int i = 0; i < nfree; i++)
                free_list[i] = (uint8_t)(count + i);
        }
        if (r.bad || !actor_set_list(order, count, free_list, nfree))
            return false;
    }
    for (int y = 0; y < MAPSIZE; y++)
        for (int x = 0; x < MAPSIZE; x++) {
            int n = actorat_now[y][x];
            if (n > MAXACTORS)
                return false;
            actorat[y][x] = n ? &objlist[n - 1] : NULL;
        }

    /* the map, if the save has one (level_load() has cleared it) */
    if (r.p < r.end) {
        uint8_t *seen = &automap_seen[0][0];
        for (int i = 0; i < MAPSIZE * MAPSIZE && !r.bad; i += 8) {
            const int bits = get8(&r);
            for (int b = 0; b < 8; b++)
                seen[i + b] = (uint8_t)((bits >> b) & 1);
        }
    }

    face_redraw();                      /* DrawFace() after CP_LoadGame() */
    return !r.bad && r.p == r.end;
}

/* ------------------------------------------------------------------ */
/* The slots                                                           */
/* ------------------------------------------------------------------ */

static uint8_t  image[SAVE_MEDIUM_BYTES] __attribute__((aligned(8)));
static uint8_t  scratch[SAVE_MEDIUM_BYTES] __attribute__((aligned(8)));
static bool     slot_used[SAVE_SLOTS];          /* holds a good save */
static bool     slot_ours[SAVE_SLOTS];          /* ...of this game */
static int      slot_offset[SAVE_SLOTS], slot_length[SAVE_SLOTS];
static char     slot_name[SAVE_SLOTS][SAVE_NAME_LEN + 1];

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void write32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* Which game a packed save is from. The packing's first byte is a flag
 * byte, and the next three are the save's first three bytes as they are:
 * nothing before them can match. The first saves began with the floor and
 * are Wolfenstein's; the rest say (write_state()). */
static bool state_is_ours(const uint8_t *p, int length)
{
    const bool spear = wolf_version == WV_SPEAR;

    if (length >= 4 && p[1] == STATE_MARK)
        return p[2] == STATE_V2 && p[3] == spear;
    return !spear;
}

/* Read the directory, and trust only the slots whose data checks out. A
 * save of the other game - Spear of Destiny's in a Wolfenstein build, or
 * the other way round - is kept, but shown as an empty slot. */
static void scan_image(void)
{
    int offset = DATA_START;
    bool ok = !memcmp(image, MAGIC, 8)
           && (image[8] << 8 | image[9]) == VERSION;

    for (int s = 0; s < SAVE_SLOTS; s++) {
        const uint8_t *d = image + HEADER_BYTES + s * DIR_BYTES;
        int length = ok ? (int)read32(d) : 0;

        slot_used[s] = slot_ours[s] = false;
        slot_offset[s] = offset;
        slot_length[s] = 0;
        slot_name[s][0] = 0;
        if (length <= 0 || offset + length > DATA_END)
            continue;
        offset += length;               /* the next save follows, good or not */
        if (crc32(image + slot_offset[s], length) != read32(d + 4))
            continue;
        slot_used[s] = true;
        slot_ours[s] = state_is_ours(image + slot_offset[s], length);
        slot_length[s] = length;
        memcpy(slot_name[s], d + 8, SAVE_NAME_LEN);
        slot_name[s][SAVE_NAME_LEN] = 0;
    }
}

/* This game's half of the score block, and the magic that marks it: the
 * other game's table is the other half and is left alone. */
static uint8_t *scores_block(uint8_t *base)
{
    return base + SAVE_MEDIUM_BYTES
         - (wolf_version == WV_SPEAR ? SCORES_BYTES : SCORES_BYTES / 2);
}

static const char *scores_magic(void)
{
    return wolf_version == WV_SPEAR ? SCORES_MAGIC_SOD : SCORES_MAGIC;
}

/* the high score table: its magic, a CRC-32 of the entries, the entries */
static void read_scores(void)
{
    const uint8_t *block = scores_block(image);
    const uint8_t *e = block + 12;

    memcpy(high_scores, default_scores, sizeof high_scores);
    if (memcmp(block, scores_magic(), 8)
        || crc32(e, MAX_SCORES * SCORE_BYTES) != read32(block + 8))
        return;
    for (int i = 0; i < MAX_SCORES; i++, e += SCORE_BYTES) {
        highscore_t *s = &high_scores[i];
        memcpy(s->name, e, HIGH_NAME_LEN);
        s->name[HIGH_NAME_LEN] = 0;
        s->score     = (int32_t)read32(e + HIGH_NAME_LEN + 1);
        s->completed = e[HIGH_NAME_LEN + 5] << 8 | e[HIGH_NAME_LEN + 6];
        s->episode   = e[HIGH_NAME_LEN + 7] << 8 | e[HIGH_NAME_LEN + 8];
    }
}

static void write_scores(uint8_t *to)
{
    uint8_t *block = scores_block(to);
    uint8_t *e = block + 12;

    memset(block, 0, SCORES_TABLE);     /* this table; not the other game's,
                                           nor the settings after it */
    memcpy(block, scores_magic(), 8);
    for (int i = 0; i < MAX_SCORES; i++, e += SCORE_BYTES) {
        const highscore_t *s = &high_scores[i];
        memcpy(e, s->name, strnlen(s->name, HIGH_NAME_LEN));
        write32(e + HIGH_NAME_LEN + 1, (uint32_t)s->score);
        e[HIGH_NAME_LEN + 5] = (uint8_t)(s->completed >> 8);
        e[HIGH_NAME_LEN + 6] = (uint8_t)s->completed;
        e[HIGH_NAME_LEN + 7] = (uint8_t)(s->episode >> 8);
        e[HIGH_NAME_LEN + 8] = (uint8_t)s->episode;
    }
    write32(block + 8, crc32(block + 12, MAX_SCORES * SCORE_BYTES));
}

/* The rest of ReadConfig()/WriteConfig(): the view size and the three sound
 * settings, a byte each after a magic and a CRC-32, then the Extras menu's
 * four switches as bits of one more byte. Left as they are when the
 * cartridge has none. A cartridge written before Extras were saved has 0
 * in that byte, which is every switch off - the defaults. Always Run shares
 * it, and the stick sensitivity is the byte after, stored plus one so that
 * the 0 an older cartridge has there reads as the default. The last two
 * bytes are not used.
 *
 * Customize Controls' inputs, four bits an action, need more room than that,
 * so they have a block of their own just before - its own magic and CRC-32,
 * in the space the high score table leaves - and a cartridge without one
 * keeps the defaults, the rest of its settings untouched. */
#define OPT_FPS        1
#define OPT_PROFILER   2
#define OPT_GODMODE    4
#define OPT_ALWAYSRUN  8
#define OPT_NOCLIP     16

static void read_config(void)
{
    const uint8_t *c = image + CONFIG_START;
    const uint8_t *k = image + CONTROLS_START;

    if (!memcmp(k, CONTROLS_MAGIC, 4)
        && crc32(k + 8, CONTROLS_BYTES - 8) == read32(k + 4))
        ctl_unpack_buttons(k + 8);

    if (memcmp(c, CONFIG_MAGIC, 4) || crc32(c + 8, 8) != read32(c + 4))
        return;
    set_view_size(c[8]);
    sd_set_effects(c[9] != 0);
    sd_set_digi(c[10] != 0);
    sd_set_music(c[11] != 0);
    opt_show_fps      = (c[12] & OPT_FPS) != 0;
    opt_show_profiler = (c[12] & OPT_PROFILER) != 0;
    opt_god_mode      = (c[12] & OPT_GODMODE) != 0;
    opt_no_clip       = (c[12] & OPT_NOCLIP) != 0;
    ctl_always_run    = (c[12] & OPT_ALWAYSRUN) != 0;
    ctl_stick_sensitivity = c[13] >= 1 && c[13] <= CTL_SENS_MAX + 1
                          ? c[13] - 1 : CTL_SENS_DEFAULT;
}

static void write_config(uint8_t *to)
{
    uint8_t *c = to + CONFIG_START;

    memset(c, 0, CONFIG_BYTES);
    memcpy(c, CONFIG_MAGIC, 4);
    c[8]  = (uint8_t)view_size;
    c[9]  = sd_effects_on;
    c[10] = sd_digi_on;
    c[11] = sd_music_on;
    c[12] = (uint8_t)((opt_show_fps ? OPT_FPS : 0)
                      | (opt_show_profiler ? OPT_PROFILER : 0)
                      | (opt_god_mode ? OPT_GODMODE : 0)
                      | (opt_no_clip ? OPT_NOCLIP : 0)
                      | (ctl_always_run ? OPT_ALWAYSRUN : 0));
    c[13] = (uint8_t)(ctl_stick_sensitivity + 1);
    write32(c + 4, crc32(c + 8, 8));

    c = to + CONTROLS_START;
    memset(c, 0, CONTROLS_BYTES);
    memcpy(c, CONTROLS_MAGIC, 4);
    ctl_pack_buttons(c + 8);
    write32(c + 4, crc32(c + 8, CONTROLS_BYTES - 8));
}

/* SetupSaveGames(), and ReadConfig() */
void save_init(void)
{
    if (!save_medium_read(image))
        memset(image, 0, sizeof image);
    scan_image();
    read_scores();
    read_config();
}

/* The first half of CheckHighScore(): the new score goes in above the first
 * one it beats - or equals, having got further - and pushes the last off. */
int high_score_insert(int32_t score, int completed, int episode)
{
    for (int i = 0; i < MAX_SCORES; i++) {
        if (score > high_scores[i].score
            || (score == high_scores[i].score
                && completed > high_scores[i].completed)) {
            for (int j = MAX_SCORES - 1; j > i; j--)
                high_scores[j] = high_scores[j - 1];
            memset(&high_scores[i], 0, sizeof high_scores[i]);
            high_scores[i].score     = score;
            high_scores[i].completed = completed;
            high_scores[i].episode   = episode;
            return i;
        }
    }
    return -1;
}

/* WriteConfig(): the table and the settings; the saves stay as they are */
bool save_config(void)
{
    memcpy(scratch, image, sizeof scratch);
    write_scores(scratch);
    write_config(scratch);
    if (!save_medium_write(scratch))
        return false;
    memcpy(image, scratch, sizeof image);
    return true;
}

bool save_high_scores(void)
{
    return save_config();
}

bool save_slot_used(int slot)
{
    return slot >= 0 && slot < SAVE_SLOTS && slot_used[slot] && slot_ours[slot];
}

const char *save_slot_name(int slot)
{
    return save_slot_used(slot) ? slot_name[slot] : "";
}


bool save_game(int slot, const char *name)
{
    static uint8_t state[SAVE_MEDIUM_BYTES];
    const size_t length = save_state(state, sizeof state);
    int offset = DATA_START;

    if (!length || slot < 0 || slot >= SAVE_SLOTS)
        return false;

    /* the new image: this slot replaced, the others packed around it */
    memset(scratch, 0, sizeof scratch);
    memcpy(scratch, MAGIC, 8);
    scratch[8] = VERSION >> 8;
    scratch[9] = VERSION & 0xff;
    for (int s = 0; s < SAVE_SLOTS; s++) {
        uint8_t *d = scratch + HEADER_BYTES + s * DIR_BYTES;
        const uint8_t *data;
        int n;

        if (s == slot) {
            data = state;
            n = (int)length;
            strncpy((char *)d + 8, name, SAVE_NAME_LEN);
        } else if (slot_used[s]) {
            data = image + slot_offset[s];
            n = slot_length[s];
            memcpy(d + 8, slot_name[s], SAVE_NAME_LEN);
        } else
            continue;

        if (offset + n > DATA_END)
            return false;                   /* STR_NOSPACE */
        memmove(scratch + offset, data, n);
        write32(d, n);
        write32(d + 4, crc32(data, n));
        offset += n;
    }
    write_scores(scratch);                  /* the table goes along unchanged */
    write_config(scratch);

    if (!save_medium_write(scratch))
        return false;
    memcpy(image, scratch, sizeof image);
    scan_image();
    return slot_used[slot];
}

bool load_game(int slot)
{
    if (!save_slot_used(slot))
        return false;
    return restore_state(image + slot_offset[slot], slot_length[slot]);
}
