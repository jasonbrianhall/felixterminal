/*
 * synth.h  –  Formant text-to-speech (synth.c) as a library
 *
 *   tts_init_file("cmudict-0.7b");             // or tts_init_text(buf, len)
 *   int n;
 *   float *pcm = tts_render("SHALL WE PLAY A GAME?", 44100, &n);
 *   ... play n mono samples in -1..1 at 44100 Hz ...
 *   tts_free(pcm);
 *
 * Not thread-safe: initialise once, then render from one thread at a time.
 */
#ifndef SYNTH_H
#define SYNTH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the pronouncing dictionary (CMUdict format) from a file. Returns 0 on
 * success, -1 if the file can't be opened (the letter rules still work). */
int    tts_init_file(const char *cmudict_path);

/* The same from the dictionary's text in memory; NULL for none. */
void   tts_init_text(const char *cmudict_text, size_t len);

/* Speak text into mono float samples at sample_rate (e.g. 44100 or 48000).
 * Returns a buffer for tts_free() and sets *n_samples. */
float *tts_render(const char *text, int sample_rate, int *n_samples);
void   tts_free(float *samples);

/* Print each word's pronunciation to stdout while rendering (default off). */
void   tts_set_verbose(int on);

/* Free the dictionary. */
void   tts_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_H */
