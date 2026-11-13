#ifndef EDITOR_H
#define EDITOR_H

/*
 * editor.h — the QBasic-style full-screen editor (editor.cpp).
 *
 * Built wherever the display can place text anywhere on the screen and
 * read single keys: the terminal (Linux/macOS), the SDL window, MS-DOS and
 * bare metal. The WOPR / Felix terminal overlays are line-based, and the
 * Windows console build reads whole lines, so those keep the command line.
 * Define BASIC_NO_EDITOR to leave it out of any build.
 */
#if !defined(WOPR) && !defined(FELIX_BASIC) && !defined(INLINEBASIC) && \
    !defined(BASIC_NO_EDITOR) && !(defined(_WIN32) && !defined(USE_SDL_WINDOW))
#define BASIC_HAS_EDITOR 1
#endif

#include "basic_ns.h"

BASIC_NS_BEGIN

#define EDITOR_EXIT 0   /* File > Exit: leave Felix BASIC */
#define EDITOR_CLI  1   /* File > Command Line: go to the Ok prompt */

/* Run the editor. file: a program to open (NULL: whatever program is in
 * memory, e.g. one typed or LOADed at the command line). The program in
 * the editor is left in memory for the command line (LIST, RUN, SAVE). */
int editor_main(const char *file);

BASIC_NS_END

#endif /* EDITOR_H */
