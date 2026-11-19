#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"
#endif
// Prior to guarding gst usage behind this macro, felixchirp.h included
// <gst/gst.h> unconditionally -- i.e. every non-Windows build assumed
// GStreamer was present. Default that assumption back on here so existing
// Makefile-driven Linux builds keep working without needing to learn about
// this flag. Windows/vcpkg builds never define it, so they stay excluded.
#if !defined(_WIN32) && !defined(__DJGPP__) && !defined(NO_GSTREAMER) && !defined(HAVE_GSTREAMER)
#define HAVE_GSTREAMER 1
#endif
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#endif
#include "cdg.h"
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// EYE OF FELIX  (F5)
//
// Image formats : JPEG, PNG, BMP, GIF, TIFF, WEBP  (via stb_image)
// Audio formats : MP3, OGG, FLAC, OPUS, WAV, AIFF, VOC          (via SDL_mixer)
//                 MOD, XM, IT, S3M, 669, MED, MTM  (tracker, via SDL_mixer/libxmp)
//                 MID/MIDI                           (via SDL_mixer + timidity/fluidsynth)
// Video formats : MP4, MKV, WebM, AVI, MOV, FLV    (via GStreamer)
// CD+G karaoke  : .cdg paired with any supported audio file
// ZIP browsing  : peek inside zip files for images/audio/video/cdg
// ============================================================================

#define IV_SUPPORTED_EXTS  { ".jpg",".jpeg",".png",".bmp",".gif",".webp",".tiff",".tif" }
#define IV_AUDIO_EXTS      { ".mp3",".ogg",".flac",".opus",".wav",".mid",".midi", \
                             ".aiff",".aif",".voc", \
                             ".mod",".xm",".it",".s3m",".669",".med",".mtm" }
#define IV_VIDEO_EXTS      { ".mp4",".mkv",".webm",".avi",".mov",".flv",".m4v",".wmv",".3gp" }
// kfn karaoke package: a self-contained mini-archive (song.ini + audio
// track(s) + word-level synced lyrics), see kfn.h/fc_kfn.cpp.
#define IV_KFN_EXTS        { ".kfn" }

// ============================================================================
// TEXT & MARKDOWN SUPPORT STRUCTURES
// ============================================================================

enum TextFormatType { TEXT_PLAIN, TEXT_MARKDOWN };

struct TextLine {
    std::string text;
    bool is_header;
    int header_level;
    bool is_code_block;
    bool is_list_item;
    
    // For links and images
    std::string image_url;     // If non-empty, this line has an image to display
    std::string image_alt;     // Alt text for image
    std::string link_url;      // If non-empty, text is a clickable link
    bool is_link;              // True if this line contains a link
};

struct TextDocument {
    std::vector<TextLine> lines;
    TextFormatType format;
    int scroll_line;
};

struct IVEntry {
    char     name[512]      = {};
    bool     is_dir         = false;
    bool     is_zip         = false;
    bool     is_zip_entry   = false;
    bool     is_audio       = false;
    bool     is_video       = false;
    bool     is_cdg         = false;
    bool     is_kfn         = false;
    bool     has_cdg_pair   = false;
    char     zip_path[4096] = {};
    char     zip_entry[512] = {};
    uint64_t size           = 0;
};

struct ImageViewer {
    bool visible  = false;
    bool remote   = false;
    bool fullscreen = false;  // Fullscreen mode for CDG/audio (hides panel/title even when not zoomed)

    char path[4096]     = {};
    bool in_zip         = false;
    char zip_file[4096] = {};

    std::vector<IVEntry> entries;
    int  selected    = 0;
    int  scroll_top  = 0;

    // Current image (GL texture)
    unsigned int tex  = 0;
    SDL_Texture *sdl_tex = nullptr;   // SDL renderer path (replaces tex)
    int    tex_w      = 0;
    int    tex_h      = 0;
    char   img_label[512] = {};
    char   error[256]     = {};

    // Audio visualizer
    int    vis_mode   = 0;      // 0=symmetry cascade, 1=radial, 2=oscilloscope, 3=starfield

    // Zoom / pan / rotation (image view)
    float  zoom       = 1.0f;   // 1.0 = fit-to-area
    float  pan_x      = 0.0f;  // offset in pixels from centre
    float  pan_y      = 0.0f;
    int    img_rot    = 0;      // 0 / 90 / 180 / 270 degrees
    bool   drag_active = false;
    int    drag_start_x = 0;
    int    drag_start_y = 0;
    float  drag_pan_x0  = 0.0f;
    float  drag_pan_y0  = 0.0f;

    // Audio playback
    bool       audio_playing      = false;
    bool       audio_paused       = false;
    Mix_Music *music              = nullptr;  // used for stream formats (MP3/OGG/FLAC/etc)
    Mix_Chunk *chunk              = nullptr;  // used for PCM formats (VOC/AIFF)
    int        chunk_channel      = -1;       // SDL_mixer channel chunk is playing on
    char       audio_label[512]   = {};
    double     audio_start_ticks  = 0.0;
    double     audio_position     = 0.0;
    
    // Playback controls
    int        repeat_mode        = 0;        // 0=off, 1=one song, 2=all songs
    float      volume             = 1.0f;     // 0.0 to 1.0
    bool       slideshow_active   = false;
    double     slideshow_delay    = 3.0;      // seconds between image advances
    double     slideshow_start    = 0.0;      // when current image started showing

    // CD+G — uses your CDGDisplay from cdg.h/cdg.cpp
    CDGDisplay  *cdg_display      = nullptr;  // non-null when CDG is active
    unsigned int cdg_tex          = 0;        // GL texture 300x216, updated each frame
    SDL_Texture *sdl_cdg_tex      = nullptr;  // SDL renderer path (replaces cdg_tex)

    // kfn (.kfn) karaoke — see kfn.h / fc_kfn.cpp
    // The vocal/primary track plays through the normal music/chunk fields
    // above (audio_playing, audio_position, etc); only the optional second
    // "backing" track and the lyric-sync state live here.
    bool         kfn_active          = false;
    std::string  kfn_title;
    std::string  kfn_artist;
    std::vector<std::string> kfn_lyrics;         // word-level syllables, in order
    std::vector<std::string> kfn_lyrics_lines;   // display text per line (no '/')
    std::vector<int>         kfn_line_indices;   // kfn_lyrics index where each line starts
    std::vector<double>      kfn_sync_times;     // seconds (from song start) per word
    int          kfn_current_word    = 0;
    std::string  kfn_tmp_vocal;                  // temp files, deleted on stop
    std::string  kfn_tmp_backing;
    Mix_Chunk   *kfn_chunk_backing   = nullptr;
    int          kfn_channel_backing = -1;
    bool         kfn_vocal_muted     = false;
    bool         kfn_backing_muted   = false;

    // Video playback (GStreamer)
    bool       video_playing      = false;
    bool       video_paused       = false;
#ifdef HAVE_GSTREAMER
    GstElement *video_pipeline    = nullptr;  // GStreamer pipeline for video
    GstElement *video_appsink     = nullptr;  // Sink to get decoded frames
#endif
    SDL_Texture *video_tex        = nullptr;  // Current video frame texture
    int        video_w            = 0;
    int        video_h            = 0;
    char       video_label[512]   = {};
    double     video_duration     = 0.0;
    double     video_position     = 0.0;
    double     video_start_ticks  = 0.0;
};

extern ImageViewer g_iv;
extern bool g_video_capable;  // true if GStreamer video playback is available

void iv_open(bool remote, int win_w, int win_h);
void iv_close();
void iv_tick(double dt);
void iv_render(int win_w, int win_h);
bool iv_keydown(SDL_Keycode sym);
bool iv_mousewheel(int x, int y, int delta_y, int win_w, int win_h);
bool iv_mousedown(int x, int y, int button, int win_w, int win_h);
bool iv_mousemotion(int x, int y, int win_w, int win_h);
bool iv_mouseup(int x, int y, int button);
std::string iv_write_tempfile(const unsigned char *data, size_t len, const char *ext);
void iv_delete_tempfile(const char *path);
TextLine iv_parse_markdown_line(const std::string &raw_line);
TextDocument iv_parse_text_document(const unsigned char *data, size_t len, TextFormatType format);
void iv_render_text_document(const TextDocument &doc, int viewport_x, int viewport_y, int viewport_w, int viewport_h);
void iv_text_keyboard(TextDocument &doc, SDL_Keycode sym);
void iv_text_scroll(TextDocument &doc, int delta_y);
void iv_truncate_name(const char *name, int max_chars, char *out, int out_sz);
void iv_enter_selected();
void iv_stop_audio();
void iv_cdg_free();
void iv_video_stop();
int iv_row_h();
std::string iv_home();
bool zip_contains_cdg_pair(const char *zip_path);
bool is_audio_ext(const char *name);
void iv_list_zip(const char *zip_filepath, std::vector<IVEntry> &out);
bool is_image_ext(const char *name);
bool is_zip_ext(const char *name);
bool is_cdg_ext(const char *name);
bool is_kfn_ext(const char *name);
bool is_text_ext(const char *name);
bool is_markdown_ext(const char *name);
bool is_video_ext(const char *name);
void fmt_size_iv(uint64_t sz, char *buf, int n);
void iv_refresh();
void iv_load_local(const char *filepath, const char *label);
void iv_load_image_from_mem(const unsigned char *data, size_t len, const char *label);
void iv_list_local(const char *path, std::vector<IVEntry> &out);
bool iv_open_path(const char *path);   // start in a folder, or open a file (after iv_open)
extern bool g_iv_standalone;   // Chirp on its own (felixchirp, --chirp): no "(F5)" in the title
extern void (*g_iv_busy_hook)(const char *name);   // called before a file is opened, when set (the front end may show it)
#ifdef __DJGPP__
bool iv_is_drive_name(const char *name);   // "D:" (DOS: drives in a root's listing)
#endif
void iv_list_remote(const char *path, std::vector<IVEntry> &out);
void iv_free_tex();
void iv_detail_release(bool forget_picture);   // (fc_file.cpp: a large picture's full detail, when zoomed in)
void iv_detail_draw(float img_cx, float img_cy, float effective, float vx, float vy, float vw, float vh);
int  iv_picture_full_w();
int  iv_picture_full_h();
float iv_max_zoom();
void iv_draw_texture_in_view(SDL_Texture *t, float tw, float th, float ccx, float ccy, float sc, int angle,
                             float vx, float vy, float vw, float vh);
void iv_upload_texture(const unsigned char *rgba, int w, int h);
bool has_paired_cdg(const char *dir, const char *audio_name);
unsigned char *iv_extract_zip_entry(const char *zip_filepath, const char *entry_name, size_t &out_size);
void iv_cdg_upload_texture();
void iv_draw_image(float x, float y, float w, float h);
void iv_draw_image_rotated(float cx, float cy, float w, float h, int angle_deg);
void iv_draw_text(const char *t, float x, float y, float r, float g, float b, float a);
unsigned char *iv_download_remote(const char *path, size_t &out_size);
bool iv_video_play(const char *file_path);
bool iv_cdg_load(const char *cdg_path);
void iv_ensure_mixer();
bool is_chunk_ext(const char *name);
void iv_play_audio(const char *audio_path, const char *label, bool load_cdg);
void iv_draw_image_tex(SDL_Texture *sdl_tex, unsigned int gl_tex, float x, float y, float w, float h);
void iv_draw_visualizer(float viz_x, float viz_top, float viz_w, float viz_h, float alpha_mul);

// kfn (.kfn) karaoke — see fc_kfn.cpp
bool iv_kfn_load(const char *kfn_path, const char *label);
void iv_kfn_stop();
void iv_kfn_tick();
void iv_kfn_render(float ix, float iy, float iw, float ih);
void iv_kfn_toggle_vocal_mute();
void iv_kfn_toggle_backing_mute();
