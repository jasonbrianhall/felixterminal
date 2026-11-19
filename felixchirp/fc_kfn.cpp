// kfn (.kfn) support for felixchirp.
//
// This is the C++ port of karaoke-library.js's KaraokePlayer, adapted to
// felixchirp's existing audio pipeline instead of HTMLAudioElement:
//   - The "vocal" (or the only) track rides on the existing g_iv.music /
//     g_iv.chunk playback path, so pause/resume/stop/repeat/progress-bar
//     all keep working unchanged (see iv_play_audio in felixchirp.cpp).
//   - An optional "backing" track plays alongside as an independent
//     Mix_Chunk on its own channel, mirroring the JS player's separate
//     audioVocal / audioBacking elements with independent mute.
//   - song.ini parsing (title/artist/textN/syncN → word-level lyrics +
//     millisecond sync times) is a straight port of parseINI().
#include "felixchirp.h"
#include "kfn.h"
#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"
float measure_text(const char *text, int font_px);   // (ft_font.cpp)

#include "miniz.h"
#include "miniz_zip.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"  // strings.h doesn't exist under MSVC; strcasecmp
#else
#include <strings.h>   // strcasecmp
#endif
#include <ctype.h>
#include <algorithm>
#include <map>
#include <sstream>

extern int g_font_size;

// ============================================================================
// HELPERS
// ============================================================================

static std::string kfn_trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string kfn_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = (char)tolower((unsigned char)c);
    return r;
}

// True if the whole string (after `prefixLen` chars) is digits — used to
// recognize "text3" / "sync12" style INI keys, same as the JS /^text\d+$/.
static bool kfn_all_digits(const std::string &s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

bool is_kfn_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".kfn") == 0;
}

static bool iv_kfn_is_audio_name(const std::string &name) {
    static const char *exts[] = { ".mp3", ".ogg", ".wav", ".m4a", ".aac" };
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = kfn_lower(name.substr(dot));
    for (auto *e : exts) if (ext == e) return true;
    return false;
}

// ============================================================================
// song.ini PARSING  (port of KaraokePlayer.parseINI)
// ============================================================================

static void iv_kfn_parse_ini(const std::string &content) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); i++) {
        if (i == content.size() || content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }

    std::string title  = "Unknown Song";
    std::string artist = "Unknown Artist";
    std::map<int, std::string>      textMap;
    std::map<int, std::vector<int>> syncMap;
    std::string currentSection;

    for (auto &raw : lines) {
        std::string line = kfn_trim(raw);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            currentSection = kfn_lower(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        std::string keyLower = kfn_lower(key);

        if (currentSection == "general" || currentSection.empty()) {
            if (keyLower == "title")  title  = value;
            if (keyLower == "artist") artist = value;
        }

        if (keyLower.rfind("text", 0) == 0 && kfn_all_digits(keyLower, 4)) {
            int idx = atoi(keyLower.c_str() + 4);
            textMap[idx] = value;
        }
        if (keyLower.rfind("sync", 0) == 0 && kfn_all_digits(keyLower, 4)) {
            int idx = atoi(keyLower.c_str() + 4);
            std::vector<int> times;
            size_t pos = 0;
            while (pos <= value.size()) {
                size_t comma = value.find(',', pos);
                std::string tok = (comma == std::string::npos) ? value.substr(pos) : value.substr(pos, comma - pos);
                times.push_back(atoi(tok.c_str()) * 10);  // matches JS: parseInt(v) * 10
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            syncMap[idx] = times;
        }
    }

    g_iv.kfn_lyrics.clear();
    g_iv.kfn_lyrics_lines.clear();
    g_iv.kfn_line_indices.clear();
    g_iv.kfn_sync_times.clear();

    // Concatenate sync0, sync1, sync2... in order, stopping at the first gap.
    std::vector<int> allTimings;
    for (int i = 0; syncMap.count(i); i++) {
        auto &v = syncMap[i];
        allTimings.insert(allTimings.end(), v.begin(), v.end());
    }

    size_t timingIdx = 0;
    for (int i = 0; textMap.count(i); i++) {
        const std::string &line = textMap[i];

        // KaraFun's syllables: '/' and ' ' each end one, and each takes the
        // next sync time, an empty one too: "/Whose" and "home/ of" have an
        // empty syllable, the end of the held note before. '_' is a timed
        // pause. Those take a time but aren't words. (Splitting on '/' and
        // then on spaces lost the empty ones, and from there every word had
        // the time of the one before.)
        std::vector<std::string> words;
        std::vector<double> times;
        {
            size_t pos = 0;
            while (pos <= line.size()) {
                size_t end = line.find_first_of("/ ", pos);
                std::string syl = (end == std::string::npos) ? line.substr(pos) : line.substr(pos, end - pos);
                double t = (timingIdx < allTimings.size()) ? (allTimings[timingIdx] / 1000.0) : 0.0;
                timingIdx++;
                if (!syl.empty() && syl != "_") { words.push_back(syl); times.push_back(t); }
                // A word ends at a space (or the line's end): its last
                // syllable keeps the space, so "prou/dly" shows as one word
                bool word_end = end == std::string::npos || line[end] == ' ';
                if (word_end && !words.empty() && words.back().back() != ' ') words.back() += ' ';
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        }

        if (!words.empty()) {
            g_iv.kfn_line_indices.push_back((int)g_iv.kfn_lyrics.size());
            std::string displayLine;
            for (char c : line) if (c != '/' && c != '_') displayLine += c;
            g_iv.kfn_lyrics_lines.push_back(kfn_trim(displayLine));
            for (size_t w = 0; w < words.size(); w++) {
                g_iv.kfn_lyrics.push_back(words[w]);
                g_iv.kfn_sync_times.push_back(times[w]);
            }
        }
    }

    g_iv.kfn_title  = title;
    g_iv.kfn_artist = artist;
}

// ============================================================================
// LOAD / STOP
// ============================================================================

void iv_kfn_stop() {
    if (g_iv.kfn_channel_backing >= 0) {
        Mix_HaltChannel(g_iv.kfn_channel_backing);
        g_iv.kfn_channel_backing = -1;
    }
    if (g_iv.kfn_chunk_backing) {
        Mix_FreeChunk(g_iv.kfn_chunk_backing);
        g_iv.kfn_chunk_backing = nullptr;
    }
    if (!g_iv.kfn_tmp_vocal.empty())   { iv_delete_tempfile(g_iv.kfn_tmp_vocal.c_str());   g_iv.kfn_tmp_vocal.clear(); }
    if (!g_iv.kfn_tmp_backing.empty()) { iv_delete_tempfile(g_iv.kfn_tmp_backing.c_str()); g_iv.kfn_tmp_backing.clear(); }

    g_iv.kfn_active = false;
    g_iv.kfn_title.clear();
    g_iv.kfn_artist.clear();
    g_iv.kfn_lyrics.clear();
    g_iv.kfn_lyrics_lines.clear();
    g_iv.kfn_line_indices.clear();
    g_iv.kfn_sync_times.clear();
    g_iv.kfn_current_word   = 0;
    g_iv.kfn_vocal_muted    = false;
    g_iv.kfn_backing_muted  = false;
}

// Picks vocal (primary) and backing track filenames out of a list of
// audio-like names, using the same "instru"/"beat" heuristic as the JS
// player's createAudioElement() isVocal check. Shared by both container
// formats below.
static void iv_kfn_classify_tracks(const std::vector<std::string> &names,
                                    std::string &vocal, std::string &backing) {
    vocal.clear();
    backing.clear();
    for (auto &name : names) {
        const char *dot  = strrchr(name.c_str(), '/');
        const char *base = dot ? dot + 1 : name.c_str();
        std::string lower = kfn_lower(base);
        bool looksBacking = lower.find("instru") != std::string::npos ||
                             lower.find("beat")   != std::string::npos;
        if (looksBacking) { if (backing.empty()) backing = name; }
        else               { if (vocal.empty())  vocal   = name; }
    }
    if (vocal.empty()) { vocal = backing; backing.clear(); }  // only one track total
    if (backing == vocal) backing.clear();
}

// Starts playback given already-extracted temp file paths (vocal required,
// backing optional). Shared tail for the v1 (zip) and v2 (KFNB) loaders —
// vocal rides the normal music/chunk pipeline, backing gets its own channel.
static bool iv_kfn_start_playback(const std::string &tmp_vocal,
                                   const std::string &tmp_backing,
                                   const char *label) {
    iv_play_audio(tmp_vocal.c_str(), label, false);
    if (!g_iv.audio_playing) {
        iv_delete_tempfile(tmp_vocal.c_str());
        return false;
    }
    g_iv.kfn_tmp_vocal = tmp_vocal;

    if (!tmp_backing.empty()) {
        g_iv.kfn_chunk_backing = Mix_LoadWAV(tmp_backing.c_str());
        if (g_iv.kfn_chunk_backing) {
            g_iv.kfn_tmp_backing = tmp_backing;
            g_iv.kfn_channel_backing = Mix_PlayChannel(-1, g_iv.kfn_chunk_backing, 0);
            if (g_iv.kfn_channel_backing >= 0)
                Mix_Volume(g_iv.kfn_channel_backing, (int)(g_iv.volume * 128.0f));
        } else {
            iv_delete_tempfile(tmp_backing.c_str());
        }
    }

    g_iv.kfn_active       = true;
    g_iv.kfn_current_word = 0;
    g_iv.error[0] = '\0';
    return true;
}

static std::string kfn_ext_of(const std::string &name) {
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? std::string(".mp3") : name.substr(dot);
}

// True if this .kfn is actually a plain ZIP (older/v1 kfn packages —
// same song.ini + audio layout, just a standard ZIP container instead of
// the custom KFNB binary format). Sniffs the standard ZIP local-file-header
// / end-of-central-directory signatures.
static bool iv_kfn_is_v1_zip(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char sig[4] = {};
    size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    if (n < 4 || sig[0] != 'P' || sig[1] != 'K') return false;
    return (sig[2] == 0x03 && sig[3] == 0x04) ||   // local file header
           (sig[2] == 0x05 && sig[3] == 0x06) ||   // empty archive (EOCD)
           (sig[2] == 0x07 && sig[3] == 0x08);      // spanned archive
}

// v1 loader: KFN-as-ZIP, no encryption, so this is just a normal zip
// extraction using the same miniz helpers as fc_zip.cpp.
static bool iv_kfn_load_v1_zip(const char *kfn_path, const char *label) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, kfn_path, 0)) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: cannot open zip archive");
        return false;
    }

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    std::string ini_entry;
    std::vector<std::string> audio_entries;

    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char fname[512] = {};
        mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
        const char *slash = strrchr(fname, '/');
        const char *base  = slash ? slash + 1 : fname;

        if (ini_entry.empty() && strcasecmp(base, "song.ini") == 0) {
            ini_entry = fname;
        } else if (iv_kfn_is_audio_name(base)) {
            audio_entries.push_back(fname);
        }
    }
    mz_zip_reader_end(&zip);

    if (ini_entry.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no song.ini inside archive");
        return false;
    }

    size_t ini_sz = 0;
    unsigned char *ini_buf = iv_extract_zip_entry(kfn_path, ini_entry.c_str(), ini_sz);
    if (!ini_buf) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: failed to extract song.ini");
        return false;
    }
    iv_kfn_parse_ini(std::string((const char *)ini_buf, ini_sz));
    free(ini_buf);

    if (audio_entries.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no audio tracks found");
        return false;
    }

    std::string vocal_name, backing_name;
    iv_kfn_classify_tracks(audio_entries, vocal_name, backing_name);

    size_t vsz = 0;
    unsigned char *vbuf = iv_extract_zip_entry(kfn_path, vocal_name.c_str(), vsz);
    if (!vbuf) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: failed to extract %s", vocal_name.c_str());
        return false;
    }
    std::string tmp_vocal = iv_write_tempfile(vbuf, vsz, kfn_ext_of(vocal_name).c_str());
    free(vbuf);
    if (tmp_vocal.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: cannot write temp file");
        return false;
    }

    std::string tmp_backing;
    if (!backing_name.empty()) {
        size_t bsz = 0;
        unsigned char *bbuf = iv_extract_zip_entry(kfn_path, backing_name.c_str(), bsz);
        if (bbuf) {
            tmp_backing = iv_write_tempfile(bbuf, bsz, kfn_ext_of(backing_name).c_str());
            free(bbuf);
        }
    }

    return iv_kfn_start_playback(tmp_vocal, tmp_backing, label);
}

// v2 loader: the custom KFNB binary container, with optional per-entry
// AES-128-ECB encryption (see kfn.h / kfn.cpp).
static bool iv_kfn_load_v2_binary(const char *kfn_path, const char *label) {
    KFNArchive archive;
    if (!archive.open(kfn_path)) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: %s", archive.lastError().c_str());
        return false;
    }

    const KFNEntry *ini_entry = archive.find("song.ini");
    if (!ini_entry) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no song.ini inside archive");
        return false;
    }
    size_t ini_sz = 0;
    unsigned char *ini_buf = archive.extract(*ini_entry, ini_sz);
    if (!ini_buf) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: %s", archive.lastError().c_str());
        return false;
    }
    iv_kfn_parse_ini(std::string((const char *)ini_buf, ini_sz));
    free(ini_buf);

    std::vector<std::string> audio_names;
    std::map<std::string, const KFNEntry *> byName;
    for (auto &e : archive.entries()) {
        if (!iv_kfn_is_audio_name(e.filename)) continue;
        audio_names.push_back(e.filename);
        byName[e.filename] = &e;
    }
    if (audio_names.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: no audio tracks found");
        return false;
    }

    std::string vocal_name, backing_name;
    iv_kfn_classify_tracks(audio_names, vocal_name, backing_name);

    const KFNEntry *vocal_entry = byName[vocal_name];
    std::string tmp_vocal = archive.extractToTemp(*vocal_entry, kfn_ext_of(vocal_name).c_str());
    if (tmp_vocal.empty()) {
        snprintf(g_iv.error, sizeof(g_iv.error), "KFN: failed to extract %s (%s)",
                 vocal_name.c_str(), archive.lastError().c_str());
        return false;
    }

    std::string tmp_backing;
    if (!backing_name.empty()) {
        const KFNEntry *backing_entry = byName[backing_name];
        tmp_backing = archive.extractToTemp(*backing_entry, kfn_ext_of(backing_name).c_str());
    }

    return iv_kfn_start_playback(tmp_vocal, tmp_backing, label);
}

bool iv_kfn_load(const char *kfn_path, const char *label) {
    iv_kfn_stop();
    iv_stop_audio();
    iv_cdg_free();
    iv_free_tex();
    iv_ensure_mixer();

    if (iv_kfn_is_v1_zip(kfn_path)) {
        return iv_kfn_load_v1_zip(kfn_path, label);
    }
    return iv_kfn_load_v2_binary(kfn_path, label);
}

// ============================================================================
// TICK  (call from iv_tick every frame while g_iv.kfn_active)
// ============================================================================

void iv_kfn_tick() {
    if (!g_iv.kfn_active) return;

    // Advance the current word using the already-updated wall-clock
    // audio_position (iv_tick refreshes it before calling us).
    double t = g_iv.audio_position;
    int word = 0;
    for (size_t i = 0; i < g_iv.kfn_sync_times.size(); i++) {
        if (t >= g_iv.kfn_sync_times[i]) word = (int)i;
        else break;
    }
    g_iv.kfn_current_word = word;
}

// ============================================================================
// RENDER  (call from iv_render's display-area block)
// ============================================================================

void iv_kfn_render(float ix, float iy, float iw, float ih) {
    int rh = iv_row_h();
    float alpha_mul = g_iv.audio_paused ? 0.6f : 1.0f;

    char header[600];
    snprintf(header, sizeof(header), "%s \xe2\x80\x94 %s", g_iv.kfn_artist.c_str(), g_iv.kfn_title.c_str());
    float lbl_y = iy + ih * 0.10f;
    if (measure_text(header, g_font_size) > iw - 8.f)          // (narrow: the title alone)
        snprintf(header, sizeof(header), "%s", g_iv.kfn_title.c_str());
    iv_draw_text(header, ix + (iw - measure_text(header, g_font_size)) * 0.5f,
                 lbl_y, 0.90f, 0.95f, 1.00f, alpha_mul);
    if (g_iv.audio_paused) {
        const char *ps = "\xe2\x80\x96 PAUSED";
        iv_draw_text(ps, ix + (iw - measure_text(ps, g_font_size)) * 0.5f,
                     lbl_y + rh, 1.00f, 0.75f, 0.30f, 1.f);
    }

    // Layer the audio visualizer underneath the lyrics, dimmed, the way the
    // original web player showed it running behind the karaoke text.
    float viz_top = lbl_y + rh * 1.6f;
    float viz_bot = iy + ih - (float)rh * 2.6f;
    if (viz_bot > viz_top) {
        iv_draw_visualizer(ix + iw * 0.03f, viz_top, iw * 0.94f, viz_bot - viz_top,
                            alpha_mul * (g_iv.audio_paused ? 0.18f : 0.35f));
    }

    // Locate the current lyric line from kfn_current_word.
    int lineIdx = 0, lineStart = 0;
    int nLines = (int)g_iv.kfn_line_indices.size();
    for (int i = 0; i < nLines; i++) {
        if (i == nLines - 1 || g_iv.kfn_current_word < g_iv.kfn_line_indices[i + 1]) {
            lineIdx = i;
            lineStart = g_iv.kfn_line_indices[i];
            break;
        }
    }
    int lineEnd = (lineIdx + 1 < nLines) ? g_iv.kfn_line_indices[lineIdx + 1] : (int)g_iv.kfn_lyrics.size();

    // Lyrics render well above normal UI text size so they read from
    // across the room, same as a typical karaoke screen.
    int cur_font  = (int)(g_font_size * 2.0f);
    int next_font = (int)(g_font_size * 1.25f);

    float line_y = iy + ih * 0.42f;
    if (!g_iv.kfn_lyrics.empty()) {
        // The line's syllables as they're drawn (each ends with its space,
        // if any), at the largest size that fits the panel
        float line_w = 0;
        for (;;) {
            line_w = 0;
            for (int w = lineStart; w < lineEnd; w++) line_w += measure_text(g_iv.kfn_lyrics[w].c_str(), cur_font);
            if (line_w <= iw * 0.94f || cur_font <= 10) break;
            cur_font -= 2;
        }
        float x = ix + (iw - line_w) * 0.5f;
        for (int w = lineStart; w < lineEnd; w++) {
            float r, g, b;
            if (w == g_iv.kfn_current_word)      { r = 1.00f; g = 0.85f; b = 0.20f; } // singing now
            else if (w < g_iv.kfn_current_word)  { r = 0.55f; g = 0.85f; b = 1.00f; } // already sung
            else                                  { r = 0.75f; g = 0.75f; b = 0.85f; } // upcoming
            x = draw_text(g_iv.kfn_lyrics[w].c_str(), x, line_y, cur_font, cur_font, r, g, b, alpha_mul);
        }
    } else {
        const char *msg = "(no synced lyrics in this file)";
        draw_text(msg, ix + (iw - measure_text(msg, cur_font)) * 0.5f,
                  line_y, cur_font, cur_font, 0.6f, 0.6f, 0.7f, alpha_mul);
    }

    if (lineIdx + 1 < (int)g_iv.kfn_lyrics_lines.size()) {
        const std::string &next = g_iv.kfn_lyrics_lines[lineIdx + 1];
        float ny = line_y + rh * 2.0f;
        float nw;
        while ((nw = measure_text(next.c_str(), next_font)) > iw * 0.94f && next_font > 10) next_font -= 2;
        draw_text(next.c_str(), ix + (iw - nw) * 0.5f,
                  ny, next_font, next_font, 0.45f, 0.50f, 0.60f, alpha_mul * 0.85f);
    }

    // Progress bar (mirrors the plain-audio layout in iv_render)
    double total = g_iv.music ? Mix_MusicDuration(g_iv.music) : 0.0;
    float  prog  = (total > 0) ? (float)(g_iv.audio_position / total) : 0.f;
    if (prog > 1.f) prog = 1.f;
    float bar_y = iy + ih - (float)rh * 2.0f;
    float bar_x = ix + iw * 0.05f;
    float bar_w = iw * 0.90f;
    draw_rect(bar_x, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
    if (prog > 0.f) draw_rect(bar_x, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);

    int cur_s = (int)g_iv.audio_position, tot_s = (int)total;
    char timebuf[32];
    snprintf(timebuf, sizeof(timebuf), "%d:%02d / %d:%02d", cur_s / 60, cur_s % 60, tot_s / 60, tot_s % 60);
    float time_w = measure_text(timebuf, g_font_size);
    iv_draw_text(timebuf, bar_x + bar_w - time_w, bar_y + 14.f, 0.55f, 0.65f, 0.75f, 1.f);

    // The tracks and volume at the left; shorter where they'd meet the time
    char status[128];
    const char *vocal   = g_iv.kfn_vocal_muted ? "muted" : "on";
    const char *backing = (g_iv.kfn_channel_backing < 0) ? "n/a" : (g_iv.kfn_backing_muted ? "muted" : "on");
    int vol = (int)(g_iv.volume * 100.f);
    float room = bar_w - time_w - 16.f;
    snprintf(status, sizeof(status), "Vocal: %s   Backing: %s   Vol: %d%%", vocal, backing, vol);
    if (measure_text(status, g_font_size) > room)
        snprintf(status, sizeof(status), "V: %s  B: %s  %d%%", vocal, backing, vol);
    if (measure_text(status, g_font_size) > room)
        snprintf(status, sizeof(status), "%d%%", vol);
    iv_draw_text(status, bar_x, bar_y + 14.f, 0.50f, 0.70f, 0.85f, 1.f);

    const char *ctrls[] = {
        "Space: pause/resume   S: stop   K: mute vocal   B: mute backing   +/-: vol   .: repeat",
        "Space: pause   S: stop   K: vocal   B: backing   +/-: vol",
        "Space: pause   K: vocal   B: backing",
    };
    for (const char *ctrl : ctrls) {
        float cw = measure_text(ctrl, g_font_size);
        if (cw <= iw - 8.f) {
            iv_draw_text(ctrl, ix + (iw - cw) * 0.5f, bar_y + (float)rh + 4.f, 0.40f, 0.45f, 0.55f, 1.f);
            break;
        }
    }
}

// ============================================================================
// MUTE TOGGLES  (bound to K / B in fc_input.cpp)
// ============================================================================

void iv_kfn_toggle_vocal_mute() {
    if (!g_iv.kfn_active) return;
    g_iv.kfn_vocal_muted = !g_iv.kfn_vocal_muted;
    int vol = g_iv.kfn_vocal_muted ? 0 : (int)(g_iv.volume * 128.0f);
    if (g_iv.music)                     Mix_VolumeMusic(vol);
    else if (g_iv.chunk_channel >= 0)   Mix_Volume(g_iv.chunk_channel, vol);
}

void iv_kfn_toggle_backing_mute() {
    if (!g_iv.kfn_active || g_iv.kfn_channel_backing < 0) return;
    g_iv.kfn_backing_muted = !g_iv.kfn_backing_muted;
    Mix_Volume(g_iv.kfn_channel_backing, g_iv.kfn_backing_muted ? 0 : (int)(g_iv.volume * 128.0f));
}
