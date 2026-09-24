/* adlib_check.c - src/adlib.c against Nuked-OPL3, sample for sample.
 *
 * Both chips are driven the same way - SDL_ALPlaySound()'s instrument
 * writes, then a byte a 140 Hz tick - and every 49716 Hz sample of the
 * cut-down voice must equal Nuked's. Two runs: each effect alone from a
 * reset chip, and then all of them back to back, each cutting the last one
 * off at a different point, so the state a sound leaves behind is checked
 * too. Then the half-rate output adlib_generate() gives the mixer is
 * checked against every second sample, and timed.
 *
 *   adlib_check <assets/audio>
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nuked-opl3/opl3.h"
#include "../../src/adlib.h"

#define MAX_SOUNDS 128

static uint8_t *chunks[MAX_SOUNDS];
static uint32_t sizes[MAX_SOUNDS];
static int      nsounds;

/* ------------------------------------------------------------------ */
/* The reference: Nuked, driven as ID_SD.C drives the OPL2             */
/* ------------------------------------------------------------------ */

static opl3_chip ref;
static const uint8_t *ref_sound;
static uint32_t ref_left, ref_acc;
static uint8_t  ref_block;

static void ref_reset(void)
{
    OPL3_Reset(&ref, 49716);
    for (int r = 1; r <= 0xf5; r++)
        OPL3_WriteReg(&ref, (uint16_t)r, 0);
    OPL3_WriteReg(&ref, 0x01, 0x20);
    ref_sound = NULL;
    ref_left = ref_acc = 0;
}

static void ref_play(const uint8_t *c, uint32_t size)
{
    static const uint8_t reg[5] = { 0x20, 0x40, 0x60, 0x80, 0xe0 };
    uint32_t length = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;

    ref_sound = NULL;                           /* SDL_ALStopSound() */
    OPL3_WriteReg(&ref, 0xb0, 0);
    if (length > size - 23)
        length = size - 23;
    if (!length)
        return;
    for (int k = 0; k < 5; k++) {               /* SDL_AlSetFXInst() */
        OPL3_WriteReg(&ref, reg[k], c[6 + k * 2]);
        OPL3_WriteReg(&ref, reg[k] + 3, c[6 + k * 2 + 1]);
    }
    OPL3_WriteReg(&ref, 0xc0, 0);
    ref_block = (uint8_t)((c[22] & 7) << 2 | 0x20);
    ref_left = length;
    ref_sound = c + 23;
    ref_acc = 49716 - 140;
}

static int ref_sample(void)
{
    int16_t buf[2];

    ref_acc += 140;
    if (ref_acc >= 49716) {
        ref_acc -= 49716;
        if (ref_sound) {                        /* SDL_ALSoundService() */
            uint8_t s = *ref_sound++;
            if (!s) {
                OPL3_WriteReg(&ref, 0xb0, 0);
            } else {
                OPL3_WriteReg(&ref, 0xa0, s);
                OPL3_WriteReg(&ref, 0xb0, ref_block);
            }
            if (!--ref_left) {
                ref_sound = NULL;
                OPL3_WriteReg(&ref, 0xb0, 0);
            }
        }
    }
    OPL3_Generate(&ref, buf);
    return buf[0];
}

/* ------------------------------------------------------------------ */

static void load(const char *dir)
{
    char path[512];
    for (int s = 0; s < MAX_SOUNDS; s++) {
        FILE *f;
        long len;
        snprintf(path, sizeof path, "%s/al%02d.adl", dir, s);
        f = fopen(path, "rb");
        if (!f)
            break;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        chunks[s] = malloc(len);
        if (fread(chunks[s], 1, len, f) != (size_t)len) { perror(path); exit(1); }
        fclose(f);
        sizes[s] = (uint32_t)len;
        nsounds = s + 1;
    }
}

static long mismatches;

static void compare(long t, int a, int b, const char *what)
{
    if (a != b && mismatches++ < 10)
        printf("  %s: sample %ld: synth %d, Nuked %d\n", what, t, a, b);
}

int main(int argc, char **argv)
{
    long total = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: adlib_check <assets/audio>\n");
        return 1;
    }
    load(argv[1]);
    if (!nsounds) {
        fprintf(stderr, "no alNN.adl files in %s\n", argv[1]);
        return 1;
    }

    /* each alone, until both have fallen silent */
    for (int s = 0; s < nsounds; s++) {
        char what[32];
        long t = 0;
        snprintf(what, sizeof what, "al%02d alone", s);
        adlib_reset();
        ref_reset();
        adlib_play(chunks[s], sizes[s]);
        ref_play(chunks[s], sizes[s]);
        while (adlib_active() && t < 49716L * 10)
            compare(t++, adlib_chip_sample(), ref_sample(), what);
        for (int k = 0; k < 4971; k++)          /* and 0.1 s past that */
            compare(t++, adlib_chip_sample(), ref_sample(), what);
        total += t;
    }
    printf("each effect alone: %ld samples, %ld mismatches\n", total, mismatches);

    /* back to back, each cut off somewhere between 10 ms and its end + 0.5 s */
    {
        unsigned seed = 1;
        long t = 0, before = mismatches;
        adlib_reset();
        ref_reset();
        for (int pass = 0; pass < 3; pass++)
            for (int s = 0; s < nsounds; s++) {
                const long len = (long)(sizes[s] - 23) * 49716 / 140;
                long run;
                seed = seed * 1103515245u + 12345u;
                run = 497 + (long)((seed >> 8) % (unsigned long)(len + 24858));
                adlib_play(chunks[s], sizes[s]);
                ref_play(chunks[s], sizes[s]);
                while (run-- > 0)
                    compare(t++, adlib_chip_sample(), ref_sample(), "back to back");
                if (s % 7 == 3) {                   /* SD_StopSound() now and then */
                    adlib_stop();
                    ref_sound = NULL;
                    OPL3_WriteReg(&ref, 0xb0, 0);
                }
            }
        printf("back to back: %ld samples, %ld mismatches\n", t, mismatches - before);
    }

    /* the mixer's half rate: every second chip sample, times the gain */
    {
        int16_t buf[512];
        long t = 0, before = mismatches, n = 0;
        clock_t c0;
        double secs;

        for (int s = 0; s < nsounds; s++) {
            adlib_reset();
            ref_reset();
            adlib_play(chunks[s], sizes[s]);
            ref_play(chunks[s], sizes[s]);
            for (long k = 0; adlib_active() && k < 10L * 24858 / 512; k++) {
                adlib_generate(buf, 512);
                for (int i = 0; i < 512; i++) {
                    int v;
                    ref_sample();
                    v = ref_sample() * 3;
                    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                    compare(t++, buf[i], v, "half rate");
                }
            }
        }
        printf("half rate: %ld samples, %ld mismatches\n", t, mismatches - before);

        /* and back to back, cut off at different points */
        before = mismatches;
        t = 0;
        adlib_reset();
        ref_reset();
        for (int pass = 0; pass < 2; pass++)
            for (int s = 0; s < nsounds; s++) {
                const long buffers = 1 + (long)((s * 7919u + pass * 104729u) % 120);
                adlib_play(chunks[s], sizes[s]);
                ref_play(chunks[s], sizes[s]);
                for (long k = 0; k < buffers; k++) {
                    /* once the synth has gone quiet it stops and settles
                     * its envelopes, where Nuked rings on inaudibly; start
                     * both afresh */
                    if (!adlib_active()) {
                        adlib_reset();
                        ref_reset();
                        break;
                    }
                    adlib_generate(buf, 512);
                    for (int i = 0; i < 512; i++) {
                        int v;
                        ref_sample();
                        v = ref_sample() * 3;
                        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                        compare(t++, buf[i], v, "half rate, back to back");
                    }
                }
            }
        printf("half rate, back to back: %ld samples, %ld mismatches\n", t,
               mismatches - before);

        c0 = clock();
        for (int rep = 0; rep < 20; rep++)
            for (int s = 0; s < nsounds; s++) {
                adlib_reset();
                adlib_play(chunks[s], sizes[s]);
                for (long k = 0; adlib_active() && k < 10L * 24858 / 512; k++) {
                    adlib_generate(buf, 512);
                    n += 512;
                }
            }
        secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
        printf("speed: %.1f ns per output sample on this host (%.0fx real time)\n",
               secs * 1e9 / n, n / ADLIB_RATE / secs);
    }
    return mismatches != 0;
}
