/* SDL2/SDL_mixer.h for Felix Chirp on MS-DOS: the part of SDL2_mixer's API
   that felixchirp/ calls, done in mixer_dos.cpp on SDL3's audio (the Sound
   Blaster, on DOS) with dr_mp3, dr_flac, dr_wav, stb_vorbis, libxmp and
   felixterminal's own OPL3 MIDI renderer. */
#pragma once
#include "SDL.h"

#define MIX_INIT_FLAC 0x01
#define MIX_INIT_MOD  0x02
#define MIX_INIT_MP3  0x08
#define MIX_INIT_OGG  0x10
#define MIX_INIT_MID  0x20
#define MIX_INIT_OPUS 0x40
#define MIX_DEFAULT_FORMAT SDL_AUDIO_S16
#define MIX_MAX_VOLUME 128

typedef struct Mix_Chunk {
    int     allocated;
    Uint8  *abuf;          /* S16 stereo at the mixer's rate */
    Uint32  alen;          /* bytes */
    Uint8   volume;        /* 0..128 */
    void   *stream;        /* (DOS: a long compressed sound read as it plays, not decoded first) */
} Mix_Chunk;
typedef struct Mix_Music Mix_Music;

int        Mix_Init(int flags);
void       Mix_Quit(void);
int        Mix_OpenAudio(int frequency, SDL_AudioFormat format, int channels, int chunksize);
void       Mix_CloseAudio(void);
int        Mix_AllocateChannels(int numchans);
#define    Mix_GetError SDL_GetError

Mix_Chunk *Mix_LoadWAV(const char *file);
void       Mix_FreeChunk(Mix_Chunk *chunk);
int        Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int        Mix_HaltChannel(int channel);
void       Mix_Pause(int channel);
void       Mix_Resume(int channel);
int        Mix_Playing(int channel);
int        Mix_Volume(int channel, int volume);

Mix_Music *Mix_LoadMUS(const char *file);
void       Mix_FreeMusic(Mix_Music *music);
int        Mix_PlayMusic(Mix_Music *music, int loops);
int        Mix_HaltMusic(void);
void       Mix_PauseMusic(void);
void       Mix_ResumeMusic(void);
int        Mix_PlayingMusic(void);
int        Mix_PausedMusic(void);
int        Mix_VolumeMusic(int volume);
int        Mix_SetMusicPosition(double position);
double     Mix_MusicDuration(Mix_Music *music);
