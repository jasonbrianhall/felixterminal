#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>

#include <cstdint>
#include "felixchirp.h"
#include "../gl_renderer.h"
#include "../sdl_renderer.h"
#include "../ft_font.h"
#include "../term_color.h"

extern bool s_viewing_text;
extern TextDocument s_text_doc;
static const int IV_PAD = 8;
// ============================================================================
// INPUT
// ============================================================================

bool iv_keydown(SDL_Keycode sym) {
    if (!g_iv.visible) return false;

    // Handle text document navigation
    if (s_viewing_text) {
        // Left/Right arrows: navigate to prev/next file (like with images/audio)
        if (sym == SDLK_LEFT) {
            if (g_iv.selected > 0) {
                g_iv.selected--;
                iv_enter_selected();
            }
            return true;
        }
        if (sym == SDLK_RIGHT) {
            int n = (int)g_iv.entries.size();
            if (g_iv.selected < n - 1) {
                g_iv.selected++;
                iv_enter_selected();
            }
            return true;
        }
        
        iv_text_keyboard(s_text_doc, sym);
        // Also handle close/navigate keys
#ifdef __DJGPP__
        if (sym == SDLK_ESCAPE) {        // (DOS: no F5; Chirp isn't an overlay there)
#else
        if (sym == SDLK_ESCAPE || sym == SDLK_F5) {
#endif
            s_viewing_text = false;
            return true;
        }
        return true;
    }

    int n = (int)g_iv.entries.size();

    switch (sym) {
    case SDLK_ESCAPE:
#ifndef __DJGPP__
    case SDLK_F5:
#endif
        // Exit fullscreen first if in fullscreen mode, otherwise close viewer
        if (g_iv.fullscreen) {
            g_iv.fullscreen = false;
            return true;
        }
        iv_close();
        return true;

    case SDLK_PAGEUP:
        g_iv.selected = std::max(0, g_iv.selected - 10);
        return true;

    case SDLK_PAGEDOWN:
        g_iv.selected = std::min(n - 1, g_iv.selected + 10);
        return true;

    case SDLK_HOME:
        g_iv.selected = 0;
        return true;

    case SDLK_END:
        g_iv.selected = n > 0 ? n - 1 : 0;
        return true;

    case SDLK_RETURN: case SDLK_KP_ENTER:
        iv_enter_selected();
        return true;

    case SDLK_SPACE:
        // If audio/video is playing/paused, toggle pause; otherwise open selected
        if (g_iv.audio_playing) {
            if (g_iv.music)                       Mix_PauseMusic();
            else if (g_iv.chunk_channel >= 0)     Mix_Pause(g_iv.chunk_channel);
            if (g_iv.kfn_channel_backing >= 0)     Mix_Pause(g_iv.kfn_channel_backing);
            g_iv.audio_paused  = true;
            g_iv.audio_playing = false;
        } else if (g_iv.audio_paused) {
            if (g_iv.music)                       Mix_ResumeMusic();
            else if (g_iv.chunk_channel >= 0)     Mix_Resume(g_iv.chunk_channel);
            if (g_iv.kfn_channel_backing >= 0)     Mix_Resume(g_iv.kfn_channel_backing);
            g_iv.audio_playing = true;
            g_iv.audio_paused  = false;
            // Adjust start_ticks to account for paused time
            g_iv.audio_start_ticks = (double)SDL_GetTicks() - g_iv.audio_position * 1000.0;
        }
#ifdef HAVE_GSTREAMER
        else if (g_iv.video_playing && g_iv.video_pipeline) {
            // Toggle video pause
            if (!g_iv.video_paused) {
                gst_element_set_state(g_iv.video_pipeline, GST_STATE_PAUSED);
                g_iv.video_paused = true;
                g_iv.video_playing = false;
            } else {
                gst_element_set_state(g_iv.video_pipeline, GST_STATE_PLAYING);
                g_iv.video_paused = false;
                g_iv.video_playing = true;
                g_iv.video_start_ticks = (double)SDL_GetTicks() - g_iv.video_position * 1000.0;
            }
        }
#endif
        else {
            iv_enter_selected();
        }
        return true;

    case SDLK_s:
        if (g_iv.audio_playing || g_iv.audio_paused) {
            if (g_iv.kfn_active) iv_kfn_stop();
            iv_stop_audio();
            iv_cdg_free();
            return true;
        }
        if (g_iv.video_playing || g_iv.video_paused) {
            iv_video_stop();
            return true;
        }
        // No audio/video active — fall through to first-letter jump
        goto first_letter_jump;

    case SDLK_r: {
        // Rotate image/video 90° clockwise
        bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
        bool has_video = (bool)g_iv.video_tex;
        if (has_image || has_video) {
            // Cycle through 0 -> 90 -> 180 -> 270 -> 0
            switch (g_iv.img_rot) {
                case 0:   g_iv.img_rot = 90;  break;
                case 90:  g_iv.img_rot = 180; break;
                case 180: g_iv.img_rot = 270; break;
                case 270: g_iv.img_rot = 0;   break;
                default:  g_iv.img_rot = 0;   break;  // Reset any invalid state
            }
            // Swap pan axes to keep image centred after rotation
            float tmp = g_iv.pan_x;
            g_iv.pan_x = -g_iv.pan_y;
            g_iv.pan_y = tmp;
            return true;
        }
        goto first_letter_jump;
    }

    case SDLK_0: case SDLK_KP_0:
        // Reset zoom and pan
        if (g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex) {
            g_iv.zoom  = 1.0f;
            g_iv.pan_x = 0.0f;
            g_iv.pan_y = 0.0f;
            return true;
        }
        return true;

    case SDLK_v:
        // Cycle visualizer mode (only when audio is playing/paused, else letter-jump)
        if (g_iv.audio_playing || g_iv.audio_paused) {
            g_iv.vis_mode = (g_iv.vis_mode + 1) % 4;
            return true;
        }
        goto first_letter_jump;

    case SDLK_EQUALS: case SDLK_PLUS:
        // Increase volume
        g_iv.volume += 0.05f;
        if (g_iv.volume > 1.0f) g_iv.volume = 1.0f;
        if (g_iv.music && !g_iv.kfn_vocal_muted) Mix_VolumeMusic((int)(g_iv.volume * 128.0f));
        if (g_iv.chunk_channel >= 0 && !g_iv.kfn_vocal_muted) Mix_Volume(g_iv.chunk_channel, (int)(g_iv.volume * 128.0f));
        if (g_iv.kfn_channel_backing >= 0 && !g_iv.kfn_backing_muted) Mix_Volume(g_iv.kfn_channel_backing, (int)(g_iv.volume * 128.0f));
        return true;

    case SDLK_MINUS:
        // Decrease volume
        g_iv.volume -= 0.05f;
        if (g_iv.volume < 0.0f) g_iv.volume = 0.0f;
        if (g_iv.music && !g_iv.kfn_vocal_muted) Mix_VolumeMusic((int)(g_iv.volume * 128.0f));
        if (g_iv.chunk_channel >= 0 && !g_iv.kfn_vocal_muted) Mix_Volume(g_iv.chunk_channel, (int)(g_iv.volume * 128.0f));
        if (g_iv.kfn_channel_backing >= 0 && !g_iv.kfn_backing_muted) Mix_Volume(g_iv.kfn_channel_backing, (int)(g_iv.volume * 128.0f));
        return true;

    case SDLK_k:
        // Mute/unmute the vocal (primary) track — kfn songs only
        if (g_iv.kfn_active) {
            iv_kfn_toggle_vocal_mute();
            return true;
        }
        goto first_letter_jump;

    case SDLK_b:
        // Mute/unmute the backing track — kfn songs only
        if (g_iv.kfn_active && g_iv.kfn_channel_backing >= 0) {
            iv_kfn_toggle_backing_mute();
            return true;
        }
        goto first_letter_jump;

    case SDLK_PERIOD:
        // Cycle repeat mode: OFF -> ONE -> ALL -> OFF
        if (g_iv.audio_playing || g_iv.audio_paused) {
            g_iv.repeat_mode = (g_iv.repeat_mode + 1) % 3;
            return true;
        }
        goto first_letter_jump;

    case SDLK_l:
        // Toggle slideshow mode (only for images)
        if (!(g_iv.audio_playing || g_iv.audio_paused)) {
            g_iv.slideshow_active = !g_iv.slideshow_active;
            if (g_iv.slideshow_active) {
                g_iv.slideshow_start = 0.0;  // Reset timer
            }
            return true;
        }
        goto first_letter_jump;

    case SDLK_LEFT:
    case SDLK_RIGHT: {
        SDL_Keymod mod = SDL_GetModState();
        // Shift+Left/Right = seek ±5 seconds when audio/video is playing
        if ((mod & KMOD_SHIFT) && (g_iv.audio_playing || g_iv.audio_paused)) {
            double delta = (sym == SDLK_RIGHT) ? 5.0 : -5.0;
            double newpos = g_iv.audio_position + delta;
            if (newpos < 0.0) newpos = 0.0;
            if (Mix_SetMusicPosition(newpos) == 0) {
                g_iv.audio_position    = newpos;
                g_iv.audio_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
                // CDG must replay from start to catch up to new position
                if (g_iv.cdg_display != nullptr) {
                    cdg_reset(g_iv.cdg_display);
                }
            }
            return true;
        }
        if ((mod & KMOD_SHIFT) && (g_iv.video_playing || g_iv.video_paused)) {
            double delta = (sym == SDLK_RIGHT) ? 5.0 : -5.0;
            double newpos = g_iv.video_position + delta;
            if (newpos < 0.0) newpos = 0.0;
#ifdef HAVE_GSTREAMER
            if (g_iv.video_pipeline) {
                gst_element_seek_simple(g_iv.video_pipeline, GST_FORMAT_TIME,
                    GST_SEEK_FLAG_FLUSH, (gint64)(newpos * GST_SECOND));
                g_iv.video_position = newpos;
                g_iv.video_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
            }
#endif
            return true;
        }
        // No shift — navigate to prev/next playable file
        std::vector<int> playable;
        for (int i = 0; i < n; i++) {
            const IVEntry &ei = g_iv.entries[i];
            if (ei.is_dir) continue;
            if (ei.is_cdg && !ei.is_zip) continue;  // skip bare .cdg sidecars
            if (ei.is_zip && !ei.has_cdg_pair) continue;  // skip non-CDG zips in nav
            playable.push_back(i);
        }
        if (playable.empty()) return true;

        int pos = -1;
        for (int i = 0; i < (int)playable.size(); i++)
            if (playable[i] == g_iv.selected) { pos = i; break; }

        if (pos < 0) pos = (sym == SDLK_RIGHT) ? 0 : (int)playable.size() - 1;
        else if (sym == SDLK_RIGHT)
            pos = (pos + 1) % (int)playable.size();
        else
            pos = (pos - 1 + (int)playable.size()) % (int)playable.size();

        g_iv.selected = playable[pos];
        iv_enter_selected();
        return true;
    }

    case SDLK_UP:
    case SDLK_DOWN: {
        SDL_Keymod mod = SDL_GetModState();
        // Shift+Up/Down = seek ±30 seconds when audio/video is playing
        if ((mod & KMOD_SHIFT) && (g_iv.audio_playing || g_iv.audio_paused)) {
            double delta = (sym == SDLK_UP) ? 30.0 : -30.0;
            double newpos = g_iv.audio_position + delta;
            if (newpos < 0.0) newpos = 0.0;
            if (Mix_SetMusicPosition(newpos) == 0) {
                g_iv.audio_position    = newpos;
                g_iv.audio_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
                if (g_iv.cdg_display != nullptr) {
                    cdg_reset(g_iv.cdg_display);
                }
            }
            return true;
        }
        if ((mod & KMOD_SHIFT) && (g_iv.video_playing || g_iv.video_paused)) {
            double delta = (sym == SDLK_UP) ? 30.0 : -30.0;
            double newpos = g_iv.video_position + delta;
            if (newpos < 0.0) newpos = 0.0;
#ifdef HAVE_GSTREAMER
            if (g_iv.video_pipeline) {
                gst_element_seek_simple(g_iv.video_pipeline, GST_FORMAT_TIME,
                    GST_SEEK_FLAG_FLUSH, (gint64)(newpos * GST_SECOND));
                g_iv.video_position = newpos;
                g_iv.video_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
            }
#endif
            return true;
        }
        // No shift — normal list navigation with wrap-around
        if (sym == SDLK_UP)
            g_iv.selected = (g_iv.selected > 0) ? g_iv.selected - 1 : n - 1;
        else
            g_iv.selected = (g_iv.selected < n - 1) ? g_iv.selected + 1 : 0;
        return true;
    }

    case SDLK_BACKSPACE:
        if (g_iv.audio_playing || g_iv.audio_paused) {
            iv_stop_audio(); iv_cdg_free();
        } else if (g_iv.video_playing || g_iv.video_paused) {
            iv_video_stop();
        } else if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (strcmp(g_iv.entries[i].name, "..") == 0) {
                    g_iv.selected = i;
                    iv_enter_selected();
                    break;
                }
            }
        }
        return true;

    default: {
        first_letter_jump:
        // First-letter jump: press a letter/digit to jump to the first entry
        // whose name starts with that character (case-insensitive). Pressing
        // the same key again cycles through all matches, wrapping around.
        if (sym < 32 || sym > 126) return true;
        char ch = (char)tolower((unsigned char)sym);
        if (!isalpha((unsigned char)ch) && !isdigit((unsigned char)ch)) return true;

        std::vector<int> matches;
        for (int i = 0; i < n; i++) {
            const char *nm = g_iv.entries[i].name;
            if (strcmp(nm, "..") == 0) continue;
            if (tolower((unsigned char)nm[0]) == (unsigned char)ch)
                matches.push_back(i);
        }
        if (matches.empty()) return true;

        // If already on a match advance to next, otherwise jump to first.
        int next = matches[0];
        for (int i = 0; i < (int)matches.size(); i++) {
            if (matches[i] == g_iv.selected) {
                next = matches[(i + 1) % (int)matches.size()];
                break;
            }
        }
        g_iv.selected = next;
        return true;
    }
    } // switch
}

// ============================================================================
// MOUSE SUPPORT — zoom, pan, file list scroll, click to select
// ============================================================================

// Returns the image display rect (ix, iy, iw, ih) for the current window size.
static void iv_image_rect(int win_w, int win_h,
                          float &ix, float &iy, float &iw, float &ih) {
    int rh       = iv_row_h();
    bool is_zoomed = g_iv.zoom > 1.0f;
    bool hide_ui = is_zoomed || g_iv.fullscreen;
    int status_h = hide_ui ? 0 : (rh + IV_PAD);
    int title_h  = hide_ui ? 0 : (rh + IV_PAD);
    int panel_w  = hide_ui ? 0 : (int)(win_w * 0.28f);
    int panel_w_min = rh * 14;
    if (panel_w > 0 && panel_w < panel_w_min) panel_w = panel_w_min;
    if (panel_w > win_w / 2)   panel_w = win_w / 2;
    ix = (float)(panel_w + (panel_w > 0 ? 2 : 0));
    iy = (float)title_h;
    iw = (float)(win_w - ix);
    ih = (float)(win_h - title_h - status_h);
}

// Returns the panel width for hit-testing the file list.
static int iv_panel_width(int win_w) {
    int rh = iv_row_h();
    bool is_zoomed = g_iv.zoom > 1.0f;
    bool hide_ui = is_zoomed || g_iv.fullscreen;
    if (hide_ui) return 0;  // No panel when zoomed or fullscreen
    int pw = (int)(win_w * 0.28f);
    int mn = rh * 14;
    if (pw < mn)        pw = mn;
    if (pw > win_w / 2) pw = win_w / 2;
    return pw;
}

bool iv_mousewheel(int x, int y, int delta_y, int win_w, int win_h) {
    if (!g_iv.visible) return false;

    // Handle text document scroll
    if (s_viewing_text) {
        iv_text_scroll(s_text_doc, delta_y);
        return true;
    }

    float ix, iy, iw, ih;
    iv_image_rect(win_w, win_h, ix, iy, iw, ih);

    int panel_w = iv_panel_width(win_w);

    if (x < panel_w) {
        // Scroll the file list — move both scroll_top and selected together
        // so the render clamp (which keeps selected visible) doesn't fight us.
        int rh2 = iv_row_h();
        int title_h2  = rh2 + IV_PAD;
        int status_h2 = rh2 + IV_PAD;
        int content_h2 = win_h - title_h2 - status_h2;
        int visible_rows = (content_h2 - rh2) / rh2;
        int n = (int)g_iv.entries.size();
        int step = (delta_y > 0) ? -3 : 3;  // wheel up = step toward top (lower index)
        g_iv.scroll_top = std::max(0, std::min(g_iv.scroll_top + step, std::max(0, n - visible_rows)));
        // Keep selected within the visible window
        if (g_iv.selected < g_iv.scroll_top)
            g_iv.selected = g_iv.scroll_top;
        if (g_iv.selected >= g_iv.scroll_top + visible_rows)
            g_iv.selected = g_iv.scroll_top + visible_rows - 1;
        g_iv.selected = std::max(0, std::min(g_iv.selected, n - 1));
        return true;
    }

    // Zoom the image around the cursor position
    bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
    if (!has_image) return true;

    float old_zoom = g_iv.zoom;
    float factor   = (delta_y > 0) ? 1.15f : (1.0f / 1.15f);
    g_iv.zoom = std::max(0.1f, std::min(g_iv.zoom * factor, 16.0f));

    // Zoom towards cursor: adjust pan so the point under cursor stays fixed.
    float cx = ix + iw * 0.5f;
    float cy = iy + ih * 0.5f;
    float mouse_rel_x = (float)x - cx;
    float mouse_rel_y = (float)y - cy;
    float zoom_ratio  = g_iv.zoom / old_zoom;
    g_iv.pan_x = mouse_rel_x + (g_iv.pan_x - mouse_rel_x) * zoom_ratio;
    g_iv.pan_y = mouse_rel_y + (g_iv.pan_y - mouse_rel_y) * zoom_ratio;

    return true;
}

bool iv_mousedown(int x, int y, int button, int win_w, int win_h) {
    if (!g_iv.visible) return false;

    int rh      = iv_row_h();
    int title_h = rh + IV_PAD;
    int panel_w = iv_panel_width(win_w);

    if (button == SDL_BUTTON_LEFT) {
        if (x < panel_w && y >= title_h) {
            // Click in file list: select entry, double-click opens it
            int list_y    = title_h + rh;  // path row inside panel
            int status_h  = rh + IV_PAD;
            int content_h = win_h - title_h - status_h;
            int visible_rows = (content_h - rh) / rh;
            int row = (y - list_y) / rh;
            int idx = g_iv.scroll_top + row;
            int n   = (int)g_iv.entries.size();
            if (row >= 0 && idx < n) {
                static uint32_t s_last_click_time = 0;
                static int      s_last_click_idx  = -1;
                uint32_t now = SDL_GetTicks();
                if (idx == s_last_click_idx && (now - s_last_click_time) < 400) {
                    // Double-click
                    g_iv.selected = idx;
                    iv_enter_selected();
                } else {
                    g_iv.selected = idx;
                }
                s_last_click_time = now;
                s_last_click_idx  = idx;
            }
            return true;
        }

        // Check visualizer button (top-right of image area) when audio is playing
        if (x >= panel_w && (g_iv.audio_playing || g_iv.audio_paused)) {
            float ix2, iy2, iw2, ih2;
            iv_image_rect(win_w, win_h, ix2, iy2, iw2, ih2);
            float btn_w = 90.f, btn_h = (float)rh;
            float btn_x = ix2 + iw2 - btn_w - IV_PAD;
            float btn_y = iy2 + IV_PAD;
            if ((float)x >= btn_x && (float)x <= btn_x + btn_w &&
                (float)y >= btn_y && (float)y <= btn_y + btn_h) {
                g_iv.vis_mode = (g_iv.vis_mode + 1) % 4;
                return true;
            }
        }

        // Check progress bar click for audio seek
        if (x >= panel_w && (g_iv.audio_playing || g_iv.audio_paused)) {
            float ix2, iy2, iw2, ih2;
            iv_image_rect(win_w, win_h, ix2, iy2, iw2, ih2);
            int rh2 = iv_row_h();
            float bar_y  = iy2 + ih2 - (float)rh2 * 2.5f;
            float bar_x  = ix2 + iw2 * 0.05f;
            float bar_w  = iw2 * 0.90f;
            float bar_h  = 6.f;
            
            // Check if click is within progress bar bounds (expand clickable area slightly)
            if ((float)y >= bar_y - 4.f && (float)y <= bar_y + bar_h + 4.f &&
                (float)x >= bar_x && (float)x <= bar_x + bar_w) {
                // Calculate seek position based on click position
                double total = g_iv.music ? Mix_MusicDuration(g_iv.music) : 0.0;
                if (total > 0.0) {
                    float click_ratio = ((float)x - bar_x) / bar_w;
                    if (click_ratio < 0.f) click_ratio = 0.f;
                    if (click_ratio > 1.f) click_ratio = 1.f;
                    double newpos = total * click_ratio;
                    
                    if (Mix_SetMusicPosition(newpos) == 0) {
                        g_iv.audio_position    = newpos;
                        g_iv.audio_start_ticks = (double)SDL_GetTicks() - newpos * 1000.0;
                        // CDG must replay from start to catch up to new position
                        if (g_iv.cdg_display != nullptr) {
                            cdg_reset(g_iv.cdg_display);
                        }
                    }
                }
                return true;
            }
        }

        // Double-click in image/audio/CDG area to toggle fullscreen
        bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
        if ((has_image || g_iv.audio_playing || g_iv.audio_paused) && x >= panel_w) {
            static uint32_t s_last_dbl_click_time = 0;
            static int      s_last_dbl_click_x = -1;
            static int      s_last_dbl_click_y = -1;
            uint32_t now = SDL_GetTicks();
            
            // Check if this is a double-click (same area, within 400ms)
            if ((now - s_last_dbl_click_time) < 400 &&
                abs(x - s_last_dbl_click_x) < 20 &&
                abs(y - s_last_dbl_click_y) < 20) {
                // This is a double-click — toggle fullscreen
                g_iv.fullscreen = !g_iv.fullscreen;
                s_last_dbl_click_time = 0;  // Reset to avoid triple-click
                return true;
            }
            
            // Record this click for next double-click check
            s_last_dbl_click_time = now;
            s_last_dbl_click_x = x;
            s_last_dbl_click_y = y;
        }

        // Start pan drag in image area
        if (has_image && x >= panel_w) {
            g_iv.drag_active  = true;
            g_iv.drag_start_x = x;
            g_iv.drag_start_y = y;
            g_iv.drag_pan_x0  = g_iv.pan_x;
            g_iv.drag_pan_y0  = g_iv.pan_y;
            return true;
        }
    }

    if (button == SDL_BUTTON_RIGHT) {
        // Right-click in image area resets zoom and pan
        bool has_image = g_use_sdl_renderer ? (bool)g_iv.sdl_tex : (bool)g_iv.tex;
        if (has_image && x >= panel_w) {
            g_iv.zoom  = 1.0f;
            g_iv.pan_x = 0.0f;
            g_iv.pan_y = 0.0f;
            return true;
        }
    }

    return true;
}

bool iv_mousemotion(int x, int y, int /*win_w*/, int /*win_h*/) {
    if (!g_iv.visible) return false;
    if (g_iv.drag_active) {
        g_iv.pan_x = g_iv.drag_pan_x0 + (float)(x - g_iv.drag_start_x);
        g_iv.pan_y = g_iv.drag_pan_y0 + (float)(y - g_iv.drag_start_y);
        return true;
    }
    return false;
}

bool iv_mouseup(int /*x*/, int /*y*/, int button) {
    if (!g_iv.visible) return false;
    if (button == SDL_BUTTON_LEFT) {
        g_iv.drag_active = false;
    }
    return true;
}
