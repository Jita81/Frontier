/*
 * Unit test for src/keymap.c on macOS (S-3, docs/07 I-10).
 *
 * keymap.c is compiled into this file with every outside dependency stubbed
 * (SDL calls, Input_PressSTKey, the shortcut and joystick hooks), and
 * synthetic SDL_keysym values are fed through Keymap_RemapKeyToSTScanCode,
 * Keymap_KeyDown and Keymap_KeyUp.  Scancodes are Mac virtual key codes
 * (kVK_* in HIToolbox/Events.h), which is what sdl12-compat reports.
 *
 * Build and run: tests/keymap/run.sh
 */
#include "../../src/keymap.c"
#undef main   /* SDL_main.h renames main on macOS */

#include <stdio.h>
#include <string.h>

#ifndef __APPLE__
#error "this test covers the macOS key-code table; build it on macOS"
#endif

/* ---- stubs for keymap.c's outside world -------------------------------- */
INPUT input;
SHORTCUT_KEY ShortCutKey;
static SDLMod stub_mod = KMOD_NONE;

struct ev { int code; int press; };
static struct ev events[64];
static int nevents;

void Input_PressSTKey(unsigned char ScanCode, BOOL bPress)
{
	if (nevents < 64) {
		events[nevents].code = ScanCode;
		events[nevents].press = bPress ? 1 : 0;
	}
	nevents++;
}
void joystick_read_config(const char *path) { (void) path; }
SDLMod SDL_GetModState(void) { return stub_mod; }
int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int i) { (void) i; return NULL; }
void SDL_JoystickClose(SDL_Joystick *j) { (void) j; }
char *SDL_GetKeyName(SDLKey key) { (void) key; return "stub"; }

/* ---- helpers ------------------------------------------------------------ */
static int failures, checks;

static SDL_keysym key(int scancode, int sym)
{
	SDL_keysym k;
	memset(&k, 0, sizeof k);
	k.scancode = (Uint8) scancode;
	k.sym = (SDLKey) sym;
	k.mod = KMOD_NONE;
	return k;
}

static void expect_map(const char *what, int scancode, int sym, int want)
{
	SDL_keysym k = key(scancode, sym);
	int got = (signed char) Keymap_RemapKeyToSTScanCode(&k);
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL %-28s kVK=0x%02x sym=%d: got %d (0x%02x), want %d (0x%02x)\n",
		       what, scancode, sym, got, got & 0xff, want, want & 0xff);
	}
}

static void reset_events(void)
{
	nevents = 0;
	memset(&input, 0, sizeof input);
	memset(&ShortCutKey, 0, sizeof ShortCutKey);
}

static void expect_events(const char *what, int n, const struct ev *want)
{
	int i, ok = (nevents == n);
	for (i = 0; ok && i < n; i++)
		ok = events[i].code == want[i].code && events[i].press == want[i].press;
	checks++;
	if (!ok) {
		failures++;
		printf("FAIL %-28s got %d event(s):", what, nevents);
		for (i = 0; i < nevents && i < 64; i++)
			printf(" (0x%02x,%s)", events[i].code, events[i].press ? "down" : "up");
		printf("; want %d\n", n);
	}
}

/* QWERTY letters, digits and punctuation: kVK code, SDL sym, ST scancode. */
static const struct { const char *name; int kvk, sym, st; } table[] = {
	{"Escape", 0x35, SDLK_ESCAPE, 0x01},
	{"1", 0x12, SDLK_1, 0x02}, {"2", 0x13, SDLK_2, 0x03}, {"3", 0x14, SDLK_3, 0x04},
	{"4", 0x15, SDLK_4, 0x05}, {"5", 0x17, SDLK_5, 0x06}, {"6", 0x16, SDLK_6, 0x07},
	{"7", 0x1A, SDLK_7, 0x08}, {"8", 0x1C, SDLK_8, 0x09}, {"9", 0x19, SDLK_9, 0x0a},
	{"0", 0x1D, SDLK_0, 0x0b}, {"-", 0x1B, SDLK_MINUS, 0x0c}, {"=", 0x18, SDLK_EQUALS, 0x0d},
	{"Backspace", 0x33, SDLK_BACKSPACE, 0x0e}, {"Tab", 0x30, SDLK_TAB, 0x0f},
	{"q", 0x0C, SDLK_q, 0x10}, {"w", 0x0D, SDLK_w, 0x11}, {"e", 0x0E, SDLK_e, 0x12},
	{"r", 0x0F, SDLK_r, 0x13}, {"t", 0x11, SDLK_t, 0x14}, {"y", 0x10, SDLK_y, 0x15},
	{"u", 0x20, SDLK_u, 0x16}, {"i", 0x22, SDLK_i, 0x17}, {"o", 0x1F, SDLK_o, 0x18},
	{"p", 0x23, SDLK_p, 0x19}, {"[", 0x21, SDLK_LEFTBRACKET, 0x1a},
	{"]", 0x1E, SDLK_RIGHTBRACKET, 0x1b}, {"Return", 0x24, SDLK_RETURN, 0x1c},
	{"Left Control", 0x3B, SDLK_LCTRL, 0x1d},
	{"a", 0x00, SDLK_a, 0x1e}, {"s", 0x01, SDLK_s, 0x1f}, {"d", 0x02, SDLK_d, 0x20},
	{"f", 0x03, SDLK_f, 0x21}, {"g", 0x05, SDLK_g, 0x22}, {"h", 0x04, SDLK_h, 0x23},
	{"j", 0x26, SDLK_j, 0x24}, {"k", 0x28, SDLK_k, 0x25}, {"l", 0x25, SDLK_l, 0x26},
	{";", 0x29, SDLK_SEMICOLON, 0x27}, {"'", 0x27, SDLK_QUOTE, 0x28},
	{"`", 0x32, SDLK_BACKQUOTE, 0x29}, {"Left Shift", 0x38, SDLK_LSHIFT, 0x2a},
	{"\\", 0x2A, SDLK_BACKSLASH, 0x2b},
	{"z", 0x06, SDLK_z, 0x2c}, {"x", 0x07, SDLK_x, 0x2d}, {"c", 0x08, SDLK_c, 0x2e},
	{"v", 0x09, SDLK_v, 0x2f}, {"b", 0x0B, SDLK_b, 0x30}, {"n", 0x2D, SDLK_n, 0x31},
	{"m", 0x2E, SDLK_m, 0x32}, {",", 0x2B, SDLK_COMMA, 0x33}, {".", 0x2F, SDLK_PERIOD, 0x34},
	{"/", 0x2C, SDLK_SLASH, 0x35}, {"Right Shift", 0x3C, SDLK_RSHIFT, 0x36},
	{"Option", 0x3A, SDLK_LALT, 0x38}, {"Space", 0x31, SDLK_SPACE, 0x39},
	{"Caps Lock", 0x39, SDLK_CAPSLOCK, 0x3a},
	/* F1-F10: standard function keys, and fn+F-key (same kVK code). */
	{"F1", 0x7A, SDLK_F1, 0x3b}, {"F2", 0x78, SDLK_F2, 0x3c}, {"F3", 0x63, SDLK_F3, 0x3d},
	{"F4", 0x76, SDLK_F4, 0x3e}, {"F5", 0x60, SDLK_F5, 0x3f}, {"F6", 0x61, SDLK_F6, 0x40},
	{"F7", 0x62, SDLK_F7, 0x41}, {"F8", 0x64, SDLK_F8, 0x42}, {"F9", 0x65, SDLK_F9, 0x43},
	{"F10", 0x6D, SDLK_F10, 0x44},
	/* ISO section key, left of 1 on ISO keyboards. */
	{"ISO section", 0x0A, SDLK_WORLD_7, 0x60},
};

int main(void)
{
	unsigned i;

	/* 1. Every table key, by Mac key code. */
	for (i = 0; i < sizeof table / sizeof table[0]; i++)
		expect_map(table[i].name, table[i].kvk, table[i].sym, table[i].st);

	/* 2. Zero-scancode guard: kVK 0 is A only when the symbol says A. */
	expect_map("scancode 0, sym a", 0x00, SDLK_a, 0x1e);
	expect_map("scancode 0, sym unknown", 0x00, SDLK_UNKNOWN, -1);
	expect_map("scancode 0, sym F13", 0x00, SDLK_F13, -1);
	expect_map("scancode 0, sym world_0", 0x00, SDLK_WORLD_0, -1);
	expect_map("scancode 0, sym menu", 0x00, SDLK_MENU, -1);
	expect_map("scancode 0, sym q", 0x00, SDLK_q, -1);  /* see run.sh: AZERTY */
	/* Unmapped Mac key codes emit nothing. */
	expect_map("kVK F13 (0x69)", 0x69, SDLK_F13, -1);
	expect_map("kVK Help (0x72)", 0x72, SDLK_HELP, -1);
	expect_map("kVK keypad = (0x51)", 0x51, SDLK_KP_EQUALS, -1);
	expect_map("kVK out of table", 0xC0, SDLK_WORLD_1, -1);

	/* 3. Keys mapped by symbol. */
	expect_map("Up", 0x7E, SDLK_UP, 0x48);
	expect_map("Down", 0x7D, SDLK_DOWN, 0x50);
	expect_map("Left", 0x7B, SDLK_LEFT, 0x4b);
	expect_map("Right", 0x7C, SDLK_RIGHT, 0x4d);
	expect_map("Home", 0x73, SDLK_HOME, 0x47);
	expect_map("Delete (fwd)", 0x75, SDLK_DELETE, 0x53);
	expect_map("Right Control", 0x3E, SDLK_RCTRL, 0x1d);
	expect_map("Right Option", 0x3D, SDLK_RALT, 0x38);
	expect_map("KP /", 0x4B, SDLK_KP_DIVIDE, 0x65);
	expect_map("KP *", 0x43, SDLK_KP_MULTIPLY, 0x66);
	expect_map("KP -", 0x4E, SDLK_KP_MINUS, 0x4a);
	expect_map("KP +", 0x45, SDLK_KP_PLUS, 0x4e);
	expect_map("KP .", 0x41, SDLK_KP_PERIOD, 0x71);
	expect_map("KP Enter", 0x4C, SDLK_KP_ENTER, 0x72);
	expect_map("sym out of range", 0x00, SDLK_LAST + 5, -1);

	/* 4. Keypad digits, NumLock on and off. */
	{
		static const int kvk[10] = {0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5B,0x5C};
		static const int on[10]  = {0x70,0x6d,0x6e,0x6f,0x6a,0x6b,0x6c,0x67,0x68,0x69};
		static const int off[10] = {0x70,0x6d,0x50,0x6f,0x4b,0x50,0x4d,0x52,0x48,0x47};
		char name[32];
		int d;
		stub_mod = KMOD_NUM;
		for (d = 0; d < 10; d++) {
			snprintf(name, sizeof name, "KP%d NumLock on", d);
			expect_map(name, kvk[d], SDLK_KP0 + d, on[d]);
		}
		stub_mod = KMOD_NONE;
		for (d = 0; d < 10; d++) {
			snprintf(name, sizeof name, "KP%d NumLock off", d);
			expect_map(name, kvk[d], SDLK_KP0 + d, off[d]);
		}
		/* Fallback by position if a keypad key arrives with another sym. */
		expect_map("kVK KP Enter, sym Return", 0x4C, SDLK_RETURN, 0x72);
		expect_map("kVK KP 5, sym 5", 0x57, SDLK_5, 0x6b);
	}

	/* 5. Key down/up event flow. */
	{
		SDL_keysym k;
		const struct ev f1[] = {{0x3b, 1}, {0x3b, 0}};
		const struct ev a[] = {{0x1e, 1}, {0x1e, 0}};

		reset_events();
		k = key(0x7A, SDLK_F1);
		Keymap_KeyDown(&k);
		Keymap_KeyDown(&k);   /* auto-repeat: no second press */
		Keymap_KeyUp(&k);
		expect_events("F1 down, repeat, up", 2, f1);

		reset_events();
		k = key(0x00, SDLK_a);
		Keymap_KeyDown(&k);
		Keymap_KeyUp(&k);
		expect_events("A down/up (kVK 0)", 2, a);

		reset_events();
		k = key(0x00, SDLK_UNKNOWN);
		Keymap_KeyDown(&k);
		Keymap_KeyUp(&k);
		expect_events("unknown key: no phantom A", 0, NULL);

		reset_events();
		k = key(0x37, SDLK_LMETA);  /* Command */
		Keymap_KeyDown(&k);
		Keymap_KeyUp(&k);
		expect_events("Command is not sent", 0, NULL);

		reset_events();
		k = key(0x67, SDLK_F11);
		Keymap_KeyDown(&k);
		checks++;
		if (ShortCutKey.Key != SDLK_F11 || nevents != 0) {
			failures++;
			printf("FAIL F11 should be a shortcut (Key=%d, events=%d)\n", ShortCutKey.Key, nevents);
		}

		reset_events();
		k = key(0x0C, SDLK_q);
		k.mod = KMOD_LCTRL;
		Keymap_KeyDown(&k);
		checks++;
		if (ShortCutKey.Key != SDLK_q || !ShortCutKey.bCtrlPressed || nevents != 0) {
			failures++;
			printf("FAIL Ctrl-Q should be a shortcut (Key=%d ctrl=%d events=%d)\n",
			       ShortCutKey.Key, ShortCutKey.bCtrlPressed, nevents);
		}

		reset_events();
		k = key(0x00, SDLK_LAST + 5);
		Keymap_KeyDown(&k);
		Keymap_KeyUp(&k);
		expect_events("sym out of range is ignored", 0, NULL);
	}

	printf("keymap test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
