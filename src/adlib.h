/* adlib.h - the AdLib sound effects' FM voice, synthesized as the game plays
 * (adlib.c). Free of libdragon, so the host tools can check it. */
#ifndef ADLIB_H
#define ADLIB_H

#include <stdbool.h>
#include <stdint.h>

/* The OPL2 runs at 49716 Hz; adlib_generate() gives every second sample. */
#define ADLIB_CHIP_RATE  49716
#define ADLIB_RATE       (ADLIB_CHIP_RATE / 2.0f)

/* SDL_ALPlaySound() with an AUDIOT AdLib chunk (SoundCommon, Instrument,
 * block, then a byte per 140 Hz tick); SDL_ALStopSound(). */
void adlib_play(const uint8_t *chunk, uint32_t size);
void adlib_stop(void);

/* A sound is being driven, or the last one is still ringing out. */
bool adlib_active(void);

/* n samples at ADLIB_RATE, with the effects' mix gain applied. */
void adlib_generate(int16_t *out, int n);

/* For tools/audio/adlib_check.c: one chip sample, unscaled, and the chip
 * back to its power-on state. */
int  adlib_chip_sample(void);
void adlib_reset(void);

#endif
