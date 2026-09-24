/* menu.c - the control panel: US_ControlPanel() and WL_MENU.C.
 *
 * The main menu, New Game (episode, then difficulty), Sound, Load Game and
 * Save Game, Read This! and View Scores, with the DOS layouts, colors, gun
 * cursor, sounds and fades. Each DOS routine was a blocking loop -
 * HandleMenu() waiting for a key, Confirm() for Y or N, MenuFadeOut() for
 * ten VBLs - so here they are states that run a frame at a time, and each
 * "then do this" that followed a blocking call is an action to run once the
 * wait is over.
 *
 * Change View sizes the 3D view, with its border drawn at the new size.
 *
 * Control keeps the DOS screen but not its rows - enabling the mouse and
 * the joystick, the second port, the Gravis GamePad and customizing the
 * keys mean nothing with one N64 pad. It has Always Run instead, Stick
 * Sensitivity, which works as Mouse Sensitivity did, and Customize controls,
 * with one section for the pad: a row for its buttons (the joystick's Run,
 * Open, Fire and Strafe, and the map, which the DOS game did not have), one
 * for what the C buttons do, and the keyboard's move row for the D-pad.
 * Quit, which has nothing
 * to quit to on a console, is replaced by Extras: the FPS readout, the
 * profiler and god mode. (Its code keeps the name it had first, Options -
 * opt_, SCR_OPTIONS.)
 *
 * On the pad: up and down move the gun, A or Start selects (Enter), B goes
 * back (Esc), and in a Confirm() A is Y and B is N.
 */
#include <stdio.h>
#include <string.h>
#include "wolf.h"

/* WL_MENU.H's colors: Wolfenstein's reds, or Spear of Destiny's blues */
#define SPEAR       (wolf_version == WV_SPEAR)
/* "Read This!" is the shareware's alone: the full game as GT Interactive
 * released it (GOODTIMES) and Spear of Destiny both built their menus
 * without it (WL_MENU.C) */
#define READ_THIS   (wolf_version == WV_SHAREWARE)
#define BORDCOLOR   (SPEAR ? 0x99 : 0x29)
#define BORD2COLOR  (SPEAR ? 0x93 : 0x23)
#define DEACTIVE    (SPEAR ? 0x9b : 0x2b)
#define BKGDCOLOR   (SPEAR ? 0x9d : 0x2d)
#define STRIPE      0x2c                    /* Wolfenstein's only */
#define READCOLOR   0x4a
#define READHCOLOR  0x47
#define TEXTCOLOR   0x17
#define HIGHLIGHT   0x13

#define MENUFADE    10                      /* MenuFadeOut(), MenuFadeIn() */
#define MENUFONT    1

#define MENU_X  76
#define MENU_Y  55
#define MENU_W  178
#define MENU_H  (READ_THIS ? 13 * 10 + 6 : 13 * 9 + 6)

#define SM_X    48
#define SM_W    250
#define SM_Y1   20
#define SM_H1   3 * 13 - 7                  /* two rows each; DOS had three */
#define SM_Y2   SM_Y1 + 5 * 13
#define SM_H2   3 * 13 - 7
#define SM_Y3   SM_Y2 + 5 * 13
#define SM_H3   3 * 13 - 7

#define CTL_X   24
#define CTL_Y   70
#define CTL_W   284
#define CTL_H   13 * 3 + 6                  /* three rows; DOS had six */

#define CST_Y       48
#define CST_ROW(r)  (CST_Y + 13 * 2 + 26 * (r)) /* row r's inputs; labels above */
#define CST_LEFT    40                      /* the columns, spread between */
#define CST_RIGHT   310                     /* the gun and the window edge */

#define NM_X    50
#define NM_Y    100
#define NM_W    225
#define NM_H    13 * 4 + 15

#define NE_X    10
#define NE_Y    23
#define NE_W    320 - NE_X * 2
#define NE_H    200 - NE_Y * 2

#define LSM_X   85
#define LSM_Y   55
#define LSM_W   175
#define LSM_H   10 * 13 + 10

#define LSA_X   96                          /* DrawLSAction()'s window */
#define LSA_Y   80
#define LSA_W   130
#define LSA_H   42
#define LSA_TICS  24                        /* how long it shows       */

/* The DOS boxes answer to the Y and N keys, which a pad does not have, so
 * each names the buttons on a line of its own. */
#define CURGAME     "You are currently in\n" \
                    "a game. Continuing will\n" \
                    "erase old game. Ok?\n" \
                    "(A = yes, B = no)"
#define ENDGAMESTR  "Are you sure you want\n" \
                    "to end the game you\n" \
                    "are playing?\n" \
                    "(A = yes, B = no):"
#define GAMESVD     "There's already a game\n" \
                    "saved at this position.\n" \
                    "      Overwrite?\n" \
                    "(A = yes, B = no)"
/* STR_NOSPACE1 and 2, with the disk made a cartridge */
#define NOSPACE     "There is not enough space\n" \
                    "on the cartridge to Save Game!"

typedef struct {
    int x, y, amount, curpos, indent;
} iteminfo_t;

typedef struct {
    int  active;                            /* 0 no, 1 yes, 2 READ, 3 order */
    char string[36];
    bool routine;                           /* selecting it fades out first */
} item_t;

enum { newgame, soundmenu, control, options, loadgame, savegame, changeview,
       readthis, viewscores, backtodemo };

static iteminfo_t main_items = { MENU_X, MENU_Y, 10, readthis, 24 };

/* Without "Read This!" - the full game and Spear of Destiny - the items
 * after it move up a place. main_shown[] is the menu as it is drawn and
 * handled, and item_at() turns a place in it back into an item. */
static int item_at(int place)
{
    return !READ_THIS && place >= readthis ? place + 1 : place;
}
static iteminfo_t snd_items  = { SM_X, SM_Y1, 12, 0, 52 };
static iteminfo_t opt_items  = { SM_X, SM_Y1, 11, 0, 52 };
static iteminfo_t ctl_items  = { CTL_X, CTL_Y, 3, 0, 56 };
static iteminfo_t cus_items  = { 8, CST_ROW(0), 7, 0, 0 };
static iteminfo_t newe_items = { NE_X, NE_Y, 11, 0, 88 };
static iteminfo_t new_items  = { NM_X, NM_Y, 4, 2, 24 };
static iteminfo_t ls_items   = { LSM_X, LSM_Y, 10, 0, 24 };

static item_t main_menu[] = {
    { 1, "New Game",     true  },
    { 1, "Sound",        true  },
    { 1, "Control",      true  },
    { 1, "Extras",       true  },           /* replaces Quit (the last item) */
    { 1, "Load Game",    true  },
    { 0, "Save Game",    true  },
    { 1, "Change View",  true  },
    { 2, "Read This!",   true  },
    { 0, "View Scores",  true  },
    { 0, "Back to Demo", false },
};
static item_t main_shown[10];

/* Extras - not in the DOS game. Laid out like the Sound menu: rows under
 * section titles. The switches have a light that is lit when they are on;
 * all start off and are saved with the other settings (save.c). The rest
 * of the cheats are the DOS game's other ones, each done once when chosen,
 * and only in a game - shown inactive otherwise, as Save Game is. */
bool opt_show_fps, opt_show_profiler, opt_god_mode, opt_no_clip;

enum {
    OPT_ROW_FPS, OPT_ROW_PROFILER,
    OPT_ROW_GODMODE = 5,                    /* Tab-G */
    OPT_ROW_NOCLIP,                         /* Tab-N, Spear of Destiny's */
    OPT_ROW_MLI,                            /* the M-L-I code */
    OPT_ROW_ITEMS,                          /* Tab-I */
    OPT_ROW_WARP,                           /* Tab-W */
    OPT_ROW_ENDFLOOR,                       /* Tab-E */
    OPT_ROWS
};

static item_t opt_menu[OPT_ROWS] = {
    { 1, "Show FPS", false }, { 1, "Show Profiler", false },
    { 0, "", false }, { 0, "", false }, { 0, "", false },
    { 1, "God Mode", false }, { 1, "No Clip", false },
    { 0, "Health, Ammo & Keys", false },    /* M-L-I, after its message */
    { 0, "Free Items", false },
    { 0, "Warp to Floor", false }, { 0, "End Floor", false },
};

/* STR_CHEATER1..5, which M-L-I shows; Tab-I's own message; Tab-W's question,
 * the number then chosen with the D-pad */
#define MLISTR      "You now have 100% Health,\n" \
                    "99 Ammo and both Keys!\n" \
                    "\n" \
                    "Note that you have basically\n" \
                    "eliminated your chances of\n" \
                    "getting a high score!"
#define ITEMSSTR    "Free items!"
#define WARPSTR     "Warp to which floor (1-%d): %d"
/* a floor of the episode being played, as Tab-W took it; Spear of
 * Destiny's are all one run */
#define WARP_FLOORS (wolf_version == WV_SPEAR ? 21 : 10)

static int warp_floor, warp_delay;          /* Tab-W's floor, 1..WARP_FLOORS */

/* CtlMenu[]: Always Run is a switch with a light, as the DOS rows that
 * enabled each device were, and Stick Sensitivity opens its own screen, as
 * Mouse Sensitivity did. Saved with the other settings; main.c reads them. */
bool ctl_always_run;
int  ctl_stick_sensitivity = CTL_SENS_DEFAULT;

static const item_t ctl_menu[] = {
    { 1, "Always Run", false }, { 1, "Stick Sensitivity", true },
    { 1, "Customize controls", true },
};

/* CustomControls() had rows of Run, Open, Fire and Strafe for the mouse,
 * the joystick and the keyboard, and the keyboard's move keys below. Here
 * there is the one section, for the pad, with three rows: the joystick's
 * with the map added, what the C buttons do, and the DOS move row for the
 * D-pad. Any input can take any action in any row, as any key could in
 * DOS. The gun picks a row, as it picked a device; the rows between are the
 * labels. */
enum { CST_ROWS = 3 };
static const int cst_row_first[CST_ROWS + 1] = {
    CTL_RUN, CTL_STRAFE_LEFT, CTL_LEFT, CTL_ACTIONS
};
/* ...and after them, where the gun can reach it the same way, Reset
 * controls: every input back to its default, once a Confirm() says so. It
 * is drawn as a fourth row (CST_ROWS) in a window of its own. */
#define CST_RESET_ITEM  (2 * CST_ROWS)
#define CST_RESET_STR   "Reset controls"
#define RESETSTR        "Reset all controls\n" \
                        "to their original settings?\n" \
                        "(A = yes, B = no)"
static const item_t cus_menu[] = {
    { 1, "", false }, { 0, "", false }, { 1, "", false }, { 0, "", false },
    { 1, "", false }, { 0, "", false }, { 1, "", false }
};

static const char *const pad_names[PAD_BUTTONS] = {
    "B", "A", "Z", "R", "L",
    "C-Left", "C-Right", "C-Up", "C-Down",
    "Left", "Right", "Up", "Down"           /* the D-pad, and the stick */
};
/* The widest of the names in a font. In the menu font C-Right is 63
 * pixels: four columns that wide fit a row, but five do not. So the first
 * row's columns are made to hold any name in the small font (C-Down, 46),
 * and a name too wide for its column in the menu font - C-Left, C-Right or
 * C-Down, if one is given to a first-row action - is written in that. */
#define SMALLFONT   0

static int pad_name_w(int font)
{
    int w = 0;
    for (int i = 0; i < PAD_BUTTONS; i++)
        if (font_measure(font, pad_names[i]) > w)
            w = font_measure(font, pad_names[i]);
    return w;
}

/* an input's name in a column colw wide: the menu font if it fits, or else
 * the small font, set down a pixel to sit in the row */
static void draw_pad_name(int x, int y, int colw, const char *name, int color)
{
    if (font_measure(MENUFONT, name) <= colw)
        font_draw(MENUFONT, x, y, name, color);
    else
        font_draw(SMALLFONT, x, y + 1, name, color);
}
static const char *const cst_labels[CTL_ACTIONS] = {
    "Run", "Open", "Fire", "Strafe",        /* STR_CRUN ... STR_CSTRAFE */
    "Map",
    "Str. L", "Str. R", "Wpn +", "Wpn -",
    "Left", "Right", "Frwd", "Bkwd"         /* STR_LEFT ... STR_BKWD */
};

int ctl_button[CTL_ACTIONS] = {             /* each action on its own input */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
};

void ctl_default_buttons(void)
{
    for (int i = 0; i < CTL_ACTIONS; i++)
        ctl_button[i] = i;
}

bool ctl_unpack_buttons(const uint8_t *p)
{
    int b[CTL_ACTIONS];
    unsigned seen = 0;

    for (int i = 0; i < CTL_ACTIONS; i++) {
        b[i] = (p[i / 2] >> (i % 2 * 4)) & 15;
        if (b[i] < PAD_BUTTONS)
            seen |= 1u << b[i];
    }
    if (seen != (1u << PAD_BUTTONS) - 1) {
        ctl_default_buttons();
        return false;
    }
    memcpy(ctl_button, b, sizeof ctl_button);
    return true;
}

void ctl_pack_buttons(uint8_t *p)
{
    memset(p, 0, CTL_PACKED_BYTES);
    for (int i = 0; i < CTL_ACTIONS; i++)
        p[i / 2] |= (uint8_t)(ctl_button[i] << (i % 2 * 4));
}

bool ctl_held(int action, unsigned pad)
{
    return (pad >> ctl_button[action]) & 1;
}

/* SndMenu[] offered each kind of sound on the PC's devices - PC Speaker or
 * AdLib/Sound Blaster effects, Disney Sound Source or Sound Blaster
 * digitized sound, AdLib/Sound Blaster music. The N64 has one sound system,
 * the RCP's audio (mixed on the RSP), so each is None or Nintendo 64 Audio.
 * The rows keep their DOS places under the three titles, one row shorter. */
static item_t snd_menu[] = {
    { 1, "None", false }, { 1, "Nintendo 64 Audio", false },
    { 0, "", false }, { 0, "", false }, { 0, "", false },
    { 1, "None", false }, { 1, "Nintendo 64 Audio", false },
    { 0, "", false }, { 0, "", false }, { 0, "", false },
    { 1, "None", false }, { 1, "Nintendo 64 Audio", false },
};

static item_t newe_menu[] = {
    { 1, "Episode 1\nEscape from Wolfenstein", false }, { 0, "", false },
    { 3, "Episode 2\nOperation: Eisenfaust",   false }, { 0, "", false },
    { 3, "Episode 3\nDie, Fuhrer, Die!",       false }, { 0, "", false },
    { 3, "Episode 4\nA Dark Secret",           false }, { 0, "", false },
    { 3, "Episode 5\nTrail of the Madman",     false }, { 0, "", false },
    { 3, "Episode 6\nConfrontation",           false },
};

/* LSMenu[]: ten slots, their names printed by PrintLSEntry() */
static const item_t ls_menu[10] = {
    { 1, "", false }, { 1, "", false }, { 1, "", false }, { 1, "", false },
    { 1, "", false }, { 1, "", false }, { 1, "", false }, { 1, "", false },
    { 1, "", false }, { 1, "", false },
};

static const item_t new_menu[] = {
    { 1, "Can I play, Daddy?",    false },
    { 1, "Don't hurt me.",        false },
    { 1, "Bring 'em on!",         false },
    { 1, "I am Death incarnate!", false },
};

/* color_hlite[] and color_norml[], by an item's active; DEACTIVE is the
 * version's */
static int color_hlite(int active)
{
    static const int c[] = { 0, HIGHLIGHT, READHCOLOR, 0x67 };
    return active ? c[active] : DEACTIVE;
}

static int color_norml(int active)
{
    static const int c[] = { 0, TEXTCOLOR, READCOLOR, 0x6b };
    return active ? c[active] : DEACTIVE;
}

/* EpisodeSelect[]: the shareware has the first episode, the full game all
 * six, and Spear of Destiny no episodes at all - New Game goes straight to
 * the skill */
static bool episode_selectable(int episode)
{
    return wolf_version == WV_FULL || episode == 0;
}

/* ------------------------------------------------------------------ */
/* Drawing - ID_VH.C and the helpers at the end of WL_MENU.C           */
/* ------------------------------------------------------------------ */

static int printx, printy, windowx, windoww = 320, windowh = 200;
static int fontcolor;
static int fontnumber = MENUFONT;

static void hlin(int x1, int x2, int y, int color)
{
    view_bar(x1, y, x2 - x1 + 1, 1, color);
}

static void vlin(int y1, int y2, int x, int color)
{
    view_bar(x, y1, 1, y2 - y1 + 1, color);
}

/* US_Print(): newlines return to the window's left edge */
static void us_print(const char *s)
{
    char line[80];

    while (*s) {
        int n = 0;
        while (s[n] && s[n] != '\n' && n < (int)sizeof line - 1)
            n++;
        memcpy(line, s, n);
        line[n] = 0;
        font_draw(fontnumber, printx, printy, line, fontcolor);
        s += n;
        if (*s == '\n') {
            s++;
            printx = windowx;
            printy += font_height(fontnumber);
        } else
            printx += font_measure(fontnumber, line);
    }
}

/* US_CPrint(): each line centered in the window */
static void us_cprint(const char *s)
{
    char line[80];

    while (*s) {
        int n = 0;
        while (s[n] && s[n] != '\n' && n < (int)sizeof line - 1)
            n++;
        memcpy(line, s, n);
        line[n] = 0;
        font_draw(MENUFONT, windowx + (windoww - font_measure(MENUFONT, line)) / 2,
                  printy, line, fontcolor);
        printy += font_height(MENUFONT);
        s += n;
        if (*s == '\n')
            s++;
    }
}

/* ClearMScreen(): a flat red for Wolfenstein, a picture for Spear of
 * Destiny */
static void clear_mscreen(void)
{
    if (SPEAR)
        view_pic(0, 0, PIC_C_BACKDROP);
    else
        view_bar(0, 0, 320, 200, BORDCOLOR);
}

static void draw_outline(int x, int y, int w, int h, int color1, int color2)
{
    hlin(x, x + w, y, color2);
    vlin(y, y + h, x, color2);
    hlin(x, x + w, y + h, color1);
    vlin(y, y + h, x + w, color1);
}

static void draw_window(int x, int y, int w, int h, int wcolor)
{
    view_bar(x, y, w, h, wcolor);
    draw_outline(x, y, w, h, BORD2COLOR, DEACTIVE);
}

static void draw_stripes(int y)
{
    if (SPEAR) {
        view_bar(0, y, 320, 22, 0);
        hlin(0, 319, y + 23, 0);
    } else {
        view_bar(0, y, 320, 24, 0);
        hlin(0, 319, y + 22, STRIPE);
    }
}

static void set_text_color(const item_t *item, bool hlight)
{
    fontcolor = hlight ? color_hlite(item->active) : color_norml(item->active);
}

static void draw_menu(const iteminfo_t *info, const item_t *items)
{
    windowx = printx = info->x + info->indent;
    printy = info->y;
    windoww = 320;
    windowh = 200;

    for (int i = 0; i < info->amount; i++) {
        set_text_color(&items[i], info->curpos == i);
        printy = info->y + i * 13;
        if (!items[i].active)
            fontcolor = DEACTIVE;
        us_print(items[i].string);
        us_print("\n");
    }
}

/* Message(): a window centered on the screen, in the menu font */
static void message(const char *string)
{
    const int height = font_height(MENUFONT);
    int h = height, w = 0, mw = 0;

    for (const char *s = string; *s; s++) {
        if (*s == '\n') {
            if (w > mw)
                mw = w;
            w = 0;
            h += height;
        } else {
            char ch[2] = { *s, 0 };
            w += font_measure(MENUFONT, ch);
        }
    }
    if (w + 10 > mw)
        mw = w + 10;

    printy = windowh / 2 - h / 2;
    printx = windowx = 160 - mw / 2;

    draw_window(windowx - 5, printy - 5, mw + 10, h + 10, TEXTCOLOR);
    draw_outline(windowx - 5, printy - 5, mw + 10, h + 10, 0, HIGHLIGHT);
    fontcolor = 0;
    us_print(string);
}

/* ------------------------------------------------------------------ */
/* DrawHighScores() - WL_INTER.C                                       */
/* ------------------------------------------------------------------ */

/* The table is Scores[], kept with the saves (save.c). The shareware build
 * (UPLOAD) prints the floor without the full game's "E1/L" before it, and
 * Spear of Destiny has a layout of its own: a whole-screen picture with the
 * headings, the menu font, and the Spear for anyone who beat the game. */
void draw_high_scores(void)
{
    char buffer[16];

    clear_mscreen();
    draw_stripes(10);
    if (SPEAR) {
        view_pic(0, 0, PIC_HIGHSCORES);
        fontnumber = MENUFONT;
        fontcolor = HIGHLIGHT;
    } else {
        view_pic(48, 0, PIC_HIGHSCORES);
        view_pic(4 * 8, 68, PIC_C_NAME);
        view_pic(20 * 8, 68, PIC_C_LEVEL);
        view_pic(28 * 8, 68, PIC_C_SCORE);
        fontnumber = 0;
        fontcolor = 15;
    }
    for (int i = 0; i < MAX_SCORES; i++) {
        printy = 76 + 16 * i;

        printx = SPEAR ? 16 : 4 * 8;
        us_print(high_scores[i].name);

        /* Wolfenstein's font's fixed-width digits are characters 129..138 */
        snprintf(buffer, sizeof buffer, "%d", high_scores[i].completed);
        if (SPEAR)
            printx = 194 - font_measure(fontnumber, buffer);
        else {
            for (char *s = buffer; *s; s++)
                *s = (char)(*s + (129 - '0'));
            printx = 22 * 8 - font_measure(0, buffer);
        }
        if (wolf_version == WV_FULL) {
            char episode[16];
            snprintf(episode, sizeof episode, "E%d/L", high_scores[i].episode + 1);
            printx -= 6;
            us_print(episode);
        }
        if (SPEAR && high_scores[i].completed == 21)
            view_pic(printx + 8, printy - 1, PIC_C_WONSPEAR);
        else
            us_print(buffer);

        snprintf(buffer, sizeof buffer, "%ld", (long)high_scores[i].score);
        if (SPEAR)
            printx = 292 - font_measure(fontnumber, buffer);
        else {
            for (char *s = buffer; *s; s++)
                *s = (char)(*s + (129 - '0'));
            printx = 34 * 8 - 8 - font_measure(0, buffer);
        }
        us_print(buffer);
    }
    fontnumber = MENUFONT;
}

/* ------------------------------------------------------------------ */
/* US_LineInput(), with a pad instead of a keyboard                    */
/* ------------------------------------------------------------------ */

/* Up and down step the last letter through these, A adds a letter, B
 * erases one - or, on an empty name, is Esc - and Start is Enter. Held,
 * up, down and B repeat. The I-bar cursor blinks at the end of the name,
 * as USL_XORICursor() drew it. */
static const char name_chars[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-'!&";

#define LI_BLINK   35                       /* TickBase / 2 */
#define LI_DELAY   20                       /* before a held button repeats */
#define LI_REPEAT  4
#define LI_MAX     63

static struct {
    int  x, y, maxchars, maxwidth, color, back, font;
    char s[LI_MAX + 1];
    bool cursorvis, erase_held, footer;
    int  blink, held, repeat, erase_repeat;
} li;

static void line_input_draw(void)
{
    view_bar(li.x - 1, li.y, li.maxwidth + 12, font_height(li.font), li.back);
    font_draw(li.font, li.x, li.y, li.s, li.color);
    if (li.cursorvis)
        font_draw(li.font, li.x + font_measure(li.font, li.s) - 1, li.y, "\x80", li.color);
}

/* the strip at the foot of the screen, the typing one: up and down LETTER,
 * A ADD, B ERASE, START DONE. It is wider than the menus' one, so what it
 * covered goes back to the screen's BORDCOLOR when typing is over. */
#define TYPING_X  ((320 - 144) / 2)

static void line_input_footer(bool show)
{
    view_bar(0, 184, 320, 16, BORDCOLOR);
    if (show)
        view_pic(TYPING_X, 184, PIC_C_MOUSELBACK_TYPING);
}

void line_input_start(int x, int y, const char *text, int maxchars,
                      int maxwidth, int color, int back, bool footer, int font)
{
    memset(&li, 0, sizeof li);
    li.font = font;
    li.x = x;
    li.y = y;
    li.maxchars = maxchars < LI_MAX ? maxchars : LI_MAX;
    li.maxwidth = maxwidth;
    li.color = color;
    li.back = back;
    li.footer = footer;
    li.cursorvis = true;
    li.blink = LI_BLINK;
    if (text)                               /* li.s is zeroed, and longer */
        memcpy(li.s, text, strnlen(text, (size_t)li.maxchars));

    if (footer)
        line_input_footer(true);
    line_input_draw();
}

int line_input_update(int tics, const buttons_t *b)
{
    const int len = (int)strlen(li.s);
    const int dir = b->up ? 1 : b->down ? -1 : 0;
    const int n = (int)sizeof name_chars - 1;
    const bool erase = b->back || b->back_held;
    bool changed = false;
    int step = 0, result = LINE_BUSY;

    if (b->menu)                            /* Enter */
        result = LINE_DONE;

    if (dir != li.held) {
        li.held = dir;
        li.repeat = LI_DELAY;
        step = dir;
    } else if (dir && (li.repeat -= tics) <= 0) {
        li.repeat = LI_REPEAT;
        step = dir;
    }

    if (result != LINE_BUSY) {
        /* finished: nothing more to type */
    } else if (erase && !li.erase_held) {
        /* a fresh press of B: erase a letter, or leave when there is none */
        li.erase_repeat = LI_DELAY;
        if (len) {
            li.s[len - 1] = 0;
            changed = true;
        } else
            result = LINE_CANCELLED;
    } else if (erase) {
        if (len && (li.erase_repeat -= tics) <= 0) {
            li.erase_repeat = LI_REPEAT;
            li.s[len - 1] = 0;
            changed = true;
        }
    } else if (step) {
        if (!len) {
            li.s[0] = 'A';
            li.s[1] = 0;
        } else {
            const char *p = strchr(name_chars, li.s[len - 1]);
            const int i = p ? (int)(p - name_chars) : 1;
            li.s[len - 1] = name_chars[(i + step + n) % n];
        }
        changed = true;
    } else if (b->use || b->right) {
        /* US_LineInput() takes a character while the name is under both
         * its limits */
        if (len < li.maxchars && font_measure(li.font, li.s) < li.maxwidth) {
            li.s[len] = 'A';
            li.s[len + 1] = 0;
            changed = true;
        }
    } else if (b->left && len) {
        li.s[len - 1] = 0;
        changed = true;
    }
    li.erase_held = erase;

    if (result != LINE_BUSY) {
        li.cursorvis = false;
        line_input_draw();
        if (li.footer)
            line_input_footer(false);
        return result;
    }

    if (changed) {
        li.cursorvis = true;                /* a moved cursor shows at once */
        li.blink = LI_BLINK;
    } else if ((li.blink -= tics) <= 0) {
        li.cursorvis = !li.cursorvis;
        li.blink = LI_BLINK;
        changed = true;
    }
    if (changed)
        line_input_draw();
    return LINE_BUSY;
}

const char *line_input_text(void)
{
    return li.s;
}

/* ------------------------------------------------------------------ */
/* The screens                                                         */
/* ------------------------------------------------------------------ */

static bool ingame;

static void draw_main_menu(void)
{
    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_stripes(10);
    view_pic(84, 0, PIC_C_OPTIONS);
    draw_window(MENU_X - 8, MENU_Y - 3, MENU_W, MENU_H, BKGDCOLOR);

    /* In a game, "Back to Demo" becomes "Back to Game" and View Scores
     * becomes End Game, which CP_EndGame() runs. */
    if (ingame) {
        strcpy(main_menu[backtodemo].string, "Back to Game");
        main_menu[backtodemo].active = 2;
        strcpy(main_menu[viewscores].string, "End Game");
        main_menu[viewscores].active = 1;
        main_menu[viewscores].routine = false;
    } else {
        strcpy(main_menu[backtodemo].string, "Back to Demo");
        main_menu[backtodemo].active = 1;
        strcpy(main_menu[viewscores].string, "View Scores");
        main_menu[viewscores].active = 1;
        main_menu[viewscores].routine = true;
    }
    main_menu[savegame].active = ingame;

    main_items.amount = READ_THIS ? 10 : 9;
    for (int i = 0; i < main_items.amount; i++)
        main_shown[i] = main_menu[item_at(i)];
    draw_menu(&main_items, main_shown);
}

/* PrintLSEntry(): a slot's box, and its name or "- empty -" */
static void print_ls_entry(int w, int color)
{
    fontcolor = color;
    draw_outline(LSM_X + ls_items.indent, LSM_Y + w * 13,
                 LSM_W - ls_items.indent - 15, 11, color, color);
    printx = LSM_X + ls_items.indent + 2;
    printy = LSM_Y + w * 13 + 1;
    fontnumber = 0;
    us_print(save_slot_used(w) ? save_slot_name(w) : "      - empty -");
    fontnumber = MENUFONT;
}

/* TrackWhichGame(): the slot under the gun is highlighted */
static void track_which_game(int w)
{
    static int lastgameon;

    print_ls_entry(lastgameon, TEXTCOLOR);
    print_ls_entry(w, HIGHLIGHT);
    lastgameon = w;
}

/* DrawLoadSaveScreen(), less its MenuFadeIn() */
static void draw_load_save_screen(bool save)
{
    clear_mscreen();
    fontnumber = MENUFONT;
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_window(LSM_X - 10, LSM_Y - 5, LSM_W, LSM_H, BKGDCOLOR);
    draw_stripes(10);
    view_pic(60, 0, save ? PIC_C_SAVEGAME : PIC_C_LOADGAME);

    for (int i = 0; i < 10; i++)
        print_ls_entry(i, TEXTCOLOR);
    draw_menu(&ls_items, ls_menu);
}

/* DrawChangeView(): the border at this size over the view's area, and the
 * instructions on VIEWCOLOR where the status bar goes. DOS said "Use arrows
 * to size", "ENTER to accept" and "ESC to cancel". */
static void draw_change_view(int view)
{
    static const char *const lines[3] = {
        "Use the D-pad to size", "A to accept", "B to cancel"
    };

    status_fill(127);
    draw_play_border(view);
    for (int i = 0; i < 3; i++)
        font_draw_status(MENUFONT, 160 - font_measure(MENUFONT, lines[i]) / 2,
                         1 + i * 13, lines[i], HIGHLIGHT);
}

/* DrawLSAction(): "Loading..." or "Saving..." beside the disk */
static void draw_ls_action(bool save)
{
    draw_window(LSA_X, LSA_Y, LSA_W, LSA_H, TEXTCOLOR);
    draw_outline(LSA_X, LSA_Y, LSA_W, LSA_H, 0, HIGHLIGHT);
    view_pic(LSA_X + 8, LSA_Y + 5, PIC_C_DISKLOADING1);

    fontnumber = MENUFONT;
    fontcolor = 0;
    printx = LSA_X + 46;
    printy = LSA_Y + 13;
    us_print(save ? "Saving..." : "Loading...");
}

static void draw_new_episode(void)
{
    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_window(NE_X - 4, NE_Y - 4, NE_W + 8, NE_H + 8, BKGDCOLOR);
    fontcolor = READHCOLOR;
    printy = 2;
    windowx = 0;
    us_cprint("Which episode to play?");

    /* the episodes this data does not have are in the ordering color */
    for (int i = 0; i < 6; i++)
        newe_menu[2 * i].active = episode_selectable(i) ? 1 : 3;
    fontcolor = TEXTCOLOR;
    draw_menu(&newe_items, newe_menu);

    for (int i = 0; i < 6; i++)
        view_pic(NE_X + 32, NE_Y + i * 26, PIC_C_EPISODE1 + i);
}

static void draw_new_game_diff(int w)
{
    view_pic(NM_X + 185, NM_Y + 7, PIC_C_BABYMODE + w);
}

static void draw_new_game(void)
{
    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK);
    fontcolor = READHCOLOR;
    printx = NM_X + 20;
    printy = NM_Y - 32;
    if (SPEAR)                              /* a picture of the words */
        view_pic(printx, printy, PIC_C_HOWTOUGH);
    else
        us_print("How tough are you?");
    draw_window(NM_X - 5, NM_Y - 10, NM_W, NM_H, BKGDCOLOR);

    draw_menu(&new_items, new_menu);
    draw_new_game_diff(new_items.curpos);
}

static void draw_sound_menu(void)
{
    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_window(SM_X - 8, SM_Y1 - 3, SM_W, SM_H1, BKGDCOLOR);
    draw_window(SM_X - 8, SM_Y2 - 3, SM_W, SM_H2, BKGDCOLOR);
    draw_window(SM_X - 8, SM_Y3 - 3, SM_W, SM_H3, BKGDCOLOR);

    draw_menu(&snd_items, snd_menu);
    view_pic(100, SM_Y1 - 20, PIC_C_FXTITLE);
    view_pic(100, SM_Y2 - 20, PIC_C_DIGITITLE);
    view_pic(100, SM_Y3 - 20, PIC_C_MUSICTITLE);

    for (int i = 0; i < snd_items.amount; i++) {
        bool on = false;
        if (!snd_menu[i].string[0])
            continue;
        switch (i) {
        case 0:  on = !sd_effects_on; break;
        case 1:  on =  sd_effects_on; break;
        case 5:  on = !sd_digi_on;    break;
        case 6:  on =  sd_digi_on;    break;
        case 10: on = !sd_music_on;   break;
        case 11: on =  sd_music_on;   break;
        }
        view_pic(SM_X + 24, SM_Y1 + i * 13 + 2,
                 on ? PIC_C_SELECTED : PIC_C_NOTSELECTED);
    }

    view_pic(snd_items.x, snd_items.y + snd_items.curpos * 13 - 2, PIC_C_CURSOR1);
}

/* A section title in the style of the Sound menu's. Those are pictures - a
 * gray bar with a bevel and a word in blocky letters, 9 pixels wide and 8
 * tall with 3-pixel strokes - and their letters do not spell DEBUG or
 * CHEATS. So this draws MUSIC's bar, paints over its word in the bar's own
 * gray, and writes the new word in the same
 * letters: D, E, U, G, C, T and S copied from the three titles, and B, H and
 * A drawn to match. */
static const struct {
    char c;
    uint16_t rows[8];                       /* bit 8 is the leftmost column */
} title_glyphs[] = {
    { 'A', { 0x0fe, 0x1ff, 0x1c7, 0x1ff, 0x1ff, 0x1c7, 0x1c7, 0x1c7 } },
    { 'B', { 0x1fe, 0x1ff, 0x1c7, 0x1fe, 0x1fe, 0x1c7, 0x1ff, 0x1fe } },
    { 'C', { 0x0fe, 0x1ff, 0x1c7, 0x1c0, 0x1c0, 0x1c7, 0x1ff, 0x0fe } },
    { 'D', { 0x1fe, 0x1ff, 0x1c7, 0x1c7, 0x1c7, 0x1c7, 0x1ff, 0x1fe } },
    { 'E', { 0x1ff, 0x1ff, 0x1c0, 0x1f8, 0x1f8, 0x1c0, 0x1ff, 0x1ff } },
    { 'G', { 0x0ff, 0x1ff, 0x1c0, 0x1cf, 0x1cf, 0x1c7, 0x1ff, 0x0ff } },
    { 'H', { 0x1c7, 0x1c7, 0x1c7, 0x1ff, 0x1ff, 0x1c7, 0x1c7, 0x1c7 } },
    { 'S', { 0x0ff, 0x1ff, 0x1c0, 0x1fe, 0x0ff, 0x007, 0x1ff, 0x1fe } },
    { 'T', { 0x1ff, 0x1ff, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038 } },
    { 'U', { 0x1c7, 0x1c7, 0x1c7, 0x1c7, 0x1c7, 0x1c7, 0x1ff, 0x0fe } },
};

static void draw_title(int x, int y, const char *text)
{
    int w, h, fill, ink = -1;
    const uint8_t *pic = assets_pic(PIC_C_MUSICTITLE, &w, &h);
    const int n = (int)strlen(text);

    /* The 136x16 picture: two rows of the menu's red, the white top edge
     * (row 2), the gray inside (rows 3..12) with the word on rows 4..11, the
     * dark bottom edge (row 13), two red rows. */
    x &= ~7;                                /* where view_pic() puts it */
    view_pic(x, y, PIC_C_MUSICTITLE);
    if (!pic || h < 14)
        return;
    fill = pic[3 * w + 4];
    for (int xx = 2; xx < w - 2 && ink < 0; xx++)
        if (pic[7 * w + xx] != fill)
            ink = pic[7 * w + xx];
    if (ink < 0)
        return;
    view_bar(x + 2, y + 4, w - 4, 8, fill);

    for (int i = 0, px = x + (w - (n * 10 - 1)) / 2; i < n; i++, px += 10)
        for (int g = 0; g < (int)(sizeof title_glyphs / sizeof *title_glyphs); g++)
            if (title_glyphs[g].c == text[i]) {
                for (int r = 0; r < 8; r++)
                    for (int col = 0; col < 9; col++)
                        if (title_glyphs[g].rows[r] & (0x100 >> col))
                            view_bar(px + col, y + 4 + r, 1, 1, ink);
                break;
            }
}

static void draw_options_menu(void)
{
    const int lights[4][2] = {
        { OPT_ROW_FPS, opt_show_fps }, { OPT_ROW_PROFILER, opt_show_profiler },
        { OPT_ROW_GODMODE, opt_god_mode }, { OPT_ROW_NOCLIP, opt_no_clip }
    };

    /* the one-off cheats are for a game in progress */
    for (int i = OPT_ROW_MLI; i <= OPT_ROW_ENDFLOOR; i++)
        opt_menu[i].active = ingame;

    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_window(SM_X - 8, SM_Y1 - 3, SM_W, 3 * 13 - 7, BKGDCOLOR);
    draw_window(SM_X - 8, SM_Y2 - 3, SM_W,
                (OPT_ROWS - OPT_ROW_GODMODE + 1) * 13 - 7, BKGDCOLOR);

    draw_menu(&opt_items, opt_menu);
    draw_title(100, SM_Y1 - 20, "DEBUG");
    draw_title(100, SM_Y2 - 20, "CHEATS");

    for (int i = 0; i < 4; i++)
        view_pic(SM_X + 24, SM_Y1 + lights[i][0] * 13 + 2,
                 lights[i][1] ? PIC_C_SELECTED : PIC_C_NOTSELECTED);

    view_pic(opt_items.x, opt_items.y + opt_items.curpos * 13 - 2, PIC_C_CURSOR1);
}

/* Tab-W's question over the Extras menu, the floor chosen after it, and
 * the foot of the screen's strip with its arrows turned left and right,
 * the way the number moves - DOS typed it */
static void draw_warp(void)
{
    char s[48];

    draw_options_menu();
    snprintf(s, sizeof s, WARPSTR, WARP_FLOORS, warp_floor);
    message(s);
    view_pic(112, 184, PIC_C_MOUSELBACK_LR);
}

/* DrawCtlScreen(), the gun left for HandleMenu() */
static void draw_ctl_screen(void)
{
    clear_mscreen();
    draw_stripes(10);
    view_pic(80, 0, PIC_C_CONTROL);
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_window(CTL_X - 8, CTL_Y - 5, CTL_W, CTL_H, BKGDCOLOR);

    draw_menu(&ctl_items, ctl_menu);
    view_pic(CTL_X + ctl_items.indent - 24, CTL_Y + 3,
             ctl_always_run ? PIC_C_SELECTED : PIC_C_NOTSELECTED);
}

/* The slider of DrawMouseSens() and MouseSensitivity(), one notch a level */
static void draw_sens_bar(int v)
{
    view_bar(60, 97, 200, 10, TEXTCOLOR);
    draw_outline(60, 97, 200, 10, 0, HIGHLIGHT);
    draw_outline(60 + 20 * v, 97, 20, 10, 0, READCOLOR);
    view_bar(61 + 20 * v, 98, 19, 9, READHCOLOR);
}

/* DrawMouseSens(), less its MenuFadeIn() */
static void draw_stick_sens(void)
{
    clear_mscreen();
    view_pic(112, 184, PIC_C_MOUSELBACK_LR);  /* the slider goes sideways */
    draw_window(10, 80, 300, 30, BKGDCOLOR);

    windowx = 0;
    windoww = 320;
    printy = 82;
    fontcolor = READCOLOR;
    us_cprint("Adjust Stick Sensitivity");

    fontcolor = TEXTCOLOR;
    printx = 14;
    printy = 95;
    us_print("Slow");
    printx = 269;
    us_print("Fast");

    draw_sens_bar(ctl_stick_sensitivity);
}

static int cst_row_of(int action)
{
    int r = 0;
    while (action >= cst_row_first[r + 1])
        r++;
    return r;
}

/* How wide an action's column is: its label, or the widest input name - in
 * the menu font for a four-action row, in the small font for the first */
static int cst_width(int action)
{
    const int r = cst_row_of(action);
    const int n = cst_row_first[r + 1] - cst_row_first[r];
    const int lw = font_measure(MENUFONT, cst_labels[action]);
    const int nw = pad_name_w(n <= 4 ? MENUFONT : SMALLFONT);
    return lw > nw ? lw : nw;
}

/* Where an action's column starts, and (w) its width. DOS set its four
 * columns 60 pixels apart (CST_START, CST_SPC); here each row's columns are
 * spread evenly from CST_LEFT to CST_RIGHT, which comes to 69 apart for a
 * four-action row. */
static int cst_column(int action, int *w)
{
    const int r = cst_row_of(action), first = cst_row_first[r];
    const int n = cst_row_first[r + 1] - first;
    int x = CST_LEFT, total = 0, gap;

    for (int j = first; j < first + n; j++)
        total += cst_width(j);
    gap = n > 1 ? (CST_RIGHT - CST_LEFT - total) / (n - 1) : 0;
    for (int j = first; j < action; j++)
        x += cst_width(j) + gap;
    if (w)
        *w = cst_width(action);
    return x;
}

/* DrawCustJoy() and the rest: a row's inputs, lit while the gun is on it */
static void draw_cust_row(int r, int hilight)
{
    fontcolor = hilight ? HIGHLIGHT : TEXTCOLOR;
    printy = CST_ROW(r);
    if (r == CST_ROWS) {                    /* Reset controls */
        printx = CST_LEFT;
        us_print(CST_RESET_STR);
        return;
    }
    for (int i = cst_row_first[r]; i < cst_row_first[r + 1]; i++) {
        int w;
        const int x = cst_column(i, &w);
        draw_pad_name(x, CST_ROW(r), w, pad_names[ctl_button[i]], fontcolor);
    }
}

/* FixupCustom(): the lines of the row's window, and what lies above and
 * below it, where the gun's box was drawn over them; the row lit, and the
 * one the gun left put back */
static void fixup_row_lines(int r)
{
    hlin(7, 32, CST_ROW(r) - 1, DEACTIVE);
    hlin(7, 32, CST_ROW(r) + 12, BORD2COLOR);
    hlin(7, 32, CST_ROW(r) - 2, BORDCOLOR);
    hlin(7, 32, CST_ROW(r) + 13, BORDCOLOR);
}

static int cst_lastrow = -1;

static void fixup_custom(int w)
{
    const int r = w / 2;                    /* items 0, 2, 4, and 6 (reset) */

    fixup_row_lines(r);
    draw_cust_row(r, 1);
    if (cst_lastrow >= 0 && cst_lastrow != r) {
        fixup_row_lines(cst_lastrow);
        draw_cust_row(cst_lastrow, 0);
    }
    cst_lastrow = r;
}


/* DrawCustomScreen(), with the one section */
static void draw_custom_screen(void)
{
    clear_mscreen();
    windowx = 0;
    windoww = 320;
    view_pic(112, 184, PIC_C_MOUSELBACK);
    draw_stripes(10);
    view_pic(80, 0, PIC_C_CUSTOMIZE);

    fontcolor = READCOLOR;
    printy = CST_Y;
    us_cprint("Nintendo 64 Controller");

    for (int r = 0; r < CST_ROWS; r++) {
        fontcolor = TEXTCOLOR;
        for (int i = cst_row_first[r]; i < cst_row_first[r + 1]; i++) {
            printx = cst_column(i, NULL);
            printy = CST_ROW(r) - 13;
            us_print(cst_labels[i]);
        }
        draw_window(5, CST_ROW(r) - 1, 310, 13, BKGDCOLOR);
        draw_cust_row(r, 0);
    }
    draw_window(5, CST_ROW(CST_ROWS) - 1, 310, 13, BKGDCOLOR);
    draw_cust_row(CST_ROWS, 0);
    cst_lastrow = -1;
}

/* EnterCtrlData()'s redraw: the row, with a box round the chosen action */
static void draw_cust_edit(int which)
{
    int w;
    const int x = cst_column(which, &w), r = cst_row_of(which);

    draw_window(5, CST_ROW(r) - 1, 310, 13, BKGDCOLOR);
    draw_cust_row(r, 1);
    draw_window(x - 3, CST_ROW(r), w + 6, 11, TEXTCOLOR);
    draw_outline(x - 3, CST_ROW(r), w + 6, 11, 0, HIGHLIGHT);
    draw_pad_name(x, CST_ROW(r), w, pad_names[ctl_button[which]], 0);
}

/* ------------------------------------------------------------------ */
/* HandleMenu(), a frame at a time                                     */
/* ------------------------------------------------------------------ */

#define HANDLE_BUSY  -2

static struct {
    iteminfo_t   *info;
    const item_t *items;
    void        (*routine)(int w);
    int  which, x, y, basey;
    int  shape, timer, count;               /* the gun's flicker */
    enum { H_IDLE, H_HALFSTEP, H_DELAY } sub;
    int  subcount, dir;
} hm;

static int redrawitem = 1, lastitem = -1;   /* HandleMenu()'s statics */

static void print_item(int which)
{
    printx = hm.info->x + hm.info->indent;
    printy = hm.info->y + which * 13;
    us_print(hm.items[which].string);
}

static void handle_begin(iteminfo_t *info, const item_t *items,
                         void (*routine)(int w))
{
    hm.info    = info;
    hm.items   = items;
    hm.routine = routine;

    /* The gun is left where the last choice was. In DOS that item is
     * always still active; here End Game turns back into an inactive View
     * Scores when the game ends, so move on to the next active item. */
    while (!items[info->curpos].active)
        info->curpos = (info->curpos + 1) % info->amount;
    hm.which   = info->curpos;
    hm.x       = info->x & -8;
    hm.basey   = info->y - 2;
    hm.y       = hm.basey + hm.which * 13;

    view_pic(hm.x, hm.y, PIC_C_CURSOR1);
    set_text_color(&items[hm.which], true);
    if (redrawitem)
        print_item(hm.which);
    if (routine)
        routine(hm.which);

    hm.shape = PIC_C_CURSOR1;
    hm.timer = 8;
    hm.count = 0;
    hm.sub   = H_IDLE;
}

/* EraseGun() */
static void erase_gun(void)
{
    view_bar(hm.x - 1, hm.y, 25, 16, BKGDCOLOR);
    set_text_color(&hm.items[hm.which], false);
    print_item(hm.which);
}

/* DrawGun() */
static void draw_gun(void)
{
    view_bar(hm.x - 1, hm.y, 25, 16, BKGDCOLOR);
    hm.y = hm.basey + hm.which * 13;
    view_pic(hm.x, hm.y, PIC_C_CURSOR1);
    set_text_color(&hm.items[hm.which], true);
    print_item(hm.which);
    if (hm.routine)
        hm.routine(hm.which);
    sd_play_sound(MOVEGUN2SND);
}

static void step_to_next(void)
{
    const int amount = hm.info->amount;
    do {
        if (hm.dir < 0)
            hm.which = hm.which ? hm.which - 1 : amount - 1;
        else
            hm.which = hm.which == amount - 1 ? 0 : hm.which + 1;
    } while (!hm.items[hm.which].active);
    draw_gun();

    /* TicDelay(20): wait, or stop waiting once the stick is let go */
    hm.sub = H_DELAY;
    hm.subcount = 20;
}

/* Returns HANDLE_BUSY, -1 for Esc, or the item chosen. */
static int handle_update(int tics, const buttons_t *b)
{
    int exit = 0;

    switch (hm.sub) {
    case H_HALFSTEP:                        /* DrawHalfStep()'s 8-tic wait */
        if ((hm.subcount -= tics) <= 0)
            step_to_next();
        return HANDLE_BUSY;

    case H_DELAY:
        if ((hm.subcount -= tics) <= 0 || !(b->up || b->down))
            hm.sub = H_IDLE;
        return HANDLE_BUSY;

    case H_IDLE:
        break;
    }

    /* change gun shape: eight tics lit, then 70 dark */
    hm.count += tics;
    if (hm.count > hm.timer) {
        hm.count = 0;
        if (hm.shape == PIC_C_CURSOR1) {
            hm.shape = PIC_C_CURSOR2;
            hm.timer = 8;
        } else {
            hm.shape = PIC_C_CURSOR1;
            hm.timer = 70;
        }
        view_pic(hm.x, hm.y, hm.shape);
        if (hm.routine)
            hm.routine(hm.which);
    }

    if (b->up || b->down) {
        const int amount = hm.info->amount;
        hm.dir = b->up ? -1 : 1;
        erase_gun();

        /* animate a half-step if the next item is right there */
        if (hm.dir < 0 ? hm.which && hm.items[hm.which - 1].active
                       : hm.which != amount - 1 && hm.items[hm.which + 1].active) {
            hm.y += hm.dir * 6;
            view_pic(hm.x, hm.y, PIC_C_CURSOR1);
            sd_play_sound(MOVEGUN1SND);
            hm.sub = H_HALFSTEP;
            hm.subcount = 8;
        } else
            step_to_next();
        return HANDLE_BUSY;
    }

    if (b->use || b->menu)                  /* A or Start: Enter */
        exit = 1;
    if (b->back)
        exit = 2;
    if (!exit)
        return HANDLE_BUSY;

    /* erase everything */
    if (lastitem != hm.which) {
        view_bar(hm.x - 1, hm.y, 25, 16, BKGDCOLOR);
        print_item(hm.which);
        redrawitem = 1;
    } else
        redrawitem = 0;
    if (hm.routine)
        hm.routine(hm.which);

    hm.info->curpos = hm.which;
    lastitem = hm.which;

    if (exit == 2) {
        sd_play_sound(ESCPRESSEDSND);
        return -1;
    }
    if (hm.items[hm.which].routine)
        sd_play_sound(SHOOTSND);            /* then MenuFadeOut(), below */
    return hm.which;
}

/* ------------------------------------------------------------------ */
/* The control panel's flow                                            */
/* ------------------------------------------------------------------ */

enum {
    S_FADE,                                 /* waiting on a fade           */
    S_HANDLE,                               /* HandleMenu() on a screen    */
    S_MESSAGE,                              /* Message(); IN_Ack()         */
    S_CONFIRM,                              /* Confirm()                   */
    S_ARTICLE,                              /* HelpScreens()               */
    S_SCORES,                               /* CP_ViewScores()'s IN_Ack()  */
    S_LSACTION,                             /* the disk turning            */
    S_NAME,                                 /* US_LineInput() for a save   */
    S_CHANGEVIEW,                           /* CP_ChangeView()'s loop      */
    S_SENS,                                 /* MouseSensitivity()'s loop   */
    S_CUSTOM,                               /* EnterCtrlData()'s loop      */
    S_WARP,                                 /* Tab-W's "which level"       */
    S_DONE
};

enum { SCR_MAIN, SCR_EPISODE, SCR_SKILL, SCR_SOUND, SCR_LOAD, SCR_SAVE,
       SCR_CHANGEVIEW, SCR_OPTIONS, SCR_CONTROL, SCR_SENS, SCR_CUSTOM };

enum {
    A_MAIN,             /* DrawMainMenu(); MenuFadeIn(); HandleMenu      */
    A_EPISODE,          /* DrawNewEpisode() (fades in); HandleMenu       */
    A_SKILL,            /* DrawNewGame() (fades in); HandleMenu          */
    A_SOUND,            /* DrawSoundMenu(); MenuFadeIn(); HandleMenu     */
    A_READTHIS,         /* CP_ReadThis()                                 */
    A_READTHIS_DONE,    /* StartCPMusic(MENUSONG); back to the main menu */
    A_HANDLE,           /* HandleMenu on the screen as it stands         */
    A_CURGAME,          /* after Confirm(CURGAME)                        */
    A_ENDGAME,          /* after Confirm(ENDGAMESTR)                     */
    A_SCORES,           /* CP_ViewScores()                               */
    A_SCORES_DONE,      /* StartCPMusic(MENUSONG); MenuFadeOut()         */
    A_LOAD,             /* DrawLoadSaveScreen(0) (fades in); HandleMenu  */
    A_LOAD_READY,       /* after the disk: ShootSnd(); MenuFadeOut()     */
    A_SAVE,             /* DrawLoadSaveScreen(1) (fades in); HandleMenu  */
    A_SAVE_OVERWRITE,   /* after Confirm(GAMESVD)                        */
    A_SAVE_NAME,        /* the slot takes its name; DrawLSAction(1)      */
    A_SAVE_WRITE,       /* SaveTheGame()                                 */
    A_CHANGEVIEW,       /* DrawChangeView() (fades in); the sizing loop  */
    A_OPTIONS,          /* the Extras menu; MenuFadeIn(); HandleMenu     */
    A_CONTROL,          /* DrawCtlScreen(); MenuFadeIn(); HandleMenu     */
    A_SENS,             /* DrawMouseSens(); MenuFadeIn(); the slider     */
    A_CUSTOM,           /* DrawCustomScreen() (fades in); HandleMenu     */
    A_CUSTOM_RESET,     /* after Confirm(RESETSTR)                       */
    A_OPTIONS_BACK,     /* a cheat's message gone: the Extras menu again */
    A_FINISH            /* US_ControlPanel() returns                     */
};

static int  state, screen, after, result, difficulty, episode;
static bool confirmed;
static int  confirm_x, confirm_y, confirm_tick, confirm_count;
static int  ls_slot, ls_tics;
static int  cv_old, cv_new, cv_delay;       /* CP_ChangeView() */
static int  sens_old, sens_delay;           /* MouseSensitivity() */
static int  cst_which, cst_dir, cst_tick, cst_count;    /* EnterCtrlData() */
static bool cst_picking;
static char ls_name[SAVE_NAME_LEN + 1];

static void wait_fade(int action)
{
    state = S_FADE;
    after = action;
}

static void menu_fade_out(int action)
{
    fade_out(SPEAR ? FADE_MENUBLUE : FADE_MENURED, MENUFADE);
    wait_fade(action);
}

static void run(int action);

static void confirm(const char *string, int action)
{
    message(string);
    confirm_x = printx;
    confirm_y = printy;
    confirm_tick = confirm_count = 0;
    state = S_CONFIRM;
    after = action;
}

static void begin_handle(void)
{
    state = S_HANDLE;
    switch (screen) {
    case SCR_MAIN:    handle_begin(&main_items, main_shown, NULL); break;
    case SCR_EPISODE: handle_begin(&newe_items, newe_menu, NULL); break;
    case SCR_SKILL:   handle_begin(&new_items, new_menu, draw_new_game_diff); break;
    case SCR_SOUND:   handle_begin(&snd_items, snd_menu, NULL); break;
    case SCR_OPTIONS: handle_begin(&opt_items, opt_menu, NULL); break;
    case SCR_CONTROL: handle_begin(&ctl_items, ctl_menu, NULL); break;
    case SCR_CUSTOM:  handle_begin(&cus_items, cus_menu, fixup_custom); break;
    case SCR_LOAD:
    case SCR_SAVE:    handle_begin(&ls_items, ls_menu, track_which_game); break;
    }
}

/* DrawLSAction(), then the disk flops for a moment before `action` */
static void ls_action(bool save, int action)
{
    draw_ls_action(save);
    ls_tics = 0;
    state = S_LSACTION;
    after = action;
}

/* Message(), and IN_Ack() before `action` */
static void message_then(const char *string, int action)
{
    message(string);
    state = S_MESSAGE;
    after = action;
}

static void run(int action)
{
    switch (action) {
    case A_MAIN:
        screen = SCR_MAIN;
        draw_main_menu();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_EPISODE:
        screen = SCR_EPISODE;
        draw_new_episode();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_SKILL:
        screen = SCR_SKILL;
        draw_new_game();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_SOUND:
        screen = SCR_SOUND;
        draw_sound_menu();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_OPTIONS:
        screen = SCR_OPTIONS;
        draw_options_menu();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_CONTROL:
        screen = SCR_CONTROL;
        draw_ctl_screen();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_SENS:
        screen = SCR_SENS;
        sens_old = ctl_stick_sensitivity;
        sens_delay = 0;
        draw_stick_sens();
        fade_in(MENUFADE);
        state = S_FADE;
        after = -4;                         /* then S_SENS */
        break;

    case A_CUSTOM:
        screen = SCR_CUSTOM;
        draw_custom_screen();
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_CUSTOM_RESET:
        /* the box goes, and the rows show the defaults if it said yes */
        if (confirmed) {
            uint8_t was[CTL_PACKED_BYTES], now[CTL_PACKED_BYTES];
            ctl_pack_buttons(was);
            ctl_default_buttons();
            ctl_pack_buttons(now);
            if (memcmp(was, now, sizeof was))
                save_config();
        }
        draw_custom_screen();
        begin_handle();
        break;

    case A_OPTIONS_BACK:
        draw_options_menu();
        begin_handle();
        break;

    case A_READTHIS:
        sd_start_music(CORNER_MUS);
        article_start(wolf_help_article);
        fade_in(MENUFADE);                  /* ShowArticle()'s first page */
        state = S_FADE;
        after = -1;                         /* then S_ARTICLE */
        break;

    case A_READTHIS_DONE:
        sd_start_music(WONDERIN_MUS);
        run(A_MAIN);
        break;

    case A_SCORES:
        sd_start_music(ROSTER_MUS);
        draw_high_scores();
        fade_in(MENUFADE);
        state = S_FADE;
        after = -2;                         /* then S_SCORES */
        break;

    case A_SCORES_DONE:
        sd_start_music(WONDERIN_MUS);
        menu_fade_out(A_MAIN);
        break;

    case A_LOAD:
    case A_SAVE:
        screen = action == A_LOAD ? SCR_LOAD : SCR_SAVE;
        draw_load_save_screen(action == A_SAVE);
        fade_in(MENUFADE);
        wait_fade(A_HANDLE);
        break;

    case A_LOAD_READY:
        /* CP_LoadGame(): StartGame, and "Read This!" to normal color.
         * game.c loads the slot once the screen has faded. */
        sd_play_sound(SHOOTSND);
        main_menu[readthis].active = 1;
        result = MENU_LOAD_GAME;
        menu_fade_out(A_FINISH);
        break;

    case A_SAVE_OVERWRITE:
        draw_load_save_screen(true);
        if (confirmed) {
            print_ls_entry(ls_slot, HIGHLIGHT);
            run(A_SAVE_NAME);
        } else
            begin_handle();
        break;

    case A_SAVE_NAME:
        /* US_LineInput(), in the slot, starting from the name already
         * there - blank for an empty slot */
        sd_play_sound(SHOOTSND);
        snprintf(ls_name, sizeof ls_name, "%s", save_slot_name(ls_slot));
        line_input_start(LSM_X + ls_items.indent + 2, LSM_Y + ls_slot * 13 + 1,
                         ls_name, SAVE_NAME_LEN - 1,
                         LSM_W - ls_items.indent - 30, HIGHLIGHT, BKGDCOLOR,
                         true, 0);
        state = S_NAME;
        break;

    case A_SAVE_WRITE:
        if (save_game(ls_slot, ls_name)) {
            sd_play_sound(SHOOTSND);
            menu_fade_out(A_MAIN);
        } else {
            sd_play_sound(NOWAYSND);
            message_then(NOSPACE, A_SAVE);
        }
        break;

    case A_CHANGEVIEW:
        screen = SCR_CHANGEVIEW;
        cv_old = cv_new = view_size;
        cv_delay = 0;
        draw_change_view(cv_old);
        fade_in(MENUFADE);
        state = S_FADE;
        after = -3;                         /* then S_CHANGEVIEW */
        break;

    case A_HANDLE:
        begin_handle();
        break;

    case A_CURGAME:
        if (confirmed)
            menu_fade_out(A_SKILL);
        else
            menu_fade_out(A_MAIN);
        break;

    case A_ENDGAME:
        /* CP_EndGame(), then DrawMainMenu(); MenuFadeIn() either way */
        screen = SCR_MAIN;
        draw_main_menu();
        if (confirmed) {
            result = MENU_END_GAME;
            state = S_DONE;
        } else
            begin_handle();
        break;

    case A_FINISH:
        state = S_DONE;
        break;
    }
}

static void select_item(int which)
{
    switch (screen) {
    case SCR_MAIN:
        switch (which < 0 ? which : item_at(which)) {
        case -1:
            /* Esc here asks to quit to DOS; on the pad, B in a game goes
             * back to it, and otherwise stays put */
            if (ingame) {
                result = MENU_RESUME;
                fade_out(FADE_BLACK, MENUFADE);
                wait_fade(A_FINISH);
            } else
                begin_handle();
            break;
        case newgame:
            if (wolf_version != WV_SPEAR)
                menu_fade_out(A_EPISODE);
            else {
                /* CP_NewGame() for Spear of Destiny: the skill at once */
                episode = 0;
                if (ingame)
                    confirm(CURGAME, A_CURGAME);
                else
                    menu_fade_out(A_SKILL);
            }
            break;
        case soundmenu:
            menu_fade_out(A_SOUND);
            break;
        case control:
            menu_fade_out(A_CONTROL);
            break;
        case readthis:
            menu_fade_out(A_READTHIS);
            break;
        case loadgame:
            menu_fade_out(A_LOAD);
            break;
        case savegame:
            menu_fade_out(A_SAVE);
            break;
        case changeview:
            menu_fade_out(A_CHANGEVIEW);
            break;
        case options:
            menu_fade_out(A_OPTIONS);
            break;
        case viewscores:
            if (ingame)                     /* End Game */
                confirm(ENDGAMESTR, A_ENDGAME);
            else
                menu_fade_out(A_SCORES);
            break;
        case backtodemo:                    /* Back to Game, or Demo */
            result = ingame ? MENU_RESUME : MENU_BACK_TO_DEMO;
            if (!ingame)
                sd_start_music(NAZI_NOR_MUS);   /* StartCPMusic(INTROSONG) */
            fade_out(FADE_BLACK, MENUFADE); /* VL_FadeOut(0,255,0,0,0,10) */
            wait_fade(A_FINISH);
            break;
        default:
            begin_handle();
        }
        break;

    case SCR_EPISODE:
        if (which < 0) {
            menu_fade_out(A_MAIN);
        } else if (!episode_selectable(which / 2)) {
            sd_play_sound(NOWAYSND);
            message_then("Please select \"Read This!\"\n"
                         "from the Options menu to\n"
                         "find out how to order this\n"
                         "episode from Apogee.", A_EPISODE);
        } else {
            sd_play_sound(SHOOTSND);
            episode = which / 2;
            if (ingame)
                confirm(CURGAME, A_CURGAME);
            else
                menu_fade_out(A_SKILL);
        }
        break;

    case SCR_SKILL:
        if (which < 0) {
            menu_fade_out(wolf_version == WV_SPEAR ? A_MAIN : A_EPISODE);
        } else {
            sd_play_sound(SHOOTSND);
            difficulty = which;
            main_menu[readthis].active = 1; /* "Read This!" to normal color */
            result = MENU_NEW_GAME;
            menu_fade_out(A_FINISH);
        }
        break;

    case SCR_SOUND: {
        const bool was[3] = { sd_effects_on, sd_digi_on, sd_music_on };
        switch (which) {
        case -1:
            menu_fade_out(A_MAIN);
            return;
        case 0:
            if (sd_effects_on) {
                sd_set_effects(false);
                draw_sound_menu();
            }
            break;
        case 1:
            if (!sd_effects_on) {
                sd_set_effects(true);
                draw_sound_menu();
                sd_play_sound(SHOOTSND);
            }
            break;
        case 5:
            if (sd_digi_on) {
                sd_set_digi(false);
                draw_sound_menu();
            }
            break;
        case 6:
            if (!sd_digi_on) {
                sd_set_digi(true);
                draw_sound_menu();
                sd_play_sound(SHOOTSND);
            }
            break;
        case 10:
            if (sd_music_on) {
                sd_set_music(false);
                draw_sound_menu();
                sd_play_sound(SHOOTSND);
            }
            break;
        case 11:
            if (!sd_music_on) {
                sd_set_music(true);
                draw_sound_menu();
                sd_play_sound(SHOOTSND);
                sd_start_music(WONDERIN_MUS);
            }
            break;
        }
        if (was[0] != sd_effects_on || was[1] != sd_digi_on || was[2] != sd_music_on)
            save_config();                  /* WriteConfig() */
        begin_handle();
        break;
    }

    case SCR_OPTIONS:
        /* a switch turns on or off, and is saved at once, as the Sound
         * menu's settings are; a one-off cheat does its DOS work there and
         * then, with the DOS message */
        switch (which) {
        case -1:
            menu_fade_out(A_MAIN);
            return;
        case OPT_ROW_FPS:      opt_show_fps      = !opt_show_fps;      break;
        case OPT_ROW_PROFILER: opt_show_profiler = !opt_show_profiler; break;
        case OPT_ROW_GODMODE:  opt_god_mode      = !opt_god_mode;      break;
        case OPT_ROW_NOCLIP:   opt_no_clip       = !opt_no_clip;       break;

        case OPT_ROW_MLI:
            cheat_mli();
            message_then(MLISTR, A_OPTIONS_BACK);
            return;
        case OPT_ROW_ITEMS:
            cheat_free_items();
            message_then(ITEMSSTR, A_OPTIONS_BACK);
            return;
        case OPT_ROW_WARP:
            /* "Warp to which level(1-10):", the floor it is on to begin */
            sd_play_sound(SHOOTSND);
            warp_floor = level_mapon(current_level) + 1;
            warp_delay = 0;
            draw_warp();
            state = S_WARP;
            return;
        case OPT_ROW_ENDFLOOR:
            /* playstate = ex_completed: out of the menu, to the stats */
            sd_play_sound(SHOOTSND);
            result = MENU_END_FLOOR;
            menu_fade_out(A_FINISH);
            return;
        }
        save_config();
        draw_options_menu();
        sd_play_sound(SHOOTSND);
        begin_handle();
        break;

    case SCR_CONTROL:
        switch (which) {
        case -1:
            menu_fade_out(A_MAIN);
            break;
        case 0:                             /* a light, as MOUSEENABLE was */
            ctl_always_run = !ctl_always_run;
            save_config();
            draw_ctl_screen();
            sd_play_sound(SHOOTSND);
            begin_handle();
            break;
        case 1:
            menu_fade_out(A_SENS);
            break;
        case 2:
            menu_fade_out(A_CUSTOM);
            break;
        }
        break;

    case SCR_CUSTOM:
        if (which < 0) {
            menu_fade_out(A_CONTROL);
        } else if (which == CST_RESET_ITEM) {
            confirm(RESETSTR, A_CUSTOM_RESET);
        } else {
            /* EnterCtrlData(): the gun goes and the row's first action is
             * boxed; the foot of the screen shows the box moving sideways */
            const int r = which / 2;
            sd_play_sound(SHOOTSND);
            hlin(7, 32, CST_ROW(r) - 2, BORDCOLOR);
            hlin(7, 32, CST_ROW(r) + 13, BORDCOLOR);
            cst_which = cst_row_first[r];
            cst_dir = 0;
            cst_picking = false;
            draw_cust_edit(cst_which);
            view_pic(112, 184, PIC_C_MOUSELBACK_LR);
            state = S_CUSTOM;
        }
        break;

    case SCR_LOAD:
        /* CP_LoadGame(): an empty slot does nothing */
        if (which < 0)
            menu_fade_out(A_MAIN);
        else if (save_slot_used(which)) {
            sd_play_sound(SHOOTSND);
            ls_slot = which;
            ls_action(false, A_LOAD_READY);
        } else
            begin_handle();
        break;

    case SCR_SAVE:
        /* CP_SaveGame(): a used slot asks first */
        if (which < 0)
            menu_fade_out(A_MAIN);
        else {
            ls_slot = which;
            if (save_slot_used(which))
                confirm(GAMESVD, A_SAVE_OVERWRITE);
            else
                run(A_SAVE_NAME);
        }
        break;
    }
}

int menu_load_slot(void)
{
    return ls_slot;
}

int menu_warp_floor(void)
{
    return level_episode(current_level) * 10 + warp_floor - 1;
}

/* Every menu screen is a 320x200 page, but Change View shows the view's
 * area at its own size, with the status bar's place below it. */
bool menu_fullscreen(void)
{
    return screen != SCR_CHANGEVIEW;
}

void menu_open(bool in_game)
{
    ingame = in_game;
    result = MENU_BUSY;
    /* Outside a game - at start-up, after a game over, from the demos - the
     * gun starts on New Game. (The shareware DOS menu started it on Read
     * This!, STARTITEM, and then left it wherever it was last.) In a game it
     * stays where it was, as in DOS. */
    if (!in_game)
        main_items.curpos = newgame;
    sd_start_music(WONDERIN_MUS);           /* StartCPMusic(MENUSONG) */
    run(A_MAIN);
}

int menu_difficulty(void)
{
    return difficulty;
}

int menu_episode(void)
{
    return episode;
}

int menu_update(int tics, const buttons_t *b)
{
    const bool ack = b->use || b->back || b->menu || b->pause;
    int which;

    switch (state) {
    case S_FADE:
        if (fade_update(tics)) {
            if (after == -1)
                state = S_ARTICLE;
            else if (after == -2)
                state = S_SCORES;
            else if (after == -3)
                state = S_CHANGEVIEW;
            else if (after == -4)
                state = S_SENS;
            else
                run(after);
        }
        break;

    case S_SCORES:
        if (ack)                            /* IN_Ack() */
            run(A_SCORES_DONE);
        break;

    case S_HANDLE:
        which = handle_update(tics, b);
        if (which != HANDLE_BUSY)
            select_item(which);
        break;

    case S_MESSAGE:
        if (ack)
            run(after);                     /* DrawNewEpisode(), and so on */
        break;

    case S_CHANGEVIEW: {
        /* ReadAnyControl(): up or right grows the view, down or left shrinks
         * it, on the D-pad or the stick, and held it keeps going: TicDelay(10)
         * holds off the next step until 10 tics have gone or the direction
         * is let go */
        const bool grow = b->up || b->right_held;
        const bool shrink = b->down || b->left_held;
        const int dir = grow ? 1 : shrink ? -1 : 0;

        if (cv_delay > 0) {
            if (grow || shrink)
                cv_delay -= tics;
            else
                cv_delay = 0;
        } else if (dir) {
            cv_new += dir;
            if (cv_new < VIEW_SIZE_MIN) cv_new = VIEW_SIZE_MIN;
            if (cv_new > VIEW_SIZE_MAX) cv_new = VIEW_SIZE_MAX;
            draw_play_border(cv_new);       /* ShowViewSize() */
            sd_play_sound(HITWALLSND);
            cv_delay = 10;
        } else if (b->use || b->menu) {
            if (cv_new != cv_old) {
                sd_play_sound(SHOOTSND);
                message("Thinking...");
                set_view_size(cv_new);      /* NewViewSize() */
                save_config();              /* WriteConfig() */
            }
            sd_play_sound(SHOOTSND);
            menu_fade_out(A_MAIN);
        } else if (b->back) {
            sd_play_sound(ESCPRESSEDSND);   /* the old size stays */
            menu_fade_out(A_MAIN);
        }
        break;
    }

    case S_SENS: {
        /* MouseSensitivity(): up or left slows, down or right speeds up, as
         * ci.dir did. DOS waited for the key to come up after each notch;
         * here a held direction keeps going, a notch every 10 tics, as in
         * Change View. A keeps the setting and B puts the old one back. */
        const bool slower = b->up || b->left_held;
        const bool faster = b->down || b->right_held;
        const int dir = slower ? -1 : faster ? 1 : 0;

        if (sens_delay > 0) {
            if (dir)
                sens_delay -= tics;
            else
                sens_delay = 0;
        } else if (dir) {
            const int v = ctl_stick_sensitivity + dir;
            if (v >= 0 && v <= CTL_SENS_MAX) {
                ctl_stick_sensitivity = v;
                draw_sens_bar(v);
                sd_play_sound(MOVEGUN1SND);
            }
            sens_delay = 10;
        } else if (b->use || b->menu) {
            sd_play_sound(SHOOTSND);
            if (ctl_stick_sensitivity != sens_old)
                save_config();
            menu_fade_out(A_CONTROL);
        } else if (b->back) {
            ctl_stick_sensitivity = sens_old;
            sd_play_sound(ESCPRESSEDSND);
            menu_fade_out(A_CONTROL);
        }
        break;
    }

    case S_WARP: {
        /* up or right one floor on, down or left one back, held repeating
         * as in Change View; A warps (ex_warped), B leaves it */
        const bool more = b->up || b->right_held;
        const bool less = b->down || b->left_held;

        if (warp_delay > 0) {
            if (more || less)
                warp_delay -= tics;
            else
                warp_delay = 0;
        } else if (more || less) {
            const int n = warp_floor + (more ? 1 : -1);
            if (n >= 1 && n <= WARP_FLOORS
                && level_episode(current_level) * 10 + n <= wolf_num_levels) {
                warp_floor = n;
                draw_warp();
                sd_play_sound(MOVEGUN1SND);
            }
            warp_delay = 10;
        } else if (b->use || b->menu) {
            sd_play_sound(SHOOTSND);
            result = MENU_WARP;
            menu_fade_out(A_FINISH);
        } else if (b->back) {
            sd_play_sound(ESCPRESSEDSND);
            run(A_OPTIONS_BACK);
        }
        break;
    }

    case S_CUSTOM: {
        const int r = cst_row_of(cst_which);
        const int dir = b->left_held ? -1 : b->right_held ? 1 : 0;

        if (cst_picking) {
            /* the box flashes a "?" until an input is pressed for the
             * action. It takes that input, and the action that had it
             * takes this one's old input: DOS left that action with none,
             * but here nothing else could do it. Pressing the input it has
             * already leaves it as it was, as Esc did in DOS. */
            if (b->pad_pressed) {
                int nb = 0, old = ctl_button[cst_which];
                while (!(b->pad_pressed & (1u << nb)))
                    nb++;
                for (int i = 0; i < CTL_ACTIONS; i++)
                    if (ctl_button[i] == nb)
                        ctl_button[i] = old;
                ctl_button[cst_which] = nb;
                if (old != nb)
                    save_config();
                sd_play_sound(SHOOTDOORSND);
                for (int o = 0; o < CST_ROWS; o++)  /* the swap may be there */
                    if (o != r) {
                        draw_window(5, CST_ROW(o) - 1, 310, 13, BKGDCOLOR);
                        draw_cust_row(o, 0);
                    }
            } else {
                if ((cst_count += tics) > 10) {
                    int w;
                    const int x = cst_column(cst_which, &w);
                    if (cst_tick == 0)
                        view_bar(x - 2, CST_ROW(r) + 1, w + 4, 10, TEXTCOLOR);
                    else {
                        draw_pad_name(x, CST_ROW(r), w, "?", 0);
                        sd_play_sound(HITWALLSND);
                    }
                    cst_tick ^= 1;
                    cst_count = 0;
                }
                break;
            }
            cst_picking = false;
            cst_dir = dir;                  /* a D-pad input does not move */
            draw_cust_edit(cst_which);
            break;
        }

        /* a press a step, as DOS waited for the direction to come up; round
         * the row, as DOS went round its four */
        if (dir && dir != cst_dir) {
            const int first = cst_row_first[r];
            const int n = cst_row_first[r + 1] - first;
            cst_which = first + (cst_which - first + dir + n) % n;
            draw_cust_edit(cst_which);
            sd_play_sound(MOVEGUN1SND);
        }
        cst_dir = dir;
        if (b->use) {
            cst_picking = true;
            cst_tick = cst_count = 0;
        } else if (b->back) {
            /* the row as it was, and the gun back on it, up and down */
            sd_play_sound(ESCPRESSEDSND);
            draw_window(5, CST_ROW(r) - 1, 310, 13, BKGDCOLOR);
            draw_cust_row(r, 0);
            view_pic(112, 184, PIC_C_MOUSELBACK);
            begin_handle();
        }
        break;
    }

    case S_NAME:
        switch (line_input_update(tics, b)) {
        case LINE_DONE: {
            const size_t n = strnlen(line_input_text(), SAVE_NAME_LEN);
            memcpy(ls_name, line_input_text(), n);
            ls_name[n] = 0;
            view_pic(112, 184, PIC_C_MOUSELBACK);   /* the menus' strip again */
            ls_action(true, A_SAVE_WRITE);
            break;
        }
        case LINE_CANCELLED:
            /* Esc: the slot as it was, and back to choosing one */
            view_bar(LSM_X + ls_items.indent + 1, LSM_Y + ls_slot * 13 + 1,
                     LSM_W - ls_items.indent - 16, 10, BKGDCOLOR);
            print_ls_entry(ls_slot, HIGHLIGHT);
            view_pic(112, 184, PIC_C_MOUSELBACK);
            sd_play_sound(ESCPRESSEDSND);
            begin_handle();
            break;
        }
        break;

    case S_LSACTION:
        /* DiskFlopAnim(): the disk turns while the game is read or written */
        ls_tics += tics;
        view_pic(LSA_X + 8, LSA_Y + 5,
                 PIC_C_DISKLOADING1 + (ls_tics / 6) % 2);
        if (ls_tics >= LSA_TICS)
            run(after);
        break;

    case S_CONFIRM:
        /* blink the cursor */
        confirm_count += tics;
        if (confirm_count >= 10) {
            if (confirm_tick == 0)
                view_bar(confirm_x, confirm_y, 8, 13, TEXTCOLOR);
            else {
                printx = confirm_x;
                printy = confirm_y;
                us_print("_");
            }
            confirm_tick ^= 1;
            confirm_count = 0;
        }
        if (b->use || b->back) {
            confirmed = b->use;
            if (confirmed)
                sd_play_sound(SHOOTSND);
            sd_play_sound(confirmed ? SHOOTSND : ESCPRESSEDSND);
            run(after);
        }
        break;

    case S_ARTICLE:
        if (article_update(b->left ? -1 : b->right ? 1 : 0, b->use,
                           b->back || b->menu)) {
            fade_out(FADE_BLACK, 30);       /* HelpScreens(): VW_FadeOut() */
            wait_fade(A_READTHIS_DONE);
        }
        break;

    case S_DONE:
        return result;
    }

    return state == S_DONE ? result : MENU_BUSY;
}
