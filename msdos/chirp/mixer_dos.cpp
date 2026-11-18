// mixer_dos.cpp: the part of SDL2_mixer that Felix Chirp uses, on SDL3's
// audio (on DOS: the Sound Blaster driver of SDL3's DOS port), so CHIRP.EXE
// needs no SDL_mixer. See include/SDL2/SDL_mixer.h.
//
// Music (one at a time, streamed from the file):
//   MP3 (dr_mp3), FLAC (dr_flac), WAV and AIFF (dr_wav), OGG Vorbis
//   (stb_vorbis), MOD/XM/IT/S3M/669/MED/MTM and the rest (libxmp), MIDI
//   (felixterminal's OPL3 renderer, midi_render.cpp, to a WAV in memory).
// Chunks (VOC, AIFF, and kfn's backing track): decoded whole, to 16-bit
//   stereo at the mixer's rate, on 8 channels.
// Opus isn't there (no libopus in the DOS build).
//
// Everything is mixed in the device stream's callback, which SDL calls from
// its audio thread (on DOS a cooperative one, run when the main loop waits).
// The API calls take the stream's lock while they change what it plays.

#include <SDL3/SDL.h>
#include "SDL2/SDL_mixer.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R
#define LIBXMP_STATIC
#include "xmp.h"

#include "../../midi_render.h"
#include "../../aiff_render.h"

enum MusicKind { MUS_NONE, MUS_MP3, MUS_FLAC, MUS_WAV, MUS_OGG, MUS_XMP };

struct Mix_Music {
    MusicKind kind = MUS_NONE;
    drmp3  mp3;
    drflac *flac = nullptr;
    drwav  wav;
    stb_vorbis *ogg = nullptr;
    xmp_context xmp = nullptr;
    std::vector<uint8_t> mem;      // a file rendered in memory (MIDI -> WAV)
    int rate = 44100, channels = 2;
    double duration = -1.0;
    bool at_end = false;
};

static const int MIX_RATE = 44100;           // what everything is mixed at (S16 stereo)
static SDL_AudioStream *s_dev = nullptr;     // the device
static SDL_AudioStream *s_conv = nullptr;    // music: decoder's format -> S16 stereo MIX_RATE
static Mix_Music *s_music = nullptr;         // playing (or paused)
static int  s_music_loops = 0;               // plays left after this one (-1: forever)
static bool s_music_paused = false;
static int  s_music_volume = MIX_MAX_VOLUME;

struct Channel {
    Mix_Chunk *chunk = nullptr;
    Uint32 pos = 0;
    int loops = 0;
    int volume = MIX_MAX_VOLUME;
    bool paused = false;
};
static std::vector<Channel> s_chan(8);

static void lock()   { if (s_dev) SDL_LockAudioStream(s_dev); }
static void unlock() { if (s_dev) SDL_UnlockAudioStream(s_dev); }

// ---------------------------------------------------------------- decoding

static bool music_rewind(Mix_Music *m) {
    m->at_end = false;
    switch (m->kind) {
    case MUS_MP3:  return drmp3_seek_to_pcm_frame(&m->mp3, 0);
    case MUS_FLAC: return drflac_seek_to_pcm_frame(m->flac, 0);
    case MUS_WAV:  return drwav_seek_to_pcm_frame(&m->wav, 0);
    case MUS_OGG:  return stb_vorbis_seek_start(m->ogg) != 0;
    case MUS_XMP:                                  // (after -XMP_END: start it again)
        xmp_end_player(m->xmp);
        if (xmp_start_player(m->xmp, MIX_RATE, 0) != 0) return false;
        xmp_play_buffer(m->xmp, nullptr, 0, 0);
        return true;
    default:       return false;
    }
}

// Up to `frames` frames of S16 at the music's own rate and channels; 0 at the end.
static int music_decode(Mix_Music *m, int16_t *out, int frames) {
    switch (m->kind) {
    case MUS_MP3:  return (int)drmp3_read_pcm_frames_s16(&m->mp3, (drmp3_uint64)frames, out);
    case MUS_FLAC: return (int)drflac_read_pcm_frames_s16(m->flac, (drflac_uint64)frames, out);
    case MUS_WAV:  return (int)drwav_read_pcm_frames_s16(&m->wav, (drwav_uint64)frames, out);
    case MUS_OGG:  return stb_vorbis_get_samples_short_interleaved(m->ogg, m->channels, out, frames * m->channels);
    case MUS_XMP: {
        // xmp_play_buffer pads the last buffer with silence; loop = 1 makes it
        // stop (-XMP_END) the first time the song comes back round.
        int r = xmp_play_buffer(m->xmp, out, frames * 4, 1);
        return r == 0 ? frames : 0;
    }
    default: return 0;
    }
}

static void music_close(Mix_Music *m) {
    switch (m->kind) {
    case MUS_MP3:  drmp3_uninit(&m->mp3); break;
    case MUS_FLAC: drflac_close(m->flac); break;
    case MUS_WAV:  drwav_uninit(&m->wav); break;
    case MUS_OGG:  stb_vorbis_close(m->ogg); break;
    case MUS_XMP:  xmp_end_player(m->xmp); xmp_release_module(m->xmp); xmp_free_context(m->xmp); break;
    default: break;
    }
    m->kind = MUS_NONE;
}

static const char *ext_of(const char *file) {
    const char *dot = strrchr(file, '.');
    const char *slash = strrchr(file, '/'), *bs = strrchr(file, '\\');
    if (bs > slash) slash = bs;
    return (dot && (!slash || dot > slash)) ? dot : "";
}

static bool read_file(const char *file, std::vector<uint8_t> &out) {
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool open_mp3(Mix_Music *m, const char *file) {
    if (!drmp3_init_file(&m->mp3, file, nullptr)) return false;
    m->kind = MUS_MP3;
    m->rate = (int)m->mp3.sampleRate; m->channels = (int)m->mp3.channels;
    drmp3_uint64 n = drmp3_get_pcm_frame_count(&m->mp3);     // (reads the file through once)
    if (n) m->duration = (double)n / m->rate;
    drmp3_seek_to_pcm_frame(&m->mp3, 0);
    return true;
}
static bool open_flac(Mix_Music *m, const char *file) {
    if (!(m->flac = drflac_open_file(file, nullptr))) return false;
    m->kind = MUS_FLAC;
    m->rate = (int)m->flac->sampleRate; m->channels = (int)m->flac->channels;
    if (m->flac->totalPCMFrameCount) m->duration = (double)m->flac->totalPCMFrameCount / m->rate;
    return true;
}
static bool wav_opened(Mix_Music *m) {
    m->kind = MUS_WAV;
    m->rate = (int)m->wav.sampleRate; m->channels = (int)m->wav.channels;
    if (m->wav.totalPCMFrameCount) m->duration = (double)m->wav.totalPCMFrameCount / m->rate;
    return true;
}
static bool open_wav(Mix_Music *m, const char *file) {
    return drwav_init_file(&m->wav, file, nullptr) && wav_opened(m);
}
static bool open_wav_mem(Mix_Music *m) {
    return drwav_init_memory(&m->wav, m->mem.data(), m->mem.size(), nullptr) && wav_opened(m);
}
static bool open_ogg(Mix_Music *m, const char *file) {
    int err = 0;
    if (!(m->ogg = stb_vorbis_open_filename(file, &err, nullptr))) return false;
    stb_vorbis_info i = stb_vorbis_get_info(m->ogg);
    m->kind = MUS_OGG;
    m->rate = (int)i.sample_rate; m->channels = i.channels;
    float len = stb_vorbis_stream_length_in_seconds(m->ogg);
    if (len > 0) m->duration = len;
    return true;
}
static bool open_xmp(Mix_Music *m, const char *file) {
    m->xmp = xmp_create_context();
    if (!m->xmp) return false;
    if (xmp_load_module(m->xmp, (char *)file) != 0 || xmp_start_player(m->xmp, MIX_RATE, 0) != 0) {
        xmp_free_context(m->xmp); m->xmp = nullptr; return false;
    }
    m->kind = MUS_XMP;
    m->rate = MIX_RATE; m->channels = 2;
    xmp_frame_info fi;
    xmp_get_frame_info(m->xmp, &fi);
    if (fi.total_time > 0) m->duration = fi.total_time / 1000.0;
    return true;
}
static bool open_midi(Mix_Music *m, const char *file) {
    std::vector<uint8_t> mid;
    if (!read_file(file, mid) || mid.size() < 14 || memcmp(mid.data(), "MThd", 4)) return false;
    if (!render_midi_to_wav(mid, m->mem) || m->mem.empty()) return false;
    return open_wav_mem(m);
}

Mix_Music *Mix_LoadMUS(const char *file) {
    if (!s_dev) { SDL_SetError("no sound card (SDL: %s)", SDL_GetError()); return nullptr; }
    Mix_Music *m = new Mix_Music;
    const char *e = ext_of(file);
    bool ok = false;
    if (!strcasecmp(e, ".opus")) {
        delete m;
        SDL_SetError("Opus isn't in the DOS build");
        return nullptr;
    }
    if (!strcasecmp(e, ".mid") || !strcasecmp(e, ".midi") || !strcasecmp(e, ".kar") || !strcasecmp(e, ".rmi"))
        ok = open_midi(m, file);
    else if (!strcasecmp(e, ".mp3"))  ok = open_mp3(m, file);
    else if (!strcasecmp(e, ".flac")) ok = open_flac(m, file);
    else if (!strcasecmp(e, ".ogg") || !strcasecmp(e, ".oga")) ok = open_ogg(m, file);
    else if (!strcasecmp(e, ".wav") || !strcasecmp(e, ".aif") || !strcasecmp(e, ".aiff")) ok = open_wav(m, file);
    // Not by its name (kfn's temp files, odd extensions): try them in turn.
    if (!ok) ok = open_wav(m, file) || open_flac(m, file) || open_ogg(m, file) || open_xmp(m, file) ||
                  open_midi(m, file) || open_mp3(m, file);
    if (!ok) { delete m; SDL_SetError("can't decode %s", file); return nullptr; }
    return m;
}

void Mix_FreeMusic(Mix_Music *m) {
    if (!m) return;
    lock();
    if (s_music == m) { s_music = nullptr; if (s_conv) SDL_ClearAudioStream(s_conv); }
    unlock();
    music_close(m);
    delete m;
}

// ---------------------------------------------------------------- mixing

static inline int16_t clip16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// Music: decode until the converter has `bytes` of S16 stereo MIX_RATE ready.
static void feed_music(int bytes) {
    static int16_t dec[4096 * 8];
    int frames = 4096;
    while (s_music && !s_music->at_end && SDL_GetAudioStreamAvailable(s_conv) < bytes) {
        int n = music_decode(s_music, dec, frames);
        if (n > 0) { SDL_PutAudioStreamData(s_conv, dec, n * s_music->channels * 2); continue; }
        if (s_music_loops != 0 && music_rewind(s_music)) {
            if (s_music_loops > 0) s_music_loops--;
            continue;
        }
        s_music->at_end = true;
        SDL_FlushAudioStream(s_conv);
    }
}

FILE *g_chirp_log;            // SET CHIRP_LOG=file (chirp_dos.cpp opens it)
#define s_log g_chirp_log
static uint64_t s_cb_bytes;
static void SDLCALL audio_cb(void *, SDL_AudioStream *stream, int additional, int total) {
    if (s_log) {
        static uint64_t last;
        uint64_t now = SDL_GetTicks();
        s_cb_bytes += additional;
        if (now - last >= 500) {
            fprintf(s_log, "%llu ms: %llu bytes asked (%d now, %d total) ch0 pos %u/%u\n", (unsigned long long)now,
                    (unsigned long long)s_cb_bytes, additional, total, s_chan[0].pos, s_chan[0].chunk ? s_chan[0].chunk->alen / 2 : 0);
            fflush(s_log);
            last = now;
        }
    }
    static std::vector<int32_t> acc;
    static std::vector<int16_t> tmp;
    while (additional > 0) {
        int frames = additional / 4;
        if (frames > 2048) frames = 2048;
        if (frames <= 0) frames = 1;
        int bytes = frames * 4;
        acc.assign((size_t)frames * 2, 0);
        tmp.resize((size_t)frames * 2);

        if (s_music && !s_music_paused) {
            feed_music(bytes);
            int got = SDL_GetAudioStreamData(s_conv, tmp.data(), bytes);
            for (int i = 0; i < got / 2; i++) acc[i] += tmp[i] * s_music_volume / MIX_MAX_VOLUME;
        }
        for (Channel &c : s_chan) {
            if (!c.chunk || c.paused) continue;
            const int16_t *src = (const int16_t *)c.chunk->abuf;
            Uint32 total = c.chunk->alen / 2;
            int vol = c.volume * c.chunk->volume / MIX_MAX_VOLUME;
            for (int i = 0; i < frames * 2; i++) {
                if (c.pos >= total) {
                    if (c.loops != 0 && total) { if (c.loops > 0) c.loops--; c.pos = 0; }
                    else { c.chunk = nullptr; break; }
                }
                acc[i] += src[c.pos++] * vol / MIX_MAX_VOLUME;
            }
        }
        for (int i = 0; i < frames * 2; i++) tmp[i] = clip16(acc[i]);
        SDL_PutAudioStreamData(stream, tmp.data(), bytes);
        additional -= bytes;
    }
}

// ---------------------------------------------------------------- setup

int Mix_Init(int flags) { return flags & ~MIX_INIT_OPUS; }
void Mix_Quit(void) {}

int Mix_OpenAudio(int, SDL_AudioFormat, int, int) {
    if (s_dev) return 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return -1;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, MIX_RATE };
    // A DMA half-buffer of 2048 frames (46 ms at 44.1 kHz; SDL's ring holds
    // four): on DOS the mixer runs only when the main loop yields, and a
    // frame of the visualizer can take a while on a slow machine.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
    s_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!s_dev) return -1;
    s_conv = SDL_CreateAudioStream(&spec, &spec);
    SDL_ResumeAudioStreamDevice(s_dev);
    if (s_log) {
        SDL_AudioSpec dev; int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_dev), &dev, &frames);
        fprintf(s_log, "audio: %s, %d Hz, %d channels, format %x, %d frames\n", SDL_GetCurrentAudioDriver(),
                dev.freq, dev.channels, dev.format, frames);
        fflush(s_log);
    }
    return 0;
}

void Mix_CloseAudio(void) {
    if (!s_dev) return;
    SDL_DestroyAudioStream(s_dev);
    s_dev = nullptr;
    SDL_DestroyAudioStream(s_conv);
    s_conv = nullptr;
    s_music = nullptr;
    for (Channel &c : s_chan) c.chunk = nullptr;
}

int Mix_AllocateChannels(int n) {
    if (n < 0) return (int)s_chan.size();
    lock();
    s_chan.resize((size_t)n);
    unlock();
    return n;
}

// ---------------------------------------------------------------- music

int Mix_PlayMusic(Mix_Music *m, int loops) {
    if (!m || !s_dev) return -1;
    lock();
    s_music = m;
    SDL_AudioSpec src = { SDL_AUDIO_S16, m->channels, m->rate };
    SDL_SetAudioStreamFormat(s_conv, &src, nullptr);
    SDL_ClearAudioStream(s_conv);
    music_rewind(m);
    s_music_loops = loops < 0 ? -1 : loops > 1 ? loops - 1 : 0;   // SDL_mixer: 0 and 1 both play once
    s_music_paused = false;
    unlock();
    return 0;
}

int Mix_HaltMusic(void) {
    lock();
    s_music = nullptr;
    if (s_conv) SDL_ClearAudioStream(s_conv);
    s_music_paused = false;
    unlock();
    return 0;
}

void Mix_PauseMusic(void)  { lock(); s_music_paused = true;  unlock(); }
void Mix_ResumeMusic(void) { lock(); s_music_paused = false; unlock(); }
int  Mix_PausedMusic(void) { return s_music && s_music_paused; }

int Mix_PlayingMusic(void) {
    lock();
    bool playing = s_music && !(s_music->at_end && SDL_GetAudioStreamAvailable(s_conv) == 0);
    unlock();
    return playing;
}

int Mix_VolumeMusic(int v) {
    int old = s_music_volume;
    if (v >= 0) s_music_volume = v > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : v;
    return old;
}

int Mix_SetMusicPosition(double pos) {
    if (!s_music) return -1;
    if (pos < 0) pos = 0;
    lock();
    Mix_Music *m = s_music;
    bool ok = false;
    drmp3_uint64 frame = (drmp3_uint64)(pos * m->rate);
    switch (m->kind) {
    case MUS_MP3:  ok = drmp3_seek_to_pcm_frame(&m->mp3, frame); break;
    case MUS_FLAC: ok = drflac_seek_to_pcm_frame(m->flac, frame); break;
    case MUS_WAV:  ok = drwav_seek_to_pcm_frame(&m->wav, frame); break;
    case MUS_OGG:  ok = stb_vorbis_seek(m->ogg, (unsigned)frame) != 0; break;
    case MUS_XMP:  ok = xmp_seek_time(m->xmp, (int)(pos * 1000)) >= 0; break;
    default: break;
    }
    if (ok) { m->at_end = false; SDL_ClearAudioStream(s_conv); }
    unlock();
    if (!ok) SDL_SetError("can't seek");
    return ok ? 0 : -1;
}

double Mix_MusicDuration(Mix_Music *m) {
    if (!m) m = s_music;
    return m ? m->duration : -1.0;
}

// ---------------------------------------------------------------- chunks

// Creative Voice File: 8-bit unsigned or 16-bit signed PCM, blocks 1
// (sound), 2 (more), 3 (silence), 8 (stereo/rate for the next 1) and 9 (new
// format). Packed (ADPCM) blocks are skipped. (SDL_mixer reads VOC itself,
// so felixterminal's voc_render.cpp, which takes block 1's time constant for
// a codec, isn't used here.)
static bool voc_decode(const std::vector<uint8_t> &v, std::vector<int16_t> &pcm, int &rate, int &channels) {
    if (v.size() < 26 || memcmp(v.data(), "Creative Voice File\x1a", 20)) return false;
    size_t p = v[20] | v[21] << 8;
    rate = 0; channels = 1;
    int ext_rate = 0, ext_ch = 0, bits = 8;
    while (p + 1 <= v.size()) {
        int type = v[p];
        if (type == 0 || p + 4 > v.size()) break;
        size_t n = v[p + 1] | v[p + 2] << 8 | v[p + 3] << 16;
        const uint8_t *d = v.data() + p + 4;
        if (p + 4 + n > v.size()) n = v.size() - p - 4;
        p += 4 + n;
        auto add8 = [&](const uint8_t *s, size_t k) { for (size_t i = 0; i < k; i++) pcm.push_back((int16_t)((s[i] - 128) << 8)); };
        switch (type) {
        case 1:
            if (n < 2 || d[1] != 0) break;                 // packed: not done
            if (ext_rate) { rate = ext_rate; channels = ext_ch; ext_rate = 0; }
            else if (!rate) { rate = 1000000 / (256 - d[0]); channels = 1; }
            bits = 8;
            add8(d + 2, n - 2);
            break;
        case 2:
            if (bits == 16) for (size_t i = 0; i + 1 < n; i += 2) pcm.push_back((int16_t)(d[i] | d[i + 1] << 8));
            else add8(d, n);
            break;
        case 3:
            if (n >= 3) { if (!rate) rate = 1000000 / (256 - d[2]); pcm.insert(pcm.end(), (size_t)((d[0] | d[1] << 8) + 1) * channels, 0); }
            break;
        case 8:
            if (n >= 4) {
                ext_ch = d[3] + 1;
                ext_rate = (int)(256000000L / (65536 - (d[0] | d[1] << 8)) / ext_ch);
            }
            break;
        case 9: {
            if (n < 12) break;
            int codec = d[6] | d[7] << 8;
            rate = (int)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24);
            channels = d[5] ? d[5] : 1;
            bits = d[4];
            if (codec == 0 && bits == 8) add8(d + 12, n - 12);
            else if (codec == 4 && bits == 16) for (size_t i = 12; i + 1 < n; i += 2) pcm.push_back((int16_t)(d[i] | d[i + 1] << 8));
            break;
        }
        default: break;
        }
    }
    if (rate <= 0) rate = 11025;
    return !pcm.empty();
}

static Mix_Chunk *chunk_from_pcm(const int16_t *pcm, size_t samples, int rate, int channels) {
    SDL_AudioSpec in = { SDL_AUDIO_S16, channels, rate }, out = { SDL_AUDIO_S16, 2, MIX_RATE };
    Uint8 *dst = nullptr; int dlen = 0;
    if (!SDL_ConvertAudioSamples(&in, (const Uint8 *)pcm, (int)(samples * 2), &out, &dst, &dlen)) return nullptr;
    return new Mix_Chunk{1, dst, (Uint32)dlen, MIX_MAX_VOLUME};
}

Mix_Chunk *Mix_LoadWAV(const char *file) {
    std::vector<uint8_t> data, wav;
    if (!read_file(file, data)) { SDL_SetError("can't read %s", file); return nullptr; }
    const char *e = ext_of(file);
    const std::vector<uint8_t> *src = &data;
    if (data.size() > 20 && !memcmp(data.data(), "Creative Voice File", 19)) {
        std::vector<int16_t> pcm;
        int rate, ch;
        if (!voc_decode(data, pcm, rate, ch)) { SDL_SetError("bad or packed VOC file"); return nullptr; }
        return chunk_from_pcm(pcm.data(), pcm.size(), rate, ch);
    } else if (data.size() > 12 && !memcmp(data.data(), "FORM", 4) &&
               (!memcmp(data.data() + 8, "AIFF", 4) || !memcmp(data.data() + 8, "AIFC", 4))) {
        if (render_aiff_to_wav(data, wav)) src = &wav;   // (else dr_wav reads the AIFF itself)
    }

    // Decode it all (WAV, AIFF, or anything Mix_LoadMUS reads), then convert
    // to S16 stereo at MIX_RATE.
    Mix_Music m;
    bool ok;
    if (src == &wav) { m.mem.swap(wav); ok = open_wav_mem(&m); }
    else {
        m.mem.swap(data);
        ok = open_wav_mem(&m);
        if (!ok) {
            m.mem.clear();
            Mix_Music *mm = Mix_LoadMUS(file);
            if (!mm) return nullptr;
            std::vector<int16_t> pcm, buf((size_t)4096 * mm->channels);
            int n;
            while ((n = music_decode(mm, buf.data(), 4096)) > 0) pcm.insert(pcm.end(), buf.begin(), buf.begin() + n * mm->channels);
            Mix_Chunk *c = chunk_from_pcm(pcm.data(), pcm.size(), mm->rate, mm->channels);
            Mix_FreeMusic(mm);
            return c;
        }
    }
    if (!ok) { SDL_SetError("can't decode %s", file); return nullptr; }
    std::vector<int16_t> pcm((size_t)m.wav.totalPCMFrameCount * m.channels);
    size_t got = drwav_read_pcm_frames_s16(&m.wav, m.wav.totalPCMFrameCount, pcm.data());
    Mix_Chunk *c = chunk_from_pcm(pcm.data(), got * m.channels, m.rate, m.channels);
    music_close(&m);
    return c;
}

void Mix_FreeChunk(Mix_Chunk *c) {
    if (!c) return;
    lock();
    for (Channel &ch : s_chan) if (ch.chunk == c) ch.chunk = nullptr;
    unlock();
    if (c->allocated) SDL_free(c->abuf);
    delete c;
}

int Mix_PlayChannel(int ch, Mix_Chunk *c, int loops) {
    if (!c || !s_dev) { SDL_SetError("no sound card"); return -1; }
    lock();
    if (ch < 0) {
        for (size_t i = 0; i < s_chan.size(); i++) if (!s_chan[i].chunk) { ch = (int)i; break; }
    }
    if (ch < 0 || ch >= (int)s_chan.size()) { unlock(); SDL_SetError("no free channels"); return -1; }
    Channel &k = s_chan[(size_t)ch];
    k.chunk = c; k.pos = 0; k.loops = loops; k.paused = false;
    unlock();
    return ch;
}

int Mix_HaltChannel(int ch) {
    lock();
    for (size_t i = 0; i < s_chan.size(); i++)
        if (ch < 0 || (int)i == ch) s_chan[i].chunk = nullptr;
    unlock();
    return 0;
}

static void set_paused(int ch, bool p) {
    lock();
    for (size_t i = 0; i < s_chan.size(); i++)
        if (ch < 0 || (int)i == ch) s_chan[i].paused = p;
    unlock();
}
void Mix_Pause(int ch)  { set_paused(ch, true); }
void Mix_Resume(int ch) { set_paused(ch, false); }

int Mix_Playing(int ch) {
    lock();
    int n = 0;
    for (size_t i = 0; i < s_chan.size(); i++)
        if ((ch < 0 || (int)i == ch) && s_chan[i].chunk) n++;
    unlock();
    return n;
}

int Mix_Volume(int ch, int v) {
    if (ch >= (int)s_chan.size()) return 0;
    lock();
    int old = ch < 0 ? MIX_MAX_VOLUME : s_chan[(size_t)ch].volume;
    if (v >= 0) {
        if (v > MIX_MAX_VOLUME) v = MIX_MAX_VOLUME;
        for (size_t i = 0; i < s_chan.size(); i++) if (ch < 0 || (int)i == ch) s_chan[i].volume = v;
    }
    unlock();
    return old;
}
