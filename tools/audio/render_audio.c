/* render_audio.c - play the AdLib music and sound effects into WAV files.
 *
 * The DOS game drove an OPL2 in real time. The N64 has no FM chip, so this
 * does the driving at build time, through Nuked-OPL3, for builds that play
 * back PCM: MUSIC=wav and SFX=wav. By default the music is sequenced instead
 * (imf2xm.c) and the effects synthesized on the N64 (src/adlib.c), and these
 * renders go unused. The register traffic reproduces ID_SD.C:
 *
 *   music   SDL_ALService(): (register, value, delay) triples at 700 Hz,
 *           looping forever. The song is played twice and the second pass
 *           kept, so the loop point starts from the state the first pass
 *           left the chip in, and wraps without a click.
 *   effects SDL_ALPlaySound() then SDL_ALSoundService(): an instrument on
 *           channel 0, then one byte per 140 Hz tick - 0 keys the note off,
 *           anything else is the F-number with the sound's block and key-on.
 *           Rendering carries on past the last byte until the release has
 *           died away.
 *
 * Nuked-OPL3 is LGPL 2.1+ (tools/audio/nuked-opl3). Here it only runs at
 * build time; src/adlib.c, which is cut down from it, is linked into the ROM.
 *
 *   render_audio <assets/audio> <out dir>      writes <out>/music, <out>/sfx
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nuked-opl3/opl3.h"

#define RATE         22050
#define MUSIC_HZ     700
#define SFX_HZ       140
#define TAIL_MAX     (RATE * 3 / 2)     /* at most 1.5 s of release       */
#define TAIL_QUIET   (RATE / 10)        /* 100 ms under the threshold ends */
#define QUIET_LEVEL  24

/* Mix levels, as gains applied to the OPL output. Rendered straight, the
 * FM music has a median RMS around 550 and the AdLib effects around 1150,
 * against about 4600 for the digitized sounds, which play back at their
 * 8-bit full scale - far quieter than a Sound Blaster's FM against its DAC.
 * One gain per kind, not per file, so songs keep their relative levels;
 * 2.5 is as much as the loudest song (WARMARCH, peak 11133) takes without
 * clipping. */
#define GAIN_MUSIC   2.5
#define GAIN_SFX     3.0

typedef struct { int16_t *s; size_t n, cap; } pcm_t;

static void apply_gain(pcm_t *p, double gain)
{
    for (size_t i = 0; i < p->n; i++) {
        double v = p->s[i] * gain;
        p->s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static void pcm_push(pcm_t *p, int16_t v)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : RATE * 4;
        p->s = realloc(p->s, p->cap * sizeof *p->s);
    }
    p->s[p->n++] = v;
}

/* One output sample. OPL2-mode output is the same on both sides. */
static int16_t generate(opl3_chip *chip)
{
    int16_t buf[2];
    OPL3_GenerateResampled(chip, buf);
    return buf[0];
}

/* Emit output for `ticks` timer ticks at `hz`, keeping the fractional
 * samples-per-tick so long songs do not drift. */
static void run_ticks(opl3_chip *chip, pcm_t *p, long ticks, int hz, long *frac)
{
    while (ticks-- > 0) {
        *frac += RATE;
        while (*frac >= hz) {
            *frac -= hz;
            if (p)
                pcm_push(p, generate(chip));
            else
                generate(chip);
        }
    }
}

static unsigned char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(*len ? *len : 1);
    if (fread(d, 1, *len, f) != (size_t)*len) { perror(path); exit(1); }
    fclose(f);
    return d;
}

static void write_wav(const char *path, const pcm_t *p)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)(p->n * 2);
    uint32_t hdr[] = { 16, 0, RATE, RATE * 2, 0 };
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f);
    { uint32_t v = 36 + data; fwrite(&v, 4, 1, f); }
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&hdr[0], 4, 1, f);
    { uint16_t fmt = 1, ch = 1; fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); }
    fwrite(&hdr[2], 4, 1, f);
    fwrite(&hdr[3], 4, 1, f);
    { uint16_t align = 2, bits = 16; fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); }
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(p->s, 2, p->n, f);   /* the build container is little-endian */
    fclose(f);
}

/* SDL_DetectAdLib() leaves every register zero, WSE on and CSM off; then
 * SDL_StartAL() clears the effects register. */
static void reset_chip(opl3_chip *chip)
{
    OPL3_Reset(chip, RATE);
    for (int r = 1; r <= 0xf5; r++)
        OPL3_WriteReg(chip, (uint16_t)r, 0);
    OPL3_WriteReg(chip, 0x01, 0x20);
    OPL3_WriteReg(chip, 0x08, 0x00);
    OPL3_WriteReg(chip, 0xbd, 0x00);
}

static void report(const char *name, const pcm_t *p)
{
    long peak = 0;
    double sum = 0;
    for (size_t i = 0; i < p->n; i++) {
        long v = labs((long)p->s[i]);
        if (v > peak) peak = v;
        sum += (double)p->s[i] * p->s[i];
    }
    printf("  %-8s %6.2f s  peak %5ld  rms %6.0f\n", name,
           (double)p->n / RATE, peak, p->n ? __builtin_sqrt(sum / p->n) : 0.0);
}

/* ------------------------------------------------------------------ */

static void render_music(const char *in, const char *out, const char *name)
{
    long len, frac = 0;
    unsigned char *d = read_file(in, &len);
    opl3_chip chip;
    pcm_t pcm = { 0 };

    reset_chip(&chip);
    for (int pass = 0; pass < 2; pass++) {
        for (long i = 0; i + 4 <= len; i += 4) {
            OPL3_WriteReg(&chip, d[i], d[i + 1]);
            run_ticks(&chip, pass ? &pcm : NULL, d[i + 2] | (d[i + 3] << 8),
                      MUSIC_HZ, &frac);
        }
    }
    /* VADPCM codes 16-sample frames and wav64 insists a loop is made of
     * whole frames, so drop the last few samples - under a millisecond -
     * rather than have the loop point land mid-frame. */
    pcm.n -= pcm.n % 16;

    apply_gain(&pcm, GAIN_MUSIC);
    write_wav(out, &pcm);
    report(name, &pcm);
    free(pcm.s);
    free(d);
}

static void write_fx_inst(opl3_chip *chip, const unsigned char *inst)
{
    /* SDL_AlSetFXInst(): modulator cell 0, carrier cell 3, and 0 for
     * feedback/connection, "for old MUSE compatibility". Instrument byte
     * order is mChar cChar mScale cScale mAttack cAttack mSus cSus mWave
     * cWave (ID_SD.H). */
    static const unsigned char reg[5] = { 0x20, 0x40, 0x60, 0x80, 0xe0 };
    for (int k = 0; k < 5; k++) {
        OPL3_WriteReg(chip, reg[k] + 0, inst[k * 2]);
        OPL3_WriteReg(chip, reg[k] + 3, inst[k * 2 + 1]);
    }
    OPL3_WriteReg(chip, 0xc0, 0);
}

static void render_sfx(const char *in, const char *out, const char *name)
{
    static const unsigned char zero_inst[16];
    long len, frac = 0, length, quiet = 0, tail = 0;
    unsigned char *d = read_file(in, &len);
    opl3_chip chip;
    pcm_t pcm = { 0 };
    unsigned char block;

    if (len < 24) {
        fprintf(stderr, "%s: too short for an AdLib sound\n", in);
        exit(1);
    }
    length = d[0] | (d[1] << 8) | (d[2] << 16) | ((long)d[3] << 24);
    block  = (unsigned char)(((d[6 + 16] & 7) << 2) | 0x20);
    if (length > len - 23)
        length = len - 23;

    reset_chip(&chip);
    write_fx_inst(&chip, zero_inst);
    write_fx_inst(&chip, d + 6);

    for (long i = 0; i < length; i++) {
        unsigned char s = d[23 + i];
        if (!s) {
            OPL3_WriteReg(&chip, 0xb0, 0);
        } else {
            OPL3_WriteReg(&chip, 0xa0, s);
            OPL3_WriteReg(&chip, 0xb0, block);
        }
        run_ticks(&chip, &pcm, 1, SFX_HZ, &frac);
    }
    OPL3_WriteReg(&chip, 0xb0, 0);

    while (tail < TAIL_MAX && quiet < TAIL_QUIET) {
        int16_t v = generate(&chip);
        pcm_push(&pcm, v);
        quiet = (v > -QUIET_LEVEL && v < QUIET_LEVEL) ? quiet + 1 : 0;
        tail++;
    }
    /* drop the silent run we waited through, keeping a few ms */
    if (quiet >= TAIL_QUIET && pcm.n > (size_t)(quiet - RATE / 200))
        pcm.n -= quiet - RATE / 200;

    apply_gain(&pcm, GAIN_SFX);
    write_wav(out, &pcm);
    if (!strcmp(name, "al18") || !strcmp(name, "al00") || !strcmp(name, "al24"))
        report(name, &pcm);
    free(pcm.s);
    free(d);
}

int main(int argc, char **argv)
{
    char in[1024], out[1024];
    struct dirent *e;
    DIR *dir;
    int nmusic = 0, nsfx = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: render_audio <assets/audio> <out dir>\n");
        return 1;
    }
    snprintf(out, sizeof out, "%s/music", argv[2]); mkdir(argv[2], 0755); mkdir(out, 0755);
    snprintf(out, sizeof out, "%s/sfx", argv[2]);   mkdir(out, 0755);

    dir = opendir(argv[1]);
    if (!dir) { perror(argv[1]); return 1; }
    printf("rendering at %d Hz (samples: a few effects, all music):\n", RATE);
    while ((e = readdir(dir))) {
        const char *dot = strrchr(e->d_name, '.');
        char base[256];
        if (!dot || dot - e->d_name >= (long)sizeof base)
            continue;
        memcpy(base, e->d_name, dot - e->d_name);
        base[dot - e->d_name] = 0;
        snprintf(in, sizeof in, "%s/%s", argv[1], e->d_name);
        if (!strcmp(dot, ".imf")) {
            snprintf(out, sizeof out, "%s/music/%s.wav", argv[2], base);
            render_music(in, out, base);
            nmusic++;
        } else if (!strcmp(dot, ".adl")) {
            snprintf(out, sizeof out, "%s/sfx/%s.wav", argv[2], base);
            render_sfx(in, out, base);
            nsfx++;
        }
    }
    closedir(dir);
    printf("%d songs, %d AdLib effects\n", nmusic, nsfx);
    return 0;
}
