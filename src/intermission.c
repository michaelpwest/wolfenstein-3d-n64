/* intermission.c - the end-of-floor screen, LevelCompleted() in WL_INTER.C.
 *
 * The DOS function is one long run of loops: count the par-time bonus up,
 * then each ratio, beeping as it goes and waiting for every beep to finish,
 * with BJ breathing in the corner. A frame loop cannot sit inside those, so
 * the same steps are laid out as a small state machine that advances one
 * step per tic, and stands still while a sound plays - SD_SoundPlaying()
 * is what paced the original, too.
 *
 * The DOS screen is the 160 rows above the status bar. The N64 view is 200
 * rows, so the same layout is drawn 20 rows down on the same background.
 */
#include <stdio.h>
#include <string.h>
#include "wolf.h"

#define YOFFSET        ((VIEW_H - 160) / 2)
#define BACKGROUND     127                  /* VWB_Bar(0,0,320,160,127)   */
#define VBLWAIT        30
#define PAR_AMOUNT     500
#define PERCENT100AMT  10000
#define RATIOXX        37

/* parTimes[] in seconds, and as the screen prints them: Wolfenstein's six
 * episodes, and Spear of Destiny's floors. */
typedef struct { int seconds; const char *str; } partime_t;

static const partime_t wolf_par_times[60] = {
    /* episode one */
    {  90, "01:30" }, { 120, "02:00" }, { 120, "02:00" }, { 210, "03:30" },
    { 180, "03:00" }, { 180, "03:00" }, { 150, "02:30" }, { 150, "02:30" },
    {   0, "??:??" },                           /* boss floor   */
    {   0, "??:??" },                           /* secret floor */
    /* episode two */
    {  90, "01:30" }, { 210, "03:30" }, { 180, "03:00" }, { 120, "02:00" },
    { 240, "04:00" }, { 360, "06:00" }, {  60, "01:00" }, { 180, "03:00" },
    {   0, "??:??" }, {   0, "??:??" },
    /* episode three */
    {  90, "01:30" }, {  90, "01:30" }, { 150, "02:30" }, { 150, "02:30" },
    { 210, "03:30" }, { 150, "02:30" }, { 120, "02:00" }, { 360, "06:00" },
    {   0, "??:??" }, {   0, "??:??" },
    /* episode four */
    { 120, "02:00" }, { 120, "02:00" }, {  90, "01:30" }, {  60, "01:00" },
    { 270, "04:30" }, { 210, "03:30" }, { 120, "02:00" }, { 270, "04:30" },
    {   0, "??:??" }, {   0, "??:??" },
    /* episode five */
    { 150, "02:30" }, {  90, "01:30" }, { 150, "02:30" }, { 150, "02:30" },
    { 240, "04:00" }, { 180, "03:00" }, { 270, "04:30" }, { 210, "03:30" },
    {   0, "??:??" }, {   0, "??:??" },
    /* episode six */
    { 390, "06:30" }, { 240, "04:00" }, { 270, "04:30" }, { 360, "06:00" },
    { 300, "05:00" }, { 330, "05:30" }, { 330, "05:30" }, { 510, "08:30" },
    {   0, "??:??" }, {   0, "??:??" },
};

static const partime_t spear_par_times[20] = {
    {  90, "01:30" }, { 210, "03:30" }, { 165, "02:45" }, { 210, "03:30" },
    {   0, "??:??" },                           /* boss 1 */
    { 270, "04:30" }, { 195, "03:15" }, { 165, "02:45" }, { 285, "04:45" },
    {   0, "??:??" },                           /* boss 2 */
    { 390, "06:30" }, { 270, "04:30" }, { 165, "02:45" }, { 270, "04:30" },
    { 360, "06:00" },
    {   0, "??:??" },                           /* boss 3 */
    { 360, "06:00" },
    {   0, "??:??" },                           /* boss 4 */
    {   0, "??:??" },                           /* secret level 1 */
    {   0, "??:??" },                           /* secret level 2 */
};

static const partime_t *par_time(int level)
{
    static const partime_t none = { 0, "??:??" };
    if (wolf_version == WV_SPEAR)
        return level < 20 ? &spear_par_times[level] : &none;
    return level < 60 ? &wolf_par_times[level] : &none;
}

enum {
    ST_TIME_COUNT,          /* the "for (i=0;i<=timeleft;i++)" loop       */
    ST_TIME_END,            /* ENDBONUS2SND                                */
    ST_RATIO_COUNT,         /* one of the three ratio loops                */
    ST_RATIO_WAIT,          /* VW_WaitVBL(VBLWAIT) before 100% or 0%       */
    ST_RATIO_END,
    ST_DONE,                /* the "done:" label                           */
    ST_ACK                  /* while (!IN_CheckAck()) BJ_Breathe();        */
};

static int     state;
static int     step;                        /* the loop's i               */
static int     which;                       /* 0 kill, 1 secret, 2 treasure */
static int     ratios[3];
static int     wait;
static int     timeleft;
static int32_t bonus;
static bool    acked;                       /* IN_CheckAck(), latched     */
static int     current;                     /* mapon                      */
static int     time_seconds;                /* min*60 + sec, as recorded  */

/* BJ_Breathe()'s statics, reset per screen. */
static int     breathe_which, breathe_max, breathe_count;

/* ------------------------------------------------------------------ */

/* LevelRatios[], kept for Victory()'s averages. */
levelratio_t level_ratios[LEVEL_RATIOS];

static void draw_pic(int x, int y, int picnum)
{
    view_pic(x, y + YOFFSET, picnum);
}

/* VWB_Bar(0,0,320,200-STATUSLINES,127), over the whole view here. */
static void clear_screen(void)
{
    clear_palette_shifts();
    view_bar(0, 0, VIEW_W, VIEW_H, BACKGROUND);
}

/* Write(): x and y in eight-pixel cells; most glyphs are two cells wide. */
void write_text(int x, int y, const char *string)
{
    int ox, nx, ny;

    ox = nx = x * 8;
    ny = y * 8;
    for (; *string; string++) {
        char ch = *string;

        if (ch == '\n') {
            nx = ox;
            ny += 16;
            continue;
        }
        switch (ch) {
        case '!':  draw_pic(nx, ny, PIC_L_EXPOINT);    nx += 8; continue;
        case '\'': draw_pic(nx, ny, PIC_L_APOSTROPHE); nx += 8; continue;
        case ':':  draw_pic(nx, ny, PIC_L_COLON);      nx += 8; continue;
        case ' ':  break;
        case '%':  draw_pic(nx, ny, PIC_L_PERCENT);    break;
        default:
            if (ch >= 'a' && ch <= 'z')
                ch -= 'a' - 'A';
            if (ch >= '0' && ch <= '9')
                draw_pic(nx, ny, PIC_L_NUM0 + (ch - '0'));
            else if (ch >= 'A' && ch <= 'Z')
                draw_pic(nx, ny, PIC_L_A + (ch - 'A'));
            break;
        }
        nx += 16;
    }
}

/* ltoa() and Write() at x = right - length * 2, the right-justified numbers
 * the tally prints. */
static void write_number(int right, int y, int32_t n)
{
    char str[12];
    snprintf(str, sizeof str, "%ld", (long)n);
    write_text(right - (int)strlen(str) * 2, y, str);
}

static void breathe(int tics)
{
    breathe_count += tics;
    if (breathe_count > breathe_max) {
        breathe_which ^= 1;
        draw_pic(0, 16, breathe_which ? PIC_L_GUY2 : PIC_L_GUY);
        breathe_count = 0;
        breathe_max = 35;
    }
}

/* ------------------------------------------------------------------ */

void intermission_start(int level)
{
    const int mapon = level_mapon(level);
    const bool tally = wolf_version == WV_SPEAR
        ? mapon != 4 && mapon != 9 && mapon != 15 && mapon < 17
        : mapon < 8;

    clear_screen();
    sd_start_music(ENDLEVEL_MUS);
    current = mapon;

    acked = false;
    breathe_which = 0;
    breathe_max = 10;
    breathe_count = 0;
    draw_pic(0, 16, PIC_L_GUY);

    /* a boss's floor, or a secret one, gets its bonus and no tally */
    if (!tally) {
        if (wolf_version != WV_SPEAR)
            write_text(14, 4, "secret floor\n completed!");
        else switch (mapon) {
            case 4:  write_text(14, 4, " trans\n grosse\ndefeated!"); break;
            case 9:  write_text(14, 4, "barnacle\nwilhelm\ndefeated!"); break;
            case 15: write_text(14, 4, "ubermutant\ndefeated!"); break;
            case 17: write_text(14, 4, " death\n knight\ndefeated!"); break;
            case 18: write_text(13, 4, "secret tunnel\n    area\n  completed!"); break;
            case 19: write_text(13, 4, "secret castle\n    area\n  completed!"); break;
        }
        write_text(10, 16, "15000 bonus!");
        give_points(15000);
        state = ST_ACK;
        return;
    }

    {
        char str[12];
        const partime_t *pt = par_time(level);
        const int par = pt->seconds;
        int sec = gamestate.timecount / 70;
        int min;

        write_text(14, 2, "floor\ncompleted");
        write_text(14, 7, "bonus     0");
        write_text(16, 10, "time");
        write_text(16, 12, " par");
        write_text(9, 14, "kill ratio    %");
        write_text(5, 16, "secret ratio    %");
        write_text(1, 18, "treasure ratio    %");

        snprintf(str, sizeof str, "%d", mapon + 1);
        write_text(26, 2, str);
        write_text(26, 12, pt->str);

        if (sec > 99 * 60)                  /* 99 minutes max */
            sec = 99 * 60;
        timeleft = 0;
        if (gamestate.timecount < par * 70)
            timeleft = par - sec;

        time_seconds = sec;
        min = sec / 60;
        sec %= 60;
        draw_pic(26 * 8, 80, PIC_L_NUM0 + min / 10);
        draw_pic(28 * 8, 80, PIC_L_NUM0 + min % 10);
        write_text(30, 10, ":");
        draw_pic(31 * 8, 80, PIC_L_NUM0 + sec / 10);
        draw_pic(33 * 8, 80, PIC_L_NUM0 + sec % 10);
    }

    ratios[0] = gamestate.killtotal
              ? gamestate.killcount * 100 / gamestate.killtotal : 0;
    ratios[1] = gamestate.secrettotal
              ? gamestate.secretcount * 100 / gamestate.secrettotal : 0;
    ratios[2] = gamestate.treasuretotal
              ? gamestate.treasurecount * 100 / gamestate.treasuretotal : 0;

    bonus = (int32_t)timeleft * PAR_AMOUNT;
    step  = 0;
    which = 0;
    state = bonus ? ST_TIME_COUNT : ST_RATIO_COUNT;
}

/* One pass of whichever DOS loop the tally is in. */
static void tally_step(void)
{
    switch (state) {
    case ST_TIME_COUNT:
        if (acked) { state = ST_DONE; break; }
        write_number(36, 7, (int32_t)step * PAR_AMOUNT);
        if (step % (PAR_AMOUNT / 10) == 0)
            sd_play_sound(ENDBONUS1SND);
        if (++step > timeleft)
            state = ST_TIME_END;
        break;

    case ST_TIME_END:
        sd_play_sound(ENDBONUS2SND);
        step  = 0;
        state = ST_RATIO_COUNT;
        break;

    case ST_RATIO_COUNT:
        if (acked) { state = ST_DONE; break; }
        write_number(RATIOXX, 14 + 2 * which, step);
        if (step % 10 == 0)
            sd_play_sound(ENDBONUS1SND);
        if (++step > ratios[which]) {
            wait  = VBLWAIT;
            state = (ratios[which] == 100 || ratios[which] == 0)
                  ? ST_RATIO_WAIT : ST_RATIO_END;
        }
        break;

    case ST_RATIO_WAIT:
        break;                              /* counted down by the caller */

    case ST_RATIO_END:
        if (ratios[which] == 100) {
            sd_stop_sound();
            bonus += PERCENT100AMT;
            write_number(RATIOXX - 1, 7, bonus);
            sd_play_sound(PERCENT100SND);
        } else if (ratios[which] == 0) {
            sd_stop_sound();
            sd_play_sound(NOBONUSSND);
        } else {
            sd_play_sound(ENDBONUS2SND);
        }
        step = 0;
        state = ++which < 3 ? ST_RATIO_COUNT : ST_DONE;
        break;

    case ST_DONE:
        write_number(RATIOXX, 14, ratios[0]);
        write_number(RATIOXX, 16, ratios[1]);
        write_number(RATIOXX, 18, ratios[2]);
        bonus = (int32_t)timeleft * PAR_AMOUNT
              + PERCENT100AMT * (ratios[0] == 100)
              + PERCENT100AMT * (ratios[1] == 100)
              + PERCENT100AMT * (ratios[2] == 100);
        give_points(bonus);
        write_number(36, 7, bonus);

        /* SAVE RATIO INFORMATION FOR ENDGAME */
        level_ratios[current].kill     = ratios[0];
        level_ratios[current].secret   = ratios[1];
        level_ratios[current].treasure = ratios[2];
        level_ratios[current].time     = time_seconds;

        acked = false;                      /* IN_StartAck() */
        state = ST_ACK;
        break;
    }
}

bool intermission_update(int tics, bool ack)
{
    breathe(tics);

    if (state == ST_ACK)
        return ack;

    acked |= ack;

    /* One step per tic, but never past a sound that is still playing. */
    while (tics > 0 && state != ST_ACK) {
        if (sd_sound_playing())
            break;
        if (state == ST_RATIO_WAIT) {
            /* VW_WaitVBL(): a plain delay, no ack check */
            int n = wait < tics ? wait : tics;
            wait -= n;
            tics -= n;
            if (wait == 0)
                state = ST_RATIO_END;
            continue;
        }
        tally_step();
        tics--;
    }
    return false;
}

bool intermission_tallying(void)
{
    return state != ST_ACK;
}

void intermission_new_game(void)
{
    memset(level_ratios, 0, sizeof level_ratios);
}

/* ------------------------------------------------------------------ */
/* PreloadGraphics() - WL_GAME.C                                       */
/* ------------------------------------------------------------------ */

/* "Get Psyched!" in a window with PreloadUpdate()'s progress bar under it.
 * Nothing needs loading from a cartridge, so the bar is drawn full, as it
 * looked for the moment before the screen faded on a fast PC. */
void psyched_start(void)
{
    const int wx = 160 - 14 * 8, wy = 80 - 3 * 8, ww = 28 * 8, wh = 48;
    const int w = ww - 10;

    clear_screen();
    draw_pic(wx, wy, PIC_GETPSYCHED);          /* LatchDrawPic(20-14,80-3*8) */
    view_bar(wx + 5, wy + wh - 3 + YOFFSET, w, 2, 0x37);
    view_bar(wx + 5, wy + wh - 3 + YOFFSET, w - 1, 1, 0x32);
}

/* ------------------------------------------------------------------ */
/* Victory() - the averages over the eight regular floors              */
/* ------------------------------------------------------------------ */

#define RATIOX  6
#define RATIOY  14
#define TIMEX   14
#define TIMEY   8

void victory_start(void)
{
    char str[12];
    int32_t sec = 0;
    int i, min, kr = 0, sr = 0, tr = 0;

    sd_start_music(URAHERO_MUS);
    clear_screen();

    write_text(18, 2, "you win!");
    write_text(TIMEX, TIMEY - 2, "total time");
    write_text(12, RATIOY - 2, "averages");
    write_text(RATIOX + 8, RATIOY,     "kill    %");
    write_text(RATIOX + 4, RATIOY + 2, "secret    %");
    write_text(RATIOX,     RATIOY + 4, "treasure    %");

    draw_pic(8, 4, PIC_L_BJWINS);

    /* an episode's eight floors; Spear of Destiny adds up all twenty and
     * divides by the fourteen with a tally */
    {
        const int floors = wolf_version == WV_SPEAR ? 20 : 8;
        const int divisor = wolf_version == WV_SPEAR ? 14 : 8;
        for (i = 0; i < floors; i++) {
            sec += level_ratios[i].time;
            kr  += level_ratios[i].kill;
            sr  += level_ratios[i].secret;
            tr  += level_ratios[i].treasure;
        }
        kr /= divisor;
        sr /= divisor;
        tr /= divisor;
    }

    min = sec / 60;
    sec %= 60;
    if (min > 99)
        min = sec = 99;

    i = TIMEX * 8 + 1;                      /* drawn at 112: x rounds down */
    draw_pic(i, TIMEY * 8, PIC_L_NUM0 + min / 10);
    i += 2 * 8;
    draw_pic(i, TIMEY * 8, PIC_L_NUM0 + min % 10);
    i += 2 * 8;
    write_text(i / 8, TIMEY, ":");
    i += 1 * 8;
    draw_pic(i, TIMEY * 8, PIC_L_NUM0 + sec / 10);
    i += 2 * 8;
    draw_pic(i, TIMEY * 8, PIC_L_NUM0 + sec % 10);

    snprintf(str, sizeof str, "%d", kr);
    write_text(RATIOX + 24 - (int)strlen(str) * 2, RATIOY, str);
    snprintf(str, sizeof str, "%d", sr);
    write_text(RATIOX + 24 - (int)strlen(str) * 2, RATIOY + 2, str);
    snprintf(str, sizeof str, "%d", tr);
    write_text(RATIOX + 24 - (int)strlen(str) * 2, RATIOY + 4, str);

    /* TOTAL TIME VERIFICATION CODE: three letters from the time, in a box
     * beside it, for anyone who finished on a hard enough skill to have it
     * checked. The shareware (UPLOAD) build and Spear of Destiny leave it
     * out. */
    if (wolf_version == WV_FULL && gamestate.difficulty >= gd_medium) {
        char code[4];
        code[0] = (char)((((min / 10) ^ (min % 10)) ^ 0xa) + 'A');
        code[1] = (char)((((sec / 10) ^ (sec % 10)) ^ 0xa) + 'A');
        code[2] = (char)((code[0] ^ code[1]) + 'A');
        code[3] = 0;
        draw_pic(30 * 8, TIMEY * 8, PIC_C_TIMECODE);
        font_draw(0, 30 * 8 - 3 + 4, TIMEY * 8 + 8 + YOFFSET, code, 0x47);  /* READHCOLOR */
    }
}
