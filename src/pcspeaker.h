/* pcspeaker.h - the PC Speaker sound effects, synthesized as the game plays
 * (pcspeaker.c). Free of libdragon, so the host tools can check it. */
#ifndef PCSPEAKER_H
#define PCSPEAKER_H

#include <stdbool.h>
#include <stdint.h>

#define PCS_RATE  22050                     /* the mixer's output rate */

/* SDL_PCPlaySound() with an AUDIOT PC Speaker chunk (SoundCommon, then a
 * byte per 140 Hz tick); SDL_PCStopSound(). */
void pcs_play(const uint8_t *chunk, uint32_t size);
void pcs_stop(void);

/* A sound is playing. */
bool pcs_active(void);

/* n samples at PCS_RATE. */
void pcs_generate(int16_t *out, int n);

#endif
