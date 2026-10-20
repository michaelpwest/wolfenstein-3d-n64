/* pcspeaker.c - the PC Speaker, for the Sound menu's PC Speaker effects:
 * SDL_PCService() in ID_SD.C, and the speaker it drove.
 *
 * A PC Speaker sound is a byte per 140 Hz tick. SDL_PCService() looked each
 * new one up in pcSoundLookup[] (the byte times 60), set channel 2 of the
 * 8253 timer to it as a divisor of its 1193181 Hz clock, and connected the
 * speaker; a 0 disconnected it. In mode 3 that channel drives the speaker
 * with a square wave, so a byte b sounds at 1193181 / (60 b) Hz - b = 20 is
 * 994 Hz - and the speaker has no volume: it is all the way out or all the
 * way in.
 *
 * Each output sample is the square wave's average over that sample's span
 * of time, worked out exactly from its phase: a box filter, which keeps
 * tones near or above half the output rate from folding back as lower
 * ones, while the edges stay as hard as the speaker's. The phase runs on
 * through a change of note, as the timer's output does when it is given a
 * new count. */
#include <stddef.h>
#include "pcspeaker.h"

#define PIT_CLOCK  1193181.0f
#define TICK_RATE  140

/* the speaker's swing, set against the AdLib effects (adlib.c's GAIN):
 * a square wave this high is about as loud as their FM tones */
#define LEVEL      5000.0f

static const uint8_t *sound;            /* the next byte, or NULL */
static uint32_t       sound_left;
static int            tick_acc;         /* SDL_t0Service()'s 140 Hz count  */
static float          cycles;           /* the square wave's cycles per sample */
static float          phase;            /* where in its cycle, 0 .. 1 */
static bool           on;               /* the speaker connected */

void pcs_stop(void)
{
    sound = NULL;
    sound_left = 0;
    on = false;
}

void pcs_play(const uint8_t *chunk, uint32_t size)
{
    uint32_t length;

    pcs_stop();
    if (size < 7)
        return;
    length = chunk[0] | chunk[1] << 8 | chunk[2] << 16 | (uint32_t)chunk[3] << 24;
    if (length > size - 6)
        length = size - 6;
    if (!length)
        return;
    sound = chunk + 6;
    sound_left = length;
    tick_acc = PCS_RATE;                /* the first byte at once */
}

bool pcs_active(void)
{
    return sound != NULL;
}

/* The integral of the square wave (+1 for the first half of each cycle, -1
 * for the second) from the start of the cycle to x of the way through it;
 * a whole cycle comes to nothing. */
static float square_area(float x)
{
    x -= (float)(int)x;
    return x < 0.5f ? x : 1.0f - x;
}

void pcs_generate(int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        float v = 0;

        if ((tick_acc += TICK_RATE) >= PCS_RATE) {      /* SDL_PCService() */
            tick_acc -= PCS_RATE;
            if (sound && sound_left) {
                const uint8_t b = *sound++;
                sound_left--;
                on = b != 0;
                if (on)
                    cycles = PIT_CLOCK / (60.0f * b) / PCS_RATE;
            } else {
                /* the tick after the last: SDL_PCStopSound() */
                sound = NULL;
                on = false;
            }
        }
        if (on) {
            /* a tone of half the output rate or more averages out to
             * nothing over a sample - and nearly did through the speaker */
            if (cycles < 0.5f)
                v = (square_area(phase + cycles) - square_area(phase)) / cycles;
            phase += cycles;
            phase -= (float)(int)phase;
        }
        out[i] = (int16_t)(v * LEVEL);
    }
}
