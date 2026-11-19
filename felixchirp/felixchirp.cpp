// felixchrip.cpp — F5 image viewer overlay for Felix Terminal
// Supports local filesystem and (when USESSH) remote SFTP browsing.
// Image decoding via stb_image (no extra library dependency).

// GL must be included first — before stb_image and everything else
#include <GL/glew.h>
#include <GL/gl.h>

// image_viewer.h must come before stb/miniz so its types are visible everywhere
#include "felixchirp.h"

// ---- stb_image (header-only, embedded) ------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we feed raw bytes, not filenames
// Note: do NOT use STBI_ONLY_* — the full decoder handles progressive JPEGs
// stb_image.h supports: JPEG, PNG, BMP, GIF, TIFF (and others)
// stb_image.h must be in the include path (https://github.com/nothings/stb)
#include "stb_image.h"

// ---- libwebp (for WebP support) -------------------------------------------
// WebP is decoded via libwebp if available
// Link with: -lwebp
#  include <webp/decode.h>

// ---- miniz (zip support) --------------------------------------------------
// Uses the split-header miniz distribution.
#include "miniz.h"
#include "miniz_zip.h"
// ---------------------------------------------------------------------------

#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"
float measure_text(const char *text, int font_px);   // (ft_font.cpp)
#include "../term_color.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>

#  include "../ssh_session.h"
#  include "../sftp_overlay.h"
#  include <libssh2_sftp.h>

#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#  include <io.h>
#  include <fcntl.h>
#endif

ImageViewer g_iv;
extern int  g_font_size;

// Text document currently being viewed
TextDocument s_text_doc;
bool s_viewing_text = false;

static const int IV_PAD = 8;


// ============================================================================
// HELPERS
// ============================================================================

bool is_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = IV_SUPPORTED_EXTS;
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

bool is_zip_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".zip") == 0;
}

bool is_audio_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = IV_AUDIO_EXTS;
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

bool is_cdg_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".cdg") == 0;
}

bool is_text_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = { ".txt", ".text", ".log", ".csv", ".tsv", ".conf", 
                           ".config", ".sh", ".bash", ".h", ".c", ".cpp", 
                           ".py", ".js", ".json", ".xml", ".html", ".css" };
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

bool is_markdown_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = { ".md", ".markdown", ".mdown", ".mkd", ".mdwn" };
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

bool is_video_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    const char *exts[] = IV_VIDEO_EXTS;
    for (auto *e : exts) if (strcmp(ext, e) == 0) return true;
    return false;
}

// Returns true if the file extension is a PCM chunk format that must be
// loaded with Mix_LoadWAV rather than Mix_LoadMUS.
bool is_chunk_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(ext, ".voc")  == 0 ||
           strcmp(ext, ".aiff") == 0 ||
           strcmp(ext, ".aif")  == 0;
}

// Given an audio filename, check if a matching .cdg exists in the same directory
bool has_paired_cdg(const char *dir, const char *audio_name) {
    // Strip extension, append .cdg
    char base[512];
    strncpy(base, audio_name, sizeof(base)-1);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char cdg_path[4608];
#ifndef _WIN32
    snprintf(cdg_path, sizeof(cdg_path), "%s/%s.cdg", dir, base);
#else
    snprintf(cdg_path, sizeof(cdg_path), "%s\\%s.cdg", dir, base);
#endif
    // Case-insensitive check: try both .cdg and .CDG
    FILE *f = fopen(cdg_path, "rb");
    if (f) { fclose(f); return true; }
    // Try uppercase
    char *ext = strrchr(cdg_path, '.');
    if (ext) { ext[1]='C'; ext[2]='D'; ext[3]='G'; }
    f = fopen(cdg_path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

void iv_draw_text(const char *t, float x, float y, float r, float g, float b, float a) {
    draw_text(t, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

int iv_row_h() { 
	return (int)(g_font_size * 1.8f);
}

// Download a remote file into a heap buffer. Returns nullptr on failure.
unsigned char *iv_download_remote(const char *path, size_t &out_size) {
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return nullptr;

    ssh_session_lock();
    libssh2_session_set_blocking(sess, 1);
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(sess);
    if (!sftp) { libssh2_session_set_blocking(sess, 0); ssh_session_unlock(); return nullptr; }

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(sftp, path, LIBSSH2_FXF_READ, 0);
    unsigned char *buf = nullptr;
    out_size = 0;
    if (fh) {
        // Read in chunks, grow buffer
        size_t cap = 256 * 1024;
        buf = (unsigned char *)malloc(cap);
        char tmp[32768];
        ssize_t n;
        while ((n = libssh2_sftp_read(fh, tmp, sizeof(tmp))) > 0) {
            if (out_size + (size_t)n > cap) {
                cap = (cap + (size_t)n) * 2;
                buf = (unsigned char *)realloc(buf, cap);
            }
            memcpy(buf + out_size, tmp, n);
            out_size += n;
        }
        libssh2_sftp_close(fh);
        if (out_size == 0) { free(buf); buf = nullptr; }
    }
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();
    return buf;
}

// ============================================================================
// TEXTURE LOADING
// ============================================================================

void iv_free_tex() {
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_tex) { SDL_DestroyTexture(g_iv.sdl_tex); g_iv.sdl_tex = nullptr; }
    } else {
        if (g_iv.tex) { glDeleteTextures(1, &g_iv.tex); g_iv.tex = 0; }
    }
    g_iv.tex_w = g_iv.tex_h = 0;
}

// ============================================================================
// AUDIO STOP / CDG FREE
// ============================================================================

void iv_stop_audio() {
    // Every "switch to something else" path (new song, video, image, text,
    // or an explicit stop) routes through here to tear down the current
    // track — so the KFN backing track has to be torn down here too, or
    // switching away from a kfn song leaves its backing Mix_Chunk
    // orphaned and playing forever. iv_kfn_stop() is a no-op if there's
    // no active KFN session, so this is safe to call unconditionally.
    if (g_iv.kfn_active) iv_kfn_stop();

    if (g_iv.music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_iv.music);
        g_iv.music = nullptr;
    }
    if (g_iv.chunk) {
        if (g_iv.chunk_channel >= 0) Mix_HaltChannel(g_iv.chunk_channel);
        Mix_FreeChunk(g_iv.chunk);
        g_iv.chunk         = nullptr;
        g_iv.chunk_channel = -1;
    }
    g_iv.audio_playing  = false;
    g_iv.audio_paused   = false;
    g_iv.audio_position = 0.0;
    g_iv.audio_label[0] = '\0';
}

void iv_cdg_free() {
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_cdg_tex) { SDL_DestroyTexture(g_iv.sdl_cdg_tex); g_iv.sdl_cdg_tex = nullptr; }
    } else {
        if (g_iv.cdg_tex) { glDeleteTextures(1, (GLuint*)&g_iv.cdg_tex); g_iv.cdg_tex = 0; }
    }
    if (g_iv.cdg_display) { cdg_display_free(g_iv.cdg_display); g_iv.cdg_display = nullptr; }
}

void iv_video_stop() {
#ifdef HAVE_GSTREAMER
    if (g_iv.video_pipeline) {
        gst_element_set_state(g_iv.video_pipeline, GST_STATE_NULL);
        gst_object_unref(g_iv.video_pipeline);
        g_iv.video_pipeline = nullptr;
        g_iv.video_appsink  = nullptr;
    }
#endif
    
    // Clean up textures (created in iv_tick)
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_tex) SDL_DestroyTexture(g_iv.sdl_tex);
        g_iv.sdl_tex = nullptr;
    } else {
        if (g_iv.tex) glDeleteTextures(1, &g_iv.tex);
        g_iv.tex = 0;
    }
    
    g_iv.tex_w = 0;
    g_iv.tex_h = 0;
    g_iv.video_playing  = false;
    g_iv.video_paused   = false;
    g_iv.video_position = 0.0;
    g_iv.video_duration = 0.0;
    g_iv.video_label[0] = '\0';
}

bool iv_video_play(const char *file_path) {
    iv_video_stop();

#ifndef HAVE_GSTREAMER
    (void)file_path;
    snprintf(g_iv.error, sizeof(g_iv.error), "Video playback requires GStreamer, not available in this build");
    return false;
#else
    // Build pipeline with video and audio output
    char pipeline_str[4096];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "filesrc location=\"%s\" ! decodebin name=demux "
        "demux. ! videoconvert ! video/x-raw,format=RGB ! appsink name=sink sync=false "
        "demux. ! audioconvert ! audio/x-raw,format=S16LE ! autoaudiosink",
        file_path);
    
    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &err);
    
    if (!pipeline) {
        if (err) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Pipeline error: %s", err->message);
            fprintf(stderr, "[VIDEO] %s\n", g_iv.error);
            g_error_free(err);
        } else {
            snprintf(g_iv.error, sizeof(g_iv.error), "Failed to create pipeline");
            fprintf(stderr, "[VIDEO] Failed to create pipeline\n");
        }
        return false;
    }
    
    // Get the appsink (for video only, audio goes to autoaudiosink)
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!appsink) {
        gst_object_unref(pipeline);
        snprintf(g_iv.error, sizeof(g_iv.error), "Failed to get appsink");
        fprintf(stderr, "[VIDEO] Failed to get appsink\n");
        return false;
    }
    
    // Configure appsink with sync enabled to throttle to real-time playback
    g_object_set(appsink, "sync", TRUE, "emit-signals", FALSE, "drop", FALSE, nullptr);
    
    g_iv.video_pipeline = pipeline;
    g_iv.video_appsink  = appsink;
    
    // Transition to PLAYING
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(pipeline);
        snprintf(g_iv.error, sizeof(g_iv.error), "Failed to start playback");
        fprintf(stderr, "[VIDEO] Failed to start playback\n");
        g_iv.video_pipeline = nullptr;
        g_iv.video_appsink = nullptr;
        return false;
    }
    
    // Wait briefly for state change to complete
    gst_element_get_state(pipeline, nullptr, nullptr, 1 * GST_SECOND);
    
    g_iv.video_playing = true;
    g_iv.video_paused  = false;
    g_iv.video_start_ticks = (double)SDL_GetTicks();
    
    snprintf(g_iv.video_label, sizeof(g_iv.video_label), "%s", 
             file_path[0] ? (strrchr(file_path, '/') ? strrchr(file_path, '/')+1 : file_path) : "");
    
    g_iv.error[0] = '\0';
    return true;
#endif
}

bool iv_cdg_load(const char *cdg_path) {
    iv_cdg_free();
    g_iv.cdg_display = cdg_display_new();
    if (!g_iv.cdg_display) return false;
    if (!cdg_load_file(g_iv.cdg_display, cdg_path)) {
        cdg_display_free(g_iv.cdg_display);
        g_iv.cdg_display = nullptr;
        return false;
    }
    // Upload an initial (blank) texture immediately so the display area
    // shows the CDG frame from frame 1 rather than falling through to
    // the audio-only UI while waiting for iv_tick to run.
    iv_cdg_upload_texture();
    return true;
}

void iv_cdg_upload_texture() {
    CDGDisplay *cdg = g_iv.cdg_display;
    if (!cdg) return;
    static uint8_t rgba[CDG_HEIGHT * CDG_WIDTH * 4];
    for (int y = 0; y < CDG_HEIGHT; y++) {
        for (int x = 0; x < CDG_WIDTH; x++) {
            uint8_t idx = cdg->screen[y][x] & 0x0F;
            uint32_t col = cdg->palette[idx];
            int off = (y * CDG_WIDTH + x) * 4;
            rgba[off+0] = (col >> 16) & 0xFF;
            rgba[off+1] = (col >>  8) & 0xFF;
            rgba[off+2] =  col        & 0xFF;
            rgba[off+3] = 0xFF;
        }
    }
    if (g_use_sdl_renderer) {
        if (g_iv.sdl_cdg_tex) SDL_DestroyTexture(g_iv.sdl_cdg_tex);
        g_iv.sdl_cdg_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            CDG_WIDTH, CDG_HEIGHT);
        if (g_iv.sdl_cdg_tex)
            SDL_UpdateTexture(g_iv.sdl_cdg_tex, nullptr, rgba, CDG_WIDTH * 4);
    } else {
        if (!g_iv.cdg_tex) {
            glGenTextures(1, (GLuint*)&g_iv.cdg_tex);
            glBindTexture(GL_TEXTURE_2D, g_iv.cdg_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CDG_WIDTH, CDG_HEIGHT, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_iv.cdg_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CDG_WIDTH, CDG_HEIGHT,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// ============================================================================
// AUDIO PLAY
// ============================================================================

static bool s_mixer_ready = false;

void iv_ensure_mixer() {
    if (s_mixer_ready) return;
    // Call Mix_Init before Mix_OpenAudio so SDL_mixer uses whichever codec
    // libs are linked in at build time rather than trying to dlopen them.
    Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC |
             MIX_INIT_OPUS | MIX_INIT_MID | MIX_INIT_MOD);
    Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
    Mix_AllocateChannels(8);
    s_mixer_ready = true;
}

void iv_play_audio(const char *audio_path, const char *label, bool load_cdg) {
    iv_stop_audio();
    iv_cdg_free();
    iv_free_tex();
    iv_ensure_mixer();

    if (is_chunk_ext(audio_path)) {
        // VOC, AIFF — PCM chunk formats, not supported by Mix_LoadMUS
        g_iv.chunk = Mix_LoadWAV(audio_path);
        if (!g_iv.chunk) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Cannot load: %s", Mix_GetError());
            return;
        }
        g_iv.chunk_channel = Mix_PlayChannel(-1, g_iv.chunk, 0);
        if (g_iv.chunk_channel < 0) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Cannot play: %s", Mix_GetError());
            Mix_FreeChunk(g_iv.chunk);
            g_iv.chunk = nullptr;
            return;
        }
    } else {
        g_iv.music = Mix_LoadMUS(audio_path);
        if (!g_iv.music) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Cannot load: %s", Mix_GetError());
            return;
        }
        Mix_PlayMusic(g_iv.music, 1);
    }

    g_iv.audio_playing     = true;
    g_iv.audio_paused      = false;
    g_iv.audio_position    = 0.0;
    g_iv.audio_start_ticks = (double)SDL_GetTicks();
    strncpy(g_iv.audio_label, label, sizeof(g_iv.audio_label)-1);
    g_iv.error[0] = '\0';

    if (load_cdg) {
        char cdg_path[4096];
        strncpy(cdg_path, audio_path, sizeof(cdg_path)-1);
        char *dot = strrchr(cdg_path, '.');
        if (dot) {
            strcpy(dot, ".cdg");
            if (!iv_cdg_load(cdg_path)) { strcpy(dot, ".CDG"); iv_cdg_load(cdg_path); }
        }
    }
}

void iv_upload_texture(const unsigned char *rgba, int w, int h) {
    iv_free_tex();
    if (g_use_sdl_renderer) {
        g_iv.sdl_tex = SDL_CreateTexture(g_sdl_renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (g_iv.sdl_tex) {
            SDL_SetTextureBlendMode(g_iv.sdl_tex, SDL_BLENDMODE_BLEND);
            SDL_UpdateTexture(g_iv.sdl_tex, nullptr, rgba, w * 4);
        }
    } else {
        glGenTextures(1, &g_iv.tex);
        glBindTexture(GL_TEXTURE_2D, g_iv.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    g_iv.tex_w = w;
    g_iv.tex_h = h;
}

// ============================================================================
// TICK  (call every frame)
// ============================================================================

void iv_tick(double /*dt*/) {
    if (!g_iv.visible) return;

    // Update audio position from wall clock whenever audio is playing
    if (g_iv.audio_playing && !g_iv.audio_paused) {
        g_iv.audio_position = ((double)SDL_GetTicks() - g_iv.audio_start_ticks) / 1000.0;
        // Check for playback completion
        bool finished = g_iv.music  ? !Mix_PlayingMusic() :
                        g_iv.chunk  ? !Mix_Playing(g_iv.chunk_channel) : true;
        if (finished) {
            // Handle repeat mode
            if (g_iv.repeat_mode == 1) {
                // Repeat one: replay current song
                iv_enter_selected();
            } else if (g_iv.repeat_mode == 2) {
                // Repeat all: advance to next song and play
                int n = (int)g_iv.entries.size();
                if (n > 0) {
                    g_iv.selected = (g_iv.selected + 1) % n;
                    iv_enter_selected();
                }
            } else {
                // No repeat: just stop
                g_iv.audio_playing  = false;
                g_iv.audio_position = 0.0;
                if (g_iv.kfn_active) iv_kfn_stop();
            }
        }
    }

    // Advance KFN lyric sync (word index) using the audio_position updated above.
    iv_kfn_tick();

    // Update video playback and pull frames
#ifdef HAVE_GSTREAMER
    if (g_iv.video_playing && !g_iv.video_paused && g_iv.video_pipeline) {
        g_iv.video_position = ((double)SDL_GetTicks() - g_iv.video_start_ticks) / 1000.0;
        
        // Pull available samples from appsink (with timeout to allow sync to work)
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(g_iv.video_appsink), 50 * GST_MSECOND);
        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstCaps *caps = gst_sample_get_caps(sample);
            
            if (buffer && caps) {
                GstStructure *s = gst_caps_get_structure(caps, 0);
                gint w = 0, h = 0;
                if (gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h)) {
                    // Get the presentation time of this frame for proper sync
                    GstClockTime pts = GST_BUFFER_PTS(buffer);
                    if (GST_CLOCK_TIME_IS_VALID(pts)) {
                        // Update position based on buffer timestamp
                        g_iv.video_position = (double)pts / GST_SECOND;
                    }
                    
                    GstMapInfo map;
                    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                        // Create/update texture (handle both OpenGL and SDL renderer modes)
                        if (!g_iv.tex || g_iv.tex_w != w || g_iv.tex_h != h) {
                            // Need to create new texture or size changed
                            g_iv.tex_w = w;
                            g_iv.tex_h = h;
                            
                            if (g_use_sdl_renderer) {
                                // SDL Renderer mode
                                if (g_iv.sdl_tex) SDL_DestroyTexture(g_iv.sdl_tex);
                                g_iv.sdl_tex = SDL_CreateTexture(g_sdl_renderer,
                                    SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
                                if (g_iv.sdl_tex) {
                                    fprintf(stderr, "[VIDEO] Created SDL texture: %dx%d\n", w, h);
                                } else {
                                    fprintf(stderr, "[VIDEO] ERROR: SDL_CreateTexture failed: %s\n", SDL_GetError());
                                }
                            } else {
                                // OpenGL mode - convert RGB24 to RGBA32
                                unsigned char *rgba = (unsigned char*)malloc(w * h * 4);
                                if (rgba) {
                                    // Convert RGB24 to RGBA32 (add alpha channel)
                                    for (int i = 0, j = 0; i < w * h * 3; i += 3, j += 4) {
                                        rgba[j+0] = map.data[i+0];  // R
                                        rgba[j+1] = map.data[i+1];  // G
                                        rgba[j+2] = map.data[i+2];  // B
                                        rgba[j+3] = 255;            // A
                                    }
                                    
                                    if (g_iv.tex) glDeleteTextures(1, &g_iv.tex);
                                    
                                    glGenTextures(1, &g_iv.tex);
                                    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    fprintf(stderr, "[VIDEO] Created OpenGL texture: %dx%d\n", w, h);
                                    free(rgba);
                                } else {
                                    fprintf(stderr, "[VIDEO] ERROR: Failed to allocate RGBA buffer\n");
                                    g_iv.tex = 0;
                                }
                            }
                        } else if (g_iv.tex || g_iv.sdl_tex) {
                            // Update existing texture with new frame
                            if (g_use_sdl_renderer && g_iv.sdl_tex) {
                                SDL_UpdateTexture(g_iv.sdl_tex, nullptr, map.data, w * 3);
                            } else if (!g_use_sdl_renderer && g_iv.tex) {
                                // For OpenGL, convert and update using glTexSubImage2D
                                unsigned char *rgba = (unsigned char*)malloc(w * h * 4);
                                if (rgba) {
                                    for (int i = 0, j = 0; i < w * h * 3; i += 3, j += 4) {
                                        rgba[j+0] = map.data[i+0];
                                        rgba[j+1] = map.data[i+1];
                                        rgba[j+2] = map.data[i+2];
                                        rgba[j+3] = 255;
                                    }
                                    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
                                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    free(rgba);
                                }
                            }
                        }
                        
                        gst_buffer_unmap(buffer, &map);
                    }
                }
            }
            gst_sample_unref(sample);
        }
        
        // Check for EOS (end of stream)
        GstBus *bus = gst_element_get_bus(g_iv.video_pipeline);
        GstMessage *msg = gst_bus_pop_filtered(bus, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                fprintf(stderr, "[VIDEO] End of stream\n");
                iv_video_stop();
            } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError *err = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "[VIDEO] Error: %s\n", err->message);
                if (debug) fprintf(stderr, "[VIDEO] Debug: %s\n", debug);
                g_error_free(err);
                g_free(debug);
                iv_video_stop();
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }
#endif

    // Advance CDG to current position using your cdg_update()
    if (g_iv.cdg_display) {
        cdg_update(g_iv.cdg_display, g_iv.audio_position);
        iv_cdg_upload_texture();
    }

    // Handle slideshow auto-advance for images
    if (g_iv.slideshow_active && !(g_iv.audio_playing || g_iv.audio_paused)) {
        double now = (double)SDL_GetTicks() / 1000.0;  // Convert to seconds
        if (g_iv.slideshow_start == 0.0) {
            g_iv.slideshow_start = now;  // Initialize timer on first check
        } else if (now - g_iv.slideshow_start >= g_iv.slideshow_delay) {
            // Time to advance to next image
            int n = (int)g_iv.entries.size();
            if (n > 0) {
                // Find next image file (skip directories)
                int start_idx = g_iv.selected;
                int next_idx = (start_idx + 1) % n;
                int attempts = 0;
                while (attempts < n && g_iv.entries[next_idx].is_dir) {
                    next_idx = (next_idx + 1) % n;
                    attempts++;
                }
                if (attempts < n) {
                    g_iv.selected = next_idx;
                    // Reload the selected image
                    const IVEntry &e = g_iv.entries[g_iv.selected];
                    if (!e.is_dir && !e.is_audio && !e.is_cdg) {
                        // Load as image for slideshow
                        char full_path[5000];
                        if (g_iv.in_zip) {
                            snprintf(full_path, sizeof(full_path), "%s|%s", g_iv.zip_file, e.name);
                        } else {
                            snprintf(full_path, sizeof(full_path), "%s/%s", g_iv.path, e.name);
                        }
                        if (!g_iv.in_zip) {
                            iv_load_local(full_path, e.name);
                        }
                    }
                    g_iv.slideshow_start = now;  // Reset timer
                } else {
                    g_iv.slideshow_active = false;  // No more images
                }
            }
        }
    }
}

void iv_refresh() {
    g_iv.selected   = 0;
    g_iv.scroll_top = 0;
    if (g_iv.in_zip) {
        iv_list_zip(g_iv.zip_file, g_iv.entries);
    } else if (g_iv.remote) {
        iv_list_remote(g_iv.path, g_iv.entries);
    } else {
        iv_list_local(g_iv.path, g_iv.entries);
    }
}

// Called just before a file is opened, which can take a few seconds (a
// .kfn's tracks to unpack, a zip to look through, a large picture): the
// front end may show that something is happening. Felix Terminal doesn't;
// Chirp for DOS (msdos/chirp) draws "Loading ...".
__attribute__((weak)) void iv_show_busy(const char *) {}

void iv_enter_selected() {
    if (g_iv.entries.empty()) return;
    const IVEntry &e = g_iv.entries[g_iv.selected];
    if (!e.is_dir) iv_show_busy(e.name);

    // ── Inside a zip: handle image, audio, or CDG entry ──────────────────
    if (e.is_zip_entry) {
        g_iv.error[0] = '\0';

        if (e.is_audio) {
            // Extract audio to a temp file (SDL_mixer needs a real file path)
            size_t sz = 0;
            unsigned char *buf = iv_extract_zip_entry(e.zip_path, e.zip_entry, sz);
            if (!buf) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Failed to extract: %s", e.name);
                return;
            }
            const char *dot = strrchr(e.name, '.');
            std::string tmp_path = iv_write_tempfile(buf, sz, dot ? dot : ".tmp");
            free(buf);
            if (tmp_path.empty()) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                return;
            }

            // If CDG pair exists in the zip, extract it too
            bool load_cdg = e.has_cdg_pair;
            if (load_cdg) {
                char cdg_entry[512]; strncpy(cdg_entry, e.zip_entry, sizeof(cdg_entry)-1);
                char *edot = strrchr(cdg_entry, '.'); if (edot) strcpy(edot, ".cdg");
                size_t csz = 0;
                unsigned char *cbuf = iv_extract_zip_entry(e.zip_path, cdg_entry, csz);
                if (!cbuf) {
                    if (edot) strcpy(edot, ".CDG");
                    cbuf = iv_extract_zip_entry(e.zip_path, cdg_entry, csz);
                }
                if (cbuf) {
                    std::string tmp_cdg = iv_write_tempfile(cbuf, csz, ".cdg");
                    free(cbuf);
                    iv_stop_audio(); iv_cdg_free(); iv_ensure_mixer();
                    g_iv.music = Mix_LoadMUS(tmp_path.c_str());
                    if (g_iv.music) {
                        Mix_PlayMusic(g_iv.music, 1);
                        g_iv.audio_playing     = true;
                        g_iv.audio_paused      = false;
                        g_iv.audio_position    = 0.0;
                        g_iv.audio_start_ticks = (double)SDL_GetTicks();
                        strncpy(g_iv.audio_label, e.name, sizeof(g_iv.audio_label)-1);
                        if (!tmp_cdg.empty()) iv_cdg_load(tmp_cdg.c_str());
                    }
                    if (!tmp_cdg.empty()) iv_delete_tempfile(tmp_cdg.c_str());
                    iv_delete_tempfile(tmp_path.c_str());
                    return;
                }
            }

            iv_play_audio(tmp_path.c_str(), e.name, false);
            iv_delete_tempfile(tmp_path.c_str());
            return;
        }

        // Image entry
        iv_free_tex();
        iv_stop_audio();
        iv_cdg_free();
        size_t sz = 0;
        unsigned char *buf = iv_extract_zip_entry(e.zip_path, e.zip_entry, sz);
        if (buf) {
            iv_load_image_from_mem(buf, sz, e.name);
            free(buf);
        } else {
            snprintf(g_iv.error, sizeof(g_iv.error), "Failed to extract: %s", e.name);
        }
        return;
    }

    if (e.is_dir) {
        // ".." from inside a zip goes back to the filesystem directory
        if (strcmp(e.name, "..") == 0 && g_iv.in_zip) {
            g_iv.in_zip = false;
            g_iv.zip_file[0] = '\0';
            iv_refresh();
            return;
        }
#ifdef __DJGPP__
        // DOS: "D:" in a drive root's listing (iv_list_local) is that drive
        if (!g_iv.remote && iv_is_drive_name(e.name)) {
            snprintf(g_iv.path, sizeof(g_iv.path), "%c:\\", toupper((unsigned char)e.name[0]));
            iv_refresh();
            return;
        }
#endif
        // Navigate into directory
        char newpath[4096];
        if (strcmp(e.name, "..") == 0) {
            char tmp[4096];
            strncpy(tmp, g_iv.path, sizeof(tmp)-1);
            if (g_iv.remote || tmp[0] == '/') {
                char *slash = strrchr(tmp, '/');
                if (slash && slash != tmp) { *slash = '\0'; }
                else { tmp[0]='/'; tmp[1]='\0'; }
            } else {
                char *slash = strrchr(tmp, '\\');
                if (!slash) slash = strrchr(tmp, '/');
                if (slash && slash != tmp) { *slash = '\0'; }
                else { tmp[0]='\\'; tmp[1]='\0'; }
#ifdef __DJGPP__
                // "C:\GAMES" -> "C:", which DOS takes as C:'s current folder: the root, "C:\"
                if (tmp[0] && tmp[1] == ':' && !tmp[2]) { tmp[2] = '\\'; tmp[3] = '\0'; }
#endif
            }
            strncpy(g_iv.path, tmp, sizeof(g_iv.path)-1);
        } else {
            // Avoid double-slash when path is already "/"
            size_t plen = strlen(g_iv.path);
            bool ends_sep = plen > 0 &&
                (g_iv.path[plen-1] == '/' || g_iv.path[plen-1] == '\\');
            const char *sep = ends_sep ? "" :
                              (g_iv.remote || g_iv.path[0] == '/') ? "/" : "\\";
            snprintf(newpath, sizeof(newpath), "%s%s%s", g_iv.path, sep, e.name);
            strncpy(g_iv.path, newpath, sizeof(g_iv.path)-1);
        }
        iv_refresh();

    } else if (e.is_zip) {
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 g_iv.path,
                 (g_iv.remote || g_iv.path[0] == '/') ? "/" : "\\",
                 e.name);

        // For remote zips, download to a temp file first
        std::string tmp_zip_path;
        const char *zip_to_open = fullpath;
        if (g_iv.remote) {
            size_t zsz = 0;
            unsigned char *zbuf = iv_download_remote(fullpath, zsz);
            if (!zbuf) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
                return;
            }
            tmp_zip_path = iv_write_tempfile(zbuf, zsz, ".zip");
            free(zbuf);
            if (tmp_zip_path.empty()) {
                snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                return;
            }
            zip_to_open = tmp_zip_path.c_str();
        }

        // Peek inside the zip — if it contains a CDG+audio pair, auto-play it
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        bool auto_played = false;
        if (mz_zip_reader_init_file(&zip, zip_to_open, 0)) {
            mz_uint n = mz_zip_reader_get_num_files(&zip);

            std::vector<std::string> all_names;
            for (mz_uint i = 0; i < n; i++) {
                char fname[512] = {};
                mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
                all_names.push_back(fname);
            }

            for (mz_uint i = 0; i < n && !auto_played; i++) {
                if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
                char fname[512] = {};
                mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
                if (!is_audio_ext(fname)) continue;

                char cdg_entry[512]; strncpy(cdg_entry, fname, sizeof(cdg_entry)-1);
                char *dot = strrchr(cdg_entry, '.'); if (!dot) continue;
                strcpy(dot, ".cdg");
                bool found_cdg = false;
                for (auto &nm : all_names)
                    if (strcasecmp(nm.c_str(), cdg_entry) == 0) { found_cdg = true; break; }
                if (!found_cdg) { strcpy(dot, ".CDG");
                    for (auto &nm : all_names)
                        if (strcasecmp(nm.c_str(), cdg_entry) == 0) { found_cdg = true; break; }
                }
                if (!found_cdg) continue;

                mz_zip_reader_end(&zip);

                auto extract_to_tmp = [&](const char *entry, const char *ext) -> std::string {
                    size_t sz = 0;
                    unsigned char *buf = iv_extract_zip_entry(zip_to_open, entry, sz);
                    if (!buf) return "";
                    std::string path = iv_write_tempfile(buf, sz, ext);
                    free(buf);
                    return path;
                };

                std::string cdg_actual;
                for (auto &nm : all_names)
                    if (strcasecmp(nm.c_str(), cdg_entry) == 0) { cdg_actual = nm; break; }

                const char *audio_ext = strrchr(fname, '.');
                std::string tmp_audio = extract_to_tmp(fname, audio_ext ? audio_ext : ".mp3");
                std::string tmp_cdg   = extract_to_tmp(cdg_actual.c_str(), ".cdg");

                if (!tmp_audio.empty()) {
                    const char *display_name = strrchr(fname, '/');
                    display_name = display_name ? display_name+1 : fname;

                    iv_stop_audio(); iv_cdg_free(); iv_free_tex();
                    iv_ensure_mixer();
                    g_iv.music = Mix_LoadMUS(tmp_audio.c_str());
                    if (g_iv.music) {
                        Mix_PlayMusic(g_iv.music, 1);
                        g_iv.audio_playing     = true;
                        g_iv.audio_paused      = false;
                        g_iv.audio_position    = 0.0;
                        g_iv.audio_start_ticks = (double)SDL_GetTicks();
                        strncpy(g_iv.audio_label, display_name, sizeof(g_iv.audio_label)-1);
                        g_iv.error[0] = '\0';
                        if (!tmp_cdg.empty()) iv_cdg_load(tmp_cdg.c_str());
                    }
                    iv_delete_tempfile(tmp_audio.c_str());
                    if (!tmp_cdg.empty()) iv_delete_tempfile(tmp_cdg.c_str());
                    auto_played = true;
                }
                break;
            }

            if (!auto_played) mz_zip_reader_end(&zip);
        }

        if (!tmp_zip_path.empty()) iv_delete_tempfile(tmp_zip_path.c_str());

        if (!auto_played) {
            // No CDG pair — browse mode (re-download if remote, or use local path)
            if (g_iv.remote) {
                // Re-download for browsing (or keep temp — but we deleted it above)
                size_t zsz = 0;
                unsigned char *zbuf = iv_download_remote(fullpath, zsz);
                if (zbuf) {
                    std::string tmp2 = iv_write_tempfile(zbuf, zsz, ".zip");
                    free(zbuf);
                    if (!tmp2.empty()) {
                        strncpy(g_iv.zip_file, tmp2.c_str(), sizeof(g_iv.zip_file)-1);
                        g_iv.in_zip = true; g_iv.selected = 0; g_iv.scroll_top = 0;
                        iv_list_zip(tmp2.c_str(), g_iv.entries);
                        // keep temp alive for zip entry extraction
                    }
                }
            } else {
                strncpy(g_iv.zip_file, fullpath, sizeof(g_iv.zip_file)-1);
                g_iv.in_zip = true; g_iv.selected = 0; g_iv.scroll_top = 0;
                iv_list_zip(fullpath, g_iv.entries);
            }
        }

    } else {
        // Load plain image or play audio
        g_iv.error[0] = '\0';
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 g_iv.path,
                 (g_iv.remote || g_iv.path[0] == '/') ? "/" : "\\",
                 e.name);

        if (e.is_kfn) {
            if (g_iv.remote) {
                // Download the whole package to a temp file first — the KFN
                // reader needs random (seekable) file access.
                size_t sz = 0;
                unsigned char *buf = iv_download_remote(fullpath, sz);
                if (buf) {
                    std::string tmp = iv_write_tempfile(buf, sz, ".kfn");
                    free(buf);
                    if (!tmp.empty()) {
                        iv_kfn_load(tmp.c_str(), e.name);
                        iv_delete_tempfile(tmp.c_str());  // audio tracks are already
                                                           // extracted to their own temp files
                    } else {
                        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                    }
                } else {
                    snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
                }
            } else {
                iv_kfn_load(fullpath, e.name);
            }

        } else if (e.is_audio) {
            if (g_iv.remote) {
                // Download to temp file — SDL_mixer can't read from SSH paths
                size_t sz = 0;
                unsigned char *buf = iv_download_remote(fullpath, sz);
                if (buf) {
                    const char *dot = strrchr(e.name, '.');
                    std::string tmp = iv_write_tempfile(buf, sz, dot ? dot : ".tmp");
                    free(buf);
                    if (!tmp.empty()) {
                        iv_play_audio(tmp.c_str(), e.name, false);
                        iv_delete_tempfile(tmp.c_str());
                    } else {
                        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                    }
                } else {
                    snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
                }
            } else {
                iv_play_audio(fullpath, e.name, e.has_cdg_pair);
            }

        } else if (e.is_video) {
            // Video playback
            iv_free_tex();
            iv_stop_audio();
            iv_cdg_free();
            s_viewing_text = false;
            
            if (g_iv.remote) {
                // Download to temp file — GStreamer can't read from SSH paths
                size_t sz = 0;
                unsigned char *buf = iv_download_remote(fullpath, sz);
                if (buf) {
                    const char *dot = strrchr(e.name, '.');
                    std::string tmp = iv_write_tempfile(buf, sz, dot ? dot : ".tmp");
                    free(buf);
                    if (!tmp.empty()) {
                        iv_video_play(tmp.c_str());
                        iv_delete_tempfile(tmp.c_str());
                    } else {
                        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot write temp file");
                    }
                } else {
                    snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
                }
            } else {
                iv_video_play(fullpath);
            }

        } else if (is_text_ext(e.name) || is_markdown_ext(e.name)) {
            // Text or Markdown file
            iv_free_tex();
            iv_stop_audio();
            iv_cdg_free();
            g_iv.error[0] = '\0';
            
            // Clear old text document immediately
            s_text_doc.lines.clear();
            s_viewing_text = false;

            size_t sz = 0;
            unsigned char *buf = nullptr;
            bool success = false;
            
            if (g_iv.remote) {
                buf = iv_download_remote(fullpath, sz);
                if (!buf) {
                    snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
                } else {
                    success = true;
                }
            } else {
                // Read local file
                FILE *f = fopen(fullpath, "rb");
                if (!f) {
                    snprintf(g_iv.error, sizeof(g_iv.error), "Cannot open: %s", e.name);
                } else {
                    fseek(f, 0, SEEK_END);
                    sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    buf = (unsigned char *)malloc(sz + 1);
                    if (!buf) {
                        fclose(f);
                        snprintf(g_iv.error, sizeof(g_iv.error), "Out of memory");
                    } else {
                        if (fread(buf, 1, sz, f) != sz) {
                            free(buf);
                            buf = nullptr;
                            snprintf(g_iv.error, sizeof(g_iv.error), "Read error: %s", e.name);
                        } else {
                            success = true;
                        }
                        fclose(f);
                    }
                }
            }

            if (success && buf) {
                // Validate that this looks like text (not binary/encoded)
                // Check if more than 10% of bytes are non-printable (excluding common whitespace)
                int non_printable = 0;
                for (size_t j = 0; j < sz && j < 1000; j++) {  // Sample first 1000 bytes
                    unsigned char c = buf[j];
                    if (c < 9 || (c > 13 && c < 32) || c == 127) {
                        non_printable++;
                    }
                }
                if (non_printable * 10 > 1000) {
                    // More than 10% non-printable: likely binary or encoded
                    snprintf(g_iv.error, sizeof(g_iv.error), "Binary file: %s", e.name);
                    s_text_doc.lines.clear();
                    s_viewing_text = false;
                    free(buf);
                    return;
                }

                // Parse document
                TextFormatType format = is_markdown_ext(e.name) ? TEXT_MARKDOWN : TEXT_PLAIN;
                s_text_doc = iv_parse_text_document(buf, sz, format);
                s_text_doc.scroll_line = 0;
                s_viewing_text = true;
                free(buf);
                
                // Mark that we're viewing text, not an image
                g_iv.tex = 0;
                g_iv.sdl_tex = nullptr;
            }
            return;

        } else {
            // Image file
            iv_free_tex();
            iv_stop_audio();
            iv_cdg_free();
            s_viewing_text = false;  // Switch back to image mode
            s_text_doc.lines.clear();  // Clear previous text document
            if (g_iv.remote) {
                size_t sz = 0;
                unsigned char *buf = iv_download_remote(fullpath, sz);
                if (buf) { iv_load_image_from_mem(buf, sz, e.name); free(buf); }
                else snprintf(g_iv.error, sizeof(g_iv.error), "Failed to download: %s", e.name);
            } else {
                iv_load_local(fullpath, e.name);
            }
        }
    }
}

// ============================================================================
// RENDER
// ============================================================================

void iv_draw_image_tex(SDL_Texture *sdl_tex, unsigned int gl_tex, float x, float y, float w, float h) {
    if (g_use_sdl_renderer) {
        if (!sdl_tex) return;
        gl_flush_verts();
        SDL_Vertex verts[6];
        auto mkv = [](float vx, float vy, float u, float v) {
            SDL_Vertex sv{};
            sv.position.x = vx; sv.position.y = vy;
            sv.color = {255, 255, 255, 255};
            sv.tex_coord.x = u; sv.tex_coord.y = v;
            return sv;
        };
        verts[0] = mkv(x,   y,   0.f, 0.f);
        verts[1] = mkv(x+w, y,   1.f, 0.f);
        verts[2] = mkv(x+w, y+h, 1.f, 1.f);
        verts[3] = mkv(x,   y,   0.f, 0.f);
        verts[4] = mkv(x+w, y+h, 1.f, 1.f);
        verts[5] = mkv(x,   y+h, 0.f, 1.f);
        SDL_RenderGeometry(g_sdl_renderer, sdl_tex, verts, 6, nullptr, 0);
        return;
    }
    if (!gl_tex) return;

    gl_flush_verts();

    GLint prev_prog;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);

    static GLuint s_tex_prog = 0;
    static GLuint s_tex_vao  = 0;
    static GLuint s_tex_vbo  = 0;
    if (!s_tex_prog) {
        const char *vs =
            "#version 330 core\n"
            "layout(location=0) in vec4 vtx;\n"
            "out vec2 uv;\n"
            "void main(){ gl_Position=vec4(vtx.xy,0,1); uv=vtx.zw; }\n";
        const char *fs =
            "#version 330 core\n"
            "in vec2 uv;\n"
            "out vec4 frag;\n"
            "uniform sampler2D tex;\n"
            "void main(){ frag=texture(tex,uv); }\n";
        auto compile = [](GLenum t, const char *src) {
            GLuint s = glCreateShader(t);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            return s;
        };
        GLuint v = compile(GL_VERTEX_SHADER, vs);
        GLuint f = compile(GL_FRAGMENT_SHADER, fs);
        s_tex_prog = glCreateProgram();
        glAttachShader(s_tex_prog, v); glAttachShader(s_tex_prog, f);
        glLinkProgram(s_tex_prog);
        glDeleteShader(v); glDeleteShader(f);
        glGenVertexArrays(1, &s_tex_vao);
        glGenBuffers(1, &s_tex_vbo);
        glBindVertexArray(s_tex_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_tex_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
        glBindVertexArray(0);
    }

    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    float ww = (float)vp[2], wh = (float)vp[3];
    auto px2ndc_x = [&](float px) { return (px / ww) * 2.f - 1.f; };
    auto px2ndc_y = [&](float py) { return 1.f - (py / wh) * 2.f; };

    float x0 = px2ndc_x(x),   y0 = px2ndc_y(y);
    float x1 = px2ndc_x(x+w), y1 = px2ndc_y(y+h);
    float verts[24] = {
        x0, y0, 0.f, 0.f,  x1, y0, 1.f, 0.f,  x1, y1, 1.f, 1.f,
        x0, y0, 0.f, 0.f,  x1, y1, 1.f, 1.f,  x0, y1, 0.f, 1.f,
    };

    glUseProgram(s_tex_prog);
    glBindVertexArray(s_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glUniform1i(glGetUniformLocation(s_tex_prog, "tex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(prev_prog);
}

void iv_draw_image(float x, float y, float w, float h) {
    iv_draw_image_tex(g_iv.sdl_tex, g_iv.tex, x, y, w, h);
}

// Draw a texture as a rotated quad. cx/cy = centre in pixels, w/h = display
// dimensions before rotation, angle_deg = clockwise degrees (0/90/180/270).
// UV assignment rotates the texture coords to match so the image actually turns.
void iv_draw_image_rotated(float cx, float cy, float w, float h, int angle_deg) {
    // Build 4 corners in pixel space, rotated around centre
    float rad  = (float)(angle_deg * M_PI / 180.0);
    float cosA = cosf(rad), sinA = sinf(rad);
    float hw = w * 0.5f, hh = h * 0.5f;

    // Local corner offsets (before rotation): TL, TR, BR, BL
    float lx[4] = { -hw,  hw,  hw, -hw };
    float ly[4] = { -hh, -hh,  hh,  hh };

    float px[4], py[4];
    for (int i = 0; i < 4; i++) {
        px[i] = cx + lx[i] * cosA - ly[i] * sinA;
        py[i] = cy + lx[i] * sinA + ly[i] * cosA;
    }

    // UV coords for each corner (TL, TR, BR, BL)
    // NOTE: The quad geometry is already rotated via cosA/sinA above.
    // We keep UVs normal - don't rotate them too (that would double-rotate).
    float u[4] = {0, 1, 1, 0};
    float v[4] = {0, 0, 1, 1};

    if (g_use_sdl_renderer) {
        if (!g_iv.sdl_tex) return;
        gl_flush_verts();
        SDL_Vertex verts[6];
        auto mkv = [](float vx, float vy, float tu, float tv) {
            SDL_Vertex sv{};
            sv.position.x = vx; sv.position.y = vy;
            sv.color = {255, 255, 255, 255};
            sv.tex_coord.x = tu; sv.tex_coord.y = tv;
            return sv;
        };
        // Two triangles: TL-TR-BR and TL-BR-BL
        verts[0] = mkv(px[0], py[0], u[0], v[0]);
        verts[1] = mkv(px[1], py[1], u[1], v[1]);
        verts[2] = mkv(px[2], py[2], u[2], v[2]);
        verts[3] = mkv(px[0], py[0], u[0], v[0]);
        verts[4] = mkv(px[2], py[2], u[2], v[2]);
        verts[5] = mkv(px[3], py[3], u[3], v[3]);
        SDL_RenderGeometry(g_sdl_renderer, g_iv.sdl_tex, verts, 6, nullptr, 0);
        return;
    }

    if (!g_iv.tex) return;
    gl_flush_verts();

    // Reuse the existing shader from iv_draw_image_tex (it's in a static)
    // We need to call it directly with rotated NDC verts.
    // Since the shader is static-local to iv_draw_image_tex, duplicate the
    // minimal GL draw here using the same VAO trick with a custom vert array.
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    float ww = (float)vp[2], wh = (float)vp[3];
    auto nx = [&](float x2) { return (x2 / ww) * 2.f - 1.f; };
    auto ny = [&](float y2) { return 1.f - (y2 / wh) * 2.f; };

    // Build 6-vert array: TL-TR-BR, TL-BR-BL (xyuv per vert)
    float verts[24] = {
        nx(px[0]),ny(py[0]),u[0],v[0],
        nx(px[1]),ny(py[1]),u[1],v[1],
        nx(px[2]),ny(py[2]),u[2],v[2],
        nx(px[0]),ny(py[0]),u[0],v[0],
        nx(px[2]),ny(py[2]),u[2],v[2],
        nx(px[3]),ny(py[3]),u[3],v[3],
    };

    // Use a simple self-contained program for the rotated draw
    static GLuint s_rot_prog = 0;
    static GLuint s_rot_vao  = 0;
    static GLuint s_rot_vbo  = 0;
    if (!s_rot_prog) {
        const char *vs =
            "#version 330 core\n"
            "layout(location=0) in vec4 vtx;\n"
            "out vec2 uv;\n"
            "void main(){ gl_Position=vec4(vtx.xy,0,1); uv=vtx.zw; }\n";
        const char *fs =
            "#version 330 core\n"
            "in vec2 uv; out vec4 frag;\n"
            "uniform sampler2D tex;\n"
            "void main(){ frag=texture(tex,uv); }\n";
        auto cmp = [](GLenum t, const char *src) {
            GLuint s = glCreateShader(t);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            return s;
        };
        GLuint vs2 = cmp(GL_VERTEX_SHADER, vs);
        GLuint fs2 = cmp(GL_FRAGMENT_SHADER, fs);
        s_rot_prog = glCreateProgram();
        glAttachShader(s_rot_prog, vs2); glAttachShader(s_rot_prog, fs2);
        glLinkProgram(s_rot_prog);
        glDeleteShader(vs2); glDeleteShader(fs2);
        glGenVertexArrays(1, &s_rot_vao);
        glGenBuffers(1, &s_rot_vbo);
        glBindVertexArray(s_rot_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_rot_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
        glBindVertexArray(0);
    }

    GLint prev_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glUseProgram(s_rot_prog);
    glBindVertexArray(s_rot_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_rot_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_iv.tex);
    glUniform1i(glGetUniformLocation(s_rot_prog, "tex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(prev_prog);
}

// ============================================================================
// AUDIO VISUALIZER  (multi-mode, reused by plain-audio playback and by the
// KFN lyric display, which layers it underneath the karaoke text)
// ============================================================================

void iv_draw_visualizer(float viz_x, float viz_top, float viz_w, float viz_h, float alpha_mul) {
    const int NUM_BANDS = 128;
    const int NUM_VIS   = 4;
    double bands[NUM_BANDS];
    double t = g_iv.audio_position;
    float viz_bot = viz_top + viz_h;
    float viz_mid = viz_top + viz_h * 0.5f;

    // Sine-bank pseudo-spectrum (no FFT available here)
    for (int i = 0; i < NUM_BANDS; i++) {
        double freq  = 0.4 + i * 0.18;
        double phase = t * freq * 6.2832;
        double v = 0.5 + 0.5  * sin(phase + i * 0.4)
                       + 0.25 * sin(phase * 2.1 + i * 0.7)
                       + 0.10 * sin(phase * 3.3 + i * 1.1);
        bands[i] = v / 1.85;
    }

    // ── Helper: HSV→RGB ───────────────────────────────────────────
    auto hsv_rgb = [](float hue, float sat, float val,
                       float &r, float &g2, float &b) {
        int   hi = (int)(hue * 6.f) % 6;
        float f  = hue * 6.f - (int)(hue * 6.f);
        float p  = val * (1.f - sat);
        float q  = val * (1.f - f * sat);
        float tv = val * (1.f - (1.f - f) * sat);
        switch (hi) {
            case 0: r=val; g2=tv;  b=p;   break;
            case 1: r=q;   g2=val; b=p;   break;
            case 2: r=p;   g2=val; b=tv;  break;
            case 3: r=p;   g2=q;   b=val; break;
            case 4: r=tv;  g2=p;   b=val; break;
            default:r=val; g2=p;   b=q;   break;
        }
    };

    int mode = g_iv.vis_mode % NUM_VIS;

    if (mode == 0) {
        // ── 0: Symmetry Cascade ──────────────────────────────────
        float band_pw = viz_w / NUM_BANDS;
        float max_half = viz_h * 0.48f;
        for (int i = 0; i < NUM_BANDS; i++) {
            float amp = (float)bands[i];
            float h   = amp * max_half;
            float bx  = viz_x + i * band_pw;
            float hue = (float)i / NUM_BANDS;
            float r, g2, b;
            hsv_rgb(hue, fminf(1.f, amp*2.f), 1.f, r, g2, b);
            draw_rect(bx+1.f, viz_mid - h, band_pw-2.f, h,
                      r, g2, b, alpha_mul * 0.90f);
            draw_rect(bx+1.f, viz_mid,     band_pw-2.f, h,
                      r, g2, b, alpha_mul * 0.50f);
        }

    } else if (mode == 1) {
        // ── 1: Radial Burst ──────────────────────────────────────
        float cx2  = viz_x + viz_w * 0.5f;
        float cy2  = viz_top + viz_h * 0.5f;
        float maxR = fminf(viz_w, viz_h) * 0.46f;
        float innerR = maxR * 0.18f;
        // Rotating spoke angle driven by time
        float rot = (float)(fmod(t * 0.4, 6.2832));
        for (int i = 0; i < NUM_BANDS; i++) {
            float angle = rot + (float)i / NUM_BANDS * 6.2832f;
            float amp   = (float)bands[i];
            float outerR = innerR + amp * (maxR - innerR);
            float x1 = cx2 + cosf(angle) * innerR;
            float y1 = cy2 + sinf(angle) * innerR;
            float x2 = cx2 + cosf(angle) * outerR;
            float y2 = cy2 + sinf(angle) * outerR;
            float hue = fmodf((float)i / NUM_BANDS + (float)t * 0.05f, 1.f);
            float r, g2, b;
            hsv_rgb(hue, 1.f, 1.f, r, g2, b);
            // Draw as a thin rect along the spoke direction
            float dx2 = x2 - x1, dy2 = y2 - y1;
            float len = sqrtf(dx2*dx2 + dy2*dy2);
            if (len < 1.f) continue;
            float nx2 = -dy2/len * 2.f, ny2 = dx2/len * 2.f;
            // 4-corner quad — draw as two axis-rects (approximation)
            // Use screen-aligned rect at spoke position
            float spoke_w = fmaxf(2.f, viz_w / NUM_BANDS * 0.7f);
            float sx = x1 + (x2-x1)*0.5f - spoke_w*0.5f;
            float sy = fminf(y1, y2);
            float sw = spoke_w;
            float sh = fmaxf(2.f, fabsf(y2-y1));
            draw_rect(sx, sy, sw, sh, r, g2, b, alpha_mul * 0.85f);
            (void)nx2; (void)ny2;
        }
        // Centre pulse circle (simulated with a filled rect square)
        float pulse = 0.4f + 0.6f * (float)bands[0];
        float pr = innerR * pulse;
        draw_rect(cx2 - pr, cy2 - pr*0.5f, pr*2.f, pr,
                  1.f, 0.8f, 0.3f, alpha_mul * 0.6f);

    } else if (mode == 2) {
        // ── 2: Oscilloscope ──────────────────────────────────────
        // Draw grid
        draw_rect(viz_x, viz_mid - 0.5f, viz_w, 1.f,
                  0.25f, 0.25f, 0.35f, 0.5f);
        for (int gi = 1; gi < 4; gi++) {
            float gy = viz_top + viz_h * gi / 4.f;
            draw_rect(viz_x, gy, viz_w, 1.f, 0.20f, 0.20f, 0.28f, 0.4f);
        }
        // Draw waveform as a series of thin vertical rects connecting samples
        const int SCOPE_PTS = 128;
        float prev_y = viz_mid;
        for (int i = 0; i < SCOPE_PTS; i++) {
            float fi   = (float)i / (SCOPE_PTS - 1);
            // Build a sample from overlapping sine bands
            double phase = t * 6.2832 * (1.0 + fi * 4.0) + fi * 12.0;
            float sample = 0.f;
            for (int b = 0; b < 6; b++)
                sample += (float)(bands[b * 8] * sin(phase * (b+1) * 0.7 + b));
            sample /= 6.f;
            float sy2 = viz_mid - sample * viz_h * 0.44f;
            sy2 = fmaxf(viz_top, fminf(viz_bot, sy2));
            float sx2 = viz_x + fi * viz_w;
            float hue = fmodf(fi + (float)t * 0.08f, 1.f);
            float r, g2, b;
            hsv_rgb(hue, 0.7f, 1.f, r, g2, b);
            float seg_h = fabsf(sy2 - prev_y) + 2.f;
            float seg_y = fminf(sy2, prev_y);
            draw_rect(sx2, seg_y, fmaxf(2.f, viz_w / SCOPE_PTS + 1.f),
                      seg_h, r, g2, b, alpha_mul * 0.90f);
            prev_y = sy2;
        }

    } else {
        // ── 3: Starfield ─────────────────────────────────────────
        // Persistent star state
        static struct Star {
            float x, y, speed, size, hue;
            bool  alive;
        } stars[200] = {};
        static bool stars_init = false;
        if (!stars_init) {
            for (auto &s : stars) {
                s.x     = (float)rand() / RAND_MAX;
                s.y     = (float)rand() / RAND_MAX;
                s.speed = 0.01f + (float)rand()/RAND_MAX * 0.04f;
                s.size  = 1.f + (float)rand()/RAND_MAX * 3.f;
                s.hue   = (float)rand()/RAND_MAX;
                s.alive = true;
            }
            stars_init = true;
        }
        // Update: move stars outward from centre, reset when off-screen
        float beat = (float)(0.5 + 0.5 * sin(t * 4.0));  // pulse
        for (auto &s : stars) {
            float dx2 = s.x - 0.5f, dy2 = s.y - 0.5f;
            float dist = sqrtf(dx2*dx2 + dy2*dy2) + 0.001f;
            float speed = s.speed * (1.f + beat * (float)bands[(int)(s.hue*47)]);
            s.x += dx2 / dist * speed;
            s.y += dy2 / dist * speed;
            s.hue = fmodf(s.hue + 0.002f, 1.f);
            if (s.x < 0 || s.x > 1 || s.y < 0 || s.y > 1) {
                // Reset near centre
                s.x     = 0.5f + ((float)rand()/RAND_MAX - 0.5f) * 0.08f;
                s.y     = 0.5f + ((float)rand()/RAND_MAX - 0.5f) * 0.08f;
                s.speed = 0.005f + (float)rand()/RAND_MAX * 0.035f;
                s.size  = 1.f + (float)rand()/RAND_MAX * 4.f;
                s.hue   = (float)rand()/RAND_MAX;
            }
        }
        // Draw stars
        for (auto &s : stars) {
            float sx2 = viz_x + s.x * viz_w;
            float sy2 = viz_top + s.y * viz_h;
            float dx2 = s.x - 0.5f, dy2 = s.y - 0.5f;
            float dist = sqrtf(dx2*dx2 + dy2*dy2);
            float brightness = fminf(1.f, dist * 3.f);
            float r, g2, b;
            hsv_rgb(s.hue, 0.6f, brightness, r, g2, b);
            float sz = s.size * (0.5f + dist * 2.f);
            draw_rect(sx2 - sz*0.5f, sy2 - sz*0.5f, sz, sz,
                      r, g2, b, alpha_mul * brightness * 0.90f);
        }
    }
}

void iv_render(int win_w, int win_h) {
    if (!g_iv.visible) return;

    int rh = iv_row_h();

    // Check if zoomed in OR in fullscreen mode — hide UI elements for immersive viewing
    bool is_zoomed = g_iv.zoom > 1.0f;
    bool hide_ui = is_zoomed || g_iv.fullscreen;

    // ── Layout ─────────────────────────────────────────────────────────────
    // Left: file browser panel (~30% width, hidden when zoomed or fullscreen)
    // Right: image display area (~70% width)
    // Bottom: status bar (1 row, hidden when zoomed or fullscreen)
    int status_h = hide_ui ? 0 : (rh + IV_PAD);
    int panel_w  = hide_ui ? 0 : (int)(win_w * 0.28f);
    int panel_w_min = rh * 14;
    if (panel_w > 0 && panel_w < panel_w_min) panel_w = panel_w_min;
    if (panel_w > win_w / 2)   panel_w = win_w / 2;

    int title_h  = hide_ui ? 0 : (rh + IV_PAD);
    int content_y = title_h;
    int content_h = win_h - title_h - status_h;
    int img_x    = panel_w + (panel_w > 0 ? 2 : 0);
    int img_w    = win_w - img_x;

    // ── Background ─────────────────────────────────────────────────────────
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.06f, 0.06f, 0.08f, 1.f);

    // ── Title bar ──────────────────────────────────────────────────────────
    if (!hide_ui) {
        draw_rect(0, 0, (float)win_w, (float)title_h, 0.12f, 0.12f, 0.18f, 1.f);
        draw_rect(0, (float)(title_h-1), (float)win_w, 1, 0.25f, 0.35f, 0.60f, 1.f);
        const char *mode_str = g_iv.remote ? "Felix Chirp — Remote" : "Felix Chirp";
        char title[256];
#ifdef __DJGPP__
        // DOS: Chirp is a program of its own (CHIRP.EXE), not Felix
        // Terminal's F5; F2 changes the screen mode, Alt+letter the drive.
        snprintf(title, sizeof(title),
                 "%s   %dx%d (F2: mode)   ↑↓: navigate   ←→: prev/next   Shift+←→: ±5s   Shift+↑↓: ±30s   Enter/DblClick: open   Space: pause   R: rotate   0: reset zoom   Alt+letter: drive   Esc: quit",
                 mode_str, win_w, win_h);
#else
        snprintf(title, sizeof(title),
                 "%s (F5)   ↑↓: navigate   ←→: prev/next   Shift+←→: ±5s   Shift+↑↓: ±30s   Enter/DblClick: open   Space: pause   R: rotate   0: reset zoom   Esc: close",
                 mode_str);
#endif
        iv_draw_text(title, (float)IV_PAD, (float)title_h * 0.72f, 0.75f, 0.88f, 1.0f, 1.f);
    }

    // ── File browser panel ──────────────────────────────────────────────────
    if (!hide_ui) {
        float px = 0, py = (float)content_y, pw = (float)panel_w, ph = (float)content_h;

        // Panel background + border
        draw_rect(px, py, pw, ph, 0.09f, 0.09f, 0.12f, 1.f);
        draw_rect(px, py, pw, 1,  0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px, py+ph-1, pw, 1, 0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px, py, 1, ph, 0.35f, 0.55f, 0.95f, 1.f);
        draw_rect(px+pw-1, py, 1, ph, 0.35f, 0.55f, 0.95f, 1.f);

        // Path row — show zip name when inside a zip
        draw_rect(px, py, pw, (float)rh, 0.08f, 0.10f, 0.10f, 1.f);
        draw_rect(px, py+rh-1, pw, 1, 0.20f, 0.20f, 0.30f, 1.f);
        char path_disp[4100];
        if (g_iv.in_zip) {
            const char *zname = strrchr(g_iv.zip_file, '/');
            if (!zname) zname = strrchr(g_iv.zip_file, '\\');
            snprintf(path_disp, sizeof(path_disp), " [ZIP] %s", zname ? zname+1 : g_iv.zip_file);
        } else {
            // Truncate path from the left if it's too long for the panel
            int char_w  = g_font_size > 0 ? g_font_size : 16;
            int avail   = (int)(pw - 2 * IV_PAD);
            int max_ch  = avail / char_w;
            if (max_ch < 6) max_ch = 6;
            int plen    = (int)strlen(g_iv.path);
            if (plen + 1 <= max_ch) {
                snprintf(path_disp, sizeof(path_disp), " %s", g_iv.path);
            } else {
                // Show ellipsis + tail: "…/rest/of/path"
                const char *tail = g_iv.path + plen - (max_ch - 2);
                // Align to next '/' boundary
                const char *sl = strchr(tail, '/');
                if (!sl) sl = tail;
                snprintf(path_disp, sizeof(path_disp), " \xe2\x80\xa6%s", sl);
            }
        }
        iv_draw_text(path_disp, px + IV_PAD, py + rh * 0.72f, 0.45f, 0.80f, 0.45f, 1.f);

        float list_y = py + rh;
        int visible_rows = (content_h - rh) / rh;
        int n = (int)g_iv.entries.size();

        // Clamp scroll
        if (g_iv.selected < g_iv.scroll_top)
            g_iv.scroll_top = g_iv.selected;
        if (g_iv.selected >= g_iv.scroll_top + visible_rows)
            g_iv.scroll_top = g_iv.selected - visible_rows + 1;

        for (int i = 0; i < visible_rows; i++) {
            int idx = g_iv.scroll_top + i;
            if (idx >= n) break;
            const IVEntry &e = g_iv.entries[idx];
            float ry = list_y + i * rh;
            bool sel = (idx == g_iv.selected);

            if      (sel)    draw_rect(px+1, ry, pw-2, (float)rh, 0.18f, 0.38f, 0.75f, 0.90f);
            else if (i%2==0) draw_rect(px,   ry, pw,   (float)rh, 1.f,   1.f,   1.f,   0.02f);

            float nr, ng, nb;
            if (sel) {
                nr = ng = nb = 1.f;
            } else if (e.is_dir) {
                nr = 0.90f; ng = 0.90f; nb = 0.55f;
            } else if (e.is_zip && e.has_cdg_pair) {
                nr = 0.40f; ng = 1.00f; nb = 0.60f; // green — CDG zip
            } else if (e.is_zip) {
                nr = 0.95f; ng = 0.75f; nb = 0.30f; // orange — plain zip
            } else if (e.is_audio && e.has_cdg_pair) {
                nr = 0.40f; ng = 1.00f; nb = 0.60f; // green — has CDG
            } else if (e.is_audio) {
                nr = 0.50f; ng = 0.85f; nb = 1.00f; // cyan — audio only
            } else if (e.is_cdg) {
                nr = 0.80f; ng = 0.60f; nb = 1.00f; // purple — CDG graphics
            } else if (e.is_kfn) {
                nr = 1.00f; ng = 0.55f; nb = 0.80f; // pink — kfn package
            } else {
                nr = 0.82f; ng = 0.82f; nb = 0.92f;
            }

            // Highlight currently playing track
            bool is_playing_this = g_iv.audio_playing &&
                strcmp(e.name, g_iv.audio_label) == 0;
            if (is_playing_this && !sel)
                draw_rect(px+1, ry, pw-2, (float)rh, 0.10f, 0.30f, 0.15f, 0.60f);

            char name_buf[520];
            const char *prefix = e.is_dir  ? "[DIR] " :
                                 (e.is_zip && e.has_cdg_pair) ? "[CDG] " :
                                 e.is_zip  ? "[ZIP] " :
                                 e.is_audio ? (e.has_cdg_pair ? "[CDG] " : "[AUD] ") :
                                 e.is_cdg  ? "[.CDG]" :
                                 e.is_kfn  ? "[KFN] " :
                                             "      ";
            // Calculate how many characters fit in the panel (approx, monospace)
            // Reserve ~7 chars on right for the size column + padding
            int prefix_len = (int)strlen(prefix);
            int avail_px   = (int)(pw - 2 * IV_PAD - 64);  // 64px reserved for size
            int char_w     = g_font_size > 0 ? g_font_size : 16;
            int max_name_chars = avail_px / char_w;
            if (max_name_chars < 8) max_name_chars = 8;
            int max_for_name = max_name_chars - prefix_len;
            if (max_for_name < 4) max_for_name = 4;

            char trunc_name[520];
            iv_truncate_name(e.name, max_for_name, trunc_name, sizeof(trunc_name));
            snprintf(name_buf, sizeof(name_buf), "%s%s", prefix, trunc_name);
            iv_draw_text(name_buf, px + IV_PAD, ry + rh * 0.72f, nr, ng, nb, 1.f);

            if (!e.is_dir && e.size > 0) {
                char sz[32]; fmt_size_iv(e.size, sz, sizeof(sz));
                iv_draw_text(sz, px + pw - IV_PAD - 56, ry + rh * 0.72f,
                             0.45f, 0.55f, 0.65f, 1.f);
            }
        }

        // Scrollbar
        if (n > visible_rows) {
            float lh = (float)(visible_rows * rh);
            float th = lh * visible_rows / n;
            float ty2 = list_y + (lh - th) * g_iv.scroll_top / (float)(n - visible_rows);
            draw_rect(px+pw-5, list_y, 5, lh, 0.12f, 0.12f, 0.20f, 1.f);
            draw_rect(px+pw-5, ty2,    5, th, 0.35f, 0.50f, 0.90f, 1.f);
        }

        // Divider
        draw_rect((float)panel_w, (float)content_y, 2, (float)content_h, 0.20f, 0.20f, 0.30f, 1.f);
    }

    // ── Display area (image / CDG / audio) ─────────────────────────────────
    {
        float ix = (float)img_x, iy = (float)content_y;
        float iw = (float)img_w,  ih = (float)content_h;

        draw_rect(ix, iy, iw, ih, 0.04f, 0.04f, 0.06f, 1.f);

        if (g_iv.kfn_active && (g_iv.audio_playing || g_iv.audio_paused)) {
            // kfn lyric display — word-synced karaoke text instead of CDG graphics.
            iv_kfn_render(ix, iy, iw, ih);

        } else if (g_iv.cdg_display && (g_use_sdl_renderer ? (bool)g_iv.sdl_cdg_tex : (bool)g_iv.cdg_tex)) {
            // CD+G display — render palette texture directly
            float scale = std::min(iw / (float)CDG_WIDTH, ih / (float)CDG_HEIGHT);
            float dw = CDG_WIDTH * scale, dh = CDG_HEIGHT * scale;
            float dx = ix + (iw - dw) * 0.5f;
            float dy = iy + (ih - dh) * 0.5f;
            draw_rect(dx, dy, dw, dh, 0.f, 0.f, 0.f, 1.f);
            iv_draw_image_tex(g_iv.sdl_cdg_tex, g_iv.cdg_tex, dx, dy, dw, dh);

            // Playback progress bar at bottom of CDG area
            if (g_iv.audio_playing) {
                float bar_y  = dy + dh + 4.f;
                float bar_w  = dw;
                double total = Mix_MusicDuration(g_iv.music);
                float prog   = (total > 0) ? (float)(g_iv.audio_position / total) : 0.f;
                if (prog > 1.f) prog = 1.f;
                draw_rect(dx, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
                draw_rect(dx, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);
            }

        } else if (g_iv.audio_playing || g_iv.audio_paused) {
            // ── Audio-only display: multi-mode visualizer ─────────────────
            double t = g_iv.audio_position;
            float  alpha_mul = g_iv.audio_paused ? 0.35f : 1.0f;

            // ── Visualizer button (top-right of display area) ─────────────
            static const char *vis_names[] = {
                "Symmetry", "Radial", "Scope", "Starfield"
            };
            const int NUM_VIS = 4;
            float btn_w = 90.f, btn_h = (float)rh;
            float btn_x = ix + iw - btn_w - IV_PAD;
            float btn_y = iy + IV_PAD;
            draw_rect(btn_x, btn_y, btn_w, btn_h, 0.18f, 0.22f, 0.35f, 0.90f);
            draw_rect(btn_x, btn_y, btn_w, 1,     0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x, btn_y+btn_h-1, btn_w, 1, 0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x, btn_y, 1, btn_h,     0.35f, 0.55f, 0.95f, 1.f);
            draw_rect(btn_x+btn_w-1, btn_y, 1, btn_h, 0.35f, 0.55f, 0.95f, 1.f);
            char btn_label[64];
            snprintf(btn_label, sizeof(btn_label), "V: %s",
                     vis_names[g_iv.vis_mode % NUM_VIS]);
            iv_draw_text(btn_label,
                         btn_x + 6.f,
                         btn_y + btn_h * 0.72f,
                         0.75f, 0.88f, 1.00f, 1.f);

            // ── Shared progress / track UI at bottom ──────────────────────
            double total   = g_iv.music ? Mix_MusicDuration(g_iv.music) : 0.0;
            float  prog    = (total > 0) ? (float)(t / total) : 0.f;
            if (prog > 1.f) prog = 1.f;
            float bar_y  = iy + ih - (float)rh * 2.5f;
            float bar_x  = ix + iw * 0.05f;
            float bar_w  = iw * 0.90f;

            // Track label
            float lbl_y = iy + ih * 0.07f;
            iv_draw_text(g_iv.audio_label,
                         ix + iw*0.5f - strlen(g_iv.audio_label)*g_font_size*0.3f,
                         lbl_y, 0.90f, 0.95f, 1.00f, 1.f);
            if (g_iv.audio_paused) {
                const char *ps = "\xe2\x80\x96 PAUSED";
                iv_draw_text(ps,
                             ix + iw*0.5f - strlen(ps)*g_font_size*0.3f,
                             lbl_y + rh, 1.00f, 0.75f, 0.30f, 1.f);
            }

            // Progress bar
            draw_rect(bar_x, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
            if (prog > 0.f)
                draw_rect(bar_x, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);

            // Time
            int cur_s = (int)t, tot_s = (int)total;
            char timebuf[32];
            snprintf(timebuf, sizeof(timebuf), "%d:%02d / %d:%02d",
                     cur_s/60, cur_s%60, tot_s/60, tot_s%60);
            // (laid out by measured widths: on a narrow screen, as 640x480,
            // the volume on the left and the time on the right don't meet)
            float time_w = measure_text(timebuf, g_font_size);
            iv_draw_text(timebuf, bar_x + bar_w - time_w, bar_y + 14.f, 0.55f, 0.65f, 0.75f, 1.f);

            // Status indicators (volume, repeat)
            char status_indicators[256] = "";
            if (g_iv.repeat_mode == 1) {
                strcat(status_indicators, "🔁① ");
            } else if (g_iv.repeat_mode == 2) {
                strcat(status_indicators, "🔁 ");
            }
            // Volume display
            int vol_pct = (int)(g_iv.volume * 100.f);
            char vol_str[32];
            snprintf(vol_str, sizeof(vol_str), "Vol: %d%%", vol_pct);
            strcat(status_indicators, vol_str);
            
            if (strlen(status_indicators) > 0) {
                iv_draw_text(status_indicators, bar_x, bar_y + 14.f,
                             0.50f, 0.70f, 0.85f, 1.f);
            }

            // The keys, centred; shorter where the long line doesn't fit
            const char *ctrls[] = {
                "Space: pause/resume   S: stop   ←→: prev/next   V: next viz   DblClick: fullscreen   +/-: vol   .: repeat",
                "Space: pause   S: stop   ←→: prev/next   V: viz   +/-: vol",
                "Space: pause   S: stop   V: viz",
            };
            for (const char *ctrl : ctrls) {
                float cw = measure_text(ctrl, g_font_size);
                if (cw <= iw - 8.f) {
                    iv_draw_text(ctrl, ix + (iw - cw) * 0.5f, bar_y + (float)rh + 4.f,
                                 0.40f, 0.45f, 0.55f, 1.f);
                    break;
                }
            }

            // ── Visualizer drawing area (between label and progress bar) ──
            float viz_top = lbl_y + rh * 1.8f;
            float viz_bot = bar_y - (float)rh * 0.5f;
            float viz_h   = viz_bot - viz_top;
            float viz_w   = iw * 0.96f;
            float viz_x   = ix + (iw - viz_w) * 0.5f;

            iv_draw_visualizer(viz_x, viz_top, viz_w, viz_h, alpha_mul);

        } else if (g_iv.video_playing && g_iv.tex) {
            // ── Video display (like CD+G) ──────────────────────────────────
            float scale = std::min(iw / (float)g_iv.tex_w, ih / (float)g_iv.tex_h);
            float dw = g_iv.tex_w * scale, dh = g_iv.tex_h * scale;
            float dx = ix + (iw - dw) * 0.5f;
            float dy = iy + (ih - dh) * 0.5f;
            
            draw_rect(dx, dy, dw, dh, 0.f, 0.f, 0.f, 1.f);
            iv_draw_image_tex(g_iv.sdl_tex, g_iv.tex, dx, dy, dw, dh);
            
            // Playback progress bar at bottom of video area
            {
                float bar_y  = dy + dh + 4.f;
                float bar_w  = dw;
                float prog   = 0.f;  // TODO: get video duration/position from GStreamer
                if (prog > 1.f) prog = 1.f;
                
                // Show time
                int mins = (int)(g_iv.video_position / 60.0);
                int secs = (int)fmod(g_iv.video_position, 60.0);
                char time_str[32];
                snprintf(time_str, sizeof(time_str), "%d:%02d", mins, secs);
                
                iv_draw_text(time_str, dx + dw - IV_PAD - 60, bar_y - 18,
                             0.40f, 0.70f, 1.00f, 0.9f);
                
                // Progress bar
                draw_rect(dx, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
                if (prog > 0) draw_rect(dx, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);
            }

            // Rotation info
            if (g_iv.img_rot != 0) {
                char rot_buf[16];
                snprintf(rot_buf, sizeof(rot_buf), "%d°", g_iv.img_rot);
                iv_draw_text(rot_buf,
                             ix + IV_PAD,
                             iy + ih - rh,
                             0.80f, 0.85f, 1.00f, 0.75f);
            }

            // Progress bar for video
            double total = g_iv.video_duration > 0 ? g_iv.video_duration : 1.0;
            float prog = (float)(g_iv.video_position / total);
            if (prog > 1.f) prog = 1.f;
            if (prog < 0.f) prog = 0.f;
            
            float bar_y  = iy + ih - (float)rh * 2.5f;
            float bar_x  = ix + iw * 0.05f;
            float bar_w  = iw * 0.90f;

            draw_rect(bar_x, bar_y, bar_w, 6.f, 0.15f, 0.15f, 0.20f, 1.f);
            if (prog > 0.f)
                draw_rect(bar_x, bar_y, bar_w * prog, 6.f, 0.30f, 0.80f, 0.40f, 1.f);

            // Time display
            int cur_s = (int)g_iv.video_position;
            int tot_s = (int)total;
            char timebuf[32];
            snprintf(timebuf, sizeof(timebuf), "%d:%02d / %d:%02d",
                     cur_s/60, cur_s%60, tot_s/60, tot_s%60);
            // (laid out by measured widths: on a narrow screen, as 640x480,
            // the volume on the left and the time on the right don't meet)
            float time_w = measure_text(timebuf, g_font_size);
            iv_draw_text(timebuf, bar_x + bar_w - time_w, bar_y + 14.f, 0.55f, 0.65f, 0.75f, 1.f);

            // Video label
            iv_draw_text(g_iv.video_label,
                         ix + iw*0.5f - strlen(g_iv.video_label)*g_font_size*0.3f,
                         iy + IV_PAD + rh, 0.90f, 0.95f, 1.00f, 1.f);

            // Status
            const char *status = g_iv.video_paused ? "█ PAUSED" : "▶ PLAYING";
            iv_draw_text(status,
                         ix + iw*0.5f - strlen(status)*g_font_size*0.3f,
                         iy + IV_PAD + rh*2.5f, 1.00f, 0.85f, 0.30f, 1.f);

        } else if (s_viewing_text) {
            // ── Text/Markdown display ──────────────────────────────────────
            iv_render_text_document(s_text_doc, (int)ix, (int)iy, (int)iw, (int)ih);
            
        } else if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            // ── Image display with zoom / pan / rotation ──────────────────
            // tw/th = texture dimensions in the rotated orientation (for fit calc)
            float tw, th;
            if (g_iv.img_rot == 0 || g_iv.img_rot == 180) {
                // No rotation or 180°: dimensions stay the same
                tw = (float)g_iv.tex_w;
                th = (float)g_iv.tex_h;
            } else {
                // 90° or 270°: dimensions swap
                tw = (float)g_iv.tex_h;
                th = (float)g_iv.tex_w;
            }
            float base_scale = std::min(iw / tw, ih / th);
            float effective  = base_scale * g_iv.zoom;

            // dw/dh = display size of the rotated image footprint
            float dw = tw * effective;
            float dh = th * effective;

            // Centre + pan offset
            float cx  = ix + iw * 0.5f;
            float cy2 = iy + ih * 0.5f;
            float dx  = cx - dw * 0.5f + g_iv.pan_x;
            float dy  = cy2 - dh * 0.5f + g_iv.pan_y;
            float img_cx = dx + dw * 0.5f;
            float img_cy = dy + dh * 0.5f;

            // Background rect fits the rotated footprint
            draw_rect(dx, dy, dw, dh, 0.25f, 0.25f, 0.25f, 1.f);

            // Draw with original (unswapped) tex dimensions — the rotation
            // function rotates the quad, so we pass raw tex size * scale.
            float raw_w = (float)g_iv.tex_w * effective;
            float raw_h = (float)g_iv.tex_h * effective;
            iv_draw_image_rotated(img_cx, img_cy, raw_w, raw_h, g_iv.img_rot);

            // Zoom level hint (fade out quickly — only show when != 1.0)
            if (g_iv.zoom != 1.0f) {
                char zoom_buf[32];
                snprintf(zoom_buf, sizeof(zoom_buf), "%.0f%%", g_iv.zoom * 100.f);
                iv_draw_text(zoom_buf,
                             ix + iw - strlen(zoom_buf)*g_font_size*0.6f - IV_PAD,
                             iy + ih - rh,
                             0.80f, 0.85f, 1.00f, 0.75f);
            }
            if (g_iv.img_rot != 0) {
                char rot_buf[16];
                snprintf(rot_buf, sizeof(rot_buf), "%d\xc2\xb0", g_iv.img_rot);
                iv_draw_text(rot_buf,
                             ix + IV_PAD,
                             iy + ih - rh,
                             0.80f, 0.85f, 1.00f, 0.75f);
            }
            
            // Slideshow indicator
            if (g_iv.slideshow_active) {
                const char *slide = "Slideshow";
                iv_draw_text(slide,
                             ix + iw*0.5f - strlen(slide)*g_font_size*0.3f,
                             iy + IV_PAD,
                             0.40f, 0.85f, 1.00f, 0.8f);
            }

        } else if (g_iv.error[0]) {
            iv_draw_text(g_iv.error,
                         ix + iw*0.5f - strlen(g_iv.error)*g_font_size*0.3f,
                         iy + ih*0.5f, 1.f, 0.4f, 0.4f, 1.f);
        } else {
            const char *hint = "Select an image, audio, or CD+G file";
            iv_draw_text(hint,
                         ix + iw*0.5f - strlen(hint)*g_font_size*0.3f,
                         iy + ih*0.5f, 0.35f, 0.40f, 0.50f, 1.f);
        }
    }

    // ── Status bar ─────────────────────────────────────────────────────────
    if (!hide_ui) {
        float st_y = (float)(win_h - status_h);
        draw_rect(0, st_y, (float)win_w, 1, 0.20f, 0.20f, 0.30f, 1.f);
        draw_rect(0, st_y+1, (float)win_w, (float)status_h-1, 0.09f, 0.09f, 0.13f, 1.f);

        char status[512];
        if (g_iv.kfn_active && (g_iv.audio_playing || g_iv.audio_paused)) {
            snprintf(status, sizeof(status), "  \xe2\x99\xab %s \xe2\x80\x94 %s   %.1fs%s",
                     g_iv.kfn_artist.c_str(), g_iv.kfn_title.c_str(), g_iv.audio_position,
                     g_iv.audio_paused ? "  [PAUSED]" : "");
        } else if (g_iv.cdg_display != nullptr) {
            int pkt = g_iv.cdg_display->current_packet, tot = g_iv.cdg_display->packet_count;
            snprintf(status, sizeof(status), "  \xe2\x99\xab %s   CD+G: packet %d/%d  (%.1fs)",
                     g_iv.audio_label, pkt, tot, g_iv.audio_position);
        } else if (g_iv.audio_playing || g_iv.audio_paused) {
            snprintf(status, sizeof(status), "  \xe2\x99\xab %s   %.1fs%s",
                     g_iv.audio_label, g_iv.audio_position,
                     g_iv.audio_paused ? "  [PAUSED]" : "");
        } else if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            snprintf(status, sizeof(status), "  %s   %dx%d px",
                     g_iv.img_label, g_iv.tex_w, g_iv.tex_h);
        } else {
            int n = (int)g_iv.entries.size();
            int imgs = 0, audio = 0, cdgs = 0, kfns = 0;
            for (auto &e : g_iv.entries) {
                if (!e.is_dir && !e.is_zip) {
                    if (e.is_audio) audio++;
                    else if (e.is_cdg) cdgs++;
                    else if (e.is_kfn) kfns++;
                    else imgs++;
                }
            }
            snprintf(status, sizeof(status), "  %d items  (%d images, %d audio, %d cdg, %d kfn)",
                     n, imgs, audio, cdgs, kfns);
        }
        iv_draw_text(status, (float)IV_PAD, st_y + status_h * 0.72f,
                     0.60f, 0.70f, 0.85f, 1.f);
    }

    gl_flush_verts();
}
