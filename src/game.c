/* game.c - DemoLoop(), GameLoop(), PlayLoop() and everything between floors.
 *
 * The DOS game ran each of these as a blocking loop of its own: a fade was
 * thirty VBL waits, the stats screen a nest of loops waiting on sounds, the
 * death sequence a loop inside Died(). Here each is a phase that gets one
 * frame's worth of tics at a time, in the same order WL_GAME.C runs them:
 *
 *   start         the main menu fades in (menu.c); New Game fades out
 *   new floor     "Get Psyched!" fades in, waits 70 tics, fades out; the
 *                 first view fizzles in (unseen, the screen is black) and
 *                 the screen fades in
 *   playing       a pause press pauses (no pad button is mapped to it
 *                 now); Start fades out to the menu, and Back to Game
 *                 fades the play screen back in and restarts the song
 *   elevator      the level-done sound plays out, the screen fades out, the
 *                 stats screen fades in, is dismissed, fades out
 *   death         turn to the killer, dissolve to red, wait; with a life
 *                 left the floor restarts and fizzles in over the red; with
 *                 none, fade out to the high scores
 *   victory       BJ runs out of the castle, fade out, the victory screen
 *                 fades in and waits, fade out, the end text, the high
 *                 scores
 *   high scores   a score that made the table takes a name; then the title
 *
 * A fade is VL_FadeOut()/VL_FadeIn(): every palette entry stepped toward a
 * color - black, or the menus' dark red - and back. Blending the finished
 * frame toward that color by the same fraction is the same arithmetic, so
 * the fade is reported as an amount and a color for the platform to apply
 * over the whole screen (see main.c). Nothing here knows about the N64, so
 * tools/sim runs all of it.
 */
#include <string.h>
#include "wolf.h"

#define FADESTEPS   30                      /* VW_FadeOut(), VW_FadeIn()   */
#define TEXTFADE    10                      /* ShowArticle()'s first page  */
#define FULL        256                     /* fade amount, fixed point    */
#define SIGNON_WAIT 35                      /* "One moment...": half a second */

static int  phase;
static int  level;
static int  leaving = USE_NOTHING;          /* the elevator used           */
static bool secret;
static bool fire_last;                      /* buttonheld[bt_attack]       */
static bool use_last;                       /* buttonheld[bt_use]          */
static bool fire_prev;                      /* last frame's Z, for acks    */
static int  wait_tics;
static int  signon_tics;                    /* Spear's FinishSignon() wait */
static bool fullscreen;
static bool paused_from_map;                /* pause returns to the map    */

/* the attract loop */
static int  title_screen;                   /* 0 title, 1 credits, 2 scores */
static int  lastdemo;
static const uint8_t *demoptr, *demoend;
static int  demo_tics;                      /* real tics not yet played    */
static bool demo_aborted;
static int  frame_fps;                      /* for the counter during demos */

void (*demo_frame_hook)(void);
int  demo_end;

/* ------------------------------------------------------------------ */
/* Fades                                                               */
/* ------------------------------------------------------------------ */

static int fade_amount = FULL;              /* 0 clear .. FULL all color   */
static int fade_rgb[3];                     /* 5-bit color faded toward    */
static int from_amount, to_amount, from_rgb[3], to_rgb[3];
static int fade_steps = 1, fade_step;

static void fade_start(int amount, const int rgb[3], int steps)
{
    from_amount = fade_amount;
    to_amount   = amount;
    for (int i = 0; i < 3; i++) {
        /* from a clear screen, the color it fades toward is the new one */
        from_rgb[i] = fade_amount ? fade_rgb[i] : rgb[i];
        to_rgb[i]   = rgb[i];
    }
    fade_steps = steps;
    fade_step  = 0;
}

void fade_out(uint16_t color, int steps)
{
    const int rgb[3] = { (color >> 11) & 31, (color >> 6) & 31, (color >> 1) & 31 };
    fade_start(FULL, rgb, steps);
}

void fade_in(int steps)
{
    fade_start(0, fade_rgb, steps);
}

bool fade_update(int tics)
{
    fade_step += tics;
    if (fade_step >= fade_steps)
        fade_step = fade_steps;
    fade_amount = from_amount + (to_amount - from_amount) * fade_step / fade_steps;
    for (int i = 0; i < 3; i++)
        fade_rgb[i] = from_rgb[i] + (to_rgb[i] - from_rgb[i]) * fade_step / fade_steps;
    return fade_step == fade_steps;
}

int game_fade(void)
{
    return fade_amount * 255 / FULL;
}

uint16_t game_fade_color(void)
{
    return (uint16_t)((fade_rgb[0] << 11) | (fade_rgb[1] << 6) | (fade_rgb[2] << 1) | 1);
}

bool game_fullscreen(void)
{
    /* Change View, alone of the menus, shows the play screen's layout */
    return fullscreen && !(phase == PH_MENU && !menu_fullscreen());
}

int game_phase(void)
{
    return phase;
}

/* ------------------------------------------------------------------ */

static void new_game(int difficulty, int episode)
{
    game_new();
    gamestate.difficulty = difficulty;      /* NewGame(which, episode) */
    intermission_new_game();
    face_redraw();                          /* DrawPlayScreen() */
    level = wolf_version == WV_SPEAR ? 0 : episode * 10;
}

/* The top of GameLoop()'s floor loop: SetupGameLevel(), StartMusic(), and
 * PreloadGraphics() unless the player just died. */
static void enter_floor(bool psyched)
{
    fire_last = true;                       /* no shot from a held trigger */
    /* Spear of Destiny's last floor: "give them the key allways" */
    if (wolf_version == WV_SPEAR && level == 20)
        gamestate.keys |= 1;
    if (psyched) {
        psyched_start();
        fade_in(FADESTEPS);
        phase = PH_PSYCHED_IN;
    } else {
        fizzle_in_start();
        phase = PH_FIZZLE_IN;
    }
}

static void start_floor(bool psyched)
{
    level_load(level);
    enter_floor(psyched);
}

/* For tools/sim: a new game on any floor, fizzling in as after a death */
void game_start_floor(int floor, int difficulty)
{
    new_game(difficulty, level_episode(floor));
    fullscreen = false;
    level = floor;
    start_floor(false);
}

/* US_ControlPanel(), over a screen that has already faded out. */
static void open_menu(bool ingame)
{
    /* MenuFadeIn() brings back the game palette, whatever flash was on;
     * the flash picks up again once play resumes */
    set_palette_shift(0);
    fullscreen = true;
    menu_open(ingame);
    phase = PH_MENU;
}

/* ------------------------------------------------------------------ */
/* DemoLoop() - the title, credits, high scores and demos, round and   */
/* round until a button is pressed                                     */
/* ------------------------------------------------------------------ */

/* CA_CacheScreen() the title or credits, or DrawHighScores(), then
 * VW_FadeIn() and IN_UserInput() for 15 or 10 seconds. */
static void show_title(int which)
{
    title_screen = which;
    fullscreen = true;
    switch (which) {
    case 0:
        if (wolf_version == WV_SPEAR) {
            /* in two halves, with the title's own palette */
            view_pic(0, 0, PIC_TITLE1);
            view_pic(0, 80, PIC_TITLE2);
        } else
            view_pic(0, 0, PIC_TITLE);
        break;
    case 1:  view_pic(0, 0, PIC_CREDITS); break;
    default: draw_high_scores();          break;
    }
    wait_tics = which == 0 ? 15 * 70 : 10 * 70;
    fade_in(FADESTEPS);
    phase = PH_TITLE_IN;
}

/* PlayDemo(): NewGame() on "I am Death incarnate!", the recorded floor,
 * DrawPlayScreen() and VW_FadeIn() over an empty view. */
static void start_demo(void)
{
    const uint8_t *d = wolf_demos[lastdemo++ % 4];
    const int length = d[1] | (d[2] << 8);

    fullscreen = false;
    game_new();
    gamestate.difficulty = gd_hard;
    intermission_new_game();
    face_redraw();
    level = d[0];
    demoptr = d + 4;
    demoend = d + length;
    demo_aborted = false;

    dos_exact = true;                       /* before the random table resets */
    level_load(level);
    view_bar(0, 0, VIEW_W, VIEW_H, 0);
    fade_in(FADESTEPS);
    phase = PH_DEMO_IN;
}

static void play(const controls_t *c, const buttons_t *b, int tics, int fps,
                 bool render);

/* CheckHighScore(), over a screen that has faded out: the score goes into
 * the table if it made it, the table fades in with the ROSTER song, and
 * then either the new row takes a name or the table waits a while. */
static int score_row;

static void check_high_score(void)
{
    score_row = high_score_insert(gamestate.score, level_mapon(current_level) + 1,
                                  level_episode(current_level));
    sd_start_music(ROSTER_MUS);
    fullscreen = true;
    draw_high_scores();
    fade_in(FADESTEPS);
    phase = PH_SCORES_IN;
}

/* Spear of Destiny's ending, Victory() and EndSpear(): BJ's collapse, the
 * averages, then the end screens in the order they are shown, each with its
 * own palette. The second has the captions. */
#define ENDSCREENS 9
static const int endscreen_pic[ENDSCREENS] = {
    PIC_ENDSCREEN11, PIC_ENDSCREEN3, PIC_ENDSCREEN3 + 1, PIC_ENDSCREEN3 + 2,
    PIC_ENDSCREEN3 + 3, PIC_ENDSCREEN3 + 4, PIC_ENDSCREEN3 + 5,
    PIC_ENDSCREEN3 + 6, PIC_ENDSCREEN12
};
#define STR_ENDGAME1 "We owe you a great debt, Mr. Blazkowicz."
#define STR_ENDGAME2 "You have served your country well."
#define STR_ENDGAME3 "With the spear gone, the Allies will finally"
#define STR_ENDGAME4 "by able to destroy Hitler..."

static int  collapse_step, endspear;
static bool endspear_second;

/* EndScreen(): the picture, faded in with its palette */
static void start_endscreen(void)
{
    fullscreen = true;
    view_bar(0, 0, 320, 200, 0);
    view_pic(0, 0, endscreen_pic[endspear]);
    endspear_second = false;
    fade_in(FADESTEPS);
    phase = PH_ENDSPEAR_IN;
}

/* US_CPrint() of two lines at the foot of the screen, in color 0xd0 - of
 * that screen's own palette */
static void endspear_caption(const char *line1, const char *line2)
{
    const uint16_t *pal = assets_pic_palette(PIC_ENDSCREEN3);
    const uint16_t ink = pal ? pal[0xd0] : wolf_palette[0xd0];

    font_draw_ink(0, (320 - font_measure(0, line1)) / 2, 180, line1, ink);
    font_draw_ink(0, (320 - font_measure(0, line2)) / 2, 180 + font_height(0),
                  line2, ink);
}

/* One four-tic frame of a recording: PollControls() reading the demo
 * buffer, then PlayLoop(). Returns false when the recording is used up. */
static bool demo_frame(bool render)
{
    controls_t c = { 0 };
    buttons_t  b = { 0 };
    const int bits = demoptr[0];
    const int controlx = (int8_t)demoptr[1] * 4;
    const int controly = (int8_t)demoptr[2] * 4;

    demoptr += 3;

    /* buttons: attack, strafe, run, use, then the four weapon keys */
    b.fire     = bits & 1;
    b.use_held = (bits & 8) != 0;
    for (int w = 0; w < 4; w++)
        if (bits & (16 << w) && !b.select)
            b.select = w + 1;

    if (bits & 2)
        c.strafe = controlx;
    else
        c.turn = controlx;
    c.forward = controly;

    play(&c, &b, 4, frame_fps, render);
    if (demo_frame_hook)
        demo_frame_hook();
    return demoptr < demoend;
}

/* The end of PlayDemo(): StopMusic(), VW_FadeOut() */
static void end_demo(void)
{
    dos_exact = false;
    sd_start_music(-1);
    fade_out(FADE_BLACK, FADESTEPS);
    phase = PH_DEMO_OUT;
}

/* SignonScreen(), IntroScreen() and FinishSignon() from WL_MAIN.C and
 * WL_MENU.C: the picture, a bar per 32 KB of main memory or 100 KB of EMS
 * and XMS, a lit box for each piece of hardware found, and a line of text
 * over the picture's own "One moment..." - on a bar in the color of the
 * screen's first pixel, 300 wide, which leaves the version number showing.
 * With no text it is the picture alone, "One moment..." and all, as it was
 * before IntroScreen() had looked at the machine; with empty text, the
 * lights but no line, as Spear of Destiny's FinishSignon() leaves it. Its
 * picture
 * has its slots in the same places, so these numbers serve both games, as
 * they did in IntroScreen(); only the bars' colors differ. */
static void draw_signon(const char *text, int color)
{
    static const int main_kb[10] = { 32, 64, 96, 128, 160, 192, 224, 256, 288, 320 };
    static const int memx[3] = { 49, 89, 129 };
    /* MAINCOLOR, EMSCOLOR and XMSCOLOR: green, or Spear of Destiny's */
    const int barcolor = wolf_version == WV_SPEAR ? 0x4f : 0x6c;
    int w, h;
    const uint8_t *pic = assets_pic(PIC_SIGNON, &w, &h);
    machine_t m;

    view_pic(0, 0, PIC_SIGNON);
    if (!text)
        return;
    memset(&m, 0, sizeof m);
    platform_machine(&m);
    for (int i = 0; i < 10; i++) {
        if (m.main_kb >= main_kb[i])
            view_bar(memx[0], 163 - 8 * i, 6, 5, barcolor - i);
        if (m.ems_kb && m.ems_kb >= 100 * (i + 1))
            view_bar(memx[1], 163 - 8 * i, 6, 5, barcolor - i);
        if (m.xms_kb && m.xms_kb >= 100 * (i + 1))
            view_bar(memx[2], 163 - 8 * i, 6, 5, barcolor - i);
    }
    /* IntroScreen() lit AdLib only without a Sound Blaster, whose FM chip
     * stood in for one. Here both are real parts of the sound - the AdLib
     * chip is emulated (adlib.c) beside the digitized voice - so each box is
     * lit for what the platform reports. */
    const bool found[5] = { m.mouse, m.joystick, m.adlib,
                            m.sound_blaster, m.sound_source };
    for (int i = 0; i < 5; i++)
        if (found[i])
            view_bar(164, 82 + 23 * i, 12, 2, 14);             /* FILLCOLOR */

    if (!*text)                             /* Spear of Destiny's: no line */
        return;
    view_bar(0, 189, 300, 11, pic ? pic[0] : 0);
    font_draw(0, (320 - font_measure(0, text)) / 2, 190, text, color);
}

static void start_pg13(void);

void game_init(void)
{
    const int black[3] = { 0, 0, 0 };
    int w = 0, h = 0;

    game_new();
    fullscreen = true;

    /* The sign-on picture is not in the game data but in WOLF3D.EXE, and
     * tools/extract_assets.py leaves it empty if it is not found there;
     * then the game starts where DemoLoop() does - StartCPMusic(INTROSONG),
     * PG13() - from black. */
    if (!assets_pic(PIC_SIGNON, &w, &h) || w != 320 || h != 200) {
        fade_start(FULL, black, 1);
        fade_update(1);
        sd_start_music(NAZI_NOR_MUS);
        start_pg13();
        return;
    }

    /* The sign-on screen is simply there, no fade. On a PC the picture's "One
     * moment..." showed while InitGame() ran, until FinishSignon() put "Press
     * a key" over it and waited for IN_Ack(). There is nothing to load here,
     * so it is shown for half a second, the memory bars and hardware lights
     * coming on with the prompt after it, and presses are ignored until then,
     * as IN_Ack() only began listening once it was called.
     *
     * Spear of Destiny's FinishSignon() asks for no key: `#else
     * VW_UpdateScreen(); VW_WaitVBL(3*70);` - three seconds of the lights,
     * and on. It has no half second before them either: with no prompt to
     * wait for, that would only be time on top of the three it asks for, so
     * the lights are on from the first frame and the three seconds start
     * there. */
    fade_start(0, black, 1);
    fade_update(1);
    if (wolf_version == WV_SPEAR) {
        draw_signon("", 0);
        wait_tics = 0;
        signon_tics = 3 * 70;
    } else {
        draw_signon(NULL, 0);
        wait_tics = SIGNON_WAIT;
    }
    phase = PH_SIGNON;
}

/* StartCPMusic(INTROSONG); PG13() - VW_FadeOut() has already run */
static void start_pg13(void)
{
    view_bar(0, 0, 320, 200, 0x82);
    view_pic(216, 110, PIC_PG13);
    fade_in(FADESTEPS);
    phase = PH_PG13_IN;
}

/* PlayLoop(), one frame: doors and push walls, the player, every actor,
 * the palette flashes, the view. */
static void play(const controls_t *c, const buttons_t *b, int tics, int fps,
                 bool render)
{
    madenoise = false;
    move_doors(tics);
    move_pwalls(tics);

    if (deathcam) {
        /* s_deathcam: the player has no think while the boss dies again */
    } else if (gamestate.victoryflag) {
        /* T_Player() spins; T_Attack() glances first, if mid-attack */
        if (player_attacking())
            update_face(tics);
        player_victory_spin(tics);
    } else if (!player_attacking()) {
        /* T_Player(): UpdateFace(), CheckWeaponChange(), Cmd_Use() for as
         * long as the button is held, Cmd_Fire() on a fresh press, then
         * ControlMovement() */
        update_face(tics);
        if (b->select)
            player_select_weapon(b->select - 1);
        if (b->use_held) {
            const int used = player_use(use_last);
            if (used != USE_NOTHING)
                leaving = used;
        }
        player_weapon(b->fire, fire_last, b->change, tics);
        player_move(c, tics);
        fire_last = b->fire;
        use_last  = b->use_held;
    } else {
        /* T_Attack(): a press that starts mid-attack is thrown away - so
         * next frame it does not count as held - then ControlMovement(),
         * and only then the attack frames */
        update_face(tics);
        player_move(c, tics);
        player_weapon(b->fire, fire_last, b->change, tics);
        fire_last = b->fire && fire_last;
        use_last  = b->use_held && use_last;
    }
    if (!gamestate.victoryflag && exit_triggered)
        victory_tile();                     /* Thrust() onto EXITTILE */

    actors_think(tics);
    update_palette_shifts(tics);
    /* The death cam has just begun: the frame on screen is the last one
     * before it, which the interlude starts from */
    if (render && !deathcam_started) {
        render_view();
        if (opt_show_fps)
            hud_draw_fps(fps);
    }
    face_funny(tics);                       /* Spear of Destiny's yawn */
    gamestate.timecount += tics;
}

void game_frame(const controls_t *c, const buttons_t *b, int tics, int fps)
{
    const bool ack = b->use || b->menu || (b->fire && !fire_prev);

    fire_prev = b->fire;
    frame_fps = fps;
    sd_update(tics);

    switch (phase) {
    /* --- the sign-on screen, then PG13(), once at start-up --- */
    case PH_SIGNON:
        if (wait_tics > 0) {                /* "One moment..." */
            wait_tics -= tics;
            draw_signon(wait_tics > 0 ? NULL : "Press a button",
                        wait_tics > 0 ? 0 : 14);
        } else if (wolf_version == WV_SPEAR) {
            /* FinishSignon()'s three seconds, running since game_init() */
            draw_signon("", 0);
            if ((signon_tics -= tics) <= 0) {
                sd_start_music(NAZI_NOR_MUS);
                fade_out(FADE_BLACK, FADESTEPS);
                phase = PH_SIGNON_OUT;
            }
        } else if (ack || b->back || b->pause) {
            /* "Working..." while InitGame() finishes; then DemoLoop()
             * starts the title song and PG13() fades the screen out */
            draw_signon("Working...", 10);
            sd_start_music(NAZI_NOR_MUS);
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_SIGNON_OUT;
        } else {
            draw_signon("Press a button", 14);  /* "Press a key" on a PC */
        }
        break;

    case PH_SIGNON_OUT:
        if (fade_update(tics))
            start_pg13();
        break;

    case PH_PG13_IN:
        if (fade_update(tics)) {
            wait_tics = 7 * 70;
            phase = PH_PG13;
        }
        break;

    case PH_PG13:
        wait_tics -= tics;
        if (wait_tics <= 0 || ack || b->back || b->pause) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_PG13_OUT;
        }
        break;

    case PH_PG13_OUT:
        if (fade_update(tics))
            show_title(0);
        break;

    /* --- DemoLoop(): title, credits, high scores --- */
    case PH_TITLE_IN:
        if (fade_update(tics))
            phase = PH_TITLE;
        break;

    case PH_TITLE:
        wait_tics -= tics;
        if (ack || b->back || b->pause) {
            /* a key: VW_FadeOut(), US_ControlPanel() */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_ATTRACT_OUT;
        } else if (wait_tics <= 0) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_TITLE_OUT;
        }
        break;

    case PH_TITLE_OUT:
        if (fade_update(tics)) {
            if (title_screen < 2)
                show_title(title_screen + 1);
            else
                start_demo();
        }
        break;

    case PH_ATTRACT_OUT:
        if (fade_update(tics))
            open_menu(false);
        break;

    /* --- PlayDemo() --- */
    case PH_DEMO_IN:
        if (fade_update(tics)) {
            /* PlayLoop()'s first frame runs, and its ThreeDRefresh()
             * fizzles in rather than showing - see fizzle_in_start() */
            fire_last = use_last = false;
            if (demo_frame(false)) {
                fizzle_in_start();
                phase = PH_DEMO_FIZZLE;
            } else
                end_demo();
        }
        break;

    case PH_DEMO_FIZZLE:
        if (fizzle_in_step(tics, 20)) {
            demo_tics = 0;
            phase = PH_DEMO;
        }
        break;

    case PH_DEMO:
        /* IN_CheckAck(): any button ends the demo, and on to the menu */
        if (ack || b->back || b->pause) {
            demo_aborted = true;
            end_demo();
            break;
        }
        /* PollControls() waits out DEMOTICS between frames, however fast
         * the machine; a slow frame here plays two */
        demo_tics += tics;
        while (demo_tics >= 4) {
            bool more;
            demo_tics -= 4;
            more = demo_frame(true);
            if (!more || player_died || leaving != USE_NOTHING) {
                demo_end = player_died ? DEMO_DIED
                         : leaving != USE_NOTHING ? DEMO_ELEVATOR : DEMO_RAN_OUT;
                leaving = USE_NOTHING;
                end_demo();
                break;
            }
        }
        break;

    case PH_DEMO_OUT:
        if (fade_update(tics)) {
            if (demo_aborted)
                open_menu(false);
            else {
                sd_start_music(NAZI_NOR_MUS);
                show_title(0);
            }
        }
        break;

    /* --- a new floor --- */
    case PH_PSYCHED_IN:
        if (fade_update(tics)) {
            wait_tics = 70;                 /* IN_UserInput(70) */
            phase = PH_PSYCHED;
        }
        break;

    case PH_PSYCHED:
        wait_tics -= tics;
        if (wait_tics <= 0 || ack) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_PSYCHED_OUT;
        }
        break;

    case PH_PSYCHED_OUT:
        if (fade_update(tics)) {
            fizzle_in_start();
            phase = PH_FIZZLE_IN;
        }
        break;

    case PH_FIZZLE_IN:
        /* FizzleFade(..., 20, false) from ThreeDRefresh() */
        if (fizzle_in_step(tics, 20)) {
            if (fade_amount) {
                fade_in(FADESTEPS);         /* if (screenfaded) VW_FadeIn() */
                phase = PH_FADE_IN;
            } else
                phase = PH_PLAY;
        }
        break;

    case PH_FADE_IN:
        if (fade_update(tics))
            phase = PH_PLAY;
        break;

    /* --- playing --- */
    case PH_PLAY:
    case PH_MAP:
        /* The map (not in the DOS game) is play with the map drawn over the
         * view, as Doom's is: everything runs, the view included - items
         * are picked up and the aim is taken as it is drawn, and it is what
         * marks the map - and L shows or hides it. Anything that ends play
         * leaves it behind. */
        play(c, b, tics, fps, true);

        if (deathcam_started) {
            /* A_StartDeathCam(): FinishPaletteShifts(), VW_WaitVBL(100) */
            clear_palette_shifts();
            wait_tics = 100;
            phase = PH_DEATHCAM_WAIT;
        } else if (spear_taken) {
            /* the Spear: GETSPEARSND over a still screen for 150 tics, or
             * till it ends without digitized sound */
            spear_taken = false;
            sd_stop_sound();
            sd_play_sound(GETSPEARSND);
            wait_tics = sd_digi_on ? 150 : 0;
            phase = PH_SPEAR;
        } else if (player_died) {
            /* PlayLoop() ends, GameLoop() stops the music, Died() starts */
            sd_start_music(-1);
            gamestate.weapon = -1;          /* take away weapon */
            sd_play_sound(PLAYERDEATHSND);
            phase = PH_DEATH_TURN;
        } else if (leaving != USE_NOTHING) {
            phase = PH_ELEVATOR;
        } else if (victory_done) {
            sd_start_music(-1);
            /* Spear of Destiny's fades slowly to a dark teal:
             * VL_FadeOut(0,255,0,17,17,300) */
            if (wolf_version == WV_SPEAR)
                fade_out((8 << 6) | (8 << 1) | 1, 300);
            else
                fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_VICTORY_OUT;
        } else if (b->pause) {
            /* CheckKeys(): LatchDrawPic(20-4,80-2*8,PAUSEDPIC) - centered
             * in the view - and the music stops. Over the map the sign goes
             * across the top of the screen instead, as Doom's does over its
             * automap, so as not to hide the map. Carrying on is Doom's way
             * too: only the pause buttons again (Start still opens the
             * menu), where DOS took any key. From the map, play goes back
             * to the map. */
            paused_from_map = phase == PH_MAP;
            if (paused_from_map) {
                automap_draw();
                view_pic((VIEW_W - 64) / 2, 4, PIC_PAUSED);
            } else {
                view_pic(16 * 8, VIEW_H / 2 - 16, PIC_PAUSED);
            }
            sd_pause_music(true);
            phase = PH_PAUSED;
        } else if (b->map) {
            phase = phase == PH_MAP ? PH_PLAY : PH_MAP;
        } else if (b->menu) {
            /* Esc: StopMusic(), VW_FadeOut(), US_ControlPanel() */
            sd_start_music(-1);
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_MENU_OUT;
        }

        if (phase == PH_MAP) {
            automap_draw();                 /* over this frame's view */
            if (opt_show_fps)
                hud_draw_fps(fps);
        }
        break;

    case PH_PAUSED:
        /* only the pause buttons carry on, as in Doom; Start goes to the
         * menu, as Esc does in play, and Back to Game from there resumes */
        if (b->pause) {
            sd_pause_music(false);
            phase = paused_from_map ? PH_MAP : PH_PLAY;
        } else if (b->menu) {
            sd_pause_music(false);
            sd_start_music(-1);             /* StopMusic(), VW_FadeOut() */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_MENU_OUT;
        }
        break;

    case PH_ELEVATOR:
        /* Cmd_Use() on the elevator calls SD_WaitSoundDone(): nothing
         * moves until the level-done sound has played out. */
        render_view();
        if (opt_show_fps)
            hud_draw_fps(fps);
        if (sd_sound_playing() != LEVELDONESND) {
            secret  = leaving == USE_SECRET_ELEVATOR;
            leaving = USE_NOTHING;
            gamestate.keys = 0;
            sd_start_music(-1);
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_LEVEL_OUT;
        }
        break;

    /* --- the rest of A_StartDeathCam(): the pause, the view fizzled out
     * to "Let's see that again!", IN_UserInput(300), DrawPlayBorder(), and
     * the view fizzling in on the boss - where play goes on --- */
    case PH_DEATHCAM_WAIT:
        wait_tics -= tics;
        if (wait_tics <= 0) {
            fizzle_start_whole();           /* VW_Bar(..., 127); FizzleFade */
            phase = PH_DEATHCAM_FIZZLE;
        }
        break;

    case PH_DEATHCAM_FIZZLE:
        if (fizzle_step(tics, 70, wolf_palette[127])) {
            write_text(0, 7, "Let's see that again!");
            wait_tics = 300;
            phase = PH_DEATHCAM_TEXT;
        }
        break;

    case PH_DEATHCAM_TEXT:
        wait_tics -= tics;
        if (wait_tics <= 0 || ack || b->back) {
            draw_play_border(view_size);
            fizzle_in_start();
            phase = PH_DEATHCAM_IN;
        }
        break;

    case PH_DEATHCAM_IN:
        if (fizzle_in_step(tics, 20))
            phase = PH_PLAY;
        break;

    /* --- Spear of Destiny: the Spear taken, on to the last floor, where
     * the player stood --- */
    case PH_SPEAR:
        wait_tics -= tics;
        if (wait_tics <= 0 && sd_sound_playing() != GETSPEARSND) {
            const int keys = gamestate.keys;    /* SetupGameLevel() keeps them */
            gamestate.oldscore = gamestate.score;
            level = 20;
            level_load(level);
            gamestate.keys = keys;
            player_place(spear_x, spear_y, spear_angle);
            phase = PH_PLAY;
        }
        break;

    /* --- the menus --- */
    case PH_MENU_OUT:
        if (fade_update(tics))
            open_menu(true);
        break;

    case PH_MENU:
        switch (menu_update(tics, b)) {
        case MENU_RESUME:
            /* DrawPlayScreen(); VW_FadeIn(); StartMusic() */
            fullscreen = false;
            render_view();
            fade_in(FADESTEPS);
            phase = PH_RESUME_IN;
            break;

        case MENU_NEW_GAME:
            /* GameLoop() starts over: DrawPlayScreen() fades to black */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_NEW_GAME_OUT;
            break;

        case MENU_LOAD_GAME:
            /* the same way out as New Game, to a floor loaded, not new */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_LOAD_GAME_OUT;
            break;

        case MENU_BACK_TO_DEMO:
            /* the menu has faded out and started the title song */
            show_title(0);
            break;

        case MENU_WARP:
            /* Tab-W: playstate = ex_warped, and GameLoop() goes round - the
             * score as the floor began, SetupGameLevel() on the floor
             * chosen, and "Get Psyched!" */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_WARP_OUT;
            break;

        case MENU_END_FLOOR:
            /* Tab-E: playstate = ex_completed, as the elevator would have
             * it - no keys, and the stats screen, then the next floor. But
             * floor 9 ends in the victory, not an elevator: LevelCompleted()
             * gives any floor past 8 the secret floor's screen, which Tab-E
             * showed there in DOS. So there it is ex_victorious - Victory(),
             * the end text and the high scores, as BJ's run leads to. The
             * same goes for Spear of Destiny's last floor, the Angel of
             * Death's, which in DOS went on to a floor that is not there. */
            leaving = USE_NOTHING;
            fade_out(FADE_BLACK, FADESTEPS);
            if (wolf_version == WV_SPEAR ? level == 20 : level_mapon(level) == 8) {
                phase = PH_VICTORY_OUT;
                break;
            }
            secret = false;
            gamestate.keys = 0;
            phase = PH_LEVEL_OUT;
            break;

        case MENU_END_GAME:
            /* CP_EndGame(): no lives, and PlayLoop() ends as a death */
            fullscreen = false;
            render_view();
            gamestate.lives = 0;
            player_died = true;
            killerobj = NULL;               /* no one to face: no turn */
            gamestate.weapon = -1;
            sd_play_sound(PLAYERDEATHSND);
            phase = PH_DEATH_TURN;
            break;
        }
        break;

    case PH_RESUME_IN:
        if (fade_update(tics)) {
            sd_start_music((int)wolf_level_song[level]);
            phase = PH_PLAY;
        }
        break;

    case PH_NEW_GAME_OUT:
        if (fade_update(tics)) {
            fullscreen = false;
            new_game(menu_difficulty(), menu_episode());
            start_floor(true);
        }
        break;

    case PH_WARP_OUT:
        if (fade_update(tics)) {
            fullscreen = false;
            leaving = USE_NOTHING;
            gamestate.score = gamestate.oldscore;
            level = menu_warp_floor();
            start_floor(true);
        }
        break;

    case PH_LOAD_GAME_OUT:
        /* LoadTheGame(), then GameLoop() with loadedgame set: no
         * SetupGameLevel() of its own, but "Get Psyched!" as ever */
        if (fade_update(tics)) {
            fullscreen = false;
            leaving = USE_NOTHING;
            if (load_game(menu_load_slot())) {
                level = current_level;
                enter_floor(true);
            } else {
                sd_start_music(NAZI_NOR_MUS);   /* checked before; cannot happen */
                show_title(0);
            }
        }
        break;

    /* --- the stats screen --- */
    case PH_LEVEL_OUT:
        if (fade_update(tics)) {
            fullscreen = false;             /* from the menu, End Floor */
            intermission_start(level);
            fade_in(FADESTEPS);
            phase = PH_INTER_IN;
        }
        break;

    case PH_INTER_IN:
        if (fade_update(tics))
            phase = PH_INTER;
        break;

    case PH_INTER:
        if (intermission_update(tics, ack)) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_INTER_OUT;
        }
        break;

    case PH_INTER_OUT:
        if (fade_update(tics)) {
            gamestate.oldscore = gamestate.score;
            level = next_floor(level, secret) % wolf_num_levels;
            start_floor(true);
        }
        break;

    /* --- Died() --- */
    case PH_DEATH_TURN:
        if (player_face_killer(tics)) {
            clear_palette_shifts();         /* FinishPaletteShifts */
            fizzle_start();
            phase = PH_DEATH_FIZZLE;
        } else
            render_view();
        break;

    case PH_DEATH_FIZZLE:
        /* the view is not redrawn; red eats into the last frame */
        if (fizzle_step(tics, 70, wolf_palette[4])) {
            wait_tics = 100;
            phase = PH_DEATH_WAIT;
        }
        break;

    case PH_DEATH_WAIT:
        /* IN_UserInput(100), then SD_WaitSoundDone() */
        wait_tics -= tics;
        if ((wait_tics <= 0 || ack) && sd_sound_playing() != PLAYERDEATHSND) {
            if (player_lose_life())
                start_floor(false);         /* fizzles in over the red */
            else {
                fade_out(FADE_BLACK, FADESTEPS);
                phase = PH_GAME_OVER;
            }
        }
        break;

    case PH_GAME_OVER:
        if (fade_update(tics))
            check_high_score();
        break;

    /* --- CheckHighScore() --- */
    case PH_SCORES_IN:
        if (fade_update(tics)) {
            if (score_row >= 0) {
                /* US_LineInput(4*8, 76+16*n, name, nil, true, MaxHighName,
                 * 100) over BORDCOLOR, with the typing strip under the table;
                 * Spear of Destiny's in the menu font, in a box of its own */
                const int y = 76 + 16 * score_row;
                if (wolf_version == WV_SPEAR) {
                    view_bar(16 - 2, y - 2, 145, 15, 0x9c);
                    line_input_start(16, y, "", HIGH_NAME_LEN, 130, 15, 0x9c,
                                     true, 1);
                } else
                    line_input_start(4 * 8, y, "", HIGH_NAME_LEN, 100, 15, 0x29,
                                     true, 0);
                phase = PH_SCORES_NAME;
            } else {
                wait_tics = 500;            /* IN_UserInput(500) */
                phase = PH_SCORES_WAIT;
            }
        }
        break;

    case PH_SCORES_NAME:
        /* Esc (B on an empty name) leaves the name empty, as in DOS */
        if (line_input_update(tics, b) != LINE_BUSY) {
            strncpy(high_scores[score_row].name, line_input_text(), HIGH_NAME_LEN);
            save_high_scores();             /* WriteConfig(), at once */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_SCORES_OUT;
        }
        break;

    case PH_SCORES_WAIT:
        wait_tics -= tics;
        if (wait_tics <= 0 || ack || b->back || b->pause) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_SCORES_OUT;
        }
        break;

    case PH_SCORES_OUT:
        /* GameLoop() returns: VW_FadeOut(), StartCPMusic(INTROSONG) */
        if (fade_update(tics)) {
            sd_start_music(NAZI_NOR_MUS);
            show_title(0);
        }
        break;

    /* --- Victory() and EndText() --- */
    case PH_VICTORY_OUT:
        if (fade_update(tics)) {
            if (wolf_version == WV_SPEAR) {
                /* BJ, out of the castle, collapses: the whole screen
                 * VIEWCOLOR, the four pictures, then a quick fade to teal
                 * and the usual screen - with the status bar long gone */
                fullscreen = true;
                sd_start_music(XTHEEND_MUS);
                view_bar(0, 0, 320, 200, 127);
                view_pic(124, 44, PIC_BJCOLLAPSE1);
                fade_in(FADESTEPS);
                collapse_step = 0;
                wait_tics = 2 * 70;
                phase = PH_COLLAPSE;
                break;
            }
            fullscreen = false;             /* from the menu, End Floor */
            victory_start();
            fade_in(FADESTEPS);
            phase = PH_VICTORY_IN;
        }
        break;

    case PH_COLLAPSE:
        if (!fade_update(tics))
            break;
        if (collapse_step == 4) {           /* faded out: the averages */
            victory_start();
            fade_in(FADESTEPS);
            phase = PH_VICTORY_IN;
            break;
        }
        if ((wait_tics -= tics) > 0)
            break;
        if (++collapse_step < 4) {
            view_pic(124, 44, PIC_BJCOLLAPSE1 + collapse_step);
            wait_tics = collapse_step == 3 ? 3 * 70 : 105;
        } else
            fade_out((8 << 6) | (8 << 1) | 1, 5);  /* VL_FadeOut(0,255,0,17,17,5) */
        break;

    case PH_VICTORY_IN:
        if (fade_update(tics))
            phase = PH_VICTORY;
        break;

    case PH_VICTORY:
        if (ack) {                          /* IN_Ack() */
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_VICTORY_DONE;
        }
        break;

    case PH_VICTORY_DONE:
        if (fade_update(tics)) {
            /* EndText(): the episode's article - or Spear of Destiny's
             * EndSpear(), its pictures */
            const int ep = level_episode(current_level);
            if (wolf_version == WV_SPEAR) {
                endspear = 0;
                start_endscreen();
                break;
            }
            if (!wolf_num_end_articles) {
                check_high_score();
                break;
            }
            article_start(wolf_end_articles[ep < wolf_num_end_articles
                                            ? ep : wolf_num_end_articles - 1]);
            fullscreen = true;
            fade_in(TEXTFADE);
            phase = PH_ENDTEXT_IN;
        }
        break;

    case PH_ENDTEXT_IN:
        if (fade_update(tics))
            phase = PH_ENDTEXT;
        break;

    case PH_ENDTEXT:
        if (article_update(b->left ? -1 : b->right ? 1 : 0, b->use,
                           b->back || b->menu)) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_ENDTEXT_OUT;
        }
        break;

    case PH_ENDTEXT_OUT:
        if (fade_update(tics))
            check_high_score();
        break;

    /* --- EndSpear(): each end screen fades in with its own palette and
     * waits for IN_Ack(); the second has two captions, each waiting ten
     * seconds or a button --- */
    case PH_ENDSPEAR_IN:
        if (fade_update(tics)) {
            if (endspear == 1) {
                endspear_caption(STR_ENDGAME1, STR_ENDGAME2);
                wait_tics = 700;
            }
            phase = PH_ENDSPEAR;
        }
        break;

    case PH_ENDSPEAR:
        if (endspear == 1) {
            wait_tics -= tics;
            if (wait_tics > 0 && !ack)
                break;
            if (endspear_second) {
                fade_out(FADE_BLACK, FADESTEPS);
                phase = PH_ENDSPEAR_OUT;
            } else {
                endspear_second = true;
                view_bar(0, 180, 320, 20, 0);
                endspear_caption(STR_ENDGAME3, STR_ENDGAME4);
                wait_tics = 700;
            }
        } else if (ack) {
            fade_out(FADE_BLACK, FADESTEPS);
            phase = PH_ENDSPEAR_OUT;
        }
        break;

    case PH_ENDSPEAR_OUT:
        if (fade_update(tics)) {
            if (++endspear < ENDSCREENS)
                start_endscreen();
            else
                check_high_score();
        }
        break;
    }

    if (!fullscreen)
        status_draw();
}
