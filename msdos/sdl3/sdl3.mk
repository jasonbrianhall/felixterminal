# SDL3 with its DOS port, compiled from the copy in this folder into
# $(BUILD)/libSDL3.a, as SDLPoPDOS's DOS build compiles it (see README.sdl3).
# Included by FLT's Makefile (msdos/) and Chirp's (msdos/chirp/); they set
# BUILD and CROSS first. Gives SDL3_LIB (the library to link) and
# SDL3_INCLUDE (-I for <SDL3/SDL.h>).

SDL3_DIR   := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
SDL3_SRCS  := $(shell cat $(SDL3_DIR)/SOURCES)
SDL3_CFLAGS = -O2 -DNDEBUG -fno-strict-aliasing -w \
              -DSDL_BUILD_MAJOR_VERSION=3 -DSDL_BUILD_MINOR_VERSION=5 -DSDL_BUILD_MICRO_VERSION=0 \
              -DSDL_STATIC_LIB -DUSING_GENERATED_CONFIG_H \
              -I$(SDL3_DIR)/sdl3-config/build_config -I$(SDL3_DIR)/sdl3-config \
              -I$(SDL3_DIR)/SDL3/include -I$(SDL3_DIR)/SDL3/src \
              -idirafter $(SDL3_DIR)/SDL3/src/video/khronos
SDL3_OBJS  := $(SDL3_SRCS:src/%.c=$(BUILD)/sdl/%.o)
SDL3_LIB   := $(BUILD)/libSDL3.a
SDL3_INCLUDE = -I$(SDL3_DIR)/SDL3/include

$(BUILD)/sdl/%.o: $(SDL3_DIR)/SDL3/src/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(SDL3_CFLAGS) -c $< -o $@

$(SDL3_LIB): $(SDL3_OBJS)
	rm -f $@
	$(CROSS)ar rcs $@ $(SDL3_OBJS)
