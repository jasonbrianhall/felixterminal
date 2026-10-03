#pragma once
// The kernel's framebuffer, for the display backend (basic_gfx_bm.cpp).
#include <stdint.h>

uint32_t fb_width();
uint32_t fb_height();
// The screen is drawn a strip at a time: video_band() hands out a scratch
// buffer for rows y .. y+h-1 (fb_width() pixels each, 0x00RRGGBB), and
// video_band_done() copies the rows that changed to the screen in its own
// format. video_invalidate(): copy everything next time.
uint32_t* video_band(int y, int h);
void video_band_done();
void video_invalidate();

void platform_poll_input();      // keyboard -> gfx_bm_key(), mouse -> gfx_bm_mouse()
void gfx_bm_key(int c);          // basic_gfx_bm.cpp: queue a key (ASCII, or 0x1000+ for arrows)
void gfx_bm_scroll(int pages);   // Shift+PgUp (+1) / Shift+PgDn (-1)
// The mouse: motion in counts (x right, y down), buttons 1 left, 2 right,
// 4 middle, wheel clicks (+ away from you).
void gfx_bm_mouse(int dx, int dy, int buttons, int wheel);
bool gfx_bm_copy();              // Ctrl+C: copy the selected text; false if nothing is selected
void gfx_bm_paste();             // Ctrl+V: type what was copied
extern volatile bool g_bm_selection;   // kernel.cpp: text is selected (irq.cpp lets Ctrl+C copy it)
