/* imf2xm.c - turn the AdLib music into XM modules for libdragon's xm64.
 *
 * render_audio.c records each song as it sounds: exact, and about 8 MB of
 * the ROM. The songs themselves are small - SDL_ALService()'s (register,
 * value, delay) triples at 700 Hz - and they use the OPL2 the way a MIDI
 * synthesizer would: eight channels of one note each, every note on an
 * equal-tempered pitch, and nothing about a note but its envelope changing
 * once it has started. So a song can be played back from recorded
 * instruments instead:
 *
 *   notes        Each key-on becomes an XM note on the same channel, with
 *                the pitch from the F-number and block, starting on the row
 *                nearest its time. At 255 BPM and speed 1 a row is 1/102 s,
 *                the finest XM allows, so notes move by up to 5 ms.
 *   loudness     A note's loudness over time is the carrier's envelope - the
 *                attack, the decay, the sustain, and the release from
 *                wherever the key-off caught it - plus its total level. The
 *                converter runs the patch through Nuked-OPL3 with the note's
 *                own key-on and key-off times and writes the result into the
 *                volume column, row by row, for as long as the note is heard.
 *   instruments  Each distinct patch (both operators' registers and the
 *                channel's feedback and connection, less the carrier's total
 *                level) is an XM instrument with a sample for each octave
 *                its notes use, recorded at a pitch in that octave so key
 *                scaling follows the pitch as on the chip. A sample is the
 *                first 0.2 s of a note with the carrier's envelope divided
 *                back out - the attack's changing timbre at an even level -
 *                then a loop of whole waveform periods. The chip's 49716 Hz
 *                output is resampled to a rate with a whole number of
 *                samples a period, so the loop is seamless.
 *
 * Vibrato and tremolo are left out: the game never sets the deep settings,
 * so they are 7 cents and 1 dB, and a loop long enough for their cycle
 * would cost far more than they are worth.
 *
 *   imf2xm <assets/audio> <out dir>             writes <out>/mNN.xm
 *   imf2xm --check <assets/audio> <song> [patch]
 *                                               plays the module back here
 *                                               and compares it to the chip
 */
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nuked-opl3/opl3.h"

#define OPL_RATE       49716
#define IMF_HZ         700
#define XM_BPM         255
#define ROWS_PER_SEC   (XM_BPM * 2.0 / 5.0)     /* speed 1: a row a tick */
#define ROW_SAMPLES    ((int)lround(OPL_RATE / ROWS_PER_SEC))
#define PATTERN_ROWS   256
#define CHANNELS       8                        /* OPL channels 1-8        */
#define SAMPLE_GAIN    2.5                      /* render_audio's GAIN_MUSIC */
#define TARGET_RATE    22050.0                  /* about; see resample_rate() */
#define HEAD_SEC       0.2                      /* recorded before the loop */
#define LOOP_SEC       0.03                     /* at least, in whole periods */
#define RELEASE_ROWS   400                      /* longest release followed */
#define EG_DB          0.1875                   /* dB a step of the envelope */

static const int reg_slot[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };   /* 0x20+ */

/* ------------------------------------------------------------------ */
/* The song as notes                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t r[11];      /* mod 20 40 60 80 e0, car 20 (40&c0) 60 80 e0, c0 */
} patch_t;

typedef struct {
    int  ch;                                    /* 0..8                  */
    int  patch;
    int  block, fnum;
    int  note;                                  /* XM note, 1..96        */
    int  tl;                                    /* carrier total level   */
    long on, off;                               /* 700 Hz ticks          */
} note_t;

static patch_t patches[128];
static int     npatches;
static note_t *notes;
static int     nnotes, notes_cap;
static long    song_ticks;

static int find_patch(const uint8_t reg[256], int ch)
{
    const int m = reg_slot[ch];
    patch_t p;
    const uint8_t r[11] = {
        reg[0x20 + m], reg[0x40 + m], reg[0x60 + m], reg[0x80 + m], reg[0xe0 + m],
        reg[0x23 + m], reg[0x43 + m] & 0xc0, reg[0x63 + m], reg[0x83 + m],
        reg[0xe3 + m], reg[0xc0 + ch] & 0x0f
    };
    memcpy(p.r, r, sizeof r);
    for (int i = 0; i < npatches; i++)
        if (!memcmp(patches[i].r, p.r, sizeof p.r))
            return i;
    if (npatches == 128) {
        fprintf(stderr, "more than 128 patches\n");
        exit(1);
    }
    patches[npatches] = p;
    return npatches++;
}

static double opl_freq(int block, int fnum)
{
    return fnum * (double)OPL_RATE / (double)(1 << (20 - block));
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

/* A note's patch, pitch and level are what the registers hold once every
 * write at its instant has been made, so a note is filled in at the next
 * delay. */
static void finish_notes(const uint8_t reg[256], int pending[9])
{
    for (int ch = 0; ch < 9; ch++) {
        note_t *n;
        double f;
        if (pending[ch] < 0)
            continue;
        n = &notes[pending[ch]];
        n->patch = find_patch(reg, ch);
        n->block = (reg[0xb0 + ch] >> 2) & 7;
        n->fnum = reg[0xa0 + ch] | (reg[0xb0 + ch] & 3) << 8;
        f = opl_freq(n->block, n->fnum);
        n->note = f > 0 ? (int)lround(12 * log2(f / 440.0)) + 58 : 1;
        if (n->note < 1) n->note = 1;
        if (n->note > 96) n->note = 96;
        n->tl = reg[0x43 + reg_slot[ch]] & 0x3f;
        pending[ch] = -1;
    }
}

static void parse_song(const unsigned char *d, long len)
{
    uint8_t reg[256] = { 0 };
    int current[9], pending[9];
    long t = 0;

    for (int ch = 0; ch < 9; ch++)
        current[ch] = pending[ch] = -1;
    nnotes = 0;

    for (long i = 0; i + 4 <= len; i += 4) {
        const int r = d[i], v = d[i + 1];
        const long delay = d[i + 2] | d[i + 3] << 8;
        const int was_on = (r >= 0xb0 && r <= 0xb8) ? (reg[r] & 0x20) : 0;

        reg[r] = (uint8_t)v;
        if (r >= 0xb0 && r <= 0xb8) {
            const int ch = r - 0xb0;
            const int on = v & 0x20;
            if (on && !was_on) {
                if (nnotes == notes_cap) {
                    notes_cap = notes_cap ? notes_cap * 2 : 1024;
                    notes = realloc(notes, notes_cap * sizeof *notes);
                }
                notes[nnotes].ch = ch;
                notes[nnotes].on = t;
                notes[nnotes].off = -1;
                pending[ch] = current[ch] = nnotes++;
            } else if (!on && was_on && current[ch] >= 0) {
                notes[current[ch]].off = t;
                current[ch] = -1;
            }
        }
        if (delay)
            finish_notes(reg, pending);
        t += delay;
    }
    finish_notes(reg, pending);
    for (int ch = 0; ch < 9; ch++)
        if (current[ch] >= 0)
            notes[current[ch]].off = t;
    song_ticks = t;
}

static long to_row(long ticks)
{
    return lround(ticks * ROWS_PER_SEC / IMF_HZ);
}

/* ------------------------------------------------------------------ */
/* The chip                                                            */
/* ------------------------------------------------------------------ */

/* The patch on channel 0 at total level 0, keyed on; the vibrato and
 * tremolo bits cleared. */
static void chip_start(opl3_chip *chip, const patch_t *p, int block, int fnum)
{
    OPL3_Reset(chip, OPL_RATE);
    for (int r = 1; r <= 0xf5; r++)
        OPL3_WriteReg(chip, (uint16_t)r, 0);
    OPL3_WriteReg(chip, 0x01, 0x20);
    OPL3_WriteReg(chip, 0x20, p->r[0] & 0x3f);
    OPL3_WriteReg(chip, 0x40, p->r[1]);
    OPL3_WriteReg(chip, 0x60, p->r[2]);
    OPL3_WriteReg(chip, 0x80, p->r[3]);
    OPL3_WriteReg(chip, 0xe0, p->r[4]);
    OPL3_WriteReg(chip, 0x23, p->r[5] & 0x3f);
    OPL3_WriteReg(chip, 0x43, p->r[6]);
    OPL3_WriteReg(chip, 0x63, p->r[7]);
    OPL3_WriteReg(chip, 0x83, p->r[8]);
    OPL3_WriteReg(chip, 0xe3, p->r[9]);
    OPL3_WriteReg(chip, 0xc0, p->r[10]);
    OPL3_WriteReg(chip, 0xa0, (uint8_t)fnum);
    OPL3_WriteReg(chip, 0xb0, (uint8_t)(0x20 | block << 2 | fnum >> 8));
}

static int16_t chip_sample(opl3_chip *chip)
{
    int16_t buf[4];
    OPL3_Generate(chip, buf);
    return buf[0];
}

/* channel 0's carrier is Nuked's slot 3; its envelope, 0 loud .. 511 */
static int carrier_eg(const opl3_chip *chip)
{
    return chip->slot[3].eg_rout;
}

/* ------------------------------------------------------------------ */
/* Samples                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int      patch, ref_note, block, fnum;
    int16_t *data;
    int      length, loop_start, loop_length;
    double   rate;
    int      relnote, finetune;
} sample_t;

static double resample_rate(double freq, int *period)
{
    int p = (int)lround(TARGET_RATE / freq);
    if (p < 4)
        p = 4;
    *period = p;
    return p * freq;
}

static void resample(const double *in, long in_len, int16_t *out, int out_len,
                     double rate)
{
    const double step = OPL_RATE / rate;
    const double cutoff = rate < OPL_RATE ? rate / OPL_RATE : 1.0;
    const int taps = 24;

    for (int i = 0; i < out_len; i++) {
        const double x = i * step;
        const long c = (long)floor(x);
        double sum = 0, wsum = 0;
        for (long k = c - taps + 1; k <= c + taps; k++) {
            const double t = x - k;
            const double arg = M_PI * t * cutoff;
            const double s = fabs(arg) < 1e-9 ? 1.0 : sin(arg) / arg;
            const double w = 0.42 + 0.5 * cos(M_PI * t / taps)
                           + 0.08 * cos(2 * M_PI * t / taps);
            const double h = s * (fabs(t) < taps ? w : 0);
            if (k >= 0 && k < in_len)
                sum += in[k] * h;
            wsum += h;
        }
        sum = wsum != 0 ? sum / wsum : 0;
        out[i] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : lround(sum));
    }
}

static void record_sample(sample_t *s)
{
    const patch_t *p = &patches[s->patch];
    const double freq = opl_freq(s->block, s->fnum);
    long native_len;
    double *native;
    opl3_chip chip;
    int period, m;

    s->rate = resample_rate(freq, &period);
    m = (int)ceil(LOOP_SEC * freq);
    s->loop_start = (int)lround(HEAD_SEC * s->rate);
    s->loop_length = m * period;
    s->length = s->loop_start + s->loop_length;

    /* the note with the carrier's envelope divided out, at most 30 dB of
     * make-up where the carrier is still almost silent */
    native_len = (long)ceil(s->length * (double)OPL_RATE / s->rate) + 64;
    native = malloc(native_len * sizeof *native);
    chip_start(&chip, p, s->block, s->fnum);
    for (long i = 0; i < native_len; i++) {
        double v = chip_sample(&chip), db = carrier_eg(&chip) * EG_DB;
        if (db > 30)
            db = 30;
        native[i] = v * pow(10, db / 20) * SAMPLE_GAIN;
    }

    s->data = malloc(s->length * sizeof *s->data);
    resample(native, native_len, s->data, s->length, s->rate);
    free(native);

    /* the modulator may still be moving where the loop is taken; even out
     * the loop's level so it repeats without a step */
    {
        double first = 0, last = 0, ratio;
        const int a = s->loop_start, b = s->length - period;
        for (int i = 0; i < period; i++) {
            first += (double)s->data[a + i] * s->data[a + i];
            last += (double)s->data[b + i] * s->data[b + i];
        }
        ratio = last > 0 ? sqrt(first / last) : 1;
        if (ratio > 2) ratio = 2;
        if (ratio < 0.5) ratio = 0.5;
        for (int i = 0; i < s->loop_length; i++) {
            const double g = 1 + (ratio - 1) * i / s->loop_length;
            const double v = s->data[a + i] * g;
            s->data[a + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lround(v));
        }
    }

    /* XM plays note N at 8363 Hz * 2^((N - 49 + relnote + finetune/128)/12);
     * make ref_note come out at its equal-tempered pitch */
    {
        const double want = 440.0 * pow(2, (s->ref_note - 58) / 12.0);
        const double play_rate = s->rate * want / freq;
        const double semis = 12 * log2(play_rate / 8363.0) - (s->ref_note - 49);
        int total = (int)lround(semis * 128);
        s->relnote = (int)floor(total / 128.0);
        s->finetune = total - s->relnote * 128;
    }
}

/* ------------------------------------------------------------------ */
/* Loudness: the carrier's envelope, note by note                      */
/* ------------------------------------------------------------------ */

/* The level of a note of `patch` held for `held` rows, for `total` rows,
 * as XM volumes. Only the key scaling rate's input - the block and the
 * top bit of the F-number - changes the envelope within a patch, so
 * results are remembered by that and the hold time. */
typedef struct {
    int      patch, ksr, held, total;
    uint8_t *vol;                               /* 0..64, at total level 0 */
} levels_t;

static levels_t *level_cache;
static int       nlevels, levels_cap;

static const uint8_t *note_levels(const note_t *n, int held, int total)
{
    const int ksr = n->block << 1 | (n->fnum >> 9);
    levels_t *l;
    opl3_chip chip;

    for (int i = 0; i < nlevels; i++) {
        l = &level_cache[i];
        if (l->patch == n->patch && l->ksr == ksr && l->held == held
            && l->total >= total)
            return l->vol;
    }
    if (nlevels == levels_cap) {
        levels_cap = levels_cap ? levels_cap * 2 : 1024;
        level_cache = realloc(level_cache, levels_cap * sizeof *level_cache);
    }
    l = &level_cache[nlevels++];
    l->patch = n->patch;
    l->ksr = ksr;
    l->held = held;
    l->total = total;
    l->vol = malloc(total);

    chip_start(&chip, &patches[n->patch], n->block, n->fnum);
    for (int r = 0; r < total; r++) {
        double db;
        int v;
        if (r == held)
            OPL3_WriteReg(&chip, 0xb0, (uint8_t)(n->block << 2 | n->fnum >> 8));
        for (int i = 0; i < ROW_SAMPLES / 2; i++)
            chip_sample(&chip);
        db = carrier_eg(&chip) * EG_DB;         /* the middle of the row */
        for (int i = ROW_SAMPLES / 2; i < ROW_SAMPLES; i++)
            chip_sample(&chip);
        v = (int)lround(64 * pow(10, -db / 20));
        l->vol[r] = (uint8_t)(v > 64 ? 64 : v);
    }
    return l->vol;
}

/* ------------------------------------------------------------------ */
/* The module                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int     nsamples;
    int     sample[8];
    uint8_t keymap[96];
} instrument_t;

static sample_t     samples[1024];
static int          nsamples;
static instrument_t instruments[128];

static void build_instruments(void)
{
    nsamples = 0;
    memset(instruments, 0, sizeof instruments);

    for (int p = 0; p < npatches; p++) {
        instrument_t *in = &instruments[p];
        int counts[96] = { 0 }, first = -1, last = -1;

        for (int i = 0; i < nnotes; i++)
            if (notes[i].patch == p)
                counts[notes[i].note - 1]++;

        for (int o = 0; o < 8; o++) {
            int best = -1, bestc = 0;
            sample_t *s;
            for (int k = o * 12; k < o * 12 + 12; k++)
                if (counts[k] > bestc) {
                    bestc = counts[k];
                    best = k;
                }
            if (best < 0)
                continue;
            s = &samples[nsamples];
            memset(s, 0, sizeof *s);
            s->patch = p;
            s->ref_note = best + 1;
            for (int i = 0; i < nnotes; i++)    /* the song's own F-number */
                if (notes[i].patch == p && notes[i].note == best + 1) {
                    s->block = notes[i].block;
                    s->fnum = notes[i].fnum;
                    break;
                }
            record_sample(s);
            for (int k = o * 12; k < o * 12 + 12; k++)
                in->keymap[k] = (uint8_t)in->nsamples;
            if (first < 0)
                first = o;
            last = o;
            in->sample[in->nsamples++] = nsamples++;
        }
        /* octaves without notes take the nearest sample below, or above */
        for (int k = 0; k < 96; k++) {
            const int o = k / 12;
            int any = 0;
            for (int j = o * 12; j < o * 12 + 12; j++)
                any |= counts[j] != 0;
            if (any)
                continue;
            if (first < 0 || o < first)
                in->keymap[k] = 0;
            else if (o > last)
                in->keymap[k] = (uint8_t)(in->nsamples - 1);
            else
                in->keymap[k] = in->keymap[k - 1];
        }
    }
}

typedef struct {
    uint8_t note, inst, vol;                    /* vol: 0 none, else 0x10 + v */
} cell_t;

static cell_t *grid;
static long    rows;

static void build_grid(void)
{
    /* the song loops the moment it ends, so the last pattern is only as
     * long as what is left */
    rows = to_row(song_ticks);
    if (rows < 1)
        rows = 1;
    grid = calloc((rows + PATTERN_ROWS - 1) / PATTERN_ROWS * PATTERN_ROWS * CHANNELS,
                  sizeof *grid);

    for (int i = 0; i < nnotes; i++) {
        const note_t *n = &notes[i];
        const long on = to_row(n->on);
        long held = to_row(n->off) - on, until = rows, last_vol = -1;
        const double tl_gain = pow(10, -0.75 * n->tl / 20);
        const uint8_t *vol;
        int total;

        if (n->ch < 1 || n->ch > CHANNELS || on >= rows)
            continue;
        if (held < 1)
            held = 1;
        /* heard until the channel's next note, or the release has ended */
        for (int j = i + 1; j < nnotes; j++)
            if (notes[j].ch == n->ch) {
                until = to_row(notes[j].on);
                break;
            }
        if (until > on + held + RELEASE_ROWS)
            until = on + held + RELEASE_ROWS;
        if (until > rows)
            until = rows;
        if (until <= on)
            continue;
        total = (int)(until - on);
        vol = note_levels(n, (int)held, total);

        for (long r = on; r < until; r++) {
            cell_t *c = &grid[r * CHANNELS + (n->ch - 1)];
            long v = lround(vol[r - on] * tl_gain);
            if (r == on) {
                c->note = (uint8_t)n->note;
                c->inst = (uint8_t)(n->patch + 1);
            }
            if (v != last_vol) {
                c->vol = (uint8_t)(0x10 + v);
                last_vol = v;
            }
            if (v == 0 && r >= on + held)
                break;                          /* released and silent */
        }
    }
}

static void put8(FILE *f, int v) { fputc(v & 0xff, f); }
static void put16(FILE *f, int v) { put8(f, v); put8(f, v >> 8); }
static void put32(FILE *f, long v) { put16(f, (int)(v & 0xffff)); put16(f, (int)(v >> 16)); }
static void putstr(FILE *f, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        put8(f, *s ? *s++ : 0);
}

static long write_xm(const char *path, const char *name, long *sample_bytes)
{
    FILE *f = fopen(path, "wb");
    const int npat = (int)((rows + PATTERN_ROWS - 1) / PATTERN_ROWS);
    long size;

    if (!f) { perror(path); exit(1); }
    *sample_bytes = 0;
    fwrite("Extended Module: ", 1, 17, f);
    putstr(f, name, 20);
    put8(f, 0x1a);
    putstr(f, "imf2xm", 20);
    put16(f, 0x0104);
    put32(f, 276);
    put16(f, npat);
    put16(f, 0);
    put16(f, CHANNELS);
    put16(f, npat);
    put16(f, npatches);
    put16(f, 1);                                /* linear frequencies */
    put16(f, 1);                                /* speed */
    put16(f, XM_BPM);
    for (int i = 0; i < 256; i++)
        put8(f, i < npat ? i : 0);

    for (int p = 0; p < npat; p++) {
        const int prows = (int)(rows - (long)p * PATTERN_ROWS < PATTERN_ROWS
                                ? rows - (long)p * PATTERN_ROWS : PATTERN_ROWS);
        uint8_t *buf = malloc(PATTERN_ROWS * CHANNELS * 4);
        int len = 0;
        for (int r = 0; r < prows; r++)
            for (int ch = 0; ch < CHANNELS; ch++) {
                const cell_t *c = &grid[((long)p * PATTERN_ROWS + r) * CHANNELS + ch];
                uint8_t flags = 0x80;
                if (c->note) flags |= 1;
                if (c->inst) flags |= 2;
                if (c->vol) flags |= 4;
                buf[len++] = flags;
                if (c->note) buf[len++] = c->note;
                if (c->inst) buf[len++] = c->inst;
                if (c->vol) buf[len++] = c->vol;
            }
        put32(f, 9);
        put8(f, 0);
        put16(f, prows);
        put16(f, len);
        fwrite(buf, 1, len, f);
        free(buf);
    }

    for (int p = 0; p < npatches; p++) {
        const instrument_t *in = &instruments[p];
        char iname[23];
        long start = ftell(f);
        snprintf(iname, sizeof iname, "patch %d", p);
        put32(f, 263);
        putstr(f, iname, 22);
        put8(f, 0);
        put16(f, in->nsamples);
        if (in->nsamples) {
            put32(f, 40);
            fwrite(in->keymap, 1, 96, f);       /* no envelopes, no fadeout */
        }
        while (ftell(f) - start < 263)
            put8(f, 0);
        for (int k = 0; k < in->nsamples; k++) {
            const sample_t *s = &samples[in->sample[k]];
            put32(f, (long)s->length * 2);
            put32(f, (long)s->loop_start * 2);
            put32(f, (long)s->loop_length * 2);
            put8(f, 64);
            put8(f, s->finetune);
            put8(f, 1 | 0x10);                  /* forward loop, 16-bit */
            put8(f, 128);
            put8(f, s->relnote);
            put8(f, 0);
            putstr(f, "", 22);
        }
        for (int k = 0; k < in->nsamples; k++) {
            const sample_t *s = &samples[in->sample[k]];
            int prev = 0;
            for (int i = 0; i < s->length; i++) {
                put16(f, (int16_t)(s->data[i] - prev));
                prev = s->data[i];
            }
            *sample_bytes += (long)s->length * 2;
        }
    }
    size = ftell(f);
    fclose(f);
    return size;
}

static void reset_song(void)
{
    for (int i = 0; i < nsamples; i++)
        free(samples[i].data);
    for (int i = 0; i < nlevels; i++)
        free(level_cache[i].vol);
    nsamples = npatches = nnotes = nlevels = 0;
    free(grid);
    grid = NULL;
}

/* ------------------------------------------------------------------ */
/* Checking: the module played back here, against the chip             */
/* ------------------------------------------------------------------ */

#define CHECK_RATE 22050

static int solo = -1;

static double *render_chip(const unsigned char *d, long len, long *out_len)
{
    opl3_chip chip;
    long frac = 0, n = 0, cap = CHECK_RATE * 8;
    double *out = malloc(cap * sizeof *out);
    uint8_t reg[256] = { 0 };

    OPL3_Reset(&chip, CHECK_RATE);
    for (int r = 1; r <= 0xf5; r++)
        OPL3_WriteReg(&chip, (uint16_t)r, 0);
    OPL3_WriteReg(&chip, 0x01, 0x20);
    for (long i = 0; i + 4 <= len; i += 4) {
        long ticks = d[i + 2] | d[i + 3] << 8;
        const int r = d[i], v = d[i + 1];
        reg[r] = (uint8_t)v;
        OPL3_WriteReg(&chip, (uint16_t)r, (uint8_t)v);
        if (solo >= 0 && ticks) {
            /* a channel whose patch is not the one asked for is silenced */
            for (int ch = 0; ch < 9; ch++) {
                const int m = reg_slot[ch];
                OPL3_WriteReg(&chip, (uint16_t)(0x43 + m),
                              find_patch(reg, ch) == solo ? reg[0x43 + m]
                                                          : (reg[0x43 + m] | 0x3f));
            }
        }
        while (ticks-- > 0) {
            frac += CHECK_RATE;
            while (frac >= IMF_HZ) {
                int16_t buf[2];
                frac -= IMF_HZ;
                OPL3_GenerateResampled(&chip, buf);
                if (n == cap) {
                    cap *= 2;
                    out = realloc(out, cap * sizeof *out);
                }
                out[n++] = buf[0] * SAMPLE_GAIN;
            }
        }
    }
    *out_len = n;
    return out;
}

/* notes, the volume column, the keymap, forward loops, linear frequencies */
static double *render_xm(long *out_len)
{
    const long spt = lround(CHECK_RATE * 2.5 / XM_BPM);
    const long n = rows * spt;
    double *out = calloc(n, sizeof *out);
    struct {
        const sample_t *s;
        double pos, step, vol;
    } ch[CHANNELS] = { { 0 } };

    for (long r = 0; r < rows; r++) {
        for (int c = 0; c < CHANNELS; c++) {
            const cell_t *cell = &grid[r * CHANNELS + c];
            if (cell->note) {
                const instrument_t *in = &instruments[cell->inst - 1];
                const sample_t *s = &samples[in->sample[in->keymap[cell->note - 1]]];
                const double semis = cell->note - 49 + s->relnote + s->finetune / 128.0;
                ch[c].s = (solo < 0 || cell->inst - 1 == solo) ? s : NULL;
                ch[c].pos = 0;
                ch[c].step = 8363.0 * pow(2, semis / 12) / CHECK_RATE;
            }
            if (cell->vol)
                ch[c].vol = (cell->vol - 0x10) / 64.0;
        }
        for (int c = 0; c < CHANNELS; c++) {
            const sample_t *s = ch[c].s;
            if (!s || ch[c].vol <= 0)
                continue;
            for (long i = 0; i < spt; i++) {
                long ip = (long)ch[c].pos;
                if (ip >= s->length) {
                    ch[c].pos -= s->loop_length;
                    ip = (long)ch[c].pos;
                }
                out[r * spt + i] += s->data[ip] * ch[c].vol;
                ch[c].pos += ch[c].step;
            }
        }
    }
    *out_len = n;
    return out;
}

static void check_song(const char *dir, const char *song)
{
    char path[512];
    long len, a_len, b_len, w;
    unsigned char *d;
    double *a, *b, ra = 0, rb = 0, sab = 0;
    const int win = CHECK_RATE / 20;

    snprintf(path, sizeof path, "%s/%s.imf", dir, song);
    d = read_file(path, &len);
    parse_song(d, len);
    build_instruments();
    build_grid();
    a = render_chip(d, len, &a_len);
    b = render_xm(&b_len);

    w = (a_len < b_len ? a_len : b_len) / win;
    for (long k = 0; k < w; k++) {
        double ea = 0, eb = 0;
        for (int i = 0; i < win; i++) {
            ea += a[k * win + i] * a[k * win + i];
            eb += b[k * win + i] * b[k * win + i];
        }
        ea = sqrt(ea / win);
        eb = sqrt(eb / win);
        ra += ea * ea;
        rb += eb * eb;
        sab += ea * eb;
    }
    printf("  %s%s: chip rms %.0f, module rms %.0f (ratio %.2f), loudness correlation %.3f\n",
           song, solo >= 0 ? " (one patch)" : "", sqrt(ra / w), sqrt(rb / w),
           sqrt(rb / ra), sab / sqrt(ra * rb));

    /* both renderings as raw 16-bit mono at CHECK_RATE, to listen to */
    mkdir("build", 0755);
    mkdir("build/check", 0755);
    for (int k = 0; k < 2; k++) {
        const double *src = k ? b : a;
        const long n = k ? b_len : a_len;
        FILE *f;
        snprintf(path, sizeof path, "build/check/%s_%s.raw", song, k ? "xm" : "chip");
        f = fopen(path, "wb");
        if (!f) { perror(path); exit(1); }
        for (long i = 0; i < n; i++) {
            const double v = src[i];
            const int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            fwrite(&s, 2, 1, f);
        }
        fclose(f);
    }
    free(a);
    free(b);
    free(d);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    DIR *dir;
    struct dirent *e;
    long total = 0, total_samples = 0;
    int count = 0;

    if (argc >= 4 && !strcmp(argv[1], "--check")) {
        if (argc == 5)
            solo = atoi(argv[4]);
        check_song(argv[2], argv[3]);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: imf2xm <assets/audio> <out dir>\n"
                        "       imf2xm --check <assets/audio> <song> [patch]\n");
        return 1;
    }
    mkdir(argv[2], 0755);
    dir = opendir(argv[1]);
    if (!dir) { perror(argv[1]); return 1; }
    while ((e = readdir(dir)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        char in[512], out[512], base[64];
        unsigned char *d;
        long len, size, sbytes;
        if (!dot || strcmp(dot, ".imf"))
            continue;
        snprintf(base, sizeof base, "%.*s", (int)(dot - e->d_name), e->d_name);
        snprintf(in, sizeof in, "%s/%s", argv[1], e->d_name);
        snprintf(out, sizeof out, "%s/%s.xm", argv[2], base);
        d = read_file(in, &len);
        parse_song(d, len);
        build_instruments();
        build_grid();
        size = write_xm(out, base, &sbytes);
        printf("  %-4s %5.1f s  %4d notes  %2d patches  %2d samples  %5ld rows"
               "  %4ld KB (samples %ld KB)\n", base, song_ticks / (double)IMF_HZ,
               nnotes, npatches, nsamples, rows, size / 1024, sbytes / 1024);
        total += size;
        total_samples += sbytes;
        count++;
        reset_song();
        free(d);
    }
    closedir(dir);
    printf("%d songs, %ld KB of XM, %ld KB of it samples\n", count, total / 1024,
           total_samples / 1024);
    return 0;
}
