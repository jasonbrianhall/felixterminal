#pragma once
#include "terminal.h"
#include <string>
#include <vector>

// ============================================================================
// URL DETECTION
// ============================================================================

// Update hover state based on mouse position. Returns true if hover changed (needs redraw).
bool        url_update_hover(Terminal *t, int mouse_px, int mouse_py, int ox, int oy);
// Returns the URL string under the given pixel, or empty string if none.
std::string url_at_pixel(Terminal *t, int mouse_px, int mouse_py, int ox, int oy);
// Returns index of currently hovered URL span, or -1. Use instead of url_at_pixel for cursor decisions.
int         term_hovered_url_index();
// Open a URL with xdg-open.
void        open_url(const std::string &url);

// ============================================================================
// SELECTION / CLIPBOARD
// ============================================================================

void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy, int *row, int *col);
bool cell_in_sel(Terminal *t, int r, int c);

void term_select_all(Terminal *t);
void term_copy_selection(Terminal *t);
void term_copy_selection_html(Terminal *t);
void term_copy_selection_ansi(Terminal *t);
void term_paste(Terminal *t);

// ============================================================================
// RENDERING
// ============================================================================

void term_render(Terminal *t, int ox, int oy);

// Underline (styles + color), strikethrough, overline for one cell.
// fc = the cell's resolved text color. Used by the sticky-prompt renderer too.
void term_draw_decorations(const Cell *c, float px, float py, float cw, float ch, TermColor fc);

// Block elements (U+2580-259F) as exact rectangles; false if cp isn't one.
bool term_draw_block_element(uint32_t cp, float px, float py, float cw, float ch, TermColor fc);

// Rebuild the clickable-link list for the visible screen (text URLs + OSC 8).
// resolve_cell maps a screen (row, col) to the cell shown there.
#include <functional>
void term_detect_urls(Terminal *t, std::function<Cell*(int row, int col)> resolve_cell);

// Links on virtual row `vrow` (selection/scrollback coordinates): OSC 8 links
// and URLs detected in the text. href is ready to open (www. gets https://,
// OSC 8 URIs are filtered to safe schemes). Used by Copy as Rich Text.
struct TermLinkSpan { int col_start, col_end; std::string href; };
std::vector<TermLinkSpan> term_row_links(Terminal *t, int vrow);

// Link underline for screen cell (row, col), if it's part of a link.
void term_draw_url_underline(int row, int col, float px, float py, float cw, float ch);

// ============================================================================
// KEYBOARD
// ============================================================================

// Forward: SDL types used in signature
#include <SDL2/SDL.h>
void handle_key(Terminal *t, SDL_Keysym ks, const char *text);

// ============================================================================
// CONTEXT MENU
// ============================================================================

// Indices into MENU_ITEMS (term_ui.cpp) — separators count as entries
#define MENU_ID_NEW_TERMINAL  0
#define MENU_ID_DUPLICATE     1
#define MENU_ID_COPY          3
#define MENU_ID_COPY_HTML     4
#define MENU_ID_COPY_ANSI     5
#define MENU_ID_PASTE         6
#define MENU_ID_RESET         8
#define MENU_ID_THEMES       10
#define MENU_ID_OPACITY      11
#define MENU_ID_RENDER_MODE  12
#define MENU_ID_ENTERTAINMENT 13
#define MENU_ID_SELECT_ALL   15
#define MENU_ID_FONTS        17
#define MENU_ID_ADV_OPTIONS  19
#define MENU_ID_HELP         21
#define MENU_ID_QUIT         23

// New Terminal submenu item indices
#define NEW_TERM_IDX_LOCAL   0
#define NEW_TERM_IDX_SSH     1
#define NEW_TERM_IDX_TELNET  2
#define NEW_TERM_IDX_SERIAL  3
// NEW_TERM_COUNT is now dynamic — see g_available_terminals in term_ui.cpp

// Entertainment submenu item indices
#define ENT_IDX_FIGHT    0
#define ENT_IDX_BOUNCING 1
#define ENT_IDX_SOUND    2
#define ENT_COUNT        3

struct MenuItem {
    const char *label;
    bool        separator;
};

struct ContextMenu {
    bool  visible;
    int   x, y;
    int   hovered;
    int   item_h, sep_h, pad_x, width;
    int   sub_open;
    int   sub_x, sub_y, sub_w, sub_h;
    int   sub_hovered;
};

extern const MenuItem MENU_ITEMS[];
extern const int MENU_COUNT;

extern ContextMenu g_menu;

void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h);
int  menu_hit(ContextMenu *m, int px, int py);
int  submenu_hit(ContextMenu *m, int px, int py);
void menu_render(ContextMenu *m);

struct TerminalOption {
    std::string name;
    std::string shell;
    bool is_builtin;
};

extern std::vector<TerminalOption> g_available_terminals;

void detect_available_terminals();
void action_new_terminal();
void action_new_ssh_session();

// Start another Felix Terminal window (this same executable) with the given
// arguments. cwd = working directory for the new process ("" = inherit).
// Arguments are passed through intact (quoted per MSVCRT rules on Windows).
void spawn_self_with_args(const std::vector<std::string> &args, const std::string &cwd = "");
void action_new_telnet_session();
void action_new_serial_session();
void action_new_terminal_custom(int idx);

// Free the dedicated menu font face — call before ft_shutdown().
void menu_font_shutdown();

// ============================================================================
// TOAST — brief top-centre status popup (~2 s, fades out)
// ============================================================================

// Show a notification; the colour tints the title, dot and accent bar.
void toast_show(const char *title, const char *subtitle, float r, float g, float b);
// True while visible — the main loop keeps redrawing so the fade animates.
bool toast_active();
// Draw it. Call with the other overlays, after gl_end_frame().
void toast_render(int win_w, int win_h);

// ============================================================================
// HELP OVERLAY  (F1)
// ============================================================================

extern bool g_help_visible;

// Render the help overlay. Call after menu_render() so it draws on top.
void help_render(int win_w, int win_h);

// Returns true if the key was consumed (Escape / F1 closes the overlay).
bool help_keydown(SDL_Keycode sym);

// Returns true if the mouse event was consumed. Opens links or closes overlay.
bool help_mousedown(int x, int y);

// Returns true if hover state changed (caller should set needs_render).
bool help_mousemotion(int x, int y);

// ============================================================================
// CUSTOM SHELL DIALOG
// ============================================================================

extern std::string g_custom_shell_input;
extern bool g_custom_shell_dialog_open;

void custom_shell_dialog_render(int win_w, int win_h);
bool custom_shell_dialog_keydown(SDL_Keycode sym, const char *text);

// ============================================================================
// DEBUG LOG OVERLAY (F12)
// ============================================================================

extern bool g_debuglog_visible;

// Installs the SDL log hook that mirrors console output into the overlay.
// Call once, early, so startup log lines are captured too.
void debuglog_init();

void debuglog_render(int win_w, int win_h);
bool debuglog_keydown(SDL_Keycode sym);
void debuglog_scroll(int dy);

#include "fight_mode.h"
#include "font_manager.h"
