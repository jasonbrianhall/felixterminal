#pragma once
/*
 * basic_gfx.h — Graphics backend API for the BASIC interpreter.
 *
 * commands.cpp calls these functions directly.  Two implementations exist:
 *
 *   basic_gfx_sdl.cpp  — SDL2 pixel-buffer renderer (USE_SDL_WINDOW)
 *   basic_gfx_osc.cpp  — Original OSC 666 escape-code emitter (default)
 *
 * All coordinates are in logical screen pixels (SCREEN-mode space).
 * Color indices are 0-15 CGA palette, overridable via gfx_palette().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/* Open a truecolor (32-bit ARGB) graphics surface of arbitrary size.
 * No palette — colors passed to gfx_* are 0x00RRGGBB packed values. */
void gfx_screen_tc(int w, int h);

/* Set the active SCREEN mode (0 = text, 1-28 = graphics).
 * Allocates / resizes the pixel buffer and SDL texture as needed.
 * mode 0 destroys the graphics surface and returns to text mode. */
void gfx_screen(int mode);

/* ── Palette ───────────────────────────────────────────────────────────────── */

/* Override palette slot idx (0-15) with a 24-bit RGB colour.
 * Affects all subsequent draw calls and redraws existing pixels if possible. */
void gfx_palette(int idx, int r, int g, int b);

/* ── Draw primitives ───────────────────────────────────────────────────────── */

/* Fill entire graphics surface with color, or clear the text grid (mode 0). */
void gfx_cls(int color);

/* Set single pixel. */
void gfx_pset(int x, int y, int color);

/* Read pixel color index at (x,y). Returns -1 if out of bounds or no gfx mode. */
int  gfx_point(int x, int y);

/* Draw a line from (x1,y1) to (x2,y2). */
void gfx_line(int x1, int y1, int x2, int y2, int color);

/* Draw an unfilled rectangle. */
void gfx_box(int x1, int y1, int x2, int y2, int color);

/* Draw a filled rectangle. */
void gfx_boxfill(int x1, int y1, int x2, int y2, int color);

/* Draw a circle outline. */
void gfx_circle(int cx, int cy, int radius, int color);

/* Draw a circle arc from start_angle to end_angle (radians, QB convention:
 * 0=right, increases counter-clockwise). Negative angles draw a radius line. */
void gfx_arc(int cx, int cy, int radius, double start_angle, double end_angle, int color);

/* The same with separate x and y radii (CIRCLE's aspect ratio). */
void gfx_ellipse(int cx, int cy, int rx, int ry, int color);
void gfx_ellipse_arc(int cx, int cy, int rx, int ry, double start_angle, double end_angle, int color);

/* Flood-fill from (x,y) with fill_color, stopping at border_color. */
void gfx_paint(int x, int y, int fill_color, int border_color);

/* ── Sprite store (GET / PUT) ──────────────────────────────────────────────── */

/* Capture rectangle to sprite slot id. */
void gfx_get(int id, int x1, int y1, int x2, int y2);

/* PUT actions, as in QBasic: PSET copies the sprite, PRESET copies its
 * inverse, AND / OR / XOR combine it with what's on the screen. */
enum { GFX_PUT_PSET = 0, GFX_PUT_XOR = 1, GFX_PUT_PRESET = 2, GFX_PUT_AND = 3, GFX_PUT_OR = 4 };

/* Blit sprite id at (x,y) with one of the GFX_PUT_* actions. */
void gfx_put(int id, int x, int y, int mode);

/* CLS 2: clear text rows top..bottom (from 1) and the pixels under them. */
void gfx_cls_text(int top, int bottom);

/* 1 if GET has captured sprite id. */
int  gfx_sprite_exists(int id);

/* Blit sprite directly from a GW-BASIC GET array (numeric data loaded from DATA
 * statements).  raw_longs points to the array elements as int32 values; count is
 * the number of elements.  Format: element[0] low-word = pixel width,
 * element[0] high-word = pixel height; remaining bytes = 4bpp packed rows
 * (high nibble = left pixel, low nibble = right pixel), each row padded to a
 * byte boundary.  mode: one of the GFX_PUT_* actions. */
void gfx_put_array(const int *raw_longs, int count, int x, int y, int mode);

/* ── Query ─────────────────────────────────────────────────────────────────── */

/* Returns 1 if a graphics mode is active (SCREEN > 0). */
int  gfx_active(void);

/* Returns current logical width / height of the graphics surface. */
int  gfx_width(void);
int  gfx_height(void);

void gfx_sprites_clear(void);

/* ── Mouse ─────────────────────────────────────────────────────────────────── */

/* QB64's mouse functions. _MOUSEINPUT reads the next queued mouse event
 * (-1) or reports there is none (0); _MOUSEX / _MOUSEY / _MOUSEBUTTON /
 * _MOUSEWHEEL then describe that event (before a program first calls
 * _MOUSEINPUT, they describe the mouse as it is now). Positions are pixels
 * in graphics modes, columns and rows (from 1) in SCREEN 0. Buttons: 1
 * left, 2 right, 3 middle; -1 while held. Wheel: -1 up, 1 down. */
int  gfx_mouse_input(void);
int  gfx_mouse_x(void);
int  gfx_mouse_y(void);
int  gfx_mouse_button(int n);
int  gfx_mouse_wheel(void);
void gfx_mouse_show(int on);          /* _MOUSESHOW (1) / _MOUSEHIDE (0) */
void gfx_mouse_move(int x, int y);    /* _MOUSEMOVE x, y */

/* The DOS mouse driver, INT 33h: AX is the function; the registers come
 * back as the driver leaves them (functions 0-8, &HB, &H21, &H24). */
void gfx_mouse_int33(int *ax, int *bx, int *cx, int *dx);

/* A program stopped: the mouse goes back to selecting text to copy. */
void gfx_mouse_program_end(void);

#ifdef __cplusplus
}
#endif
