/*
 * Unit test for the joystick.ini button bounds in src/joystick.c (S-17).
 *
 * joystick.c and ini.c are compiled into this file with the input layer
 * stubbed.  A joystick.ini with out-of-range buttons (16, 99, -1) must be
 * skipped with a warning, in-range buttons must still work, and button
 * events past the table must be ignored.  run.sh builds it with
 * -fsanitize=address,undefined so any out-of-bounds index aborts the run.
 *
 * Build and run: tests/joystick/run.sh
 */
#include "../../src/ini.c"
#include "../../src/joystick.c"
#undef main   /* SDL_main.h renames main on macOS */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

INPUT input;
int currentTimeMode;
int screen_w = 320, screen_h = 200;
int delta_x, delta_y, abs_delta_x, abs_delta_y;

static int presses[256], mouse_presses;
void Input_PressSTKey(unsigned char ScanCode, BOOL bPress) { if (bPress) presses[ScanCode]++; }
void Input_MousePress(int button) { (void) button; mouse_presses++; }
void Input_MouseRelease(int button) { (void) button; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	char path[] = "/tmp/joystick-ini-XXXXXX";
	int fd = mkstemp(path);
	FILE *f;
	unsigned b;

	if (fd < 0) { perror("mkstemp"); return 2; }
	f = fdopen(fd, "w");
	fputs("[Aventure Mode]\n"
	      "LASER = 99\n"        /* out of range: was a write past the table */
	      "BOMB = 16\n"         /* one past the end */
	      "MISSILE = -1\n"      /* negative */
	      "THRUST = 15\n"       /* last valid button */
	      "RADAR = 0\n"         /* first valid button */
	      "[Battle Mode]\n"
	      "EJECT = 300\n", f);
	fclose(f);

	current_mode = MODE_AVENTURE;
	joystick_read_config(path);
	unlink(path);

	CHECK(JoystickButtonToSTScanCode[MODE_AVENTURE][15] == THRUST, "button 15 should map to THRUST");
	CHECK(JoystickButtonToSTScanCode[MODE_AVENTURE][0] == RADAR, "button 0 should map to RADAR");
	for (b = 1; b < 15; b++)
		CHECK(JoystickButtonToSTScanCode[MODE_AVENTURE][b] == 0, "button %u should be unmapped", b);
	for (b = 0; b < JS_NB_BUTTONS_MAX; b++)
		CHECK(JoystickButtonToSTScanCode[MODE_BATTLE][b] == 0, "battle button %u should be unmapped", b);

	Keymap_JoystickUpDown(15, 1);
	Keymap_JoystickUpDown(15, 0);
	CHECK(presses[THRUST] == 1, "button 15 press should send THRUST once (got %d)", presses[THRUST]);
	Keymap_JoystickUpDown(16, 1);   /* was read one past the table */
	Keymap_JoystickUpDown(99, 1);
	Keymap_JoystickUpDown(0xFFFFFFFFu, 1);
	CHECK(presses[LASER] == 0 && presses[BOMB] == 0 && presses[MISSILE] == 0,
	      "out-of-range buttons must not send keys");

	printf("joystick test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
