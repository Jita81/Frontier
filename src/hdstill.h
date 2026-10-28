/*
 * HD stills for the GL renderer: new art drawn in place of the game's
 * bitmaps (the title logo, the control panel and its icons, ...).
 *
 * Same contract as hdtext: the 68k code draws into the 320x200 ST screen
 * and the renderer only ever sees the finished picture, so nothing
 * recorded is trusted.  Something is redrawn only while it is still on
 * the screen being shown, and nothing here writes ST memory (the oracle
 * hashes that).
 *
 * Coordinates: ST pixel (x, y), y down from the top, is at (x, 200 - y)
 * in the ortho view the draw calls get (0..320 x 0..200, y up).
 */
#ifndef _HDSTILL_H
#define _HDSTILL_H

/* --pixel-art: keep the game's own bitmaps */
extern int pixel_art;

/* Call_BlitBmp is drawing a bitmap: planes is its pixel data in ST memory
 * (4 bit planes one after another, each height rows of width_words big
 * endian 16 bit words), bmp its ST address, (x, y) its top left on the
 * screen at ST address scr. */
void hdstill_blit (const unsigned char *planes, int width_words, int height,
		   int x, int y, unsigned int bmp, unsigned int scr);

/* Once a frame, after hdtext_frame: raster is the 320x200 screen being
 * shown, out the copy that will be uploaded to GL (the HD text's glyphs
 * already taken out).  Change out where this layer will draw instead
 * (255 is transparent).  pal is the RGBA palette of rows 0..167,
 * ctrl_pal (16 entries) that of rows 168..199. */
void hdstill_frame (const unsigned char *raster, unsigned char *out,
		    const unsigned int *pal, const unsigned int *ctrl_pal);

/* Draw over the uploaded screen and under the HD text, in the ortho view.
 * On return GL_TEXTURE_2D is enabled, blending off, the texture env
 * GL_REPLACE and the colour white, as hdtext_draw leaves them. */
void hdstill_draw (const unsigned int *pal, const unsigned int *ctrl_pal);

/* The HD gauges' red thresholds, for hover text: the laser will not fire
 * above LASER_CUT, the hull nears damage at HEAT_WARN.  The SPEED/HEAT
 * plate covers ST x 212-237, y 169.4-172.6 and, right of the missile,
 * x 221-243, y 172.6-177. */
#define HDSTILL_LASER_CUT	0xf000
#define HDSTILL_HEAT_WARN	0xc000
/* The icons in the function keys, as this layer names them */
enum {
	G_ABSENT, G_EMPTY, G_FRONT, G_REAR, G_EXT, G_GALMAP, G_SYSMAP, G_SHIP,
	G_CARGO, G_CMDR, G_PHONE, G_TARGET, G_CROSS, G_WRENCH, G_HYPER,
	G_PERSON, G_LAUNCH, G_LIST, G_ARM, G_BEAM, G_BURST, G_ORRERY,
	G_LOAD, G_SAVE, G_DELETE, G_EXIT, G_SECTOR, G_ROUTE, G_TAKEOFF,
	G_DOC, G_ALPHA
};
#define G_OFF		0x80	/* with an icon: drawn crossed out */

#define F_MAPKEYS	1	/* the map's time keys where the second display is */
#define F_SCANNER	2	/* the scanner in the bay */
#define F_BUTTONS	4	/* the row of keys along the bay's floor */

/* The control panel as the last hdstill_frame found it on the screen:
 * the icon in each function key (slot[1..10], G_ABSENT where none was
 * found), F_ flags of the bay and the second display, and how many
 * missiles are on the pylons.  Read only, for hdtip. */
struct hdstill_panel {
	int valid, flags, seen;
	unsigned char slot[11];
	int n_missiles;
};
/* Fills *p; returns p->valid (0 with --pixel-art or no panel seen) */
int hdstill_panel (struct hdstill_panel *p);

/* The GL context is going away: drop this layer's textures. */
void hdstill_forget (void);

#endif /* _HDSTILL_H */
