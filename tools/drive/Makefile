# tools/drive/Makefile - builds libfe2drive.dylib (macOS only).
# Links against libSystem only; SDL_PollEvent is left undefined and bound at
# load time to the copy the game already loaded (dyld two-level namespace is
# flattened for that one symbol with -U).
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Werror -std=c11 -D_DARWIN_C_SOURCE
LIB     = libfe2drive.dylib

all: $(LIB)

$(LIB): drive.c
	$(CC) $(CFLAGS) -dynamiclib -Wl,-U,_SDL_PollEvent -o $@ drive.c

# Optional: compare the SDL 1.2 and INPUT offsets hard-coded in drive.c with
# the real headers (needs sdl-config and the game's src/input.h).
check-layout: layout_check.c
	$(CC) -Wall -o layout_check layout_check.c $$(sdl-config --cflags) && ./layout_check; rc=$$?; rm -f layout_check; exit $$rc

clean:
	rm -f $(LIB)

.PHONY: all clean check-layout
