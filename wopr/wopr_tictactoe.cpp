// wopr_tictactoe.cpp — WOPR Tic-Tac-Toe sub-game
//
// Player is X, WOPR is O.  Minimax AI — unbeatable unless player finds the draw.
// Controls: arrow keys / WASD to move cursor, ENTER/SPACE to place, or mouse click.
// 0-player mode: WOPR X vs WOPR O, auto-plays with think delays.
//
// Visual design:
//   - Board scales to fill the available window space
//   - X drawn as two thick crossing lines (cyan/teal)
//   - O drawn as a thick circle approximated by 16-segment poly (amber/gold)
//   - Winning line animates across the board
//   - Hover highlight on mouse-over, cursor highlight for keyboard nav
//   - Grid lines drawn as rects with rounded-corner illusion via slight overlap

#include "wopr.h"
#include <string.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <algorithm>
#include "wopr_render.h"

// ─── Screen / mode ────────────────────────────────────────────────────────

enum TttScreen { TTT_LOBBY, TTT_SIDE_SELECT, TTT_GAME };

// ─── State ────────────────────────────────────────────────────────────────

struct TttState {
    TttScreen screen;
    int       lobby_sel;
    int       side_sel;    // 0=X, 1=O (selection index in side-select screen)
    int       player_side; // 1=X, -1=O

    int  board[9];   // 0=empty, 1=X, -1=O
    int  cursor;     // 0-8, keyboard nav
    int  result;     // 0=playing, 1=X wins, -1=O wins, 2=draw
    bool wopr_turn;
    int  num_players;
    double ai_delay;
    double min_think_ms;
    int    game_count;
    int    stop_at_game;
    char   message[96];

    // Winning line (set when result != 0 and != draw)
    int  win_line[3];     // indices of the three winning cells (-1 if none)
    bool has_win_line;

    // Win line draw animation
    double win_anim;      // 0..1, grows to 1 over ~0.35s

    // Board pixel geometry (set by render, read by mouse)
    float board_x, board_y;     // top-left of the 3x3 grid
    float cell_size;            // square cell dimension in pixels
    float grid_thick;           // grid line thickness

    // Mouse hover cell (0-8, or -1)
    int hover_cell;
};

static TttState *ttt(WoprState *w) { return (TttState *)w->sub_state; }

// ─── Minimax ──────────────────────────────────────────────────────────────

static int ttt_winner(const int board[9]) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto &l : lines) {
        int s = board[l[0]] + board[l[1]] + board[l[2]];
        if (s ==  3) return  1;
        if (s == -3) return -1;
    }
    return 0;
}

// Returns the winning line indices into win_out[3], or false if none
static bool ttt_winner_line(const int board[9], int win_out[3]) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto &l : lines) {
        int s = board[l[0]] + board[l[1]] + board[l[2]];
        if (s == 3 || s == -3) {
            win_out[0] = l[0]; win_out[1] = l[1]; win_out[2] = l[2];
            return true;
        }
    }
    return false;
}

static bool ttt_full(const int board[9]) {
    for (int i = 0; i < 9; i++) if (!board[i]) return false;
    return true;
}

static int minimax_x(int board[9], int side, int depth) {
    int w = ttt_winner(board);
    if (w) return w * (10 - depth);
    if (ttt_full(board)) return 0;
    int best = (side == 1) ? INT_MIN : INT_MAX;
    for (int i = 0; i < 9; i++) {
        if (!board[i]) {
            board[i] = side;
            int val = minimax_x(board, -side, depth + 1);
            board[i] = 0;
            if (side == 1) best = val > best ? val : best;
            else           best = val < best ? val : best;
        }
    }
    return best;
}

static int ai_best_move(const int board[9], int side) {
    int tmp[9]; memcpy(tmp, board, sizeof(tmp));
    int best_val = (side == 1) ? INT_MIN : INT_MAX;
    int candidates[9], ncand = 0;
    for (int i = 0; i < 9; i++) {
        if (!tmp[i]) {
            tmp[i] = side;
            int val = minimax_x(tmp, -side, 1);
            tmp[i] = 0;
            bool better = (side == 1) ? (val > best_val) : (val < best_val);
            if (better) { best_val = val; ncand = 0; candidates[ncand++] = i; }
            else if (val == best_val) candidates[ncand++] = i;
        }
    }
    if (ncand == 0) return -1;
    if (ncand == 1) return candidates[0];
    return candidates[rand() % (unsigned)ncand];
}

// ─── Game helpers ─────────────────────────────────────────────────────────

static void reset_game(TttState *s) {
    memset(s->board, 0, sizeof(s->board));
    s->cursor      = 4;
    s->result      = 0;
    s->wopr_turn   = false;
    s->ai_delay    = 0.0;
    s->has_win_line = false;
    s->win_anim    = 0.0;
    s->hover_cell  = -1;

    if (s->num_players == 0) {
        s->game_count++;
        s->min_think_ms *= 0.80;
        if (s->min_think_ms < 50.0) s->min_think_ms = 0.0;

        if (s->game_count >= s->stop_at_game) {
            s->result    = 2;
            s->wopr_turn = false;
            strcpy(s->message, "A STRANGE GAME.  THE ONLY WINNING MOVE IS NOT TO PLAY.");
            wopr_say(s->message);
            return;
        }

        strcpy(s->message, "WOPR X VS WOPR O.  X CALCULATING...");
        s->wopr_turn = true;
        s->ai_delay  = s->min_think_ms / 1000.0 + (rand() % 40) / 1000.0;
    } else {
        // If player chose O, WOPR plays X and goes first
        if (s->player_side == -1) {
            strcpy(s->message, "WOPR (X) GOES FIRST.  CALCULATING...");
            s->wopr_turn = true;
            s->ai_delay  = 0.6 + (rand() % 80) / 1000.0;
        } else {
            strcpy(s->message, "YOUR MOVE, PROFESSOR.");
        }
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_ttt_enter(WoprState *w) {
    srand((unsigned)time(nullptr));
    TttState *s = new TttState{};
    s->screen        = TTT_LOBBY;
    s->lobby_sel     = 1;
    s->side_sel      = 0;    // default to X
    s->player_side   = 1;    // default to X
    s->min_think_ms  = 800.0;
    s->game_count    = 0;
    s->stop_at_game  = 50 + rand() % 51;
    s->win_line[0] = s->win_line[1] = s->win_line[2] = -1;
    s->hover_cell    = -1;
    s->board_x = s->board_y = s->cell_size = s->grid_thick = 0.f;
    w->sub_state = s;
}

void wopr_ttt_free(WoprState *w) {
    delete ttt(w);
    w->sub_state = nullptr;
}

void wopr_ttt_update(WoprState *w, double dt) {
    TttState *s = ttt(w);
    if (!s || s->screen != TTT_GAME) return;

    // Animate win line
    if (s->has_win_line && s->win_anim < 1.0) {
        s->win_anim += dt / 0.35;
        if (s->win_anim > 1.0) s->win_anim = 1.0;
    }

    // 0-player: auto-restart after terminal state pause
    if (s->result && s->num_players == 0 && s->wopr_turn) {
        s->ai_delay -= dt;
        if (s->ai_delay <= 0) reset_game(s);
        return;
    }

    if (s->result) return;

    if (s->wopr_turn) {
        s->ai_delay -= dt;
        if (s->ai_delay <= 0) {
            int nx = 0, no_ = 0;
            for (int i = 0; i < 9; i++) {
                if (s->board[i] ==  1) nx++;
                if (s->board[i] == -1) no_++;
            }
            int side = (nx == no_) ? 1 : -1;

            int mv = ai_best_move(s->board, side);
            if (mv >= 0) {
                s->board[mv] = side;
                int res = ttt_winner(s->board);
                if (res) {
                    s->result = res;
                    s->has_win_line = ttt_winner_line(s->board, s->win_line);
                    s->win_anim = 0.0;
                    if (s->num_players == 0) {
                        s->wopr_turn = true;
                        s->ai_delay  = (s->min_think_ms > 0.0)
                                       ? s->min_think_ms / 1000.0 * 2.0 : 0.4;
                        const char *winner = (res == 1) ? "X" : "O";
                        char buf[96];
                        snprintf(buf, sizeof(buf), "WOPR %s WINS.  RESTARTING...", winner);
                        strcpy(s->message, buf);
                    } else {
                        // res==player_side means player won, else WOPR won
                        if (res == s->player_side)
                            { strcpy(s->message, "YOU WIN.  UNEXPECTED OUTCOME."); wopr_say(s->message); }
                        else
                            { strcpy(s->message, "WOPR WINS.  INTERESTING GAME."); wopr_say(s->message); }
                    }
                } else if (ttt_full(s->board)) {
                    s->result = 2;
                    if (s->num_players == 0) {
                        s->wopr_turn = true;
                        s->ai_delay  = (s->min_think_ms > 0.0)
                                       ? s->min_think_ms / 1000.0 * 2.0 : 0.4;
                        strcpy(s->message, "DRAW.  RESTARTING...");
                    } else {
                        { strcpy(s->message, "DRAW.  SHALL WE PLAY AGAIN?"); wopr_say(s->message); }
                    }
                } else {
                    if (s->num_players == 0) {
                        int next_side = (side == 1) ? -1 : 1;
                        const char *nm = (next_side == 1) ? "X" : "O";
                        char buf[96];
                        snprintf(buf, sizeof(buf), "WOPR %s CALCULATING...", nm);
                        strcpy(s->message, buf);
                        s->ai_delay = s->min_think_ms / 1000.0 + (rand() % 40) / 1000.0;
                    } else {
                        strcpy(s->message, "YOUR MOVE, PROFESSOR.");
                        s->wopr_turn = false;
                    }
                }
            } else {
                s->wopr_turn = false;
            }
        }
    }
}

// ─── Draw helpers ─────────────────────────────────────────────────────────
// All coordinates are in screen-space pixels (Y=0 at top).
// We use gl_draw_rect for all geometry — thick lines drawn as thin rects,
// X arms as two rotated rects approximated as axis-aligned rects (plus
// two diagonal ones via multiple thin horizontal slices).

// Draw a thick line from (x1,y1) to (x2,y2) as a chain of axis-aligned rects.
// This is a simple DDA approach — works perfectly for the lines we need.
static void draw_line_thick(float x1, float y1, float x2, float y2,
                             float thick,
                             float r, float g, float b, float a)
{
    // Decompose into axis-aligned strips by walking along the major axis
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1.f) return;

    // Unit perpendicular
    float px = -dy / len, py = dx / len;
    float half = thick * 0.5f;

    // Number of steps along the line
    int steps = (int)(len) + 1;
    for (int i = 0; i <= steps; i++) {
        float t  = (float)i / (float)steps;
        float cx = x1 + dx * t;
        float cy = y1 + dy * t;
        // Draw a 1px wide rect perpendicular to the line
        float sx = cx + px * (-half);
        float sy = cy + py * (-half);
        float ex = cx + px * half;
        float ey = cy + py * half;
        // Swap so sx<=ex, sy<=ey
        if (sx > ex) { float tmp=sx; sx=ex; ex=tmp; }
        if (sy > ey) { float tmp=sy; sy=ey; ey=tmp; }
        float w = ex - sx; if (w < 1.f) w = 1.f;
        float h = ey - sy; if (h < 1.f) h = 1.f;
        gl_draw_rect(sx, sy, w, h, r, g, b, a);
    }
}

// Draw an X in a cell (cx,cy = top-left of cell, size = cell dimension)
static void draw_x(float cx, float cy, float size,
                   float r, float g, float b, float alpha)
{
    float pad   = size * 0.18f;
    float thick = size * 0.12f;
    float x0 = cx + pad, y0 = cy + pad;
    float x1 = cx + size - pad, y1 = cy + size - pad;
    draw_line_thick(x0, y0, x1, y1, thick, r, g, b, alpha);
    draw_line_thick(x1, y0, x0, y1, thick, r, g, b, alpha);
}

// Draw an O in a cell using a filled-ring approach: draw N radial rectangles
static void draw_o(float cx, float cy, float size,
                   float r, float g, float b, float alpha)
{
    float pad   = size * 0.15f;
    float outer = (size - 2.f * pad) * 0.5f;
    float thick = size * 0.11f;
    float inner = outer - thick;
    float ocx   = cx + size * 0.5f;
    float ocy   = cy + size * 0.5f;

    // Approximate the ring with 32 thin rectangular slices
    int N = 48;
    for (int i = 0; i < N; i++) {
        float a0 = (float)i     / (float)N * 2.f * 3.14159265f;
        float a1 = (float)(i+1) / (float)N * 2.f * 3.14159265f;

        // Midpoint of this arc segment
        float am = (a0 + a1) * 0.5f;
        float cos_m = cosf(am), sin_m = sinf(am);

        // Four corners of the ring slice
        float ix0 = ocx + inner * cosf(a0), iy0 = ocy + inner * sinf(a0);
        float ox0 = ocx + outer * cosf(a0), oy0 = ocy + outer * sinf(a0);
        float ix1 = ocx + inner * cosf(a1), iy1 = ocy + inner * sinf(a1);
        float ox1 = ocx + outer * cosf(a1), oy1 = ocy + outer * sinf(a1);

        // Bounding rect of the four corners
        float rx0 = std::min({ix0,ox0,ix1,ox1});
        float ry0 = std::min({iy0,oy0,iy1,oy1});
        float rx1 = std::max({ix0,ox0,ix1,ox1});
        float ry1 = std::max({iy0,oy0,iy1,oy1});
        float rw = rx1 - rx0; if (rw < 1.f) rw = 1.f;
        float rh = ry1 - ry0; if (rh < 1.f) rh = 1.f;

        gl_draw_rect(rx0, ry0, rw, rh, r, g, b, alpha);
        (void)cos_m; (void)sin_m;
    }
}

// ─── Render ───────────────────────────────────────────────────────────────

void wopr_ttt_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    TttState *s = ttt(w);
    if (!s) return;

    float scale = 1.0f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;
    (void)cols;

    // ── Lobby ─────────────────────────────────────────────────────────────
    if (s->screen == TTT_LOBBY) {
        gl_draw_text("TIC-TAC-TOE  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;
        gl_draw_text("HOW MANY PLAYERS?",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "0  --  WOPR VS WOPR", "1  --  PLAYER VS WOPR" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->lobby_sel == i);
            float lg = sel ? 1.0f : 0.4f, lb = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i*fch*2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw*3, y0 + i*fch*2.5f, 0.f, lg, lb, 1.f, scale);
        }
        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=START  ESC=MENU",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Side Select ───────────────────────────────────────────────────────
    if (s->screen == TTT_SIDE_SELECT) {
        gl_draw_text("TIC-TAC-TOE  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;
        gl_draw_text("CHOOSE YOUR SIDE.",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "X  --  YOU GO FIRST", "O  --  WOPR GOES FIRST" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->side_sel == i);
            float lg = sel ? 1.0f : 0.4f, lb = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i*fch*2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw*3, y0 + i*fch*2.5f, 0.f, lg, lb, 1.f, scale);
        }
        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=CONFIRM  ESC=BACK",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Game ──────────────────────────────────────────────────────────────

    // Get window size for board sizing
    int win_w = 800, win_h = 600;
    {
        extern SDL_Window *g_sdl_window;
        if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &win_w, &win_h);
    }

    char title_buf[64];
    const char *title;
    if (s->num_players == 0) {
        title = "TIC-TAC-TOE  --  WOPR VS WOPR";
    } else {
        const char *ps = (s->player_side == 1) ? "X" : "O";
        const char *ws = (s->player_side == 1) ? "O" : "X";
        snprintf(title_buf, sizeof(title_buf),
                 "TIC-TAC-TOE  --  YOU (%s) vs WOPR (%s)", ps, ws);
        title = title_buf;
    }
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 2.0f;

    // Board: square, fills most of available space
    float avail_h = (float)win_h - y0 - fch * 5.f;
    float avail_w = (float)win_w - x0 - fcw * 4.f;
    float board_sz = std::min(avail_h, avail_w);
    board_sz = std::max(board_sz, 120.f);
    float cell  = board_sz / 3.f;
    float thick = std::max(3.f, cell * 0.045f);   // grid line thickness

    // Center the board
    float bx = x0 + (avail_w - board_sz) * 0.5f + fcw * 2.f;
    float by = y0;

    // Store geometry for mouse
    s->board_x   = bx;
    s->board_y   = by;
    s->cell_size = cell;
    s->grid_thick = thick;

    // ── Board background ──────────────────────────────────────────────────
    // Dark semi-transparent backing
    gl_draw_rect(bx, by, board_sz, board_sz, 0.04f, 0.06f, 0.08f, 0.92f);

    // ── Cell highlights ───────────────────────────────────────────────────
    for (int i = 0; i < 9; i++) {
        int row = i / 3, col = i % 3;
        float cx = bx + col * cell, cy = by + row * cell;

        bool is_cursor  = (s->num_players == 1 && s->cursor == i
                           && !s->result && !s->wopr_turn);
        bool is_hover   = (s->hover_cell == i && !s->result && !s->wopr_turn
                           && s->num_players == 1 && s->board[i] == 0);
        bool in_win     = (s->has_win_line &&
                           (s->win_line[0]==i || s->win_line[1]==i || s->win_line[2]==i));

        if (in_win && s->win_anim > 0.f) {
            // Winning cells: golden glow
            float wa = std::min(s->win_anim, 1.0) * 0.35f;
            gl_draw_rect(cx+thick, cy+thick,
                         cell-2*thick, cell-2*thick,
                         0.8f, 0.65f, 0.05f, (float)wa);
        } else if (is_hover) {
            gl_draw_rect(cx+thick, cy+thick,
                         cell-2*thick, cell-2*thick,
                         0.1f, 0.5f, 0.45f, 0.25f);
        } else if (is_cursor) {
            gl_draw_rect(cx+thick, cy+thick,
                         cell-2*thick, cell-2*thick,
                         0.05f, 0.7f, 0.35f, 0.3f);
        }
    }

    // ── Grid lines ────────────────────────────────────────────────────────
    // Colour: muted phosphor green
    float gr = 0.05f, gg = 0.55f, gb = 0.25f, ga = 0.85f;

    // Two vertical lines
    for (int i = 1; i < 3; i++) {
        float lx = bx + i * cell - thick * 0.5f;
        gl_draw_rect(lx, by, thick, board_sz, gr, gg, gb, ga);
    }
    // Two horizontal lines
    for (int i = 1; i < 3; i++) {
        float ly = by + i * cell - thick * 0.5f;
        gl_draw_rect(bx, ly, board_sz, thick, gr, gg, gb, ga);
    }

    // ── Pieces ────────────────────────────────────────────────────────────
    for (int i = 0; i < 9; i++) {
        int row = i/3, col = i%3;
        float cx = bx + col * cell;
        float cy = by + row * cell;
        int val  = s->board[i];

        if (val == 1) {
            // X — cyan/teal
            float alpha = 1.0f;
            // Dim non-winning pieces when game over and we have a winner
            if (s->result && s->result != 2 && s->has_win_line) {
                bool in_win = (s->win_line[0]==i || s->win_line[1]==i || s->win_line[2]==i);
                if (!in_win) alpha = 0.35f;
            }
            draw_x(cx, cy, cell, 0.15f, 0.9f, 0.85f, alpha);
        } else if (val == -1) {
            // O — amber/gold
            float alpha = 1.0f;
            if (s->result && s->result != 2 && s->has_win_line) {
                bool in_win = (s->win_line[0]==i || s->win_line[1]==i || s->win_line[2]==i);
                if (!in_win) alpha = 0.35f;
            }
            draw_o(cx, cy, cell, 0.95f, 0.70f, 0.10f, alpha);
        } else if (i == s->cursor && s->num_players == 1
                   && !s->result && !s->wopr_turn) {
            // Keyboard cursor on empty cell — faint piece preview
            if (s->player_side == 1)
                draw_x(cx, cy, cell, 0.1f, 0.5f, 0.45f, 0.25f);
            else
                draw_o(cx, cy, cell, 0.6f, 0.45f, 0.05f, 0.25f);
        }
    }

    // ── Winning line stroke ────────────────────────────────────────────────
    if (s->has_win_line && s->win_anim > 0.0) {
        int a = s->win_line[0], c2 = s->win_line[2];
        int ra = a/3, ca = a%3;
        int rc = c2/3, cc2 = c2%3;
        // Center of first and last winning cell
        float sx = bx + ca  * cell + cell * 0.5f;
        float sy = by + ra  * cell + cell * 0.5f;
        float ex = bx + cc2 * cell + cell * 0.5f;
        float ey = by + rc  * cell + cell * 0.5f;
        // Interpolate the stroke end by win_anim
        float ex2 = sx + (ex - sx) * (float)s->win_anim;
        float ey2 = sy + (ey - sy) * (float)s->win_anim;
        float lthick = thick * 2.2f;

        // White stroke with slight glow — draw twice: wide+dim then narrow+bright
        draw_line_thick(sx, sy, ex2, ey2, lthick * 1.8f,
                        1.f, 1.f, 0.7f, 0.18f);
        draw_line_thick(sx, sy, ex2, ey2, lthick,
                        1.f, 0.95f, 0.5f, 0.85f);
    }

    // ── Status text ───────────────────────────────────────────────────────
    float my = by + board_sz + fch * 1.2f;

    // Result flash
    float mr = 0.f, mg = 1.f, mb = 0.6f;
    if (s->result == 1)       { mr = 0.15f; mg = 0.9f; mb = 0.85f; }  // X wins — cyan
    else if (s->result == -1) { mr = 0.95f; mg = 0.70f; mb = 0.1f; }  // O wins — amber
    else if (s->result == 2)  { mr = 0.7f;  mg = 0.7f;  mb = 0.7f; }  // draw — grey

    gl_draw_text(s->message, x0, my, mr, mg, mb, 1.f, scale);
    my += fch * 1.5f;

    if (s->result) {
        if (s->num_players == 0) {
            if ((SDL_GetTicks() / 500) % 2 == 0)
                gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.3f, 0.1f, 1.f, scale);
        } else {
            gl_draw_text("ENTER/CLICK=REMATCH   R=LOBBY   ESC=MENU",
                         x0, my, 0.f, 0.55f, 0.2f, 1.f, scale);
        }
    } else if (s->num_players == 0) {
        if ((SDL_GetTicks() / 500) % 2 == 0)
            gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.3f, 0.1f, 1.f, scale);
    } else if (s->wopr_turn) {
        if ((SDL_GetTicks() / 400) % 2 == 0)
            gl_draw_text("WOPR IS THINKING...", x0, my, 0.8f, 0.6f, 0.f, 1.f, scale);
    } else {
        gl_draw_text("CLICK=PLACE   ARROWS=MOVE   ENTER=CONFIRM   ESC=MENU",
                     x0, my, 0.f, 0.45f, 0.15f, 1.f, scale);
    }
}

// ─── Mouse ────────────────────────────────────────────────────────────────

static int pixel_to_cell(TttState *s, int px, int py) {
    if (s->cell_size <= 0.f) return -1;
    float rx = (float)px - s->board_x;
    float ry = (float)py - s->board_y;
    int col = (int)(rx / s->cell_size);
    int row = (int)(ry / s->cell_size);
    if (col < 0 || col > 2 || row < 0 || row > 2) return -1;
    return row * 3 + col;
}

static void ttt_try_place(TttState *s, int idx) {
    if (idx < 0 || idx > 8) return;
    if (s->board[idx] != 0) return;
    s->cursor    = idx;
    s->board[idx] = s->player_side;
    int res = ttt_winner(s->board);
    if (res == s->player_side) {
        s->result = res;
        s->has_win_line = ttt_winner_line(s->board, s->win_line);
        s->win_anim = 0.0;
        { strcpy(s->message, "YOU WIN.  UNEXPECTED OUTCOME."); wopr_say(s->message); }
    } else if (ttt_full(s->board)) {
        s->result = 2;
        { strcpy(s->message, "DRAW.  SHALL WE PLAY AGAIN?"); wopr_say(s->message); }
    } else {
        strcpy(s->message, "WOPR CALCULATING...");
        s->wopr_turn = true;
        s->ai_delay  = 0.6 + (rand() % 80) / 1000.0;
    }
}

void wopr_ttt_mousedown(WoprState *w, int x, int y, int button) {
    TttState *s = ttt(w);
    if (!s || button != SDL_BUTTON_LEFT) return;

    if (s->screen == TTT_LOBBY) {
        s->num_players = s->lobby_sel;
        if (s->num_players == 1) {
            // Go to side-select for 1-player
            s->screen = TTT_SIDE_SELECT;
        } else {
            s->player_side = 1; // irrelevant for 0-player
            s->screen = TTT_GAME;
            reset_game(s);
        }
        return;
    }

    if (s->screen == TTT_SIDE_SELECT) {
        s->player_side = (s->side_sel == 0) ? 1 : -1;
        s->screen = TTT_GAME;
        reset_game(s);
        return;
    }

    // Rematch on click after game ends (1-player)
    if (s->result && s->num_players == 1) {
        reset_game(s);
        return;
    }

    if (s->num_players == 0 || s->wopr_turn || s->result) return;

    int idx = pixel_to_cell(s, x, y);
    if (idx < 0) return;
    ttt_try_place(s, idx);
}

void wopr_ttt_mousemove(WoprState *w, int x, int y) {
    TttState *s = ttt(w);
    if (!s || s->screen != TTT_GAME) return;
    if (s->num_players != 1 || s->wopr_turn || s->result) {
        s->hover_cell = -1;
        return;
    }
    int idx = pixel_to_cell(s, x, y);
    // Only hover over empty cells
    s->hover_cell = (idx >= 0 && s->board[idx] == 0) ? idx : -1;
}

// ─── Keyboard ─────────────────────────────────────────────────────────────

bool wopr_ttt_keydown(WoprState *w, SDL_Keycode sym) {
    TttState *s = ttt(w);
    if (!s) return false;

    // ── Lobby ─────────────────────────────────────────────────────────────
    if (s->screen == TTT_LOBBY) {
        switch (sym) {
            case SDLK_UP:   case SDLK_w: s->lobby_sel = 0; break;
            case SDLK_DOWN: case SDLK_s: s->lobby_sel = 1; break;
            case SDLK_0: s->lobby_sel = 0; break;
            case SDLK_1: s->lobby_sel = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->num_players = s->lobby_sel;
                if (s->num_players == 1) {
                    s->screen = TTT_SIDE_SELECT;
                } else {
                    s->player_side = 1;
                    s->screen = TTT_GAME;
                    reset_game(s);
                }
                break;
            default: break;
        }
        return true;
    }

    // ── Side Select ───────────────────────────────────────────────────────
    if (s->screen == TTT_SIDE_SELECT) {
        switch (sym) {
            case SDLK_UP:   case SDLK_w: s->side_sel = 0; break;
            case SDLK_DOWN: case SDLK_s: s->side_sel = 1; break;
            case SDLK_x: s->side_sel = 0; break;
            case SDLK_o: s->side_sel = 1; break;
            case SDLK_ESCAPE:
                s->screen = TTT_LOBBY;
                break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->player_side = (s->side_sel == 0) ? 1 : -1;
                s->screen = TTT_GAME;
                reset_game(s);
                break;
            default: break;
        }
        return true;
    }

    // ── Game ──────────────────────────────────────────────────────────────
    if (sym == SDLK_r) { s->screen = TTT_LOBBY; return true; }

    if (s->result) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE)
            reset_game(s);
        return true;
    }

    if (s->num_players == 0) return true;
    if (s->wopr_turn) return true;

    int r = s->cursor / 3, c = s->cursor % 3;
    switch (sym) {
        case SDLK_UP:    case SDLK_w: r = (r+2)%3; break;
        case SDLK_DOWN:  case SDLK_s: r = (r+1)%3; break;
        case SDLK_LEFT:  case SDLK_a: c = (c+2)%3; break;
        case SDLK_RIGHT: case SDLK_d: c = (c+1)%3; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            ttt_try_place(s, r*3 + c);
            return true;
        default: return true;
    }
    s->cursor = r*3 + c;
    return true;
}
