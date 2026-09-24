/* sound.c - SD_PlaySound() and its rules, from ID_SD.C and WL_GAME.C.
 *
 * The DOS sound manager with a Sound Blaster Pro and AdLib selected had two
 * sound-effect voices: digitized sounds on the DAC, for the handful of
 * sounds wolfdigimap maps, and everything else on one FM channel. Each voice
 * plays one sound at a time, and a new sound only replaces the current one
 * if its priority is at least as high. Door sounds are placed in stereo from
 * lefttable/righttable, and the placement follows the player until the
 * sound ends. Music is a third, independent voice.
 *
 * That logic is all here, free of libdragon, with the platform reduced to
 * the four snd_backend_* calls. What the DOS code learned from the hardware -
 * whether a sound is still playing - is tracked with a tic count instead:
 * AdLib sounds last one tic per two 140 Hz bytes, digitized sounds their
 * length at 7042 Hz.
 */
#include "wolf.h"

#define ATABLEMAX 15

static int  soundnumber, soundpriority, soundtics;      /* the FM voice   */
static int  diginumber, digipriority, digitics;         /* the DAC voice  */
static bool soundpositioned;
static fixed globalsoundx, globalsoundy;
static int  current_song = -1;

/* SoundMode, DigiMode and MusicMode, as the Sound menu sets them. */
bool sd_effects_on = true, sd_digi_on = true, sd_music_on = true;

void sd_init(void)
{
    soundnumber = soundpriority = soundtics = 0;
    diginumber = digipriority = digitics = 0;
    soundpositioned = false;
}

static int priority_of(int sound)
{
    return (int)(wolf_sound_info[sound] >> 16);
}

/* SetSoundLoc(): the source's position relative to the player, as the pair
 * of attenuations 0 (loud) .. 8 the SB Pro mixer is given. */
static void set_sound_loc(fixed gx, fixed gy, int *left, int *right)
{
    fixed viewcos = fcos(player.angle), viewsin = fsin(player.angle);
    int x, y;

    gx -= player.x;
    gy -= player.y;

    x = (fixmul(gx, viewcos) - fixmul(gy, viewsin)) >> TILESHIFT;
    y = (fixmul(gx, viewsin) + fixmul(gy, viewcos)) >> TILESHIFT;

    if (y >= ATABLEMAX)
        y = ATABLEMAX - 1;
    else if (y <= -ATABLEMAX)
        y = -ATABLEMAX;
    if (x < 0)
        x = -x;
    if (x >= ATABLEMAX)
        x = ATABLEMAX - 1;

    *left  = (int)wolf_pantable[x * 30 + y + ATABLEMAX];
    *right = (int)wolf_pantable[ATABLEMAX * 30 + x * 30 + y + ATABLEMAX];
}

/* SD_PlaySound() with the position SD_PositionSound() queued, if any. The
 * attenuations become SDL_PositionSBP()'s 15 - pos mixer levels. */
static bool play(int sound, int leftpos, int rightpos, bool positioned)
{
    int data, digi;

    /* The game's number, which is what SD_SoundPlaying() answers with, and
     * the data's, which everything about the sound is looked up by. */
    if (sound < 0 || sound >= wolf_num_game_sounds)
        return false;
    data = (int)wolf_sound_map[sound];
    if (data < 0 || data >= wolf_num_sounds)
        return false;

    /* With digitized sound off, a sound that has a recording plays its
     * AdLib version instead; with effects off, only recordings play. */
    digi = (int)wolf_sound_digi[data];
    if (sd_digi_on && digi >= 0 && digi < wolf_num_digi) {
        if (priority_of(data) < digipriority)
            return false;
        snd_backend_digi(digi, 15 - leftpos, 15 - rightpos);
        digitics        = (int)wolf_digi_tics[digi];
        soundpositioned = positioned;
        diginumber      = sound;
        digipriority    = priority_of(data);
        return true;
    }

    if (!sd_effects_on)
        return false;
    if (priority_of(data) < soundpriority)
        return false;
    snd_backend_adlib(data);
    soundtics     = (int)(wolf_sound_info[data] & 0xffff);
    soundnumber   = sound;
    soundpriority = priority_of(data);
    return false;
}

bool sd_play_sound(int sound)
{
    return play(sound, 0, 0, false);
}

/* PlaySoundLocTile(): the centre of the tile. Only a digitized sound keeps
 * its position - the FM voice is mono - and that is the one that is then
 * re-panned as the player moves. */
void sd_play_sound_loc_tile(int sound, int tilex, int tiley)
{
    sd_play_sound_loc_global(sound, ((fixed)tilex << TILESHIFT) + TILEGLOBAL / 2,
                             ((fixed)tiley << TILESHIFT) + TILEGLOBAL / 2);
}

/* PlaySoundLocGlobal(): the same, at any point - actors use their own
 * position. */
void sd_play_sound_loc_global(int sound, fixed gx, fixed gy)
{
    int left, right;

    set_sound_loc(gx, gy, &left, &right);
    if (play(sound, left, right, true)) {
        globalsoundx = gx;
        globalsoundy = gy;
    }
}

/* SD_SoundPlaying(): as in DOS, it only knows about the FM voice. */
int sd_sound_playing(void)
{
    return soundtics > 0 ? soundnumber : 0;
}

void sd_update(int tics)
{
    if (soundtics > 0 && (soundtics -= tics) <= 0) {
        soundtics = 0;
        soundnumber = soundpriority = 0;        /* SDL_SoundFinished() */
    }
    if (digitics > 0 && (digitics -= tics) <= 0) {
        digitics = 0;
        diginumber = digipriority = 0;          /* SDL_DigitizedDone() */
        soundpositioned = false;
    }

    /* UpdateSoundLoc(), once a frame from PlayLoop(). */
    if (soundpositioned) {
        int left, right;
        set_sound_loc(globalsoundx, globalsoundy, &left, &right);
        snd_backend_digi_volume(15 - left, 15 - right);
    }
}

/* SD_StopSound(): both effect voices. */
void sd_stop_sound(void)
{
    snd_backend_adlib(-1);
    snd_backend_digi(-1, 0, 0);
    soundtics = soundnumber = soundpriority = 0;
    digitics = diginumber = digipriority = 0;
    soundpositioned = false;
}

/* StartCPMusic(): any song, for the screens between floors. With music off
 * SD_StartMusic() does nothing, but the song is remembered for when it is
 * turned back on. */
void sd_start_music(int song)
{
    current_song = song;
    snd_backend_music(sd_music_on ? song : -1);
}

/* ClearMemory() stops the digitized sound between floors, and StartMusic()
 * plays songs[mapon] from the top. */
void sd_level_start(int level)
{
    snd_backend_digi(-1, 0, 0);
    digitics = diginumber = digipriority = 0;
    soundpositioned = false;

    sd_start_music(level >= 0 && level < wolf_num_levels
                   ? (int)wolf_level_song[level] : -1);
}

/* SD_SetSoundMode(): turning effects off silences the one playing. */
void sd_set_effects(bool on)
{
    sd_effects_on = on;
    if (!on) {
        snd_backend_adlib(-1);
        soundtics = soundnumber = soundpriority = 0;
    }
}

/* SD_SetDigiDevice() */
void sd_set_digi(bool on)
{
    sd_digi_on = on;
    if (!on) {
        snd_backend_digi(-1, 0, 0);
        digitics = diginumber = digipriority = 0;
        soundpositioned = false;
    }
}

/* SD_SetMusicMode(). The menu restarts its song when music comes back on. */
void sd_set_music(bool on)
{
    sd_music_on = on;
    if (!on)
        snd_backend_music(-1);
}

/* SD_MusicOff() / SD_MusicOn() around the pause: the song holds its place. */
void sd_pause_music(bool paused)
{
    if (sd_music_on)
        snd_backend_music_pause(paused);
}
