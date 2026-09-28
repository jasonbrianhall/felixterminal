#pragma once

// ============================================================================
// CONFIG / CONSTANTS
// ============================================================================

#define TERM_COLS_DEFAULT  80
#define TERM_ROWS_DEFAULT  24
#define TERM_MAX_COLS      512
#define TERM_MAX_ROWS      256
#define SCROLLBACK_LINES   5000
#define FONT_SIZE_DEFAULT  16
#define FONT_SIZE_MIN      6
#define FONT_SIZE_MAX      72
#define WIN_TITLE          "Felix Terminal"
#define MAX_VERTS          400000

// ============================================================================
// CELL ATTRIBUTE BITS
// ============================================================================

#define ATTR_BOLD      (1<<0)
#define ATTR_UNDERLINE (1<<1)
#define ATTR_REVERSE   (1<<2)
#define ATTR_BLINK     (1<<3)
#define ATTR_ITALIC    (1<<4)
#define ATTR_STRIKE    (1<<5)
#define ATTR_OVERLINE  (1<<6)
#define ATTR_DIM       (1<<7)
