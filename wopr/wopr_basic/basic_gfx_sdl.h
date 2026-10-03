#pragma once
/*
 * basic_gfx_sdl.h — SDL2 backend lifecycle for the BASIC interpreter graphics.
 *
 * Call gfx_sdl_init() once from your main(), then call gfx_sdl_pump() and
 * gfx_sdl_render() each frame.  The BASIC interpreter thread calls the
 * gfx_* and display_* functions directly with no escape-code round-trip.
 */

#ifdef USE_SDL_WINDOW

/*
 * gfx_sdl_init — open the SDL window and initialise FreeType.
 *
 *   title    Window title.
 *   w, h     Initial window dimensions in pixels.
 *
 * The embedded DejaVu Sans Mono font is used by default.
 * Call gfx_sdl_load_font() afterwards to switch to a system font.
 *
 * Returns true on success.
 */
bool gfx_sdl_init(const char *title, int w, int h);

/*
 * gfx_sdl_load_font — replace the active FreeType face with a TTF/OTF file.
 * Falls back to the embedded font on failure.
 * Returns true if the file was loaded successfully.
 */
bool gfx_sdl_load_font(const char *path);

/*
 * gfx_sdl_shutdown — destroy all SDL and FreeType resources.
 * Call once before exit.
 */
void gfx_sdl_shutdown(void);

/*
 * gfx_sdl_pump — drain the SDL event queue.
 *
 * Must be called from the main thread (SDL requirement).
 * Returns false if the user closed the window (SDL_QUIT).
 *
 * Keyboard events are enqueued internally and consumed by
 * display_inkey() / display_getchar() / display_getline().
 */
bool gfx_sdl_pump(void);

/*
 * gfx_sdl_render — flush the current frame to the screen.
 *
 * Only re-draws when the internal dirty flag is set (set automatically
 * by any gfx_* or display_* call that changes the visible state).
 * Call once per main-loop iteration after gfx_sdl_pump().
 */
void gfx_sdl_render(void);

/* Called after each statement: paces a program that is polling INKEY$. */
void gfx_sdl_pace_statement(void);

/*
 * gfx_sdl_mark_dirty — force a re-render on the next gfx_sdl_render() call.
 * Useful after palette changes that don't touch the pixel buffer directly.
 */
void gfx_sdl_mark_dirty(void);

void gfx_maybe_mark_dirty();

/*
 * gfx_sdl_set_fullscreen — enter or leave fullscreen (desktop mode).
 * gfx_sdl_toggle_fullscreen — flip the current fullscreen state.
 * Also triggered by F11 in the event pump.
 */
void gfx_sdl_set_fullscreen(bool fs);

/*
 * gfx_screen_ex — extended SCREEN init with page support.
 * apage = active draw page (-1 = unchanged), vpage = visible page (-1 = unchanged).
 */
void gfx_screen_ex(int mode, int colorswitch, int apage, int vpage);
void gfx_sdl_toggle_fullscreen(void);

/* Reset all 16 palette slots to CGA defaults */
void gfx_palette_reset_pub(void);

/*
 * gfx_sdl_frame_start / frame_end — 60 FPS frame rate limiting.
 * Call frame_start() at the beginning of your main loop iteration,
 * then frame_end() after rendering. This ensures display updates
 * only 60 times per second regardless of how fast the loop runs.
 */
void gfx_sdl_frame_start(void);
void gfx_sdl_frame_end(void);

#endif /* USE_SDL_WINDOW */
