#include "term_ui.h"
#include "term_pty.h"
#include "ft_font.h"
#include "gl_renderer.h"
#include "term_color.h"
#include "gl_terminal.h"
#include "gl_bouncingcircle.h"
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include "basic_graphics.h"
#include "sticky_prompt.h"
#include "ssh_key_manager.h"
#include "serial_session.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#ifndef _WIN32
#  include <strings.h>   // strcasecmp
#  include <unistd.h>    // gethostname
#else
#  define strcasecmp _stricmp
#endif
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <algorithm>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>    // readlink, setsid, execl, _exit, fork
#  include <sys/types.h> // pid_t
#endif

#include "fight_mode.h"
#include "crt_audio.h"
#include "font_manager.h"

// ============================================================================
// GLOBALS
// ============================================================================

extern bool g_sticky_prompt_enabled;
extern bool g_autoscroll_enabled;
extern bool g_line_numbers_enabled;

// ============================================================================
// TERMINAL DETECTION & LAUNCHING
// ============================================================================

#include <vector>

std::vector<TerminalOption> g_available_terminals;

// State for custom shell input dialog
std::string g_custom_shell_input;
bool g_custom_shell_dialog_open = false;

void detect_available_terminals() {
    g_available_terminals.clear();
    g_available_terminals.push_back({"Local Terminal",      "", true});
    g_available_terminals.push_back({"SSH Session",         "--ssh", true});
    g_available_terminals.push_back({"Telnet/SSL Session",  "--telnet", true});
    g_available_terminals.push_back({"Serial Session",      "--serial", true});
    g_available_terminals.push_back({"Custom Shell...",     "", false});
}

static void launch_terminal_with_shell(const char *shell) {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", self, shell);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, shell, nullptr); _exit(1); }
#endif
}

#ifdef _WIN32
// Quote one argument per the MSVCRT/CommandLineToArgvW rules so the child's
// argv gets back exactly this string (spaces, quotes, trailing backslashes).
static std::string win_quote_arg(const std::string &a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t i = 0, n = a.size();
    while (true) {
        size_t bs = 0;
        while (i < n && a[i] == '\\') { ++i; ++bs; }
        if (i == n) { out.append(bs * 2, '\\'); break; }
        if (a[i] == '"') { out.append(bs * 2 + 1, '\\'); out += '"'; }
        else             { out.append(bs, '\\');         out += a[i]; }
        ++i;
    }
    out += '"';
    return out;
}
#endif

void spawn_self_with_args(const std::vector<std::string> &args, const std::string &cwd) {
#ifdef _WIN32
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
    // Build the command line in UTF-8, then convert (paths may be non-ASCII)
    std::string cmd;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, self, -1, nullptr, 0, nullptr, nullptr);
        std::string self8(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, self, -1, &self8[0], n, nullptr, nullptr);
        cmd = win_quote_arg(self8);
    }
    for (const std::string &a : args) cmd += " " + win_quote_arg(a);
    auto widen = [](const std::string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n : 1, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;   // includes terminating NUL
    };
    std::wstring wcmd = widen(cmd);
    std::wstring wcwd = cwd.empty() ? std::wstring() : widen(cwd);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                       nullptr, cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    std::vector<char *> argv;
    argv.push_back(self);
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) { /* keep inherited cwd */ }
        execv(self, argv.data());
        _exit(1);
    }
#endif
}

void action_new_terminal_custom(int idx) {
    if (idx < 0 || idx >= (int)g_available_terminals.size()) return;
    const TerminalOption &opt = g_available_terminals[idx];
    if (opt.is_builtin) {
        if (opt.name == "Local Terminal")           action_new_terminal();
        else if (opt.name == "SSH Session")         action_new_ssh_session();
        else if (opt.name == "Telnet/SSL Session")  action_new_telnet_session();
        else if (opt.name == "Serial Session")      action_new_serial_session();
    } else {
        // Custom Shell - open input dialog
        g_custom_shell_input.clear();
        g_custom_shell_dialog_open = true;
        SDL_StartTextInput();
    }
}

// ============================================================================
// URL DETECTION
// ============================================================================

struct UrlSpan {
    int row, col_start, col_end;  // col_end is inclusive
    std::string url;   // display text
    std::string href;  // actual href (may prepend https:// for www. links)
    int link_id = 0;   // OSC 8 link id (0 = detected from text)
};

static std::vector<UrlSpan> s_urls;
static int s_hovered_url = -1;  // index into s_urls, or -1

static bool is_url_char(char c) {
    // Characters valid inside a URL (not terminal punctuation)
    return (c > ' ') && c != '"' && c != '\'' && c != '<' && c != '>' && c != '`';
}

static bool starts_with(const std::string &s, const char *prefix) {
    size_t plen = strlen(prefix);
    return s.size() >= plen && s.compare(0, plen, prefix) == 0;
}

// Strip trailing punctuation that is likely not part of the URL
static std::string trim_url(const std::string &s) {
    size_t end = s.size();
    // Strip paired closers if their opener isn't in the URL
    while (end > 0) {
        char c = s[end-1];
        if (c == ')' && s.find('(') == std::string::npos) { end--; continue; }
        if (c == ']' && s.find('[') == std::string::npos) { end--; continue; }
        if (c == '}' && s.find('{') == std::string::npos) { end--; continue; }
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') { end--; continue; }
        break;
    }
    return s.substr(0, end);
}

// OSC 8 links come from the program, not the user, so only open schemes a
// browser/file manager should handle. file://HOST/path from `ls --hyperlink`
// is rewritten to file:///path when HOST is this machine (xdg-open and the
// Windows shell don't accept a hostname); a different host (e.g. over SSH)
// is refused, since the path doesn't exist here.
static std::string osc8_href(const char *uri) {
    if (!uri) return {};
    std::string u(uri);
    auto lower_starts = [&](const char *pfx) {
        size_t n = strlen(pfx);
        if (u.size() < n) return false;
        for (size_t i = 0; i < n; i++)
            if (tolower((unsigned char)u[i]) != pfx[i]) return false;
        return true;
    };
    if (lower_starts("http://") || lower_starts("https://") ||
        lower_starts("ftp://")  || lower_starts("mailto:"))
        return u;
    if (lower_starts("file://")) {
        size_t slash = u.find('/', 7);
        if (slash == std::string::npos) return {};
        std::string host = u.substr(7, slash - 7);
        if (!host.empty() && host != "localhost") {
            char me[256] = {0};
#ifdef _WIN32
            DWORD n = sizeof(me);
            GetComputerNameA(me, &n);
#else
            gethostname(me, sizeof(me) - 1);
#endif
            if (strcasecmp(host.c_str(), me) != 0) return {};   // remote file
        }
        return "file://" + u.substr(slash);
    }
    return {};   // javascript:, custom app schemes, etc.
}

// Add a span for each run of cells sharing an OSC 8 link id on this row.
static void append_osc8_spans(Terminal *t, int row, std::vector<UrlSpan> &out,
                              const std::function<Cell*(int, int)> &resolve_cell) {
    int col = 0;
    while (col < t->cols) {
        uint16_t id = cell_link(resolve_cell(row, col));
        if (!id) { col++; continue; }
        int start = col;
        while (col < t->cols && cell_link(resolve_cell(row, col)) == id) col++;
        std::string href = osc8_href(term_link_uri(id));
        if (href.empty()) continue;
        UrlSpan span;
        span.row = row; span.col_start = start; span.col_end = col - 1;
        span.url = term_link_uri(id); span.href = href; span.link_id = id;
        out.push_back(span);
    }
}

// Scan the visible grid and rebuild s_urls
// All links on one row: OSC 8 links first, then URLs found in the text
// (https://, http://, ftp://, file://, www.). Cells that are part of an
// OSC 8 link are skipped by the text scan so a link isn't counted twice.
// Shared by the on-screen link detection and both HTML copy paths.
static void scan_row_links(Terminal *t, int row,
                           const std::function<Cell*(int, int)> &resolve_cell,
                           std::vector<UrlSpan> &out) {
    append_osc8_spans(t, row, out, resolve_cell);

    static const char *prefixes[] = { "https://", "http://", "ftp://", "file://", "www.", nullptr };

    // Plain-text copy of the row, one char per cell so columns line up
    std::string line;
    line.reserve(t->cols);
    for (int col = 0; col < t->cols; col++) {
        Cell *c = resolve_cell(row, col);
        uint32_t cp = c->cp;
        if (!cp) cp = ' ';
        if (cell_link(c)) cp = ' ';   // part of an OSC 8 link
        // Only handle ASCII for URL scanning simplicity
        line += (cp < 0x80) ? (char)cp : '?';  // placeholder keeps column alignment
    }

    size_t pos = 0;
    while (pos < line.size()) {
        // Find earliest prefix match from pos
        size_t best = std::string::npos;
        for (int pi = 0; prefixes[pi]; pi++) {
            size_t f = line.find(prefixes[pi], pos);
            if (f < best) best = f;
        }
        if (best == std::string::npos) break;

        // Scan forward to end of URL
        size_t end = best;
        while (end < line.size() && is_url_char(line[end])) end++;

        std::string url = trim_url(line.substr(best, end - best));
        size_t min_len = starts_with(url, "www.") ? 6 : 8; // www.x.com minimum
        if (url.size() > min_len) {
            UrlSpan span;
            span.row       = row;
            span.col_start = (int)best;
            span.col_end   = (int)(best + url.size() - 1);
            span.url       = url;
            span.href      = starts_with(url, "www.") ? "https://" + url : url;
            out.push_back(span);
        }
        pos = end;
    }
}

// Scan the visible grid and rebuild s_urls
static void detect_urls(Terminal *t,
                         std::function<Cell*(int row, int col)> resolve_cell) {
    s_urls.clear();
    for (int row = 0; row < t->rows; row++)
        scan_row_links(t, row, resolve_cell, s_urls);
}

std::vector<TermLinkSpan> term_row_links(Terminal *t, int vrow) {
    std::vector<UrlSpan> spans;
    scan_row_links(t, vrow, [t](int r, int c) { return vcell(t, r, c); }, spans);
    std::vector<TermLinkSpan> out;
    out.reserve(spans.size());
    for (const UrlSpan &u : spans)
        out.push_back({u.col_start, u.col_end, u.href});
    return out;
}

static int url_at(int row, int col) {
    for (int i = 0; i < (int)s_urls.size(); i++) {
        const UrlSpan &u = s_urls[i];
        if (u.row == row && col >= u.col_start && col <= u.col_end)
            return i;
    }
    return -1;
}

// Link underline for a screen cell (text URLs and OSC 8 links). All cells
// of the hovered link light up — for OSC 8 that includes other rows.
void term_draw_url_underline(int row, int col, float px, float py, float cw, float ch) {
    int uid = url_at(row, col);
    if (uid < 0) return;
    bool hovered = (uid == s_hovered_url);
    if (!hovered && s_hovered_url >= 0 && s_hovered_url < (int)s_urls.size()) {
        int lid = s_urls[uid].link_id;
        hovered = lid && lid == s_urls[s_hovered_url].link_id;
    }
    float ur = hovered ? 0.4f : 0.35f;
    float ug = hovered ? 0.8f : 0.6f;
    float ub = hovered ? 1.0f : 0.9f;
    float uh = hovered ? 2.f : 1.f;
    draw_rect(px, py + ch - uh - 1, cw, uh, ur, ug, ub, 1.f);
}

void term_detect_urls(Terminal *t, std::function<Cell*(int row, int col)> resolve_cell) {
    detect_urls(t, resolve_cell);
}

void open_url(const std::string &url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(1);
    }
#endif
}

extern int  g_font_size;
extern bool g_blink_text_on;

// ============================================================================
// MENU FONT — always DejaVu Regular at a fixed size, unaffected by the user's
// shell font choice or zoom level.
// ============================================================================

#define MENU_FONT_SIZE 14

// Dedicated FreeType face for menu rendering.  Created once from the embedded
// DejaVu Regular data via ft_make_menu_face(), so it is never affected by
// font switching or zoom changes.
static FT_Face s_menu_face = nullptr;

static void ensure_menu_face() {
    if (s_menu_face) return;
    s_menu_face = ft_make_menu_face(MENU_FONT_SIZE);
}

// Call at shutdown to free the menu face's own buffer.
void menu_font_shutdown() {
    if (s_menu_face) {
        // ft_make_menu_face stores the buffer in generic.data with a finalizer
        free(s_menu_face->generic.data);
        s_menu_face->generic.data = nullptr;
        FT_Done_Face(s_menu_face);
        s_menu_face = nullptr;
    }
}

// Render menu text using the fixed DejaVu face at MENU_FONT_SIZE.
// Temporarily swaps the global faces so draw_text() picks them up, then
// restores everything afterwards.
static float draw_text_menu(const char *text, float x, float y,
                            float r, float g, float b, float a, uint8_t attrs = 0) {
    ensure_menu_face();
    if (!s_menu_face)
        return draw_text(text, x, y, MENU_FONT_SIZE, MENU_FONT_SIZE, r, g, b, a, attrs);

    // Swap all four shell faces for the menu face (bold/obl variants fall back
    // to the same menu face, which is fine for a UI menu).
    FT_Face saved      = s_ft_face;
    FT_Face saved_reg  = s_ft_face_reg;
    FT_Face saved_obl  = s_ft_face_obl;
    FT_Face saved_bobl = s_ft_face_bobl;
    s_ft_face      = s_menu_face;
    s_ft_face_reg  = s_menu_face;
    s_ft_face_obl  = s_menu_face;
    s_ft_face_bobl = s_menu_face;

    float ret = draw_text(text, x, y, MENU_FONT_SIZE, MENU_FONT_SIZE, r, g, b, a, attrs);

    s_ft_face      = saved;
    s_ft_face_reg  = saved_reg;
    s_ft_face_obl  = saved_obl;
    s_ft_face_bobl = saved_bobl;
    return ret;
}

// ============================================================================
// CONTEXT MENU DATA
// ============================================================================

static const float OPACITY_LEVELS[] = { 1.0f, 0.85f, 0.7f, 0.5f, 0.3f, 0.1f };
static const char* OPACITY_NAMES[]  = { "100%", "85%", "70%", "50%", "30%", "10%" };
static const int   OPACITY_COUNT    = 6;

static const char* ADV_OPTIONS_NAMES[] = { "Sticky Prompt", "Disable Autoscroll", "Line Numbers" };
static const int   ADV_OPTIONS_COUNT   = 3;

static const char* RENDER_MODE_NAMES[] = { "Normal", "CRT", "LCD", "VHS", "Focus", "Commodore 64", "Bad Composite", "Bloom", "Ghosting", "Wireframe" };
static_assert(sizeof(RENDER_MODE_NAMES)/sizeof(RENDER_MODE_NAMES[0]) == RENDER_MODE_COUNT,
              "RENDER_MODE_NAMES count mismatch");

const MenuItem MENU_ITEMS[] = {
    { "New Terminal  >", false },
    { "Duplicate Session", false },
    { nullptr,           true  },
    { "Copy as Rich Text", false },
    { "Copy as HTML",    false },
    { "Copy as ANSI",    false },
    { "Paste",           false },
    { nullptr,           true  },
    { "Reset",           false },
    { nullptr,           true  },
    { "Color Theme  >",  false },
    { "Transparency  >", false },
    { "Render Mode  >",  false },
    { "Entertainment >", false },
    { nullptr,           true  },
    { "Select All",      false },
    { nullptr,           true  },
    { "Font  >",         false },
    { nullptr,           true  },
    { "Advanced Options >", false },
    { nullptr,           true  },
    { "Help",            false },
    { nullptr,           true  },
    { "Quit",            false },
};
const int MENU_COUNT = (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]));
// MENU_ID_* in term_ui.h are indices into MENU_ITEMS; catch them drifting apart
static_assert(MENU_ID_QUIT == (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0])) - 1,
              "MENU_ID_* constants out of sync with MENU_ITEMS");

ContextMenu g_menu = {};

extern float g_opacity;

// ============================================================================
// SELECTION HELPERS
// ============================================================================

void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy, int *row, int *col) {
    *col = (int)((px - ox) / t->cell_w);
    *row = (int)((py - oy) / t->cell_h);
    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;
    if (*col >= t->cols) *col = t->cols - 1;
    if (*row >= t->rows) *row = t->rows - 1;
    *row += t->sb_count - t->sb_offset;
}

bool cell_in_sel(Terminal *t, int r, int c) {
    if (!t->sel_exists && !t->sel_active) return false;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    if (r < r0 || r > r1) return false;
    if (r == r0 && c < c0) return false;
    if (r == r1 && c > c1) return false;
    return true;
}

// ============================================================================
// URL HOVER / HIT TEST
// ============================================================================

static int pixel_to_render_row(Terminal *t, int py, int oy) {
    int row = (int)((py - oy) / t->cell_h);
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    return row;
}
static int pixel_to_render_col(Terminal *t, int px, int ox) {
    int col = (int)((px - ox) / t->cell_w);
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    return col;
}

bool url_update_hover(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid != s_hovered_url) {
        s_hovered_url = uid;
        return true;
    }
    return false;
}

std::string url_at_pixel(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid >= 0) return s_urls[uid].href;
    return {};
}

// ============================================================================
// CLIPBOARD
// ============================================================================

void term_select_all(Terminal *t) {
    t->sel_start_row = 0;
    t->sel_start_col = 0;
    t->sel_end_row   = t->sb_count + t->rows - 1;
    t->sel_end_col   = t->cols - 1;
    t->sel_active    = false;
    t->sel_exists    = true;
}

void term_copy_selection(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    int bufsize = (r1 - r0 + 1) * (t->cols * 4 + 1) + 1;
    char *buf = (char*)malloc(bufsize);
    int pos = 0;
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            if (cell_is_wide_tail(vcell(t,r,c))) continue;  // 2nd half of a wide char
            uint32_t cp = vcell(t,r,c)->cp;
            if (!cp) cp = ' ';
            if      (cp < 0x80)    { buf[pos++] = (char)cp; }
            else if (cp < 0x800)   { buf[pos++]=(char)(0xC0|(cp>>6)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { buf[pos++]=(char)(0xE0|(cp>>12)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else { buf[pos++]=(char)(0xF0|(cp>>18)); buf[pos++]=(char)(0x80|((cp>>12)&0x3F)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
        }
        if (r < r1) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    SDL_SetClipboardText(buf);
    free(buf);
}

static void append_hex_color(std::string &s, float r, float g, float b) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             (int)(r*255+.5f), (int)(g*255+.5f), (int)(b*255+.5f));
    s += buf;
}

static void append_html_char(std::string &s, uint32_t cp) {
    if      (cp == '<')  s += "&lt;";
    else if (cp == '>')  s += "&gt;";
    else if (cp == '&')  s += "&amp;";
    else if (cp == '"')  s += "&quot;";
    else if (cp < 0x80)  s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0|(cp>>6)); s += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0|(cp>>12)); s += (char)(0x80|((cp>>6)&0x3F)); s += (char)(0x80|(cp&0x3F)); }
    else { s+=(char)(0xF0|(cp>>18)); s+=(char)(0x80|((cp>>12)&0x3F)); s+=(char)(0x80|((cp>>6)&0x3F)); s+=(char)(0x80|(cp&0x3F)); }
}

void term_copy_selection_html(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    const Theme &th = THEMES[g_theme_idx];
    char bg_hex[8], fg_hex[8];
    snprintf(bg_hex, sizeof(bg_hex), "#%02x%02x%02x",
             (int)(th.bg_r*255+.5f), (int)(th.bg_g*255+.5f), (int)(th.bg_b*255+.5f));
    // Default foreground from palette index 7
    TermColor deffg = tcolor_resolve(TCOLOR_PALETTE(7));
    snprintf(fg_hex, sizeof(fg_hex), "#%02x%02x%02x",
             (int)(deffg.r*255+.5f), (int)(deffg.g*255+.5f), (int)(deffg.b*255+.5f));

    std::string html;
    html.reserve(8192);
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n";
    html += "<title>Terminal — "; html += th.name; html += "</title>\n";
    html += "<style>\n";
    html += "  body { margin: 0; padding: 0; background: "; html += bg_hex; html += "; }\n";
    html += "  .terminal {\n";
    html += "    background: "; html += bg_hex; html += ";\n";
    html += "    color: "; html += fg_hex; html += ";\n";
    html += "    font-family: 'DejaVu Sans Mono', 'Cascadia Code', 'Fira Code', 'Consolas', monospace;\n";
    html += "    font-size: 14px;\n";
    html += "    line-height: 1;\n";
    html += "    white-space: pre;\n";
    html += "    overflow-x: auto;\n";
    html += "  }\n";
    html += "  .terminal a {\n";
    html += "    color: #6ab0f5;\n";
    html += "    text-decoration: underline;\n";
    html += "  }\n";
    html += "  .terminal a:hover { color: #9dcfff; }\n";
    html += "</style>\n</head>\n<body>\n<div class=\"terminal\">";

    // Collect any kitty images that fall within the selection row range
    std::vector<KittyHtmlImage> kimages = kitty_get_html_images(t, r0, r1);
    int kimg_idx = 0;

    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;
    bool span_open = false;
    auto close_span = [&]() { if (span_open) { html += "</span>"; span_open = false; } };

    // Build a helper to detect URLs in a row of cells (selection-relative row index)
    auto row_url_spans = [&](int r) -> std::vector<UrlSpan> {
        std::vector<UrlSpan> spans;
        scan_row_links(t, r, [&](int rr, int cc) { return vcell(t, rr, cc); }, spans);
        return spans;
    };

    for (int r = r0; r <= r1; r++) {
        // Insert any kitty images that start on this row (before the text line)
        while (kimg_idx < (int)kimages.size() && kimages[kimg_idx].y_cell == r) {
            close_span();
            html += kimages[kimg_idx].img_tag;
            kimg_idx++;
        }

        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        std::vector<UrlSpan> row_urls = row_url_spans(r);
        bool link_open = false;
        auto close_link = [&]() { if (link_open) { html += "</a>"; link_open = false; } };

        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            if (cell_is_wide_tail(cellp)) continue;  // 2nd half of a wide char
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }

            // Check if we're entering or leaving a URL span
            int uid = -1;
            for (int ui = 0; ui < (int)row_urls.size(); ui++) {
                if (c >= row_urls[ui].col_start && c <= row_urls[ui].col_end) { uid = ui; break; }
            }
            bool in_url = (uid >= 0);
            bool at_url_start = in_url && (c == row_urls[uid].col_start);
            bool past_url     = link_open && !in_url;
            if (past_url) { close_span(); close_link(); }
            if (at_url_start) {
                close_span();
                close_link();
                html += "<a href=\"";
                // HTML-escape the href
                for (char ch : row_urls[uid].href) {
                    if (ch == '"') html += "&quot;";
                    else           html += ch;
                }
                html += "\" >";
                link_open = true;
                // Reset span state so colors are re-emitted inside the link
                last_fg = ~(TermColorVal)0; last_bg = ~(TermColorVal)0; last_attrs = 0xFF;
            }

            if (fg != last_fg || bg != last_bg || attrs != last_attrs) {
                close_span();
                TermColor fc = tcolor_resolve(fg);
                TermColor bc = tcolor_resolve(bg);

                // Only emit background if it differs from the theme background
                bool fg_is_default = !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) == 7;
                bool bg_is_default = !TCOLOR_IS_RGB(bg) && TCOLOR_IDX(bg) == 0;
                if (!bg_is_default) {
                    bg_is_default = (fabsf(bc.r - th.bg_r) < 0.01f &&
                                     fabsf(bc.g - th.bg_g) < 0.01f &&
                                     fabsf(bc.b - th.bg_b) < 0.01f);
                }

                std::string style;
                if (!fg_is_default) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                             (int)(fc.r*255+.5f), (int)(fc.g*255+.5f), (int)(fc.b*255+.5f));
                    style += "color:"; style += hex;
                }
                if (!bg_is_default) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                             (int)(bc.r*255+.5f), (int)(bc.g*255+.5f), (int)(bc.b*255+.5f));
                    if (!style.empty()) style += ";";
                    style += "background:"; style += hex;
                }
                if (attrs & ATTR_BOLD)      { if (!style.empty()) style += ";"; style += "font-weight:bold"; }
                if (attrs & ATTR_ITALIC)    { if (!style.empty()) style += ";"; style += "font-style:italic"; }
                if (attrs & ATTR_UNDERLINE) { if (!style.empty()) style += ";"; style += "text-decoration:underline"; }

                if (!style.empty()) {
                    html += "<span style=\""; html += style; html += "\">";
                    span_open = true;
                }
                last_fg = fg; last_bg = bg; last_attrs = attrs;
            }
            append_html_char(html, cp);
        }
        close_span();
        close_link();
        last_fg = ~(TermColorVal)0;
        if (r < r1) html += '\n';
    }
    html += "</div>\n";
    html += "<script>\n";
    html += "document.querySelectorAll('a.img-dl').forEach(function(a){\n";
    html += "  var m = a.getAttribute('href').match(/^data:([^;]+);base64,(.*)$/);\n";
    html += "  if (!m) return;\n";
    html += "  var bin = atob(m[2]);\n";
    html += "  var bytes = new Uint8Array(bin.length);\n";
    html += "  for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);\n";
    html += "  a.href = URL.createObjectURL(new Blob([bytes], {type: m[1]}));\n";
    html += "});\n";
    html += "</script>\n";
    html += "</body>\n</html>\n";
    SDL_SetClipboardText(html.c_str());
}

void term_copy_selection_ansi(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    std::string out;
    out.reserve(4096);
    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;

    auto emit_sgr = [&](TermColorVal fg, TermColorVal bg, uint8_t attrs) {
        out += "\x1b[0";
        if (attrs & ATTR_BOLD)      out += ";1";
        if (attrs & ATTR_UNDERLINE) out += ";4";
        if (attrs & ATTR_BLINK)     out += ";5";
        if (TCOLOR_IS_RGB(fg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";38;2;%d;%d;%d",(int)TCOLOR_R(fg),(int)TCOLOR_G(fg),(int)TCOLOR_B(fg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(fg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";3%d",idx);     out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";9%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";38;5;%d",idx); out+=b; }
        }
        if (TCOLOR_IS_RGB(bg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";48;2;%d;%d;%d",(int)TCOLOR_R(bg),(int)TCOLOR_G(bg),(int)TCOLOR_B(bg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(bg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";4%d",idx);      out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";10%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";48;5;%d",idx);  out+=b; }
        }
        out += 'm';
        last_fg = fg; last_bg = bg; last_attrs = attrs;
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            if (cell_is_wide_tail(cellp)) continue;  // 2nd half of a wide char
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            if (fg != last_fg || bg != last_bg || attrs != last_attrs)
                emit_sgr(fg, bg, attrs);
            // ANSI output: only ASCII printable (32-126) and common control chars
            if (cp >= 32 && cp < 127) {
                out += (char)cp;
            } else if (cp == '\t') {
                out += '\t';
            } else if (cp == ' ') {
                out += ' ';
            }
            // Non-ASCII characters are silently skipped in ANSI mode
        }
        out += "\x1b[0m";
        last_fg = ~(TermColorVal)0;
        if (r < r1) out += '\n';
    }
    SDL_SetClipboardText(out.c_str());
}

void term_paste(Terminal *t) {
    if (!SDL_HasClipboardText()) return;
    char *text = SDL_GetClipboardText();
    if (text && text[0]) {
        if (t->bracketed_paste) term_write(t, "\x1b[200~", 6);
        term_write(t, text, (int)strlen(text));
        if (t->bracketed_paste) term_write(t, "\x1b[201~", 6);
    }
    SDL_free(text);
}

// ============================================================================
// RENDERING
// ============================================================================

// Block Elements (U+2580-259F): draw as exact procedural rectangles instead
// of going through the font atlas. Font anti-aliasing/hinting on solid block
// glyphs leaves partial-coverage pixels at the edges, which produces a
// visible seam/grid pattern once thousands of them tile together (as in
// ANSI-art image rendering) — and several of these codepoints aren't even in
// the embedded font. Returns false if cp isn't a block element.
// Shared with the sticky-prompt renderer.
bool term_draw_block_element(uint32_t cp, float px, float py, float cw, float ch, TermColor fc) {
    if (cp == 0x2580) {
        draw_rect(px, py, cw, ch * 0.5f, fc.r, fc.g, fc.b, 1.f); // upper half
    } else if (cp >= 0x2581 && cp <= 0x2588) {
        // Lower N-eighths block, bottom-aligned (2581=1/8 .. 2588=8/8)
        float frac = (float)(cp - 0x2580) / 8.f;
        draw_rect(px, py + ch * (1.f - frac), cw, ch * frac, fc.r, fc.g, fc.b, 1.f);
    } else if (cp >= 0x2589 && cp <= 0x258F) {
        // Left N-eighths block, left-aligned (2589=7/8 .. 258F=1/8)
        float frac = (float)(0x2590 - cp) / 8.f;
        draw_rect(px, py, cw * frac, ch, fc.r, fc.g, fc.b, 1.f);
    } else if (cp == 0x2590) {
        draw_rect(px + cw * 0.5f, py, cw * 0.5f, ch, fc.r, fc.g, fc.b, 1.f); // right half
    } else if (cp == 0x2591) {
        draw_rect(px, py, cw, ch, fc.r, fc.g, fc.b, 0.25f); // light shade
    } else if (cp == 0x2592) {
        draw_rect(px, py, cw, ch, fc.r, fc.g, fc.b, 0.50f); // medium shade
    } else if (cp == 0x2593) {
        draw_rect(px, py, cw, ch, fc.r, fc.g, fc.b, 0.75f); // dark shade
    } else if (cp == 0x2594) {
        draw_rect(px, py, cw, ch * 0.125f, fc.r, fc.g, fc.b, 1.f); // upper 1/8
    } else if (cp == 0x2595) {
        draw_rect(px + cw * 0.875f, py, cw * 0.125f, ch, fc.r, fc.g, fc.b, 1.f); // right 1/8
    } else if (cp >= 0x2596 && cp <= 0x259F) {
        // Quadrant blocks: bit0=TL bit1=TR bit2=BL bit3=BR
        static const uint8_t quad_mask[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
        uint8_t m = quad_mask[cp - 0x2596];
        float hw = cw * 0.5f, hh = ch * 0.5f;
        if (m & 1) draw_rect(px,      py,      hw, hh, fc.r, fc.g, fc.b, 1.f); // TL
        if (m & 2) draw_rect(px + hw, py,      hw, hh, fc.r, fc.g, fc.b, 1.f); // TR
        if (m & 4) draw_rect(px,      py + hh, hw, hh, fc.r, fc.g, fc.b, 1.f); // BL
        if (m & 8) draw_rect(px + hw, py + hh, hw, hh, fc.r, fc.g, fc.b, 1.f); // BR
    } else {
        return false;
    }
    return true;
}

// Underline (all SGR 4:n styles, SGR 58 color), strikethrough and overline
// for one cell. Shared by term_render() and sticky_prompt_render_split().
void term_draw_decorations(const Cell *c, float px, float py, float cw, float ch, TermColor fc) {
    if (c->attrs & ATTR_UNDERLINE) {
        // SGR 58 underline color, else the text color
        TermColor uc = c->ul_color ? tcolor_resolve(CELL_UL_COLOR(c)) : fc;
        switch (cell_ul_style(c)) {
        case UL_DOUBLE:
            draw_rect(px, py+ch-4, cw, 1, uc.r, uc.g, uc.b, 1.f);
            draw_rect(px, py+ch-2, cw, 1, uc.r, uc.g, uc.b, 1.f);
            break;
        case UL_CURLY: {
            // Sine wave, phase taken from the absolute x so it runs
            // continuously across cells. One 1px-wide column per x.
            float amp    = SDL_max(1.f, ch * 0.07f);
            float period = SDL_max(4.f, cw);
            float base   = py + ch - 1.5f - amp;
            for (int x = 0; x < (int)cw; x++) {
                float ax = px + x;
                float y  = base + amp * sinf(ax * 6.2831853f / period);
                draw_rect(ax, y, 1, 1.5f, uc.r, uc.g, uc.b, 1.f);
            }
            break;
        }
        case UL_DOTTED:
            for (int x = 0; x < (int)cw; x++)
                if ((((int)px + x) & 3) < 2)
                    draw_rect(px + x, py+ch-2, 1, 1.5f, uc.r, uc.g, uc.b, 1.f);
            break;
        case UL_DASHED:
            for (int x = 0; x < (int)cw; x++)
                if ((((int)px + x) % 6) < 4)
                    draw_rect(px + x, py+ch-2, 1, 1.5f, uc.r, uc.g, uc.b, 1.f);
            break;
        default:
            draw_rect(px, py+ch-2, cw, 2, uc.r, uc.g, uc.b, 1.f);
            break;
        }
    }
    if (c->attrs & ATTR_STRIKE)
        draw_rect(px, py+ch*0.45f, cw, 1, fc.r, fc.g, fc.b, 1.f);
    if (c->attrs & ATTR_OVERLINE)
        draw_rect(px, py+1, cw, 1, fc.r, fc.g, fc.b, 1.f);
}

void term_render(Terminal *t, int ox, int oy) {
    // If sticky prompt is enabled, use split rendering
    if (g_sticky_prompt_enabled) {
        sticky_prompt_render_split(t, ox, oy);
        return;
    }
    
    float cw = t->cell_w, ch = t->cell_h;
    
    // Adjust ox for line numbers if enabled
    float line_num_width = 0;
    if (g_line_numbers_enabled) {
        line_num_width = cw * 6;  // 6 character widths for line numbers
        ox += line_num_width;  // Shift terminal to the right
    }
    
    bool scrolled = (t->sb_offset > 0);

    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    auto resolve_cell = [&](int row, int col) -> Cell* {
        if (scrolled) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };

    // Detect URLs in visible content
    detect_urls(t, resolve_cell);

    // Pass 1: backgrounds — only redraw rows that changed
    for (int row = 0; row < t->rows; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor bc = tcolor_resolve(bg);
            int vrow = row + t->sb_count - t->sb_offset;
            if (cell_in_sel(t, vrow, col)) {
                // Selection highlight: use original foreground color as background
                TermColor sel_bg = tcolor_resolve(fg);
                draw_rect(px, py, cw, ch, sel_bg.r, sel_bg.g, sel_bg.b, 1.0f);
            } else {
                // When BASIC graphics are active, draw default cell backgrounds
                // transparent so the BASIC layer shows through underneath.
                float bg_alpha = (s_basic_palette_active && bg == TCOLOR_PALETTE(0)) ? 0.f : 1.f;
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, bg_alpha);
            }
        }
    }

    // Pass 1.5: line numbers (if enabled)
    if (g_line_numbers_enabled) {
        for (int row = 0; row < t->rows; row++) {
            int vrow = row + t->sb_count - t->sb_offset;
            char line_num[16];
            snprintf(line_num, sizeof(line_num), "%5d", vrow);
            // Draw at original ox position (before we shifted right)
            float line_num_x = ox - line_num_width;
            float line_num_y = oy + row * ch;
            draw_text(line_num, line_num_x, line_num_y + ch * 0.82f, g_font_size, (int)ch, 0.6f, 0.6f, 0.7f, 0.8f, 0);  // Dim gray
        }
    }

    // Pass 2: glyphs and decorations — only redraw rows that changed
    int dirty_cells = 0;
    for (int row = 0; row < t->rows; row++) {
        if (!term_row_is_dirty(t, row)) continue;
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }

            // If cell is selected, invert: use background color as text on foreground-colored background
            int vrow = row + t->sb_count - t->sb_offset;
            if (cell_in_sel(t, vrow, col)) {
                // Use background color as text (inverted)
                TermColor bc = tcolor_resolve(bg);
                fc = bc;
            }

            uint32_t cp = c->cp;
            bool blink_hidden = ((c->attrs & ATTR_BLINK) && !g_blink_text_on) || cell_is_hidden(c);
            if (cp && cp != ' ' && !blink_hidden) {
                // Block elements as exact rectangles (see term_draw_block_element)
                bool drew_block = term_draw_block_element(cp, px, py, cw, ch, fc);
                if (!drew_block) {
                    char tmp[5] = {};
                    cp_to_utf8(cp, tmp);
                    float baseline = py + ch * 0.82f;
                    draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs,
                              cell_is_wide(c) ? cw * 2.f : 0.f);
                }
                dirty_cells++;
            }
            if (!blink_hidden)
                term_draw_decorations(c, px, py, cw, ch, fc);

            // URL / OSC 8 link underline
            term_draw_url_underline(row, col, px, py, cw, ch);
        }
    }

    // Clear dirty flags — rows are now up-to-date in the term FBO
    term_clear_dirty(t);

    // Feed glyph activity to CRT audio buzz
    crt_audio_set_activity((float)dirty_cells / (float)(t->cols * t->rows));

    // Kitty graphics — render placed images over the glyph layer
    kitty_render(t, ox, oy);
    sixel_render(t, ox, oy);

    // Cursor
    if (!scrolled && term_cursor_visible(t)) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        switch (t->cursor_shape) {
        case 0: draw_rect(cx, cy, cw, ch, 1,1,1, 0.3f); break;
        case 2: draw_rect(cx, cy, 2, ch, 1,1,1, 0.85f); break;
        default: draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f); break;
        }
    }

    // Scrollbar
    if (scrolled && t->sb_count > 0) {
        float win_h   = t->rows * ch;
        int   total_rows = t->sb_count + t->rows;
        float bar_h   = win_h * t->rows / total_rows;
        if (bar_h < 8) bar_h = 8;
        float bar_y   = oy + (win_h - bar_h) * (float)(total_rows - t->rows - t->sb_offset) / (total_rows - t->rows);
        float bar_x   = ox + t->cols * cw - 4;
        draw_rect(bar_x, oy, 4, win_h, 0,0,0, 0.3f);
        draw_rect(bar_x, bar_y, 4, bar_h, 0.6f, 0.6f, 0.7f, 0.8f);
    }
}

// ============================================================================
// KEYBOARD
// ============================================================================

void handle_key(Terminal *t, SDL_Keysym ks, const char *text) {
    SDL_Keymod mod = (SDL_Keymod)ks.mod;
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool ctrl  = (mod & KMOD_CTRL)  != 0;
    bool alt   = (mod & KMOD_ALT)   != 0;

    if (ctrl && shift) {
        if (ks.sym == SDLK_v) { term_paste(t); return; }
        if (ks.sym == SDLK_c) { return; }
    }

    if (ctrl && ks.sym == SDLK_a) { term_select_all(t); return; }

    auto arrow = [&](const char *normal, const char *app, char letter) {
        if (!shift && !ctrl && !alt) {
            term_write(t, t->app_cursor_keys ? app : normal, 3);
        } else {
            int m = 1 + (shift?1:0) + (alt?2:0) + (ctrl?4:0);
            char seq[16];
            int n = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", m, letter);
            term_write(t, seq, n);
        }
    };

    switch (ks.sym) {
    case SDLK_UP:    arrow("\x1b[A", "\x1bOA", 'A'); crt_audio_cursor_ping(); return;
    case SDLK_DOWN:  arrow("\x1b[B", "\x1bOB", 'B'); crt_audio_cursor_ping(); return;
    case SDLK_RIGHT: arrow("\x1b[C", "\x1bOC", 'C'); crt_audio_cursor_ping(); return;
    case SDLK_LEFT:  arrow("\x1b[D", "\x1bOD", 'D'); crt_audio_cursor_ping(); return;
    case SDLK_HOME:  term_write(t, t->app_cursor_keys ? "\x1bOH" : "\x1b[H", 3); crt_audio_cursor_ping(); return;
    case SDLK_END:   term_write(t, t->app_cursor_keys ? "\x1bOF" : "\x1b[F", 3); crt_audio_cursor_ping(); return;
    default: break;
    }

    switch (ks.sym) {
    case SDLK_RETURN:
        term_write(t, "\r", 1);
        break;
    case SDLK_BACKSPACE: term_write(t, "\x7f",    1); break;
    case SDLK_TAB:
        if (shift) term_write(t, "\x1b[Z", 3);
        else       term_write(t, "\t",     1);
        break;
    case SDLK_ESCAPE:    term_write(t, "\x1b",    1); break;
    case SDLK_INSERT:    term_write(t, "\x1b[2~", 4); break;
    case SDLK_DELETE:    term_write(t, "\x1b[3~", 4); break;
    case SDLK_PAGEUP:    term_write(t, "\x1b[5~", 4); break;
    case SDLK_PAGEDOWN:  term_write(t, "\x1b[6~", 4); break;
    case SDLK_F1:   term_write(t, "\x1bOP",   3); break;
    case SDLK_F2:   term_write(t, "\x1bOQ",   3); break;
    case SDLK_F3:   term_write(t, "\x1bOR",   3); break;
    case SDLK_F4:   term_write(t, "\x1bOS",   3); break;
    case SDLK_F5:   term_write(t, "\x1b[15~", 5); break;
    case SDLK_F6:   term_write(t, "\x1b[17~", 5); break;
    case SDLK_F7:   term_write(t, "\x1b[18~", 5); break;
    case SDLK_F8:
        if (!g_ssh_key_mgr.visible)
            ssh_key_mgr_open(0, 0);
        else
            ssh_key_mgr_close();
        break;
    case SDLK_F9:   term_write(t, "\x1b[20~", 5); break;
    case SDLK_F10:  term_write(t, "\x1b[21~", 5); break;
    case SDLK_F11:  term_write(t, "\x1b[23~", 5); break;
    case SDLK_F12:  term_write(t, "\x1b[24~", 5); break;
    default:
        if (ctrl && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {
            char c = (char)(ks.sym - SDLK_a + 1);
            if (alt) term_write(t, "\x1b", 1);
            term_write(t, &c, 1);
        } else if (alt && text && text[0]) {
            term_write(t, "\x1b", 1);
            term_write(t, text, (int)strlen(text));
        } else if (text && text[0]) {
            term_write(t, text, (int)strlen(text));
        }
        break;
    }
}

// ============================================================================
// CONTEXT MENU
// ============================================================================

static void menu_layout(ContextMenu *m, int /*font_size*/) {
    m->item_h = (int)(MENU_FONT_SIZE * 1.8f);
    m->sep_h  = 8;
    m->pad_x  = (int)(MENU_FONT_SIZE * 0.8f);
    m->width  = (int)(MENU_FONT_SIZE * 14.0f);
}

static int menu_total_height(ContextMenu *m) {
    int h = 8;
    for (int i = 0; i < MENU_COUNT; i++)
        h += MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
    return h;
}

void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h) {
    menu_layout(m, 0);
    m->visible = true; m->hovered = -1; m->sub_open = -1; m->sub_hovered = -1;
    int th = menu_total_height(m);
    m->x = SDL_min(x, win_w - m->width - 2);
    m->y = SDL_min(y, win_h - th - 2);
    if (m->x < 0) m->x = 0;
    if (m->y < 0) m->y = 0;
}

int menu_hit(ContextMenu *m, int px, int py) {
    if (!m->visible) return -1;
    if (px < m->x || px > m->x + m->width) return -1;
    int y = m->y + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        int h = MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
        if (!MENU_ITEMS[i].separator && py >= y && py < y + h) return i;
        y += h;
    }
    return -1;
}

int submenu_hit(ContextMenu *m, int px, int py) {
    if (m->sub_open < 0) return -1;
    if (px < m->sub_x || px > m->sub_x + m->sub_w) return -1;
    if (py < m->sub_y || py > m->sub_y + m->sub_h) return -1;
    int count = (m->sub_open == MENU_ID_THEMES)        ? THEME_COUNT :
                (m->sub_open == MENU_ID_RENDER_MODE)    ? RENDER_MODE_COUNT :
                (m->sub_open == MENU_ID_ENTERTAINMENT)  ? ENT_COUNT :
                (m->sub_open == MENU_ID_NEW_TERMINAL)   ? (int)g_available_terminals.size() :
                (m->sub_open == MENU_ID_FONTS)          ? (int)g_font_list.size() :
                                                          OPACITY_COUNT;
    int idx = (py - m->sub_y) / m->item_h;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

static void draw_menu_panel(float mx, float my, float mw, float mh) {
    draw_rect(mx+3, my+3, mw, mh, 0,0,0, 0.35f);
    draw_rect(mx, my, mw, mh, 0.13f, 0.13f, 0.16f, 0.96f);
    draw_rect(mx, my, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my+mh-1, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx+mw-1, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
}

void menu_render(ContextMenu *m) {
    if (!m->visible) return;
    menu_layout(m, 0);
    int th = menu_total_height(m);
    float mx = (float)m->x, my = (float)m->y, mw = (float)m->width;

    draw_menu_panel(mx, my, mw, (float)th);

    float y = my + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (MENU_ITEMS[i].separator) {
            draw_rect(mx+4, y + m->sep_h*0.5f, mw-8, 1, 0.35f,0.35f,0.45f, 1.f);
            y += m->sep_h; continue;
        }
        float ih = (float)m->item_h;
        bool hov = (i == m->hovered);
        bool sub_open = (m->sub_open == i);
        
        // Check if this menu item should be highlighted (active/enabled)
        bool active = false;
        
        if (hov || sub_open) draw_rect(mx+2, y, mw-4, ih, 0.25f, 0.45f, 0.85f, 0.85f);
        else if (active) draw_rect(mx+2, y, mw-4, ih, 0.2f, 0.35f, 0.6f, 0.6f);  // Highlight active toggles
        
        float tr = (hov||sub_open)?1.f:(active?0.7f:0.88f), tg = (hov||sub_open)?1.f:(active?0.9f:0.88f), tb = (hov||sub_open)?1.f:(active?1.0f:0.92f);
        if (active) draw_text_menu("\xe2\x9c\x93", mx+4, y + ih*0.72f, 0.4f,0.8f,0.4f,1.f);  // Checkmark
        draw_text_menu(MENU_ITEMS[i].label, mx + m->pad_x + (active?MENU_FONT_SIZE:0), y + ih*0.72f, tr,tg,tb,1.f);
        y += ih;
    }

    if (m->sub_open == MENU_ID_THEMES || m->sub_open == MENU_ID_OPACITY ||
        m->sub_open == MENU_ID_RENDER_MODE || m->sub_open == MENU_ID_ENTERTAINMENT ||
        m->sub_open == MENU_ID_NEW_TERMINAL || m->sub_open == MENU_ID_FONTS ||
        m->sub_open == MENU_ID_ADV_OPTIONS) {
        int count = (m->sub_open == MENU_ID_THEMES)       ? THEME_COUNT :
                    (m->sub_open == MENU_ID_RENDER_MODE)   ? RENDER_MODE_COUNT :
                    (m->sub_open == MENU_ID_ENTERTAINMENT) ? ENT_COUNT :
                    (m->sub_open == MENU_ID_NEW_TERMINAL)  ? (int)g_available_terminals.size() :
                    (m->sub_open == MENU_ID_FONTS)         ? (int)g_font_list.size() :
                    (m->sub_open == MENU_ID_ADV_OPTIONS)   ? ADV_OPTIONS_COUNT :
                                                             OPACITY_COUNT;
        float sw = (float)(m->width + (int)(MENU_FONT_SIZE * 2));
        float sh = (float)(count * m->item_h + 8);
        float sx = (float)m->sub_x, sy = (float)m->sub_y;
        m->sub_w = (int)sw; m->sub_h = (int)sh;
        draw_menu_panel(sx, sy, sw, sh);

        static const char *ENT_NAMES[]      = { "Fight Mode", "Bouncing Circle", "Sound" };

        for (int j = 0; j < count; j++) {
            const char *lbl = nullptr;
            if (m->sub_open == MENU_ID_THEMES) {
                lbl = THEMES[j].name;
            } else if (m->sub_open == MENU_ID_RENDER_MODE) {
                lbl = RENDER_MODE_NAMES[j];
            } else if (m->sub_open == MENU_ID_ENTERTAINMENT) {
                lbl = ENT_NAMES[j];
            } else if (m->sub_open == MENU_ID_NEW_TERMINAL) {
                lbl = g_available_terminals[j].name.c_str();
            } else if (m->sub_open == MENU_ID_FONTS) {
                lbl = g_font_list[j].display_name.c_str();
            } else if (m->sub_open == MENU_ID_ADV_OPTIONS) {
                lbl = ADV_OPTIONS_NAMES[j];
            } else {
                lbl = OPACITY_NAMES[j];
            }
            float iy = sy + 4 + j * m->item_h, ih = (float)m->item_h;
            bool hov = (j == m->sub_hovered);

            bool active = false;
            if (m->sub_open == MENU_ID_THEMES)
                active = (j == g_theme_idx);
            else if (m->sub_open == MENU_ID_FONTS)
                active = (j == g_font_index);
            else if (m->sub_open == MENU_ID_RENDER_MODE)
                active = (j == RENDER_MODE_NORMAL) ? (g_render_mode == 0)
                                                   : ((g_render_mode & (1u << j)) != 0);
            else if (m->sub_open == MENU_ID_OPACITY)
                active = (fabsf(OPACITY_LEVELS[j] - g_opacity) < 0.01f);
            else if (m->sub_open == MENU_ID_ENTERTAINMENT)
                active = (j == ENT_IDX_FIGHT    && fight_get_enabled())   ||
                         (j == ENT_IDX_BOUNCING  && bc_get_enabled())      ||
                         (j == ENT_IDX_SOUND     && term_audio_get_enabled());
            else if (m->sub_open == MENU_ID_ADV_OPTIONS) {
                if (j == 0) active = g_sticky_prompt_enabled;           // Sticky Prompt
                else if (j == 1) active = !g_autoscroll_enabled;        // Disable Autoscroll
                else if (j == 2) active = g_line_numbers_enabled;       // Line Numbers
            }

            if (hov)          draw_rect(sx+2,iy,sw-4,ih,0.25f,0.45f,0.85f,0.85f);
            if (active&&!hov) draw_rect(sx+2,iy,sw-4,ih,0.2f,0.35f,0.6f,0.6f);
            float tr=hov?1.f:(active?0.7f:0.88f), tg=hov?1.f:(active?0.9f:0.88f), tb=hov?1.f:(active?1.0f:0.92f);
            if (active) draw_text_menu("\xe2\x9c\x93", sx+4, iy+ih*0.72f, 0.4f,0.8f,0.4f,1.f);
            draw_text_menu(lbl, sx + m->pad_x + MENU_FONT_SIZE, iy+ih*0.72f, tr,tg,tb,1.f);
        }
    }

    // Menu is drawn after gl_end_frame (to stay outside post-process), so we
    // must flush the accumulator ourselves — there's no automatic flush after this.
    gl_flush_verts();
}

// ============================================================================
// ACTIONS
// ============================================================================

void action_new_terminal() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(self, nullptr, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, nullptr); _exit(1); }
#endif
}

void action_new_ssh_session() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "\"%s\" --ssh", self);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, "--ssh", nullptr); _exit(1); }
#endif
}

void action_new_telnet_session() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "\"%s\" --telnet", self);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, "--telnet", nullptr); _exit(1); }
#endif
}

void action_new_serial_session() {
#ifdef _WIN32
    char self[512] = {};
    if (!GetModuleFileNameA(nullptr, self, sizeof(self)-1)) return;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "\"%s\" --serial", self);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, "--serial", nullptr); _exit(1); }
#endif
}


// ============================================================================
// TOAST — brief status notification (e.g. F9 web server on/off).
// Top-centre panel: fades in, holds, fades out. Drawn after post-processing
// so CRT/VHS effects never distort it.
// ============================================================================

#define TOAST_FADE_IN_MS   150
#define TOAST_HOLD_MS      1800
#define TOAST_FADE_OUT_MS  450

static std::string s_toast_title, s_toast_sub;
static float       s_toast_r = 1, s_toast_g = 1, s_toast_b = 1;
static Uint32      s_toast_start = 0;
static bool        s_toast_on = false;

static float measure_text_menu(const char *text) {
    ensure_menu_face();
    if (!s_menu_face) return measure_text(text, MENU_FONT_SIZE);
    FT_Face saved = s_ft_face;
    s_ft_face = s_menu_face;
    float w = measure_text(text, MENU_FONT_SIZE);
    s_ft_face = saved;
    return w;
}

void toast_show(const char *title, const char *subtitle, float r, float g, float b) {
    s_toast_title = title ? title : "";
    s_toast_sub   = subtitle ? subtitle : "";
    s_toast_r = r; s_toast_g = g; s_toast_b = b;
    s_toast_start = SDL_GetTicks();
    s_toast_on = true;
}

bool toast_active() {
    if (!s_toast_on) return false;
    if (SDL_GetTicks() - s_toast_start > TOAST_FADE_IN_MS + TOAST_HOLD_MS + TOAST_FADE_OUT_MS)
        s_toast_on = false;
    return s_toast_on;
}

void toast_render(int win_w, int win_h) {
    (void)win_h;
    if (!toast_active()) return;

    Uint32 el = SDL_GetTicks() - s_toast_start;
    float a;
    if (el < TOAST_FADE_IN_MS)
        a = (float)el / TOAST_FADE_IN_MS;
    else if (el < TOAST_FADE_IN_MS + TOAST_HOLD_MS)
        a = 1.f;
    else
        a = 1.f - (float)(el - TOAST_FADE_IN_MS - TOAST_HOLD_MS) / TOAST_FADE_OUT_MS;
    if (a <= 0.f) return;
    // Slide down a few pixels while fading in
    float slide = (el < TOAST_FADE_IN_MS) ? (1.f - a) * -8.f : 0.f;

    const float pad = 14.f, dot = 10.f, gap = 10.f;
    const float line_h = MENU_FONT_SIZE * 1.45f;
    bool  has_sub = !s_toast_sub.empty();
    float tw = measure_text_menu(s_toast_title.c_str());
    float sw = has_sub ? measure_text_menu(s_toast_sub.c_str()) : 0.f;
    float w  = pad + dot + gap + SDL_max(tw, sw) + pad;
    float h  = pad * 0.8f * 2 + line_h * (has_sub ? 2 : 1);
    if (w > win_w - 16) w = (float)(win_w - 16);
    float x = (win_w - w) * 0.5f;
    float y = 18.f + slide;

    // Shadow, body, accent bar in the status colour, thin border
    draw_rect(x + 3, y + 3, w, h, 0, 0, 0, 0.35f * a);
    draw_rect(x, y, w, h, 0.09f, 0.09f, 0.12f, 0.94f * a);
    draw_rect(x, y, 3, h, s_toast_r, s_toast_g, s_toast_b, a);
    draw_rect(x,         y,         w, 1, 0.35f, 0.35f, 0.50f, a);
    draw_rect(x,         y + h - 1, w, 1, 0.35f, 0.35f, 0.50f, a);
    draw_rect(x + w - 1, y,         1, h, 0.35f, 0.35f, 0.50f, a);

    // Status dot, vertically centred on the title line
    float ty = y + pad * 0.8f;
    draw_rect(x + pad, ty + (line_h - dot) * 0.5f, dot, dot, s_toast_r, s_toast_g, s_toast_b, a);

    float tx = x + pad + dot + gap;
    draw_text_menu(s_toast_title.c_str(), tx, ty + line_h * 0.75f,
                   s_toast_r, s_toast_g, s_toast_b, a);
    if (has_sub)
        draw_text_menu(s_toast_sub.c_str(), tx, ty + line_h + line_h * 0.75f,
                       0.82f, 0.82f, 0.88f, a);
}

// ============================================================================
// HELP OVERLAY
// ============================================================================

bool g_help_visible = false;

struct HelpRow {
    const char *header;  // non-null = section header row; key/desc/url ignored
    const char *key;
    const char *desc;
    const char *url;     // non-null = desc is a clickable hyperlink opening this URL
};

static const HelpRow HELP_ROWS[] = {
    { "── General ──",            nullptr, nullptr, nullptr },
    { nullptr, "F1",              "Toggle this help screen",                             nullptr },
    { nullptr, "F5",              "Felix Chirp - image/audio player (SSH or local)",     nullptr },
    { nullptr, "F7",              "WOPR terminal (games and entertainment)",              nullptr },
    { nullptr, "F9",              "Felix Stargate - Web server (SFTP tunnel or local)",  nullptr },
    { nullptr, "F9 Cont.",        "Default port is 53716 F5E3L7I1X6; see F12",           nullptr },
    { nullptr, "F11",             "Toggle fullscreen",                                   nullptr },
    { nullptr, "F12",             "Debug log (mirrors console output)",                  nullptr },
    { nullptr, "Right-click",     "Open context menu",                                   nullptr },
    { nullptr, "Ctrl+A",          "Select all",                                          nullptr },
    { nullptr, "Ctrl+C",          "Copy selection as rich text (keeps colors for Word)", nullptr },
    { nullptr, "Ctrl+Shift+C",    "Copy selection as HTML",                              nullptr },
    { nullptr, "Ctrl+V",          "Paste",                                               nullptr },
    { nullptr, "Ctrl+Scroll",     "Zoom font in / out",                                  nullptr },
    { nullptr, "Shift+PgUp/Dn",   "Scroll through scrollback buffer",                    nullptr },
    { nullptr, "Ctrl+click URL",  "Open hyperlink in browser",                           nullptr },

    { "── SSH ──",                nullptr, nullptr, nullptr },
    { nullptr, "--ssh [user@host]","Connect to a remote host via SSH (see CLI options)", nullptr },
    { nullptr, "F2",              "SFTP upload overlay",                                 nullptr },
    { nullptr, "F3",              "SFTP download overlay",                               nullptr },
    { nullptr, "F4",              "Interactive SFTP console (F9 inside for web server)", nullptr },
    { nullptr, "F6",              "Port forwarding console",                             nullptr },
    { nullptr, "F8",              "SSH Key Manager (generate / copy / delete keys)",     nullptr },
    { "── Serial ──",             nullptr, nullptr, nullptr },
    { nullptr, "--serial [port]", "Open a serial/RS-232 console (/dev/ttyUSB0, COM3)",  nullptr },
    { nullptr, "Baud / settings", "Configured via connection dialog on launch",          nullptr },
    { "── Appearance ──",         nullptr, nullptr, nullptr },
    { nullptr, "Render Mode",     "Post-process visual effects (right-click menu)",      nullptr },
    { nullptr, "Color Theme",     "Built-in color schemes (right-click menu)",          nullptr },
    { nullptr, "Transparency",    "Adjust window opacity (right-click menu)",            nullptr },
    { nullptr, "Font",            "Switch monospace font (right-click menu)",            nullptr },

    { "── Entertainment ──",      nullptr, nullptr, nullptr },
    { nullptr, "Entertainment",   "Fight mode, bouncing ball, CRT audio (right-click menu)", nullptr },

    { "── Project ──",            nullptr, nullptr, nullptr },
    { nullptr, "Source & License","https://github.com/jasonbrianhall/glterminal  (MIT)",
                                   "https://github.com/jasonbrianhall/glterminal" },
    { nullptr, "Buy Me a Coffee", "https://buymeacoffee.com/jasonbrianhall",
                                   "https://buymeacoffee.com/jasonbrianhall"      },
};
static const int HELP_ROW_COUNT = (int)(sizeof(HELP_ROWS)/sizeof(HELP_ROWS[0]));

// Layout constants
#define HELP_KEY_COL_W  (MENU_FONT_SIZE * 15)
#define HELP_DESC_COL_W (MENU_FONT_SIZE * 30)
#define HELP_PAD        (MENU_FONT_SIZE)
#define HELP_ROW_H      (int)(MENU_FONT_SIZE * 1.7f)
#define HELP_HDR_H      (int)(MENU_FONT_SIZE * 2.0f)

// Hit-rects for clickable links — rebuilt each frame
struct HelpLinkRect { int x, y, w, h; const char *url; };
static std::vector<HelpLinkRect> s_help_links;
static int s_help_hovered_link = -1;  // index into s_help_links, or -1

void help_render(int win_w, int win_h) {
    if (!g_help_visible) return;

    ensure_menu_face();
    s_help_links.clear();

    // --- layout ---
    int total_w = HELP_KEY_COL_W + HELP_DESC_COL_W + HELP_PAD * 3;

    int total_h = HELP_PAD + HELP_HDR_H;
    for (int i = 0; i < HELP_ROW_COUNT; i++)
        total_h += HELP_ROWS[i].header ? HELP_HDR_H : HELP_ROW_H;
    total_h += HELP_PAD;

    int ox = (win_w - total_w) / 2;
    int oy = (win_h - total_h) / 2;
    if (ox < 4) ox = 4;
    if (oy < 4) oy = 4;

    // --- dim backdrop ---
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.55f);

    // --- panel shadow + body ---
    draw_rect((float)(ox+3), (float)(oy+3), (float)total_w, (float)total_h, 0,0,0, 0.4f);
    draw_rect((float)ox, (float)oy, (float)total_w, (float)total_h, 0.10f, 0.10f, 0.13f, 0.97f);
    draw_rect((float)ox,             (float)oy,             (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)(oy+total_h-1), (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)(ox+total_w-1), (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);

    // --- title bar ---
    float ty = (float)oy + HELP_PAD;
    draw_rect((float)ox+1, ty, (float)total_w-2, (float)HELP_HDR_H, 0.18f, 0.25f, 0.45f, 1.f);

    const char *title = "Felix Terminal — Help";
    draw_text_menu(title,
                   (float)(ox + total_w/2) - (float)(strlen(title) * MENU_FONT_SIZE * 0.3f),
                   ty + HELP_HDR_H * 0.72f,
                   1.f, 1.f, 1.f, 1.f, ATTR_BOLD);

    const char *hint = "Esc or F1 to close";
    float hint_x = (float)(ox + total_w) - (float)(strlen(hint) * MENU_FONT_SIZE * 0.56f) - HELP_PAD;
    draw_text_menu(hint, hint_x, ty + HELP_HDR_H * 0.72f, 0.50f, 0.50f, 0.62f, 1.f);

    ty += HELP_HDR_H;

    // --- rows ---
    for (int i = 0; i < HELP_ROW_COUNT; i++) {
        const HelpRow &row = HELP_ROWS[i];
        if (row.header) {
            float rh = (float)HELP_HDR_H;
            draw_rect((float)ox+1, ty, (float)total_w-2, rh, 0.14f, 0.14f, 0.20f, 1.f);
            draw_rect((float)ox+1, ty + rh - 1.f, (float)total_w-2, 1.f, 0.28f, 0.28f, 0.42f, 1.f);
            draw_text_menu(row.header, (float)(ox + HELP_PAD), ty + rh * 0.72f,
                           0.55f, 0.75f, 1.0f, 1.f, ATTR_BOLD);
            ty += rh;
        } else {
            float rh = (float)HELP_ROW_H;
            if (i % 2 == 0)
                draw_rect((float)ox+1, ty, (float)total_w-2, rh, 0.f,0.f,0.f, 0.12f);

            // key column
            draw_text_menu(row.key, (float)(ox + HELP_PAD), ty + rh * 0.72f,
                           0.85f, 0.95f, 1.0f, 1.f);

            // desc / link column
            float dx = (float)(ox + HELP_PAD + HELP_KEY_COL_W);
            if (row.url) {
                // Find if this link is hovered
                int link_idx = (int)s_help_links.size();
                bool hov = (s_help_hovered_link == link_idx);

                // Highlight bar on hover
                if (hov)
                    draw_rect(dx - 2.f, ty + 1.f, (float)HELP_DESC_COL_W, rh - 2.f,
                              0.25f, 0.45f, 0.85f, 0.25f);

                float lr = hov ? 0.55f : 0.40f;
                float lg = hov ? 0.85f : 0.65f;
                float lb = 1.0f;
                draw_text_menu(row.desc, dx, ty + rh * 0.72f, lr, lg, lb, 1.f, ATTR_UNDERLINE);

                // Record hit-rect
                HelpLinkRect lr2;
                lr2.x   = (int)dx - 2;
                lr2.y   = (int)ty;
                lr2.w   = HELP_DESC_COL_W;
                lr2.h   = (int)rh;
                lr2.url = row.url;
                s_help_links.push_back(lr2);
            } else {
                draw_text_menu(row.desc, dx, ty + rh * 0.72f, 0.78f, 0.78f, 0.82f, 1.f);
            }
            ty += rh;
        }
    }

    gl_flush_verts();
}

bool help_keydown(SDL_Keycode sym) {
    if (!g_help_visible) return false;
    if (sym == SDLK_ESCAPE || sym == SDLK_F1) {
        g_help_visible = false;
        return true;
    }
    return true;  // swallow all keys while help is open
}

// Returns true if the mouse event was consumed.
bool help_mousedown(int x, int y) {
    if (!g_help_visible) return false;
    for (auto &lr : s_help_links) {
        if (x >= lr.x && x < lr.x + lr.w && y >= lr.y && y < lr.y + lr.h) {
            open_url(lr.url);
            return true;
        }
    }
    // Click outside a link closes the overlay
    g_help_visible = false;
    return true;
}

// Call from SDL_MOUSEMOTION to update hover state. Returns true if redraw needed.
bool help_mousemotion(int x, int y) {
    if (!g_help_visible) return false;
    int prev = s_help_hovered_link;
    s_help_hovered_link = -1;
    for (int i = 0; i < (int)s_help_links.size(); i++) {
        auto &lr = s_help_links[i];
        if (x >= lr.x && x < lr.x + lr.w && y >= lr.y && y < lr.y + lr.h) {
            s_help_hovered_link = i;
            break;
        }
    }
    return s_help_hovered_link != prev;
}

// ============================================================================
// DEBUG LOG OVERLAY (F12) — mirrors everything SDL_Log() prints to console
// ============================================================================

bool g_debuglog_visible = false;

#define DEBUGLOG_MAX_LINES 2000

static std::deque<std::string> s_debuglog_lines;
// NOTE: was std::mutex, but on Windows the Debug STL's lazy Mtx_lock init
// path was crashing with a null internal handle on the very first lock in
// the process (confirmed via ASan: access-violation, not heap corruption --
// the mutex object itself is at a valid address, its internal handle is
// just zero when Mtx_lock dereferences it as if already initialized).
// A raw CRITICAL_SECTION is eagerly initialized at construction with no
// lazy-init step, sidestepping whatever that toolset's bug is. CRITICAL_SECTION
// is Windows-only, so non-Windows builds fall back to a pthread mutex, which
// is likewise eagerly initialized (via PTHREAD_MUTEX_INITIALIZER).
#ifdef _WIN32
struct DebuglogLock {
    CRITICAL_SECTION cs;
    DebuglogLock()  { InitializeCriticalSection(&cs); }
    ~DebuglogLock() { DeleteCriticalSection(&cs); }
    void lock()   { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
};
#else
#include <pthread.h>
struct DebuglogLock {
    pthread_mutex_t cs = PTHREAD_MUTEX_INITIALIZER;
    void lock()   { pthread_mutex_lock(&cs); }
    void unlock() { pthread_mutex_unlock(&cs); }
};
#endif
static DebuglogLock            s_debuglog_mutex;
static SDL_LogOutputFunction   s_debuglog_prev_fn = nullptr;
static void                   *s_debuglog_prev_ud  = nullptr;
static int                     s_debuglog_scroll   = 0;  // lines back from bottom

static void debuglog_capture(void *userdata, int category, SDL_LogPriority priority,
                              const char *message) {
    if (s_debuglog_prev_fn)
        s_debuglog_prev_fn(s_debuglog_prev_ud, category, priority, message);

    s_debuglog_mutex.lock();
    s_debuglog_lines.emplace_back(message);
    while (s_debuglog_lines.size() > DEBUGLOG_MAX_LINES)
        s_debuglog_lines.pop_front();
    s_debuglog_mutex.unlock();
}

void debuglog_init() {
    SDL_LogGetOutputFunction(&s_debuglog_prev_fn, &s_debuglog_prev_ud);
    SDL_LogSetOutputFunction(debuglog_capture, nullptr);
}

#define DEBUGLOG_ROW_H (int)(MENU_FONT_SIZE * 1.4f)
#define DEBUGLOG_PAD   (MENU_FONT_SIZE)

void debuglog_render(int win_w, int win_h) {
    if (!g_debuglog_visible) return;
    ensure_menu_face();

    int total_w = win_w - DEBUGLOG_PAD * 4;
    int total_h = win_h - DEBUGLOG_PAD * 4;
    int ox = DEBUGLOG_PAD * 2;
    int oy = DEBUGLOG_PAD * 2;

    draw_rect(0, 0, (float)win_w, (float)win_h, 0.f, 0.f, 0.f, 0.55f);
    draw_rect((float)(ox+3), (float)(oy+3), (float)total_w, (float)total_h, 0,0,0, 0.4f);
    draw_rect((float)ox, (float)oy, (float)total_w, (float)total_h, 0.08f, 0.08f, 0.10f, 0.97f);
    draw_rect((float)ox,             (float)oy,             (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)(oy+total_h-1), (float)total_w, 1,              0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)ox,             (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);
    draw_rect((float)(ox+total_w-1), (float)oy,             1,              (float)total_h, 0.35f,0.35f,0.55f, 1.f);

    int hdr_h = (int)(MENU_FONT_SIZE * 2.0f);
    draw_rect((float)ox+1, (float)oy, (float)total_w-2, (float)hdr_h, 0.18f, 0.25f, 0.45f, 1.f);
    const char *title = "Felix Terminal — Debug Log";
    draw_text_menu(title, (float)(ox + DEBUGLOG_PAD), (float)oy + hdr_h * 0.72f,
                   1.f, 1.f, 1.f, 1.f, ATTR_BOLD);
    const char *hint = "Esc or F12 to close  \xe2\x80\xa2  PgUp/PgDn or wheel to scroll";
    float hint_x = (float)(ox + total_w) - (float)(strlen(hint) * MENU_FONT_SIZE * 0.56f) - DEBUGLOG_PAD;
    draw_text_menu(hint, hint_x, (float)oy + hdr_h * 0.72f, 0.50f, 0.50f, 0.62f, 1.f);

    int content_top = oy + hdr_h;
    int content_h   = total_h - hdr_h - DEBUGLOG_PAD;
    int max_rows    = content_h / DEBUGLOG_ROW_H;
    if (max_rows < 1) max_rows = 1;

    s_debuglog_mutex.lock();
    int count = (int)s_debuglog_lines.size();
    int max_scroll = count > max_rows ? count - max_rows : 0;
    if (s_debuglog_scroll > max_scroll) s_debuglog_scroll = max_scroll;
    if (s_debuglog_scroll < 0) s_debuglog_scroll = 0;

    int end   = count - s_debuglog_scroll;
    int start = end - max_rows;
    if (start < 0) start = 0;

    float ty = (float)content_top;
    for (int i = start; i < end; i++) {
        const std::string &line = s_debuglog_lines[i];
        float r = 0.80f, g = 0.82f, b = 0.85f;
        if (line.find("ERROR") != std::string::npos)
            { r = 1.0f; g = 0.45f; b = 0.45f; }
        else if (line.find("WARNING") != std::string::npos || line.find("WARN") != std::string::npos)
            { r = 1.0f; g = 0.85f; b = 0.40f; }
        draw_text_menu(line.c_str(), (float)(ox + DEBUGLOG_PAD), ty + DEBUGLOG_ROW_H * 0.72f,
                       r, g, b, 1.f);
        ty += DEBUGLOG_ROW_H;
    }
    s_debuglog_mutex.unlock();

    gl_flush_verts();
}

bool debuglog_keydown(SDL_Keycode sym) {
    if (!g_debuglog_visible) return false;
    if (sym == SDLK_ESCAPE || sym == SDLK_F12) { g_debuglog_visible = false; return true; }
    if (sym == SDLK_PAGEUP)   { s_debuglog_scroll += 10; return true; }
    if (sym == SDLK_PAGEDOWN) { s_debuglog_scroll = SDL_max(0, s_debuglog_scroll - 10); return true; }
    if (sym == SDLK_UP)       { s_debuglog_scroll += 1; return true; }
    if (sym == SDLK_DOWN)     { s_debuglog_scroll = SDL_max(0, s_debuglog_scroll - 1); return true; }
    return true;  // swallow all keys while open
}

void debuglog_scroll(int dy) {
    if (!g_debuglog_visible) return;
    s_debuglog_scroll = SDL_max(0, s_debuglog_scroll + dy * 3);
}

// ============================================================================
// CUSTOM SHELL DIALOG
// ============================================================================

void custom_shell_dialog_render(int win_w, int win_h) {
    if (!g_custom_shell_dialog_open) return;
    
    // Draw semi-transparent overlay
    draw_rect(0, 0, (float)win_w, (float)win_h, 0, 0, 0, 0.5f);
    
    // Draw dialog box
    float dialog_w = 400, dialog_h = 100;
    float dialog_x = (win_w - dialog_w) / 2;
    float dialog_y = (win_h - dialog_h) / 2;
    
    draw_menu_panel(dialog_x, dialog_y, dialog_w, dialog_h);
    
    // Draw prompt text
    draw_text_menu("Enter command:", 
                   dialog_x + 15, dialog_y + 15, 0.9f, 0.9f, 0.9f, 1.f);
    
    // Draw input field
    float input_y = dialog_y + 32;
    float input_h = 25;
    draw_rect(dialog_x + 10, input_y, dialog_w - 20, input_h, 0.2f, 0.2f, 0.2f, 0.8f);
    draw_rect(dialog_x + 10, input_y, dialog_w - 20, input_h, 0.5f, 0.5f, 0.5f, 0.3f);
    
    // Draw input text (vertically centered in field: baseline at center)
    draw_text_menu(g_custom_shell_input.c_str(), dialog_x + 15, input_y + input_h * 0.6f, 
                   0.9f, 0.9f, 0.9f, 1.f);
    
    // Draw instructions
    draw_text_menu("(Enter to launch, Esc to cancel)", 
                   dialog_x + 15, dialog_y + 75, 0.6f, 0.6f, 0.6f, 1.f);
}

bool custom_shell_dialog_keydown(SDL_Keycode sym, const char *text) {
    if (!g_custom_shell_dialog_open) return false;
    
    if (sym == SDLK_ESCAPE) {
        g_custom_shell_dialog_open = false;
        g_custom_shell_input.clear();
        SDL_StopTextInput();
        return true;
    }
    
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (!g_custom_shell_input.empty()) {
            // Check if command exists
#ifdef _WIN32
            std::string check_cmd = "where " + g_custom_shell_input + " >nul 2>&1";
#else
            std::string check_cmd = "which " + g_custom_shell_input + " >/dev/null 2>&1";
#endif
            int cmd_exists = system(check_cmd.c_str());
            
            if (cmd_exists == 0) {
                // Command found, launch it
                launch_terminal_with_shell(g_custom_shell_input.c_str());
                g_custom_shell_dialog_open = false;
                g_custom_shell_input.clear();
                SDL_StopTextInput();
            } else {
                // Command not found - show error by clearing input and prepending error text
                g_custom_shell_input = "Command not found!";
                // Will be cleared on next key press
            }
        }
        return true;
    }
    
    if (sym == SDLK_BACKSPACE) {
        if (!g_custom_shell_input.empty()) {
            g_custom_shell_input.pop_back();
        }
        return true;
    }
    
    // Handle regular text input
    if (text && text[0]) {
        // Clear error message on first keystroke
        if (g_custom_shell_input == "Command not found!") {
            g_custom_shell_input.clear();
        }
        g_custom_shell_input += text;
        return true;
    }
    
    // When dialog is open, consume all other keys too (don't let them reach terminal)
    return true;
}
