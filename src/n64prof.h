/* n64prof.h - the frame profiler and the benchmark (n64prof.c). */
#ifndef N64PROF_H
#define N64PROF_H

#include <stdbool.h>

void prof_enable(bool on);
bool prof_enabled(void);
void prof_frame(void);                  /* the top of every frame */
void prof_draw(const char *title);      /* the overlay, into viewbuf */

#ifdef WOLF_BENCH
struct surface_s;
void bench_frame(int tics);             /* in place of game_frame() */
void bench_check_frame(const struct surface_s *disp, int view_y);
#endif

#endif
