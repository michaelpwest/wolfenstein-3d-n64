/* wolf.h - shared state for the Nintendo 64 port of Wolfenstein 3D.
 *
 * Constants and map semantics are taken from id's source release
 * (github.com/id-Software/wolf3d, WOLFSRC); the file each one comes from is
 * noted where it is not obvious. Coordinates are Wolfenstein's: 16.16 fixed point with one
 * tile = 1.0, +x east and +y south, and a 64x64 tile grid.
 */
#ifndef WOLF_H
#define WOLF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The port's own version - not the DOS game's, whose "v1.4" is on the
 * sign-on screen. build.ps1 and build.sh read it from this line. */
#define PORT_VERSION     "1.0.0"

/* ------------------------------------------------------------------ */
/* Map, from WL_DEF.H                                                  */
/* ------------------------------------------------------------------ */

#define MAPSIZE          64
#define TILESHIFT        16
#define TILEGLOBAL       (1L << TILESHIFT)
#define MINDIST          0x5800L            /* closest a wall may come    */
#define PLAYERSIZE       MINDIST            /* player radius              */

#define AREATILE         107                /* first floor tile           */
#define PUSHABLETILE     98                 /* plane 1: secret push wall  */
#define EXITTILE         99                 /* plane 1: level exit        */
#define ELEVATORTILE     21                 /* plane 0: elevator switch   */
#define ALTELEVATORTILE  107
#define AMBUSHTILE       106

#define MAXDOORS         64
#define MAXWALLTILES     64
#define MAXSTATS         400                /* lamps, bonus, etc. */

typedef int32_t fixed;                      /* 16.16 */

/* ------------------------------------------------------------------ */
/* Angles. Wolfenstein's FINEANGLES: 3600 units to the circle.         */
/* ------------------------------------------------------------------ */

#define FINEANGLES       3600
#define ANG90            (FINEANGLES / 4)
#define ANG180           (FINEANGLES / 2)

/* sintab is FINEANGLES + ANG90 long so cosine can overlay it at a
 * quarter-phase shift, the same trick WL_MAIN.C's BuildTables plays. */
extern fixed sintab[FINEANGLES + ANG90];
#define fsin(a)  (sintab[(a)])
#define fcos(a)  (sintab[(a) + ANG90])

void build_tables(void);

/* The DOS game's sintable, whole degrees 0..450 in sign-magnitude, and its
 * FixedByFrac() - for demo playback, see dos_exact. */
extern int32_t dos_sintable[451];
fixed fixed_by_frac(fixed a, int32_t b);

/* Multiply a 16.16 fixed by a 16.16 fraction. */
static inline fixed fixmul(fixed a, fixed b)
{
    return (fixed)(((int64_t)a * (int64_t)b) >> 16);
}

/* ------------------------------------------------------------------ */
/* Screen layout                                                       */
/* ------------------------------------------------------------------ */

#define SCREEN_W    320
#define SCREEN_H    240
#define VIEW_W      320
/* STATUSBARPIC is 320x40 and is shown at its own size, so the 3D view gets
 * the other 200 rows. That is eight more than the DOS game's share of the
 * screen would be (160 of 200 VGA rows, which are 1.2x taller than these),
 * so the view shows a sliver more floor and ceiling - worth it to keep the
 * status bar art pixel for pixel. */
#define STATUS_H    40
#define VIEW_H      (SCREEN_H - STATUS_H)

/* VIEW_STRIDE is deliberately not VIEW_W. Wall columns are written down
 * the buffer, and a 320-pixel stride is 40 cache lines, which collides
 * every column onto the same 32 sets of the VR4300's data cache. 328 is
 * 41 lines - odd, so consecutive rows spread over every set. */
#define VIEW_STRIDE 328

/* Pixel height of a one-tile wall one tile away. CalcProjection() in
 * WL_MAIN.C works out 218.75 VGA rows; a VGA row is 1.2x taller than one
 * of these, so the same wall is 262 rows here. This follows from the
 * horizontal field of view and the pixel shape, not from VIEW_H - a taller
 * view just shows more floor and ceiling. */
#define SCALE_PX    262

extern uint16_t viewbuf[VIEW_H][VIEW_STRIDE];

/* The 3D view is drawn column by column - walls, scenery, the hands - and a
 * column of viewbuf is a pixel in every one of 200 rows, 656 bytes apart:
 * each store lands on a cache line of its own. So render_view() draws into
 * colbuf instead, one column after another in memory, and the platform shows
 * it turned on its side (main.c). view_columns says which buffer holds the
 * picture; the small things drawn over a view (the frame rate, the paused
 * sign) go wherever it is, and whole-screen effects (fizzles, fades) first
 * turn it back into rows with view_to_rows(). */
extern uint16_t colbuf[VIEW_W][VIEW_H];
extern bool     view_columns;
void view_to_rows(void);

static inline uint16_t *view_pixel(int x, int y)
{
    return view_columns ? &colbuf[x][y] : &viewbuf[y][x];
}

/* ------------------------------------------------------------------ */
/* Assets (assets.c) - views into the linked-in blob                   */
/* ------------------------------------------------------------------ */

#define WALL_PAGE_SIZE  4096                /* 64x64 CI8, column major   */

extern const uint16_t *wolf_palette;        /* 256 entries, RGBA5551     */
extern const uint16_t *wolf_palettes;       /* ten of them - see below   */
extern const uint8_t  *wolf_walls;          /* wall_pages * 4096 bytes   */
extern int             wolf_wall_pages;
extern int             wolf_num_levels;

/* The last eight wall pages are the door art - DOORWALL in WL_DRAW.C. */
#define DOORWALL        (wolf_wall_pages - 8)

/* The static-object sprites, SPR_STAT_0..SPR_STAT_47. They are compiled
 * shapes: a per-column list of vertical runs, so transparency is structural
 * and the renderer never has to test for it. A sprite record is
 *
 *   rec[0]      byte offset from the sprite base to this sprite's pixels
 *   rec[1+c]    column c: 0, or (run count << 24) | byte offset to its runs
 *
 * and each run is (starty << 24) | (length << 16) | offset into the pixels.
 */
#define SPRITE_RUN_START(r)  ((int)((r) >> 24))
#define SPRITE_RUN_LEN(r)    ((int)(((r) >> 16) & 0xff))
#define SPRITE_RUN_PIX(r)    ((int)((r) & 0xffff))

extern const uint8_t *wolf_sprites;          /* sprite section base        */
extern int            wolf_num_sprites;

/* Sprite numbers (sprites.h) are WL_DEF.H's enum for Wolfenstein, which
 * VSWAP.WL1 keeps too (leaving the registered-only sprites empty), with
 * Spear of Destiny's own sprites numbered after it; each version's are
 * placed by name. assets_sprite() returns NULL for any number the data does
 * not carry. The weapons: weapon w, frame f is SPR_KNIFEREADY + w*5 + f. */
#include "sprites.h"

/* The palette variants InitRedShifts() builds: 0 is the game palette,
 * 1..6 the red damage steps, 7..9 the white bonus steps. */
#define NUMREDSHIFTS    6
#define NUMWHITESHIFTS  3
void set_palette_shift(int which);

/* The artwork out of VGAGRAPH, already un-planarized into plain CI8 rows.
 * The order is fixed by tools/extract_assets.py; a pic a version does not
 * have is 0x0. */
enum {
    PIC_STATUSBAR,
    PIC_KNIFE, PIC_GUN, PIC_MACHINEGUN, PIC_GATLINGGUN,
    PIC_NOKEY, PIC_GOLDKEY, PIC_SILVERKEY,
    PIC_BLANK,
    PIC_N0,                                  /* .. PIC_N0 + 9             */
    PIC_FACE = PIC_N0 + 10,                  /* 22 frames, FACE1A..FACE8A */
    /* the LEVELEND lump, for LevelCompleted() */
    PIC_L_GUY = PIC_FACE + 22,               /* BJ, and his other breath  */
    PIC_L_COLON,
    PIC_L_NUM0,                              /* .. PIC_L_NUM0 + 9         */
    PIC_L_PERCENT = PIC_L_NUM0 + 10,
    PIC_L_A,                                 /* .. PIC_L_A + 25           */
    PIC_L_EXPOINT = PIC_L_A + 26,
    PIC_L_APOSTROPHE,
    PIC_L_GUY2,
    PIC_GOTGATLING,                          /* BJ's grin                 */
    PIC_GETPSYCHED,
    PIC_L_BJWINS,                            /* the victory screen        */
    /* the frame round a text page, ShowArticle() */
    PIC_H_TOPWINDOW, PIC_H_LEFTWINDOW, PIC_H_RIGHTWINDOW, PIC_H_BOTTOMINFO,
    PIC_PAUSED,
    /* the CONTROLS lump, for the menus */
    PIC_C_OPTIONS, PIC_C_CURSOR1, PIC_C_CURSOR2,
    PIC_C_NOTSELECTED, PIC_C_SELECTED,
    PIC_C_FXTITLE, PIC_C_DIGITITLE, PIC_C_MUSICTITLE,
    PIC_C_MOUSELBACK,
    PIC_C_BABYMODE,                          /* .. + 3, one per difficulty */
    PIC_C_EPISODE1 = PIC_C_BABYMODE + 4,     /* .. + 5                     */
    /* the attract loop */
    PIC_TITLE = PIC_C_EPISODE1 + 6, PIC_PG13, PIC_CREDITS, PIC_HIGHSCORES,
    PIC_C_LEVEL, PIC_C_NAME, PIC_C_SCORE,
    /* Load Game and Save Game */
    PIC_C_DISKLOADING1, PIC_C_DISKLOADING2, PIC_C_LOADGAME, PIC_C_SAVEGAME,
    PIC_C_CONTROL, PIC_C_CUSTOMIZE,
    PIC_C_TIMECODE,                         /* the full game's only */
    /* Spear of Destiny's only: its face for a big hit, the menus' backdrop
     * and skill title, the high scores' Spear, BJ's collapse, the title in
     * two halves and the end screens (these with palettes of their own -
     * assets_pic_palette()), and god mode's faces */
    PIC_BJOUCH,
    PIC_C_BACKDROP, PIC_C_HOWTOUGH, PIC_C_WONSPEAR,
    PIC_BJCOLLAPSE1,                        /* .. + 3 */
    PIC_TITLE1 = PIC_BJCOLLAPSE1 + 4, PIC_TITLE2,
    PIC_ENDSCREEN11, PIC_ENDSCREEN12,
    PIC_ENDSCREEN3,                         /* .. PIC_ENDSCREEN3 + 6 is 9 */
    PIC_GODFACE1 = PIC_ENDSCREEN3 + 7,      /* .. + 2 */
    PIC_BJWAITING1 = PIC_GODFACE1 + 3,      /* .. + 1: BJ standing about */
    PIC_BJWAITING2,
    PIC_C_MOUSELBACK_LR,                    /* its arrows turned: extractor */
    PIC_C_MOUSELBACK_TYPING,                /* for typing a name, 144 wide  */
    PIC_SIGNON,                          /* from WOLF3D.EXE; 0x0 if absent */
    PIC_MUTANTBJ,                    /* killed by a syringe; not in Spear */
    NUM_PICS                                /* the articles' own pics follow */
};

/* The blob's 16- and 32-bit fields are big-endian, which is free on the N64
 * and is why tools/sim byte-swaps them after loading. The maps and the
 * full-screen pictures are in the second file, wolfbig.dat, which the
 * platform reads: `len` bytes from `offset` into `dst` (the ROM from its
 * filesystem, tools/sim from the file). */
bool assets_init(const uint8_t *blob);
void platform_read_big(uint32_t offset, void *dst, uint32_t len);
void            assets_load_level(int level, uint16_t *plane0, uint16_t *plane1);
const char     *assets_level_name(int level);
const uint32_t *assets_sprite(int n);               /* NULL if not in the data */
const uint8_t  *assets_pic(int n, int *w, int *h);  /* 0x0 if not in the data  */
/* whether the data has fixed pic n at all: data extracted before
 * PIC_MUTANTBJ was added has the articles' first pic in its place */
bool            assets_has_pic(int n);
/* the palette a pic is drawn with, if not the game's; else NULL */
const uint16_t *assets_pic_palette(int n);

/* Which game the data is. The shareware's is laid out by the full game's
 * numbering, with only episode one; Spear of Destiny's by its own. */
enum { WV_SHAREWARE, WV_FULL, WV_SPEAR };
extern int wolf_version;

/* The text section: VGAGRAPH's two fonts as they were (little-endian
 * fontstructs: height, 256 offsets, 256 widths, then the glyphs) - 0 the
 * small one for text pages, 1 the menu font - and the "Read This!" article
 * (empty if the data has none) and the end-of-episode ones, with their pic
 * numbers already turned into PIC_ indices. */
extern const uint8_t *wolf_fonts[2];
extern const char    *wolf_help_article;
extern const char    *wolf_end_articles[6];
extern int            wolf_num_end_articles;

/* T_DEMO0..3: a byte of floor, a little-endian word of total length, a
 * spare byte, then per frame of four tics a byte of buttons and signed
 * bytes of controlx and controly. */
extern const uint8_t *wolf_demos[4];
/* ------------------------------------------------------------------ */
/* Level (level.c)                                                     */
/* ------------------------------------------------------------------ */

/* tilemap holds what the renderer and the collision test need, in the
 * encoding SetupGameLevel()/SpawnDoor() use:
 *   0          floor
 *   1..63      wall tile, indexes horizwall[]/vertwall[]
 *   | 0x40     that wall is a door jamb (draw the door-side texture)
 *   0x80 | n   door number n
 * Indexed [y][x] - the reverse of the DOS game, so that a row of the map
 * is contiguous for the ray caster. */
extern uint8_t  tilemap[MAPSIZE][MAPSIZE];
extern uint16_t plane0[MAPSIZE][MAPSIZE];   /* mutable copy: area numbers */
extern uint16_t plane1[MAPSIZE][MAPSIZE];

/* actorat, reduced to what this build needs: nonzero means "cannot walk
 * here". Doors write themselves in and out of it as they open and close,
 * and blocking scenery sets it once at load. */
extern uint8_t  blockmap[MAPSIZE][MAPSIZE];

/* spotvis: set by the ray caster for every tile a ray passed through, so
 * that only scenery in view is considered for drawing. Same idea as the
 * DOS game's spotvis, cleared every frame. */
extern uint8_t  spotvis[MAPSIZE][MAPSIZE];

/* The map (automap.c): every tile any ray has reached on this floor, kept
 * from frame to frame, and saved with the game. Cleared by level_load(). */
extern uint8_t  automap_seen[MAPSIZE][MAPSIZE];
void automap_clear(void);
int  automap_count(void);                   /* tiles seen, for tools/sim */
void automap_draw(void);                    /* over the view area        */

extern int      current_level;

void level_load(int level);
int  level_ceiling_color(int level);

/* Areas: every floor tile's plane-0 value is AREATILE + its room number.
 * An open door connects the rooms on either side; areabyplayer marks every
 * room connected to the one the player is in. Actors only hear or see the
 * player - or get simulated at all, until first seen - in those rooms.
 * WL_ACT1.C's areaconnect/ConnectAreas. */
#define NUMAREAS  37

extern bool    areabyplayer[NUMAREAS];
extern uint8_t areaconnect[NUMAREAS][NUMAREAS];     /* open doors between */
extern int     player_area;                 /* player->areanumber */

void areas_init(void);
void connect_areas(void);
int  area_at(int tilex, int tiley);         /* 0..NUMAREAS-1 */

/* ------------------------------------------------------------------ */
/* Doors (doors.c) - WL_ACT1.C                                         */
/* ------------------------------------------------------------------ */

enum { dr_normal, dr_lock1, dr_lock2, dr_lock3, dr_lock4, dr_elevator };
enum { dr_open, dr_closed, dr_opening, dr_closing };

typedef struct {
    uint8_t tilex, tiley;
    uint8_t vertical;
    uint8_t lock;
    uint8_t action;
    int16_t ticcount;
} doorobj_t;

extern doorobj_t doorobjlist[MAXDOORS];
extern uint16_t  doorposition[MAXDOORS];    /* 0 shut .. 0xffff wide open */
extern int       doornum;

void doors_init(void);
void spawn_door(int tilex, int tiley, bool vertical, int lock);
void operate_door(int door);
void open_door(int door);                   /* OpenDoor(), for actors too */
void move_doors(int tics);
int  door_page(int door);                   /* first of the two wall pages */

/* Push walls, the other half of WL_ACT1.C. Only one moves at a time. The
 * sliding tile is marked in tilemap with both high bits set (0xc0 | tile),
 * which no door or jamb can produce, and pwallpos is how far it has slid
 * into the next tile, in 64ths. */
enum { di_north, di_east, di_south, di_west };

extern int pwallstate;                      /* 0 when nothing is moving */
extern int pwallpos;                        /* 0..63 */
extern int pwallx, pwally, pwalldir;        /* the tile it is in, and where to */

void pushwall_init(void);
bool push_wall(int checkx, int checky, int dir);
void move_pwalls(int tics);

/* ------------------------------------------------------------------ */
/* Game state (player.c) - WL_DEF.H's gametype, trimmed to what exists */
/* ------------------------------------------------------------------ */

#define STARTAMMO    8
#define EXTRAPOINTS  40000

enum { wp_knife, wp_pistol, wp_machinegun, wp_chaingun };

/* The New Game menu's four settings. With no menu yet the game starts on
 * gd_medium, "Bring 'em on!". It decides which map actors exist at all
 * (ScanInfoPlane) and how many hit points bosses have. */
enum { gd_baby, gd_easy, gd_medium, gd_hard };

typedef struct {
    int     difficulty;
    int     health;                         /* 0..100                    */
    int     ammo;                           /* 0..99                     */
    int     lives;
    int32_t score;
    int32_t oldscore;                       /* score as the floor began  */
    int     killcount, killtotal;           /* this floor                */
    int32_t nextextra;                      /* score for the next 1-up   */
    int     keys;                           /* bit 0 gold, bit 1 silver  */
    int     weapon, bestweapon;
    int     chosenweapon;                   /* what weapon goes back to  */
    int     attackframe, attackcount;       /* place in attackinfo[]     */
    int     weaponframe;                    /* 0 ready, 1..4 attack art  */
    int     treasurecount, treasuretotal;   /* this floor                */
    int     secretcount, secrettotal;       /* push walls, this floor    */
    int     faceframe;                      /* 0..2, which way BJ looks  */
    int32_t timecount;                      /* tics spent on this floor  */
    bool    victoryflag;                    /* watching BJ run for it    */
    fixed   killx, killy;                   /* where the player killed a
                                             * boss with a death cam from */
} gametype;

extern gametype gamestate;

/* The port numbers floors from the start of the data. For Wolfenstein that
 * is gamestate.episode * 10 + mapon; Spear of Destiny is one run of 21. */
static inline int level_episode(int level)
{
    return wolf_version == WV_SPEAR ? 0 : level / 10;
}
static inline int level_mapon(int level)
{
    return wolf_version == WV_SPEAR ? level : level % 10;
}

void game_new(void);                        /* NewGame()                 */
int  us_rndt(void);                         /* US_RndT()                 */
void us_init_rndt(void);                    /* US_InitRndT(false)        */
int  us_rnd_index(void);                    /* for tools/sim's demo traces */
void us_set_rnd_index(int index);           /* and for a loaded game       */

/* Demo playback needs the simulation to come out as it did on the PC that
 * recorded it, so while this is set the player moves with the DOS game's
 * own arithmetic - whole-degree turns, its sine table and FixedByFrac(),
 * thrustspeed per frame, push walls a frame's tics at a time - and each
 * level starts the random table from the top. */
extern bool dos_exact;
void give_points(int32_t points);
void give_ammo(int ammo);
void give_key(int key);
void heal_self(int points);
void give_weapon(int weapon);               /* GiveWeapon() */
void cheat_mli(void);                       /* M-L-I */
void cheat_free_items(void);                /* Tab-I */

/* ------------------------------------------------------------------ */
/* Static scenery (statics.c) - WL_ACT1.C's statinfo[] and SpawnStatic */
/* ------------------------------------------------------------------ */

/* stat_t from WL_DEF.H. Everything from bo_gibs on is a pickup, which is
 * what SpawnStatic() flags as FL_BONUS. */
enum {
    dressing, block,
    bo_gibs, bo_alpo, bo_firstaid, bo_key1, bo_key2, bo_key3, bo_key4,
    bo_cross, bo_chalice, bo_bible, bo_crown, bo_clip, bo_clip2,
    bo_machinegun, bo_chaingun, bo_food, bo_fullheal, bo_25clip, bo_spear
};

typedef struct {
    uint8_t tilex, tiley;
    int16_t shapenum;                       /* SPR_STAT_n, -1 once taken */
    uint8_t itemnumber;                     /* bo_*, or dressing/block   */
} statobj_t;

extern statobj_t statobjlist[MAXSTATS];
extern int       numstats;

void statics_init(void);
void spawn_static(int tilex, int tiley, int type);
void place_item_type(int itemtype, int tilex, int tiley);   /* drops     */
void get_bonus(statobj_t *s);               /* GetBonus()                */

/* ------------------------------------------------------------------ */
/* Player (player.c)                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    fixed x, y;
    int   angle;                            /* 0..FINEANGLES-1, 0 = east */
    int   tilex, tiley;
    int   anglefrac;
} player_t;

extern player_t player;

/* Wolfenstein's own sign conventions, from PollControls(): positive turns
 * and strafes to the right, positive forward walks backwards. Magnitudes
 * are BASEMOVE/RUNMOVE times tics, as PollKeyboardMove() scales them. */
#define BASEMOVE  35
#define RUNMOVE   70

typedef struct {
    int turn;
    int forward;
    int strafe;
} controls_t;

extern bool     exit_triggered;             /* stepped on the exit tile */

/* How fast the player moved this tic - Thrust()'s thrustspeed, which enemy
 * marksmanship reads - and whether a gun went off (madenoise), which
 * wakes up every enemy within earshot. */
extern int32_t  thrustspeed;
extern bool     madenoise;
#define RUNSPEED  6000

void player_spawn(int tilex, int tiley, int dir);
void player_move(const controls_t *c, int tics);
/* Cmd_Use(). Returns what the player did to the level: nothing, rode the
 * elevator, or rode it from the ALTELEVATORTILE spot that leads to the
 * secret floor. */
enum { USE_NOTHING, USE_ELEVATOR, USE_SECRET_ELEVATOR };
int  player_use(bool held);                 /* held: buttonheld[bt_use]    */

/* CheckWeaponChange() with the number keys: 0..3, if owned and loaded. */
void player_select_weapon(int weapon);

/* GameLoop()'s choice of the next floor: back from the secret floor to
 * ElevatorBackTo[], to the secret floor, or simply the next one. */
int  next_floor(int level, bool secret);

/* Spear of Destiny's Spear, picked up: where the player stood, for the last
 * floor, which player_place() puts them at. */
extern bool  spear_taken;
extern fixed spear_x, spear_y;
extern int   spear_angle;
void player_place(fixed x, fixed y, int angle);

/* TakeDamage(). Sets player_died when health runs out; `attacker` is kept
 * so the death sequence can turn to face them. */
struct objstruct;
extern bool player_died;
extern struct objstruct *killerobj;
extern int last_attacker_class;             /* LastAttacker->obclass      */
void take_damage(int points, struct objstruct *attacker);
bool player_face_killer(int tics);          /* Died()'s turn; true when done */
void player_victory_spin(int tics);         /* VictorySpin()                 */
bool player_lose_life(void);                /* false: that was the last one */

/* The red and white palette flashes, WL_PLAY.C's UpdatePaletteShifts(). */
void start_damage_flash(int damage);
void start_bonus_flash(void);
void clear_palette_shifts(void);
void update_palette_shifts(int tics);
void palette_shift_counts(int *damage, int *bonus);     /* for a save */
void set_palette_shift_counts(int damage, int bonus);

/* Weapons: Cmd_Fire, T_Attack and CheckWeaponChange. `fire` is the fire
 * button now and `fireheld` what it was last frame - buttonstate and
 * buttonheld in the DOS code, which tell a fresh press from a held one.
 * `change` is +1/-1 to cycle weapons, 0 to leave them. */
void player_weapon(bool fire, bool fireheld, int change, int tics);
bool player_attacking(void);                /* the DOS game's s_attack */

#define WEAPON_FRAMES     5

/* ------------------------------------------------------------------ */
/* Actors (actors.c) - WL_STATE.C and WL_ACT2.C: every enemy of the    */
/* three games, their projectiles, and the death cam                   */
/* ------------------------------------------------------------------ */

#define MAXACTORS     150
#define MINACTORDIST  0x10000L              /* closest an actor may come */

/* dirtype: east, then counter-clockwise in eighths; nodir = 8. */
enum { east, northeast, north, northwest, west, southwest, south, southeast,
       nodir };

/* classtype. Saved games store these, so the shareware's five keep their
 * numbers and the rest follow in WL_DEF.H's order. */
enum {
    inertobj, guardobj, ssobj, dogobj, bossobj, bjobj,
    officerobj, mutantobj, schabbobj, fakeobj, mechahitlerobj, realhitlerobj,
    gretelobj, giftobj, fatobj, ghostobj,
    needleobj, fireobj, rocketobj,                  /* the projectiles */
    spectreobj, angelobj, transobj, uberobj, willobj, deathobj,  /* Spear */
    hrocketobj, sparkobj
};

#define FL_SHOOTABLE    1
#define FL_NEVERMARK    4                   /* never in actorat           */
#define FL_VISABLE      8
#define FL_ATTACKMODE   16
#define FL_FIRSTATTACK  32
#define FL_AMBUSH       64
#define FL_NONMARK      128                 /* in actorat only if it is free */

typedef struct objstruct objtype;

/* statetype: a frame of animation, how many tics it lasts (0 = forever,
 * thinking every tic), what to do each tic and on leaving it. rotate 1
 * means eight directional views follow shapenum; 2 means the two-view
 * pain frames. */
typedef struct statestruct {
    uint8_t rotate;
    int16_t shapenum;
    int16_t tictime;
    void  (*think)(objtype *);
    void  (*action)(objtype *);
    const struct statestruct *next;
} statetype;

struct objstruct {
    bool     active;
    int      ticcount;
    uint8_t  obclass;
    const statetype *state;
    uint8_t  flags;
    int32_t  distance;                      /* <0: waiting on door -1-d  */
    uint8_t  dir;
    fixed    x, y;
    int      tilex, tiley;
    int      areanumber;
    int      viewx, viewheight;             /* set by the renderer        */
    int      aimx;                          /* viewx as GunAttack() sees it */
    fixed    transx;
    int      hitpoints;
    int32_t  speed;
    int      temp1;                         /* the Angel's shots in a row */
    int      temp2;                         /* reaction-time countdown    */
    int      angle;                         /* a projectile's, 0..359     */
};

/* The actors are a list, as objlist was in DOS: new ones go on the end,
 * DoActor() runs down it, and projectiles and smoke leave it when they are
 * done. obj_order holds the slots in list order, so a slot keeps its actor
 * for life - actorat and killerobj point at them - and a freed slot is the
 * next one given out, as DOS's free list worked. */
extern objtype  objlist[MAXACTORS];
extern uint8_t  obj_order[MAXACTORS];
extern int      numobjs;
#define actor_at(i)  (&objlist[obj_order[i]])

/* actorat for actors: which one stands on each tile, or NULL. Walls, doors
 * and blocking scenery stay in blockmap. */
extern objtype *actorat[MAPSIZE][MAPSIZE];

void actors_init(void);
void spawn_map_actor(int tile, int tilex, int tiley, bool ambush);
void actors_think(int tics);                /* DoActor() over the list   */
void gun_attack(void);                      /* GunAttack()               */
void knife_attack(void);                    /* KnifeAttack()             */
int  actor_shape(objtype *ob);              /* shapenum incl. CalcRotate */

/* For saved games: states by number (-1 / NULL for one not in the tables),
 * and the list as slots - the order, then the free slots, next first. */
int              actor_state_number(const statetype *state);
const statetype *actor_state(int number);
int              actor_free_slots(uint8_t *slots);
bool             actor_set_list(const uint8_t *order, int count,
                                const uint8_t *free_slots, int nfree);

/* VictoryTile(): BJ runs out of the castle ahead of the camera. victory_done
 * is playstate = ex_victorious, from T_BJDone() - or from the second
 * A_StartDeathCam(), or Spear of Destiny's A_Victory(). */
extern bool victory_done;
void victory_tile(void);

/* The death cam, A_StartDeathCam(): a boss that has one has died and the
 * player has been moved to watch it happen again. The rest of it - the
 * wait, the fizzle to "Let's see that again!", the fizzle back in - is the
 * game's to show (game.c), after which play goes on with the player still. */
extern bool deathcam_started;               /* this frame: show the interlude */
extern bool deathcam;                       /* the player is only watching */

/* ------------------------------------------------------------------ */
/* Renderer (render.c)                                                 */
/* ------------------------------------------------------------------ */

void render_view(void);

/* Change View. The 3D view is 16 * size pixels wide and 10 * size tall,
 * centered in the 320x200 area with DrawPlayBorder()'s gray round it; size
 * 20, the default, fills it. set_view_size() is NewViewSize(): the
 * projection, the aim and the hands all follow the width.
 * view_border_dirty asks the next render_view() to redraw the border,
 * after something has drawn over it. */
#define VIEW_SIZE_MIN  4
#define VIEW_SIZE_MAX  20
extern int  view_size;
extern int  view_width, view_height, view_x, view_y;
extern bool view_border_dirty;
void set_view_size(int size);
void draw_play_border(int size);            /* the border, the view black */
void view_border_restore(int x, int y, int w, int h);   /* under an overlay */

/* Profiling. When the platform sets prof_mark, the game calls it as each
 * stretch of a frame begins, and the time until the next mark counts toward
 * that stretch (see main.c). tools/sim leaves it NULL. */
enum {
    PROF_LOGIC,         /* the controller, the game, the menus          */
    PROF_BACKGROUND,    /* the frame's setup, spotvis cleared           */
    PROF_RAYS,          /* ray casting                                  */
    PROF_COLUMNS,       /* each column: ceiling, scaled wall, floor     */
    PROF_COLLECT,       /* finding and projecting what is in view       */
    PROF_SPRITES,       /* sorting and drawing scenery and actors       */
    PROF_WEAPON,        /* the hands                                    */
    PROF_STATUS,        /* composing the status bar                     */
    PROF_PRESENT,       /* fade copy, cache flush, RDP blits, show      */
    PROF_WAIT,          /* display_get() waiting for a free buffer      */
    PROF_AUDIO,         /* filling the audio buffers                    */
    PROF_OVERLAY,       /* drawing these figures on the screen          */
    PROF_SECTIONS
};
extern void (*prof_mark)(int section);
#define PROF(section)  do { if (prof_mark) prof_mark(section); } while (0)

/* FizzleFade(). Either to a color, as Died() does, or - fizzle_in_start() -
 * from what is on screen to a freshly rendered view, as a floor starts.
 * `frames` is how many 70 Hz frames the whole dissolve takes. */
void fizzle_start(void);
void fizzle_start_whole(void);              /* the border too: the death cam */
void fizzle_in_start(void);
bool fizzle_step(int tics, int frames, uint16_t color); /* true when done */
bool fizzle_in_step(int tics, int frames);

/* VWB_DrawPic() and VWB_Bar() into viewbuf: x rounds down to a multiple of
 * eight as it did on the planar VGA, and anything off the view is clipped. */
void view_pic(int x, int y, int picnum);
void view_bar(int x, int y, int w, int h, int color);

/* ------------------------------------------------------------------ */
/* Sound (sound.c) - SD_PlaySound and friends                         */
/* ------------------------------------------------------------------ */

/* The game's sound numbers: AUDIOWL6.H's, which the shareware's data is
 * laid out by too, then the sounds only Spear of Destiny has. The blob maps
 * each to its own data's number (wolf_sound_map), or to none where that
 * version lacks it - Spear of Destiny has no Mutti or Gut'n Tag. */
enum {
    HITWALLSND = 0, SELECTWPNSND = 1, SELECTITEMSND = 2, HEARTBEATSND = 3,
    MOVEGUN2SND = 4, MOVEGUN1SND = 5, NOWAYSND = 6, NAZIHITPLAYERSND = 7,
    SCHABBSTHROWSND = 8, PLAYERDEATHSND = 9, DOGDEATHSND = 10, ATKGATLINGSND = 11,
    GETKEYSND = 12, NOITEMSND = 13, WALK1SND = 14, WALK2SND = 15,
    TAKEDAMAGESND = 16, GAMEOVERSND = 17, OPENDOORSND = 18, CLOSEDOORSND = 19,
    DONOTHINGSND = 20, HALTSND = 21, DEATHSCREAM2SND = 22, ATKKNIFESND = 23,
    ATKPISTOLSND = 24, DEATHSCREAM3SND = 25, ATKMACHINEGUNSND = 26, HITENEMYSND = 27,
    SHOOTDOORSND = 28, DEATHSCREAM1SND = 29, GETMACHINESND = 30, GETAMMOSND = 31,
    SHOOTSND = 32, HEALTH1SND = 33, HEALTH2SND = 34, BONUS1SND = 35,
    BONUS2SND = 36, BONUS3SND = 37, GETGATLINGSND = 38, ESCPRESSEDSND = 39,
    LEVELDONESND = 40, DOGBARKSND = 41, ENDBONUS1SND = 42, ENDBONUS2SND = 43,
    BONUS1UPSND = 44, BONUS4SND = 45, PUSHWALLSND = 46, NOBONUSSND = 47,
    PERCENT100SND = 48, BOSSACTIVESND = 49, MUTTISND = 50, SCHUTZADSND = 51,
    AHHHGSND = 52, DIESND = 53, EVASND = 54, GUTENTAGSND = 55,
    LEBENSND = 56, SCHEISTSND = 57, NAZIFIRESND = 58, BOSSFIRESND = 59,
    SSFIRESND = 60, SLURPIESND = 61, TOT_HUNDSND = 62, MEINGOTTSND = 63,
    SCHABBSHASND = 64, HITLERHASND = 65, SPIONSND = 66, NEINSOVASSND = 67,
    DOGATTACKSND = 68, FLAMETHROWERSND = 69, MECHSTEPSND = 70, GOOBSSND = 71,
    YEAHSND = 72, DEATHSCREAM4SND = 73, DEATHSCREAM5SND = 74, DEATHSCREAM6SND = 75,
    DEATHSCREAM7SND = 76, DEATHSCREAM8SND = 77, DEATHSCREAM9SND = 78, DONNERSND = 79,
    EINESND = 80, ERLAUBENSND = 81, KEINSND = 82, MEINSND = 83,
    ROSESND = 84, MISSILEFIRESND = 85, MISSILEHITSND = 86,
    /* Spear of Destiny's own */
    GHOSTSIGHTSND = 87, GHOSTFADESND = 88, GETAMMOBOXSND = 89, ANGELSIGHTSND = 90,
    ANGELFIRESND = 91, TRANSSIGHTSND = 92, TRANSDEATHSND = 93, WILHELMSIGHTSND = 94,
    WILHELMDEATHSND = 95, UBERDEATHSND = 96, KNIGHTSIGHTSND = 97, KNIGHTDEATHSND = 98,
    ANGELDEATHSND = 99, KNIGHTMISSILESND = 100, GETSPEARSND = 101, ANGELTIREDSND = 102,
    NUM_GAME_SOUNDS
};

/* Songs by what they are for; the blob has each version's (-1 where it
 * has none - Spear of Destiny has no "Read This!"). */
enum { MUSROLE_CORNER, MUSROLE_INTRO, MUSROLE_MENU, MUSROLE_ROSTER,
       MUSROLE_ENDLEVEL, MUSROLE_VICTORY, MUSROLE_IDGUYS, MUSROLE_THEEND,
       MUSROLES };
extern const uint32_t *wolf_music_roles;
#define CORNER_MUS    ((int)wolf_music_roles[MUSROLE_CORNER])   /* "Read This!" */
#define NAZI_NOR_MUS  ((int)wolf_music_roles[MUSROLE_INTRO])    /* the title    */
#define WONDERIN_MUS  ((int)wolf_music_roles[MUSROLE_MENU])     /* the menus    */
#define ROSTER_MUS    ((int)wolf_music_roles[MUSROLE_ROSTER])   /* high scores  */
#define ENDLEVEL_MUS  ((int)wolf_music_roles[MUSROLE_ENDLEVEL]) /* stats screen */
#define URAHERO_MUS   ((int)wolf_music_roles[MUSROLE_VICTORY])  /* victory      */
#define XTHEEND_MUS   ((int)wolf_music_roles[MUSROLE_THEEND])   /* Spear's end  */

/* The Sound menu's three settings: SD_SetSoundMode() (AdLib effects on or
 * off), SD_SetDigiDevice() (digitized effects, or their AdLib versions) and
 * SD_SetMusicMode(). */
extern bool sd_effects_on, sd_digi_on, sd_music_on;
void sd_set_effects(bool on);
void sd_set_digi(bool on);
void sd_set_music(bool on);
void sd_pause_music(bool paused);           /* SD_MusicOff() / SD_MusicOn() */

void sd_stop_sound(void);                   /* SD_StopSound()             */
void sd_start_music(int song);              /* StartCPMusic()             */

void sd_init(void);
bool sd_play_sound(int sound);              /* true if it went digitized  */
void sd_play_sound_loc_tile(int sound, int tilex, int tiley);
void sd_play_sound_loc_global(int sound, fixed gx, fixed gy);
int  sd_sound_playing(void);                /* the AdLib sound, or 0      */
void sd_update(int tics);                   /* timers and stereo position */
void sd_level_start(int level);             /* stop sounds, start the song */

/* What sound.c needs from the platform: three voices, as on a Sound
 * Blaster Pro - FM music, one FM effect at a time, one digitized sound at a
 * time. Volumes are the SB Pro mixer's 0..15 per side. The ROM implements
 * these in n64audio.c; tools/sim.c records them. */
void snd_backend_adlib(int sound);          /* -1 stops it */
void snd_backend_digi(int digi, int lvol, int rvol);     /* -1 stops it */
void snd_backend_digi_volume(int lvol, int rvol);
void snd_backend_music(int song);           /* -1 stops it */
void snd_backend_music_pause(bool paused);  /* hold it where it is */

/* The audio section of the blob, parsed by assets.c. Indexed by the data's
 * own sound numbers, which wolf_sound_map gives for each of the game's. */
extern int             wolf_num_sounds, wolf_num_digi, wolf_num_game_sounds;
extern const uint32_t *wolf_sound_map;      /* the game's sound -> the data's */
extern const uint32_t *wolf_sound_info;     /* (priority << 16) | AdLib tics */
extern const uint32_t *wolf_sound_digi;     /* digitized index or ~0         */
extern const uint32_t *wolf_digi_tics;
extern const uint32_t *wolf_level_song;     /* NUM levels                    */
extern const uint32_t *wolf_pantable;       /* 15x30 left, then 15x30 right  */

/* ------------------------------------------------------------------ */
/* The end-of-floor screen (intermission.c) - LevelCompleted()         */
/* ------------------------------------------------------------------ */

/* Starts it for the floor just finished, drawing into viewbuf; update
 * advances the tally, and returns true once the player has dismissed the
 * finished screen. `ack` is a button press this frame. */
void intermission_start(int level);
bool intermission_update(int tics, bool ack);
bool intermission_tallying(void);           /* false once it waits on ack */

/* The rest of WL_INTER.C: forgetting the floor ratios for a new game, the
 * "Get Psyched!" screen PreloadGraphics() shows, and Victory(). All draw the
 * DOS game's 160-row screens into the middle of the view. */
void intermission_new_game(void);
void psyched_start(void);
void victory_start(void);

/* Write(): the LEVELEND lump's big letters, at x, y in eight-pixel cells of
 * the DOS game's 160-row screen, as these screens are placed in the view. */
void write_text(int x, int y, const char *string);

/* LevelRatios[]: each finished floor's percentages and time in seconds, by
 * mapon, for Victory()'s averages - over floors 1-8 of the episode, or
 * Spear of Destiny's 20. */
#define LEVEL_RATIOS 20
typedef struct { int kill, secret, treasure, time; } levelratio_t;
extern levelratio_t level_ratios[LEVEL_RATIOS];

/* ------------------------------------------------------------------ */
/* Text pages (text.c) - EndText() and ShowArticle() from WL_TEXT.C    */
/* ------------------------------------------------------------------ */

/* The page fills all 200 rows of viewbuf. update takes this frame's
 * presses - turn a page -1/+1, accept (next page, or done on the last),
 * leave - and returns true when the reader is done. */
void article_start(const char *article);
bool article_update(int turn, bool accept, bool leave);

/* The proportional fonts, VW_DrawPropString(): only the ink is drawn, in
 * `color`. font_draw returns the x after the string. */
int  font_height(int font);
int  font_measure(int font, const char *s);
int  font_draw(int font, int x, int y, const char *s, int color);
int  font_draw_ink(int font, int x, int y, const char *s, uint16_t ink);

/* ------------------------------------------------------------------ */
/* Saved games (save.c) - SaveTheGame(), LoadTheGame() and the slots   */
/* ------------------------------------------------------------------ */

#define SAVE_SLOTS         10
#define SAVE_NAME_LEN      32
#define SAVE_MEDIUM_BYTES  32768                /* 256 Kbit cartridge SRAM */

void        save_init(void);                    /* SetupSaveGames()       */
bool        save_slot_used(int slot);
const char *save_slot_name(int slot);
/* false if the ten saves would no longer fit, or the write failed */
bool        save_game(int slot, const char *name);
/* rebuilds the floor as it was saved; false if the slot is empty or bad */
bool        load_game(int slot);

/* The high score table, Scores[] in ID_US_1.C: seven entries, best first,
 * kept on the cartridge after the saves. `completed` is the floor reached,
 * in its episode. */
#define MAX_SCORES     7
#define HIGH_NAME_LEN  57                       /* MaxHighName */
typedef struct {
    char    name[HIGH_NAME_LEN + 1];
    int32_t score;
    int     completed, episode;
} highscore_t;
extern highscore_t high_scores[MAX_SCORES];
/* CheckHighScore()'s insertion: the new row, or -1 if the score missed */
int  high_score_insert(int32_t score, int completed, int episode);
bool save_high_scores(void);                    /* WriteConfig()          */
bool save_config(void);                         /* the same, for settings */

/* level_load() brackets its work with these, so a save can be written as
 * the differences from the floor as it loaded. */
void   save_level_loading(void);
void   save_level_loaded(void);
size_t save_state(uint8_t *buf, size_t size);   /* 0 if it does not fit */
size_t save_space(void);        /* what the ten slots have between them */

/* What save.c needs from the platform: SAVE_MEDIUM_BYTES of memory that
 * survives power-off, read and written whole. The ROM uses cartridge SRAM
 * (n64save.c); tools/sim keeps it in a file. */
bool save_medium_read(uint8_t *buf);
bool save_medium_write(const uint8_t *buf);

/* ------------------------------------------------------------------ */
/* The menus (menu.c) - US_ControlPanel() and WL_MENU.C                */
/* ------------------------------------------------------------------ */

enum { MENU_BUSY, MENU_RESUME, MENU_NEW_GAME, MENU_END_GAME, MENU_BACK_TO_DEMO,
       MENU_LOAD_GAME,
       MENU_WARP,                           /* Extras' Warp to Floor: Tab-W */
       MENU_END_FLOOR };                    /* Extras' End Floor: Tab-E */

/* DrawHighScores(): the table on a menu background, 320x200. */
void draw_high_scores(void);

/* US_LineInput() on a pad, for a name at (x, y) in the small font, `color`
 * on `back`, starting from `text`: up/down step the last letter, A adds
 * one, B erases one (on an empty name it is Esc), Start is Enter. With
 * `footer`, the strip at the foot of the screen says so while it lasts, and
 * is cleared after. update returns LINE_BUSY until the name is done or
 * given up. */
enum { LINE_BUSY, LINE_DONE, LINE_CANCELLED };
struct buttons_s;
void        line_input_start(int x, int y, const char *text, int maxchars,
                             int maxwidth, int color, int back, bool footer,
                             int font);
int         line_input_update(int tics, const struct buttons_s *b);
const char *line_input_text(void);

/* Opens the main menu over a screen that has already faded out. update
 * returns MENU_BUSY while the menus are up; any other value means the menu
 * has closed, with the screen faded (MENU_RESUME, MENU_NEW_GAME) or not
 * (MENU_END_GAME), as US_ControlPanel() left it. */
struct buttons_s;
void menu_open(bool ingame);
int  menu_update(int tics, const struct buttons_s *b);
int  menu_difficulty(void);                 /* what New Game picked */
int  menu_episode(void);                    /* 0..5; 0 for Spear of Destiny */
int  menu_load_slot(void);                  /* what Load Game picked */
int  menu_warp_floor(void);                 /* what Warp to Floor picked */
bool menu_fullscreen(void);                 /* false for Change View  */

/* The Extras menu's settings, which the DOS game had only as debug keys
 * (or not at all). All start off and are saved with the other settings
 * (save_config()). The platform shows the profiler. */
extern bool opt_show_fps;
extern bool opt_show_profiler;
extern bool opt_god_mode;                   /* TakeDamage() skips health */
extern bool opt_no_clip;                    /* ClipMove() lets walls by  */

/* The Control menu's settings, saved the same way. Always Run swaps what the
 * Run button does; the stick's turn and strafe speed is scaled by
 * (ctl_stick_sensitivity + 5) / 10, so the default 5 is the DOS speed. */
#define CTL_SENS_DEFAULT 5
#define CTL_SENS_MAX     9
extern bool ctl_always_run;
extern int  ctl_stick_sensitivity;          /* 0..CTL_SENS_MAX */

/* Customize Controls: every action the pad can be given, and every input
 * that can take one. The first four actions are the ones CustomControls()
 * gave a joystick's buttons, in its order, then the map; then what the C
 * buttons do; then the D-pad's moves, in the order of the DOS keyboard's
 * move row. Each input has one action and each action one input, as with
 * DOS's keys; the defaults are the order below, action for input. The
 * stick's directions go with the D-pad's (pad_state_t). The menus keep A and
 * B as Enter and Esc and the D-pad as the arrows whatever is set, as DOS
 * menus kept theirs, L + R still pauses, and Start is not customizable. */
enum {
    CTL_RUN, CTL_OPEN, CTL_FIRE, CTL_STRAFE, CTL_MAP,
    CTL_STRAFE_LEFT, CTL_STRAFE_RIGHT, CTL_NEXT_WEAPON, CTL_PREV_WEAPON,
    CTL_LEFT, CTL_RIGHT, CTL_FORWARD, CTL_BACKWARD,
    CTL_ACTIONS
};
enum {
    PAD_B, PAD_A, PAD_Z, PAD_R, PAD_L,
    PAD_C_LEFT, PAD_C_RIGHT, PAD_C_UP, PAD_C_DOWN,
    PAD_D_LEFT, PAD_D_RIGHT, PAD_D_UP, PAD_D_DOWN,
    PAD_BUTTONS
};
#define CTL_PACKED_BYTES 7                  /* four bits an action */
extern int ctl_button[CTL_ACTIONS];         /* the PAD_ input of each action */
void ctl_default_buttons(void);
bool ctl_unpack_buttons(const uint8_t *p);  /* false if not one input each
                                             * (and then the defaults) */
void ctl_pack_buttons(uint8_t *p);
/* is `action`'s input down? `pad` has bit PAD_x set for each held input */
bool ctl_held(int action, unsigned pad);

/* ------------------------------------------------------------------ */
/* The game's flow (game.c) - GameLoop(), PlayLoop() and the screens   */
/* in between, one frame at a time                                     */
/* ------------------------------------------------------------------ */

typedef struct buttons_s {
    bool use, menu, pause;                  /* pressed this frame: A, Start, L+R */
    bool map;                               /* the Map button (L) released,
                                             * not in an L+R                */
    bool use_held;                          /* held: the Open button (A)    */
    bool back, left, right;                 /* pressed: B, left, right      */
    bool back_held;                         /* held: B, for typing a name   */
    bool up, down;                          /* held: up, down (D-pad/stick) */
    bool left_held, right_held;             /* held: left, right (D-pad/stick) */
    bool fire;                              /* held: the Fire button (Z)    */
    unsigned pad_pressed;                   /* pressed: bits PAD_B..PAD_D_DOWN */
    int  change;                            /* next weapon +1, previous -1  */
    int  select;                            /* 1..4: a weapon's number key  */
} buttons_t;

/* The pad as the platform reads it (controls.c turns it into the above).
 * The stick's four directions are the D-pad's four inputs: whatever the
 * D-pad's up is set to do, pushing the stick up does too - in proportion,
 * for a move, and as a press past halfway, for anything else. */
typedef struct {
    unsigned held, pressed, released;       /* bits PAD_x: buttons and D-pad */
    bool     start;                         /* pressed: Start               */
    int      stick_x, stick_y;              /* up and right are positive    */
} pad_state_t;

void ctl_read_pad(const pad_state_t *p, int tics, controls_t *c, buttons_t *b);

void game_init(void);                       /* the sign-on screen, at power-on */

/* What IntroScreen() lights on the sign-on screen: the memory bars (0 for a
 * kind that is not there) and the boxes for the hardware found. It asks the
 * platform, every frame the screen is up, so a controller plugged in late
 * still lights its box. */
typedef struct {
    int  main_kb, ems_kb, xms_kb;
    bool mouse, joystick, adlib, sound_blaster, sound_source;
} machine_t;
void platform_machine(machine_t *m);
void game_frame(const controls_t *c, const buttons_t *b, int tics, int fps);

/* How to show the frame: how far it is faded (0 none .. 255 all the way)
 * toward which color (RGBA5551 - black, or the menus' dark red), and
 * whether viewbuf is a full 320x200 page with no status bar under it. */
int      game_fade(void);
uint16_t game_fade_color(void);
bool     game_fullscreen(void);
int      game_phase(void);                  /* for tools/sim */
extern void (*demo_frame_hook)(void);       /* tools/sim: after each demo frame */
void     game_start_floor(int floor, int difficulty);   /* tools/sim */
enum { DEMO_RAN_OUT, DEMO_DIED, DEMO_ELEVATOR };
extern int demo_end;                        /* how the last demo ended */
enum {
    PH_PSYCHED_IN, PH_PSYCHED, PH_PSYCHED_OUT, PH_FIZZLE_IN, PH_FADE_IN,
    PH_PLAY, PH_DEATH_TURN, PH_DEATH_FIZZLE, PH_DEATH_WAIT, PH_GAME_OVER,
    PH_ELEVATOR, PH_LEVEL_OUT, PH_INTER_IN, PH_INTER, PH_INTER_OUT,
    PH_VICTORY_OUT, PH_VICTORY_IN, PH_VICTORY, PH_VICTORY_DONE,
    PH_ENDTEXT_IN, PH_ENDTEXT, PH_ENDTEXT_OUT,
    PH_PAUSED, PH_MENU_OUT, PH_MENU, PH_RESUME_IN, PH_NEW_GAME_OUT,
    PH_PG13_IN, PH_PG13, PH_PG13_OUT,
    PH_TITLE_IN, PH_TITLE, PH_TITLE_OUT,    /* title, credits, high scores */
    PH_DEMO_IN, PH_DEMO_FIZZLE, PH_DEMO, PH_DEMO_OUT, PH_ATTRACT_OUT,
    PH_LOAD_GAME_OUT,
    PH_SCORES_IN, PH_SCORES_NAME, PH_SCORES_WAIT, PH_SCORES_OUT,
    PH_SIGNON, PH_SIGNON_OUT,
    PH_MAP,
    PH_WARP_OUT,                            /* Extras' Warp to Floor */
    PH_DEATHCAM_WAIT, PH_DEATHCAM_FIZZLE, PH_DEATHCAM_TEXT, PH_DEATHCAM_IN,
    PH_SPEAR,                               /* Spear of Destiny's Spear */
    PH_COLLAPSE, PH_ENDSPEAR_IN, PH_ENDSPEAR, PH_ENDSPEAR_OUT   /* its end */
};

/* VL_FadeOut() toward a color and VL_FadeIn() back, for game.c and the
 * menus: start one, then call fade_update() each frame until it is done. */
#define FADE_BLACK    0x0001
#define FADE_MENURED  ((21 << 11) | 1)      /* MenuFadeOut(): VGA 43,0,0 */
#define FADE_MENUBLUE ((25 << 1) | 1)       /* Spear of Destiny's: 0,0,51 */
void fade_out(uint16_t color, int steps);
void fade_in(int steps);
bool fade_update(int tics);

/* ------------------------------------------------------------------ */
/* Status bar (status.c)                                               */
/* ------------------------------------------------------------------ */

extern uint16_t statusbuf[STATUS_H][SCREEN_W];

void status_draw(void);                     /* compose it into statusbuf */
void status_invalidate(void);               /* statusbuf was used for else */
void status_fill(int color);                /* ...such as Change View's text */
int  font_draw_status(int font, int x, int y, const char *s, int color);
int  status_face_pic(void);                 /* DrawFace()'s pic choice   */
void update_face(int tics);                 /* UpdateFace()              */
void face_reset(void);                      /* PlayLoop()'s facecount = 0 */
void face_gatling(void);                    /* GetBonus()'s grin          */
void face_ouch(void);                       /* Spear's TakeDamage() > 30  */
void face_funny(int tics);                  /* Spear's PlayLoop(): standing */
void face_moved(void);                      /* ...and Thrust() moving     */
int  face_funny_count(void);                /* funnyticount, for a save   */
void set_face_funny_count(int count);
void face_redraw(void);                     /* a DrawFace() call          */
int  face_count(void);                      /* facecount, for a save      */
void set_face_count(int count);
void hud_draw_fps(int fps);                 /* top left of the screen    */

#endif /* WOLF_H */
