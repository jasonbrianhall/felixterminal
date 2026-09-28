#pragma once
#include "terminal.h"
#include <stdint.h>
#include <string>
#include <vector>

// ============================================================================
// KITTY GRAPHICS PROTOCOL
// Spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/
// ============================================================================

// Call once after gl_init_renderer().
void kitty_init(void);

// Feed a complete APC payload (everything between ESC_ and ESC\, not
// including the delimiters themselves).  Called from term_feed().
void kitty_handle_apc(Terminal *t, const char *payload, int len);

// Render all placed images for this terminal into the current FBO.
// Call from term_render() after the glyph pass, before gl_flush_verts().
// clip_rows: draw only within the first N rows (0 = all rows). The sticky
// prompt passes rows-1 so images never cover its fixed input line.
void kitty_render(Terminal *t, int ox, int oy, int clip_rows = 0);

// Advance animation timers by dt seconds.
// Returns true if any animated image changed frame (caller should set needs_render).
// Call once per main-loop iteration regardless of needs_render.
bool kitty_tick(double dt);

// For HTML copy: returns <img> tags for any placements whose y_cell falls
// within [row_start, row_end] (inclusive, virtual row coordinates matching
// the selection). Each entry is keyed by y_cell so the caller can insert
// them at the right line break. Call once per copy operation.
struct KittyHtmlImage {
    int         y_cell;
    int         cols;
    std::string img_tag;
};
std::vector<KittyHtmlImage> kitty_get_html_images(Terminal *t, int row_start, int row_end);

// For rich clipboard copy: the same placements as kitty_get_html_images, but
// as raw PNG bytes plus the on-screen display size, so the caller can choose
// how to embed them (data: URI, temp file, native image clipboard format).
struct KittyPngImage {
    int                  vrow;        // virtual row of the image's top edge
    int                  rows_used;   // terminal rows the image covers
    int                  cols;        // placement c= (0 = natural size)
    int                  disp_w_px;   // on-screen display size in pixels
    int                  disp_h_px;
    std::vector<uint8_t> png;
};
std::vector<KittyPngImage> kitty_get_png_images(Terminal *t, int row_start, int row_end);

// Encode tightly-or-strided RGBA8 pixels to PNG (shared with sixel_graphics).
bool kitty_encode_png(const uint8_t *rgba, int w, int h, int stride, std::vector<uint8_t> &out);

// Drop all images associated with this terminal (e.g. on reset / alt-screen swap).
void kitty_clear(Terminal *t);

// Remove placements on the visible part of the current screen (ED 2 / clear)
void kitty_erase_screen(Terminal *t);

// Called by scroll_up() — shift all placement y_cells up by `lines`,
// removing any that scroll off the top.
void kitty_scroll(Terminal *t, int lines);

// Leaving the alternate screen (vim, less... exiting): drop images that were
// placed on it. Images from the normal screen are kept and shown again.
void kitty_leave_alt_screen(Terminal *t);

// Free GL resources on shutdown.
void kitty_shutdown(void);
