/*
 * Rebindable controls: a host-side layer between SDL's key and mouse
 * events and the ST scan codes and mouse buttons the game reads.
 *
 * With no controls.ini every key and button does exactly what it did
 * before this layer existed: keys go through Keymap_RemapKeyToSTScanCode
 * unchanged and the mouse through Input_MousePress/Release.  The file is
 * never read when an oracle flag is given, and an oracle replay drops
 * live input anyway, so nothing here can reach a recorded trace.  Nothing
 * here writes ST memory; the game state it looks at is only read.
 */
#ifndef BINDINGS_H
#define BINDINGS_H

#include <SDL.h>

/* the mouse buttons a binding can name (SDL 1.2 numbering) */
enum {
	MB_NONE = 0, MB_LEFT = 1, MB_MIDDLE = 2, MB_RIGHT = 3,
	MB_WHEEL_UP = 4, MB_WHEEL_DOWN = 5, MB_X1 = 6, MB_X2 = 7, MB_COUNT
};

/* the groups of the Controls page */
enum { G_FLIGHT, G_WEAPONS, G_VIEWS, G_TIME, G_MAPS, G_OTHER, G_COUNT };

/* what an action does when it fires */
enum {
	K_KEY,		/* holds an ST key down */
	K_STEER,	/* the game's right button: hold and move to steer */
	K_CLICK,	/* the game's left button: fixed, shown for reference */
	K_TIME		/* clicks a time control on the console */
};

struct binding_action {
	const char *name;	/* in controls.ini (joystick.ini's names where it has one) */
	const char *label;	/* on the Controls page */
	const char *help;	/* hover help */
	unsigned char group, kind;
	unsigned char code;	/* K_KEY: the ST scan code the game reads (and the default key);
				 * K_TIME: +1 faster, 0 slower */
	unsigned char def_mouse;
};

extern const struct binding_action binding_actions[];
extern const int n_binding_actions;

/* the current bindings: per action, a key (the physical key's own ST scan
 * code, 0 none) and a mouse button (MB_*) */
extern unsigned char binding_key[];
extern unsigned char binding_mouse[];

const char *binding_key_name (int st_code);	/* "M", "Space", "F1"; NULL if unnamed */
const char *binding_mouse_name (int mb);
const char *binding_group_name (int g);
int binding_action_index (const char *name);

/* load controls.ini (main, once, after the oracle flags are parsed) */
void bindings_init (void);
/* write controls.ini now (the page calls it after every change) */
int bindings_save (void);
/* back to the built-in defaults (not saved) */
void bindings_reset (void);
/* rebuild the tables after binding_key/binding_mouse changed */
void bindings_changed (void);
/* nonzero when every binding is the default and the layer passes through */
int bindings_are_default (void);
/* the action holding key k (an ST code) or mouse button mb, other than
 * skip, or -1 */
int bindings_key_owner (int k, int skip);
int bindings_mouse_owner (int mb, int skip);
/* two actions sharing a key or button (a hand-edited controls.ini; the
 * later one has it): 1 and their indices, or 0 */
int bindings_clash (int *a, int *b);

/* "Left click fires laser" */
int bindings_left_fires_laser (void);
void bindings_set_left_fires_laser (int on);

/* From keymap.c: a key that would go to the game as ST code phys (or -1).
 * Returns the code to send instead, -1 to send nothing, or -2 or less
 * for an action that is not a key (bindings_time_key).  A typing key is
 * not moved in the game menu or out of a game, where it types. */
int bindings_key_code (int phys);
/* From keymap.c: a code of -2 or less from bindings_key_code is a time
 * action; this runs it */
void bindings_time_key (int code, int down);
/* From keymap.c: send an ST key down or up (counted, so two sources of
 * one code combine) */
void bindings_emit (int code, int down);

/* From main.c: an SDL mouse button event.  Returns nonzero when this
 * layer took it; otherwise the caller does what it always did. */
int bindings_mouse_event (int button, int down);

/* Let go of everything this layer or the player holds (keys and mouse
 * buttons), as if every one were released now. */
void bindings_release_all (void);

/* The flight test for the left button: flying (not docked or landed),
 * the cockpit 3D view on the screen, and ST pixel (x, y) over that view
 * rather than the console. */
int bindings_in_flight_view (int x, int y);
/* the pointer in ST pixels, as Call_GetMouseInput works it out */
void bindings_pointer_st (int *x, int *y);

/* The Controls page (hdcontrols.c in the GL build; a stub otherwise) */
void controls_page_toggle (void);
int controls_page_open (void);
/* a key while the page is open; nonzero when taken */
int controls_page_key (SDL_keysym *k, int down);
int controls_page_mouse (int button, int down);
/* draw it, last thing in the frame, over the whole picture */
void controls_page_draw (void);
/* the GL context is going away: drop the page's textures */
void controls_page_forget (void);

#endif /* BINDINGS_H */
