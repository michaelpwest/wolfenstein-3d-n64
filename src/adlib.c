/* adlib.c - the AdLib sound effects, synthesized as the game plays.
 *
 * An AdLib effect is tiny - an instrument and one F-number byte per 140 Hz
 * tick - but rendered to PCM the 87 of them take 5 MB of cartridge. So the
 * ROM plays them the way the DOS game did: SDL_ALPlaySound() programs the
 * instrument into channel 0 of the OPL2, and SDL_ALSoundService() writes a
 * byte a tick. The chip is emulated here, and only as much of it as those
 * writes reach: one channel, two operators in series, no feedback (the
 * driver always writes 0 to the feedback/connection register), OPL2
 * waveforms, no rhythm mode, the default vibrato and tremolo depths.
 *
 * The emulation is Nuked-OPL3's, cut down to that channel, and gives the
 * same samples: tools/audio/adlib_check.c runs both side by side over every
 * effect. It runs at the chip's 49716 Hz, because the envelope generator's
 * timing is defined per chip sample, but only every second sample's output
 * is computed - the mixer takes 24858 Hz, above the 22050 Hz the rest of
 * the audio uses.
 *
 * Derived from Nuked OPL3, Copyright (C) 2013-2020 Nuke.YKT, and like it
 * licensed under the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version; see tools/audio/nuked-opl3.
 */
#include <string.h>
#include "adlib.h"

/* render_audio.c's GAIN_SFX: FM effects against the digitized sounds */
#define GAIN  3

/* ------------------------------------------------------------------ */
/* The chip's ROMs and tables                                          */
/* ------------------------------------------------------------------ */

static const uint16_t logsinrom[256] = {
    0x859, 0x6c3, 0x607, 0x58b, 0x52e, 0x4e4, 0x4a6, 0x471,
    0x443, 0x41a, 0x3f5, 0x3d3, 0x3b5, 0x398, 0x37e, 0x365,
    0x34e, 0x339, 0x324, 0x311, 0x2ff, 0x2ed, 0x2dc, 0x2cd,
    0x2bd, 0x2af, 0x2a0, 0x293, 0x286, 0x279, 0x26d, 0x261,
    0x256, 0x24b, 0x240, 0x236, 0x22c, 0x222, 0x218, 0x20f,
    0x206, 0x1fd, 0x1f5, 0x1ec, 0x1e4, 0x1dc, 0x1d4, 0x1cd,
    0x1c5, 0x1be, 0x1b7, 0x1b0, 0x1a9, 0x1a2, 0x19b, 0x195,
    0x18f, 0x188, 0x182, 0x17c, 0x177, 0x171, 0x16b, 0x166,
    0x160, 0x15b, 0x155, 0x150, 0x14b, 0x146, 0x141, 0x13c,
    0x137, 0x133, 0x12e, 0x129, 0x125, 0x121, 0x11c, 0x118,
    0x114, 0x10f, 0x10b, 0x107, 0x103, 0x0ff, 0x0fb, 0x0f8,
    0x0f4, 0x0f0, 0x0ec, 0x0e9, 0x0e5, 0x0e2, 0x0de, 0x0db,
    0x0d7, 0x0d4, 0x0d1, 0x0cd, 0x0ca, 0x0c7, 0x0c4, 0x0c1,
    0x0be, 0x0bb, 0x0b8, 0x0b5, 0x0b2, 0x0af, 0x0ac, 0x0a9,
    0x0a7, 0x0a4, 0x0a1, 0x09f, 0x09c, 0x099, 0x097, 0x094,
    0x092, 0x08f, 0x08d, 0x08a, 0x088, 0x086, 0x083, 0x081,
    0x07f, 0x07d, 0x07a, 0x078, 0x076, 0x074, 0x072, 0x070,
    0x06e, 0x06c, 0x06a, 0x068, 0x066, 0x064, 0x062, 0x060,
    0x05e, 0x05c, 0x05b, 0x059, 0x057, 0x055, 0x053, 0x052,
    0x050, 0x04e, 0x04d, 0x04b, 0x04a, 0x048, 0x046, 0x045,
    0x043, 0x042, 0x040, 0x03f, 0x03e, 0x03c, 0x03b, 0x039,
    0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02f,
    0x02e, 0x02d, 0x02b, 0x02a, 0x029, 0x028, 0x027, 0x026,
    0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01f, 0x01e,
    0x01d, 0x01c, 0x01b, 0x01a, 0x019, 0x018, 0x017, 0x017,
    0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
    0x010, 0x00f, 0x00f, 0x00e, 0x00d, 0x00d, 0x00c, 0x00c,
    0x00b, 0x00a, 0x00a, 0x009, 0x009, 0x008, 0x008, 0x007,
    0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004,
    0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
    0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000
};

static const uint16_t exprom[256] = {
    0x7fa, 0x7f5, 0x7ef, 0x7ea, 0x7e4, 0x7df, 0x7da, 0x7d4,
    0x7cf, 0x7c9, 0x7c4, 0x7bf, 0x7b9, 0x7b4, 0x7ae, 0x7a9,
    0x7a4, 0x79f, 0x799, 0x794, 0x78f, 0x78a, 0x784, 0x77f,
    0x77a, 0x775, 0x770, 0x76a, 0x765, 0x760, 0x75b, 0x756,
    0x751, 0x74c, 0x747, 0x742, 0x73d, 0x738, 0x733, 0x72e,
    0x729, 0x724, 0x71f, 0x71a, 0x715, 0x710, 0x70b, 0x706,
    0x702, 0x6fd, 0x6f8, 0x6f3, 0x6ee, 0x6e9, 0x6e5, 0x6e0,
    0x6db, 0x6d6, 0x6d2, 0x6cd, 0x6c8, 0x6c4, 0x6bf, 0x6ba,
    0x6b5, 0x6b1, 0x6ac, 0x6a8, 0x6a3, 0x69e, 0x69a, 0x695,
    0x691, 0x68c, 0x688, 0x683, 0x67f, 0x67a, 0x676, 0x671,
    0x66d, 0x668, 0x664, 0x65f, 0x65b, 0x657, 0x652, 0x64e,
    0x649, 0x645, 0x641, 0x63c, 0x638, 0x634, 0x630, 0x62b,
    0x627, 0x623, 0x61e, 0x61a, 0x616, 0x612, 0x60e, 0x609,
    0x605, 0x601, 0x5fd, 0x5f9, 0x5f5, 0x5f0, 0x5ec, 0x5e8,
    0x5e4, 0x5e0, 0x5dc, 0x5d8, 0x5d4, 0x5d0, 0x5cc, 0x5c8,
    0x5c4, 0x5c0, 0x5bc, 0x5b8, 0x5b4, 0x5b0, 0x5ac, 0x5a8,
    0x5a4, 0x5a0, 0x59c, 0x599, 0x595, 0x591, 0x58d, 0x589,
    0x585, 0x581, 0x57e, 0x57a, 0x576, 0x572, 0x56f, 0x56b,
    0x567, 0x563, 0x560, 0x55c, 0x558, 0x554, 0x551, 0x54d,
    0x549, 0x546, 0x542, 0x53e, 0x53b, 0x537, 0x534, 0x530,
    0x52c, 0x529, 0x525, 0x522, 0x51e, 0x51b, 0x517, 0x514,
    0x510, 0x50c, 0x509, 0x506, 0x502, 0x4ff, 0x4fb, 0x4f8,
    0x4f4, 0x4f1, 0x4ed, 0x4ea, 0x4e7, 0x4e3, 0x4e0, 0x4dc,
    0x4d9, 0x4d6, 0x4d2, 0x4cf, 0x4cc, 0x4c8, 0x4c5, 0x4c2,
    0x4be, 0x4bb, 0x4b8, 0x4b5, 0x4b1, 0x4ae, 0x4ab, 0x4a8,
    0x4a4, 0x4a1, 0x49e, 0x49b, 0x498, 0x494, 0x491, 0x48e,
    0x48b, 0x488, 0x485, 0x482, 0x47e, 0x47b, 0x478, 0x475,
    0x472, 0x46f, 0x46c, 0x469, 0x466, 0x463, 0x460, 0x45d,
    0x45a, 0x457, 0x454, 0x451, 0x44e, 0x44b, 0x448, 0x445,
    0x442, 0x43f, 0x43c, 0x439, 0x436, 0x433, 0x430, 0x42d,
    0x42a, 0x428, 0x425, 0x422, 0x41f, 0x41c, 0x419, 0x416,
    0x414, 0x411, 0x40e, 0x40b, 0x408, 0x406, 0x403, 0x400
};

/* frequency multipliers, times two: 1/2, 1, 2, 3, ... 15 */
static const uint8_t mt[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

static const uint8_t kslrom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};

static const uint8_t kslshift[4] = { 8, 1, 2, 0 };

static const uint8_t eg_incstep[4][4] = {
    { 0, 0, 0, 0 },
    { 1, 0, 0, 0 },
    { 1, 0, 1, 0 },
    { 1, 1, 1, 0 }
};

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Nuked works out an operator's envelope rate, level offset and phase
 * increment from its registers on every sample. They only change when a
 * register is written (or, for the increment, when the vibrato steps), so
 * here they are worked out then. Working values are int-sized: on the
 * VR4300, 8- and 16-bit ones cost a zero- or sign-extension every time. */
typedef struct {
    /* registers */
    int am, vib, type, ksr, mult;               /* 0x20 */
    int ksl, tl;                                /* 0x40 */
    int ar, dr;                                 /* 0x60 */
    int sl, rr;                                 /* 0x80 */
    int wf;                                     /* 0xe0 */
    int key;
    /* derived from them */
    int eg_ksl, eg_base;                        /* tl and key scaling level */
    int rate_hi[4], rate_lo[4], rate_nz[4];     /* per envelope stage */
    unsigned inc;                               /* phase increment */
    /* envelope and phase */
    int eg_gen, eg_rout, eg_out;
    unsigned pg_phase, pg_phase_out;
    int out;
    /* see update_steady() */
    int steady;
    unsigned trig;
} op_t;

static struct {
    op_t     op[2];                             /* modulator, carrier */
    int      f_num, block, ksv;
    unsigned timer, eg_timer;
    int      eg_state, eg_add, eg_timer_lo;
    int      vibpos, tremolo, tremolopos;
} chip;

/* SDL_ALSoundService()'s state */
static const uint8_t *sound;
static uint32_t sound_left;
static uint8_t  sound_block;
static uint32_t tick_acc;

/* ------------------------------------------------------------------ */
/* Register writes                                                     */
/* ------------------------------------------------------------------ */

static void update_level(op_t *op)
{
    int ksl = (kslrom[chip.f_num >> 6] << 2) - ((8 - chip.block) << 5);
    op->eg_ksl = ksl < 0 ? 0 : ksl;
    op->eg_base = (op->tl << 2) + (op->eg_ksl >> kslshift[op->ksl]);
}

/* OPL3_EnvelopeCalc()'s rate, for each stage; a key-on during the release
 * uses the attack's */
static void update_rates(op_t *op)
{
    const int ks = chip.ksv >> ((op->ksr ^ 1) << 1);
    const int reg[4] = { op->ar, op->dr, op->type ? 0 : op->rr, op->rr };

    for (int g = 0; g < 4; g++) {
        const int rate = ks + (reg[g] << 2);
        int hi = rate >> 2;
        if (hi & 0x10)
            hi = 15;
        op->rate_hi[g] = hi;
        op->rate_lo[g] = rate & 3;
        op->rate_nz[g] = reg[g] != 0;
    }
}

/* OPL3_PhaseGenerate()'s increment, vibrato included */
static void update_inc(op_t *op)
{
    int f_num = chip.f_num;

    if (op->vib) {
        int range = (f_num >> 7) & 7;
        if (!(chip.vibpos & 3))
            range = 0;
        else if (chip.vibpos & 1)
            range >>= 1;
        range >>= 1;                            /* vibshift: shallow */
        if (chip.vibpos & 4)
            range = -range;
        f_num = (f_num + range) & 0xffff;
    }
    op->inc = ((((unsigned)f_num << chip.block) >> 1) * mt[op->mult]) >> 1;
}

/* Most of the time an envelope sits in a slow stage where a sample changes
 * nothing: no stage change is due, the level is not being snapped off, and
 * the rate only steps on the samples where the envelope timer's eg_add
 * takes one of up to three values. An operator in that state is "steady",
 * and trig holds those eg_add values as bits; chip_step() skips
 * OPL3_EnvelopeCalc() for it on every other sample. */
static void update_steady(op_t *op)
{
    const int gen = op->eg_gen, rout = op->eg_rout;

    op->trig = 0;
    if (gen == EG_ATTACK) {
        op->steady = op->key && rout != 0;
    } else {
        op->steady = (op->key ? gen != EG_RELEASE : gen == EG_RELEASE)
                  && !(gen == EG_DECAY && (rout >> 4) == op->sl)
                  && ((rout & 0x1f8) != 0x1f8 || rout == 0x1ff);
        if (rout == 0x1ff)
            return;                             /* off: cannot step */
    }
    if (!op->steady || !op->rate_nz[gen])
        return;
    if (op->rate_hi[gen] >= 12) {
        op->steady = 0;                         /* steps on most samples */
        return;
    }
    op->trig = 1u << (12 - op->rate_hi[gen]);
    if (op->rate_lo[gen] & 2)
        op->trig |= 1u << (13 - op->rate_hi[gen]);
    if (op->rate_lo[gen] & 1)
        op->trig |= 1u << (14 - op->rate_hi[gen]);
}

static void update_op(op_t *op)
{
    update_level(op);
    update_rates(op);
    update_inc(op);
    update_steady(op);
}

static void write_op(op_t *op, int reg, uint8_t v)
{
    switch (reg) {
    case 0x20:
        op->am = v >> 7;
        op->vib = (v >> 6) & 1;
        op->type = (v >> 5) & 1;
        op->ksr = (v >> 4) & 1;
        op->mult = v & 15;
        break;
    case 0x40:
        op->ksl = v >> 6;
        op->tl = v & 0x3f;
        break;
    case 0x60:
        op->ar = v >> 4;
        op->dr = v & 15;
        break;
    case 0x80:
        op->sl = v >> 4;
        if (op->sl == 15)
            op->sl = 0x1f;
        op->rr = v & 15;
        break;
    case 0xe0:
        op->wf = v & 3;                         /* OPL2: four waveforms */
        break;
    }
    update_op(op);
}

static void update_frequency(void)
{
    chip.ksv = chip.block << 1 | ((chip.f_num >> 9) & 1);
    update_op(&chip.op[0]);
    update_op(&chip.op[1]);
}

static void write_a0(uint8_t v)
{
    chip.f_num = (chip.f_num & 0x300) | v;
    update_frequency();
}

static void write_b0(uint8_t v)
{
    chip.f_num = (chip.f_num & 0xff) | (v & 3) << 8;
    chip.block = (v >> 2) & 7;
    chip.op[0].key = chip.op[1].key = (v & 0x20) != 0;
    update_frequency();
}

/* ------------------------------------------------------------------ */
/* One chip sample                                                     */
/* ------------------------------------------------------------------ */

/* OPL3_EnvelopeCalc(); returns whether the phase is reset */
static inline int envelope(op_t *op)
{
    const int gen = op->eg_gen, rout = op->eg_rout;
    const int reset = op->key && gen == EG_RELEASE;
    const int g = reset ? EG_ATTACK : gen;
    const int hi = op->rate_hi[g];
    int shift = 0, inc = 0, next = rout, new_gen = gen;

    op->eg_out = rout + op->eg_base + (op->am ? chip.tremolo : 0);
    if (op->rate_nz[g]) {
        const int lo = op->rate_lo[g];
        if (hi < 12) {
            if (chip.eg_state) {
                switch (hi + chip.eg_add) {
                case 12: shift = 1; break;
                case 13: shift = (lo >> 1) & 1; break;
                case 14: shift = lo & 1; break;
                }
            }
        } else {
            shift = (hi & 3) + eg_incstep[lo][chip.eg_timer_lo];
            if (shift & 4)
                shift = 3;
            if (!shift)
                shift = chip.eg_state;
        }
    }
    if (reset && hi == 15)                      /* instant attack */
        next = 0;
    if (gen != EG_ATTACK && !reset && (rout & 0x1f8) == 0x1f8)
        next = 0x1ff;                           /* envelope off */
    switch (gen) {
    case EG_ATTACK:
        if (!rout)
            new_gen = EG_DECAY;
        else if (op->key && shift > 0 && hi != 15)
            inc = ~rout >> (4 - shift);
        break;
    case EG_DECAY:
        if ((rout >> 4) == op->sl)
            new_gen = EG_SUSTAIN;
        else if ((rout & 0x1f8) != 0x1f8 && !reset && shift > 0)
            inc = 1 << (shift - 1);
        break;
    default:
        if ((rout & 0x1f8) != 0x1f8 && !reset && shift > 0)
            inc = 1 << (shift - 1);
        break;
    }
    op->eg_rout = (next + inc) & 0x1ff;
    if (reset)
        new_gen = EG_ATTACK;
    if (!op->key)
        new_gen = EG_RELEASE;
    op->eg_gen = new_gen;
    update_steady(op);
    return reset;
}

static inline __attribute__((always_inline)) int envelope_step(op_t *op)
{
    if (op->steady && !(chip.eg_state && ((op->trig >> chip.eg_add) & 1))) {
        op->eg_out = op->eg_rout + op->eg_base + (op->am ? chip.tremolo : 0);
        return 0;
    }
    return envelope(op);
}

static inline int calc_exp(unsigned level)
{
    if (level > 0x1fff)
        level = 0x1fff;
    return (exprom[level & 0xff] << 1) >> (level >> 8);
}

/* OPL3_EnvelopeCalcSin0..3 */
static inline __attribute__((always_inline)) int operator_out(const op_t *op, unsigned ph)
{
    const unsigned env = (unsigned)op->eg_out << 3;

    ph &= 0x3ff;
    switch (op->wf) {
    case 0:                                     /* sine */
    {
        const int v = calc_exp(logsinrom[(ph & 0x100) ? ~ph & 0xff : ph & 0xff] + env);
        return (ph & 0x200) ? ~v : v;
    }
    case 1:                                     /* half sine */
        return calc_exp(((ph & 0x200) ? 0x1000
                         : logsinrom[(ph & 0x100) ? ~ph & 0xff : ph & 0xff]) + env);
    case 2:                                     /* absolute sine */
        return calc_exp(logsinrom[(ph & 0x100) ? ~ph & 0xff : ph & 0xff] + env);
    default:                                    /* pulse sine */
        return calc_exp(((ph & 0x100) ? 0x1000 : logsinrom[ph & 0xff]) + env);
    }
}

/* SDL_ALSoundService(), at 140 Hz */
static void sound_service(void)
{
    uint8_t s;

    if (!sound)
        return;
    s = *sound++;
    if (!s) {
        write_b0(0);
    } else {
        write_a0(s);
        write_b0(sound_block);
    }
    if (!--sound_left) {
        sound = NULL;
        write_b0(0);
    }
}

/* trailing zeros of a byte, 8 for none (gcc's ctz is a libgcc call here) */
static const uint8_t ctz8[256] = {
    8, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
};

/* OPL3_Generate() for the one channel; the output only when it is wanted */
static inline __attribute__((always_inline)) int chip_step(int output)
{
    op_t *const mod = &chip.op[0], *const car = &chip.op[1];
    int out = 0;

    tick_acc += 140;
    if (tick_acc >= ADLIB_CHIP_RATE) {
        tick_acc -= ADLIB_CHIP_RATE;
        sound_service();
    }

    /* modulator, then carrier: OPL3_ProcessSlot() */
    mod->pg_phase_out = mod->pg_phase >> 9;
    if (envelope_step(mod))
        mod->pg_phase = 0;
    mod->pg_phase += mod->inc;
    car->pg_phase_out = car->pg_phase >> 9;
    if (envelope_step(car))
        car->pg_phase = 0;
    car->pg_phase += car->inc;
    if (output) {
        mod->out = operator_out(mod, mod->pg_phase_out);
        car->out = operator_out(car, car->pg_phase_out + (unsigned)mod->out);
        out = car->out;
    }

    /* tremolo, vibrato and the envelope timer */
    if ((chip.timer & 0x3f) == 0x3f) {
        if (++chip.tremolopos == 210)
            chip.tremolopos = 0;
        chip.tremolo = (chip.tremolopos < 105 ? chip.tremolopos
                                              : 210 - chip.tremolopos) >> 4;
    }
    if ((chip.timer & 0x3ff) == 0x3ff) {
        chip.vibpos = (chip.vibpos + 1) & 7;
        if (mod->vib)
            update_inc(mod);
        if (car->vib)
            update_inc(car);
    }
    chip.timer++;

    if (chip.eg_state) {
        const unsigned t = chip.eg_timer & 0x1fff;
        chip.eg_add = (t & 0xff) ? ctz8[t & 0xff] + 1
                    : t ? ctz8[t >> 8] + 9 : 0;
        chip.eg_timer_lo = chip.eg_timer & 3;
        chip.eg_timer++;
    }
    chip.eg_state ^= 1;
    return out;
}

/* ------------------------------------------------------------------ */

void adlib_reset(void)
{
    memset(&chip, 0, sizeof chip);
    for (int i = 0; i < 2; i++) {
        chip.op[i].eg_rout = chip.op[i].eg_out = 0x1ff;
        chip.op[i].eg_gen = EG_RELEASE;
        update_op(&chip.op[i]);
    }
    sound = NULL;
    sound_left = 0;
    tick_acc = 0;
}

void adlib_stop(void)
{
    sound = NULL;
    write_b0(0);
}

void adlib_play(const uint8_t *chunk, uint32_t size)
{
    /* the Instrument's field order: mChar cChar mScale cScale mAttack
     * cAttack mSus cSus mWave cWave */
    static const uint8_t reg[5] = { 0x20, 0x40, 0x60, 0x80, 0xe0 };
    const uint8_t *inst = chunk + 6;
    uint32_t length;

    adlib_stop();
    if (size < 24)
        return;
    length = chunk[0] | chunk[1] << 8 | chunk[2] << 16 | (uint32_t)chunk[3] << 24;
    if (length > size - 23)
        length = size - 23;
    if (!length)
        return;
    for (int k = 0; k < 5; k++)                 /* SDL_AlSetFXInst() */
        write_op(&chip.op[0], reg[k], inst[k * 2]);
    for (int k = 0; k < 5; k++)
        write_op(&chip.op[1], reg[k], inst[k * 2 + 1]);
    sound_block = (uint8_t)((chunk[22] & 7) << 2 | 0x20);
    sound_left = length;
    sound = chunk + 23;
    tick_acc = ADLIB_CHIP_RATE - 140;           /* the first byte at once */
}

/* Some instruments release so slowly (a release rate of 1 takes 40 s to
 * reach silence) that "finished" has to mean "inaudible": the carrier 54 dB
 * down, where the output peaks at 24 - the level render_audio.c cut the
 * rendered effects' tails at. */
#define SILENT_EG  0x120

bool adlib_active(void)
{
    return sound || chip.op[1].key || chip.op[1].eg_out < SILENT_EG;
}

int adlib_chip_sample(void)
{
    return (int16_t)chip_step(1);
}

void adlib_generate(int16_t *out, int n)
{
    if (!adlib_active()) {
        /* where the chip would have ended up, had it kept running */
        for (int i = 0; i < 2; i++) {
            chip.op[i].eg_rout = chip.op[i].eg_out = 0x1ff;
            chip.op[i].eg_gen = EG_RELEASE;
            update_steady(&chip.op[i]);
        }
        memset(out, 0, (size_t)n * sizeof *out);
        return;
    }
    for (int i = 0; i < n; i++) {
        op_t *const mod = &chip.op[0], *const car = &chip.op[1];
        int v;

        /* Two chip samples in which nothing but the phases can move: both
         * envelopes steady and not due to step, no driver tick, and no
         * tremolo or vibrato step (those come when the timer's low six
         * bits are all ones). One of the two has eg_state set, and it sees
         * the eg_add there is now. */
        if (mod->steady && car->steady
            && tick_acc + 280 < ADLIB_CHIP_RATE
            && ((chip.timer | 1) & 0x3f) != 0x3f
            && !(((mod->trig | car->trig) >> chip.eg_add) & 1)) {
            const unsigned t = chip.eg_timer & 0x1fff;

            tick_acc += 280;
            mod->pg_phase += mod->inc;
            car->pg_phase += car->inc;
            mod->eg_out = mod->eg_rout + mod->eg_base + (mod->am ? chip.tremolo : 0);
            car->eg_out = car->eg_rout + car->eg_base + (car->am ? chip.tremolo : 0);
            mod->out = operator_out(mod, mod->pg_phase >> 9);
            car->out = operator_out(car, (car->pg_phase >> 9) + (unsigned)mod->out);
            mod->pg_phase += mod->inc;
            car->pg_phase += car->inc;

            chip.timer += 2;
            chip.eg_add = (t & 0xff) ? ctz8[t & 0xff] + 1
                        : t ? ctz8[t >> 8] + 9 : 0;
            chip.eg_timer_lo = chip.eg_timer & 3;
            chip.eg_timer++;
            v = car->out * GAIN;
        } else {
            chip_step(0);
            v = (int16_t)chip_step(1) * GAIN;
        }
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
