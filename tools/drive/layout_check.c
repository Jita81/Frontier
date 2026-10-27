/* layout_check.c - `make check-layout`: prints PASS if the SDL 1.2 event and
 * INPUT offsets hard-coded in drive.c match the real headers. */
#include <stddef.h>
#include <stdio.h>
#include <SDL.h>
typedef int BOOL;
#include "../../src/input.h"
#include "../../src/shortcut.h"
#undef main
#define CHECK(expr) do { if (!(expr)) { printf("FAIL: %s\n", #expr); bad = 1; } } while (0)
int main(void)
{
	int bad = 0;
	CHECK(sizeof(SDL_Event) == 24);
	CHECK(offsetof(SDL_KeyboardEvent, keysym.scancode) == 4);
	CHECK(offsetof(SDL_KeyboardEvent, keysym.sym) == 8);
	CHECK(offsetof(SDL_KeyboardEvent, keysym.mod) == 12);
	CHECK(SDL_KEYDOWN == 2 && SDL_KEYUP == 3 && SDL_MOUSEMOTION == 4);
	CHECK(SDL_MOUSEBUTTONDOWN == 5 && SDL_MOUSEBUTTONUP == 6 && SDL_QUIT == 12);
	CHECK(KMOD_LSHIFT == 0x1 && KMOD_LCTRL == 0x40 && KMOD_RCTRL == 0x80);
	CHECK(KMOD_LALT == 0x100 && KMOD_LMETA == 0x400 && KMOD_RMETA == 0x800 && KMOD_MODE == 0x4000);
	CHECK(SDLK_F1 == 282 && SDLK_F15 == 296 && SDLK_UP == 273 && SDLK_KP0 == 256);
	CHECK(SDLK_HELP == 315 && SDLK_UNDO == 322 && SDLK_LCTRL == 306 && SDLK_RMETA == 309);
	CHECK(offsetof(INPUT, key_buf) == 323 && SIZE_KEYBUF == 16);
	CHECK(offsetof(INPUT, buf_tail) == 360);
	CHECK(offsetof(INPUT, cur_mousebut_state) == 372);
	CHECK(offsetof(INPUT, motion_x) == 376 && offsetof(INPUT, motion_y) == 380);
	CHECK(offsetof(SHORTCUT_KEY, Key) == 0 && sizeof(ShortCutKey.Key) == 2);
	CHECK(offsetof(INPUT, abs_x) == 384 && offsetof(INPUT, abs_y) == 388);
	CHECK(sizeof(INPUT) == 400);
	puts(bad ? "layout: FAIL" : "layout: PASS");
	return bad;
}
