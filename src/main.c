/* main.c - the N64 side: display, controller, and the frame loop.
 *
 * Everything the game actually does lives in the other files, which know
 * nothing about the N64 so that tools/sim can run them on a PC. This file
 * is the boundary: it reads the pad, runs a frame of game.c, and gets
 * viewbuf and statusbuf onto the screen with the fade applied.
 */
#include <libdragon.h>
#include <stdio.h>
#include "wolf.h"
#include "n64prof.h"

/* The blob, linked in by src/blob.S. */
extern const uint8_t wolf3d_dat[];

/* n64audio.c */
void n64audio_init(void);
void n64audio_poll(void);

/* Wolfenstein's clock is 70 Hz and it scales movement by the number of
 * those tics a frame took. MAXTICS in WL_DEF.H caps it so that a long
 * stall cannot teleport the player through a wall. */
#define TICS_PER_SECOND  70
#define MAXTICS          10

static surface_t view_surface;
static surface_t column_surface;            /* colbuf, the 3D view on its side */
static surface_t status_surface;

/* ------------------------------------------------------------------ */
/* The palette fade                                                    */
/* ------------------------------------------------------------------ */

/* VL_FadeOut() stepped every palette entry toward a color - black, or the
 * menus' dark red; moving every pixel of the finished frame the same
 * fraction of the way is the same arithmetic.
 * It is done here on copies of the two buffers, so screens that are drawn
 * a piece at a time (the stats screen) keep their pixels. The DOS fades
 * block the game, so the picture under a fade never needs re-rendering and
 * this costs nothing while playing. (Blending black over the frame on the
 * RDP was tried first; ares kept showing black after the fade had ended.) */
static uint16_t fadeview[VIEW_H][VIEW_STRIDE] __attribute__((aligned(16)));
static uint16_t fadestatus[STATUS_H][SCREEN_W] __attribute__((aligned(16)));
static surface_t fadeview_surface, fadestatus_surface;

static void fade_copy(const uint16_t *src, uint16_t *dst, int rows, int stride,
                      int cols, int fade, uint16_t color)
{
    uint16_t lut[3][32];

    /* each 5-bit channel, moved `fade` 255ths of the way to the color's */
    for (int c = 0; c < 3; c++) {
        const int target = (color >> (11 - 5 * c)) & 31;
        for (int i = 0; i < 32; i++)
            lut[c][i] = (uint16_t)((i + (target - i) * fade / 255) << (11 - 5 * c));
    }

    for (int y = 0; y < rows; y++, src += stride, dst += stride)
        for (int x = 0; x < cols; x++) {
            const uint16_t p = src[x];
            dst[x] = (uint16_t)(lut[0][(p >> 11) & 31] | lut[1][(p >> 6) & 31]
                               | lut[2][(p >> 1) & 31] | (p & 1));
        }
}

/* ------------------------------------------------------------------ */

/* The customizable inputs of a joypad_buttons_t, as bits PAD_x */
static unsigned pad_bits(joypad_buttons_t j)
{
    return (unsigned)j.b << PAD_B | (unsigned)j.a << PAD_A
         | (unsigned)j.z << PAD_Z | (unsigned)j.r << PAD_R
         | (unsigned)j.l << PAD_L
         | (unsigned)j.c_left << PAD_C_LEFT | (unsigned)j.c_right << PAD_C_RIGHT
         | (unsigned)j.c_up << PAD_C_UP     | (unsigned)j.c_down << PAD_C_DOWN
         | (unsigned)j.d_left << PAD_D_LEFT | (unsigned)j.d_right << PAD_D_RIGHT
         | (unsigned)j.d_up << PAD_D_UP     | (unsigned)j.d_down << PAD_D_DOWN;
}

/* The pad as it is this frame; controls.c does the rest */
static void __attribute__((unused))
read_controls(controls_t *c, int tics, buttons_t *b)
{
    const joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    const joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
    const pad_state_t p = {
        .held     = pad_bits(joypad_get_buttons(JOYPAD_PORT_1)),
        .pressed  = pad_bits(pressed),
        .released = pad_bits(joypad_get_buttons_released(JOYPAD_PORT_1)),
        .start    = pressed.start,
        .stick_x  = in.stick_x,
        .stick_y  = in.stick_y,
    };

    ctl_read_pad(&p, tics, c, b);
}

/* The sign-on screen's hardware, as the N64 has it. Main memory is the
 * console's 4 MB and an Expansion Pak's 4 MB shows as XMS. EMS has no N64
 * counterpart; the console's own memory stands in for it too, so the column
 * is full, as the game never runs short of it. A controller in any port is
 * a joystick and the N64 mouse a mouse. The sound is the AdLib chip,
 * emulated for the effects (adlib.c), beside the Sound Blaster's digitized
 * voice (n64audio.c), so both of those boxes are lit. */
void platform_machine(machine_t *m)
{
    const int kb = get_memory_size() / 1024;

    m->main_kb = kb < 4096 ? kb : 4096;
    m->ems_kb  = m->main_kb;
    m->xms_kb  = kb - m->main_kb;
    m->mouse = m->joystick = false;
    JOYPAD_PORT_FOREACH(port) {
        const joypad_style_t style = joypad_get_style(port);
        if (style == JOYPAD_STYLE_MOUSE)
            m->mouse = true;
        else if (style != JOYPAD_STYLE_NONE)
            m->joystick = true;
    }
    m->adlib = true;
    m->sound_blaster = true;
    m->sound_source = false;
}

/* The maps and the full-screen pictures, from wolfbig.dat in the ROM's
 * filesystem. */
static FILE *big_file;

void platform_read_big(uint32_t offset, void *dst, uint32_t len)
{
    assertf(big_file && fseek(big_file, (long)offset, SEEK_SET) == 0
            && fread(dst, 1, len, big_file) == len,
            "cannot read %lu bytes at %lu of wolfbig.dat",
            (unsigned long)len, (unsigned long)offset);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    uint32_t last_ticks, fps_ticks;
    int64_t  tic_frac = 0;              /* ticks x 70, not yet a whole tic */
    int frames = 0, fps = 0;

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE,
                 FILTERS_RESAMPLE);
    rdpq_init();
    joypad_init();

    assertf(dfs_init(DFS_DEFAULT_LOCATION) == DFS_ESUCCESS,
            "no ROM filesystem - wolfbig.dat and the audio were not built in");
    big_file = fopen("rom:/wolfbig.dat", "rb");
    assertf(big_file, "wolfbig.dat is missing from the ROM filesystem");
    assertf(assets_init(wolf3d_dat),
            "assets/wolf3d.dat is missing or stale - run tools/extract_assets.py");
    build_tables();
    n64audio_init();                    /* before level_load starts a song */
    sd_init();
    save_init();                        /* SetupSaveGames() */
    game_init();

    view_surface   = surface_make(viewbuf, FMT_RGBA16, VIEW_W, VIEW_H,
                                  VIEW_STRIDE * 2);
    column_surface = surface_make(colbuf, FMT_RGBA16, VIEW_H, VIEW_W,
                                  VIEW_H * 2);
    status_surface = surface_make(statusbuf, FMT_RGBA16, SCREEN_W, STATUS_H,
                                  SCREEN_W * 2);
    fadeview_surface   = surface_make(fadeview, FMT_RGBA16, VIEW_W, VIEW_H,
                                      VIEW_STRIDE * 2);
    fadestatus_surface = surface_make(fadestatus, FMT_RGBA16, SCREEN_W,
                                      STATUS_H, SCREEN_W * 2);

    last_ticks = fps_ticks = TICKS_READ();

    while (1) {
        controls_t c;
        buttons_t b;
        surface_t *view, *status;
        uint32_t now;
        int tics, fade;

        /* --- how much game time this frame is worth --- */
        /* CalcTics(): the part of a tic left over carries into the next
         * frame. Dropping it would lose most of a tic whenever a frame takes
         * between one and two - at 40 fps, the game would run at 40 tics a
         * second instead of 70. */
        now  = TICKS_READ();
        tic_frac += (int64_t)TICKS_DISTANCE(last_ticks, now) * TICS_PER_SECOND;
        last_ticks = now;
        tics = (int)(tic_frac / TICKS_PER_SECOND);
        tic_frac -= (int64_t)tics * TICKS_PER_SECOND;
        if (tics < 1) {                 /* DOS waited for the next tic */
            tics = 1;
            tic_frac -= TICKS_PER_SECOND;
            if (tic_frac < 0)
                tic_frac = 0;
        }
        if (tics > MAXTICS) {           /* and dropped the rest of a stall */
            tics = MAXTICS;
            tic_frac = 0;
        }

        /* Frames shown in the last whole second. */
        frames++;
        if (TICKS_DISTANCE(fps_ticks, now) >= (int32_t)TICKS_PER_SECOND) {
            fps = frames;
            frames = 0;
            fps_ticks = now;
        }

        /* --- simulate and draw into viewbuf / statusbuf --- */
        prof_frame();
#ifdef WOLF_BENCH
        (void)c; (void)b; (void)fps;
        bench_frame(tics);
#else
        joypad_poll();
        read_controls(&c, tics, &b);
        game_frame(&c, &b, tics, fps);
        if (prof_enabled() != opt_show_profiler)    /* Extras' Show Profiler */
            prof_enable(opt_show_profiler);
        if (!game_fullscreen())
            prof_draw(NULL);
#endif

        PROF(PROF_PRESENT);
        /* Both buffers are ordinary cached memory: the RDP reads RDRAM
         * directly, so what the CPU just wrote has to be pushed out of the
         * data cache before the blits can see it. */
        fade = game_fade();
        if (fade) {
            const uint16_t color = game_fade_color();
            view_to_rows();                     /* a fade works on rows */
            fade_copy(viewbuf[0], fadeview[0], VIEW_H, VIEW_STRIDE, VIEW_W,
                      fade, color);
            fade_copy(statusbuf[0], fadestatus[0], STATUS_H, SCREEN_W, SCREEN_W,
                      fade, color);
            view   = &fadeview_surface;
            status = &fadestatus_surface;
            data_cache_hit_writeback(fadeview, sizeof(fadeview));
            data_cache_hit_writeback(fadestatus, sizeof(fadestatus));
        } else if (view_columns) {
            view   = &column_surface;
            status = &status_surface;
            data_cache_hit_writeback(colbuf, sizeof(colbuf));
            data_cache_hit_writeback(statusbuf, sizeof(statusbuf));
        } else {
            view   = &view_surface;
            status = &status_surface;
            data_cache_hit_writeback(viewbuf, sizeof(viewbuf));
            data_cache_hit_writeback(statusbuf, sizeof(statusbuf));
        }

        PROF(PROF_WAIT);
        surface_t *disp = display_get();
        PROF(PROF_PRESENT);
        if (game_fullscreen())
            rdpq_attach_clear(disp, NULL);      /* black round a text page */
        else
            rdpq_attach(disp, NULL);
        {
            /* a 320x200 text page is centered on black */
            const int view_y = game_fullscreen() ? (SCREEN_H - VIEW_H) / 2 : 0;

            if (view == &column_surface) {
                /* colbuf is 200 wide and 320 tall, texel (s, t) the view's
                 * pixel (t, s). The RDP's TEXTURE_RECTANGLE_FLIP steps S
                 * down the screen and T across it, which is exactly that
                 * transpose, one texel to a pixel. It does not work in copy
                 * mode, so this is drawn in standard mode, point-sampled.
                 * A load into texture memory is at most 4096 bytes, and a
                 * row of colbuf takes 512 of them (200 16-bit texels on a
                 * power-of-two pitch), so the view goes up in strips of 8
                 * columns, each drawn with the whole texture's coordinates. */
                rdpq_set_mode_standard();
                for (int t0 = 0; t0 < VIEW_W; t0 += 8) {
                    rdpq_tex_upload_sub(TILE0, view, NULL, 0, t0, VIEW_H, t0 + 8);
                    rdpq_texture_rectangle_flip_raw(TILE0, t0, view_y, t0 + 8,
                                                    view_y + VIEW_H, 0, t0, 1, 1);
                }
                rdpq_set_mode_copy(false);
            } else {
                rdpq_set_mode_copy(false);
                rdpq_tex_blit(view, 0, view_y, NULL);
            }
            if (!game_fullscreen())
                rdpq_tex_blit(status, 0, VIEW_H, NULL);
        }

        /* Wait for the RDP to finish copying before showing the frame and,
         * more importantly, before the next frame starts writing into
         * viewbuf and statusbuf. rdpq_detach_show() returns as soon as the
         * blits are queued; on real hardware the RDP then copies while the
         * CPU is already clearing the next frame from row 0 down, and it
         * picks up half-drawn rows - a flicker along the top of the screen,
         * where the ceiling fill starts and the FPS text is redrawn last. */
        rdpq_detach_wait();
#ifdef WOLF_BENCH
        bench_check_frame(disp, game_fullscreen() ? (SCREEN_H - VIEW_H) / 2 : 0);
#endif
        display_show(disp);

        /* Top up the audio queue now, while the RSP is idle - mixer_poll
         * mixes on the RSP and would otherwise wait behind the blits. */
        PROF(PROF_AUDIO);
        n64audio_poll();
    }
}
