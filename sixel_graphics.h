#pragma once
#include "terminal.h"
#include <stdint.h>
#include <vector>
#include "kitty_graphics.h"  // KittyPngImage

// ============================================================================
// DECSIXEL (Sixel graphics) — decodes a complete DCS-wrapped sixel payload
// and rasterizes it into a texture placed at the cursor position, mirroring
// kitty_graphics.h's placement/render model.
//
// Sixel has no "protocol probe" or persistent image ids the way Kitty does —
// each DCS sequence is transmit-and-display-immediately, so there is no
// query/response handshake to implement.
// ============================================================================

// Call once after gl_init_renderer(), alongside kitty_init().
void sixel_init(void);

// Feed a complete DCS payload (everything between the DCS introducer's
// final byte and the terminating ST/BEL, NOT including "ESC P" or the
// terminator). Only called when terminal.cpp has recognized the DCS
// parameter string as a sixel sequence (ends in 'q').
// `params` is the raw parameter string before 'q' (e.g. "0;1;0"), may be
// empty. `body` is everything after 'q'.
void sixel_handle_dcs(Terminal *t, const char *params, int params_len,
                      const char *body, int body_len);

// Render all placed sixel images for this terminal. Call from term_render()
// right after kitty_render(), before gl_flush_verts().
// clip_rows: draw only within the first N rows (0 = all). See kitty_render().
void sixel_render(Terminal *t, int ox, int oy, int clip_rows = 0);

// Drop all images associated with this terminal (reset / alt-screen swap).
void sixel_clear(Terminal *t);

// Called by scroll_up() — shift all placement y_cells up by `lines`,
// removing any that scroll off the top. Mirrors kitty_scroll().
void sixel_scroll(Terminal *t, int lines);

// Remove images on the visible part of the current screen (ED 2 / clear)
void sixel_erase_screen(Terminal *t);

// For rich clipboard copy: sixel images whose top row falls within
// [row_start, row_end] (virtual rows, matching the selection), as PNG bytes.
// Reuses the KittyPngImage struct so both kinds can be merged by the caller.
std::vector<KittyPngImage> sixel_get_png_images(Terminal *t, int row_start, int row_end);

// Leaving the alternate screen: drop images placed on it (see kitty version).
void sixel_leave_alt_screen(Terminal *t);

// Free GL resources on shutdown.
void sixel_shutdown(void);
