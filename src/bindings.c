/*
 * Rebindable controls (bindings.h).  Host-side only: the game still reads
 * ST scan codes and the ST mouse; this file decides which SDL key or
 * button produces which of them.
 *
 * The model: every action has one key and one mouse button.  A key is
 * named by the ST scan code the physical key sends by default, so a
 * binding works whatever the host keyboard is.  The table remap[] turns
 * the code a key would send into the code it sends now; with the default
 * bindings it is the identity and this layer passes everything through
 * exactly as before (bindings_are_default).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <SDL.h>

#include "main.h"
#include "input.h"
#include "screen.h"
#include "oracle.h"
#include "bindings.h"
#include "../m68000.h"
#include "../fixups.h"
#ifdef WITH_GL
#include "hdui.h"
#endif

#define A6		__DL5eb6_a6_base
#define CONTROLS_FILE	"controls.ini"

/* ST scan codes the game reads (joystick.c has the same) */
#define ST_ESC		0x01
#define ST_SPACE	0x39

const struct binding_action binding_actions[] = {
	/* Flight */
	{ "STEER", "Steer (hold and move the mouse)",
	  "Hold this button and move the mouse to turn the ship. It is the game's right button.",
	  G_FLIGHT, K_STEER, 0, MB_RIGHT },
	{ "THRUST", "Thrust forward",
	  "Main engines forward while held (joystick.ini THRUST).", G_FLIGHT, K_KEY, 0x1c, 0 },
	{ "RTHRUST", "Thrust reverse",
	  "Retro thrust while held (joystick.ini RTHRUST).", G_FLIGHT, K_KEY, 0x36, 0 },
	{ "MOVE_UP", "Move up",
	  "The game's Z key (joystick.ini MOVE_UP).", G_FLIGHT, K_KEY, 0x2c, 0 },
	{ "MOVE_DOWN", "Move down",
	  "The game's A key (joystick.ini MOVE_DOWN).", G_FLIGHT, K_KEY, 0x1e, 0 },
	{ "MOVE_LEFT", "Move left",
	  "The game's comma key (joystick.ini MOVE_LEFT).", G_FLIGHT, K_KEY, 0x33, 0 },
	{ "MOVE_RIGHT", "Move right",
	  "The game's full stop key (joystick.ini MOVE_RIGHT).", G_FLIGHT, K_KEY, 0x34, 0 },
	{ "HYPERSPACE", "Hyperspace",
	  "Engage the hyperdrive to the selected system (joystick.ini HYPERSPACE).", G_FLIGHT, K_KEY, 0x23, 0 },
	/* Weapons */
	{ "LASER", "Fire laser",
	  "Fires the laser while held. Bind it to the left button with the switch above.", G_WEAPONS, K_KEY, ST_SPACE, 0 },
	{ "MISSILE", "Fire missile",
	  "Launches a missile at the target (joystick.ini MISSILE).", G_WEAPONS, K_KEY, 0x32, 0 },
	{ "ECM", "ECM",
	  "Electronic countermeasures against incoming missiles.", G_WEAPONS, K_KEY, 0x12, 0 },
	{ "BOMB", "Energy bomb",
	  "Detonates the energy bomb, if fitted (joystick.ini BOMB).", G_WEAPONS, K_KEY, 0x30, 0 },
	/* Views and screens */
	{ "F1", "Console button 1 (F1)", "Presses the console's first button: the views in flight.", G_VIEWS, K_KEY, 0x3b, 0 },
	{ "F2", "Console button 2 (F2)", "Presses the console's second button.", G_VIEWS, K_KEY, 0x3c, 0 },
	{ "F3", "Console button 3 (F3)", "Presses the console's third button.", G_VIEWS, K_KEY, 0x3d, 0 },
	{ "F4", "Console button 4 (F4)", "Presses the console's fourth button.", G_VIEWS, K_KEY, 0x3e, 0 },
	{ "F5", "Console button 5 (F5)", "Presses the console's fifth button.", G_VIEWS, K_KEY, 0x3f, 0 },
	{ "F6", "Console button 6 (F6)", "Presses the console's sixth button.", G_VIEWS, K_KEY, 0x40, 0 },
	{ "F7", "Console button 7 (F7)", "Presses the console's seventh button.", G_VIEWS, K_KEY, 0x41, 0 },
	{ "F8", "Console button 8 (F8)", "Presses the console's eighth button.", G_VIEWS, K_KEY, 0x42, 0 },
	{ "F9", "Console button 9 (F9)", "Presses the console's ninth button.", G_VIEWS, K_KEY, 0x43, 0 },
	{ "F10", "Console button 10 (F10)", "Presses the console's tenth button.", G_VIEWS, K_KEY, 0x44, 0 },
	{ "LOOK_UP", "Look up", "Turns the view up (joystick.ini LOOK_UP).", G_VIEWS, K_KEY, 0x48, 0 },
	{ "LOOK_DOWN", "Look down", "Turns the view down (joystick.ini LOOK_DOWN).", G_VIEWS, K_KEY, 0x50, 0 },
	{ "LOOK_LEFT", "Look left", "Turns the view left (joystick.ini LOOK_LEFT).", G_VIEWS, K_KEY, 0x4b, 0 },
	{ "LOOK_RIGHT", "Look right", "Turns the view right (joystick.ini LOOK_RIGHT).", G_VIEWS, K_KEY, 0x4d, 0 },
	{ "RADAR", "Radar", "The game's R key (joystick.ini RADAR).", G_VIEWS, K_KEY, 0x13, 0 },
	/* Time */
	{ "PAUSE", "Pause / game menu",
	  "Pauses the game; pressed again while paused it opens the game menu.", G_TIME, K_KEY, ST_ESC, 0 },
	{ "TIME_FASTER", "Time faster",
	  "Clicks the next faster time control on the console. Not bound by default.", G_TIME, K_TIME, 1, 0 },
	{ "TIME_SLOWER", "Time slower",
	  "Clicks the next slower time control on the console (down to stopped). Not bound by default.",
	  G_TIME, K_TIME, 0, 0 },
	/* Maps */
	{ "MAP_CENTER", "Centre map", "Centres the map (joystick.ini MAP_CENTER).", G_MAPS, K_KEY, 0x2e, 0 },
	{ "ZOOM_IN", "Zoom in", "Zooms the map or external view in (joystick.ini ZOOM_IN).", G_MAPS, K_KEY, 0x4a, 0 },
	{ "ZOOM_OUT", "Zoom out", "Zooms the map or external view out (joystick.ini ZOOM_OUT).", G_MAPS, K_KEY, 0x4e, 0 },
	/* Other */
	{ "CLICK", "Click, select a target",
	  "The left button clicks the game's buttons and picks targets; it cannot be moved. Shift + left click always targets.",
	  G_OTHER, K_CLICK, 0, MB_LEFT },
	{ "EJECT", "Eject", "Launches the escape capsule, if fitted (joystick.ini EJECT).", G_OTHER, K_KEY, 0x2d, 0 },
	{ "MB4_PHOTO", "Photo (D)", "The game's D key (joystick.ini MB4_PHOTO).", G_OTHER, K_KEY, 0x20, 0 },
	{ "ALT", "Alternate", "The ST's Alternate key, Option on a Mac (joystick.ini ALT).", G_OTHER, K_KEY, 0x38, 0 },
};
const int n_binding_actions = (int) (sizeof (binding_actions) / sizeof (binding_actions[0]));

#define NA	((int) (sizeof (binding_actions) / sizeof (binding_actions[0])))

unsigned char binding_key[NA];
unsigned char binding_mouse[NA];

static int is_default = 1;
static signed short remap[128];		/* the code a key sends -> the code it sends now (-1 none) */
static int mouse_action[MB_COUNT];	/* the action on each button, -1 none */
static unsigned char held[128];		/* sources holding each ST code down */
static int left_active = -1;		/* the action the left button is holding, or -1 */
static int steer_down;			/* the game's right button is held by the steer binding */
static int mouse_active[MB_COUNT];	/* the action a button is holding (key or time), or -1 */
static int debug = -1;

/* ---------------------------------------------------------------- */
/* Names */

static const char *const st_names[128] = {
	[0x01] = "Esc", [0x02] = "1", [0x03] = "2", [0x04] = "3", [0x05] = "4", [0x06] = "5",
	[0x07] = "6", [0x08] = "7", [0x09] = "8", [0x0a] = "9", [0x0b] = "0", [0x0c] = "-",
	[0x0d] = "=", [0x0e] = "Backspace", [0x0f] = "Tab",
	[0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R", [0x14] = "T", [0x15] = "Y",
	[0x16] = "U", [0x17] = "I", [0x18] = "O", [0x19] = "P", [0x1a] = "[", [0x1b] = "]",
	[0x1c] = "Return", [0x1d] = "Control",
	[0x1e] = "A", [0x1f] = "S", [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H",
	[0x24] = "J", [0x25] = "K", [0x26] = "L", [0x27] = ";", [0x28] = "'", [0x29] = "`",
	[0x2a] = "Left Shift", [0x2b] = "\\",
	[0x2c] = "Z", [0x2d] = "X", [0x2e] = "C", [0x2f] = "V", [0x30] = "B", [0x31] = "N",
	[0x32] = "M", [0x33] = ",", [0x34] = ".", [0x35] = "/", [0x36] = "Right Shift",
	[0x38] = "Option", [0x39] = "Space", [0x3a] = "Caps Lock",
	[0x3b] = "F1", [0x3c] = "F2", [0x3d] = "F3", [0x3e] = "F4", [0x3f] = "F5",
	[0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9", [0x44] = "F10",
	[0x47] = "Home", [0x48] = "Up", [0x4a] = "Keypad -", [0x4b] = "Left", [0x4d] = "Right",
	[0x4e] = "Keypad +", [0x50] = "Down", [0x52] = "Insert", [0x53] = "Delete",
	[0x60] = "\xc2\xa7", [0x61] = "Undo", [0x62] = "Help",
	[0x63] = "Keypad (", [0x64] = "Keypad )", [0x65] = "Keypad /", [0x66] = "Keypad *",
	[0x67] = "Keypad 7", [0x68] = "Keypad 8", [0x69] = "Keypad 9",
	[0x6a] = "Keypad 4", [0x6b] = "Keypad 5", [0x6c] = "Keypad 6",
	[0x6d] = "Keypad 1", [0x6e] = "Keypad 2", [0x6f] = "Keypad 3",
	[0x70] = "Keypad 0", [0x71] = "Keypad .", [0x72] = "Keypad Enter",
};

static const char *const mb_names[MB_COUNT] = {
	"none", "Left", "Middle", "Right", "Wheel up", "Wheel down", "Button 4", "Button 5"
};
static const char *const mb_ini[MB_COUNT] = {
	"none", "left", "middle", "right", "wheelup", "wheeldown", "x1", "x2"
};
static const char *const group_names[G_COUNT] = {
	"Flight", "Weapons", "Views and screens", "Time", "Maps", "Other"
};

const char *binding_key_name (int st)
{
	return st > 0 && st < 128 ? st_names[st] : NULL;
}

const char *binding_mouse_name (int mb)
{
	return mb >= 0 && mb < MB_COUNT ? mb_names[mb] : "?";
}

const char *binding_group_name (int g)
{
	return g >= 0 && g < G_COUNT ? group_names[g] : "?";
}

int binding_action_index (const char *name)
{
	int i;
	for (i = 0; i < NA; i++)
		if (!strcmp (binding_actions[i].name, name)) return i;
	return -1;
}

static int default_key (int i)
{
	return binding_actions[i].kind == K_KEY ? binding_actions[i].code : 0;
}

/* ---------------------------------------------------------------- */
/* The tables */

void bindings_changed (void)
{
	int i, c;

	is_default = 1;
	for (i = 0; i < NA; i++)
		if (binding_key[i] != default_key (i) || binding_mouse[i] != binding_actions[i].def_mouse)
			is_default = 0;

	for (c = 0; c < 128; c++) remap[c] = c;
	/* an action's own key does nothing unless something is bound to it */
	for (i = 0; i < NA; i++)
		if (default_key (i)) remap[default_key (i)] = -1;
	for (i = 0; i < NA; i++)
		if (binding_key[i] && binding_actions[i].kind == K_KEY)
			remap[binding_key[i]] = binding_actions[i].code;
		else if (binding_key[i] && binding_actions[i].kind == K_TIME)
			remap[binding_key[i]] = -2 - i;		/* a time action */
	for (c = 0; c < MB_COUNT; c++) mouse_action[c] = -1;
	for (i = 0; i < NA; i++)
		if (binding_mouse[i] && binding_actions[i].kind != K_CLICK)
			mouse_action[binding_mouse[i]] = i;
}

void bindings_reset (void)
{
	int i;
	for (i = 0; i < NA; i++) {
		binding_key[i] = default_key (i);
		binding_mouse[i] = binding_actions[i].def_mouse;
	}
	bindings_changed ();
}

int bindings_are_default (void)
{
	return is_default;
}

int bindings_key_owner (int k, int skip)
{
	int i;
	if (!k) return -1;
	for (i = 0; i < NA; i++)
		if (i != skip && binding_key[i] == k) return i;
	return -1;
}

int bindings_mouse_owner (int mb, int skip)
{
	int i;
	if (!mb) return -1;
	for (i = 0; i < NA; i++)
		if (i != skip && binding_mouse[i] == mb) return i;
	return -1;
}

int bindings_left_fires_laser (void)
{
	int laser = binding_action_index ("LASER");
	return binding_mouse[laser] == MB_LEFT;
}

void bindings_set_left_fires_laser (int on)
{
	int laser = binding_action_index ("LASER");
	if (on) {
		int i;
		/* the left button is the laser's alone */
		for (i = 0; i < NA; i++)
			if (i != laser && binding_mouse[i] == MB_LEFT && binding_actions[i].kind != K_CLICK)
				binding_mouse[i] = 0;
		binding_mouse[laser] = MB_LEFT;
	} else if (binding_mouse[laser] == MB_LEFT)
		binding_mouse[laser] = 0;
	bindings_changed ();
}

/* ---------------------------------------------------------------- */
/* controls.ini: a [Controls] section of ACTION=key:NAME mouse:NAME lines
 * and LEFT_FIRES_LASER=0|1.  Only this file reads and writes it. */

static int parse_key (const char *s)
{
	int c;
	if (!strcasecmp (s, "none") || !*s) return 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		c = (int) strtol (s, NULL, 16);
		return c > 0 && c < 128 ? c : -1;
	}
	for (c = 1; c < 128; c++)
		if (st_names[c] && !strcasecmp (st_names[c], s)) return c;
	return -1;
}

static int parse_mouse (const char *s)
{
	int m;
	for (m = 0; m < MB_COUNT; m++)
		if (!strcasecmp (mb_ini[m], s)) return m;
	return -1;
}

static char *trim (char *s)
{
	char *e;
	while (*s && isspace ((unsigned char) *s)) s++;
	e = s + strlen (s);
	while (e > s && isspace ((unsigned char) e[-1])) *--e = 0;
	return s;
}

static void load (const char *path)
{
	FILE *f = fopen (path, "r");
	char line[256];
	int in_section = 0, lno = 0, left_laser = -1, first, later;

	if (!f) return;
	while (fgets (line, sizeof line, f)) {
		char *s = trim (line), *eq, *val, *tok;
		int i;
		lno++;
		if (!*s || *s == ';' || *s == '#') continue;
		if (*s == '[') {
			in_section = !strncasecmp (s, "[Controls]", 10);
			if (!strchr (s, ']'))
				fprintf (stderr, "%s:%d: malformed section header %s, its lines are skipped\n", path, lno, s);
			continue;
		}
		if (!in_section || !(eq = strchr (s, '='))) continue;
		*eq = 0;
		val = trim (eq + 1);
		s = trim (s);
		if (!strcasecmp (s, "LEFT_FIRES_LASER")) {
			left_laser = atoi (val) != 0;
			if (strcmp (val, "0") && strcmp (val, "1"))
				fprintf (stderr, "%s:%d: LEFT_FIRES_LASER=%s is not 0 or 1, read as %d\n",
					 path, lno, val, left_laser);
			continue;
		}
		if ((i = binding_action_index (s)) < 0) {
			fprintf (stderr, "%s:%d: unknown action %s, skipped\n", path, lno, s);
			continue;
		}
		/* key:NAME mouse:NAME, names may hold spaces ("Keypad +"), so
		 * split on the word "mouse:" */
		tok = strstr (val, "mouse:");
		if (tok) {
			int m;
			*tok = 0;
			m = parse_mouse (trim (tok + 6));
			if (m < 0 || (m == MB_LEFT && binding_actions[i].kind == K_STEER) ||
			    (binding_actions[i].kind == K_CLICK))
				fprintf (stderr, "%s:%d: bad mouse button for %s, skipped\n", path, lno, s);
			else
				binding_mouse[i] = m;
		}
		val = trim (val);
		if (!strncasecmp (val, "key:", 4) && binding_actions[i].kind != K_STEER &&
		    binding_actions[i].kind != K_CLICK) {
			int k = parse_key (trim (val + 4));
			if (k < 0) fprintf (stderr, "%s:%d: unknown key for %s, skipped\n", path, lno, s);
			else binding_key[i] = k;
		}
	}
	fclose (f);
	bindings_changed ();
	if (left_laser >= 0) bindings_set_left_fires_laser (left_laser);
	fprintf (stderr, "controls: read %s%s\n", path, is_default ? " (all defaults)" : "");
	if (bindings_clash (&first, &later))
		fprintf (stderr, "%s: %s and %s share a key or button; %s has it until one is rebound (F12)\n",
			 path, binding_actions[first].name, binding_actions[later].name, binding_actions[later].name);
}

int bindings_clash (int *a, int *b)
{
	int i, j;
	for (i = 0; i < NA; i++)
		for (j = i + 1; j < NA; j++)
			if ((binding_key[i] && binding_key[i] == binding_key[j]) ||
			    (binding_mouse[i] && binding_mouse[i] == binding_mouse[j] &&
			     binding_actions[i].kind != K_CLICK && binding_actions[j].kind != K_CLICK)) {
				*a = i;
				*b = j;
				return 1;
			}
	return 0;
}

int bindings_save (void)
{
	char tmp[64];
	FILE *f;
	int i;

	if (oracle.active) return 0;
	snprintf (tmp, sizeof tmp, "%s.tmp", CONTROLS_FILE);
	if (!(f = fopen (tmp, "w"))) {
		perror (tmp);
		return 0;
	}
	fprintf (f, "; Frontier controls, written by the game's Controls page (F12).\n"
		 "; ACTION=key:NAME mouse:NAME (none to unbind).  Delete this file for the defaults.\n"
		 "[Controls]\nLEFT_FIRES_LASER=%d\n", bindings_left_fires_laser ());
	for (i = 0; i < NA; i++) {
		const struct binding_action *a = &binding_actions[i];
		if (a->kind == K_CLICK) continue;
		fprintf (f, "%s=", a->name);
		if (a->kind != K_STEER) {
			const char *n = binding_key_name (binding_key[i]);
			if (!binding_key[i]) fprintf (f, "key:none ");
			else if (n) fprintf (f, "key:%s ", n);
			else fprintf (f, "key:0x%02x ", binding_key[i]);
		}
		fprintf (f, "mouse:%s\n", mb_ini[binding_mouse[i]]);
	}
	if (fclose (f) != 0 || rename (tmp, CONTROLS_FILE) != 0) {
		perror (CONTROLS_FILE);
		unlink (tmp);
		return 0;
	}
	return 1;
}

void bindings_init (void)
{
	const char *d = getenv ("FE2_CONTROLS_DEBUG");
	debug = d && *d && *d != '0';
	bindings_reset ();
	/* an oracle run never reads it: its bindings are the defaults */
	if (oracle.active) return;
	load (CONTROLS_FILE);
}

/* ---------------------------------------------------------------- */
/* Game state, read only */

static int rb (unsigned long a) { return STMemory_ReadByte (A6 + a) & 0xff; }
static unsigned long rl (unsigned long a) { return (unsigned long) STMemory_ReadLong (A6 + a); }

int bindings_in_flight_view (int x, int y)
{
	int fs = rb (704);
	int view = (rb (42) & 0x80) != 0;	/* the cockpit view screen is up */
	int clr3d = rb (536) & 1;		/* a 3D view is being drawn */
	int intro = (rb (598) & 0x20) != 0;	/* the intro and the start menu */
	int ok = !intro && view && clr3d && fs != 0x54 && fs != 0x60 && fs != 0x68 && y >= 0 && y < 168;
#ifdef WITH_GL
	if (ok && !classic_ui && hdui_owns_page ()) ok = 0;
#endif
	if (debug)
		fprintf (stderr, "controls: flight test x=%d y=%d state=%02x view42=%02x clr536=%02x "
			 "view706=%02x rate=%lx -> %d\n", x, y, fs, rb (42), rb (536), rb (706), rl (10428), ok);
	return ok;
}

void bindings_pointer_st (int *px, int *py)
{
	int x = view_w > 0 ? 320 * (input.abs_x - view_x) / view_w : 320 * input.abs_x / screen_w;
	int y = view_h > 0 ? 200 * (input.abs_y - view_y) / view_h : 200 * input.abs_y / screen_h;
	*px = x < 0 ? 0 : x > 319 ? 319 : x;
	*py = y < 0 ? 0 : y > 199 ? 199 : y;
}

/* ---------------------------------------------------------------- */
/* Sending */

void bindings_emit (int code, int down)
{
	if (code <= 0 || code >= 128) return;
	if (is_default) {
		/* exactly the old path */
		Input_PressSTKey (code, down);
		return;
	}
	if (down) {
		if (held[code]++ == 0) Input_PressSTKey (code, TRUE);
	} else if (held[code]) {
		if (--held[code] == 0) Input_PressSTKey (code, FALSE);
	}
}

/* The time controls: the rates the game sets for its console's time
 * buttons, slowest first (0 is stopped) */
static const unsigned long time_rates[] = { 0, 0x3e2, 0x26d6, 0x1845c, 0xf2b98, 0x97b3f0 };
#define N_RATES	6
static int time_saved_x, time_saved_y, time_pressing;

static void time_action (int faster, int down)
{
	if (down) {
		unsigned long r = rl (10428);
		int cur = -1, m, sx, sy;
		for (m = 0; m < N_RATES; m++)
			if (time_rates[m] == r) cur = m;
		if (cur < 0) return;		/* some other state (hyperspace): leave it */
		m = faster ? cur + 1 : cur - 1;
		if (m < 0 || m >= N_RATES || time_pressing) return;
		/* the console's time buttons are 10 ST pixels apart from x 5 at y 181 */
		sx = 5 + 10 * m;
		sy = 181;
		time_saved_x = input.abs_x;
		time_saved_y = input.abs_y;
		input.abs_x = view_w > 0 ? view_x + (sx * view_w + view_w / 2) / 320 : sx * screen_w / 320;
		input.abs_y = view_h > 0 ? view_y + (sy * view_h + view_h / 2) / 200 : sy * screen_h / 200;
		time_pressing = 1;
		Input_MousePress (SDL_BUTTON_LEFT);
		if (debug) fprintf (stderr, "controls: time %d -> %d\n", cur, m);
	} else if (time_pressing) {
		Input_MouseRelease (SDL_BUTTON_LEFT);
		input.abs_x = time_saved_x;
		input.abs_y = time_saved_y;
		time_pressing = 0;
	}
}

/* a key that types (digits, letters, punctuation, Backspace, Return,
 * Space): in the game menu (its file names) and out of a game it is
 * itself, so typing works whatever is bound to it */
static int types (int c)
{
	return (c >= 0x02 && c <= 0x0e) || (c >= 0x10 && c <= 0x1c) || (c >= 0x1e && c <= 0x29) ||
	       (c >= 0x2b && c <= 0x35) || c == ST_SPACE;
}

int bindings_key_code (int phys)
{
	if (phys < 0 || phys >= 128 || is_default) return phys;
	if (types (phys) && ((rb (598) & 0x20) || (rb (10469) && !rl (10428)))) return phys;
	return remap[phys];	/* -1 nothing; -2 - i action i, done by bindings_time_key */
}

/* run action i from a source going down or up */
static void act (int i, int down)
{
	const struct binding_action *a = &binding_actions[i];
	if (debug) fprintf (stderr, "controls: %s %s\n", a->name, down ? "down" : "up");
	if (a->kind == K_KEY) bindings_emit (a->code, down);
	else if (a->kind == K_TIME) time_action (a->code, down);
}

void bindings_time_key (int code, int down)
{
	if (code <= -2) act (-2 - code, down);
}

int bindings_mouse_event (int button, int down)
{
	int i;

	if (controls_page_open ()) return controls_page_mouse (button, down);
	/* the game menu's Controls button */
	if (controls_page_mouse (button, down)) return 1;
	if (button <= 0 || button >= MB_COUNT) return 0;
	if (is_default) return 0;	/* exactly the old path */

	if (button == MB_LEFT) {
		if (down) {
			int x, y;
			i = mouse_action[MB_LEFT];
			if (i < 0 || left_active >= 0) return 0;
			/* Shift + left always targets */
			if (SDL_GetModState () & KMOD_SHIFT) return 0;
			bindings_pointer_st (&x, &y);
			if (!bindings_in_flight_view (x, y)) return 0;
			left_active = i;
			act (i, 1);
			return 1;
		}
		if (left_active >= 0) {
			act (left_active, 0);
			left_active = -1;
			return 1;
		}
		return 0;
	}

	/* the wheel only taps */
	if (button == MB_WHEEL_UP || button == MB_WHEEL_DOWN) {
		i = mouse_action[button];
		if (i < 0 || !down) return 1;
		if (binding_actions[i].kind == K_KEY) {
			act (i, 1);
			act (i, 0);
		}
		return 1;
	}

	if (down) {
		i = mouse_action[button];
		if (i < 0) return 1;	/* unbound (the right button when steering moved): nothing */
		if (mouse_active[button] >= 0) return 1;
		mouse_active[button] = i;
		if (binding_actions[i].kind == K_STEER) {
			steer_down = 1;
			Input_MousePress (SDL_BUTTON_RIGHT);
		} else
			act (i, 1);
		return 1;
	}
	i = mouse_active[button];
	if (i < 0) return 1;
	mouse_active[button] = -1;
	if (binding_actions[i].kind == K_STEER) {
		steer_down = 0;
		Input_MouseRelease (SDL_BUTTON_RIGHT);
	} else
		act (i, 0);
	return 1;
}

void bindings_release_all (void)
{
	extern void Keymap_ReleaseAll (void);
	int b;

	Keymap_ReleaseAll ();
	if (left_active >= 0) act (left_active, 0);
	left_active = -1;
	for (b = 0; b < MB_COUNT; b++)
		if (mouse_active[b] >= 0) {
			if (binding_actions[mouse_active[b]].kind == K_STEER) Input_MouseRelease (SDL_BUTTON_RIGHT);
			else act (mouse_active[b], 0);
			mouse_active[b] = -1;
		}
	steer_down = 0;
	if (time_pressing) time_action (0, 0);
	if (input.cur_mousebut_state & 1) Input_MouseRelease (SDL_BUTTON_RIGHT);
	if (input.cur_mousebut_state & 2) Input_MouseRelease (SDL_BUTTON_LEFT);
	memset (held, 0, sizeof held);
}

/* mouse_active starts empty */
__attribute__((constructor)) static void bindings_boot (void)
{
	int b;
	for (b = 0; b < MB_COUNT; b++) mouse_active[b] = -1;
	for (b = 0; b < 128; b++) remap[b] = b;
}

#ifndef WITH_GL
/* No Controls page without the GL renderer; controls.ini still applies */
void controls_page_toggle (void) { }
int controls_page_open (void) { return 0; }
int controls_page_key (SDL_keysym *k, int down) { (void) k; (void) down; return 0; }
int controls_page_mouse (int button, int down) { (void) button; (void) down; return 0; }
void controls_page_draw (void) { }
void controls_page_forget (void) { }
#endif
