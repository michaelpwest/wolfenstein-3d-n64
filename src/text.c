/* text.c - the text pages, HelpScreens(), EndText() and ShowArticle() from
 * WL_TEXT.C, and the two proportional fonts everything else prints with.
 *
 * An article is plain text with ^ commands: ^P starts a page, ^E ends the
 * article, ^Cxx sets the text color, ^Gy,x,pic draws a picture and pushes
 * the margins away from it, ^Lx,y moves the pen, ^By,x,w,h draws a bar,
 * ^> moves to mid-screen. PageLayout() word-wraps one page at a time, and
 * the reader flips back and forth; the pointer into the text is left at
 * the next page's ^P, and going back scans for the ^P two pages behind.
 *
 * The page is the whole 320x200 VGA screen, drawn into viewbuf with the
 * small font: VGAGRAPH's STARTFONT chunk, a fontstruct of little-endian
 * words read here byte by byte.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "wolf.h"

#define BACKCOLOR       0x11
#define WORDLIMIT       80
#define FONTHEIGHT      10
#define TOPMARGIN       16
#define BOTTOMMARGIN    32
#define LEFTMARGIN      16
#define RIGHTMARGIN     16
#define PICMARGIN       8
#define TEXTROWS        ((200 - TOPMARGIN - BOTTOMMARGIN) / FONTHEIGHT)
#define SPACEWIDTH      7
#define SCREENPIXWIDTH  320
#define SCREENMID       (SCREENPIXWIDTH / 2)

static const char *text;
static int  pagenum, numpages;
static int  leftmargin[TEXTROWS], rightmargin[TEXTROWS];
static int  rowon;
static int  px, py;
static int  fontcolor;
static bool layoutdone;

/* ------------------------------------------------------------------ */
/* The proportional fonts - VW_DrawPropString(), VW_MeasurePropString() */
/* ------------------------------------------------------------------ */

int font_height(int font)
{
    const uint8_t *f = wolf_fonts[font];
    return f[0] | (f[1] << 8);
}

static int glyph_width(int font, unsigned char ch)
{
    return wolf_fonts[font][2 + 512 + ch];
}

static const uint8_t *glyph(int font, unsigned char ch)
{
    const uint8_t *f = wolf_fonts[font];
    return f + (f[2 + 2 * ch] | (f[3 + 2 * ch] << 8));
}

int font_measure(int font, const char *s)
{
    int w = 0;
    while (*s)
        w += glyph_width(font, (unsigned char)*s++);
    return w;
}

/* Rows are stored whole, one byte a pixel; anything nonzero is ink. */
int font_draw(int font, int x, int y, const char *s, int color)
{
    return font_draw_ink(font, x, y, s, wolf_palette[color & 0xff]);
}

/* The same in a color given as it is, not as a color of the game's
 * palette - for text over a picture with a palette of its own. */
int font_draw_ink(int font, int x, int y, const char *s, uint16_t ink)
{
    const int height = font_height(font);

    view_border_dirty |= view_columns;      /* it may cover the border */
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        const int w = glyph_width(font, ch);
        const uint8_t *src = glyph(font, ch);

        for (int xx = 0; xx < w; xx++, x++)
            for (int yy = 0; yy < height; yy++)
                if (src[yy * w + xx] && x >= 0 && x < VIEW_W
                    && y + yy >= 0 && y + yy < VIEW_H)
                    *view_pixel(x, y + yy) = ink;
    }
    return x;
}

/* The same into the status bar's buffer, for Change View's instructions,
 * which DOS printed where the status bar goes. */
int font_draw_status(int font, int x, int y, const char *s, int color)
{
    const int height = font_height(font);
    const uint16_t ink = wolf_palette[color & 0xff];

    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        const int w = glyph_width(font, ch);
        const uint8_t *src = glyph(font, ch);

        for (int xx = 0; xx < w; xx++, x++)
            for (int yy = 0; yy < height; yy++)
                if (src[yy * w + xx] && x >= 0 && x < SCREEN_W
                    && y + yy >= 0 && y + yy < STATUS_H)
                    statusbuf[y + yy][x] = ink;
    }
    return x;
}

static int measure(const char *s)
{
    return font_measure(0, s);
}

static void draw_string(const char *s)
{
    px = font_draw(0, px, py, s, fontcolor);
}

/* ------------------------------------------------------------------ */
/* PageLayout() and its helpers                                        */
/* ------------------------------------------------------------------ */

static void rip_to_eol(void)
{
    while (*text++ != '\n')
        ;
}

static int parse_number(void)
{
    int n = 0;

    while (*text < '0' || *text > '9')
        text++;
    while (*text >= '0' && *text <= '9')
        n = n * 10 + (*text++ - '0');
    return n;
}

static int hex_digit(int c)
{
    c = toupper(c);
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

static void handle_command(void)
{
    int picx, picy, picnum, picw, pich, picmid, margin, top, bottom;

    switch (toupper(*++text)) {
    case 'B':
        picy = parse_number();
        picx = parse_number();
        picw = parse_number();
        pich = parse_number();
        view_bar(picx, picy, picw, pich, BACKCOLOR);
        rip_to_eol();
        break;

    case ';':                               /* comment */
        rip_to_eol();
        break;

    case 'P':                               /* start of the next page */
    case 'E':                               /* end of the article */
        layoutdone = true;
        text--;                             /* back up to the '^' */
        break;

    case 'C':                               /* ^Cxx: color, two hex digits */
        fontcolor = hex_digit(*++text) * 16;
        fontcolor += hex_digit(*++text);
        text++;
        break;

    case '>':
        px = 160;
        text++;
        break;

    case 'L':
        py = parse_number();
        rowon = (py - TOPMARGIN) / FONTHEIGHT;
        py = TOPMARGIN + rowon * FONTHEIGHT;
        px = parse_number();
        rip_to_eol();
        break;

    case 'T':                               /* ^Ty,x,pic,delay */
    case 'G':                               /* ^Gy,x,pic */
    {
        const bool timed = toupper(*text) == 'T';

        picy = parse_number();
        picx = parse_number();
        picnum = parse_number();
        if (timed)
            parse_number();                 /* the delay: see below */
        rip_to_eol();
        view_pic(picx & ~7, picy, picnum);

        /* TimedPicCommand() only waits and draws; the margins are pushed
         * for ^G alone. The wait is dropped - no article in the shareware
         * or the full game uses ^T. */
        if (timed)
            break;

        assets_pic(picnum, &picw, &pich);
        picmid = picx + picw / 2;
        if (picmid > SCREENMID)
            margin = picx - PICMARGIN;              /* new right margin */
        else
            margin = picx + picw + PICMARGIN;       /* new left margin */

        top = (picy - TOPMARGIN) / FONTHEIGHT;
        if (top < 0)
            top = 0;
        bottom = (picy + pich - TOPMARGIN) / FONTHEIGHT;
        if (bottom >= TEXTROWS)
            bottom = TEXTROWS - 1;

        for (int i = top; i <= bottom; i++) {
            if (picmid > SCREENMID)
                rightmargin[i] = margin;
            else
                leftmargin[i] = margin;
        }

        /* adjust this line if needed */
        if (px < leftmargin[rowon])
            px = leftmargin[rowon];
        break;
    }
    }
}

static void new_line(void)
{
    if (++rowon == TEXTROWS) {
        /* overflowed the page, so skip until the next page break */
        layoutdone = true;
        for (;; text++) {
            if (*text == '^') {
                const int ch = toupper(text[1]);
                if (ch == 'E' || ch == 'P')
                    return;
            }
        }
    }
    px = leftmargin[rowon];
    py += FONTHEIGHT;
}

static void handle_ctrls(void)
{
    if (*text++ == '\n')
        new_line();
}

static void handle_word(void)
{
    char word[WORDLIMIT];
    int n = 0, width;

    word[n++] = *text++;
    while ((unsigned char)*text > 32 && n < WORDLIMIT - 1)
        word[n++] = *text++;
    word[n] = 0;

    /* see if it fits on this line */
    width = measure(word);
    while (px + width > rightmargin[rowon]) {
        new_line();
        if (layoutdone)
            return;                         /* overflowed the page */
    }

    draw_string(word);

    /* suck up any extra spaces */
    while (*text == ' ') {
        px += SPACEWIDTH;
        text++;
    }
}

static void page_layout(void)
{
    char str[32];

    fontcolor = 0;

    view_bar(0, 0, 320, 200, BACKCOLOR);
    view_pic(0, 0, PIC_H_TOPWINDOW);
    view_pic(0, 8, PIC_H_LEFTWINDOW);
    view_pic(312, 8, PIC_H_RIGHTWINDOW);
    view_pic(8, 176, PIC_H_BOTTOMINFO);

    for (int i = 0; i < TEXTROWS; i++) {
        leftmargin[i] = LEFTMARGIN;
        rightmargin[i] = SCREENPIXWIDTH - RIGHTMARGIN;
    }

    px = LEFTMARGIN;
    py = TOPMARGIN;
    rowon = 0;
    layoutdone = false;

    /* the page must start with ^P */
    while ((unsigned char)*text <= 32)
        text++;
    if (*text != '^' || toupper(*++text) != 'P')
        return;                             /* DOS quits: nothing to show */
    rip_to_eol();

    do {
        const char ch = *text;
        if (ch == '^')
            handle_command();
        else if (ch == '\t') {
            px = (px + 8) & 0xf8;
            text++;
        } else if ((unsigned char)ch <= 32)
            handle_ctrls();
        else
            handle_word();
    } while (!layoutdone);

    pagenum++;

    snprintf(str, sizeof str, "pg %d of %d", pagenum, numpages);
    py = 183;
    px = 213;
    fontcolor = 0x4f;
    draw_string(str);
}

/* BackPage(): scan back for the previous ^P. */
static void back_page(void)
{
    pagenum--;
    do
        text--;
    while (!(*text == '^' && toupper(text[1]) == 'P'));
}

/* ------------------------------------------------------------------ */
/* ShowArticle()                                                       */
/* ------------------------------------------------------------------ */

void article_start(const char *article)
{
    /* CacheLayoutGraphics(): count the pages */
    numpages = pagenum = 0;
    for (const char *s = article; *s; s++) {
        if (*s != '^')
            continue;
        if (toupper(s[1]) == 'P')
            numpages++;
        if (toupper(s[1]) == 'E')
            break;
    }

    text = article;
    view_bar(0, 0, 320, 200, BACKCOLOR);
    page_layout();
}

/* Up, PgUp or Left for the page before; Enter, Down, PgDn or Right for the
 * next; Escape leaves. On the pad `turn` is left or right, `accept` is A -
 * the next page, or leaving from the last one, which Enter did not do but a
 * pad has no key marked Esc - and `leave` is B or Start. */
bool article_update(int turn, bool accept, bool leave)
{
    if (leave)
        return true;
    if (turn < 0) {
        if (pagenum > 1) {
            back_page();
            back_page();
            page_layout();
        }
    } else if (turn > 0 || accept) {
        if (pagenum < numpages)
            page_layout();
        else if (accept)
            return true;
    }
    return false;
}
