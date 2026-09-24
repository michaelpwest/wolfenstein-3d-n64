/* player.c - the player, from WL_AGENT.C.
 *
 * ControlMovement/Thrust/ClipMove/TryMove, Cmd_Use, the weapons, GetBonus,
 * TakeDamage and the palette flashes, with the angle kept in tenths of a
 * degree instead of whole ones, which the analog stick needs.
 */
#include <math.h>
#include <stdlib.h>
#include "wolf.h"

#define MOVESCALE      150L
#define BACKMOVESCALE  100L
#define ANGLESCALE     2        /* 20 in WL_AGENT.C, over ten times the
                                 * angular resolution here */

player_t player;
gametype gamestate;
bool     exit_triggered;
int      player_area;
int32_t  thrustspeed;
bool     madenoise;
bool     player_died;
objtype *killerobj;
int      last_attacker_class = inertobj;
bool     spear_taken;
fixed    spear_x, spear_y;
int      spear_angle;

static bool attacking;                      /* in s_attack, not s_player */

/* ------------------------------------------------------------------ */
/* TakeDamage and the palette flashes - WL_AGENT.C, WL_PLAY.C          */
/* ------------------------------------------------------------------ */

#define WHITETICS  6

static int damagecount, bonuscount;

void take_damage(int points, objtype *attacker)
{
    /* LastAttacker, for DrawFace()'s choice of dead face. The DOS game
     * keeps the pointer and reads its class later; the class is taken now,
     * as a syringe is gone from the actor list once it has hit. */
    last_attacker_class = attacker ? attacker->obclass : inertobj;

    if (gamestate.victoryflag)
        return;
    if (gamestate.difficulty == gd_baby)
        points >>= 2;

    /* godmode, the Tab-G debug key: the hit still flashes, but costs no
     * health. Not in a demo, which must play as it was recorded. */
    if (!opt_god_mode || dos_exact)
        gamestate.health -= points;
    if (gamestate.health <= 0) {
        gamestate.health = 0;
        player_died = true;
        killerobj = attacker;
    }

    start_damage_flash(points);
    face_redraw();                          /* DrawHealth(); DrawFace(); */

    /* Spear of Destiny: "make BJ's eyes bug if major damage" - which also
     * puts off his next glance, and the random numbers it draws */
    if (wolf_version == WV_SPEAR && points > 30 && gamestate.health != 0
        && (!opt_god_mode || dos_exact))
        face_ouch();
}

void start_damage_flash(int damage) { damagecount += damage; }
void start_bonus_flash(void)        { bonuscount = NUMWHITESHIFTS * WHITETICS; }

void clear_palette_shifts(void)
{
    bonuscount = damagecount = 0;
    set_palette_shift(0);
}

/* UpdatePaletteShifts(): red wins over white, and fades 10 points of
 * damage to a step. */
void update_palette_shifts(int tics)
{
    int red = 0, white = 0;

    if (bonuscount) {
        white = bonuscount / WHITETICS + 1;
        if (white > NUMWHITESHIFTS)
            white = NUMWHITESHIFTS;
        bonuscount -= tics;
        if (bonuscount < 0)
            bonuscount = 0;
    }

    if (damagecount) {
        red = damagecount / 10 + 1;
        if (red > NUMREDSHIFTS)
            red = NUMREDSHIFTS;
        damagecount -= tics;
        if (damagecount < 0)
            damagecount = 0;
    }

    if (red)
        set_palette_shift(red);
    else if (white)
        set_palette_shift(NUMREDSHIFTS + white);
    else
        set_palette_shift(0);
}

/* ------------------------------------------------------------------ */
/* NewGame() and the Give* helpers from WL_GAME.C / WL_AGENT.C         */
/* ------------------------------------------------------------------ */

/* rndtable and US_RndT() from ID_US_A.ASM. A fixed table rather than a
 * generator, so a given sequence of frames always plays out the same way. */
static const uint8_t rndtable[256] = {
      0,   8, 109, 220, 222, 241, 149, 107,  75, 248, 254, 140,  16,  66,
     74,  21, 211,  47,  80, 242, 154,  27, 205, 128, 161,  89,  77,  36,
     95, 110,  85,  48, 212, 140, 211, 249,  22,  79, 200,  50,  28, 188,
     52, 140, 202, 120,  68, 145,  62,  70, 184, 190,  91, 197, 152, 224,
    149, 104,  25, 178, 252, 182, 202, 182, 141, 197,   4,  81, 181, 242,
    145,  42,  39, 227, 156, 198, 225, 193, 219,  93, 122, 175, 249,   0,
    175, 143,  70, 239,  46, 246, 163,  53, 163, 109, 168, 135,   2, 235,
     25,  92,  20, 145, 138,  77,  69, 166,  78, 176, 173, 212, 166, 113,
     94, 161,  41,  50, 239,  49, 111, 164,  70,  60,   2,  37, 171,  75,
    136, 156,  11,  56,  42, 146, 138, 229,  73, 146,  77,  61,  98, 196,
    135, 106,  63, 197, 195,  86,  96, 203, 113, 101, 170, 247, 181, 113,
     80, 250, 108,   7, 255, 237, 129, 226,  79, 107, 112, 166, 103, 241,
     24, 223, 239, 120, 198,  58,  60,  82, 128,   3, 184,  66, 143, 224,
    145, 224,  81, 206, 163,  45,  63,  90, 168, 114,  59,  33, 159,  95,
     28, 139, 123,  98, 125, 196,  15,  70, 194, 253,  54,  14, 109, 226,
     71,  17, 161,  93, 186,  87, 244, 138,  20,  52, 123, 251,  26,  36,
     17,  46,  52, 231, 232,  76,  31, 221,  84,  37, 216, 165, 212, 106,
    197, 242,  98,  43,  39, 175, 254, 145, 190,  84, 118, 222, 187, 136,
    120, 163, 236, 249
};

static int rndindex;

int us_rndt(void)
{
    rndindex = (rndindex + 1) & 0xff;
    return rndtable[rndindex];
}

void us_init_rndt(void)
{
    rndindex = 0;
}

int us_rnd_index(void)
{
    return rndindex;
}

void us_set_rnd_index(int index)
{
    rndindex = index & 0xff;
}

void palette_shift_counts(int *damage, int *bonus)
{
    *damage = damagecount;
    *bonus  = bonuscount;
}

void set_palette_shift_counts(int damage, int bonus)
{
    damagecount = damage;
    bonuscount  = bonus;
}

void game_new(void)
{
    gamestate = (gametype){ 0 };
    gamestate.difficulty = gd_medium;
    gamestate.weapon = gamestate.bestweapon = gamestate.chosenweapon = wp_pistol;
    gamestate.health = 100;
    gamestate.ammo   = STARTAMMO;
    gamestate.lives  = 3;
    gamestate.nextextra = EXTRAPOINTS;
}

static void give_extra_man(void)
{
    if (gamestate.lives < 9)
        gamestate.lives++;
    sd_play_sound(BONUS1UPSND);
}

void give_points(int32_t points)
{
    gamestate.score += points;
    while (gamestate.score >= gamestate.nextextra) {
        gamestate.nextextra += EXTRAPOINTS;
        give_extra_man();
    }
}

void give_ammo(int ammo)
{
    /* Out of ammo means the knife was forced out; ammo brings back the
     * weapon you had chosen, unless you are mid-swing. */
    if (!gamestate.ammo && !gamestate.attackframe)
        gamestate.weapon = gamestate.chosenweapon;
    gamestate.ammo += ammo;
    if (gamestate.ammo > 99)
        gamestate.ammo = 99;
}

void give_key(int key)
{
    gamestate.keys |= 1 << key;
}

void heal_self(int points)
{
    gamestate.health += points;
    if (gamestate.health > 100)
        gamestate.health = 100;
    face_redraw();                          /* DrawHealth(); DrawFace(); */
}

/* GiveWeapon(): six rounds, and the weapon if it is better */
void give_weapon(int weapon)
{
    give_ammo(6);
    if (gamestate.bestweapon < weapon)
        gamestate.bestweapon = gamestate.weapon = gamestate.chosenweapon = weapon;
}

/* CheckKeys()' secret code M-L-I, for anyone: all health, 99 rounds, both
 * keys and the gatling gun - at the cost of the score, and of 42 000 tics
 * (ten minutes) on the floor's time, against the par time. */
void cheat_mli(void)
{
    gamestate.health = 100;
    gamestate.ammo = 99;
    gamestate.keys = 3;
    gamestate.score = 0;
    gamestate.timecount += 42000;
    give_weapon(wp_chaingun);
    face_redraw();
}

/* DebugKeys()' Tab-I, "Free items!": 100 000 points (and the extra lives
 * they bring), all health, the next weapon up and 50 rounds */
void cheat_free_items(void)
{
    give_points(100000);
    heal_self(99);
    if (gamestate.bestweapon < wp_chaingun)
        give_weapon(gamestate.bestweapon + 1);
    gamestate.ammo += 50;
    if (gamestate.ammo > 99)
        gamestate.ammo = 99;
}

/* GetBonus(). The renderer calls this the moment an item comes within
 * grabbing distance, which is where DrawScaleds() does it too. Returning
 * without taking the item leaves it on the floor for later - that is how
 * a full-health player walks over food and it stays put. */
void get_bonus(statobj_t *s)
{
    switch (s->itemnumber) {
    case bo_firstaid:
        if (gamestate.health == 100)
            return;
        sd_play_sound(HEALTH2SND);
        heal_self(25);
        break;

    case bo_key1: case bo_key2: case bo_key3: case bo_key4:
        give_key(s->itemnumber - bo_key1);
        sd_play_sound(GETKEYSND);
        break;

    case bo_cross:
        sd_play_sound(BONUS1SND); give_points(100);  gamestate.treasurecount++; break;
    case bo_chalice:
        sd_play_sound(BONUS2SND); give_points(500);  gamestate.treasurecount++; break;
    case bo_bible:
        sd_play_sound(BONUS3SND); give_points(1000); gamestate.treasurecount++; break;
    case bo_crown:
        sd_play_sound(BONUS4SND); give_points(5000); gamestate.treasurecount++; break;

    case bo_clip:
        if (gamestate.ammo == 99)
            return;
        sd_play_sound(GETAMMOSND);
        give_ammo(8);
        break;
    case bo_clip2:
        if (gamestate.ammo == 99)
            return;
        sd_play_sound(GETAMMOSND);
        give_ammo(4);
        break;

    case bo_machinegun:
    case bo_chaingun: {
        int w = (s->itemnumber == bo_chaingun) ? wp_chaingun : wp_machinegun;
        sd_play_sound(w == wp_chaingun ? GETGATLINGSND : GETMACHINESND);
        give_weapon(w);
        if (w == wp_chaingun)
            face_gatling();             /* StatusDrawPic(17,4,GOTGATLINGPIC) */
        break;
    }

    case bo_fullheal:
        sd_play_sound(BONUS1UPSND);
        heal_self(99);
        give_ammo(25);
        give_extra_man();
        gamestate.treasurecount++;
        break;

    case bo_food:
        if (gamestate.health == 100)
            return;
        sd_play_sound(HEALTH1SND);
        heal_self(10);
        break;

    case bo_alpo:
        if (gamestate.health == 100)
            return;
        sd_play_sound(HEALTH1SND);
        heal_self(4);
        break;

    case bo_gibs:
        if (gamestate.health > 10)
            return;
        sd_play_sound(SLURPIESND);
        heal_self(1);
        break;

    /* Spear of Destiny's box of 25 rounds, and the Spear itself: that
     * ends the floor (ex_completed), and GameLoop() goes on to the last. */
    case bo_25clip:
        if (gamestate.ammo == 99)
            return;
        sd_play_sound(GETAMMOBOXSND);
        give_ammo(25);
        break;

    case bo_spear:
        spear_taken = true;
        spear_x = player.x;
        spear_y = player.y;
        spear_angle = player.angle;
        break;

    default:
        return;                         /* dressing or furniture */
    }

    start_bonus_flash();
    s->shapenum = -1;                   /* remove from the list */
}

void player_spawn(int tilex, int tiley, int dir)
{
    int angle;

    player.x     = ((fixed)tilex << TILESHIFT) + TILEGLOBAL / 2;
    player.y     = ((fixed)tiley << TILESHIFT) + TILEGLOBAL / 2;
    player.tilex = tilex;
    player.tiley = tiley;

    /* SpawnPlayer(): (1 - dir) * 90 degrees, with dir 0..3 running
     * north, east, south, west. */
    angle = (1 - dir) * (FINEANGLES / 4);
    while (angle < 0)
        angle += FINEANGLES;
    player.angle     = angle % FINEANGLES;
    player.anglefrac = 0;

    player_area = area_at(tilex, tiley);
    exit_triggered = false;
    player_died = false;
    killerobj = NULL;
    attacking = false;
    gamestate.attackframe = gamestate.attackcount = gamestate.weaponframe = 0;
    if (!gamestate.ammo)
        gamestate.weapon = wp_knife;
    else
        gamestate.weapon = gamestate.chosenweapon;
}

/* ------------------------------------------------------------------ */
/* Weapons - Cmd_Fire(), T_Attack() and CheckWeaponChange()            */
/* ------------------------------------------------------------------ */

/* attackinfo[] from WL_AGENT.C: tics to hold each frame, what happens as
 * the frame is left (0 nothing, 1 shoot, 2 stab, 3 machine gun repeat,
 * 4 gatling repeat-and-shoot, -1 done), and the sprite frame shown. */
static const struct { int8_t tics, attack, frame; } attackinfo[4][4] = {
    { { 6, 0, 1 }, { 6, 2, 2 }, { 6, 0, 3 }, { 6, -1, 4 } },   /* knife   */
    { { 6, 0, 1 }, { 6, 1, 2 }, { 6, 0, 3 }, { 6, -1, 4 } },   /* pistol  */
    { { 6, 0, 1 }, { 6, 1, 2 }, { 6, 3, 3 }, { 6, -1, 4 } },   /* machine */
    { { 6, 0, 1 }, { 6, 1, 2 }, { 6, 4, 3 }, { 6, -1, 4 } },   /* gatling */
};

bool player_attacking(void)
{
    return attacking;
}

static void cmd_fire(void)
{
    attacking = true;
    gamestate.attackframe = 0;
    gamestate.attackcount = attackinfo[gamestate.weapon][0].tics;
    gamestate.weaponframe = attackinfo[gamestate.weapon][0].frame;
}

/* CheckWeaponChange() reads number keys 1-4 for a direct pick; an N64 pad
 * has none to spare, so this steps through the weapons you own instead.
 * The rule that matters is the same: no ammo, no choice but the knife. */
static void check_weapon_change(int change)
{
    int n = gamestate.bestweapon + 1;

    if (!gamestate.ammo)
        return;
    gamestate.weapon = gamestate.chosenweapon =
        ((gamestate.weapon + change) % n + n) % n;
}

void player_weapon(bool fire, bool fireheld, int change, int tics)
{
    if (!attacking) {
        /* T_Player(): weapon change, then a fresh press starts an attack.
         * Holding the button from an earlier attack does not. */
        if (change)
            check_weapon_change(change);
        if (fire && !fireheld)
            cmd_fire();
        return;
    }

    /* T_Attack(). A press that began during the attack does not count for
     * the repeating guns - the button has to have been held throughout. */
    if (fire && !fireheld)
        fire = false;

    gamestate.attackcount -= tics;
    while (gamestate.attackcount <= 0) {
        const int w = gamestate.weapon;
        const int attack = attackinfo[w][gamestate.attackframe].attack;
        const int holdtics = attackinfo[w][gamestate.attackframe].tics;

        switch (attack) {
        case -1:
            attacking = false;
            if (!gamestate.ammo)
                gamestate.weapon = wp_knife;
            else if (gamestate.weapon != gamestate.chosenweapon)
                gamestate.weapon = gamestate.chosenweapon;
            gamestate.attackframe = gamestate.weaponframe = 0;
            return;

        case 4:                                 /* gatling: loop and shoot */
            if (!gamestate.ammo)
                break;
            if (fire)
                gamestate.attackframe -= 2;
            /* fall through */
        case 1:                                 /* GunAttack() */
            if (!gamestate.ammo) {              /* only the gatling gets here */
                gamestate.attackframe++;
                break;
            }
            sd_play_sound(w == wp_pistol     ? ATKPISTOLSND
                        : w == wp_machinegun ? ATKMACHINEGUNSND
                                             : ATKGATLINGSND);
            gun_attack();
            gamestate.ammo--;
            break;

        case 2:                                 /* KnifeAttack() */
            sd_play_sound(ATKKNIFESND);
            knife_attack();
            break;

        case 3:                                 /* machine gun: loop */
            if (gamestate.ammo && fire)
                gamestate.attackframe -= 2;
            break;
        }

        gamestate.attackcount += holdtics;     /* cur->tics, before any loop-back */
        gamestate.attackframe++;
        gamestate.weaponframe = attackinfo[w][gamestate.attackframe].frame;
    }
}

/* ------------------------------------------------------------------ */

static bool try_move(fixed x, fixed y)
{
    int xl = (x - PLAYERSIZE) >> TILESHIFT;
    int yl = (y - PLAYERSIZE) >> TILESHIFT;
    int xh = (x + PLAYERSIZE) >> TILESHIFT;
    int yh = (y + PLAYERSIZE) >> TILESHIFT;
    int tx, ty;

    if (xl < 0 || yl < 0 || xh >= MAPSIZE || yh >= MAPSIZE)
        return false;

    for (ty = yl; ty <= yh; ty++)
        for (tx = xl; tx <= xh; tx++)
            if (blockmap[ty][tx])
                return false;

    /* check for actors, one tile further out */
    if (yl > 0) yl--;
    if (yh < MAPSIZE - 1) yh++;
    if (xl > 0) xl--;
    if (xh < MAPSIZE - 1) xh++;

    for (ty = yl; ty <= yh; ty++)
        for (tx = xl; tx <= xh; tx++) {
            objtype *check = actorat[ty][tx];
            if (check && (check->flags & FL_SHOOTABLE)
                && x - check->x >= -MINACTORDIST && x - check->x <= MINACTORDIST
                && y - check->y >= -MINACTORDIST && y - check->y <= MINACTORDIST)
                return false;
        }

    return true;
}

/* Slide along whichever axis is still free, so a wall taken at an angle
 * does not bring you to a dead stop. */
static void clip_move(fixed xmove, fixed ymove)
{
    fixed basex = player.x, basey = player.y;

    if (try_move(basex + xmove, basey + ymove)) {
        player.x = basex + xmove;
        player.y = basey + ymove;
        return;
    }
    /* Extras' No Clip, Spear of Destiny's Tab-N: through the wall anyway,
     * so long as it is not onto the outermost two tiles of the map (not in
     * a demo, which must play as recorded) */
    if (opt_no_clip && !dos_exact
        && basex + xmove > 2 * TILEGLOBAL && basey + ymove > 2 * TILEGLOBAL
        && basex + xmove < ((fixed)(MAPSIZE - 1) << TILESHIFT)
        && basey + ymove < ((fixed)(MAPSIZE - 1) << TILESHIFT)) {
        player.x = basex + xmove;
        player.y = basey + ymove;
        return;
    }
    /* ClipMove(): a bump, unless the FM voice is already busy. */
    if (!sd_sound_playing())
        sd_play_sound(HITWALLSND);
    if (try_move(basex + xmove, basey)) {
        player.x = basex + xmove;
        return;
    }
    if (try_move(basex, basey + ymove))
        player.y = basey + ymove;
}

static void thrust(int angle, int32_t speed)
{
    fixed xmove, ymove;

    thrustspeed += speed;
    if (speed)
        face_moved();                       /* Spear's funnyticount = 0 */

    if (speed >= MINDIST * 2)
        speed = MINDIST * 2 - 1;

    angle %= FINEANGLES;
    if (angle < 0)
        angle += FINEANGLES;

    if (dos_exact) {
        /* whole degrees during a demo: costable/sintable and FixedByFrac() */
        xmove =  fixed_by_frac(speed, dos_sintable[(angle / 10 + 90) % 360]);
        ymove = -fixed_by_frac(speed, dos_sintable[angle / 10]);
    } else {
        xmove =  fixmul(speed, fcos(angle));
        ymove = -fixmul(speed, fsin(angle));
    }

    clip_move(xmove, ymove);

    player.tilex = player.x >> TILESHIFT;
    player.tiley = player.y >> TILESHIFT;
    /* Inside a wall (No Clip) there is no room to be in - DOS read a
     * nonsense one - so the player stays in the last room walked in */
    if (plane0[player.tiley][player.tilex] >= AREATILE)
        player_area = area_at(player.tilex, player.tiley);

    if (plane1[player.tiley][player.tilex] == EXITTILE)
        exit_triggered = true;
}

void player_move(const controls_t *c, int tics)
{
    thrustspeed = 0;

    /* Turning is accumulated in fractional units so that a slow turn is
     * not rounded away frame by frame. */
    if (c->strafe) {
        if (c->strafe > 0)
            thrust(player.angle - FINEANGLES / 4, c->strafe * MOVESCALE);
        else
            thrust(player.angle + FINEANGLES / 4, -c->strafe * MOVESCALE);
    }

    if (c->turn) {
        /* During a demo, ControlMovement()'s own rounding: whole degrees,
         * twenty units to the degree, as the recording was made. */
        const int scale = dos_exact ? 20 : ANGLESCALE;
        int angleunits;
        player.anglefrac += c->turn;
        angleunits = player.anglefrac / scale;
        player.anglefrac -= angleunits * scale;
        player.angle -= dos_exact ? angleunits * 10 : angleunits;
        while (player.angle >= FINEANGLES)
            player.angle -= FINEANGLES;
        while (player.angle < 0)
            player.angle += FINEANGLES;
    }

    if (c->forward < 0)
        thrust(player.angle, -c->forward * MOVESCALE);
    else if (c->forward > 0)
        thrust(player.angle + FINEANGLES / 2, c->forward * BACKMOVESCALE);

    /* T_Shoot() compares thrustspeed against RUNSPEED to decide whether
     * the player is running. The DOS figure is per frame, which at 70 fps is
     * per tic: walking is 5250 and running 10500 either side of 6000. At the
     * N64's frame rate a frame is two or three tics, so it is taken per tic
     * here, or walking would count as running. A demo keeps the DOS figure
     * for its four-tic frames, as it was recorded. */
    if (tics > 1 && !dos_exact)
        thrustspeed /= tics;
}

/* Died(): swing round to face the attacker, DEATHROTATE degrees a tic, the
 * short way. Returns true once facing them - at once when there is none,
 * as for End Game. (CP_EndGame() never set killerobj, so the DOS Died()
 * swung toward a stale pointer: a former killer's slot, or with none,
 * whatever lay at the start of the data segment. The port does not turn.) */
bool player_face_killer(int tics)
{
    const int DEATHROTATE = 2 * 10;         /* in tenths of a degree */
    double fangle;
    int iangle, diff, change;

    if (!killerobj)
        return true;

    fangle = atan2((double)(player.y - killerobj->y),
                   (double)(killerobj->x - player.x));
    if (fangle < 0)
        fangle += 2 * 3.14159265358979323846;
    iangle = (int)(fangle / (2 * 3.14159265358979323846) * FINEANGLES) % FINEANGLES;

    diff = iangle - player.angle;
    while (diff > FINEANGLES / 2)   diff -= FINEANGLES;
    while (diff < -FINEANGLES / 2)  diff += FINEANGLES;
    if (!diff)
        return true;

    change = tics * DEATHROTATE;
    if (change > abs(diff))
        change = abs(diff);
    player.angle += diff > 0 ? change : -change;
    while (player.angle >= FINEANGLES) player.angle -= FINEANGLES;
    while (player.angle < 0)           player.angle += FINEANGLES;
    return player.angle == iangle;
}

/* VictorySpin(): turn to face south, three degrees a tic, and back north
 * to five tiles and a bit beyond the exit, so BJ runs toward the camera.
 * player.tiley is not updated while it runs, so the target stays put. The
 * DOS angle is whole degrees and never wraps here; nor does this. */
void player_victory_spin(int tics)
{
    const fixed desty = (((fixed)player.tiley - 5) << TILESHIFT) - 0x3000;

    if (player.angle > 2700) {
        player.angle -= tics * 30;
        if (player.angle < 2700)
            player.angle = 2700;
    } else if (player.angle < 2700) {
        player.angle += tics * 30;
        if (player.angle > 2700)
            player.angle = 2700;
    }

    if (player.y > desty) {
        player.y -= tics * 4096;
        if (player.y < desty)
            player.y = desty;
    }
}

/* The end of Died(): a life gone, and if any are left, back to the start of
 * the floor with the default kit and the score the floor began with.
 * Returns false when that was the last life. */
bool player_lose_life(void)
{
    gamestate.lives--;
    if (gamestate.lives < 0)
        return false;

    gamestate.health = 100;
    gamestate.weapon = gamestate.bestweapon = gamestate.chosenweapon = wp_pistol;
    gamestate.ammo   = STARTAMMO;
    gamestate.keys   = 0;
    gamestate.attackframe = gamestate.attackcount = gamestate.weaponframe = 0;
    gamestate.score  = gamestate.oldscore;
    face_redraw();
    return true;
}

/* ------------------------------------------------------------------ */

/* GameLoop(), on ex_completed / ex_secretlevel. Wolfenstein's secret floor
 * is each episode's tenth, and back from it is ElevatorBackTo[] - the floor
 * after the one its elevator is on. Spear of Destiny's two secret floors
 * are 19 and 20, from floors 4 and 12. */
int next_floor(int level, bool secret)
{
    if (wolf_version == WV_SPEAR) {
        enum { FROMSECRET1 = 3, FROMSECRET2 = 11 };
        if (secret) {
            if (level == FROMSECRET1) return 18;
            if (level == FROMSECRET2) return 19;
        }
        if (level == 18) return FROMSECRET1 + 1;
        if (level == 19) return FROMSECRET2 + 1;
        return level + 1;
    } else {
        static const int elevator_back_to[6] = { 1, 1, 7, 3, 5, 3 };
        const int episode = level_episode(level);
        if (level_mapon(level) == 9)
            return episode * 10 + elevator_back_to[episode];
        if (secret)
            return episode * 10 + 9;
        return level + 1;
    }
}

/* After the Spear: the last floor, as the player stood when they took it -
 * Thrust(0,0) - GameLoop() */
void player_place(fixed x, fixed y, int angle)
{
    player.x = x;
    player.y = y;
    player.angle = angle;
    player.anglefrac = 0;
    thrust(0, 0);
}

/* Cmd_Use() runs every frame the button is down, not just the first:
 * holding it pushes a secret wall as soon as the player faces one. Doors
 * and elevators want a fresh press (`held` is buttonheld[bt_use]); anything
 * else gets the "nothing here" grunt, again every frame it is held. */
int player_use(bool held)
{
    int checkx = player.tilex, checky = player.tiley;
    bool elevatorok;
    int tile, dir;

    /* Cmd_Use() snaps to the cardinal direction you are nearest to facing.
     * The elevator switch only works from the east or west, because that
     * is the only side its texture is on. */
    if (player.angle < FINEANGLES / 8 || player.angle > 7 * FINEANGLES / 8) {
        checkx++;
        dir = di_east;
        elevatorok = true;
    } else if (player.angle < 3 * FINEANGLES / 8) {
        checky--;
        dir = di_north;
        elevatorok = false;
    } else if (player.angle < 5 * FINEANGLES / 8) {
        checkx--;
        dir = di_west;
        elevatorok = true;
    } else {
        checky++;
        dir = di_south;
        elevatorok = false;
    }

    if (checkx < 0 || checky < 0 || checkx >= MAPSIZE || checky >= MAPSIZE)
        return USE_NOTHING;

    /* A secret wall is marked on the object plane, and Cmd_Use() checks
     * for it before anything else. */
    if (plane1[checky][checkx] == PUSHABLETILE) {
        push_wall(checkx, checky, dir);
        return USE_NOTHING;
    }

    tile = tilemap[checky][checkx];

    if (!held && tile == ELEVATORTILE && elevatorok) {
        tilemap[checky][checkx]++;      /* flip the switch texture */
        sd_play_sound(LEVELDONESND);    /* the caller waits for it to end */
        /* The floor under the player says which elevator this is: the
         * secret one stands on ALTELEVATORTILE. The original map value is
         * still in plane0 - only door and ambush tiles are rewritten. */
        return plane0[player.tiley][player.tilex] == ALTELEVATORTILE
             ? USE_SECRET_ELEVATOR : USE_ELEVATOR;
    }
    if (!held && (tile & 0xc0) == 0x80)     /* a door, not a moving wall */
        operate_door(tile & 0x7f);
    else
        sd_play_sound(DONOTHINGSND);

    return USE_NOTHING;
}

void player_select_weapon(int weapon)
{
    if (!gamestate.ammo)                    /* must use knife with no ammo */
        return;
    if (weapon <= gamestate.bestweapon)
        gamestate.weapon = gamestate.chosenweapon = weapon;
}
