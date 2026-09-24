/* sim.c - run the game on the host and dump what it drew.
 *
 * src/ is deliberately free of libdragon so the map loader, the doors, the
 * movement and the ray caster can be compiled for a PC and inspected. This
 * driver plays a fixed script - look around the start room, walk into the
 * first door, open it, walk through - and writes a PPM per captured frame,
 * which tools/sheet.py turns into a contact sheet.
 *
 * Build and run through tools/sim.ps1.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/wolf.h"

static uint8_t *blob;
static char     outdir[512];
static int      shot;

/* ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* The blob is big-endian, which is native on the N64 and is not here. */
static void swap16(void *p, size_t count)
{
    uint16_t *v = p;
    for (size_t i = 0; i < count; i++)
        v[i] = (uint16_t)((v[i] >> 8) | (v[i] << 8));
}

static void swap32(void *p, size_t count)
{
    uint32_t *v = p;
    for (size_t i = 0; i < count; i++)
        v[i] = (v[i] >> 24) | ((v[i] >> 8) & 0xff00)
             | ((v[i] << 8) & 0xff0000) | (v[i] << 24);
}

static void host_byteswap(void)
{
    uint32_t pal_off     = be32(blob + 12);
    uint32_t sprites_off = be32(blob + 28);

    swap16(blob + pal_off, 256 * 10);          /* ten palettes */

    /* The sprite and pic sections each keep every multi-byte field in one
     * 32-bit run ahead of the pixels, exactly so these are single calls. */
    swap32(blob + sprites_off, be32(blob + sprites_off + 8) / 4);

    uint32_t pics_off = be32(blob + 32);
    swap32(blob + pics_off, be32(blob + pics_off + 4) / 4);

    /* The sound section is 32-bit words throughout, and its first says how
     * many. The maps in wolfbig.dat are put in order as they are read. */
    uint32_t snd = be32(blob + 36);
    swap32(blob + snd, be32(blob + snd));
}

/* wolfbig.dat, from beside the blob */
static FILE *big_file;

void platform_read_big(uint32_t offset, void *dst, uint32_t len)
{
    if (!big_file || fseek(big_file, (long)offset, SEEK_SET) != 0
        || fread(dst, 1, len, big_file) != len) {
        fprintf(stderr, "cannot read %u bytes at %u of wolfbig.dat\n",
                (unsigned)len, (unsigned)offset);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* A sound backend that records what the game asked for               */
/* ------------------------------------------------------------------ */

static struct {
    int adlib_calls, digi_calls, vol_calls, music_calls;
    int last_adlib, last_digi, last_lvol, last_rvol, last_music;
    bool music_paused;
    int count_by_sound[100];
} sb;

void snd_backend_adlib(int sound)
{
    sb.adlib_calls++;
    sb.last_adlib = sound;
    if (sound >= 0 && sound < 100)
        sb.count_by_sound[sound]++;
}

void snd_backend_digi(int digi, int lvol, int rvol)
{
    if (digi < 0)
        return;                         /* a stop, from a level start */
    sb.digi_calls++;
    sb.last_digi = digi;
    sb.last_lvol = lvol;
    sb.last_rvol = rvol;
}

void snd_backend_digi_volume(int lvol, int rvol)
{
    sb.vol_calls++;
    sb.last_lvol = lvol;
    sb.last_rvol = rvol;
}

void snd_backend_music(int song)
{
    sb.music_calls++;
    sb.last_music = song;
}

void snd_backend_music_pause(bool paused)
{
    sb.music_paused = paused;
}

/* The sign-on screen's hardware, as main.c reports it for a console without
 * an Expansion Pak: main memory and EMS full, no XMS, one controller, and
 * both AdLib and the Sound Blaster. */
void platform_machine(machine_t *m)
{
    m->main_kb = m->ems_kb = 4096;
    m->xms_kb = 0;
    m->mouse = false;
    m->joystick = true;
    m->adlib = m->sound_blaster = true;
    m->sound_source = false;
}

/* ------------------------------------------------------------------ */
/* Cartridge SRAM, in memory, so that every run starts with no saves   */
/* ------------------------------------------------------------------ */

static uint8_t sram[SAVE_MEDIUM_BYTES];
static int     sram_writes;

bool save_medium_read(uint8_t *buf)
{
    memcpy(buf, sram, sizeof sram);
    return true;
}

bool save_medium_write(const uint8_t *buf)
{
    memcpy(sram, buf, sizeof sram);
    sram_writes++;
    return true;
}

/* A checksum of everything a saved game has to bring back: the maps, the
 * areas, doors, push wall, statics, every actor (less what the renderer
 * recomputes), the player, gamestate and the random index. */
static uint32_t state_digest(void)
{
    uint32_t h = 2166136261u;
#define MIX(v)  do { uint32_t m_ = (uint32_t)(v); \
                     for (int k_ = 0; k_ < 4; k_++, m_ >>= 8) \
                         h = (h ^ (m_ & 0xff)) * 16777619u; } while (0)
    for (int i = 0; i < MAPSIZE * MAPSIZE; i++) {
        MIX(tilemap[i / MAPSIZE][i % MAPSIZE]);
        MIX(blockmap[i / MAPSIZE][i % MAPSIZE]);
        MIX(plane0[i / MAPSIZE][i % MAPSIZE]);
        MIX(plane1[i / MAPSIZE][i % MAPSIZE]);
        MIX(automap_seen[i / MAPSIZE][i % MAPSIZE]);
        MIX(actorat[i / MAPSIZE][i % MAPSIZE]
            ? actorat[i / MAPSIZE][i % MAPSIZE] - objlist + 1 : 0);
    }
    for (int i = 0; i < NUMAREAS * NUMAREAS; i++)
        MIX(areaconnect[i / NUMAREAS][i % NUMAREAS]);
    for (int i = 0; i < NUMAREAS; i++)
        MIX(areabyplayer[i]);
    for (int i = 0; i < doornum; i++) {
        MIX(doorobjlist[i].action);
        MIX(doorobjlist[i].ticcount);
        MIX(doorposition[i]);
    }
    MIX(pwallstate); MIX(pwallpos); MIX(pwallx); MIX(pwally); MIX(pwalldir);
    MIX(numstats);
    for (int i = 0; i < numstats; i++) {
        MIX(statobjlist[i].tilex); MIX(statobjlist[i].tiley);
        MIX(statobjlist[i].shapenum); MIX(statobjlist[i].itemnumber);
    }
    MIX(numobjs);
    for (int i = 0; i < numobjs; i++) {
        const objtype *ob = actor_at(i);
        MIX(obj_order[i]);
        MIX(ob->active); MIX(ob->ticcount); MIX(ob->obclass);
        MIX(actor_state_number(ob->state)); MIX(ob->flags); MIX(ob->distance);
        MIX(ob->dir); MIX(ob->x); MIX(ob->y); MIX(ob->tilex); MIX(ob->tiley);
        MIX(ob->areanumber); MIX(ob->hitpoints); MIX(ob->speed); MIX(ob->temp2);
        MIX(ob->temp1); MIX(ob->angle);
    }
    {
        uint8_t free_list[MAXACTORS];
        const int nfree = actor_free_slots(free_list);
        for (int i = 0; i < nfree; i++)
            MIX(free_list[i]);
    }
    MIX(gamestate.killx); MIX(gamestate.killy); MIX(face_funny_count());
    MIX(player.x); MIX(player.y); MIX(player.angle); MIX(player.anglefrac);
    MIX(player.tilex); MIX(player.tiley); MIX(player_area);
    MIX(current_level); MIX(gamestate.difficulty); MIX(gamestate.health);
    MIX(gamestate.ammo); MIX(gamestate.lives); MIX(gamestate.score);
    MIX(gamestate.oldscore); MIX(gamestate.killcount); MIX(gamestate.killtotal);
    MIX(gamestate.nextextra); MIX(gamestate.keys); MIX(gamestate.weapon);
    MIX(gamestate.bestweapon); MIX(gamestate.chosenweapon);
    MIX(gamestate.treasurecount); MIX(gamestate.treasuretotal);
    MIX(gamestate.secretcount); MIX(gamestate.secrettotal);
    MIX(gamestate.faceframe); MIX(gamestate.timecount);
    for (int i = 0; i < 8; i++) {
        MIX(level_ratios[i].kill); MIX(level_ratios[i].secret);
        MIX(level_ratios[i].treasure); MIX(level_ratios[i].time);
    }
    MIX(us_rnd_index()); MIX(face_count());
#undef MIX
    return h;
}

/* ------------------------------------------------------------------ */

static void capture(const char *label)
{
    char path[640];
    FILE *f;

    snprintf(path, sizeof path, "%s/%02d_%s.ppm", outdir, shot++, label);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    /* The whole screen, the way the ROM composes it: the 3D view on top of
     * the status bar, or a text page centered on black, and the fade
     * blended over everything. */
    const bool full = game_fullscreen();
    const int fade = game_fade();
    const uint16_t fc = game_fade_color();
    if (!full && game_phase() != PH_MENU)   /* Change View writes the bar's place */
        status_draw();
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const int top = (SCREEN_H - VIEW_H) / 2;
            /* the view from whichever buffer holds it, rows or columns */
            uint16_t c = full ? (y >= top && y < top + VIEW_H ? *view_pixel(x, y - top) : 0)
                       : y < VIEW_H ? *view_pixel(x, y) : statusbuf[y - VIEW_H][x];
            unsigned char rgb[3];
            for (int ch = 0; ch < 3; ch++) {
                /* main.c's fade_copy(), per 5-bit channel */
                const int shift = 11 - 5 * ch;
                int v = (c >> shift) & 31, t = (fc >> shift) & 31;
                v = v + (t - v) * fade / 255;
                rgb[ch] = (unsigned char)(v * 255 / 31);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("  %-14s  tile (%2d,%2d)  angle %3d\n",
           label, player.tilex, player.tiley, player.angle / 10);
}

/* One 70 Hz tic of game time. */
static void tic(int forward, int turn, int strafe)
{
    controls_t c = { .turn = turn, .forward = forward, .strafe = strafe };
    sd_update(1);
    move_doors(1);
    player_move(&c, 1);
    update_face(1);
}

/* Stand `away` tiles from a piece of scenery, looking at it. */
static bool view_static_at(int si, int away)
{
    static const int dx[4] = { 1, 0, -1, 0 };
    static const int dy[4] = { 0, 1, 0, -1 };
    static const int face[4] = { 1800, 900, 0, 2700 };   /* toward the prop */
    int sx = statobjlist[si].tilex, sy = statobjlist[si].tiley;

    for (int d = 0; d < 4; d++) {
        int px = sx + dx[d] * away, py = sy + dy[d] * away;
        bool clear = true;
        if (px < 1 || py < 1 || px >= MAPSIZE - 1 || py >= MAPSIZE - 1)
            continue;
        for (int k = 1; k <= away && clear; k++)
            if (blockmap[sy + dy[d] * k][sx + dx[d] * k])
                clear = false;
        if (!clear)
            continue;
        player.x = ((fixed)px << TILESHIFT) + TILEGLOBAL / 2;
        player.y = ((fixed)py << TILESHIFT) + TILEGLOBAL / 2;
        player.tilex = px;
        player.tiley = py;
        player.angle = face[d];
        return true;
    }
    return false;
}

static bool view_static(int si) { return view_static_at(si, 2); }

static const char *item_name(int n)
{
    switch (n) {
    case bo_gibs:       return "gibs";
    case bo_alpo:       return "dog food";
    case bo_firstaid:   return "first aid";
    case bo_key1:       return "gold key";
    case bo_key2:       return "silver key";
    case bo_cross:      return "cross";
    case bo_chalice:    return "chalice";
    case bo_bible:      return "bible";
    case bo_crown:      return "crown";
    case bo_clip:       return "clip";
    case bo_clip2:      return "clip2";
    case bo_machinegun: return "machine gun";
    case bo_chaingun:   return "gatling gun";
    case bo_food:       return "food";
    case bo_fullheal:   return "one up";
    default:            return "?";
    }
}

static int live_bonus_count(void)
{
    int n = 0;
    for (int i = 0; i < numstats; i++)
        if (statobjlist[i].itemnumber > block && statobjlist[i].shapenum >= 0)
            n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Enemies                                                             */
/* ------------------------------------------------------------------ */

static bool sim_fire_last;

/* One tic of PlayLoop(), in the ROM's order, with a frame drawn - the
 * renderer is what marks actors visible and wakes them. */
static void frame(int forward, int turn, bool fire)
{
    controls_t c = { .turn = turn, .forward = forward, .strafe = 0 };
    madenoise = false;
    sd_update(1);
    move_doors(1);
    move_pwalls(1);
    player_weapon(fire, sim_fire_last, 0, 1);
    sim_fire_last = fire;
    player_move(&c, 1);
    actors_think(1);
    update_palette_shifts(1);
    render_view();
}

static void place_player(int tx, int ty, int angle)
{
    player.x = ((fixed)tx << TILESHIFT) + TILEGLOBAL / 2;
    player.y = ((fixed)ty << TILESHIFT) + TILEGLOBAL / 2;
    player.tilex = tx;
    player.tiley = ty;
    player.angle = angle;
    player_area = area_at(tx, ty);
    connect_areas();
}

static const int8_t ddx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int8_t ddy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

/* An idle enemy of the class with `away` clear tiles in front of it (or
 * behind, if behind is set); the player is put at the far end, facing it. */
static objtype *setup_facing(int obclass, int away, bool behind)
{
    for (int i = 0; i < numobjs; i++) {
        objtype *ob = actor_at(i);
        int d, px, py, k;
        bool clear = true;

        if (ob->obclass != obclass || !(ob->flags & FL_SHOOTABLE)
            || (ob->flags & FL_ATTACKMODE) || ob->dir >= nodir || (ob->dir & 1)
            || ((ob->flags & FL_AMBUSH) && obclass != bossobj))
            continue;
        d = behind ? (ob->dir + 4) % 8 : ob->dir;
        for (k = 1; k <= away && clear; k++) {
            int tx = ob->tilex + ddx[d] * k, ty = ob->tiley + ddy[d] * k;
            if (tx < 1 || ty < 1 || tx >= MAPSIZE - 1 || ty >= MAPSIZE - 1
                || blockmap[ty][tx] || actorat[ty][tx] || tilemap[ty][tx])
                clear = false;
        }
        if (!clear)
            continue;
        px = ob->tilex + ddx[d] * away;
        py = ob->tiley + ddy[d] * away;
        place_player(px, py, (d * 45 + 180) % 360 * 10);
        return ob;
    }
    return NULL;
}

static const char *const class_names[] = {
    "inert", "guard", "SS", "dog", "Hans", "BJ", "officer", "mutant",
    "Schabbs", "fake Hitler", "Mecha Hitler", "Hitler", "Gretel",
    "Giftmacher", "Fettgesicht", "ghost", "needle", "fire", "rocket",
    "spectre", "Angel", "Trans", "Uber", "Wilhelm", "Death Knight",
    "hrocket", "spark"
};
#define NUM_CLASSES ((int)(sizeof class_names / sizeof class_names[0]))

static const char *classname(int c)
{
    return c >= 0 && c < NUM_CLASSES ? class_names[c] : "?";
}

static void enemy_tests(void)
{
    printf("\nenemies:\n");

    /* spawn counts: each difficulty adds actors */
    {
        static const char *dname[4] = { "baby", "easy", "medium", "hard" };
        for (int d = gd_easy; d <= gd_hard; d++) {
            int total[32] = { 0 };
            printf("  %-6s:", dname[d]);
            for (int l = 0; l < wolf_num_levels; l++) {
                game_new();
                gamestate.difficulty = d;
                level_load(l);
                for (int i = 0; i < numobjs; i++)
                    total[actor_at(i)->obclass]++;
                printf(" %2d", gamestate.killtotal);
            }
            printf("   (guards %d, SS %d, dogs %d, Hans %d, dead guards %d)\n",
                   total[guardobj], total[ssobj], total[dogobj], total[bossobj],
                   total[inertobj]);
        }
    }

    /* a standing guard, seen from four sides: CalcRotate picks the view */
    {
        game_new();
        level_load(0);
        objtype *ob = setup_facing(guardobj, 3, false);
        if (ob) {
            int front, side, back;
            render_view();
            front = actor_shape(ob);
            capture("enemy_guard_front");
            place_player(ob->tilex - ddx[(ob->dir + 2) % 8] * 3,
                         ob->tiley - ddy[(ob->dir + 2) % 8] * 3, 0);
            /* face the guard from its side */
            {
                int d = (ob->dir + 6) % 8;       /* player looks this way */
                player.angle = d * 45 * 10;
            }
            render_view();
            side = actor_shape(ob);
            place_player(ob->tilex, ob->tiley, 0);
            setup_facing(guardobj, 3, true);
            render_view();
            back = actor_shape(ob);
            printf("  guard at (%d,%d) facing dir %d: sprite %d from the front,"
                   " %d from the side, %d from behind (SPR_GRD_S_1 is %d)\n",
                   ob->tilex, ob->tiley, ob->dir, front, side, back, SPR_GRD_S_1);
        }
    }

    /* sight: stand in front of a guard and wait */
    game_new();
    level_load(0);
    memset(&sb, 0, sizeof sb);
    objtype *g = setup_facing(guardobj, 3, false);
    if (g) {
        int t;
        for (t = 0; t < 300 && !(g->flags & FL_ATTACKMODE); t++)
            frame(0, 0, false);
        printf("  guard sees the player 3 tiles ahead: %s after %d tics,"
               " HALT digi %s\n", g->flags & FL_ATTACKMODE ? "alerted" : "NOT alerted",
               t, sb.last_digi == 0 ? "played" : "not played");

        /* ...and shoots: stand still and count the damage */
        int hits = 0, shots = 0, before = gamestate.health = 999;
        for (t = 0; t < 70 * 10 && !player_died; t++) {
            int h = gamestate.health, d = sb.digi_calls;
            frame(0, 0, false);
            if (gamestate.health < h) hits++;
            if (sb.digi_calls != d && sb.last_digi == 21) shots++;
        }
        printf("  10s standing in front of it: %d shots, %d hits, %d damage;"
               " palette shift now %d\n", shots, hits, before - gamestate.health,
               (int)(wolf_palette - wolf_palettes) / 256);
        capture("enemy_guard_shooting");
    }

    /* noise: a guard facing away ignores the player until a gun goes off */
    game_new();
    level_load(0);
    g = setup_facing(guardobj, 4, true);
    if (g) {
        int t;
        for (t = 0; t < 140; t++)
            frame(0, 0, false);
        bool quiet = !(g->flags & FL_ATTACKMODE);
        gamestate.ammo = 99;
        /* fire away from it - into the wall behind - so the noise is all
         * that reaches the guard, not the bullet */
        player.angle = (player.angle + 1800) % 3600;
        for (t = 0; t < 140 && !(g->flags & FL_ATTACKMODE); t++)
            frame(0, 0, t == 0);
        printf("  guard with its back turned: %s for 2s, %s %d tics after a shot\n",
               quiet ? "idle" : "ALERTED (wrong)",
               g->flags & FL_ATTACKMODE ? "alerted" : "NOT alerted", t);
    }

    /* killing one: tap the pistol at a guard 2 tiles ahead */
    game_new();
    level_load(0);
    g = setup_facing(guardobj, 2, false);
    if (g) {
        int t, taps = 0, score0 = gamestate.score, kills0 = gamestate.killcount;
        gamestate.ammo = 99;
        gamestate.health = 9999;
        for (t = 0; t < 70 * 15 && (g->flags & FL_SHOOTABLE); t++) {
            bool press = (t % 20) == 0;
            if (press) taps++;
            frame(0, 0, press);
        }
        int gx = g->x >> TILESHIFT, gy = g->y >> TILESHIFT, dropped = 0;
        for (t = 0; t < 70; t++)
            frame(0, 0, false);                 /* let it fall */
        for (int i = 0; i < numstats; i++)
            if (statobjlist[i].itemnumber == bo_clip2 && statobjlist[i].tilex == gx
                && statobjlist[i].tiley == gy && statobjlist[i].shapenum >= 0)
                dropped = 1;
        printf("  shooting a guard: %s after %d trigger pulls; score %+d,"
               " kills %d->%d, clip dropped %s, sprite now %d (DEAD is %d)\n",
               g->flags & FL_SHOOTABLE ? "STILL ALIVE" : "dead", taps,
               gamestate.score - score0, kills0, gamestate.killcount,
               dropped ? "yes" : "no", g->state->shapenum, SPR_GRD_DEAD);
        capture("enemy_guard_dead");
    }

    /* a guard chasing the player opens a door on the way */
    game_new();
    level_load(0);
    actors_init();
    spawn_map_actor(110, 34, 57, false);        /* guard facing west */
    g = actor_at(0);
    {
        int door = tilemap[57][32] & 0x3f, t, opened = 0;
        place_player(30, 57, 0);
        gamestate.health = 9999;
        /* open and connect the rooms, wake the guard, then shut the door
         * in its face without disconnecting them */
        open_door(door);
        for (t = 0; t < 70; t++) frame(0, 0, false);
        gamestate.ammo = 99;
        g->hitpoints = 1000;                    /* the wake-up shot must not kill */
        frame(0, 0, true);
        for (t = 0; t < 30; t++) frame(0, 0, false);
        doorobjlist[door].action = dr_closed;
        doorposition[door] = 0;
        blockmap[57][32] = 1;
        for (t = 0; t < 70 * 6 && g->tilex > 31; t++) {
            frame(0, 0, false);
            if (doorobjlist[door].action == dr_opening) opened = 1;
        }
        printf("  guard chasing through a shut door: %s, reached x=%d after %d tics\n",
               opened ? "opened it" : "did NOT open it", g->tilex, t);
    }

    /* dogs bite */
    game_new();
    level_load(0);
    actors_init();
    spawn_map_actor(138, 33, 57, false);        /* dog patrol */
    g = actor_at(0);
    {
        int t, bitten = 0;
        place_player(35, 57, 0);                /* facing away, east */
        gamestate.health = 9999;
        gamestate.ammo = 99;
        frame(0, 0, true);                      /* noise wakes it */
        for (t = 0; t < 70 * 5; t++) {
            int h = gamestate.health;
            frame(0, 0, false);
            if (gamestate.health < h) bitten++;
        }
        printf("  dog next to the player for 5s: %s, bitten %d times\n",
               g->flags & FL_ATTACKMODE ? "chasing" : "NOT chasing", bitten);
    }

    /* dying: health 1, an awake guard, then Died()'s turn and a lost life */
    game_new();
    level_load(0);
    g = setup_facing(guardobj, 3, false);
    if (g) {
        int t, turn;
        gamestate.health = 1;
        player.angle = (player.angle + 1800) % 3600;    /* back to it */
        for (t = 0; t < 70 * 20 && !player_died; t++)
            frame(0, 0, false);
        for (turn = 0; turn < 500 && !player_face_killer(1); turn++)
            ;
        printf("  with 1 health: %s after %d tics, killed by a %s; turned to"
               " face it in %d tics\n", player_died ? "died" : "SURVIVED", t,
               killerobj ? classname(killerobj->obclass) : "?", turn);
        gamestate.score = 1234;
        gamestate.oldscore = 1000;
        bool more = player_lose_life();
        printf("  losing a life: %s, lives %d, health %d, ammo %d, score %ld\n",
               more ? "continues" : "GAME OVER", gamestate.lives, gamestate.health,
               gamestate.ammo, (long)gamestate.score);
        gamestate.lives = 0;
        printf("  losing the last one: %s\n",
               player_lose_life() ? "CONTINUES (wrong)" : "game over");
    }

    /* Hans Grosse, and the key he drops */
    game_new();
    level_load(8);
    g = NULL;
    for (int i = 0; i < numobjs; i++)
        if (actor_at(i)->obclass == bossobj) g = actor_at(i);
    if (g) {
        int t, hp = g->hitpoints, key = 0, score0 = gamestate.score;
        objtype *h = setup_facing(bossobj, 2, false);
        if (!h) {                               /* no room in front: any side */
            for (int d = 0; d < 8 && !h; d += 2) {
                g->dir = d;
                h = setup_facing(bossobj, 2, false);
            }
        }
        gamestate.health = 30000;
        gamestate.ammo = 99;
        gamestate.bestweapon = gamestate.weapon = gamestate.chosenweapon = wp_chaingun;
        render_view();
        capture("enemy_hans");
        for (t = 0; t < 70 * 60 && (g->flags & FL_SHOOTABLE); t++) {
            /* he dodges once awake, so keep him in the sights, as a player
             * would */
            double a = atan2((double)(player.y - g->y), (double)(g->x - player.x));
            if (a < 0)
                a += 2 * M_PI;
            player.angle = (int)(a / (2 * M_PI) * FINEANGLES) % FINEANGLES;
            gamestate.ammo = 99;
            frame(0, 0, true);
        }
        for (int i = 0; i < 70; i++) frame(0, 0, false);
        for (int i = 0; i < numstats; i++)
            if (statobjlist[i].itemnumber == bo_key1 && statobjlist[i].shapenum >= 0)
                key = 1;
        printf("  Hans: %d hit points; %s after %.1fs of gatling fire, score %+d,"
               " gold key dropped %s\n", hp,
               g->flags & FL_SHOOTABLE ? "STILL ALIVE" : "dead", t / 70.0,
               gamestate.score - score0, key ? "yes" : "no");
    }

    /* the bonus flash */
    clear_palette_shifts();
    start_bonus_flash();
    update_palette_shifts(1);
    printf("  bonus flash palette %d (white shifts are 7..9)\n",
           (int)(wolf_palette - wolf_palettes) / 256);
    clear_palette_shifts();
}

static int find_door_ahead(void)
{
    /* The tile Cmd_Use() would operate on, if it holds a door. */
    int x = player.tilex, y = player.tiley, t;
    if (player.angle < FINEANGLES / 8 || player.angle > 7 * FINEANGLES / 8) x++;
    else if (player.angle < 3 * FINEANGLES / 8) y--;
    else if (player.angle < 5 * FINEANGLES / 8) x--;
    else y++;
    if (x < 0 || y < 0 || x >= MAPSIZE || y >= MAPSIZE) return -1;
    t = tilemap[y][x];
    return (t & 0x80) ? (t & 0x7f) : -1;
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* The end-of-floor screen                                             */
/* ------------------------------------------------------------------ */

/* Stand beside an elevator switch on this floor, facing it; returns what
 * using it did, or -1 if the floor has no switch reachable from the side. */
static int use_elevator(int level, bool want_secret)
{
    level_load(level);
    for (int y = 1; y < MAPSIZE - 1; y++) {
        for (int x = 1; x < MAPSIZE - 1; x++) {
            if (tilemap[y][x] != ELEVATORTILE)
                continue;
            for (int side = -1; side <= 1; side += 2) {
                int px = x + side;
                if (blockmap[y][px])
                    continue;
                if ((plane0[y][px] == ALTELEVATORTILE) != want_secret)
                    continue;
                place_player(px, y, side < 0 ? 0 : 1800);
                return player_use(false);
            }
        }
    }
    return -1;
}

/* Run the screen a tic at a time, as the ROM would at 70 fps. A button is
 * pressed `skip_at` tics in (0: never), and again once the tally is over
 * to leave. Captures `name`_t<shot_at> and `name`_done if a name is given.
 * Returns the tic the tally finished on. */
static int run_intermission(int level, int skip_at, int shot_at,
                            const char *name)
{
    int t, finished = 0;
    char label[40];

    sd_init();
    memset(&sb, 0, sizeof sb);
    intermission_start(level);
    for (t = 1; t < 20000; t++) {
        bool ack = (t == skip_at) || (finished && t == finished + 70);
        sd_update(1);
        if (intermission_update(1, ack))
            break;
        if (name && t == shot_at) {
            snprintf(label, sizeof label, "%s_t%d", name, t);
            capture(label);
        }
        if (!finished && !intermission_tallying()) {
            finished = t;
            if (name) {
                snprintf(label, sizeof label, "%s_done", name);
                capture(label);
            }
        }
    }
    if (t >= 20000)
        printf("  NEVER LEFT the screen\n");
    return finished;
}

static void intermission_tests(void)
{
    int r, l, t;
    int32_t before;

    printf("\nend of floor:\n");

    r = use_elevator(0, false);
    printf("  floor 1 elevator: %s, goes to floor %d\n",
           r == USE_ELEVATOR ? "normal (right)" : "WRONG", next_floor(0, false) + 1);
    for (l = 0; l < wolf_num_levels; l++)
        if ((r = use_elevator(l, true)) >= 0)
            break;
    printf("  secret elevator on floor %d: %s, goes to floor %d; back to floor %d\n",
           l + 1, r == USE_SECRET_ELEVATOR ? "secret (right)" : "WRONG",
           next_floor(l, true) + 1, next_floor(9, false) + 1);

    /* Everything found, 1:00 on floor 1 (par 1:30): 30 s x 500 + 3 x 10000,
     * if the floor has secrets. */
    level_load(0);
    gamestate.killtotal = 20;       gamestate.killcount = 20;
    gamestate.secretcount = gamestate.secrettotal;
    gamestate.treasurecount = gamestate.treasuretotal;
    gamestate.timecount = 60 * 70;
    before = gamestate.score;
    t = run_intermission(0, 0, 150, "stats");
    printf("  all found, 1:00: +%ld (expect %d), tally took %d tics, "
           "%d ENDBONUS1 %d ENDBONUS2 %d PERCENT100, music %d\n",
           (long)(gamestate.score - before),
           15000 + 10000 * (2 + (gamestate.secrettotal > 0)), t,
           sb.count_by_sound[ENDBONUS1SND], sb.count_by_sound[ENDBONUS2SND],
           sb.count_by_sound[PERCENT100SND], sb.last_music);

    /* The same, skipped a second in: the same points, far sooner. */
    level_load(0);
    gamestate.killtotal = 20;       gamestate.killcount = 20;
    gamestate.secretcount = gamestate.secrettotal;
    gamestate.treasurecount = gamestate.treasuretotal;
    gamestate.timecount = 60 * 70;
    before = gamestate.score;
    t = run_intermission(0, 70, 0, NULL);
    printf("  skipped:          +%ld, tally took %d tics\n",
           (long)(gamestate.score - before), t);

    /* Half the kills, no treasure, over par on floor 2. */
    level_load(1);
    gamestate.killtotal = 20;       gamestate.killcount = 10;
    gamestate.secretcount = 0;
    gamestate.treasurecount = 0;
    gamestate.timecount = (4 * 60 + 5) * 70;
    before = gamestate.score;
    t = run_intermission(1, 0, 0, "stats_partial");
    printf("  partial, 4:05:    +%ld (expect 0), %d NOBONUS\n",
           (long)(gamestate.score - before), sb.count_by_sound[NOBONUSSND]);

    /* The secret floor: a flat 15000. */
    level_load(9);
    before = gamestate.score;
    run_intermission(9, 0, 0, "stats_secret");
    printf("  secret floor:     +%ld (expect 15000)\n",
           (long)(gamestate.score - before));
}

/* ------------------------------------------------------------------ */
/* BJ's face                                                           */
/* ------------------------------------------------------------------ */

static const char *face_name(int pic)
{
    static char buf[16];
    if (pic == PIC_GOTGATLING)
        return "GOTGATLING";
    if (pic == PIC_FACE + 21)
        return "FACE8A (dead)";
    snprintf(buf, sizeof buf, "FACE%d%c", (pic - PIC_FACE) / 3 + 1,
             'A' + (pic - PIC_FACE) % 3);
    return buf;
}

static void face_tests(void)
{
    static const int healths[] = { 100, 85, 84, 69, 50, 20, 4, 1, 0 };
    statobj_t gun = { 0, 0, SPR_STAT_0, bo_chaingun };
    int t;

    printf("\nfaces:\n");
    level_load(0);
    actors_init();
    sd_init();

    for (int i = 0; i < (int)(sizeof healths / sizeof healths[0]); i++) {
        gamestate.health = healths[i];
        gamestate.faceframe = 1;
        printf("  health %3d: %s\n", healths[i], face_name(status_face_pic()));
    }

    /* the gatling gun: the grin, held while an AdLib pickup sound plays */
    gamestate.health = 100;
    gamestate.faceframe = 0;
    gamestate.bestweapon = wp_pistol;
    get_bonus(&gun);
    printf("  gatling pickup: %s, sound %s\n", face_name(status_face_pic()),
           sd_sound_playing() == GETGATLINGSND ? "on the AdLib voice"
           : "digitized (SD_SoundPlaying() cannot see it)");
    capture("face_gatling");
    for (t = 1; t < 2000 && status_face_pic() == PIC_GOTGATLING; t++) {
        sd_update(1);
        update_face(1);
    }
    printf("  grin lasted %d tics (the pickup sound is %d), then %s\n", t,
           (int)(wolf_sound_info[GETGATLINGSND] & 0xffff),
           face_name(status_face_pic()));

    /* damage, healing and a new pickup */
    gun.shapenum = SPR_STAT_0;
    gamestate.bestweapon = wp_pistol;
    get_bonus(&gun);
    take_damage(10, NULL);
    printf("  grin, then 10 damage: %s\n", face_name(status_face_pic()));
    gun.shapenum = SPR_STAT_0;
    gamestate.bestweapon = wp_pistol;
    get_bonus(&gun);
    heal_self(10);
    printf("  grin, then healed:    %s\n", face_name(status_face_pic()));
    gun.shapenum = SPR_STAT_0;
    gamestate.bestweapon = wp_chaingun;     /* already had one */
    get_bonus(&gun);
    printf("  a second gatling gun: %s\n", face_name(status_face_pic()));
    face_redraw();
    take_damage(200, NULL);
    printf("  killed:               %s\n", face_name(status_face_pic()));
    game_new();
}

/* ------------------------------------------------------------------ */
/* The whole flow through game.c: fades, a floor start, the victory    */
/* ------------------------------------------------------------------ */

static const char *phase_names[] = {
    "PSYCHED_IN", "PSYCHED", "PSYCHED_OUT", "FIZZLE_IN", "FADE_IN",
    "PLAY", "DEATH_TURN", "DEATH_FIZZLE", "DEATH_WAIT", "GAME_OVER",
    "ELEVATOR", "LEVEL_OUT", "INTER_IN", "INTER", "INTER_OUT",
    "VICTORY_OUT", "VICTORY_IN", "VICTORY", "VICTORY_DONE",
    "ENDTEXT_IN", "ENDTEXT", "ENDTEXT_OUT",
    "PAUSED", "MENU_OUT", "MENU", "RESUME_IN", "NEW_GAME_OUT",
    "PG13_IN", "PG13", "PG13_OUT", "TITLE_IN", "TITLE", "TITLE_OUT",
    "DEMO_IN", "DEMO_FIZZLE", "DEMO", "DEMO_OUT", "ATTRACT_OUT",
    "LOAD_GAME_OUT",
    "SCORES_IN", "SCORES_NAME", "SCORES_WAIT", "SCORES_OUT",
    "SIGNON", "SIGNON_OUT",
    "MAP", "WARP_OUT",
    "DEATHCAM_WAIT", "DEATHCAM_FIZZLE", "DEATHCAM_TEXT", "DEATHCAM_IN",
    "SPEAR", "COLLAPSE", "ENDSPEAR_IN", "ENDSPEAR", "ENDSPEAR_OUT"
};

static int flow_tic;

/* One 70 Hz frame of the game, logging each change of phase. */
static void game_tic(const controls_t *c, const buttons_t *b)
{
    int before = game_phase();
    game_frame(c, b, 1, 70);
    flow_tic++;
    if (game_phase() != before)
        printf("    tic %5d  %-12s -> %-12s fade %3d\n", flow_tic,
               phase_names[before], phase_names[game_phase()], game_fade());
}

static void run_until(int phase, int limit, const buttons_t *b)
{
    controls_t c = { 0 };
    for (int i = 0; i < limit && game_phase() != phase; i++)
        game_tic(&c, b);
    if (game_phase() != phase)
        printf("    NEVER REACHED %s\n", phase_names[phase]);
}

/* A press on the pad, then long enough for the gun or a fade to settle. */
static void menu_press(const buttons_t *b, int settle)
{
    const buttons_t none = { 0 };
    const controls_t c = { 0 };
    game_tic(&c, b);
    for (int i = 0; i < settle; i++)
        game_tic(&c, &none);
}

/* Change View's preview size: where the gray stops along the middle row */
static int preview_size(void)
{
    const uint16_t gray = wolf_palette[127];
    int x = 0;
    while (x < VIEW_W / 2 && *view_pixel(x, VIEW_H / 2) == gray)
        x++;
    return (VIEW_W - 2 * (x + 1)) / 16;
}

/* ------------------------------------------------------------------ */
/* Saved games: a floor played rough, saved, played on, loaded, played */
/* again - the two runs after the save must not differ at all          */
/* ------------------------------------------------------------------ */

static uint32_t chaos_seed;
static bool     chaos_fire, chaos_use;

static int chaos_rand(void)
{
    chaos_seed = chaos_seed * 1103515245u + 12345u;
    return (int)((chaos_seed >> 16) & 0x7fff);
}

/* PlayLoop()'s order, with inputs from a fixed random sequence, and doors,
 * push walls and pickups nudged so that every part of a save gets used. */
static void chaos_tic(bool may_fire)
{
    controls_t c = { 0 };
    const bool fire = may_fire && chaos_rand() % 3 == 0;
    const bool use  = chaos_rand() % 25 == 0;

    c.turn    = (chaos_rand() % 5 - 2) * BASEMOVE;
    c.forward = chaos_rand() % 4 == 0 ? BASEMOVE : -RUNMOVE;
    if (chaos_rand() % 6 == 0)
        c.strafe = (chaos_rand() % 2 ? 1 : -1) * BASEMOVE;

    if (doornum && chaos_rand() % 60 == 0)
        operate_door(chaos_rand() % doornum);
    if (numstats && chaos_rand() % 90 == 0)
        get_bonus(&statobjlist[chaos_rand() % numstats]);
    if (!pwallstate && chaos_rand() % 200 == 0) {
        for (int y = 1; y < MAPSIZE - 1; y++)
            for (int x = 1; x < MAPSIZE - 1; x++)
                if (plane1[y][x] == PUSHABLETILE && !pwallstate)
                    for (int d = 0; d < 4 && !pwallstate; d++)
                        push_wall(x, y, d);
    }

    madenoise = false;
    move_doors(1);
    move_pwalls(1);
    update_face(1);
    if (!player_attacking()) {
        if (use)
            player_use(chaos_use);
        player_weapon(fire, chaos_fire, 0, 1);
        player_move(&c, 1);
        chaos_fire = fire;
        chaos_use  = use;
    } else {
        player_move(&c, 1);
        player_weapon(fire, chaos_fire, 0, 1);
        chaos_fire = fire && chaos_fire;
        chaos_use  = false;
    }
    actors_think(1);
    update_palette_shifts(1);
    render_view();
    sd_update(1);
    gamestate.timecount++;

    if (player_died) {                      /* keep going, the same both times */
        player_died = false;
        gamestate.health = 100;
    }
}

static uint32_t chaos_trace(uint32_t seed, int tics)
{
    uint32_t h = 0;
    chaos_seed = seed;
    chaos_fire = chaos_use = false;
    render_view();                          /* the aim GunAttack() reads */
    for (int t = 0; t < tics; t++) {
        chaos_tic(true);
        h = h * 31 + state_digest();
    }
    return h;
}

static void save_tests(void)
{
    static uint8_t buf[SAVE_MEDIUM_BYTES];
    size_t total = 0, largest = 0;
    int good = 0;

    printf("\nsaved games - each floor on \"I am Death incarnate!\": 3000 tics"
           " of rough play, save, 400 more, load, the same 400 again:\n");
    memset(sram, 0, sizeof sram);
    save_init();

    for (int f = 0; f < wolf_num_levels; f++) {
        const int slot = f % SAVE_SLOTS;
        uint32_t digest, seed, run1, run2;
        char name[SAVE_NAME_LEN + 1];
        int t, changed = 0, taken = 0, open = 0;
        size_t size;
        bool saved, loaded, same_state;

        game_new();
        gamestate.difficulty = gd_hard;
        intermission_new_game();
        level_ratios[3].kill = 77;          /* something to carry */
        dos_exact = false;
        us_set_rnd_index(f * 37);
        level_load(f);

        chaos_seed = 1000 + f;
        chaos_fire = chaos_use = false;
        for (t = 0; t < 3000; t++)
            chaos_tic(true);
        for (t = 0; t < 200 && player_attacking(); t++)
            chaos_tic(false);               /* not in the middle of a shot */
        sd_stop_sound();

        for (int i = 0; i < numobjs; i++)
            changed += (actor_at(i)->flags & FL_ATTACKMODE) != 0;
        for (int i = 0; i < numstats; i++)
            taken += statobjlist[i].shapenum == -1;
        for (int i = 0; i < doornum; i++)
            open += doorobjlist[i].action != dr_closed;
        if (f < 2) {
            char label[32];
            snprintf(label, sizeof label, "map_floor%d_explored", f + 1);
            automap_draw();                 /* what the rough play has seen */
            fade_in(1);                     /* no fade left over from before */
            fade_update(1);
            capture(label);
            printf("  floor %d's map after the rough play: %d tiles seen\n",
                   f + 1, automap_count());
        }

        digest = state_digest();
        size   = save_state(buf, sizeof buf);
        snprintf(name, sizeof name, "Floor %d", f + 1);
        saved  = save_game(slot, name);
        seed   = chaos_seed;
        run1   = chaos_trace(seed, 400);

        us_set_rnd_index((us_rnd_index() + 99) & 0xff);  /* loading must not care */
        loaded = load_game(slot);
        same_state = state_digest() == digest;
        sd_stop_sound();
        run2 = chaos_trace(seed, 400);

        printf("  floor %2d: %4zu bytes (%3d actors, %2d awake; %2d items gone;"
               " %d doors open) \"%s\": saved %d loaded %d, state %s,"
               " 400 tics %s\n", f + 1, size, numobjs, changed, taken, open,
               name, saved, loaded, same_state ? "same" : "DIFFERENT",
               run1 == run2 ? "same" : "DIFFERENT");
        good += saved && loaded && same_state && run1 == run2;
        total += size;
        if (size > largest)
            largest = size;
    }
    printf("  %d of %d floors come back exactly; largest save %zu bytes, the"
           " largest ten would need %zu of %zu bytes\n", good, wolf_num_levels,
           largest, largest * 10, save_space());
    (void)total;

    /* The worst a player can do to a save's size: every enemy on the
     * floor killed where it was chased to, and every item taken. */
    {
        static const int dead_state[] = { -1, 21, 71, 42, 82 };  /* by obclass */
        size_t worst = 0;
        int worst_floor = 0;
        for (int f = 0; f < wolf_num_levels; f++) {
            size_t size;
            game_new();
            gamestate.difficulty = gd_hard;
            level_load(f);
            for (int i = 0; i < numobjs; i++) {
                objtype *ob = actor_at(i);
                if (ob->obclass == inertobj || ob->obclass > bossobj)
                    continue;
                ob->state = actor_state(dead_state[ob->obclass]);
                ob->ticcount = 0;
                ob->flags = (ob->flags & ~FL_SHOOTABLE) | FL_ATTACKMODE | FL_FIRSTATTACK;
                ob->x += 0x12345;
                ob->y -= 0x6789;
                ob->dir = nodir;
                ob->hitpoints = -3;
                ob->speed *= 3;
                ob->distance = 0x4321;
                ob->temp2 = 0;
                ob->active = true;
            }
            for (int i = 0; i < numstats; i++)
                if (statobjlist[i].itemnumber >= bo_gibs)
                    statobjlist[i].shapenum = -1;
            size = save_state(buf, sizeof buf);
            if (size > worst) {
                worst = size;
                worst_floor = f;
            }
        }
        printf("  every enemy killed and every item taken: largest save %zu bytes"
               " (floor %d); ten of those need %zu\n", worst, worst_floor + 1,
               worst * 10);
    }

    /* A damaged save is not offered, and the ones after it are unharmed.
     * Slot 2's data follows slot 1's, whose length is the first directory
     * entry's first four bytes. */
    {
        const uint8_t *d = sram + 16;
        const uint32_t off = 16 + SAVE_SLOTS * 40
                           + ((uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]);
        sram[off + 20] ^= 0x55;
        save_init();
        printf("  slot 2 with a byte flipped: %s; slot 3 %s\n",
               save_slot_used(1) ? "STILL OFFERED" : "shown empty",
               save_slot_used(2) && load_game(2) ? "still loads" : "LOST");
    }
    memset(sram, 0, sizeof sram);
}

static void flow_tests(void)
{
    buttons_t none = { 0 }, press = { 0 }, up = { 0 }, down = { 0 };
    buttons_t back = { 0 }, start = { 0 }, pause = { 0 };
    controls_t c = { 0 };
    int ex = -1, ey = -1, t;

    press.use = true;
    up.up = true;
    down.down = true;
    back.back = true;
    start.menu = true;
    pause.pause = true;

    printf("\ngame flow - start-up and the attract loop:\n");
    sd_init();
    memset(sram, 0, sizeof sram);
    save_init();
    sb.last_music = -2;
    game_init();
    {
        /* "One moment..." for half a second (35 tics), no lights yet and
         * deaf to buttons, then the lights and the prompt in yellow over it */
        const uint16_t yellow = wolf_palette[14];
        int prompt[2] = { 0, 0 }, lights = 0;
        for (t = 0; t < 30; t++)
            game_tic(&c, &none);
        capture("signon_one_moment");
        for (int y = 189; y < 200; y++)
            for (int x = 0; x < 300; x++)
                prompt[0] += *view_pixel(x, y) == yellow;
        for (int i = 0; i < 10; i++)
            lights += *view_pixel(51, 165 - 8 * i) == wolf_palette[0x6c - i];
        for (int i = 0; i < 5; i++)
            lights += *view_pixel(170, 83 + 23 * i) == yellow;
        menu_press(&press, 0);
        printf("  sign-on at 30 tics: %d prompt pixels, %d bars and boxes lit"
               " (expect 0 0); a button: still %s\n", prompt[0], lights,
               phase_names[game_phase()]);
        for (t = 0; t < 50; t++)
            game_tic(&c, &none);
        for (int y = 189; y < 200; y++)
            for (int x = 0; x < 300; x++)
                prompt[1] += *view_pixel(x, y) == yellow;
        printf("  sign-on at 81 tics: %d prompt pixels (expect some)\n",
               prompt[1]);
    }
    for (t = 0; t < 140; t++)                /* the sign-on screen waits */
        game_tic(&c, &none);
    capture("signon");
    {
        const uint16_t yellow = wolf_palette[14];
        int bars = 0, boxes = 0;
        for (int i = 0; i < 10; i++) {
            bars += *view_pixel(51, 165 - 8 * i) == wolf_palette[0x6c - i];
            bars += *view_pixel(91, 165 - 8 * i) == wolf_palette[0x6c - i];
            bars += *view_pixel(131, 165 - 8 * i) == wolf_palette[0x6c - i];
        }
        for (int i = 0; i < 5; i++)
            boxes += *view_pixel(170, 83 + 23 * i) == yellow;
        printf("  sign-on: %s after 3 s, fade %d; %d memory bars lit (expect 20:"
               " all main and EMS, no XMS), %d boxes lit (expect 3: joystick,"
               " AdLib, Sound Blaster), music %d (expect none yet)\n",
               phase_names[game_phase()], game_fade(), bars, boxes, sb.last_music);
    }
    menu_press(&press, 0);
    printf("  a button: %s, music %d (expect %d)\n", phase_names[game_phase()],
           sb.last_music, NAZI_NOR_MUS);
    capture("signon_working");
    run_until(PH_PG13, 100, &none);
    capture("attract_pg13");
    run_until(PH_TITLE, 800, &none);
    capture("attract_title");
    run_until(PH_TITLE_OUT, 1200, &none);
    run_until(PH_TITLE, 100, &none);
    capture("attract_credits");
    run_until(PH_TITLE_OUT, 800, &none);
    run_until(PH_TITLE, 100, &none);
    capture("attract_scores");
    run_until(PH_DEMO, 800, &none);
    printf("  demo 1 plays floor %d on difficulty %d, dos_exact %d\n",
           current_level + 1, gamestate.difficulty, dos_exact);
    for (t = 0; t < 70; t++)
        game_tic(&c, &none);
    capture("attract_demo_start");
    menu_press(&press, 0);                  /* a button ends it */
    run_until(PH_MENU, 100, &none);

    printf("\ngame flow - the menus:\n");
    menu_press(&none, 30);
    printf("  main menu: phase %s, fullscreen %d, music %d (expect %d)\n",
           phase_names[game_phase()], game_fullscreen(), sb.last_music,
           WONDERIN_MUS);
    capture("menu_main");

    /* the gun starts on New Game; down half-steps to Sound, then on to
     * Control, and back up */
    game_tic(&c, &down);
    for (t = 0; t < 4; t++)
        game_tic(&c, &none);
    capture("menu_halfstep");
    menu_press(&none, 30);
    menu_press(&down, 40);                  /* Control */
    menu_press(&up, 40);                    /* Sound */
    menu_press(&up, 40);                    /* New Game */
    printf("  moving the gun: MOVEGUN1 %d, MOVEGUN2 %d\n",
           sb.count_by_sound[MOVEGUN1SND], sb.count_by_sound[MOVEGUN2SND]);
    menu_press(&press, 40);
    capture("menu_episode");
    menu_press(&down, 40);                  /* Episode 2 */
    menu_press(&press, 10);
    capture("menu_order_message");
    printf("  episode 2: NOWAYSND %d\n", sb.count_by_sound[NOWAYSND]);
    menu_press(&press, 30);
    menu_press(&up, 40);                    /* Episode 1 */
    menu_press(&press, 40);
    capture("menu_skill");
    menu_press(&down, 40);                  /* I am Death incarnate! */
    capture("menu_skill_death");
    menu_press(&back, 40);                  /* back to the episodes... */
    printf("  B on the difficulty: %s\n", phase_names[game_phase()]);
    menu_press(&press, 40);                 /* ...and forward again */
    menu_press(&press, 5);
    capture("menu_fading_red");
    run_until(PH_PSYCHED_IN, 200, &none);
    printf("  new game: difficulty %d (expect 3), floor %d\n",
           gamestate.difficulty, current_level + 1);

    for (t = 0; t < 15; t++)
        game_tic(&c, &none);
    capture("flow_psyched_half");
    run_until(PH_PSYCHED, 100, &none);
    capture("flow_psyched");
    run_until(PH_FIZZLE_IN, 200, &none);
    run_until(PH_FADE_IN, 100, &none);
    for (t = 0; t < 15; t++)
        game_tic(&c, &none);
    capture("flow_fade_in_half");
    run_until(PH_PLAY, 100, &none);

    printf("  a death with lives left:\n");
    take_damage(200, NULL);
    run_until(PH_FIZZLE_IN, 1000, &none);
    for (t = 0; t < 10; t++)
        game_tic(&c, &none);
    capture("flow_respawn_fizzle");
    run_until(PH_PLAY, 100, &none);
    printf("  lives %d (expect 2), health %d, music restarted %d\n",
           gamestate.lives, gamestate.health, sb.last_music);

    printf("  pause:\n");
    menu_press(&pause, 20);
    capture("flow_paused");
    printf("    %s, music held %d\n", phase_names[game_phase()], sb.music_paused);
    menu_press(&press, 2);
    menu_press(&back, 2);
    printf("    A, then B: still %s (only the pause buttons carry on, as in"
           " Doom)\n", phase_names[game_phase()]);
    menu_press(&pause, 2);
    printf("    L + R again: %s, music held %d\n", phase_names[game_phase()],
           sb.music_paused);

    printf("  the map:\n");
    {
        buttons_t map = { 0 };
        const uint16_t marker = (31 << 11) | (4 << 6) | (4 << 1) | 1;
        const int seen = automap_count();
        const int x = player.x, y = player.y, health = gamestate.health;
        int red = 0;

        map.map = true;
        menu_press(&map, 10);
        for (int yy = 0; yy < VIEW_H; yy++)
            for (int xx = 0; xx < VIEW_W; xx++)
                red += *view_pixel(xx, yy) == marker;
        capture("flow_map");
        printf("    L: %s, %d of 4096 tiles seen (expect some, far from all),"
               " player marker %d pixels (expect 9)\n", phase_names[game_phase()],
               seen, red);

        /* the game runs on under it, as Doom's does: walk with it up */
        {
            controls_t walk = { .forward = -BASEMOVE, .turn = BASEMOVE / 2 };
            buttons_t a = { 0 };
            automap_clear();                /* so that any marks are new ones */
            for (t = 0; t < 60; t++)
                game_tic(&walk, &none);
            a.use = true;
            menu_press(&a, 2);              /* A is Use, and leaves it up */
            opt_show_fps = true;            /* to see it beside the labels */
            game_tic(&c, &none);
            capture("flow_map_walked");
            opt_show_fps = false;
            printf("    60 tics walking with it up: moved %s, marked %d tiles"
                   " afresh (expect some: the view still marks the map), health"
                   " %d (was %d); A: still %s\n",
                   player.x != x || player.y != y ? "yes" : "NO",
                   automap_count(), gamestate.health, health,
                   phase_names[game_phase()]);
        }
        {
            /* L + R: paused over the map, and back to the map after */
            const int px = player.x, py = player.y;
            controls_t walk = { .forward = -BASEMOVE };
            menu_press(&pause, 2);
            capture("flow_map_paused");
            printf("    L + R with the map up: %s, music held %d\n",
                   phase_names[game_phase()], sb.music_paused);
            for (t = 0; t < 20; t++)
                game_tic(&walk, &none);     /* nothing moves while paused */
            printf("    20 tics of walking while paused: moved %s\n",
                   player.x != px || player.y != py ? "YES" : "no");
            menu_press(&pause, 2);
            printf("    L + R again: %s, music held %d\n",
                   phase_names[game_phase()], sb.music_paused);
        }
        menu_press(&map, 2);
        printf("    L again: %s\n", phase_names[game_phase()]);
    }

    printf("  the menu from a game, by way of a pause:\n");
    menu_press(&pause, 2);
    menu_press(&start, 60);                 /* Start still opens the menu */
    printf("    paused, then Start: %s, music held %d\n",
           phase_names[game_phase()], sb.music_paused);
    capture("menu_ingame");
    menu_press(&down, 40);                  /* Sound (the gun was on New Game) */
    menu_press(&press, 40);
    capture("menu_sound");
    for (int i = 0; i < 4; i++)             /* 0 -> 1 -> 5 -> 6 -> 10 */
        menu_press(&down, 40);
    menu_press(&press, 40);
    capture("menu_sound_music_off");
    printf("    music None: sd_music_on %d, last music %d\n", sd_music_on,
           sb.last_music);
    menu_press(&down, 40);                  /* Nintendo 64 Audio again */
    menu_press(&press, 40);
    printf("    music Nintendo 64 Audio: sd_music_on %d, last music %d\n", sd_music_on,
           sb.last_music);
    capture("menu_sound_music_on");
    {
        int effects[2], digi[2];
        for (int i = 0; i < 3; i++)         /* 11 -> 10 -> 6 -> 5 */
            menu_press(&up, 40);
        menu_press(&press, 40);
        digi[0] = sd_digi_on;
        menu_press(&down, 40);              /* 6 */
        menu_press(&press, 40);
        digi[1] = sd_digi_on;
        for (int i = 0; i < 3; i++)         /* 6 -> 5 -> 1 -> 0 */
            menu_press(&up, 40);
        menu_press(&press, 40);
        effects[0] = sd_effects_on;
        capture("menu_sound_effects_off");
        menu_press(&down, 40);              /* 1 */
        menu_press(&press, 40);
        effects[1] = sd_effects_on;
        printf("    digitized None %d then Nintendo 64 Audio %d (expect 0 1); effects None"
               " %d then Nintendo 64 Audio %d (expect 0 1)\n", digi[0], digi[1],
               effects[0], effects[1]);
    }
    menu_press(&back, 40);

    for (int i = 0; i < 6; i++)             /* Sound -> Control, Extras, Load, Save, Change View, Read This! */
        menu_press(&down, 40);
    menu_press(&press, 40);
    printf("    Read This!: music %d (expect %d)\n", sb.last_music, CORNER_MUS);
    capture("menu_readthis1");
    {
        buttons_t right = { 0 };
        right.right = true;
        menu_press(&right, 2);
        menu_press(&right, 2);
        capture("menu_readthis3");
    }
    menu_press(&back, 60);
    for (int i = 0; i < 7; i++)             /* Change View, Save, Load, Extras, Control, Sound, New Game */
        menu_press(&up, 40);
    menu_press(&press, 40);
    menu_press(&press, 30);                 /* Episode 1: "currently in a game" */
    capture("menu_curgame_confirm");
    menu_press(&back, 40);                  /* no: back to the main menu */
    printf("    New Game in a game, answered no: %s, difficulty still %d\n",
           phase_names[game_phase()], gamestate.difficulty);
    menu_press(&down, 40);                  /* Sound */
    menu_press(&back, 30);                  /* B in a game: back to it */
    run_until(PH_PLAY, 100, &none);
    printf("    back in the game: music %d, fullscreen %d\n", sb.last_music,
           game_fullscreen());

    printf("  Save Game and Load Game:\n");
    {
        controls_t walk = { .forward = -RUNMOVE, .turn = BASEMOVE };
        buttons_t hold_b = { .back_held = true };
        uint32_t saved_digest;
        int saved_level, writes = sram_writes;

        for (t = 0; t < 120; t++)           /* somewhere else first */
            game_tic(&walk, &none);
        menu_press(&start, 60);
        menu_press(&down, 40);              /* Sound -> Control */
        menu_press(&down, 40);              /* Extras */
        menu_press(&down, 40);              /* Load Game */
        menu_press(&down, 40);              /* Save Game */
        menu_press(&press, 40);
        capture("menu_save");
        saved_digest = state_digest();      /* the game waits under the menu */
        saved_level = current_level;
        menu_press(&press, 2);              /* an empty slot starts blank */
        capture("menu_save_name");
        printf("    empty slot 1 starts with \"%s\" (expect blank)\n",
               line_input_text());
        for (int i = 0; i < 4; i++)
            menu_press(&press, 2);          /* AAAA */
        for (t = 0; t < 120; t++)           /* B held erases it, and no more */
            game_tic(&c, &hold_b);
        game_tic(&c, &none);
        printf("    B held 120 tics: \"%s\", still %s, SRAM writes %d\n",
               line_input_text(), phase_names[game_phase()], sram_writes - writes);
        menu_press(&up, 2);                 /* A */
        menu_press(&up, 2);                 /* B */
        menu_press(&press, 2);              /* BA */
        for (int i = 0; i < 9; i++)
            menu_press(&up, 2);             /* BJ */
        capture("menu_save_typed");
        menu_press(&start, 8);              /* Enter */
        capture("menu_saving");
        menu_press(&none, 60);
        printf("    saved to slot 1: used %d, name \"%s\", SRAM writes %d, back at %s\n",
               save_slot_used(0), save_slot_name(0), sram_writes - writes,
               phase_names[game_phase()]);

        menu_press(&press, 40);             /* Save Game again, the same slot */
        menu_press(&press, 30);
        capture("menu_save_overwrite");
        menu_press(&back, 40);              /* no */
        printf("    overwrite, answered no: SRAM writes %d, %s\n",
               sram_writes - writes, phase_names[game_phase()]);
        menu_press(&press, 30);             /* and again, yes this time */
        menu_press(&press, 4);
        printf("    overwrite, answered yes: the old name offered, \"%s\"\n",
               line_input_text());
        menu_press(&start, 60);

        menu_press(&press, 40);             /* Save Game, then give up */
        menu_press(&down, 40);              /* slot 2, empty */
        menu_press(&press, 2);
        menu_press(&back, 10);              /* B on the blank name: Esc */
        capture("menu_save_cancelled");
        printf("    B on the empty name: %s, slot 2 used %d, slot 1 \"%s\","
               " SRAM writes %d\n", phase_names[game_phase()], save_slot_used(1),
               save_slot_name(0), sram_writes - writes);
        menu_press(&up, 40);                /* the gun back on slot 1 */
        menu_press(&back, 40);              /* back to the main menu */
        menu_press(&back, 30);              /* and the game */
        run_until(PH_PLAY, 100, &none);

        for (t = 0; t < 200; t++)           /* wander off, and take a hit */
            game_tic(&walk, &none);
        take_damage(30, NULL);
        printf("    played on: state %s the save\n",
               state_digest() == saved_digest ? "STILL MATCHES" : "differs from");

        menu_press(&start, 60);
        menu_press(&up, 40);                /* Save Game -> Load Game */
        menu_press(&press, 40);
        capture("menu_load");
        menu_press(&press, 8);
        capture("menu_loading");
        run_until(PH_LOAD_GAME_OUT, 200, &none);
        run_until(PH_PSYCHED_IN, 200, &none);
        printf("    loaded: floor %d (expect %d), state %s, health %d, music %d\n",
               current_level + 1, saved_level + 1,
               state_digest() == saved_digest ? "the same as saved" : "DIFFERENT",
               gamestate.health, sb.last_music);
        run_until(PH_PLAY, 400, &none);
        capture("flow_after_load");
        menu_press(&start, 60);             /* leave the gun on Sound */
        menu_press(&up, 40);                /* Load Game -> Extras */
        menu_press(&up, 40);                /* Control */
        menu_press(&up, 40);                /* Sound */
        menu_press(&back, 30);
        run_until(PH_PLAY, 100, &none);
    }

    printf("  End Game:\n");
    menu_press(&start, 60);
    for (int i = 0; i < 7; i++)             /* Sound -> Control, Extras, Load, Save, Change View, Read This!, End Game */
        menu_press(&down, 40);
    menu_press(&press, 30);
    capture("menu_endgame_confirm");
    {
        /* no attacker to face, so Died() does not turn */
        const int angle = player.angle;
        menu_press(&press, 0);
        run_until(PH_DEATH_TURN, 100, &none);
        {
            const int turn_from = flow_tic;
            run_until(PH_DEATH_FIZZLE, 400, &none);
            printf("    End Game: angle %d -> %d (expect no turn), %d tic(s)"
                   " before the dissolve (expect 1)\n", angle, player.angle,
                   flow_tic - turn_from);
        }
    }
    run_until(PH_SCORES_WAIT, 800, &none);
    printf("    game over with score %ld: %s, music %d (expect %d), table row 1 \"%s\"\n",
           (long)gamestate.score, phase_names[game_phase()], sb.last_music,
           ROSTER_MUS, high_scores[0].name);
    {
        int waited = 0;
        while (game_phase() == PH_SCORES_WAIT && waited < 1000) {
            game_tic(&c, &none);
            waited++;
        }
        printf("    the table waited %d tics (IN_UserInput(500))\n", waited);
    }
    run_until(PH_TITLE, 200, &none);
    printf("    after the game over: %s (the attract loop)\n",
           phase_names[game_phase()]);
    menu_press(&press, 0);
    run_until(PH_MENU, 100, &none);
    menu_press(&none, 20);
    capture("menu_after_game_over");

    /* after a game over the gun starts on New Game again, so up wraps round
     * to Back to Demo, and once more is View Scores */
    printf("  View Scores:\n");
    menu_press(&up, 40);
    menu_press(&up, 40);
    menu_press(&press, 40);
    printf("    music %d (expect %d)\n", sb.last_music, ROSTER_MUS);
    capture("menu_view_scores");
    menu_press(&press, 40);
    for (int i = 0; i < 7; i++)             /* Read This!, Change View, Load, Extras, Control, Sound, New Game */
        menu_press(&up, 40);

    printf("  a new game, straight to the boss floor:\n");
    /* this time with Start, which selects in the menus as A does */
    {
        const int shots = sb.count_by_sound[SHOOTSND];
        menu_press(&start, 40);             /* New Game */
        printf("  Start on New Game: %s\n",
               sb.count_by_sound[SHOOTSND] > shots ? "selected" : "NOT SELECTED");
    }
    menu_press(&start, 40);                 /* Episode 1 */
    menu_press(&up, 40);                    /* Bring 'em on! */
    menu_press(&start, 40);
    run_until(PH_PLAY, 600, &none);
    printf("  difficulty %d (expect 2), chosen with Start\n", gamestate.difficulty);
    level_load(8);
    printf("  on floor %d; ", current_level + 1);
    for (int y = 1; y < MAPSIZE - 1 && ex < 0; y++)
        for (int x = 1; x < MAPSIZE - 1; x++)
            if (plane1[y][x] == EXITTILE) { ex = x; ey = y; break; }
    printf("exit tile at (%d,%d), south of it %s\n", ex, ey,
           ex >= 0 && !blockmap[ey + 1][ex] ? "open" : "BLOCKED");
    if (ex < 0)
        return;

    actors_init();                          /* no Hans to spoil the run */
    place_player(ex, ey + 1, 900);          /* facing north onto it */
    for (t = 0; t < 200 && !gamestate.victoryflag; t++) {
        controls_t walk = { .forward = -BASEMOVE };
        game_tic(&walk, &none);
    }
    printf("  stepped on the exit after %d tics: victoryflag %d, BJ %s\n", t,
           gamestate.victoryflag,
           numobjs && actor_at(numobjs - 1)->obclass == bjobj ? "spawned" : "MISSING");

    {
        /* Every possible spawn animates: a zero start count would freeze
         * the run on its first frame. The random table is walked by
         * spawning BJ over and over, then the real run is restored. */
        const int real = numobjs;
        const objtype saved = *actor_at(real - 1);
        const bool flag = gamestate.victoryflag;
        uint8_t order[MAXACTORS], free_list[MAXACTORS];
        const int nfree = actor_free_slots(free_list);
        int frozen = 0, tries;
        memcpy(order, obj_order, real);
        for (tries = 0; tries < 256 && numobjs < MAXACTORS; tries++) {
            victory_tile();
            if (!actor_at(numobjs - 1)->ticcount)
                frozen++;
            actor_set_list(order, real, free_list, nfree);
            *actor_at(real - 1) = saved;
        }
        gamestate.victoryflag = flag;
        printf("  %d BJ spawns: %d would never animate the run (expect 0)\n",
               tries, frozen);
    }
    {
        int frames_seen = 0;
        for (t = 0; t < 1500 && game_phase() == PH_PLAY; t++) {
            game_tic(&c, &none);
            if (numobjs && actor_at(numobjs - 1)->state->shapenum < SPR_BJ_JUMP1)
                frames_seen |= 1 << (actor_at(numobjs - 1)->state->shapenum - SPR_BJ_W1);
            if (game_phase() != PH_PLAY)
                break;
            if (t == 40 || t == 90 || t == 150 || t == 200) {
                char label[32];
                objtype *bj = actor_at(numobjs - 1);
                snprintf(label, sizeof label, "victory_run_t%d", t);
                capture(label);
                printf("    t%-4d camera y %.2f angle %d, BJ y %.2f frame %d%s\n", t,
                       player.y / 65536.0, player.angle / 10, bj->y / 65536.0,
                       bj->state->shapenum - SPR_BJ_W1,
                       sb.count_by_sound[YEAHSND] ? "  (yelled)" : "");
            }
        }
        printf("  running frames shown: W1 %s W2 %s W3 %s W4 %s\n",
               frames_seen & 1 ? "yes" : "NO", frames_seen & 2 ? "yes" : "NO",
               frames_seen & 4 ? "yes" : "NO", frames_seen & 8 ? "yes" : "NO");
    }
    printf("  BJ done after %d tics; YEAHSND (digitized as %d): AdLib plays %d,"
           " last digitized sound %d\n", t, (int)wolf_sound_digi[YEAHSND],
           sb.count_by_sound[YEAHSND], sb.last_digi);

    run_until(PH_VICTORY, 200, &none);
    printf("  victory music %d (expect %d)\n", sb.last_music, URAHERO_MUS);
    capture("victory_screen");
    game_tic(&c, &press);
    run_until(PH_ENDTEXT, 200, &none);
    capture("endtext_page1");
    {
        buttons_t right = { 0 }, left = { 0 };
        right.right = true;
        left.left = true;
        game_tic(&c, &right);
        capture("endtext_page2");
        game_tic(&c, &right);               /* the last page: stays */
        printf("  right on the last page: %s\n", phase_names[game_phase()]);
        game_tic(&c, &left);
        capture("endtext_back_to_1");
        game_tic(&c, &right);
    }
    printf("  CheckHighScore() after the victory, with 123456 points:\n");
    gamestate.score = 123456;
    game_tic(&c, &press);                   /* A on the last page leaves */
    run_until(PH_SCORES_NAME, 200, &none);
    printf("    %s, row %d is the new one: score %ld, floor %d\n",
           phase_names[game_phase()], 1, (long)high_scores[0].score,
           high_scores[0].completed);
    {
        /* "BJ!": up makes an A and steps it on; A adds a letter; down
         * steps back past the space to the punctuation; B erases */
        buttons_t right = { 0 };
        const int writes = sram_writes;
        char name_before_start[64];

        right.right = true;
        menu_press(&up, 2);                 /* A */
        menu_press(&up, 2);                 /* B */
        menu_press(&press, 2);              /* BA */
        for (int i = 0; i < 9; i++)
            menu_press(&up, 2);             /* BJ */
        menu_press(&press, 2);              /* BJA */
        menu_press(&back, 2);               /* BJ */
        menu_press(&right, 2);              /* BJA */
        for (int i = 0; i < 3; i++)
            menu_press(&down, 2);           /* space, &, ! */
        capture("scores_name_entry");
        snprintf(name_before_start, sizeof name_before_start, "%s",
                 line_input_text());
        {
            /* held: one step, then a pause, then a repeat every 4 tics */
            for (t = 0; t < 32; t++)
                game_tic(&c, &up);
            printf("    typed \"%s\"; up held 32 tics steps the last letter to"
                   " \"%s\"\n", name_before_start, line_input_text());
            for (t = 0; t < 8; t++)         /* that was four steps */
                game_tic(&c, t % 2 ? &none : &down);
        }
        printf("    down pressed four times brings it back: \"%s\"\n",
               line_input_text());
        game_tic(&c, &start);
        printf("    Start: %s; row 1 \"%s\" %ld floor %d; SRAM writes %d; row 2"
               " \"%s\"\n", phase_names[game_phase()], high_scores[0].name,
               (long)high_scores[0].score, high_scores[0].completed,
               sram_writes - writes, high_scores[1].name);
    }
    capture("scores_named");
    run_until(PH_TITLE_IN, 100, &none);
    printf("  after the high scores: %s, music %d\n", phase_names[game_phase()],
           sb.last_music);

    {
        /* the table is on the cartridge: read everything back, and see the
         * ordering rules on a copy of it */
        highscore_t copy[MAX_SCORES];
        save_init();
        printf("  after a power cycle: row 1 \"%s\" %ld, slot 1 still %s\n",
               high_scores[0].name, (long)high_scores[0].score,
               save_slot_used(0) ? "saved" : "LOST");
        memcpy(copy, high_scores, sizeof copy);
        printf("  10000 on floor 1 ties the last row: row %d (expect -1);",
               high_score_insert(10000, 1, 0));
        printf(" on floor 2 it gets further: row %d (expect 1)\n",
               high_score_insert(10000, 2, 0));
        memcpy(high_scores, copy, sizeof copy);
    }

    printf("  Change View:\n");
    {
        const uint16_t gray = wolf_palette[127];
        buttons_t start2 = { 0 };
        int writes;

        start2.menu = true;
        run_until(PH_TITLE, 200, &none);
        menu_press(&press, 0);
        run_until(PH_MENU, 100, &none);
        menu_press(&none, 30);
        for (int i = 0; i < 5; i++)         /* New Game -> Sound, Control, Extras, Load, Change View */
            menu_press(&down, 40);
        menu_press(&press, 40);
        printf("    the screen: fullscreen %d (expect 0), view %d\n",
               game_fullscreen(), view_size);
        capture("changeview_20");
        for (int i = 0; i < 5; i++)
            menu_press(&down, 12);          /* a press a step */
        capture("changeview_15");
        for (t = 0; t < 25; t++)            /* held: a step every 10 tics */
            game_tic(&c, &down);
        menu_press(&none, 5);
        printf("    five presses and 25 tics held from 20: the preview is"
               " size %d (expect 12)\n", preview_size());
        {
            /* right and left repeat the same way when held */
            buttons_t right_held = { 0 }, left_held = { 0 };
            int after_right;
            right_held.right_held = true;
            left_held.left_held = true;
            for (t = 0; t < 25; t++)
                game_tic(&c, &right_held);
            menu_press(&none, 5);
            after_right = preview_size();
            for (t = 0; t < 25; t++)
                game_tic(&c, &left_held);
            menu_press(&none, 5);
            printf("    right held 25 tics: size %d (expect 15); then left held"
                   " 25 tics: size %d (expect 12)\n", after_right, preview_size());
        }
        for (int i = 0; i < 3; i++)
            menu_press(&up, 12);            /* back to 15 */
        writes = sram_writes;
        menu_press(&press, 40);             /* A: accept */
        printf("    accepted: view %d (expect 15), %dx%d at %d,%d, SRAM writes %d,"
               " %s, fullscreen %d\n", view_size, view_width, view_height,
               view_x, view_y, sram_writes - writes, phase_names[game_phase()],
               game_fullscreen());

        menu_press(&press, 40);             /* again, and give up */
        menu_press(&up, 12);
        menu_press(&back, 40);
        printf("    B after a change: view %d (expect 15)\n", view_size);

        {
            /* Extras, under Control: the gun from Change View, up past
             * Save Game (inactive outside a game) and Load Game */
            const bool was[3] = { opt_show_fps, opt_show_profiler, opt_god_mode };
            const int health = gamestate.health;
            bool after[3];

            for (int i = 0; i < 2; i++)
                menu_press(&up, 40);
            menu_press(&press, 40);
            capture("options");
            menu_press(&press, 40);         /* Show FPS */
            menu_press(&down, 40);
            menu_press(&press, 40);         /* Show Profiler */
            menu_press(&down, 40);
            menu_press(&press, 40);         /* God Mode */
            capture("options_toggled");
            after[0] = opt_show_fps;
            after[1] = opt_show_profiler;
            after[2] = opt_god_mode;
            take_damage(30, NULL);
            printf("    Extras: FPS %d->%d, profiler %d->%d, god mode %d->%d"
                   " (expect 0->1 each); 30 damage in god mode: health"
                   " %d -> %d (expect the same)\n", was[0], after[0], was[1],
                   after[1], was[2], after[2], health, gamestate.health);
            clear_palette_shifts();
            gamestate.health = health;

            /* they are saved: forget them, and read the cartridge again */
            opt_show_fps = opt_show_profiler = opt_god_mode = false;
            save_init();
            printf("    Extras after a power cycle: FPS %d, profiler %d, god"
                   " mode %d (expect 1 1 1)\n", opt_show_fps, opt_show_profiler,
                   opt_god_mode);

            /* back to the defaults, on the cartridge too, for what follows */
            opt_show_fps = opt_show_profiler = opt_god_mode = false;
            save_config();
            menu_press(&back, 40);          /* to the main menu, gun on Extras */
        }

        {
            /* Control, just above Extras: Always Run's light, and the
             * Stick Sensitivity slider, kept with A or put back with B */
            buttons_t right_held = { 0 };
            const uint16_t knob = wolf_palette[0x47];     /* READHCOLOR */
            int sens[4], writes;
            bool run;

            right_held.right_held = true;
            menu_press(&up, 40);
            menu_press(&press, 40);
            capture("control");
            printf("    Control: %s, Always Run %d, sensitivity %d (expect 0 5)\n",
                   phase_names[game_phase()], ctl_always_run,
                   ctl_stick_sensitivity);
            writes = sram_writes;
            menu_press(&press, 40);         /* Always Run */
            run = ctl_always_run;
            capture("control_always_run");
            menu_press(&down, 40);
            menu_press(&press, 40);         /* Stick Sensitivity */
            capture("control_sensitivity");
            sens[0] = *view_pixel(61 + 20 * 5 + 9, 102) == knob;
            for (t = 0; t < 25; t++)        /* held: a notch every 10 tics */
                game_tic(&c, &right_held);
            menu_press(&none, 5);
            sens[1] = ctl_stick_sensitivity;
            capture("control_sensitivity_8");
            menu_press(&press, 40);         /* A keeps it */
            sens[2] = ctl_stick_sensitivity;
            printf("    Always Run %d (expect 1); the knob at 5 %s; right held 25"
                   " tics: %d (expect 8), A: %d, back at %s, SRAM writes %d"
                   " (expect 2)\n", run, sens[0] ? "drawn" : "MISSING", sens[1],
                   sens[2], phase_names[game_phase()], sram_writes - writes);

            menu_press(&press, 40);         /* again, and give up */
            for (int i = 0; i < 20; i++)
                menu_press(&up, 12);        /* stops at the slow end */
            sens[3] = ctl_stick_sensitivity;
            menu_press(&back, 40);
            printf("    up 20 times: %d (expect 0); B: %d (expect 8)\n", sens[3],
                   ctl_stick_sensitivity);

            ctl_always_run = false;
            ctl_stick_sensitivity = CTL_SENS_DEFAULT;
            save_init();
            printf("    Control after a power cycle: Always Run %d, sensitivity %d"
                   " (expect 1 8)\n", ctl_always_run, ctl_stick_sensitivity);

            ctl_always_run = false;         /* the defaults again */
            ctl_stick_sensitivity = CTL_SENS_DEFAULT;
            save_config();

            {
                /* Customize controls, three rows. On the buttons' row, Open
                 * given Z (Fire takes Open's A), then Map given R (Strafe
                 * takes L). On the C buttons' row, Wpn + given D-Up, which
                 * Frwd on the D-pad row had (so Frwd takes C-Up); then D-Rt,
                 * held on, which must not also move the box. Then a change
                 * given up. */
                static const char *const nm[PAD_BUTTONS] = {
                    "B", "A", "Z", "R", "L", "C-Left", "C-Right", "C-Up", "C-Down",
                    "Left", "Right", "Up", "Down"
                };
                buttons_t right1 = { 0 }, pick = { 0 }, held_right = { 0 };
                int saved[CTL_ACTIONS];
                bool open_z, fire_a;

                right1.right_held = true;
                held_right.right_held = true;
                writes = sram_writes;
                menu_press(&down, 40);      /* Customize controls */
                menu_press(&press, 40);
                capture("customize");
                printf("    Customize controls: %s; defaults ", phase_names[game_phase()]);
                for (int i = 0; i < CTL_ACTIONS; i++)
                    printf("%s%s", i ? " " : "", nm[ctl_button[i]]);
                printf(" (expect B A Z R L C-Left C-Right C-Up C-Down Left Right Up Down)\n");

                menu_press(&press, 5);      /* the buttons' row: Run boxed */
                menu_press(&right1, 5);     /* Open */
                menu_press(&press, 15);     /* waiting for an input */
                capture("customize_picking");
                pick.pad_pressed = 1u << PAD_Z;
                menu_press(&pick, 5);
                open_z = ctl_held(CTL_OPEN, 1u << PAD_Z);
                fire_a = ctl_held(CTL_FIRE, 1u << PAD_A);
                printf("    Open given Z: Open %s Fire %s (expect Z A); Z opens %d,"
                       " A fires %d, SRAM writes %d (expect 1)\n",
                       nm[ctl_button[CTL_OPEN]], nm[ctl_button[CTL_FIRE]],
                       open_z, fire_a, sram_writes - writes);
                for (int i = 0; i < 3; i++)
                    menu_press(&right1, 5); /* Fire, Strafe, Map */
                menu_press(&press, 5);
                pick.pad_pressed = 1u << PAD_R;
                menu_press(&pick, 5);
                printf("    Map given R: Strafe %s Map %s (expect L R)\n",
                       nm[ctl_button[CTL_STRAFE]], nm[ctl_button[CTL_MAP]]);
                menu_press(&press, 5);      /* and then C-Right, too wide for */
                pick.pad_pressed = 1u << PAD_C_RIGHT;   /* the menu font there */
                menu_press(&pick, 5);
                capture("customize_map_c_right");
                printf("    Map given C-Right: Map %s, Str. R %s (expect C-Right R)\n",
                       nm[ctl_button[CTL_MAP]], nm[ctl_button[CTL_STRAFE_RIGHT]]);
                menu_press(&back, 5);       /* the row done: the gun again */

                menu_press(&down, 30);      /* the C buttons' row */
                capture("customize_c_row");
                menu_press(&press, 5);      /* Str. L boxed */
                menu_press(&right1, 5);
                menu_press(&right1, 5);     /* Wpn + */
                menu_press(&press, 5);
                pick.pad_pressed = 1u << PAD_D_UP;
                menu_press(&pick, 5);
                capture("customize_cross_row");
                printf("    Wpn + given Up: Wpn + %s, Frwd %s (expect Up C-Up);"
                       " Up changes weapon %d\n", nm[ctl_button[CTL_NEXT_WEAPON]],
                       nm[ctl_button[CTL_FORWARD]],
                       ctl_held(CTL_NEXT_WEAPON, 1u << PAD_D_UP));

                menu_press(&press, 5);      /* D-Rt for it, held a while */
                pick.pad_pressed = 1u << PAD_D_RIGHT;
                pick.right_held = true;
                game_tic(&c, &pick);
                for (t = 0; t < 5; t++)
                    game_tic(&c, &held_right);
                menu_press(&none, 5);
                pick.right_held = false;
                menu_press(&press, 5);      /* the box should still be on Wpn + */
                pick.pad_pressed = 1u << PAD_C_DOWN;
                menu_press(&pick, 5);
                printf("    Right held as the input, then C-Down: Wpn + %s, Wpn - %s,"
                       " Right %s (expect C-Down Right Up)\n",
                       nm[ctl_button[CTL_NEXT_WEAPON]], nm[ctl_button[CTL_PREV_WEAPON]],
                       nm[ctl_button[CTL_RIGHT]]);

                {
                    /* again: Start does nothing now, and the input it has
                     * already leaves it as it was, saving nothing */
                    const int writes2 = sram_writes;
                    menu_press(&press, 15);
                    capture("customize_row_footer");
                    menu_press(&start, 5);
                    pick.pad_pressed = 1u << PAD_C_DOWN;
                    menu_press(&pick, 5);
                    printf("    Start while picking, then its own C-Down: Wpn +"
                           " %s (expect C-Down), SRAM writes %d (expect 0)\n",
                           nm[ctl_button[CTL_NEXT_WEAPON]], sram_writes - writes2);
                }
                menu_press(&back, 5);
                capture("customize_done");

                memcpy(saved, ctl_button, sizeof saved);
                ctl_default_buttons();      /* forget it; read the cartridge */
                save_init();
                printf("    after a power cycle: all 13 %s\n",
                       memcmp(saved, ctl_button, sizeof saved) ? "LOST" : "as set");

                {
                    /* Reset controls, under the rows: B keeps them, A puts
                     * the defaults back, and they are saved */
                    int defaults = 0, kept;
                    menu_press(&down, 30);  /* the D-pad row */
                    menu_press(&down, 30);  /* Reset controls */
                    capture("customize_reset_row");
                    menu_press(&press, 30);
                    capture("customize_reset_confirm");
                    menu_press(&back, 30);
                    kept = !memcmp(saved, ctl_button, sizeof saved);
                    writes = sram_writes;
                    menu_press(&press, 30);
                    menu_press(&press, 30);
                    capture("customize_reset_done");
                    for (int i = 0; i < CTL_ACTIONS; i++)
                        defaults += ctl_button[i] == i;
                    memcpy(ctl_button, saved, sizeof saved);    /* forget it */
                    save_init();
                    printf("    Reset controls: B keeps them %s; A: %d of 13 back"
                           " to the defaults, SRAM writes %d (expect 1), after a"
                           " power cycle %s; %s\n", kept ? "yes" : "NO", defaults,
                           sram_writes - writes,
                           ctl_button[CTL_OPEN] == PAD_A
                           && ctl_button[CTL_NEXT_WEAPON] == PAD_C_UP
                           ? "still the defaults" : "NOT THE DEFAULTS",
                           phase_names[game_phase()]);
                }
                printf("   ");
                {
                    static const uint8_t zeros[CTL_PACKED_BYTES];
                    const bool valid = ctl_unpack_buttons(zeros);
                    printf(" zeros, as a cartridge without the block has, read"
                           " as the defaults: %s\n", !valid
                           && ctl_button[CTL_OPEN] == PAD_A
                           && ctl_button[CTL_BACKWARD] == PAD_D_DOWN ? "yes" : "NO");
                }
                ctl_default_buttons();
                save_config();
                menu_press(&back, 40);      /* to Control, gun on Customize */
            }
            menu_press(&back, 40);          /* the main menu, gun on Control */
            for (int i = 0; i < 3; i++)
                menu_press(&down, 40);      /* back to Change View */
        }

        set_view_size(VIEW_SIZE_MAX);
        save_init();                        /* a power cycle */
        printf("    after a power cycle: view %d (expect 15)\n", view_size);

        for (int i = 0; i < 5; i++)         /* Change View -> Load, Extras, Control, Sound, New Game */
            menu_press(&up, 40);
        menu_press(&start2, 40);            /* New Game, episode, difficulty */
        menu_press(&start2, 40);
        menu_press(&start2, 40);
        run_until(PH_PLAY, 600, &none);
        for (t = 0; t < 10; t++)
            game_tic(&c, &none);
        capture("view15_play");
        {
            int outside = 0, inside = 0;
            for (int y = 0; y < VIEW_H; y++)
                for (int x = 0; x < VIEW_W; x++) {
                    const bool in = x >= view_x && x < view_x + view_width
                                 && y >= view_y && y < view_y + view_height;
                    const bool edge = x >= view_x - 1 && x <= view_x + view_width
                                   && y >= view_y - 1 && y <= view_y + view_height;
                    const bool fps = x >= 8 && x < 8 + 25 && y >= 6 && y < 6 + 6;
                    if (!in && !edge && !fps)
                        outside += *view_pixel(x, y) != gray;
                    if (in)
                        inside += *view_pixel(x, y) == gray;
                }
            printf("    playing at 15: %d border pixels not gray (expect 0);"
                   " %d view pixels happen to be the border's gray\n", outside,
                   inside);
        }
        {
            /* the FPS readout sits in the screen's corner, on the border;
             * going from "47 FPS" to "5 FPS" shortens it by a glyph, and the
             * last one's place (x 28..32) must go back to gray */
            const uint16_t yellow = 0xffc1;
            int lit_two = 0, stale = 0;
            opt_show_fps = true;            /* Show FPS is off by default */
            game_frame(&c, &none, 1, 47);
            for (int y = 6; y < 12; y++)
                for (int x = 28; x < 33; x++)
                    lit_two += *view_pixel(x, y) == yellow;
            capture("view15_fps47");
            game_frame(&c, &none, 1, 5);
            for (int y = 6; y < 12; y++)
                for (int x = 28; x < 33; x++)
                    stale += *view_pixel(x, y) != gray;
            capture("view15_fps5");
            printf("    FPS in the corner at 15: \"47 FPS\" lights %d pixels of its"
                   " last glyph (expect >0); after \"5 FPS\", %d there not gray"
                   " (expect 0)\n", lit_two, stale);
            opt_show_fps = false;
        }
        take_damage(200, NULL);
        run_until(PH_DEATH_FIZZLE, 400, &none);
        for (t = 0; t < 35; t++)
            game_tic(&c, &none);
        capture("view15_death_fizzle");
        printf("    dying: the dissolve leaves the border %s\n",
               *view_pixel(view_x - 3, view_y + 5) == gray
               && *view_pixel(4, 4) == gray ? "gray" : "DISSOLVED");
        run_until(PH_PLAY, 1000, &none);
        printf("    the floor again: %s, view %d\n", phase_names[game_phase()],
               view_size);
    }

    printf("  No Clip:\n");
    {
        /* from where the player stands, face the nearest plain wall along
         * one of the four directions - not a door or a push wall - and walk
         * at it: with No Clip on, into its tile; off, not */
        static const int dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, -1, 0, 1 };
        const controls_t walk = { .forward = -BASEMOVE };
        const int tx = player.tilex, ty = player.tiley;
        int d, dist = 0, wx = 0, wy = 0, inside[2];

        for (d = 0; d < 4; d++) {
            for (dist = 1; dist < 8; dist++) {
                wx = tx + dx[d] * dist;
                wy = ty + dy[d] * dist;
                if (plane0[wy][wx] < AREATILE)
                    break;
            }
            if (dist < 8 && !(tilemap[wy][wx] & 0x80)
                && plane1[wy][wx] != PUSHABLETILE)
                break;
        }
        for (int on = 1; on >= 0 && d < 4; on--) {
            opt_no_clip = on;
            place_player(tx, ty, d * 900);
            for (t = 0; t < 40 * dist; t++)
                game_tic(&walk, &none);
            inside[on] = (player.tilex - tx) * dx[d] + (player.tiley - ty) * dy[d]
                         >= dist;
            if (on)
                capture("noclip_in_wall");
        }
        opt_no_clip = false;
        place_player(tx, ty, 0);
        game_tic(&c, &none);
        if (d < 4)
            printf("    a wall %d tiles %s of (%d,%d): reached its tile with No"
                   " Clip %s, without %s\n", dist, (const char *[]){ "east",
                   "north", "west", "south" }[d], tx, ty,
                   inside[1] ? "yes" : "NO", inside[0] ? "YES" : "no");
        else
            printf("    NO PLAIN WALL near (%d,%d) to walk into\n", tx, ty);
    }

    printf("  Extras' one-off cheats, in a game:\n");
    {
        buttons_t start2 = { 0 }, right1 = { 0 };
        int32_t time0;
        int lives0, floor0;

        start2.menu = true;
        right1.right_held = true;
        menu_press(&start2, 60);            /* the gun on New Game */
        for (int i = 0; i < 3; i++)
            menu_press(&down, 40);          /* Sound, Control, Extras */
        menu_press(&press, 40);
        capture("extras_ingame");
        menu_press(&down, 40);              /* God Mode -> No Clip */
        {
            bool now[2];
            const int writes = sram_writes;
            menu_press(&press, 30);
            now[0] = opt_no_clip;
            capture("extras_no_clip");
            menu_press(&press, 30);
            now[1] = opt_no_clip;
            printf("    No Clip: on %d, then off %d (expect 1 0), SRAM writes %d"
                   " (expect 2)\n", now[0], now[1], sram_writes - writes);
        }
        menu_press(&down, 40);              /* Health, Ammo & Keys */
        gamestate.score = 5000;
        gamestate.health = 40;
        gamestate.keys = 0;
        time0 = gamestate.timecount;
        menu_press(&press, 30);
        capture("extras_mli");
        printf("    M-L-I: health %d ammo %d keys %d score %ld weapon %d, %ld tics"
               " added (expect 100 99 3 0 %d, 42000); %s\n", gamestate.health,
               gamestate.ammo, gamestate.keys, (long)gamestate.score,
               gamestate.bestweapon, (long)(gamestate.timecount - time0),
               wp_chaingun, phase_names[game_phase()]);
        menu_press(&press, 30);             /* the message goes */

        menu_press(&down, 40);              /* Free Items */
        gamestate.health = 50;
        gamestate.ammo = 10;
        lives0 = gamestate.lives;
        menu_press(&press, 30);
        capture("extras_free_items");
        printf("    Free Items: score %ld health %d ammo %d (expect 100000 100 60:"
               " 10 + 50, with no weapon past the gatling), lives %d -> %d\n",
               (long)gamestate.score, gamestate.health, gamestate.ammo, lives0,
               gamestate.lives);
        menu_press(&press, 30);

        menu_press(&down, 40);              /* Warp to Floor */
        floor0 = current_level + 1;
        gamestate.oldscore = 1234;
        menu_press(&press, 10);
        menu_press(&right1, 5);
        menu_press(&right1, 5);
        capture("extras_warp");
        menu_press(&press, 0);
        run_until(PH_WARP_OUT, 100, &none);
        run_until(PH_PSYCHED_IN, 100, &none);
        run_until(PH_PLAY, 600, &none);
        printf("    Warp from floor %d, two to the right: floor %d (expect %d),"
               " score %ld (expect 1234, as the floor began), %s\n", floor0,
               current_level + 1, floor0 + 2, (long)gamestate.score,
               phase_names[game_phase()]);

        menu_press(&start2, 60);            /* the gun left on Extras */
        menu_press(&press, 40);
        menu_press(&down, 40);              /* Warp to Floor -> End Floor */
        gamestate.keys = 3;
        floor0 = current_level + 1;
        menu_press(&press, 0);
        run_until(PH_INTER, 300, &none);
        capture("extras_end_floor");
        printf("    End Floor: %s (expect INTER), keys %d (expect 0)\n",
               phase_names[game_phase()], gamestate.keys);
        for (t = 0; t < 3000 && game_phase() != PH_PLAY; t++)
            game_tic(&c, t % 20 ? &none : &press);
        printf("    through the stats: floor %d (expect %d), %s\n",
               current_level + 1, floor0 + 1, phase_names[game_phase()]);

        /* floor 9 ends in the victory, so End Floor there does too */
        menu_press(&start2, 60);            /* the gun left on Extras */
        menu_press(&press, 40);
        menu_press(&up, 40);                /* End Floor -> Warp to Floor */
        menu_press(&press, 10);
        for (int i = current_level + 1; i < 9; i++)
            menu_press(&right1, 5);
        menu_press(&press, 0);
        run_until(PH_PLAY, 700, &none);
        floor0 = current_level + 1;
        menu_press(&start2, 60);
        menu_press(&press, 40);
        menu_press(&down, 40);              /* End Floor */
        menu_press(&press, 0);
        run_until(PH_VICTORY, 300, &none);
        capture("extras_end_floor_9");
        printf("    End Floor on floor %d: %s (expect VICTORY, not the secret"
               " floor's stats)\n", floor0, phase_names[game_phase()]);
    }
}

/* ------------------------------------------------------------------ */
/* The four recorded demos, left to play through the attract loop      */
/* ------------------------------------------------------------------ */

/* DEMOTRACE=file writes a line per demo frame, in the format of the
 * reference build (Wolf4SDL, patched to print the same) that demo sync was
 * checked against: the player, the counters, the random table's index, and
 * checksums of every door and actor. */
static FILE *demo_trace;
static int   demo_trace_frame;

static void trace_demo_frame(void)
{
    unsigned dsum = 0, osum = 0;

    if (!demo_trace_frame)
        fprintf(demo_trace, "DEMO floor %d\n", current_level + 1);
    for (int i = 0; i < doornum; i++)
        dsum += doorposition[i] + doorobjlist[i].action * 7;
    for (int i = 0; i < numobjs; i++)
        osum += (unsigned)actor_at(i)->x + (unsigned)actor_at(i)->y * 3
              + (unsigned)actor_at(i)->hitpoints * 5;
    fprintf(demo_trace, "f%d x %x y %x a %d k %d hp %d am %d t %d s %d"
            " r %d d %x o %x\n",
            demo_trace_frame++, (unsigned)player.x, (unsigned)player.y,
            player.angle / 10, gamestate.killcount, gamestate.health,
            gamestate.ammo, gamestate.treasurecount, gamestate.secretcount,
            us_rnd_index(), dsum, osum);
}

/* How each demo ends in the reference, Wolf4SDL built as the same game,
 * which follows the recording to the last frame: with BJ's death, or with
 * the recording used up (which PollControls() also calls ex_completed).
 * The floor is counted from the start of the data. */
typedef struct {
    int floor, frames;
    unsigned x, y;
    int angle, kills, ammo, rnd, end;
} demo_expect_t;

static const demo_expect_t demo_expect_wl1[4] = {
    { 1, 1152, 0x9e779,  0x20e140,  92, 18, 45, 112, DEMO_DIED },
    { 3, 1284, 0x3e2e31, 0x2cd4ad, 135, 20, 42, 194, DEMO_DIED },
    { 5,  671, 0x63437,  0x1ed9c9,  34, 13, 16,  10, DEMO_DIED },
    { 7,  633, 0x7c063,  0x354763, 176,  7,  0, 205, DEMO_DIED },
};
static const demo_expect_t demo_expect_wl6[4] = {
    { 38,  691, 0x2a089,  0x89e26,   90, 15, 75,  22, DEMO_DIED },
    { 44, 1899, 0x265954, 0x2daa6d,  32, 20,  0,  28, DEMO_RAN_OUT },
    { 57, 1140, 0x1b851c, 0x167c70,   9, 27, 16, 194, DEMO_RAN_OUT },
    { 32,  869, 0x171a6c, 0x3491da, 124, 10, 78, 200, DEMO_DIED },
};
static const demo_expect_t demo_expect_sod[4] = {
    {  2,  650, 0x3499d3, 0x21928d, 182, 13,  8,  57, DEMO_DIED },
    {  4,  746, 0x1498c6, 0x2a3adf,  95, 10, 22,  82, DEMO_DIED },
    {  6,  783, 0x1df88a, 0x1ca56f,  84, 12,  9, 132, DEMO_DIED },
    { 13,  855, 0x168613, 0x1e2d3e, 357, 13,  7, 120, DEMO_DIED },
};
/* controls.c: the stick's directions go with the D-pad's, in proportion for
 * a move and as a press for anything else, as Customize controls sets them */
static void pad_read(pad_state_t p, controls_t *c, buttons_t *b)
{
    ctl_read_pad(&p, 1, c, b);
}

static void pad_tests(void)
{
    const pad_state_t rest = { 0 };
    controls_t c;
    buttons_t b;
    int half, dpad, turn5, turn9, edge[2], up_menu;

    printf("\nthe pad, as controls.c reads it:\n");
    ctl_default_buttons();
    ctl_stick_sensitivity = CTL_SENS_DEFAULT;
    pad_read(rest, &c, &b);

    pad_read((pad_state_t){ .stick_y = 36 }, &c, &b);
    half = c.forward;
    pad_read((pad_state_t){ .held = 1u << PAD_D_UP }, &c, &b);
    dpad = c.forward;
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    turn5 = c.turn;
    ctl_stick_sensitivity = CTL_SENS_MAX;
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    turn9 = c.turn;
    ctl_stick_sensitivity = CTL_SENS_DEFAULT;
    printf("  defaults: stick half up walks %d (expect -17), D-Up %d (expect -35);"
           " stick full right turns %d, at the fastest %d (expect 35 49)\n",
           half, dpad, turn5, turn9);

    /* the sensitivity scales walking too, as DOS's did the mouse's forward
     * and back - but no faster than the D-pad - and never the D-pad */
    {
        int slow, fast, full, dfast;
        ctl_stick_sensitivity = 0;
        pad_read((pad_state_t){ .stick_y = 36 }, &c, &b);
        slow = c.forward;
        ctl_stick_sensitivity = CTL_SENS_MAX;
        pad_read((pad_state_t){ .stick_y = 36 }, &c, &b);
        fast = c.forward;
        pad_read((pad_state_t){ .stick_y = 72 }, &c, &b);
        full = c.forward;
        pad_read((pad_state_t){ .held = 1u << PAD_D_UP }, &c, &b);
        dfast = c.forward;
        ctl_stick_sensitivity = CTL_SENS_DEFAULT;
        printf("  stick half up walks %d at the slowest, %d at the fastest (expect -8"
               " -24); full up at the fastest %d, D-Up %d (expect -35 -35)\n",
               slow, fast, full, dfast);
    }

    pad_read(rest, &c, &b);
    pad_read((pad_state_t){ .stick_y = 72 }, &c, &b);
    edge[0] = (b.pad_pressed >> PAD_D_UP) & 1;
    pad_read((pad_state_t){ .stick_y = 72 }, &c, &b);
    edge[1] = (b.pad_pressed >> PAD_D_UP) & 1;
    printf("  the stick pushed up is D-Up pressed %d, then held %d (expect 1 0)\n",
           edge[0], edge[1]);

    /* the text pages, Warp to Floor and name typing take left and right as
     * presses: the stick gives them as the D-pad does */
    pad_read(rest, &c, &b);
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    edge[0] = b.right;
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    edge[1] = b.right;
    pad_read(rest, &c, &b);
    pad_read((pad_state_t){ .stick_x = -72 }, &c, &b);
    printf("  the stick pushed right is a right press %d, then %d; left %d (expect 1 0 1)\n",
           edge[0], edge[1], b.left);

    /* Fire given D-Up, so Frwd takes Z */
    ctl_button[CTL_FIRE] = PAD_D_UP;
    ctl_button[CTL_FORWARD] = PAD_Z;
    pad_read(rest, &c, &b);
    pad_read((pad_state_t){ .stick_y = 72 }, &c, &b);
    printf("  Fire on D-Up: the stick up fires %d and walks %d (expect 1 0);",
           b.fire, c.forward);
    up_menu = b.up;
    pad_read((pad_state_t){ .stick_y = 30 }, &c, &b);
    printf(" a little up fires %d (expect 0);", b.fire);
    pad_read((pad_state_t){ .held = 1u << PAD_Z }, &c, &b);
    printf(" Z walks %d (expect -35); the menus still see up %d\n", c.forward,
           up_menu);

    /* Wpn + given D-Rt, so Right takes C-Up */
    ctl_default_buttons();
    ctl_button[CTL_NEXT_WEAPON] = PAD_D_RIGHT;
    ctl_button[CTL_RIGHT] = PAD_C_UP;
    pad_read(rest, &c, &b);
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    edge[0] = b.change;
    turn5 = c.turn;
    pad_read((pad_state_t){ .stick_x = 72 }, &c, &b);
    edge[1] = b.change;
    pad_read((pad_state_t){ .held = 1u << PAD_C_UP }, &c, &b);
    printf("  Wpn + on D-Rt: the stick right changes weapon %d, then %d, and turns"
           " %d (expect 1 0 0); C-Up turns %d (expect 35)\n", edge[0], edge[1],
           turn5, c.turn);

    ctl_default_buttons();
    pad_read(rest, &c, &b);
}

static void demo_tests(void)
{
    if (getenv("DEMOTRACE")) {
        demo_trace = fopen(getenv("DEMOTRACE"), "w");
        if (demo_trace)
            demo_frame_hook = trace_demo_frame;
    }
    const buttons_t none = { 0 };
    const controls_t c = { 0 };

    printf("\ndemos (the attract loop, untouched):\n");
    sd_init();
    game_init();
    {
        buttons_t a = { 0 };
        a.use = true;
        game_frame(&c, &none, 35, 70);      /* "One moment..." */
        game_frame(&c, &a, 1, 70);          /* past the sign-on screen */
    }

    const demo_expect_t *demo_expect = wolf_version == WV_SPEAR ? demo_expect_sod
                                     : wolf_version == WV_FULL  ? demo_expect_wl6
                                                                : demo_expect_wl1;
    static const char *const ends[] = { "RECORDING RAN OUT", "ends with BJ's death",
                                        "ends at the elevator" };

    for (int played = 0; played < 4; played++) {
        int which = 0, tics = 0, e = 0;
        char label[40];
        bool match;

        run_until(PH_DEMO, 5000, &none);
        if (game_phase() != PH_DEMO)
            return;
        for (int i = 0; i < 4; i++)         /* each demo is a different floor */
            if (wolf_demos[i][0] == current_level)
                which = i;
        for (int i = 0; i < 4; i++)
            if (demo_expect[i].floor == current_level + 1)
                e = i;

        while (game_phase() == PH_DEMO) {
            game_tic(&c, &none);
            if (++tics == demo_expect[e].frames * 2) {
                snprintf(label, sizeof label, "demo%d_middle", which + 1);
                capture(label);
            }
        }
        snprintf(label, sizeof label, "demo%d_end", which + 1);
        capture(label);

        match = (unsigned)player.x == demo_expect[e].x
             && (unsigned)player.y == demo_expect[e].y
             && player.angle / 10 == demo_expect[e].angle
             && gamestate.killcount == demo_expect[e].kills
             && gamestate.ammo == demo_expect[e].ammo
             && us_rnd_index() == demo_expect[e].rnd
             && demo_end == demo_expect[e].end;
        printf("  demo %d, floor %d: %d frames, %s; kills %d, ammo %d, at %x,%x"
               " angle %d, random index %d - %s\n",
               which + 1, current_level + 1, (tics + 3) / 4 + 1, ends[demo_end],
               gamestate.killcount, gamestate.ammo, (unsigned)player.x,
               (unsigned)player.y, player.angle / 10, us_rnd_index(),
               match ? "in sync with the reference" : "OUT OF SYNC");
        demo_trace_frame = 0;
    }
}

/* ------------------------------------------------------------------ */
/* The registered games: the full game's and Spear of Destiny's cast, */
/* their bosses, projectiles and ends                                 */
/* ------------------------------------------------------------------ */

static void aim_at(const objtype *ob);

/* Put the player up to `away` tiles from ob along a clear straight line -
 * its own facing first - looking at it. */
static bool face_actor(objtype *ob, int most)
{
    static const int dirs[4] = { east, north, west, south };
    for (int away = most; away >= 2; away--)
    for (int k = -1; k < 4; k++) {
        const int d = k >= 0 ? dirs[k]
                    : ob->dir < nodir && !(ob->dir & 1) ? ob->dir : -1;
        bool clear = true;
        if (d < 0)
            continue;
        for (int s = 1; s <= away && clear; s++) {
            const int tx = ob->tilex + ddx[d] * s, ty = ob->tiley + ddy[d] * s;
            if (tx < 1 || ty < 1 || tx >= MAPSIZE - 1 || ty >= MAPSIZE - 1
                || blockmap[ty][tx] || (actorat[ty][tx] && actorat[ty][tx] != ob))
                clear = false;
        }
        if (!clear)
            continue;
        place_player(ob->tilex + ddx[d] * away, ob->tiley + ddy[d] * away,
                     ((d + 4) % 8) * 450);
        return true;
    }

    /* no straight line: any floor tile nearby that can see it */
    for (int r = 2; r <= most + 2; r++)
        for (int ty = ob->tiley - r; ty <= ob->tiley + r; ty++)
            for (int tx = ob->tilex - r; tx <= ob->tilex + r; tx++) {
                bool clear = true;
                if (tx < 1 || ty < 1 || tx >= MAPSIZE - 1 || ty >= MAPSIZE - 1
                    || blockmap[ty][tx] || actorat[ty][tx]
                    || plane0[ty][tx] < AREATILE)
                    continue;
                for (int s = 1; s < 64 && clear; s++) {     /* a sixty-fourth at a time */
                    const fixed x = ((fixed)tx << TILESHIFT) + TILEGLOBAL / 2
                                  + (ob->x - (((fixed)tx << TILESHIFT) + TILEGLOBAL / 2)) / 64 * s;
                    const fixed y = ((fixed)ty << TILESHIFT) + TILEGLOBAL / 2
                                  + (ob->y - (((fixed)ty << TILESHIFT) + TILEGLOBAL / 2)) / 64 * s;
                    const int cx = x >> TILESHIFT, cy = y >> TILESHIFT;
                    if ((cx != ob->tilex || cy != ob->tiley) && blockmap[cy][cx])
                        clear = false;
                }
                if (!clear)
                    continue;
                place_player(tx, ty, 0);
                aim_at(ob);
                return true;
            }
    return false;
}

/* Turn the player to face ob, as the aim needs */
static void aim_at(const objtype *ob)
{
    double a = atan2((double)(player.y - ob->y), (double)(ob->x - player.x));
    if (a < 0)
        a += 2 * M_PI;
    player.angle = (int)(a / (2 * M_PI) * FINEANGLES) % FINEANGLES;
}

static objtype *find_class(int obclass)
{
    for (int i = 0; i < numobjs; i++)
        if (actor_at(i)->obclass == obclass)
            return actor_at(i);
    return NULL;
}

/* the first floor with one of the class on it, loaded on the hardest skill */
static int load_floor_with(int obclass)
{
    for (int l = 0; l < wolf_num_levels; l++) {
        game_new();
        gamestate.difficulty = gd_hard;
        level_load(l);
        if (find_class(obclass))
            return l;
    }
    return -1;
}

static void label_of(char *label, size_t size, const char *prefix, int obclass)
{
    snprintf(label, size, "%s%s", prefix, classname(obclass));
    for (char *p = label; *p; p++)
        if (*p == ' ')
            *p = '_';
}

/* Every class on every floor, on the hardest skill, and a look at each */
static void census(void)
{
    int count[NUM_CLASSES] = { 0 }, first[NUM_CLASSES];

    printf("\nthe cast, every floor on \"I am Death incarnate!\":\n");
    for (int c = 0; c < NUM_CLASSES; c++)
        first[c] = -1;
    for (int l = 0; l < wolf_num_levels; l++) {
        game_new();
        gamestate.difficulty = gd_hard;
        level_load(l);
        for (int i = 0; i < numobjs; i++) {
            const int c = actor_at(i)->obclass;
            count[c]++;
            if (first[c] < 0)
                first[c] = l;
        }
    }
    for (int c = 0; c < NUM_CLASSES; c++)
        if (count[c])
            printf("  %-13s %4d, first on floor %d\n", classname(c), count[c],
                   first[c] + 1);

    for (int c = 1; c < NUM_CLASSES; c++) {
        char label[40];
        objtype *ob;
        if (first[c] < 0 || c == bjobj)
            continue;
        game_new();
        gamestate.difficulty = gd_hard;
        level_load(first[c]);
        ob = find_class(c);
        if (!ob || !face_actor(ob, 3)) {
            printf("  %s: no clear line to it\n", classname(c));
            continue;
        }
        fade_in(1);
        fade_update(1);
        render_view();
        label_of(label, sizeof label, "cast_", c);
        capture(label);
        printf("    %s drawn with sprite %d%s\n", classname(c), actor_shape(ob),
               assets_sprite(actor_shape(ob)) ? "" : " - NOT IN THE DATA");
    }
}

/* A boss fought to the end through game.c, in god mode, with its hit
 * points cut short: a key dropped, or the death cam and the victory. */
static void boss_fight(int obclass)
{
    const buttons_t none = { 0 }, ack = { .use = true };
    const controls_t c = { 0 };
    const buttons_t fire = { .fire = true };
    char label[48];
    int floor = load_floor_with(obclass), t, seen = 0, most = 0, replay_tics = 0;
    int collapse_tics = 0;
    objtype *boss;
    bool deathcam_seen = false, replay_shown = false;

    if (floor < 0)
        return;
    printf("\n  %s, floor %d:\n", classname(obclass), floor + 1);
    sd_init();
    memset(&sb, 0, sizeof sb);
    game_start_floor(floor, gd_hard);
    run_until(PH_PLAY, 400, &none);
    boss = find_class(obclass);
    if (!boss || !face_actor(boss, 4)) {
        printf("    no clear line to it\n");
        return;
    }
    opt_god_mode = true;
    gamestate.bestweapon = gamestate.weapon = gamestate.chosenweapon = wp_chaingun;
    boss->hitpoints = 40;

    for (t = 0; t < 70 * 60; t++) {
        objtype *target = NULL;
        /* Mecha Hitler's suit, then the man */
        for (int i = 0; i < numobjs; i++) {
            objtype *ob = actor_at(i);
            if ((ob->obclass == obclass || (obclass == mechahitlerobj
                                            && ob->obclass == realhitlerobj))
                && (ob->flags & FL_SHOOTABLE))
                target = ob;
        }
        if (target) {
            aim_at(target);
            if (target->obclass == realhitlerobj && target->hitpoints > 40)
                target->hitpoints = 40;
        }
        gamestate.ammo = 99;
        /* the gatling, held - let go now and then, as a fresh press is
         * what starts it */
        game_tic(&c, target && t % 40 ? &fire : &none);
        for (int i = 0; i < numobjs; i++)
            seen |= 1 << actor_at(i)->obclass;
        if (numobjs > most)
            most = numobjs;
        if (game_phase() == PH_DEATHCAM_TEXT && !deathcam_seen) {
            deathcam_seen = true;
            label_of(label, sizeof label, "deathcam_", obclass);
            capture(label);
        }
        if (game_phase() == PH_COLLAPSE && ++collapse_tics == 300) {
            capture("victory_collapse");
            printf("    BJ collapses\n");
        }
        if (deathcam && game_phase() == PH_PLAY && !replay_shown
            && ++replay_tics == 40) {
            label_of(label, sizeof label, "deathcam_replay_", obclass);
            capture(label);
            replay_shown = true;
        }
        if (game_phase() == PH_VICTORY || (!target && t > 70 * 8
                                           && game_phase() == PH_PLAY && !deathcam))
            break;
        if (game_phase() == PH_DEATHCAM_TEXT)
            game_tic(&c, &ack);
    }
    printf("    %s after %d tics; the most actors at once %d; projectiles seen:%s%s%s%s%s%s\n",
           game_phase() == PH_VICTORY ? "the victory screen" : "dead", t, most,
           seen & 1 << needleobj ? " needles" : "", seen & 1 << rocketobj ? " rockets" : "",
           seen & 1 << fireobj ? " flames" : "", seen & 1 << hrocketobj ? " hrockets" : "",
           seen & 1 << sparkobj ? " sparks" : "",
           seen & (1 << needleobj | 1 << rocketobj | 1 << fireobj | 1 << hrocketobj
                   | 1 << sparkobj) ? "" : " none");
    {
        bool key = false;
        for (int i = 0; i < numstats; i++)
            key |= statobjlist[i].itemnumber == bo_key1 && statobjlist[i].shapenum >= 0;
        printf("    death cam %s, gold key on the floor %s\n",
               deathcam_seen ? "shown" : "not shown", key ? "yes" : "no");
    }
    if (game_phase() == PH_VICTORY) {
        label_of(label, sizeof label, "victory_", obclass);
        capture(label);
        menu_press(&ack, 0);
        if (wolf_version == WV_SPEAR) {
            /* EndSpear(): nine screens, the second with two captions */
            int shown = 0;
            for (int n = 0; n < 12; n++) {
                for (int k = 0; k < 400 && game_phase() != PH_ENDSPEAR
                                && (game_phase() < PH_SCORES_IN
                                    || game_phase() > PH_SCORES_OUT); k++)
                    game_tic(&c, &none);
                if (game_phase() != PH_ENDSPEAR)
                    break;
                snprintf(label, sizeof label, "endspear_%d", n + 1);
                capture(label);
                shown++;
                menu_press(&ack, 2);
                if (game_phase() == PH_ENDSPEAR) {  /* the second caption */
                    snprintf(label, sizeof label, "endspear_%db", n + 1);
                    capture(label);
                    menu_press(&ack, 2);
                }
            }
            printf("    then %d end screens, and the high scores %s\n", shown,
                   game_phase() >= PH_SCORES_IN && game_phase() <= PH_SCORES_WAIT
                   ? "(right)" : "NEVER");
        } else {
            run_until(PH_ENDTEXT, 200, &none);
            if (game_phase() == PH_ENDTEXT) {
                label_of(label, sizeof label, "endtext_", obclass);
                capture(label);
            }
            printf("    then %s\n", game_phase() == PH_ENDTEXT ? "the end text"
                   : "no end text");
        }
    }
    opt_god_mode = false;
    fade_in(1);
    fade_update(1);
}

/* Rockets, their smoke and their explosions leave the list when they are
 * done, and a save with some in the air comes back exactly. */
static void projectile_tests(int obclass, int projectile)
{
    static uint8_t buf[SAVE_MEDIUM_BYTES];
    int floor = load_floor_with(obclass), t, fired = 0, most = 0, in_air = 0;
    objtype *boss;
    bool shown = false, saved_ok = false;

    if (floor < 0)
        return;
    boss = find_class(obclass);
    printf("\n  %s's %ss, floor %d:\n", classname(obclass), classname(projectile),
           floor + 1);
    if (!boss || !face_actor(boss, 5)) {
        printf("    no clear line to it\n");
        return;
    }
    opt_god_mode = true;
    memset(sram, 0, sizeof sram);
    save_init();
    sim_fire_last = false;
    boss->hitpoints = 30000;                /* it has to live to shoot */
    for (t = 0; t < 70 * 30; t++) {
        int now = 0;
        if (boss->flags & FL_SHOOTABLE)
            aim_at(boss);
        gamestate.ammo = 99;
        frame(0, 0, t % 140 == 0);          /* a shot now and then keeps it awake */
        for (int i = 0; i < numobjs; i++)
            now += actor_at(i)->obclass == projectile;
        if (now > in_air)
            fired++;
        in_air = now;
        if (numobjs > most)
            most = numobjs;
        if (now && !shown) {
            for (int i = 0; i < numobjs; i++) {
                objtype *p = actor_at(i);
                if (p->obclass == projectile && abs(p->tilex - boss->tilex)
                                                + abs(p->tiley - boss->tiley) >= 2) {
                    char label[40];
                    label_of(label, sizeof label, "projectile_", projectile);
                    capture(label);
                    shown = true;
                    break;
                }
            }
        }
        if (now && !saved_ok && t > 200) {
            /* a save with something in the air */
            const uint32_t digest = state_digest();
            const size_t size = save_state(buf, sizeof buf);
            uint32_t run1, run2;
            bool same;
            save_game(0, "in the air");
            run1 = chaos_trace(4242, 300);
            load_game(0);
            same = state_digest() == digest;
            run2 = chaos_trace(4242, 300);
            printf("    saved with %d in the air: %zu bytes, loaded %s, 300 tics %s\n",
                   now, size, same ? "the same" : "DIFFERENT",
                   run1 == run2 ? "the same" : "DIFFERENT");
            saved_ok = true;
            boss = find_class(obclass);
            if (!boss)
                break;
        }
    }
    printf("    %d launched in %d tics; the list peaked at %d, %d in the air at the"
           " end\n", fired, t, most, in_air);
    opt_god_mode = false;
    memset(sram, 0, sizeof sram);
}

/* Killed by Dr. Schabbs's syringe: DrawFace() shows the mutant face, where
 * any other killer leaves the plain dead one. */
static void needle_death_test(void)
{
    int floor = load_floor_with(schabbobj), t;
    objtype *boss;

    if (floor < 0)
        return;
    boss = find_class(schabbobj);
    if (!boss || !face_actor(boss, 5)) {
        printf("  killed by a syringe: no clear line to Schabbs\n");
        return;
    }
    sim_fire_last = false;
    player_died = false;
    fade_in(1);
    fade_update(1);
    boss->hitpoints = 30000;
    gamestate.health = 1;
    gamestate.ammo = 99;
    /* The shot that wakes him wakes the floor's mutants too, and they could
     * get in first: so god mode keeps the player alive but for the tics a
     * syringe is about to arrive, and a death by anything else is undone. */
    for (t = 0; t < 70 * 60; t++) {
        bool close = false;
        for (int i = 0; i < numobjs; i++) {
            const objtype *p = actor_at(i);
            if (p->obclass == needleobj && abs(p->x - player.x) < TILEGLOBAL
                && abs(p->y - player.y) < TILEGLOBAL)
                close = true;
        }
        opt_god_mode = !close;
        gamestate.health = 1;
        aim_at(boss);
        frame(0, 0, t == 0);                /* the shot wakes him */
        if (player_died && last_attacker_class == needleobj)
            break;
        player_died = false;
    }
    opt_god_mode = false;
    render_view();
    capture("dead_by_needle");
    printf("  killed by a syringe: %s after %d tics by a %s, face %s\n",
           player_died ? "died" : "SURVIVED", t, classname(last_attacker_class),
           status_face_pic() == PIC_MUTANTBJ ? "mutant (MUTANTBJPIC)"
           : "NOT the mutant one");
    last_attacker_class = guardobj;
    printf("  killed by anything else: face %s\n",
           status_face_pic() == PIC_FACE + 21 ? "FACE8APIC" : "WRONG");
}

/* The screens around the game, as the attract loop and the menus show
 * them */
static void screens_test(void)
{
    const buttons_t none = { 0 }, a = { .use = true }, back = { .back = true };
    const controls_t c = { 0 };
    static const char *const titles[3] = { "title", "credits", "scores" };

    printf("\nthe screens:\n");
    sd_init();
    game_init();
    game_frame(&c, &none, 35, 70);          /* past "One moment..." */
    capture("signon");
    if (wolf_version != WV_SPEAR)           /* Spear of Destiny asks no key */
        menu_press(&a, 0);
    run_until(PH_PG13, 1000, &none);
    capture("pg13");
    for (int k = 0; k < 3; k++) {
        run_until(PH_TITLE, 1500, &none);
        capture(titles[k]);
        if (k < 2)
            run_until(PH_TITLE_OUT, 1500, &none);
    }
    menu_press(&a, 0);                      /* to the menu */
    run_until(PH_MENU, 200, &none);
    menu_press(&none, 20);
    capture("menu_main");
    menu_press(&a, 60);                     /* New Game */
    capture(wolf_version == WV_SPEAR ? "menu_skill" : "menu_episode");
    if (wolf_version != WV_SPEAR) {
        menu_press(&a, 60);
        capture("menu_skill");
        menu_press(&back, 60);
    }
    menu_press(&back, 60);
    menu_press(&none, 20);
    printf("  back at the main menu: %s\n", game_phase() == PH_MENU ? "yes" : "NO");
}

/* Spear of Destiny: walk onto the Spear, and on to the last floor as BJ
 * stood when he took it */
static void spear_test(void)
{
    const buttons_t none = { 0 };
    const controls_t fwd = { .forward = -BASEMOVE };

    for (int l = 0; l < wolf_num_levels; l++) {
        game_new();
        gamestate.difficulty = gd_hard;
        level_load(l);
        for (int i = 0; i < numstats; i++) {
            int t;
            fixed sx, sy;
            if (statobjlist[i].itemnumber != bo_spear)
                continue;
            printf("\n  the Spear, on floor %d:\n", l + 1);
            sd_init();
            game_start_floor(l, gd_hard);
            run_until(PH_PLAY, 400, &none);
            actors_init();                  /* no one in the way */
            if (!view_static_at(i, 3)) {
                printf("    no clear way to it\n");
                return;
            }
            gamestate.keys = 2;
            for (t = 0; t < 300 && game_phase() == PH_PLAY; t++)
                game_tic(&fwd, &none);
            sx = player.x;
            sy = player.y;
            capture("spear_taken");
            run_until(PH_PLAY, 400, &none);
            capture("spear_last_floor");
            printf("    taken after %d tics; now on floor %d at %s place, keys %d,"
                   " music %d\n", t, current_level + 1,
                   player.x == sx && player.y == sy ? "the same" : "ANOTHER",
                   gamestate.keys, sb.last_music);
            return;
        }
    }
}

static void registered_tests(void)
{
    const bool spear = wolf_version == WV_SPEAR;

    memset(sram, 0, sizeof sram);           /* SetupSaveGames(), as at power-on */
    save_init();
    screens_test();
    census();
    if (spear)
        spear_test();

    printf("\nbosses:\n");
    if (!spear) {
        static const int bosses[] = { bossobj, schabbobj, mechahitlerobj,
                                      giftobj, gretelobj, fatobj };
        for (int i = 0; i < (int)(sizeof bosses / sizeof bosses[0]); i++)
            boss_fight(bosses[i]);
        projectile_tests(giftobj, rocketobj);
        projectile_tests(schabbobj, needleobj);
        needle_death_test();
        projectile_tests(fakeobj, fireobj);
    } else {
        static const int bosses[] = { transobj, willobj, uberobj, deathobj,
                                      angelobj };
        for (int i = 0; i < (int)(sizeof bosses / sizeof bosses[0]); i++)
            boss_fight(bosses[i]);
        projectile_tests(willobj, rocketobj);
        projectile_tests(deathobj, hrocketobj);
        projectile_tests(angelobj, sparkobj);
    }

    /* the floors each elevator leads to */
    printf("\nfloors after each:\n");
    for (int l = 0; l < wolf_num_levels; l++) {
        const int r = use_elevator(l, true);
        if (r == USE_SECRET_ELEVATOR)
            printf("  floor %d has the secret elevator, to floor %d; back from there"
                   " to floor %d\n", l + 1, next_floor(l, true) + 1,
                   next_floor(next_floor(l, true), false) + 1);
    }

    /* the stats screen part way through the game, and a secret floor's */
    {
        const int l = spear ? 12 : 12;
        level_load(l);
        gamestate.killtotal = 20;
        gamestate.killcount = 15;
        gamestate.secretcount = 0;
        gamestate.treasurecount = gamestate.treasuretotal / 2;
        gamestate.timecount = 100 * 70;
        run_intermission(l, 0, 0, "stats_floor13");
        level_load(spear ? 18 : 19);
        run_intermission(spear ? 18 : 19, 0, 0, "stats_secret");
        if (spear) {
            level_load(4);
            run_intermission(4, 0, 0, "stats_boss");
        }
    }

    save_tests();

    /* A save of the other game is shown as an empty slot, and outlives a
     * save made over another slot. (The sim pretends to be the other build
     * by changing wolf_version.) */
    {
        const int real = wolf_version, other = spear ? WV_FULL : WV_SPEAR;
        bool mine_hidden, kept;
        memset(sram, 0, sizeof sram);
        save_init();
        game_new();
        level_load(0);
        save_game(0, "this game's");
        wolf_version = other;
        save_init();
        mine_hidden = !save_slot_used(0);
        save_game(1, "the other game's");
        wolf_version = real;
        save_init();
        kept = save_slot_used(0) && !save_slot_used(1) && load_game(0);
        printf("\n  a save seen from the other game: %s; after it saves in"
               " another slot, still here and loads: %s\n",
               mine_hidden ? "empty (right)" : "SHOWN", kept ? "yes" : "NO");
        memset(sram, 0, sizeof sram);
    }

    /* The high score table is one game's as well: the other game sees the
     * default table, and what it writes there leaves this one's alone. */
    {
        const int real = wolf_version, other = spear ? WV_FULL : WV_SPEAR;
        bool theirs_fresh, mine_kept = true;
        memset(sram, 0, sizeof sram);
        save_init();
        high_score_insert(123456, 9, 2);
        snprintf(high_scores[0].name, sizeof high_scores[0].name, "this game");
        save_high_scores();

        wolf_version = other;
        save_init();
        theirs_fresh = high_scores[0].score == 10000
                       && strcmp(high_scores[0].name, "this game") != 0;
        high_score_insert(999999, 1, 0);
        snprintf(high_scores[0].name, sizeof high_scores[0].name, "the other");
        save_high_scores();

        wolf_version = real;
        save_init();
        if (high_scores[0].score != 123456
            || strcmp(high_scores[0].name, "this game"))
            mine_kept = false;
        for (int i = 0; i < MAX_SCORES; i++)
            if (high_scores[i].score == 999999)
                mine_kept = false;
        printf("  the table seen from the other game: %s; after it writes its"
               " own, this one's is intact: %s\n",
               theirs_fresh ? "the default one (right)" : "OURS",
               mine_kept ? "yes" : "NO");
        memset(sram, 0, sizeof sram);
        save_init();
    }
}

static void readme_machinegun(void)
{
    gamestate.bestweapon = gamestate.weapon = gamestate.chosenweapon = wp_machinegun;
    gamestate.ammo = 32;
}

/* Clean in-game views on floor 1 for the README's screenshots, taken
 * with `sim <wolf3d.dat> <outdir> readme`: the cell the game starts in,
 * a guard taking machine gun fire down the hall, and the scenery from a few steps
 * back - the last two with the machine gun. */
static void readme_shots(void)
{
    char label[32];

    game_new();
    level_load(0);
    fade_in(1);
    fade_update(1);
    render_view();
    capture("readme_cell");

    game_new();
    level_load(0);
    fade_in(1);
    fade_update(1);
    readme_machinegun();
    objtype *g = setup_facing(guardobj, 5, false);
    if (g) {
        g->hitpoints = 1000;                    /* on its feet for the shot */
        for (int t = 0, taken = 0, next = 0; t < 70 * 20 && taken < 12; t++) {
            /* aimed square at it, as a player's would be */
            double a = atan2((double)(player.y - g->y), (double)(g->x - player.x));
            if (a < 0)
                a += 2 * M_PI;
            player.angle = (int)(a / (2 * M_PI) * FINEANGLES) % FINEANGLES;
            gamestate.ammo = 32;
            frame(0, 0, true);
            gamestate.health = 100;
            if (t >= next && gamestate.weaponframe == 2 && wolf_palette == wolf_palettes) {
                next = t + 10;
                snprintf(label, sizeof label, "readme_guard_s%d_t%d",
                         g->state->shapenum, t);
                capture(label);
                taken++;
            }
        }
    }

    game_new();
    level_load(0);
    fade_in(1);
    fade_update(1);
    readme_machinegun();
    for (int i = 0, shown = 0; i < numstats && shown < 16; i++) {
        if (!view_static_at(i, 4))
            continue;
        player_area = area_at(player.tilex, player.tiley);
        connect_areas();
        render_view();
        snprintf(label, sizeof label, "readme_room_%d", i);
        capture(label);
        shown++;
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    int door;

    if (argc < 3) {
        fprintf(stderr, "usage: sim <wolf3d.dat> <outdir> [readme]\n");
        return 1;
    }
    snprintf(outdir, sizeof outdir, "%s", argv[2]);

    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc(size);
    if (fread(blob, 1, size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    {
        char big[512];
        const char *slash = strrchr(argv[1], '/');
        const char *bslash = strrchr(argv[1], '\\');
        int dir = 0;
        if (bslash && (!slash || bslash > slash))
            slash = bslash;
        if (slash)
            dir = (int)(slash - argv[1]) + 1;
        snprintf(big, sizeof big, "%.*swolfbig.dat", dir, argv[1]);
        big_file = fopen(big, "rb");
        if (!big_file) { perror(big); return 1; }
    }

    if (!assets_init(blob)) { fprintf(stderr, "bad blob\n"); return 1; }
    host_byteswap();
    build_tables();
    sd_init();
    game_new();

    printf("%d levels, %d wall pages, DOORWALL = %d\n",
           wolf_num_levels, wolf_wall_pages, DOORWALL);

    if (argc > 3 && !strcmp(argv[3], "readme")) {
        readme_shots();
        printf("%d frames written to %s\n", shot, outdir);
        return 0;
    }

    /* --- the start of every shareware floor --- */
    for (int l = 0; l < wolf_num_levels; l++) {
        char label[32];
        level_load(l);
        printf("floor %2d %-16s %2d doors, %3d props, %2d treasure, spawn (%2d,%2d)\n",
               l + 1, assets_level_name(l), doornum, numstats,
               gamestate.treasuretotal, player.tilex, player.tiley);
        render_view();
        snprintf(label, sizeof label, "floor%d", l + 1);
        capture(label);
    }

    /* The rest checks the shareware's floors against what is known of them;
     * the registered games have checks of their own. */
    if (wolf_version != WV_SHAREWARE) {
        registered_tests();
        demo_tests();
        printf("\n%d frames written to %s\n", shot, outdir);
        return 0;
    }

    /* --- a look at the scenery, spread across the first few floors --- */
    printf("\nscenery:\n");
    for (int l = 0; l < 3; l++) {
        int shown = 0;
        level_load(l);
        for (int i = 0; i < numstats && shown < 3; i += 7) {
            char label[32];
            if (!view_static(i))
                continue;
            render_view();
            snprintf(label, sizeof label, "prop%d_%d_spr%d", l + 1, i,
                     statobjlist[i].shapenum);
            capture(label);
            shown++;
        }
    }

    /* --- walk onto pickups and check they are taken ---
     *
     * Items are grabbed during the render, the way DrawScaleds() does it,
     * so this walks a tic at a time with a frame drawn after each. */
    printf("\npickups:  (delta is for the single tic the item vanished in)\n");
    for (int l = 0; l < 3; l++) {
        int shown = 0;
        level_load(l);
        actors_init();              /* pickups, not combat: no one in the way */
        for (int i = 0; i < numstats && shown < 5; i++) {
            statobj_t *s = &statobjlist[i];
            int item = s->itemnumber, t;
            bool got = false;

            if (item <= block || s->shapenum < 0)
                continue;
            if (!view_static_at(i, 3))
                continue;

            gamestate.health = 50;           /* so food and aid are taken */
            gamestate.ammo = 10;
            gamestate.keys = 0;

            for (t = 0; t < 120 && !got; t++) {
                gametype before = gamestate;
                int others = live_bonus_count();

                tic(-BASEMOVE, 0, 0);
                render_view();

                if (s->shapenum >= 0)
                    continue;
                got = true;
                others = others - live_bonus_count() - 1;
                printf("  floor %d %-12s health %+4d  ammo %+3d  score %+5ld"
                       "  keys %d  in %2d tics%s\n",
                       l + 1, item_name(item),
                       gamestate.health - before.health,
                       gamestate.ammo - before.ammo,
                       (long)(gamestate.score - before.score),
                       gamestate.keys, t + 1,
                       others ? "  (shared the tic)" : "");
            }
            if (!got)
                printf("  floor %d %-12s NOT TAKEN after %d tics\n",
                       l + 1, item_name(item), t);
            shown++;
        }
    }

    /* --- no moonwalkers: every actor spawned into a timed state has a
     * count to run down, on every floor and difficulty, over enough
     * reloads to walk the whole random table --- */
    {
        int patrols = 0, frozen = 0;
        for (int d = gd_baby; d <= gd_hard; d++)
            for (int rep = 0; rep < 8; rep++)
                for (int l = 0; l < wolf_num_levels; l++) {
                    gamestate.difficulty = d;
                    level_load(l);
                    for (int i = 0; i < numobjs; i++) {
                        if (!actor_at(i)->state->tictime)
                            continue;
                        patrols++;
                        if (!actor_at(i)->ticcount)
                            frozen++;
                    }
                }
        game_new();
        printf("\nspawned patrols: %d, %d starting frozen on one frame (expect 0)\n",
               patrols, frozen);
    }

    /* --- enemies --- */
    enemy_tests();

    /* --- the end-of-floor screen --- */
    intermission_tests();
    face_tests();

    /* --- sound ---
     *
     * The rules sound.c ports from ID_SD.C, checked against the recording
     * backend: which voice a sound goes to, priorities, the stereo
     * position and its updates, the per-floor songs, and the sounds the
     * game's own actions produce. */
    printf("\nsound:\n");
    {
        static const char *songname[27] = { [0] = "CORNER", [2] = "WARMARCH",
            [3] = "GETTHEM", [9] = "POW", [11] = "SEARCHN", [12] = "SUSPENSE" };
        #define PRIO(s) ((int)(wolf_sound_info[s] >> 16))
        #define TICS(s) ((int)(wolf_sound_info[s] & 0xffff))
        #define DIGI(s) ((int)wolf_sound_digi[s])

        printf("  songs:");
        for (int l = 0; l < wolf_num_levels; l++) {
            level_load(l);
            printf(" %d=%s", l + 1, sb.last_music >= 0 && sb.last_music < 27
                   && songname[sb.last_music] ? songname[sb.last_music] : "?");
        }
        printf("\n");

        printf("  digitized: OPENDOOR %d CLOSEDOOR %d PUSHWALL %d ATKPISTOL %d"
               " ATKGATLING %d SLURPIE %d; AdLib: LEVELDONE %d GETAMMO %d\n",
               DIGI(OPENDOORSND), DIGI(CLOSEDOORSND), DIGI(PUSHWALLSND),
               DIGI(ATKPISTOLSND), DIGI(ATKGATLINGSND), DIGI(SLURPIESND),
               DIGI(LEVELDONESND), DIGI(GETAMMOSND));

        /* priority on the FM voice */
        level_load(0);
        sd_init();
        sb.adlib_calls = 0;
        sd_play_sound(GETAMMOSND);
        bool blocked = !sd_play_sound(HITWALLSND) && sb.last_adlib == GETAMMOSND;
        sd_play_sound(DONOTHINGSND);
        printf("  FM priority: GETAMMO(%d) then HITWALL(%d) -> %s; then"
               " DONOTHING(%d) -> %s\n", PRIO(GETAMMOSND), PRIO(HITWALLSND),
               blocked ? "ignored (right)" : "PLAYED (wrong)", PRIO(DONOTHINGSND),
               sb.last_adlib == DONOTHINGSND ? "played" : "ignored");
        for (int t = 0; t < 200; t++) sd_update(1);
        printf("  FM voice free after it ends: %s\n",
               sd_sound_playing() ? "NO (wrong)" : "yes (right)");

        /* the door by the spawn: open it and turn round while it sounds */
        level_load(0);
        sd_init();
        memset(&sb, 0, sizeof sb);
        for (int i = 0; i < 400; i++) {                 /* walk to the door */
            fixed px = player.x;
            tic(-BASEMOVE, 0, 0);
            if (player.x == px) break;
        }
        player_use(false);
        tic(0, 0, 0);
        printf("  opening the spawn door: digi %d (OPENDOOR is %d) at L%d R%d\n",
               sb.last_digi, DIGI(OPENDOORSND), sb.last_lvol, sb.last_rvol);
        int vol0 = sb.vol_calls, l0 = sb.last_lvol, r0 = sb.last_rvol;
        for (int i = 0; i < 25; i++)
            tic(0, BASEMOVE * 2, 0);                    /* turn right */
        printf("  turned right while it plays: %d position updates, L%d R%d"
               " (from L%d R%d)\n", sb.vol_calls - vol0, sb.last_lvol,
               sb.last_rvol, l0, r0);

        /* bumping a wall: HITWALL, but never over itself */
        level_load(0);
        sd_init();
        memset(&sb, 0, sizeof sb);
        player.angle = 1800;                            /* face west, wall */
        for (int i = 0; i < 70; i++)
            tic(-BASEMOVE, 0, 0);
        /* HITWALLSND is sound 0, and SD_SoundPlaying() returns the playing
         * sound's number - so while the bump plays it reads as "nothing",
         * and ClipMove() restarts it every tic you grind a wall, as in DOS. */
        printf("  70 tics walking into a wall: HITWALL started %d times"
               " (restarts every blocked tic, as DOS does)\n",
               sb.count_by_sound[HITWALLSND]);

        /* stereo: a digitized sound three tiles to one side */
        level_load(0);
        sd_init();
        player.angle = 0;                               /* facing east */
        sd_play_sound_loc_tile(OPENDOORSND, player.tilex, player.tiley + 3);
        printf("  sound 3 tiles to the right: L%d R%d (want right louder)\n",
               sb.last_lvol, sb.last_rvol);
        player.angle = 1800;                            /* now facing west */
        sd_update(1);
        printf("  after turning round:        L%d R%d (want left louder)\n",
               sb.last_lvol, sb.last_rvol);

        /* weapons and pickups make their sounds */
        memset(&sb, 0, sizeof sb);
        sd_init();
        gamestate.ammo = 10;
        gamestate.weapon = gamestate.chosenweapon = wp_pistol;
        {
            bool fl = false;
            for (int i = 0; i < 40; i++) {
                player_weapon(i == 0, fl, 0, 1);
                fl = (i == 0);
                sd_update(1);
            }
        }
        printf("  pistol shot: digi %d (ATKPISTOL is %d)\n", sb.last_digi,
               DIGI(ATKPISTOLSND));
        {
            statobj_t cross = { .itemnumber = bo_cross, .shapenum = 29 };
            sd_init();
            get_bonus(&cross);
            printf("  cross pickup: FM sound %d (BONUS1 is %d)\n", sb.last_adlib,
                   BONUS1SND);
        }
        #undef PRIO
        #undef TICS
        #undef DIGI
    }

    /* --- weapons ---
     *
     * Drives player_weapon() a tic at a time with the fire button held or
     * not, the way the ROM feeds it, and checks the ammo each weapon spends
     * against attackinfo[]: one shot per press for the pistol, a shot every
     * 12 tics held for the machine gun, two every 12 for the gatling gun. */
    printf("\nweapons:\n");
    {
        static bool fire_last;
        #define STEP(fire, n) do { for (int _i = 0; _i < (n); _i++) { \
            player_weapon((fire), fire_last, 0, 1); fire_last = (fire); } } while (0)
        const char *wname[] = { "knife", "pistol", "machine gun", "gatling gun" };

        level_load(0);
        game_new();
        player_spawn(29, 57, 1);
        fire_last = false;

        render_view(); capture("weapon_pistol_ready");

        /* pistol: a tap, then a long hold */
        gamestate.ammo = 8;
        STEP(true, 1);  STEP(false, 7);
        render_view(); capture("weapon_pistol_flash");   /* frame 2 */
        STEP(false, 40);
        printf("  pistol tap:            ammo 8 -> %d (want 7), weapon %s\n",
               gamestate.ammo, wname[gamestate.weapon]);
        STEP(true, 70); STEP(false, 30);
        printf("  pistol held 1s:        ammo 7 -> %d (want 6 - no autofire)\n",
               gamestate.ammo);

        /* machine gun and gatling, held for 60 tics */
        for (int w = wp_machinegun; w <= wp_chaingun; w++) {
            gamestate.bestweapon = gamestate.weapon = gamestate.chosenweapon = w;
            gamestate.ammo = 99;
            STEP(true, 60); STEP(false, 30);
            printf("  %-11s held 60 tics: %2d shots\n", wname[w], 99 - gamestate.ammo);
        }
        gamestate.ammo = 99;
        STEP(true, 8);
        render_view(); capture("weapon_gatling_fire");
        STEP(false, 40);

        /* run dry: knife comes out, and ammo brings the gatling back */
        gamestate.ammo = 3;
        STEP(true, 200); STEP(false, 40);
        printf("  gatling from 3 rounds: ammo %d, weapon now %s (want knife)\n",
               gamestate.ammo, wname[gamestate.weapon]);
        render_view(); capture("weapon_knife");
        player_weapon(false, false, 1, 1);
        printf("  change with no ammo:   still %s (want knife)\n",
               wname[gamestate.weapon]);
        give_ammo(8);
        printf("  pick up a clip:        %s (want gatling gun)\n",
               wname[gamestate.weapon]);

        /* cycling through what you own, both ways, and wrapping */
        gamestate.bestweapon = wp_machinegun;
        gamestate.weapon = gamestate.chosenweapon = wp_pistol;
        printf("  cycle up from pistol, own up to machine gun:");
        for (int i = 0; i < 4; i++) {
            player_weapon(false, false, 1, 1);
            printf(" %s,", wname[gamestate.weapon]);
        }
        player_weapon(false, false, -1, 1);
        printf(" down: %s\n", wname[gamestate.weapon]);

        /* no switching mid-attack */
        gamestate.weapon = gamestate.chosenweapon = wp_pistol;
        STEP(true, 1);
        player_weapon(true, true, 1, 1);
        printf("  change mid-attack:     %s (want pistol)\n",
               wname[gamestate.weapon]);
        STEP(false, 40);
        #undef STEP
    }

    /* --- push walls: push every secret wall on every floor ---
     *
     * Stand on the floor tile behind each one, face it, press use, and run
     * tics until it stops. Reports how far each travelled and checks the
     * bookkeeping: one solid tile where it came to rest, floor where it
     * started. Floor 1's first wall is also filmed. */
    printf("\npush walls:\n");
    {
        static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
        static const int face[4] = { 900, 0, 2700, 1800 };  /* N E S W */
        int filmed = 0, bad = 0, total = 0, travel_hist[4] = { 0 };

        for (int l = 0; l < wolf_num_levels; l++) {
            int pushed = 0, found = 0;
            level_load(l);
            for (int y = 1; y < MAPSIZE - 1; y++)
            for (int x = 1; x < MAPSIZE - 1; x++) {
                if (plane1[y][x] != PUSHABLETILE)
                    continue;
                found++;
                for (int d = 0; d < 4; d++) {
                    int sx = x - dx[d], sy = y - dy[d], tile, t, dist = 0;
                    if (tilemap[sy][sx] || blockmap[sy][sx])
                        continue;               /* nowhere to stand */
                    if (blockmap[y + dy[d]][x + dx[d]])
                        continue;               /* nothing to push into */
                    tile = tilemap[y][x] & 0x3f;

                    player.x = ((fixed)sx << TILESHIFT) + TILEGLOBAL / 2;
                    player.y = ((fixed)sy << TILESHIFT) + TILEGLOBAL / 2;
                    player.tilex = sx; player.tiley = sy;
                    player.angle = face[d];

                    bool film = (l == 0 && !filmed);
                    if (film) { render_view(); capture("pwall_before"); }

                    player_use(false);
                    if (!pwallstate) {
                        printf("  floor %d (%d,%d): did not start\n", l + 1, x, y);
                        bad++;
                        break;
                    }
                    for (t = 0; t < 1000 && pwallstate; t++) {
                        move_pwalls(1);
                        if (film && (t == 40 || t == 110 || t == 200)) {
                            char label[32];
                            render_view();
                            snprintf(label, sizeof label, "pwall_t%d", t);
                            capture(label);
                        }
                    }
                    if (film) { render_view(); capture("pwall_done"); filmed = 1; }

                    /* where did it come to rest? */
                    for (int k = 1; k <= 4; k++)
                        if ((tilemap[y + dy[d] * k][x + dx[d] * k] & 0x3f) == tile
                            && blockmap[y + dy[d] * k][x + dx[d] * k]) {
                            dist = k;
                            break;
                        }
                    bool ok = !tilemap[y][x] && !blockmap[y][x] && dist > 0
                              && !(tilemap[y + dy[d] * dist][x + dx[d] * dist] & 0xc0);
                    if (!ok) {
                        printf("  floor %d (%d,%d) dir %d: BAD end state"
                               " (travel %d, start tile %02x)\n",
                               l + 1, x, y, d, dist, tilemap[y][x]);
                        bad++;
                    }
                    if (dist >= 1 && dist <= 3) travel_hist[dist]++;
                    pushed++;
                    total++;
                    break;
                }
            }
            printf("  floor %2d: %2d secret walls, %2d pushed, secrets %d/%d\n",
                   l + 1, found, pushed, gamestate.secretcount,
                   gamestate.secrettotal);
        }
        printf("  travel: 1 tile %d, 2 tiles %d, 3 tiles %d;  %d pushed, %d %s\n",
               travel_hist[1], travel_hist[2], travel_hist[3], total, bad,
               bad ? "PROBLEMS" : "problems");
    }

    /* --- the face, and what the bar shows at each health band --- */
    printf("\nstatus bar:\n");
    {
        int seen[3] = { 0, 0, 0 }, changes = 0, last = gamestate.faceframe;
        for (int t = 0; t < 70 * 60; t++) {     /* a minute of game time */
            update_face(1);
            seen[gamestate.faceframe]++;
            if (gamestate.faceframe != last) {
                changes++;
                last = gamestate.faceframe;
            }
        }
        printf("  face over 60s: frame0 %d  frame1 %d  frame2 %d,"
               " %d changes (%.1fs apart)\n",
               seen[0], seen[1], seen[2], changes,
               changes ? 60.0 / changes : 0.0);
    }
    for (int hp = 100; hp >= 0; hp -= 20) {
        gamestate.health = hp;
        printf("  health %3d -> face pic %d\n", hp,
               status_face_pic() - PIC_FACE);
    }
    gamestate.health = 100;

    /* --- keys, and the doors they are for --- */
    printf("\nkeys and locked doors:\n");
    for (int l = 0; l < wolf_num_levels; l++) {
        int locked[2] = { -1, -1 }, keys_on_floor[2] = { 0, 0 };
        level_load(l);

        for (int d = 0; d < doornum; d++) {
            int lk = doorobjlist[d].lock;
            if (lk == dr_lock1 || lk == dr_lock2)
                locked[lk - dr_lock1] = d;
        }
        for (int i = 0; i < numstats; i++) {
            if (statobjlist[i].itemnumber == bo_key1) keys_on_floor[0]++;
            if (statobjlist[i].itemnumber == bo_key2) keys_on_floor[1]++;
        }
        if (locked[0] < 0 && locked[1] < 0 && !keys_on_floor[0] && !keys_on_floor[1])
            continue;

        printf("  floor %2d  gold: %d key(s), door %s   silver: %d key(s), door %s",
               l + 1, keys_on_floor[0], locked[0] >= 0 ? "yes" : "no",
               keys_on_floor[1], locked[1] >= 0 ? "yes" : "no");

        for (int k = 0; k < 2; k++) {
            if (locked[k] < 0)
                continue;
            gamestate.keys = 0;
            operate_door(locked[k]);
            bool stayed = doorobjlist[locked[k]].action == dr_closed;
            gamestate.keys = 1 << k;
            operate_door(locked[k]);
            bool opened = doorobjlist[locked[k]].action == dr_opening;
            printf("  [%s door: %s without key, %s with]",
                   k ? "silver" : "gold",
                   stayed ? "shut" : "OPENED (wrong)",
                   opened ? "opens" : "STAYED SHUT (wrong)");
        }
        printf("\n");
    }

    /* Walk onto a gold key and check the bit lands. */
    for (int l = 0; l < wolf_num_levels; l++) {
        bool done = false;
        level_load(l);
        actors_init();              /* pickups, not combat: no one in the way */
        for (int i = 0; i < numstats && !done; i++) {
            statobj_t *s = &statobjlist[i];
            if (s->itemnumber != bo_key1 || !view_static_at(i, 3))
                continue;
            gamestate.keys = 0;
            for (int t = 0; t < 120 && s->shapenum >= 0; t++) {
                tic(-BASEMOVE, 0, 0);
                render_view();
            }
            printf("  picked up the gold key on floor %d: keys = %d (%s)\n",
                   l + 1, gamestate.keys,
                   gamestate.keys & 1 ? "right" : "WRONG");
            done = true;
        }
        if (done)
            break;
    }

    /* A full-health player should leave food on the floor, as GetBonus()
     * returns before removing it. */
    level_load(0);
    for (int i = 0; i < numstats; i++) {
        statobj_t *s = &statobjlist[i];
        if (s->itemnumber != bo_food && s->itemnumber != bo_firstaid)
            continue;
        if (!view_static_at(i, 3))
            continue;
        gamestate.health = 100;
        for (int t = 0; t < 120 && s->shapenum >= 0; t++) {
            tic(-BASEMOVE, 0, 0);
            render_view();
        }
        printf("  at full health, %s was %s\n", item_name(s->itemnumber),
               s->shapenum < 0 ? "TAKEN (wrong)" : "left alone (right)");
        break;
    }

    /* --- walk into the first door on floor 1 and open it --- */
    level_load(0);
    printf("\nfloor 1 walkthrough:\n");
    fade_in(1);                             /* the game has not faded in yet */
    fade_update(1);
    render_view();
    hud_draw_fps(47);                       /* any two digits, to eyeball */
    capture("spawn");

    /* Look around from the start: a quarter turn to the right. */
    for (int i = 0; i < 25; i++)
        tic(0, BASEMOVE, 0);
    render_view();
    capture("turned");
    for (int i = 0; i < 25; i++)
        tic(0, -BASEMOVE, 0);

    /* Walk forward until a wall stops us. */
    {
        fixed px = player.x, py = player.y;
        int stuck = 0, i;
        for (i = 0; i < 400 && stuck < 3; i++) {
            tic(-BASEMOVE, 0, 0);
            stuck = (player.x == px && player.y == py) ? stuck + 1 : 0;
            px = player.x; py = player.y;
        }
        printf("  walked %d tics\n", i);
    }
    render_view();
    capture("at_wall");

    door = find_door_ahead();
    printf("  door ahead: %d\n", door);
    if (door >= 0) {
        player_use(false);
        for (int step = 0; step < 4; step++) {
            char label[32];
            for (int i = 0; i < 12; i++)
                tic(0, 0, 0);
            render_view();
            snprintf(label, sizeof label, "door%d_%d", step,
                     doorposition[door] * 100 / 0xffff);
            capture(label);
        }
        /* Walk through it and look back. */
        for (int i = 0; i < 90; i++)
            tic(-BASEMOVE, 0, 0);
        render_view();
        capture("through");
        for (int i = 0; i < 100; i++)
            tic(0, BASEMOVE * 2, 0);
        render_view();
        capture("lookback");
        automap_draw();
        capture("map_walkthrough");
        printf("  the map after the walk: %d tiles seen\n", automap_count());
    }

    /* last: they leave the screen faded for anything after them */
    save_tests();
    pad_tests();
    flow_tests();
    demo_tests();

    printf("\n%d frames written to %s\n", shot, outdir);
    return 0;
}
