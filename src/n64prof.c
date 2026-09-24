/* n64prof.c - where a frame's time goes, measured on the N64.
 *
 * The game marks the start of each stretch of a frame with PROF() (see
 * wolf.h); here each mark reads the CPU's COUNT register, which ticks at
 * half the 93.75 MHz clock, and charges the time since the last mark to the
 * stretch that was running. Once a second the totals become per-frame
 * averages for the overlay. Hold R and press L in the game to show it.
 *
 * Built with BENCH=1 (make BENCH=1), the ROM skips the game and plays a
 * fixed set of scenes instead - turning on the spot at the start of several
 * floors, a guard at point blank, a wall filling the view - and ends on a
 * table of the averages per scene, so that runs can be compared.
 */
#include <stdio.h>
#include <libdragon.h>
#include "wolf.h"
#include "n64prof.h"

static const char *const names[PROF_SECTIONS] = {
    "logic", "background", "rays", "columns", "collect", "sprites",
    "weapon", "status", "present", "wait", "audio", "overlay"
};

static bool     enabled;
static int      current;
static uint32_t last_mark;

static uint64_t total[PROF_SECTIONS];       /* ticks since enabled */
static uint64_t second_base[PROF_SECTIONS]; /* total when this second began */
static int      second_frames;
static uint32_t second_worst, second_start, frame_start, last_frame;

static int      avg_us[PROF_SECTIONS];      /* last second, per frame */
static int      avg_fps, avg_worst_us;

static int ticks_to_us(uint64_t ticks)
{
    return (int)(ticks * 1000000 / TICKS_PER_SECOND);
}

static void mark(int section)
{
    const uint32_t now = TICKS_READ();
    total[current] += TICKS_DISTANCE(last_mark, now);
    last_mark = now;
    current = section;
}

void prof_enable(bool on)
{
    enabled = on;
    prof_mark = on ? mark : NULL;
    for (int s = 0; s < PROF_SECTIONS; s++)
        total[s] = second_base[s] = 0;
    second_frames = 0;
    second_worst = 0;
    avg_fps = 0;
    last_mark = second_start = frame_start = TICKS_READ();
    current = PROF_LOGIC;
}

bool prof_enabled(void)
{
    return enabled;
}

/* The top of the frame loop: close the last frame, and every second turn
 * that second's totals into per-frame averages. */
void prof_frame(void)
{
    uint32_t now;

    if (!enabled)
        return;
    mark(PROF_LOGIC);
    now = last_mark;
    last_frame = TICKS_DISTANCE(frame_start, now);
    frame_start = now;
    if (last_frame > second_worst)
        second_worst = last_frame;
    second_frames++;

    if (TICKS_DISTANCE(second_start, now) < (int32_t)TICKS_PER_SECOND)
        return;
    for (int s = 0; s < PROF_SECTIONS; s++) {
        avg_us[s] = ticks_to_us(total[s] - second_base[s]) / second_frames;
        second_base[s] = total[s];
    }
    avg_fps = second_frames;
    avg_worst_us = ticks_to_us(second_worst);
    second_frames = 0;
    second_worst = 0;
    second_start = now;
}

static void draw_ms(int right, int y, int us)
{
    char text[16];
    snprintf(text, sizeof text, "%d.%d", us / 1000, us % 1000 / 100);
    font_draw(0, right - font_measure(0, text), y, text, 15);
}

/* The last second's averages, top left of the view. */
void prof_draw(const char *title)
{
    const int lines = PROF_SECTIONS + 2 + (title != NULL);
    char text[48];
    int y = 2;

    if (!enabled)
        return;
    PROF(PROF_OVERLAY);
    view_bar(0, 0, 112, lines * 10 + 4, 0);
    if (title) {
        font_draw(0, 4, y, title, 14);
        y += 10;
    }
    snprintf(text, sizeof text, "%d fps", avg_fps);
    font_draw(0, 4, y, text, 14);
    y += 10;
    font_draw(0, 4, y, "worst", 14);
    draw_ms(108, y, avg_worst_us);
    y += 10;
    for (int s = 0; s < PROF_SECTIONS; s++, y += 10) {
        font_draw(0, 4, y, names[s], 15);
        draw_ms(108, y, avg_us[s]);
    }
}

/* ------------------------------------------------------------------ */
/* The benchmark                                                       */
/* ------------------------------------------------------------------ */

#ifdef WOLF_BENCH

enum { SC_TURN, SC_GUARD, SC_WALL, SC_PATTERN_ROWS, SC_PATTERN_COLUMNS };

static const struct {
    const char *name;
    int kind, level, seconds, size;
} scenes[] = {
    /* the same test card shown both ways, to check the turned blit */
    { "rows",    SC_PATTERN_ROWS,    0, 3, 20 },
    { "columns", SC_PATTERN_COLUMNS, 0, 3, 20 },
    { "floor 1", SC_TURN,  0, 8, 20 },
    { "floor 3", SC_TURN,  2, 8, 20 },
    { "floor 5", SC_TURN,  4, 8, 20 },
    { "floor 9", SC_TURN,  8, 8, 20 },
    { "guard",   SC_GUARD, 0, 5, 20 },
    { "wall",    SC_WALL,  0, 5, 20 },
    { "1 at 15", SC_TURN,  0, 8, 15 },      /* Change View */
    { "1 at 10", SC_TURN,  0, 8, 10 },
};
#define NUM_SCENES  ((int)(sizeof scenes / sizeof scenes[0]))
#define SKIP_FRAMES 10                      /* the load, and settling */

static const int dir_dx[4] = { 1, 0, -1, 0 }, dir_dy[4] = { 0, -1, 0, 1 };

/* Every scene runs twice: first with the probes and the overlay, for where
 * the time goes, then with neither, for the frame rate they cost. */
static int      pass, scene = -1, scene_frames, scene_counted;
static uint32_t scene_start, counted_start, scene_worst;
static uint64_t scene_base[PROF_SECTIONS];
static int      result_us[NUM_SCENES][PROF_SECTIONS];
static int      result_fps10[2][NUM_SCENES], result_worst_us[2][NUM_SCENES];

/* After the test cards are blitted, the frame the RDP drew is read back
 * and compared with the picture, pixel for pixel. */
static int check_frames[2], check_bad[2], check_first_x[2] = { -1, -1 },
           check_first_y[2], check_last_x[2], check_last_y[2];

void bench_check_frame(const surface_t *disp, int view_y)
{
    const int kind = scene >= 0 && scene < NUM_SCENES ? scenes[scene].kind : -1;
    const int which = kind - SC_PATTERN_ROWS;
    const uint16_t *fb = disp->buffer;
    const int stride = disp->stride / 2;

    if (which < 0 || which > 1 || scene_frames < SKIP_FRAMES)
        return;
    check_frames[which]++;
    for (int y = 0; y < VIEW_H; y++)
        for (int x = 0; x < VIEW_W; x++) {
            const uint16_t want = *view_pixel(x, y) & 0xfffe;
            if ((fb[(y + view_y) * stride + x] & 0xfffe) != want) {
                if (check_first_x[which] < 0) {
                    check_first_x[which] = x;
                    check_first_y[which] = y;
                }
                check_last_x[which] = x;
                check_last_y[which] = y;
                check_bad[which]++;
            }
        }
}

static void start_scene(int n)
{
    int tx, ty;

    scene = n;
    scene_frames = scene_counted = 0;
    scene_worst = 0;
    scene_start = TICKS_READ();
    if (n >= NUM_SCENES)
        return;

    dos_exact = false;
    set_view_size(scenes[n].size);
    game_new();
    gamestate.difficulty = gd_hard;
    level_load(scenes[n].level);
    set_palette_shift(0);
    tx = player.tilex;
    ty = player.tiley;

    if (scenes[n].kind == SC_GUARD) {
        /* a guard filling the view, one tile ahead, facing the player */
        actors_init();
        for (int d = 0; d < 4; d++)
            if (!tilemap[ty + dir_dy[d]][tx + dir_dx[d]]) {
                player.angle = d * 900;
                spawn_map_actor(108 + (d + 2) % 4, tx + dir_dx[d], ty + dir_dy[d],
                                false);
                break;
            }
    } else if (scenes[n].kind == SC_WALL) {
        /* the nearest wall straight ahead */
        for (int d = 0; d < 4; d++) {
            const int t = tilemap[ty + dir_dy[d]][tx + dir_dx[d]];
            if (t && !(t & 0x80)) {
                player.angle = d * 900;
                break;
            }
        }
    }
}

static void end_scene(uint32_t now)
{
    const int n = scene;
    const uint32_t span = TICKS_DISTANCE(counted_start, now);

    if (pass == 0)
        for (int s = 0; s < PROF_SECTIONS; s++)
            result_us[n][s] = scene_counted
                ? ticks_to_us(total[s] - scene_base[s]) / scene_counted : 0;
    result_fps10[pass][n] = span
        ? (int)((int64_t)scene_counted * 10 * TICKS_PER_SECOND / span) : 0;
    result_worst_us[pass][n] = ticks_to_us(scene_worst);
}

/* One frame of the benchmark in place of game_frame(), after prof_frame(). */
void bench_frame(int tics)
{
    const uint32_t now = TICKS_READ();

    if (scene < 0) {
        fade_in(1);                         /* the game starts faded out */
        fade_update(1);
        prof_enable(true);
        start_scene(0);
    } else if (scene < NUM_SCENES) {
        /* the frame just finished counts, once the scene has settled */
        if (scene_frames == SKIP_FRAMES) {
            for (int s = 0; s < PROF_SECTIONS; s++)
                scene_base[s] = total[s];
            counted_start = now;
        } else if (scene_frames > SKIP_FRAMES) {
            if (last_frame > scene_worst)
                scene_worst = last_frame;
            scene_counted++;
        }
        scene_frames++;

        if (TICKS_DISTANCE(scene_start, now)
            >= (int32_t)(scenes[scene].seconds * TICKS_PER_SECOND)) {
            end_scene(now);
            start_scene(scene + 1);
            if (scene == NUM_SCENES && pass == 0) {
                pass = 1;                   /* again, unprobed */
                prof_mark = NULL;
                start_scene(0);
            }
        }
    }

    if (scene < NUM_SCENES && scenes[scene].kind >= SC_PATTERN_ROWS) {
        /* a one-pixel checkerboard with colored edges and a marker near the
         * top left: a mirrored, turned or shifted blit shows at once */
        view_columns = scenes[scene].kind == SC_PATTERN_COLUMNS;
        for (int y = 0; y < VIEW_H; y++)
            for (int x = 0; x < VIEW_W; x++) {
                uint16_t c = ((x + y) & 1) ? 0xffff : 0x0001;
                if (x == 0)                 c = 0xf801;     /* red   */
                else if (x == VIEW_W - 1)   c = 0x07c1;     /* green */
                else if (y == 0)            c = 0x003f;     /* blue  */
                else if (y == VIEW_H - 1)   c = 0xffc1;     /* yellow */
                else if (x >= 20 && x < 60 && y >= 20 && y < 40)
                    c = 0xf801;                             /* red block */
                *view_pixel(x, y) = c;
            }
        status_draw();
        return;
    }

    if (scene < NUM_SCENES) {
        if (scenes[scene].kind == SC_TURN) {
            controls_t c = { 0 };           /* a full turn over the scene */
            c.turn = 2 * 3600 * tics / (scenes[scene].seconds * 70) + 1;
            player_move(&c, tics);
        }
        move_doors(tics);
        move_pwalls(tics);
        if (scenes[scene].kind == SC_TURN)
            actors_think(tics);
        gamestate.health = 100;             /* nothing ends the run */
        player_died = false;
        set_palette_shift(0);
        render_view();
        status_draw();
        if (pass == 0)
            prof_draw(scenes[scene].name);
        return;
    }

    /* the results: a column a scene, a row a stretch, ms a frame */
    view_bar(0, 0, VIEW_W, VIEW_H, 0);
    font_draw(0, 2, 2, "scene", 14);
    font_draw(0, 2, 14, "fps", 14);
    font_draw(0, 2, 26, "worst ms", 14);
    font_draw(0, 2, 38, "probed fps", 14);
    for (int s = 0; s < PROF_SECTIONS; s++)
        font_draw(0, 2, 52 + s * 10, names[s], 15);
    {
        char text[96];
        snprintf(text, sizeof text, "rows: %d bad / %d frames", check_bad[0],
                 check_frames[0]);
        font_draw(0, 2, 178, text, 14);
        snprintf(text, sizeof text, "cols: %d bad / %d frames, %d,%d .. %d,%d",
                 check_bad[1], check_frames[1], check_first_x[1],
                 check_first_y[1], check_last_x[1], check_last_y[1]);
        font_draw(0, 2, 189, text, 14);
    }
    for (int n = 2; n < NUM_SCENES; n++) {        /* less the test cards */
        char text[16];
        const int right = 104 + (n - 2) * 27;
        snprintf(text, sizeof text, "%d", n - 1);
        font_draw(0, right - font_measure(0, text), 2, text, 14);
        snprintf(text, sizeof text, "%d.%d", result_fps10[1][n] / 10,
                 result_fps10[1][n] % 10);
        font_draw(0, right - font_measure(0, text), 14, text, 14);
        draw_ms(right, 26, result_worst_us[1][n]);
        snprintf(text, sizeof text, "%d.%d", result_fps10[0][n] / 10,
                 result_fps10[0][n] % 10);
        font_draw(0, right - font_measure(0, text), 38, text, 15);
        for (int s = 0; s < PROF_SECTIONS; s++)
            draw_ms(right, 52 + s * 10, result_us[n][s]);
    }
    status_draw();
}

#endif
