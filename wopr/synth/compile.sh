gcc -DSYNTH_STANDALONE -o synth synth.c -lm $(sdl2-config --cflags --libs)
