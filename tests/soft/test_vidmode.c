/*
 * Unit test for the software renderer's video-mode clamp and fallback in
 * src/soft.c change_vidmode() (S-16).
 *
 * soft.c is compiled into this file; SDL and libpng are replaced by stubs,
 * so the test needs SDL 1.2 headers but no display and no SDL library.  The
 * SDL_SetVideoMode stub records every request and can be told to fail
 * full-screen, hardware or all requests, which a real desktop will not do on
 * demand.  run.sh builds it with -fsanitize=address,undefined.
 *
 * Build and run: tests/soft/run.sh
 */
#include "../../src/soft.c"
#undef main   /* SDL_main.h renames main on macOS */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- the game-side symbols soft.c refers to ---- */
char *STRam;
INPUT input;
int currentTimeMode;
int GetReg (int reg) { (void) reg; return 0; }
void SetReg (int reg, int val) { (void) reg; (void) val; }
int MemReadLong (p68K pos) { (void) pos; return 0; }
const char *mode_name (void) { return ""; }
int in_mouse_mode (void) { return 0; }

/* ---- SDL stubs ---- */
static SDL_VideoInfo video_info;
static SDL_Surface surface;
static int fail_fullscreen, fail_hw, fail_all;
#define MAX_CALLS 64
static struct { int w, h; Uint32 flags; } calls[MAX_CALLS];
static int ncalls;

SDL_Surface *SDL_SetVideoMode (int w, int h, int bpp, Uint32 flags)
{
	(void) bpp;
	if (ncalls < MAX_CALLS) {
		calls[ncalls].w = w; calls[ncalls].h = h; calls[ncalls].flags = flags;
	}
	ncalls++;
	if (fail_all || (fail_fullscreen && (flags & SDL_FULLSCREEN)) ||
	    (fail_hw && (flags & SDL_HWSURFACE)))
		return NULL;
	surface.w = w;
	surface.h = h;
	video_info.current_w = w;   /* as SDL 1.2 does after a mode set */
	video_info.current_h = h;
	return &surface;
}
const SDL_VideoInfo *SDL_GetVideoInfo (void) { return &video_info; }
char *SDL_GetError (void) { return (char *) "stub failure"; }
void SDL_Quit (void) { }
int SDL_ShowCursor (int toggle) { (void) toggle; return 0; }
void SDL_WM_SetCaption (const char *t, const char *i) { (void) t; (void) i; }
Uint8 SDL_EventState (Uint8 type, int state) { (void) type; (void) state; return 0; }
SDL_Surface *SDL_CreateRGBSurface (Uint32 f, int w, int h, int d, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{ (void) f; (void) w; (void) h; (void) d; (void) r; (void) g; (void) b; (void) a; return NULL; }
int SDL_Flip (SDL_Surface *s) { (void) s; return 0; }
void SDL_FreeSurface (SDL_Surface *s) { (void) s; }
int SDL_LockSurface (SDL_Surface *s) { (void) s; return 0; }
void SDL_UnlockSurface (SDL_Surface *s) { (void) s; }
int SDL_UpperBlit (SDL_Surface *s, SDL_Rect *sr, SDL_Surface *d, SDL_Rect *dr)
{ (void) s; (void) sr; (void) d; (void) dr; return 0; }
#ifdef NO_SDL_IMAGE
int png_image_begin_read_from_file (png_imagep image, const char *file_name)
{ (void) image; (void) file_name; return 0; }
int png_image_finish_read (png_imagep image, png_const_colorp bg, void *buf, png_int_32 stride, void *cmap)
{ (void) image; (void) bg; (void) buf; (void) stride; (void) cmap; return 0; }
void png_image_free (png_imagep image) { (void) image; }
#endif

/* ---- the test ---- */
static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* A fresh start: desktop dw x dh (0 = unknown), a --size request, windowed. */
static void reset (int dw, int dh, int w, int h)
{
	video_info.current_w = dw;
	video_info.current_h = dh;
	desk_w = desk_h = -1;
	screen_w = w;
	screen_h = h;
	bInFullScreen = FALSE;
	sdlscrn = NULL;
	fail_fullscreen = fail_hw = fail_all = 0;
	ncalls = 0;
}

static void check_size (const char *what, int w, int h)
{
	CHECK(sdlscrn != NULL, "%s: no surface", what);
	CHECK(screen_w == w && screen_h == h, "%s: got %dx%d, want %dx%d", what, screen_w, screen_h, w, h);
	CHECK(ncalls >= 1 && ncalls <= MAX_CALLS, "%s: %d SDL_SetVideoMode calls", what, ncalls);
	if (ncalls >= 1 && ncalls <= MAX_CALLS)
		CHECK(calls[ncalls - 1].w == w && calls[ncalls - 1].h == h,
		      "%s: last SDL_SetVideoMode asked for %dx%d, want %dx%d", what,
		      calls[ncalls - 1].w, calls[ncalls - 1].h, w, h);
}

int main (void)
{
	int i, status;
	pid_t pid;

	/* The verifier's case: --size 20000 20000 on a 3440x1440 desktop must ask
	 * SDL for no more than the desktop (it aborted in Metal before). */
	reset (3440, 1440, 20000, 20000);
	Screen_Init ();
	check_size ("--size 20000 20000, desktop 3440x1440", 3440, 1440);
	CHECK(ncalls == 1, "--size 20000 20000: %d SDL_SetVideoMode calls, want 1", ncalls);

	/* Desktop size unknown: the hard cap applies. */
	reset (0, 0, 20000, 20000);
	Screen_Init ();
	check_size ("--size 20000 20000, desktop unknown", SOFT_MAX_DIM, SOFT_MAX_DIM);

	/* Too small: the 320x200 minimum (draw_control_panel writes 320x200). */
	reset (3440, 1440, 1, 1);
	Screen_Init ();
	check_size ("--size 1 1", 320, 200);
	reset (3440, 1440, 100, 100);
	Screen_Init ();
	check_size ("--size 100 100", 320, 200);
	reset (3440, 1440, -5, 0);
	Screen_Init ();
	check_size ("--size -5 0", 320, 200);

	/* An ordinary size passes through unchanged. */
	reset (3440, 1440, 640, 400);
	Screen_Init ();
	check_size ("--size 640 400", 640, 400);

	/* The desktop is read once: after a mode set SDL reports the window, and
	 * that must not shrink the cap for a later full-screen toggle. */
	reset (3440, 1440, 640, 400);
	Screen_Init ();
	screen_w = 1920; screen_h = 1200;
	Screen_ToggleFullScreen ();
	check_size ("toggle to 1920x1200 after a 640x400 window", 1920, 1200);
	CHECK(bInFullScreen == TRUE, "toggle: not full screen");

	/* Full screen fails: fall back to the same size in a window. */
	reset (3440, 1440, 640, 400);
	Screen_Init ();
	fail_fullscreen = 1;
	ncalls = 0;
	Screen_ToggleFullScreen ();
	check_size ("full screen refused", 640, 400);
	CHECK(bInFullScreen == FALSE, "full screen refused: still flagged full screen");
	CHECK(ncalls == 2 && (calls[0].flags & SDL_FULLSCREEN) && !(calls[1].flags & SDL_FULLSCREEN),
	      "full screen refused: calls %d, want full screen then window", ncalls);
	/* ... and again, with full screen still refused, several times. */
	ncalls = 0;
	for (i = 0; i < 6; i++)
		Screen_ToggleFullScreen ();
	check_size ("6 refused toggles", 640, 400);
	CHECK(ncalls == 12, "6 refused toggles: %d calls, want 12", ncalls);

	/* Hardware surfaces refused: the 320x200 software window. */
	reset (3440, 1440, 1280, 800);
	fail_hw = 1;
	Screen_Init ();
	check_size ("hardware surfaces refused", 320, 200);
	CHECK(ncalls >= 1 && ncalls <= MAX_CALLS && !(calls[ncalls - 1].flags & SDL_HWSURFACE),
	      "fallback still asked for a hardware surface");

	/* Full screen at start-up with both full screen and hardware refused:
	 * full screen, then the window, then 320x200 software. */
	reset (3440, 1440, 1280, 800);
	bInFullScreen = TRUE;
	fail_fullscreen = fail_hw = 1;
	Screen_Init ();
	check_size ("full screen and hardware refused", 320, 200);
	CHECK(ncalls == 3, "full screen and hardware refused: %d calls, want 3", ncalls);

	/* Nothing works at all: the game exits -1, as before (there is no
	 * picture to show).  Run it in a child so this test survives. */
	fflush (stdout);
	pid = fork ();
	if (pid == 0) {
		reset (3440, 1440, 640, 400);
		fail_all = 1;
		freopen ("/dev/null", "w", stderr);
		Screen_Init ();
		_exit (0);   /* reached only if change_vidmode returned */
	}
	CHECK(pid > 0 && waitpid (pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 255, "every mode refused: want exit(-1)");

	printf ("soft vidmode test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
