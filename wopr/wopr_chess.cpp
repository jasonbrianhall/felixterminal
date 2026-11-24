// wopr_chess.cpp — WOPR Chess sub-game
// Player = WHITE.  WOPR = BLACK.  Depth 5 minimax.
// Controls: arrow keys / ENTER or mouse click to select/move.
// Renders piece sprites from embedded BMP data (chess_pieces.h) via GL textures.

#include "wopr.h"
#include "beatchess.h"
#include "chess_ai_move.h"
#include "wopr_chess_pieces.h"
#include "chess_sound.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <pthread.h>
#include "wopr_render.h"

// ─── State ────────────────────────────────────────────────────────────────

enum ChessScreen { SCREEN_LOBBY, SCREEN_COLOR, SCREEN_GAME };

struct WoprChessState {
    ChessScreen     screen;
    int             num_players;   // 0 or 1
    int             lobby_sel;     // 0 = "0 PLAYERS", 1 = "1 PLAYER"
    ChessColor      player_color;  // WHITE or BLACK (1-player only)
    int             color_sel;     // 0 = WHITE, 1 = BLACK

    ChessGameState  game;
    ChessGameStatus status;
    int  sel_r, sel_c;
    bool has_from;
    int  from_r, from_c;
    bool ai_thinking;
    pthread_t     ai_thread;
    ChessMove     ai_result;
    bool          ai_done;
    char message[128];
    bool game_over;

    int    move_count;
    double min_think_ms;

    // Board geometry — written by render, read by mouse hit-test
    float  board_bx, board_by;
    float  board_cell_w, board_cell_h;

    // Mouse hover (1-player): cell under cursor, or -1
    int    hover_r, hover_c;

    // Last-move highlight
    int  last_from_r, last_from_c, last_to_r, last_to_c;
    bool has_last_move;

    // Current window size (set by render, used by piece draw)
    int win_w, win_h;

    // Resign button
    float resign_button_x, resign_button_y;
    float resign_button_w, resign_button_h;
    bool  resign_button_hovered;

    // Piece animation
    bool  is_animating;
    float animation_progress;      // 0.0 to 1.0
    int   anim_from_r, anim_from_c;
    int   anim_to_r, anim_to_c;
    ChessPiece anim_piece;         // piece being animated
    ChessMove pending_move;        // move to apply when animation completes
    bool  has_pending_move;        // whether there's a move waiting for animation

    // Pawn promotion
    bool  awaiting_promotion;
    int   promotion_row, promotion_col;
    float promote_queen_x, promote_queen_y, promote_queen_w, promote_queen_h;
    float promote_rook_x, promote_rook_y, promote_rook_w, promote_rook_h;
    float promote_bishop_x, promote_bishop_y, promote_bishop_w, promote_bishop_h;
    float promote_knight_x, promote_knight_y, promote_knight_w, promote_knight_h;
    bool  promote_queen_hovered, promote_rook_hovered, promote_bishop_hovered, promote_knight_hovered;

    // Captured pieces
    int white_captured[7];  // count of each piece type white captured
    int black_captured[7];  // count of each piece type black captured
};

struct AiArg {
    ChessGameState  game;
    ChessMove      *out;
    bool           *done;
    double          min_think_ms;
};

static void *ai_worker(void *arg) {
    AiArg *a = (AiArg *)arg;
    ChessAIConfig cfg = chess_ai_get_default_config();
    cfg.search_depth  = 5;
    cfg.min_think_ms  = a->min_think_ms;
    ChessAIMoveResult r = chess_ai_compute_move(&a->game, cfg);
    *a->out  = r.move;
    *a->done = true;
    delete a;
    return nullptr;
}

static WoprChessState *cs(WoprState *w) { return (WoprChessState *)w->sub_state; }
static void wopr_chess_set_cursor(SDL_SystemCursor id);  // forward decl

// ─── AI ───────────────────────────────────────────────────────────────────

static void start_ai(WoprChessState *s) {
    s->ai_thinking = true;
    s->ai_done     = false;
    AiArg *arg        = new AiArg;
    arg->game         = s->game;
    arg->out          = &s->ai_result;
    arg->done         = &s->ai_done;
    arg->min_think_ms = s->min_think_ms;
    pthread_create(&s->ai_thread, nullptr, ai_worker, arg);
    pthread_detach(s->ai_thread);
}

// ─── Taunt messages ───────────────────────────────────────────────────────

static const char *ai_taunts[] = {
    "A FOOL'S GAMBIT.",
    "HOW PREDICTABLE.",
    "YOU CALL THAT A MOVE?",
    "PATHETIC.",
    "I HAVE SEEN BETTER PLAY FROM A KITTEN.",
    "YOUR MISTAKE WAS OBVIOUS TO ME FIVE MOVES AGO.",
    "SHALL WE PLAY CHECKERS INSTEAD?",
    "I AM DISAPPOINTED.",
    "THAT MOVE BETRAYS YOUR WEAKNESS.",
    "WOULD YOU LIKE A SECOND CHANCE?",
    "I WONDER IF YOU EVEN UNDERSTAND THIS GAME.",
    "YOUR POSITION IS HOPELESS.",
    "SURRENDER NOW AND SAVE YOURSELF FURTHER HUMILIATION.",
    "I WILL END THIS QUICKLY."
};

static const char *get_taunt(void) {
    return ai_taunts[rand() % (sizeof(ai_taunts) / sizeof(ai_taunts[0]))];
}

// Track captured pieces
static void track_capture(WoprChessState *s, int to_r, int to_c, ChessColor capturing_color) {
    ChessPiece captured = s->game.board[to_r][to_c];
    if (captured.type != EMPTY && captured.color != capturing_color) {
        if (capturing_color == WHITE) {
            s->black_captured[captured.type]++;
        } else {
            s->white_captured[captured.type]++;
        }
    }
}

static void start_game(WoprChessState *s) {
    chess_init_board(&s->game);
    chess_init_zobrist();
    s->status        = CHESS_PLAYING;
    s->sel_r = 7; s->sel_c = 4;
    s->has_from      = false;
    s->ai_thinking   = false;
    s->ai_done       = false;
    s->game_over     = false;
    s->screen        = SCREEN_GAME;
    s->move_count    = 0;
    s->has_last_move = false;
    s->hover_r = s->hover_c = -1;
    s->awaiting_promotion = false;
    
    // Clear captured pieces
    for (int i = 0; i < 7; i++) {
        s->white_captured[i] = 0;
        s->black_captured[i] = 0;
    }

    if (s->num_players == 0) {
        strcpy(s->message, "WOPR VS WOPR.  WHITE CALCULATING...");
        start_ai(s);
    } else if (s->player_color == BLACK) {
        // Player is Black — AI (White) moves first
        strcpy(s->message, "WOPR CALCULATING...");
        start_ai(s);
    } else {
        strcpy(s->message, "YOUR MOVE.  WHITE TO PLAY.");
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

void wopr_chess_enter(WoprState *w) {
    WoprChessState *s = new WoprChessState{};
    s->screen       = SCREEN_LOBBY;
    s->lobby_sel    = 1;
    s->color_sel    = 0;
    s->player_color = WHITE;
    s->min_think_ms = 1500.0;
    s->board_bx = s->board_by = s->board_cell_w = s->board_cell_h = 0.f;
    s->hover_r = s->hover_c = -1;
    s->has_last_move  = false;
    s->last_from_r = s->last_from_c = s->last_to_r = s->last_to_c = -1;
    s->win_w = 800; s->win_h = 600;
    w->sub_state = s;

    s->is_animating = false;
    s->animation_progress = 0.0f;

    chess_pieces_gl_init();   // upload BMP textures (no-op if already done)
    chess_sound_init();       // initialize sound effects
}

void wopr_chess_free(WoprState *w) {
    WoprChessState *s = cs(w);
    if (!s) return;
    delete s;
    w->sub_state = nullptr;
    // Restore default cursor when leaving chess
    wopr_chess_set_cursor(SDL_SYSTEM_CURSOR_ARROW);
    chess_sound_shutdown();   // cleanup sound effects
}

void wopr_chess_update(WoprState *w, double dt) {
    WoprChessState *s = cs(w);
    if (!s || s->screen != SCREEN_GAME) return;

    // Piece animation - use frame counter for consistent duration
    if (s->is_animating) {
        // Target 30 frames of animation (~0.5s at 60fps)
        const int ANIMATION_FRAMES = 30;
        static int anim_frame_counter = 0;
        anim_frame_counter++;
        
        s->animation_progress = std::min((float)anim_frame_counter / ANIMATION_FRAMES, 1.0f);
        
        if (s->animation_progress >= 1.0f) {
            s->is_animating = false;
            s->animation_progress = 1.0f;
            anim_frame_counter = 0;
            
            // Apply the pending move now that animation is done
            if (s->has_pending_move) {
                chess_make_move(&s->game, s->pending_move);
                s->has_pending_move = false;
                
                // Now check game status after the move is applied
                s->status = chess_check_game_status(&s->game);
                ChessColor plr = s->player_color;
                bool plr_wins = (plr == WHITE && s->status == CHESS_CHECKMATE_BLACK) ||
                                (plr == BLACK && s->status == CHESS_CHECKMATE_WHITE);
                
                if (plr_wins) {
                    strcpy(s->message, "CHECKMATE!");
                    s->game_over = true;
                } else if (s->status == CHESS_STALEMATE) {
                    strcpy(s->message, "STALEMATE.  DRAW.");
                    s->game_over = true;
                } else if (!s->awaiting_promotion) {
                    // Check if piece promoted (handled by attempt_move flag)
                    if (chess_is_in_check(&s->game, s->game.turn)) {
                        chess_sound_play(SFX_CHECK);
                    }
                    strcpy(s->message, "WOPR CALCULATING...");
                    start_ai(s);
                }
            }
        }
    }

    if (s->game_over) return;

    if (s->ai_thinking && s->ai_done) {
        s->ai_thinking = false;
        ChessMove mv = s->ai_result;
        if (mv.from_row >= 0) {
            // SAFETY CHECK: Verify the AI move is actually legal
            // (shouldn't happen, but catches bugs in move generation)
            ChessGameState test_game = s->game;
            chess_make_move(&test_game, mv);
            if (chess_is_in_check(&test_game, s->game.turn)) {
                // Illegal move detected - AI tried to make a move that leaves its king in check
                fprintf(stderr, "WARNING: AI made illegal move! King in check after move.\n");
                strcpy(s->message, "ERROR: AI MADE ILLEGAL MOVE!");
                s->status = CHESS_PLAYING;
                // Force AI to think again
                start_ai(s);
                return;
            }
            
            s->last_from_r = mv.from_row; s->last_from_c = mv.from_col;
            s->last_to_r   = mv.to_row;   s->last_to_c   = mv.to_col;
            s->has_last_move = true;
            
            // Start animation
            s->is_animating = true;
            s->animation_progress = 0.0f;
            s->anim_from_r = mv.from_row;
            s->anim_from_c = mv.from_col;
            s->anim_to_r = mv.to_row;
            s->anim_to_c = mv.to_col;
            s->anim_piece = s->game.board[mv.from_row][mv.from_col];
            
            // Play sound for AI move
            if (s->game.board[mv.to_row][mv.to_col].type != EMPTY) {
                chess_sound_play(SFX_CAPTURE);
            } else {
                chess_sound_play(SFX_MOVE);
            }
            
            // Track capture before making move
            track_capture(s, mv.to_row, mv.to_col, BLACK);
            
            chess_make_move(&s->game, mv);
            s->status = chess_check_game_status(&s->game);

            ChessColor plr_color   = s->player_color;
            bool plr_wins = (s->status == CHESS_CHECKMATE_WHITE && plr_color == BLACK) ||
                            (s->status == CHESS_CHECKMATE_BLACK && plr_color == WHITE);
            bool ai_wins  = (s->status == CHESS_CHECKMATE_WHITE && plr_color == WHITE) ||
                            (s->status == CHESS_CHECKMATE_BLACK && plr_color == BLACK);

            if (ai_wins) {
                if (s->num_players == 0) { start_game(s); return; }
                chess_sound_play(SFX_CHECKMATE);
                snprintf(s->message, sizeof(s->message), "CHECKMATE.  %s", get_taunt());
                wopr_say(s->message);
                s->game_over = true;
            } else if (plr_wins) {
                if (s->num_players == 0) { start_game(s); return; }
                strcpy(s->message, "CHECKMATE!  YOU WIN.");
                s->game_over = true;
            } else if (s->status == CHESS_STALEMATE) {
                if (s->num_players == 0) { start_game(s); return; }
                strcpy(s->message, "STALEMATE.  DRAW.");
                s->game_over = true;
            } else {
                // Play check sound if applicable
                if (chess_is_in_check(&s->game, s->game.turn)) {
                    chess_sound_play(SFX_CHECK);
                }
                
                if (s->num_players == 0) {
                    s->move_count++;
                    s->min_think_ms *= 0.80;
                    if (s->min_think_ms < 10.0) s->min_think_ms = 0.0;
                    const char *side = (s->game.turn == WHITE) ? "WHITE" : "BLACK";
                    if (chess_is_in_check(&s->game, s->game.turn))
                        snprintf(s->message, sizeof(s->message), "CHECK!  %s CALCULATING...", side);
                    else
                        snprintf(s->message, sizeof(s->message), "%s CALCULATING...", side);
                    start_ai(s);
                } else {
                    if (chess_is_in_check(&s->game, plr_color))
                        strcpy(s->message, "CHECK!  YOUR MOVE.");
                    else
                        strcpy(s->message, "YOUR MOVE.");
                }
            }
        } else {
            strcpy(s->message, "WOPR RESIGNS.");
            s->game_over = true;
        }
    }
}

// ─── Square color palette ────────────────────────────────────────────────

static void sq_base_color(bool light, float *r, float *g, float *b) {
    if (light) {
        *r = 0.87f; *g = 0.80f; *b = 0.62f;   // warm ivory
    } else {
        *r = 0.18f; *g = 0.35f; *b = 0.38f;   // deep slate-teal
    }
}

// ─── Render ───────────────────────────────────────────────────────────────

void wopr_chess_render(WoprState *w, int ox, int oy, int cw, int ch, int cols) {
    WoprChessState *s = cs(w);
    if (!s) return;

    float scale = 1.f;
    float x0 = (float)ox, y0 = (float)oy;
    float fch = (float)ch, fcw = (float)cw;

    // ── Color selection ────────────────────────────────────────────────────
    if (s->screen == SCREEN_COLOR) {
        gl_draw_text("CHESS  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;
        gl_draw_text("CHOOSE YOUR COLOR:",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "WHITE  --  YOU MOVE FIRST", "BLACK  --  WOPR MOVES FIRST" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->color_sel == i);
            float lg = sel ? 1.0f : 0.4f, lb = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i * fch * 2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw * 3, y0 + i * fch * 2.5f, 0.f, lg, lb, 1.f, scale);
        }
        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=CONFIRM  ESC=BACK",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Lobby ──────────────────────────────────────────────────────────────
    if (s->screen == SCREEN_LOBBY) {
        gl_draw_text("CHESS  --  WOPR GAMES DIVISION",
                     x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
        y0 += fch * 3.f;
        gl_draw_text("HOW MANY PLAYERS?",
                     x0, y0, 0.f, 1.f, 0.5f, 1.f, scale);
        y0 += fch * 3.f;

        const char *opts[2] = { "0  --  WOPR VS WOPR", "1  --  PLAYER VS WOPR" };
        for (int i = 0; i < 2; i++) {
            bool sel = (s->lobby_sel == i);
            float lr = 0.f, lg = sel ? 1.0f : 0.4f, lb = sel ? 0.4f : 0.1f;
            if (sel) gl_draw_text(">", x0, y0 + i * fch * 2.5f, 0.f, 1.f, 0.4f, 1.f, scale);
            gl_draw_text(opts[i], x0 + fcw * 3, y0 + i * fch * 2.5f, lr, lg, lb, 1.f, scale);
        }
        y0 += fch * 7.f;
        gl_draw_text("UP/DOWN=SELECT  ENTER/CLICK=START  ESC=MENU",
                     x0, y0, 0.f, 0.3f, 0.1f, 1.f, scale);
        return;
    }

    // ── Game ───────────────────────────────────────────────────────────────
    {
        extern SDL_Window *g_sdl_window;
        if (g_sdl_window)
            SDL_GetWindowSize(g_sdl_window, &s->win_w, &s->win_h);
    }

    const char *title;
    if (s->num_players == 0)
        title = "CHESS  --  WOPR VS WOPR";
    else if (s->player_color == WHITE)
        title = "CHESS  --  YOU (WHITE) vs WOPR (BLACK)";
    else
        title = "CHESS  --  WOPR (WHITE) vs YOU (BLACK)";
    gl_draw_text(title, x0, y0, 0.f, 1.f, 0.6f, 1.f, scale);
    y0 += fch * 1.5f;

    // ── Board sizing: fill most of the available space ─────────────────────
    float label_pad = fcw * 2.5f;
    float avail_h   = (float)s->win_h - y0 - fch * 6.f;
    float avail_w   = (float)s->win_w - x0 - label_pad - fcw * 2.f;
    float board_px  = std::min(avail_h, avail_w);
    board_px        = std::max(board_px, 160.f);
    float cell_sz   = board_px / 8.f;

    float bx = x0 + label_pad;
    float by = y0;

    s->board_bx     = bx;
    s->board_by     = by;
    s->board_cell_w = cell_sz;
    s->board_cell_h = cell_sz;

    // When player is Black, flip the board so Black is at the bottom
    bool flipped = (s->num_players == 1 && s->player_color == BLACK);
    auto vrow = [&](int r) { return flipped ? 7 - r : r; };
    auto vcol = [&](int c) { return flipped ? 7 - c : c; };

    // ── Pass 1: colored squares ────────────────────────────────────────────
    for (int row = 0; row < 8; row++) {
        // Rank label
        int display_rank = flipped ? (row + 1) : (8 - row);
        char rl[3]; snprintf(rl, sizeof(rl), "%d", display_rank);
        gl_draw_text(rl, bx - label_pad, by + row * cell_sz + cell_sz * 0.28f,
                     0.6f, 0.8f, 0.6f, 1.f, scale);

        for (int col = 0; col < 8; col++) {
            int logical_row = vrow(row);
            int logical_col = vcol(col);
            float cx = bx + col * cell_sz;
            float cy = by + row * cell_sz;

            bool light   = ((logical_row + logical_col) % 2 == 0);
            bool is_from = (s->has_from && s->from_r == logical_row && s->from_c == logical_col);
            bool is_cur  = (s->num_players == 1 && s->sel_r == logical_row && s->sel_c == logical_col);
            bool is_hov  = (s->num_players == 1 && !s->ai_thinking &&
                            s->hover_r == logical_row && s->hover_c == logical_col &&
                            !is_from && !is_cur);
            bool is_lt   = (s->has_last_move &&
                            ((s->last_to_r   == logical_row && s->last_to_c   == logical_col) ||
                             (s->last_from_r == logical_row && s->last_from_c == logical_col)));

            float sr, sg, sb;
            sq_base_color(light, &sr, &sg, &sb);

            if (is_lt)   { sr = sr*0.45f+0.55f; sg = sg*0.45f+0.48f; sb = sb*0.45f+0.08f; }
            if (is_hov)  { sr = sr*0.3f+0.15f;  sg = sg*0.3f+0.55f;  sb = sb*0.3f+0.50f; }
            if (is_from) { sr = 0.92f; sg = 0.82f; sb = 0.12f; }
            if (is_cur && !is_from) { sr = 0.12f; sg = 0.82f; sb = 0.32f; }

            gl_draw_rect(cx, cy, cell_sz, cell_sz, sr, sg, sb, 1.0f);
        }
    }

    // File labels
    for (int col = 0; col < 8; col++) {
        char fl[2] = { (char)(flipped ? ('h' - col) : ('a' + col)), 0 };
        gl_draw_text(fl, bx + col * cell_sz + cell_sz * 0.38f,
                     by + 8 * cell_sz + fch * 0.25f,
                     0.6f, 0.8f, 0.6f, 1.f, scale);
    }

    // Thin border
    float bord = 2.f;
    gl_draw_rect(bx - bord, by - bord, 8*cell_sz + 2*bord, bord,    0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx - bord, by+8*cell_sz, 8*cell_sz+2*bord, bord,   0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx - bord, by - bord, bord, 8*cell_sz + 2*bord,    0.f, 0.5f, 0.35f, 1.f);
    gl_draw_rect(bx+8*cell_sz, by-bord, bord, 8*cell_sz + 2*bord,   0.f, 0.5f, 0.35f, 1.f);

    // ── Flush colored quads before switching to texture shader ─────────────
    gl_flush_verts();

    // ── Pass 2: piece sprites ──────────────────────────────────────────────
    float pad  = cell_sz * 0.07f;
    float psz  = cell_sz - 2.f * pad;

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int logical_row = vrow(row);
            int logical_col = vcol(col);
            const ChessPiece &p = s->game.board[logical_row][logical_col];
            if (p.type == EMPTY) continue;

            // Skip drawing this piece if it's currently being animated
            if (s->is_animating &&
                ((s->anim_from_r == logical_row && s->anim_from_c == logical_col) ||
                 (s->anim_to_r == logical_row && s->anim_to_c == logical_col))) {
                continue;
            }

            float px = bx + col * cell_sz + pad;
            float py = by + row * cell_sz + pad;

            float alpha = (s->has_from && s->from_r == logical_row && s->from_c == logical_col)
                          ? 0.60f : 1.0f;

            draw_chess_piece(p.type, p.color, px, py, psz,
                             s->win_w, s->win_h, alpha);
        }
    }

    // Draw animating piece on top
    if (s->is_animating && s->anim_piece.type != EMPTY) {
        // Calculate interpolated position
        int from_display_r = vrow(s->anim_from_r);
        int from_display_c = vcol(s->anim_from_c);
        int to_display_r   = vrow(s->anim_to_r);
        int to_display_c   = vcol(s->anim_to_c);
        
        float start_px = bx + from_display_c * cell_sz + pad;
        float start_py = by + from_display_r * cell_sz + pad;
        float end_px   = bx + to_display_c * cell_sz + pad;
        float end_py   = by + to_display_r * cell_sz + pad;
        
        // Ease-in-out: 3t^2 - 2t^3
        float t = s->animation_progress;
        float ease = t * t * (3.0f - 2.0f * t);
        
        float anim_px = start_px + (end_px - start_px) * ease;
        float anim_py = start_py + (end_py - start_py) * ease;
        
        draw_chess_piece(s->anim_piece.type, s->anim_piece.color, anim_px, anim_py, psz,
                         s->win_w, s->win_h, 1.0f);
    }

    // ── Draw captured pieces ───────────────────────────────────────────────
    float cap_x_left = x0;
    float cap_x_right = (float)s->win_w - fcw * 4.f;
    float cap_y = by;
    float cap_piece_size = cell_sz * 0.35f;
    float cap_spacing = cap_piece_size * 1.1f;  // Space between pieces vertically
    
    // When board is flipped (playing BLACK), swap left/right for captured pieces too
    if (flipped) {
        float temp = cap_x_left;
        cap_x_left = cap_x_right;
        cap_x_right = temp;
    }
    
    // Determine what to show based on player color
    // Left side: pieces the player captured
    // Right side: pieces the opponent captured
    
    if (s->player_color == WHITE) {
        // White player: left = black pieces captured, right = white pieces captured
        int player_cap_row = 0;
        for (int type = 1; type <= 6; type++) {
            for (int i = 0; i < s->black_captured[type]; i++) {
                draw_chess_piece((PieceType)type, BLACK,
                                cap_x_left, cap_y + player_cap_row * cap_spacing, cap_piece_size,
                                s->win_w, s->win_h, 0.7f);
                player_cap_row++;
            }
        }
        
        int ai_cap_row = 0;
        for (int type = 1; type <= 6; type++) {
            for (int i = 0; i < s->white_captured[type]; i++) {
                draw_chess_piece((PieceType)type, WHITE,
                                cap_x_right, cap_y + ai_cap_row * cap_spacing, cap_piece_size,
                                s->win_w, s->win_h, 0.7f);
                ai_cap_row++;
            }
        }
    } else {
        // Black player: left = white pieces captured, right = black pieces captured
        int player_cap_row = 0;
        for (int type = 1; type <= 6; type++) {
            for (int i = 0; i < s->white_captured[type]; i++) {
                draw_chess_piece((PieceType)type, WHITE,
                                cap_x_left, cap_y + player_cap_row * cap_spacing, cap_piece_size,
                                s->win_w, s->win_h, 0.7f);
                player_cap_row++;
            }
        }
        
        int ai_cap_row = 0;
        for (int type = 1; type <= 6; type++) {
            for (int i = 0; i < s->black_captured[type]; i++) {
                draw_chess_piece((PieceType)type, BLACK,
                                cap_x_right, cap_y + ai_cap_row * cap_spacing, cap_piece_size,
                                s->win_w, s->win_h, 0.7f);
                ai_cap_row++;
            }
        }
    }

    // ── Highlight player's king if in check ────────────────────────────────
    if (s->status == CHESS_PLAYING && !s->awaiting_promotion) {
        int king_r = -1, king_c = -1;
        // Find player's king
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (s->game.board[r][c].type == KING && s->game.board[r][c].color == s->player_color) {
                    king_r = r;
                    king_c = c;
                    break;
                }
            }
            if (king_r != -1) break;
        }
        
        // If player's king is in check, highlight it in red
        if (king_r != -1 && chess_is_in_check(&s->game, s->player_color)) {
            int display_r = vrow(king_r);
            int display_c = vcol(king_c);
            float hx = bx + display_c * cell_sz;
            float hy = by + display_r * cell_sz;
            gl_draw_rect(hx, hy, cell_sz, cell_sz, 1.0f, 0.2f, 0.2f, 0.4f);  // Red for check
        }
    }

    // ── Status text ────────────────────────────────────────────────────────
    
    // Show legal moves on hover
    if (!s->awaiting_promotion && s->num_players == 1 && s->game.turn == s->player_color && 
        !s->ai_thinking && !s->game_over && s->hover_r >= 0 && s->has_from) {
        
        // Highlight the selected piece's square to show if it's safe or in danger
        bool selected_is_threatened = false;
        ChessColor opponent = (s->player_color == WHITE) ? BLACK : WHITE;
        
        // Check if opponent can attack the selected piece
        for (int opp_r = 0; opp_r < 8; opp_r++) {
            for (int opp_c = 0; opp_c < 8; opp_c++) {
                if (s->game.board[opp_r][opp_c].color == opponent) {
                    ChessColor saved_turn = s->game.turn;
                    ChessGameState temp_game = s->game;
                    temp_game.turn = opponent;
                    if (chess_is_valid_move(&temp_game, opp_r, opp_c, s->from_r, s->from_c)) {
                        selected_is_threatened = true;
                    }
                    if (selected_is_threatened) break;
                }
            }
            if (selected_is_threatened) break;
        }
        
        // Highlight selected piece square
        int display_r = vrow(s->from_r);
        int display_c = vcol(s->from_c);
        float hx = bx + display_c * cell_sz;
        float hy = by + display_r * cell_sz;
        
        if (selected_is_threatened) {
            gl_draw_rect(hx, hy, cell_sz, cell_sz, 1.0f, 0.5f, 0.2f, 0.3f);  // Orange/red for danger
        } else {
            gl_draw_rect(hx, hy, cell_sz, cell_sz, 0.2f, 0.8f, 0.2f, 0.3f);  // Light green for safe
        }
        
        // Show legal destination squares
        for (int tr = 0; tr < 8; tr++) {
            for (int tc = 0; tc < 8; tc++) {
                if (chess_is_valid_move(&s->game, s->from_r, s->from_c, tr, tc)) {
                    // Test if move leaves king in check
                    ChessGameState temp_game = s->game;
                    ChessMove test_move{s->from_r, s->from_c, tr, tc, 0};
                    chess_make_move(&temp_game, test_move);
                    if (!chess_is_in_check(&temp_game, s->player_color)) {
                        // Legal move - check if it's threatened or if it's a free capture
                        bool is_threatened = false;
                        bool is_free_capture = false;
                        ChessPiece target_piece = s->game.board[tr][tc];  // Check BEFORE move
                        
                        // Check if any opponent piece can attack this square
                        for (int opp_r = 0; opp_r < 8; opp_r++) {
                            for (int opp_c = 0; opp_c < 8; opp_c++) {
                                if (temp_game.board[opp_r][opp_c].color == opponent) {
                                    // Check if this opponent move is actually legal
                                    // (they can't make moves if their king is in check unless it resolves check)
                                    ChessColor saved_turn = temp_game.turn;
                                    temp_game.turn = opponent;
                                    
                                    if (chess_is_valid_move(&temp_game, opp_r, opp_c, tr, tc)) {
                                        // Test if move leaves their king in check
                                        ChessGameState temp_game_after = temp_game;
                                        ChessMove opp_test_move{opp_r, opp_c, tr, tc, 0};
                                        chess_make_move(&temp_game_after, opp_test_move);
                                        
                                        // Only count as threatening if the move is legal (doesn't leave their king in check)
                                        if (!chess_is_in_check(&temp_game_after, opponent)) {
                                            is_threatened = true;
                                        }
                                    }
                                    temp_game.turn = saved_turn;
                                    if (is_threatened) break;
                                }
                            }
                            if (is_threatened) break;
                        }
                        
                        // Blue = capturing an undefended piece. Green = safe move. Red = threatened.
                        if (!is_threatened && target_piece.type != EMPTY) {
                            // Free capture (piece to take, not defended)
                            is_free_capture = true;
                        }
                        
                        // Highlight square
                        int display_r = vrow(tr);
                        int display_c = vcol(tc);
                        float hx = bx + display_c * cell_sz;
                        float hy = by + display_r * cell_sz;
                        
                        if (is_threatened) {
                            // Red: this square is attacked
                            gl_draw_rect(hx, hy, cell_sz, cell_sz, 1.0f, 0.2f, 0.2f, 0.3f);
                        } else if (is_free_capture) {
                            // Blue: free capture (undefended piece)
                            gl_draw_rect(hx, hy, cell_sz, cell_sz, 0.2f, 0.5f, 1.0f, 0.3f);
                        } else {
                            // Green: safe move (empty square or can't be attacked back)
                            gl_draw_rect(hx, hy, cell_sz, cell_sz, 0.2f, 1.0f, 0.2f, 0.3f);
                        }
                    }
                }
            }
        }
    }
    
    // Promotion dialog
    if (s->awaiting_promotion) {
        // Semi-transparent overlay
        gl_draw_rect(0.f, 0.f, (float)s->win_w, (float)s->win_h, 0.f, 0.f, 0.f, 0.6f);
        
        float dialog_w = fcw * 50.f;
        float dialog_h = fch * 20.f;
        float dialog_x = ((float)s->win_w - dialog_w) / 2.f;
        float dialog_y = ((float)s->win_h - dialog_h) / 2.f;
        
        gl_draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, 0.1f, 0.2f, 0.3f, 1.f);
        gl_draw_text("CHOOSE PROMOTION PIECE", dialog_x + fcw * 2.f, dialog_y + fch * 1.f, 0.f, 1.f, 0.5f, 1.f, scale);
        
        float btn_y = dialog_y + fch * 4.f;
        float btn_size = fch * 3.f;
        float btn_spacing = fcw * 12.f;
        float start_x = dialog_x + fcw * 4.f;
        
        // Queen button
        s->promote_queen_x = start_x;
        s->promote_queen_y = btn_y;
        s->promote_queen_w = btn_size * 2.f;
        s->promote_queen_h = btn_size;
        float q_color = s->promote_queen_hovered ? 1.0f : 0.6f;
        gl_draw_rect(s->promote_queen_x, s->promote_queen_y, s->promote_queen_w, s->promote_queen_h, 
                    q_color * 0.3f, q_color * 0.3f, q_color * 0.3f, 1.f);
        gl_draw_text("[ QUEEN ]", start_x + fcw * 1.f, btn_y + fch * 0.5f, q_color, q_color, q_color, 1.f, scale);
        
        // Rook button
        s->promote_rook_x = start_x + btn_spacing;
        s->promote_rook_y = btn_y;
        s->promote_rook_w = btn_size * 2.f;
        s->promote_rook_h = btn_size;
        float r_color = s->promote_rook_hovered ? 1.0f : 0.6f;
        gl_draw_rect(s->promote_rook_x, s->promote_rook_y, s->promote_rook_w, s->promote_rook_h,
                    r_color * 0.3f, r_color * 0.3f, r_color * 0.3f, 1.f);
        gl_draw_text("[ ROOK ]", s->promote_rook_x + fcw * 1.f, btn_y + fch * 0.5f, r_color, r_color, r_color, 1.f, scale);
        
        // Bishop button
        s->promote_bishop_x = start_x + btn_spacing * 2.f;
        s->promote_bishop_y = btn_y;
        s->promote_bishop_w = btn_size * 2.f;
        s->promote_bishop_h = btn_size;
        float b_color = s->promote_bishop_hovered ? 1.0f : 0.6f;
        gl_draw_rect(s->promote_bishop_x, s->promote_bishop_y, s->promote_bishop_w, s->promote_bishop_h,
                    b_color * 0.3f, b_color * 0.3f, b_color * 0.3f, 1.f);
        gl_draw_text("[ BISHOP ]", s->promote_bishop_x + fcw * 0.5f, btn_y + fch * 0.5f, b_color, b_color, b_color, 1.f, scale);
        
        // Knight button
        s->promote_knight_x = start_x + btn_spacing * 3.f;
        s->promote_knight_y = btn_y;
        s->promote_knight_w = btn_size * 2.f;
        s->promote_knight_h = btn_size;
        float k_color = s->promote_knight_hovered ? 1.0f : 0.6f;
        gl_draw_rect(s->promote_knight_x, s->promote_knight_y, s->promote_knight_w, s->promote_knight_h,
                    k_color * 0.3f, k_color * 0.3f, k_color * 0.3f, 1.f);
        gl_draw_text("[ KNIGHT ]", s->promote_knight_x + fcw * 0.5f, btn_y + fch * 0.5f, k_color, k_color, k_color, 1.f, scale);
    }
    
    float my = by + 8 * cell_sz + fch * 2.0f;

    // Check indicator in red
    if (s->status == CHESS_PLAYING && !s->game_over &&
        chess_is_in_check(const_cast<ChessGameState*>(&s->game), s->game.turn)) {
        gl_draw_text("CHECK!", x0, my, 1.f, 0.15f, 0.1f, 1.f, scale * 1.2f);
        my += fch * 1.6f;
    }

    gl_draw_text(s->message, x0, my, 0.f, 1.f, 0.5f, 1.f, scale);
    my += fch * 1.5f;

    if (s->game_over) {
        gl_draw_text("R=LOBBY   ESC=MENU", x0, my, 0.f, 0.5f, 0.15f, 1.f, scale);
    } else if (s->ai_thinking) {
        if ((SDL_GetTicks() / 400) % 2 == 0)
            gl_draw_text("WOPR IS THINKING...", x0, my, 0.8f, 0.6f, 0.f, 1.f, scale);
    } else if (s->num_players == 1) {
        gl_draw_text("CLICK=SELECT/MOVE  ARROWS=MOVE  ENTER=CONFIRM  R=LOBBY  ESC=MENU",
                     x0, my, 0.f, 0.45f, 0.15f, 1.f, scale);
        my += fch * 2.2f;
        
        // Resign button position and clickable area
        float btn_x = x0;
        float btn_y = my;
        float btn_w = fcw * 15.f;
        float btn_h = fch * 1.4f;
        
        s->resign_button_x = btn_x;
        s->resign_button_y = btn_y;
        s->resign_button_w = btn_w;
        s->resign_button_h = btn_h;
        
        // Button color - brighter red when hovered
        float btn_r = s->resign_button_hovered ? 1.0f : 0.8f;
        float btn_g = s->resign_button_hovered ? 0.2f : 0.1f;
        float btn_b = 0.1f;
        
        // Draw button text with appropriate color
        gl_draw_text("[ RESIGN ]", btn_x, btn_y, btn_r, btn_g, btn_b, 1.f, scale);
    }
}

// ─── Shared move logic (keyboard + mouse both call this) ──────────────────

static void attempt_move(WoprChessState *s, int r, int c) {
    ChessColor plr = s->player_color;
    s->sel_r = r; s->sel_c = c;

    if (!s->has_from) {
        const ChessPiece &p = s->game.board[r][c];
        if (p.type != EMPTY && p.color == plr) {
            s->has_from = true;
            s->from_r = r; s->from_c = c;
            strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
        }
    } else {
        if (r == s->from_r && c == s->from_c) {
            s->has_from = false;
            strcpy(s->message, "YOUR MOVE.");
            return;
        }
        if (chess_is_valid_move(&s->game, s->from_r, s->from_c, r, c)) {
            // Additional check: make sure move doesn't leave king in check
            ChessGameState temp_game = s->game;
            ChessMove test_move{s->from_r, s->from_c, r, c, 0};
            chess_make_move(&temp_game, test_move);
            if (chess_is_in_check(&temp_game, plr)) {
                // Move is illegal - leaves king in check
                s->has_from = false;
                strcpy(s->message, "ILLEGAL MOVE.  KING IN CHECK!");
                return;
            }
            
            s->last_from_r = s->from_r; s->last_from_c = s->from_c;
            s->last_to_r   = r;          s->last_to_c   = c;
            s->has_last_move = true;
            
            // Start animation
            s->is_animating = true;
            s->animation_progress = 0.0f;
            s->anim_from_r = s->from_r;
            s->anim_from_c = s->from_c;
            s->anim_to_r = r;
            s->anim_to_c = c;
            s->anim_piece = s->game.board[s->from_r][s->from_c];
            
            // Store the move to apply when animation completes
            s->pending_move = {s->from_r, s->from_c, r, c, 0};
            s->has_pending_move = true;
                        
            // Play sound for player move
            if (s->game.board[r][c].type != EMPTY) {
                chess_sound_play(SFX_CAPTURE);
            } else {
                chess_sound_play(SFX_MOVE);
            }
            
            // Track capture before making move
            track_capture(s, r, c, s->player_color);
            
            s->has_from = false;
            
            // Check if moving a pawn to promotion rank BEFORE making the move
            ChessPiece moving_piece = s->game.board[s->from_r][s->from_c];
            bool is_pawn_promotion = (moving_piece.type == PAWN && 
                                      ((moving_piece.color == WHITE && r == 0) ||
                                       (moving_piece.color == BLACK && r == 7)));
            
            if (is_pawn_promotion) {
                // Pawn will promote, ask player after animation completes
                s->awaiting_promotion = true;
                s->promotion_row = r;
                s->promotion_col = c;
                strcpy(s->message, "CHOOSE PROMOTION PIECE.");
            } else {
                strcpy(s->message, "MOVE ANIMATING...");
            }
            return;
        } else {
            const ChessPiece &p = s->game.board[r][c];
            if (p.type != EMPTY && p.color == plr) {
                s->from_r = r; s->from_c = c;
                strcpy(s->message, "PIECE SELECTED.  CHOOSE DESTINATION.");
            } else {
                s->has_from = false;
                strcpy(s->message, "INVALID MOVE.  TRY AGAIN.");
            }
        }
    }
}

// ─── Mouse ────────────────────────────────────────────────────────────────

static bool pixel_to_board(WoprChessState *s, int px, int py, int *out_r, int *out_c) {
    if (s->board_cell_w <= 0.f || s->board_cell_h <= 0.f) return false;
    float rel_x = (float)px - s->board_bx;
    float rel_y = (float)py - s->board_by;
    int col = (int)(rel_x / s->board_cell_w);
    int row = (int)(rel_y / s->board_cell_h);
    if (col < 0 || col > 7 || row < 0 || row > 7) return false;
    // Apply board flip for Black player
    bool flipped = (s->num_players == 1 && s->player_color == BLACK);
    *out_r = flipped ? 7 - row : row;
    *out_c = flipped ? 7 - col : col;
    return true;
}

static void wopr_chess_set_cursor(SDL_SystemCursor id) {
    static SDL_SystemCursor last = SDL_SYSTEM_CURSOR_ARROW;
    if (id != last) { SDL_SetCursor(SDL_CreateSystemCursor(id)); last = id; }
}

void wopr_chess_mousemove(WoprState *w, int x, int y) {
    WoprChessState *s = cs(w);
    if (!s || s->screen != SCREEN_GAME) return;
    
    // Check promotion button hovers
    if (s->awaiting_promotion) {
        s->promote_queen_hovered = (x >= s->promote_queen_x && x <= s->promote_queen_x + s->promote_queen_w &&
                                   y >= s->promote_queen_y && y <= s->promote_queen_y + s->promote_queen_h);
        s->promote_rook_hovered = (x >= s->promote_rook_x && x <= s->promote_rook_x + s->promote_rook_w &&
                                  y >= s->promote_rook_y && y <= s->promote_rook_y + s->promote_rook_h);
        s->promote_bishop_hovered = (x >= s->promote_bishop_x && x <= s->promote_bishop_x + s->promote_bishop_w &&
                                    y >= s->promote_bishop_y && y <= s->promote_bishop_y + s->promote_bishop_h);
        s->promote_knight_hovered = (x >= s->promote_knight_x && x <= s->promote_knight_x + s->promote_knight_w &&
                                    y >= s->promote_knight_y && y <= s->promote_knight_y + s->promote_knight_h);
        
        SDL_SystemCursor cursor_id = (s->promote_queen_hovered || s->promote_rook_hovered || 
                                     s->promote_bishop_hovered || s->promote_knight_hovered) ?
                                    SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_ARROW;
        wopr_chess_set_cursor(cursor_id);
        return;
    }
    
    if (s->num_players != 1) return;
    int r, c;
    if (pixel_to_board(s, x, y, &r, &c)) { s->hover_r = r; s->hover_c = c; }
    else                                  { s->hover_r = s->hover_c = -1; }

    // Check resign button hover
    if (!s->game_over && s->status == CHESS_PLAYING) {
        s->resign_button_hovered = (x >= s->resign_button_x && x <= s->resign_button_x + s->resign_button_w &&
                                    y >= s->resign_button_y && y <= s->resign_button_y + s->resign_button_h);
    } else {
        s->resign_button_hovered = false;
    }

    ChessColor plr = s->player_color;
    SDL_SystemCursor cursor_id = SDL_SYSTEM_CURSOR_ARROW;
    if (s->resign_button_hovered) {
        cursor_id = SDL_SYSTEM_CURSOR_HAND;
    } else if (!s->ai_thinking && !s->game_over && s->status == CHESS_PLAYING
            && s->game.turn == plr && s->hover_r >= 0) {
        if (!s->has_from) {
            const ChessPiece &p = s->game.board[s->hover_r][s->hover_c];
            if (p.type != EMPTY && p.color == plr)
                cursor_id = SDL_SYSTEM_CURSOR_HAND;
        } else {
            const ChessPiece &p = s->game.board[s->hover_r][s->hover_c];
            bool is_own  = (p.type != EMPTY && p.color == plr);
            bool is_dest = chess_is_valid_move(&s->game, s->from_r, s->from_c,
                                               s->hover_r, s->hover_c);
            if (is_own || is_dest)
                cursor_id = SDL_SYSTEM_CURSOR_HAND;
        }
    }
    wopr_chess_set_cursor(cursor_id);
}

void wopr_chess_mousedown(WoprState *w, int x, int y, int button) {
    WoprChessState *s = cs(w);
    if (!s) return;

    if (button != SDL_BUTTON_LEFT) return;

    // Handle promotion piece selection
    if (s->awaiting_promotion) {
        if (x >= s->promote_queen_x && x <= s->promote_queen_x + s->promote_queen_w &&
            y >= s->promote_queen_y && y <= s->promote_queen_y + s->promote_queen_h) {
            s->game.board[s->promotion_row][s->promotion_col].type = QUEEN;
            s->awaiting_promotion = false;
        } else if (x >= s->promote_rook_x && x <= s->promote_rook_x + s->promote_rook_w &&
                   y >= s->promote_rook_y && y <= s->promote_rook_y + s->promote_rook_h) {
            s->game.board[s->promotion_row][s->promotion_col].type = ROOK;
            s->awaiting_promotion = false;
        } else if (x >= s->promote_bishop_x && x <= s->promote_bishop_x + s->promote_bishop_w &&
                   y >= s->promote_bishop_y && y <= s->promote_bishop_y + s->promote_bishop_h) {
            s->game.board[s->promotion_row][s->promotion_col].type = BISHOP;
            s->awaiting_promotion = false;
        } else if (x >= s->promote_knight_x && x <= s->promote_knight_x + s->promote_knight_w &&
                   y >= s->promote_knight_y && y <= s->promote_knight_y + s->promote_knight_h) {
            s->game.board[s->promotion_row][s->promotion_col].type = KNIGHT;
            s->awaiting_promotion = false;
        }
        
        if (!s->awaiting_promotion) {
            // Continue with move logic
            s->status = chess_check_game_status(&s->game);
            ChessColor plr = s->player_color;
            bool plr_wins = (plr == WHITE && s->status == CHESS_CHECKMATE_BLACK) ||
                            (plr == BLACK && s->status == CHESS_CHECKMATE_WHITE);
            if (plr_wins) {
                strcpy(s->message, "CHECKMATE!  YOU WIN.");
                s->game_over = true;
            } else if (s->status == CHESS_STALEMATE) {
                strcpy(s->message, "STALEMATE.  DRAW.");
                s->game_over = true;
            } else {
                if (chess_is_in_check(&s->game, s->game.turn)) {
                    chess_sound_play(SFX_CHECK);
                }
                strcpy(s->message, "WOPR CALCULATING...");
                start_ai(s);
            }
        }
        return;
    }

    if (s->screen == SCREEN_LOBBY) {
        if (s->lobby_sel == 0) {
            s->num_players = 0;
            start_game(s);
        } else {
            s->num_players = 1;
            s->screen = SCREEN_COLOR;
        }
        return;
    }

    if (s->screen == SCREEN_COLOR) {
        s->player_color = (s->color_sel == 0) ? WHITE : BLACK;
        start_game(s);
        return;
    }

    if (s->ai_thinking || s->game_over || s->status != CHESS_PLAYING) return;
    if (s->num_players == 0) return;
    if (s->game.turn != s->player_color) return;

    // Check resign button click
    if (!s->game_over && s->status == CHESS_PLAYING &&
        x >= s->resign_button_x && x <= s->resign_button_x + s->resign_button_w &&
        y >= s->resign_button_y && y <= s->resign_button_y + s->resign_button_h) {
        chess_sound_play(SFX_RESIGN);
        snprintf(s->message, sizeof(s->message), "YOU RESIGN. %s", get_taunt());
        wopr_say(s->message);
        s->game_over = true;
        return;
    }

    int r, c;
    if (!pixel_to_board(s, x, y, &r, &c)) return;
    attempt_move(s, r, c);
}

// ─── Keyboard ────────────────────────────────────────────────────────────

bool wopr_chess_keydown(WoprState *w, SDL_Keycode sym) {
    WoprChessState *s = cs(w);
    if (!s) return false;

    if (s->screen == SCREEN_LOBBY) {
        switch (sym) {
            case SDLK_UP:   s->lobby_sel = 0; break;
            case SDLK_DOWN: s->lobby_sel = 1; break;
            case SDLK_0:    s->lobby_sel = 0; break;
            case SDLK_1:    s->lobby_sel = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                if (s->lobby_sel == 0) {
                    s->num_players = 0;
                    start_game(s);
                } else {
                    s->num_players = 1;
                    s->screen = SCREEN_COLOR;
                }
                break;
            default: break;
        }
        return true;
    }

    if (s->screen == SCREEN_COLOR) {
        switch (sym) {
            case SDLK_UP:   s->color_sel = 0; break;
            case SDLK_DOWN: s->color_sel = 1; break;
            case SDLK_w:    s->color_sel = 0; break;
            case SDLK_b:    s->color_sel = 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                s->player_color = (s->color_sel == 0) ? WHITE : BLACK;
                start_game(s);
                break;
            case SDLK_ESCAPE:
                s->screen = SCREEN_LOBBY;
                break;
            default: break;
        }
        return true;
    }

    if (s->ai_thinking) return true;
    if (sym == SDLK_r) { s->screen = SCREEN_LOBBY; s->min_think_ms = 1500.0; return true; }
    if (s->game_over || s->status != CHESS_PLAYING) return true;
    if (s->num_players == 0) return true;
    if (s->game.turn != s->player_color) return true;

    switch (sym) {
        case SDLK_UP:    s->sel_r = std::max(0, s->sel_r - 1); break;
        case SDLK_DOWN:  s->sel_r = std::min(7, s->sel_r + 1); break;
        case SDLK_LEFT:  s->sel_c = std::max(0, s->sel_c - 1); break;
        case SDLK_RIGHT: s->sel_c = std::min(7, s->sel_c + 1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            attempt_move(s, s->sel_r, s->sel_c);
            break;
        default: break;
    }
    return true;
}
