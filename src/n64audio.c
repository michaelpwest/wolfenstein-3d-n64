/* n64audio.c - the N64 side of sound: libdragon's mixer playing the
 * digitized sounds, the AdLib effects and the music.
 *
 * Two mixer channels stand in for the Sound Blaster Pro's FM effect and DAC
 * voices, and the music gets the rest; sound.c decides what goes on them.
 *
 * The digitized sounds are wav64 files. The AdLib effects are synthesized as
 * they play: src/adlib.c emulates the OPL2 channel they used, fed from the
 * effects' AUDIOT chunks, and the mixer pulls its samples through a
 * streaming waveform - the chip runs only while an effect can be heard.
 * Built with SFX=wav, each effect is instead a wav64 rendered at build time
 * by tools/audio/render_audio.c.
 *
 * The music is sequenced (tools/audio/imf2xm.c): each song is an XM module
 * of recorded OPL instruments, one mixer channel for each OPL channel the
 * songs use, played by xm64player with its samples streamed off the
 * cartridge. Built with MUSIC=wav, each song is instead one rendered wav64
 * on one channel.
 */
#include <libdragon.h>
#include <stdlib.h>
#include "wolf.h"
#include "adlib.h"

#define AUDIO_RATE   22050

/* Each audio buffer is 1/25 s. The mixer tops the queue up every frame, so
 * the queue depth is roughly the delay between a sound starting and being
 * heard: three buffers is the least that survives a slow frame without a
 * gap in the music. */
#define AUDIO_BUFFERS  3

#ifdef WOLF_MUSIC_WAV
#define MUSIC_CHANNELS 1
#else
#define MUSIC_CHANNELS 8                /* imf2xm's CHANNELS */
#endif

enum { CH_ADLIB, CH_DIGI, CH_MUSIC, NUM_CHANNELS = CH_MUSIC + MUSIC_CHANNELS };

#define NUM_SONGS  27

#ifdef WOLF_SFX_WAV
static wav64_t *adlib_wav;              /* one per sound */
#else
typedef struct {
    uint8_t *data;
    uint32_t size;
} adlib_chunk_t;
static adlib_chunk_t *adlib_chunks;     /* one per sound */
static waveform_t     adlib_wave;
#endif
static wav64_t *digi_wav;               /* one per recording, when present */
static bool    *digi_ok;
static bool     song_present[NUM_SONGS];
static int      current_song = -1;

#ifdef WOLF_MUSIC_WAV
static wav64_t  songs[NUM_SONGS];
#else
/* One module open at a time: a module's patterns and instruments live in
 * RAM while it is open, and the songs only change between screens.
 *
 * imf2xm writes samples at the level render_audio gives the rendered songs.
 * libxm scales a channel by 0.25 and by sqrt(1/2) for a centered pan, so
 * about 4 * sqrt(2) puts it back - a little under, as the mixer's channel
 * volume can't exceed 1. */
static xm64player_t song_player;
#define SONG_VOLUME 5.6f
#endif

#ifndef WOLF_SFX_WAV
/* The mixer asks for the next wlen samples; the synth only ever runs
 * forward, and the channel is restarted (which empties its buffer) rather
 * than seeked. */
static void adlib_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen,
                       bool seeking)
{
    (void)ctx; (void)wpos; (void)seeking;
    adlib_generate(samplebuffer_append(sbuf, wlen), wlen);
}
#endif

void n64audio_init(void)
{
    char path[64];

    /* main() has opened the ROM filesystem */
    audio_init(AUDIO_RATE, AUDIO_BUFFERS);
    mixer_init(NUM_CHANNELS);

#ifdef WOLF_SFX_WAV
    /* Opening a wav64 reads its header from the cartridge, so do it once,
     * here, rather than on the frame a sound first plays. */
    adlib_wav = calloc(wolf_num_sounds, sizeof *adlib_wav);
    for (int s = 0; s < wolf_num_sounds; s++) {
        snprintf(path, sizeof path, "rom:/sfx/al%02d.wav64", s);
        wav64_open(&adlib_wav[s], path);
    }
#else
    /* The 81 to 87 chunks come to about 11 KB; keep them in memory. */
    adlib_chunks = calloc(wolf_num_sounds, sizeof *adlib_chunks);
    for (int s = 0; s < wolf_num_sounds; s++) {
        FILE *f;
        long len;
        snprintf(path, sizeof path, "rom:/sfx/al%02d.adl", s);
        f = fopen(path, "rb");
        assertf(f, "missing %s", path);
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        adlib_chunks[s].data = malloc(len);
        assertf(adlib_chunks[s].data && fread(adlib_chunks[s].data, 1, len, f) == (size_t)len,
                "cannot read %s", path);
        adlib_chunks[s].size = (uint32_t)len;
        fclose(f);
    }
    adlib_reset();
    /* the synth's rate is above the mixer's default limit of the output
     * rate; this sizes the channel's buffer for it */
    mixer_ch_set_limits(CH_ADLIB, 16, ADLIB_RATE, 0);
    adlib_wave = (waveform_t){
        .name = "adlib", .bits = 16, .channels = 1, .frequency = ADLIB_RATE,
        .len = WAVEFORM_UNKNOWN_LEN, .loop_len = 0, .read = adlib_read,
    };
#endif

    digi_wav = calloc(wolf_num_digi, sizeof *digi_wav);
    digi_ok  = calloc(wolf_num_digi, sizeof *digi_ok);
    for (int d = 0; d < wolf_num_digi; d++) {
        if (!wolf_digi_tics[d])
            continue;                   /* not in the shareware's VSWAP */
        snprintf(path, sizeof path, "rom:/sfx/dg%02d.wav64", d);
        wav64_open(&digi_wav[d], path);
        digi_ok[d] = true;
    }

    /* Every floor's song, and the ones the title, menus, high scores,
     * end-of-floor and victory screens play. */
    for (int l = 0; l < wolf_num_levels + MUSROLES; l++) {
        int m = l < wolf_num_levels ? (int)wolf_level_song[l]
                                    : (int)wolf_music_roles[l - wolf_num_levels];
        if (m < 0 || m >= NUM_SONGS || song_present[m])
            continue;
#ifdef WOLF_MUSIC_WAV
        snprintf(path, sizeof path, "rom:/music/m%02d.wav64", m);
        wav64_open(&songs[m], path);
#endif
        song_present[m] = true;
    }
}

/* Fill whatever audio buffers have been played out since the last call. */
void n64audio_poll(void)
{
    while (audio_can_write()) {
        short *buf = audio_write_begin();
        mixer_poll(buf, audio_get_buffer_length());
        audio_write_end();
    }
#ifndef WOLF_SFX_WAV
    /* once the effect has died away, stop pulling silence out of the chip */
    if (!adlib_active() && mixer_ch_playing(CH_ADLIB))
        mixer_ch_stop(CH_ADLIB);
#endif
}

/* ------------------------------------------------------------------ */

#ifdef WOLF_SFX_WAV

void snd_backend_adlib(int sound)
{
    if (sound < 0 || sound >= wolf_num_sounds) {
        mixer_ch_stop(CH_ADLIB);
        return;
    }
    wav64_play(&adlib_wav[sound], CH_ADLIB);
}

#else

/* As SDL_ALPlaySound() and SDL_ALStopSound(): the chip keeps its state from
 * one effect to the next, and stopping only keys the note off, so it rings
 * out on its release. */
void snd_backend_adlib(int sound)
{
    if (sound < 0 || sound >= wolf_num_sounds) {
        adlib_stop();
        return;
    }
    adlib_play(adlib_chunks[sound].data, adlib_chunks[sound].size);
    if (!mixer_ch_playing(CH_ADLIB))
        mixer_ch_play(CH_ADLIB, &adlib_wave);
}

#endif

void snd_backend_digi(int digi, int lvol, int rvol)
{
    if (digi < 0 || digi >= wolf_num_digi || !digi_ok[digi]) {
        mixer_ch_stop(CH_DIGI);
        return;
    }
    wav64_play(&digi_wav[digi], CH_DIGI);
    snd_backend_digi_volume(lvol, rvol);
}

void snd_backend_digi_volume(int lvol, int rvol)
{
    mixer_ch_set_vol(CH_DIGI, lvol / 15.0f, rvol / 15.0f);
}

#ifdef WOLF_MUSIC_WAV

void snd_backend_music(int song)
{
    if (song == current_song && mixer_ch_playing(CH_MUSIC)) {
        /* StartMusic() restarts even the same song, from the top. */
        mixer_ch_set_pos(CH_MUSIC, 0);
        return;
    }
    mixer_ch_stop(CH_MUSIC);
    current_song = -1;
    if (song < 0 || song >= NUM_SONGS || !song_present[song])
        return;
    wav64_play(&songs[song], CH_MUSIC);
    current_song = song;
}

/* The pause holds the song's place: stop the channel, and start it again
 * from where it was. */
void snd_backend_music_pause(bool paused)
{
    static float pos;

    if (current_song < 0)
        return;
    if (paused) {
        pos = mixer_ch_get_pos(CH_MUSIC);
        mixer_ch_stop(CH_MUSIC);
    } else {
        wav64_play(&songs[current_song], CH_MUSIC);
        mixer_ch_set_pos(CH_MUSIC, pos);
    }
}

#else

void snd_backend_music(int song)
{
    char path[64];

    if (song == current_song) {
        /* StartMusic() restarts even the same song, from the top. */
        xm64player_seek(&song_player, 0, 0, 0);
        song_player.stop_requested = false;
        xm64player_play(&song_player, CH_MUSIC);
        return;
    }
    if (current_song >= 0)
        xm64player_close(&song_player);     /* stops its channels at once */
    current_song = -1;
    if (song < 0 || song >= NUM_SONGS || !song_present[song])
        return;
    snprintf(path, sizeof path, "rom:/music/m%02d.xm64", song);
    xm64player_open(&song_player, path);
    xm64player_set_loop(&song_player, true);
    xm64player_set_vol(&song_player, SONG_VOLUME);
    xm64player_play(&song_player, CH_MUSIC);
    current_song = song;
}

/* The pause holds the song's place: stop the player, and start it again
 * from the row it was on. */
void snd_backend_music_pause(bool paused)
{
    static int patidx, row;

    if (current_song < 0)
        return;
    if (paused) {
        xm64player_tell(&song_player, &patidx, &row, NULL);
        xm64player_stop(&song_player);
    } else {
        xm64player_seek(&song_player, patidx, row, 0);
        /* the stop takes effect on the player's next tick; if that has not
         * come yet, cancel it rather than start a second one */
        song_player.stop_requested = false;
        xm64player_play(&song_player, CH_MUSIC);
    }
}

#endif
