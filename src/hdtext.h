/*
 * HD text for the GL renderer.
 *
 * The game draws all its text with DrawStr into the 320x200 ST screen.
 * hdtext records each glyph as it is drawn, finds the ones that are still
 * on the screen when a frame is shown, takes their pixels out of the
 * picture uploaded to GL (never out of ST memory: the oracle hashes that)
 * and draws them again on top as smooth, full resolution type.  See
 * hdtext.c and docs/hd/ui.md.
 *
 * The glyphs found are joined into runs (a word or words drawn one after
 * another on one line in one colour), and each run is typeset again in
 * one of the design system's roles (title, section, label, body, value
 * ...) with the bundled fonts, at its place or where hdui moves it.
 * Under --classic-ui the text is drawn exactly as before the redesign.
 */
#ifndef _HDTEXT_H
#define _HDTEXT_H

/* --pixel-text: keep the original 8x8 bitmap font */
extern int pixel_text;

/* The galaxy map is up (not under --classic-ui): 1, its names are found
 * though the grid and other names cross them; 2 (the GL renderer), they
 * also get a halo and are kept apart (declutter). */
extern int hdtext_map;

/* gl.c is drawing its n-th 3D view name (from 1) into the screen: on the
 * map it is taken as drawn, for this frame only.  0: other text. */
extern int hdtext_queue;

/* the map's selected star, ST pixels (x < 0: none): its name keeps its place */
extern float hdtext_focus_x, hdtext_focus_y;

/* DrawStr drew glyph chr of font (the 10 byte a glyph bitmap font
 * DrawChar draws with) (character code - 0x20) with its top left
 * at ST pixel (x, y) in palette colour col, with a shadow at (x+1, y+1)
 * when shadowed. */
void hdtext_glyph (const unsigned char *font, int x, int y, int col, int chr,
		   int shadowed);

/* Once a frame: check the recorded glyphs against the 320x200 screen
 * raster and write to out a copy of it with the pixels of the ones still
 * on it replaced by the colour behind them; then join them into runs. */
void hdtext_frame (const unsigned char *font, const unsigned char *raster,
		   unsigned char *out);

/* Draw the runs hdtext_frame found, in the current viewport, with ST
 * coordinates (0..320, 0..200 from the bottom) as the modelview.  pal is
 * the RGBA palette of rows 0..167, ctrl_pal (16 entries) of 168..199. */
void hdtext_draw (const unsigned char *font, const unsigned int *pal,
		  const unsigned int *ctrl_pal);

/* The GL context is going away: drop the glyph textures. */
void hdtext_forget (void);

/* The design system's text roles */
enum {
	HDT_AUTO,	/* hdtext decides: the panel's, else a HUD label's */
	HDT_TITLE,	/* a page's title */
	HDT_SECTION,	/* a group's heading: small capitals, spaced */
	HDT_LABEL,	/* a control's or a row's name */
	HDT_BODY,	/* running text */
	HDT_DESC,	/* a description under a label: smaller, quieter */
	HDT_VALUE,	/* a number or a value: tabular figures */
	HDT_PANEL,	/* the control panel's text */
	HDT_MAP,	/* over a 3D view: a strong soft shadow */
	HDT_STRONG,	/* running text, bold */
	HDT_NROLES
};

enum { HDT_ALIGN_AUTO, HDT_LEFT, HDT_RIGHT, HDT_CENTRE };

/* what a run holds */
#define HDT_K_DIGIT	1	/* a digit */
#define HDT_K_NUMERIC	2	/* mostly digits and number punctuation */
#define HDT_K_CAPS	4	/* letters, all capitals */
#define HDT_K_COLON	8	/* ends in a colon */
#define HDT_K_ICON	16	/* one of the font's icons, alone */
#define HDT_K_LETTER	32	/* a letter */

struct hdtext_run {
	/* found: ST pixels */
	short x0, x1, y;	/* left of the first glyph, end of the last's advance, top */
	short ink0, ink1;	/* left of the first ink column, right of the last */
	short first, n;		/* glyphs, in the frame's own list */
	unsigned char col, shadow, kind, chr0;	/* colour, game's shadow, HDT_K_*, first glyph code */
	unsigned int hash;	/* FNV-1a 32 of its letters and digits, lower case */
	short letters;		/* letters and digits in it */

	/* what hdui says, reset every frame */
	unsigned char role;	/* HDT_* role */
	unsigned char align;	/* HDT_ALIGN_AUTO, HDT_LEFT ... */
	unsigned char hide;	/* not drawn (hdui draws something in its place) */
	unsigned char upper;	/* set in capitals */
	float dx, dy;		/* move, ST pixels */
	float ax;		/* anchored here (ST x) whatever the align; 0: by its align */
	float maxw;		/* the most width it may take, ST pixels; 0: to the next run */
	float scale;		/* of the role's size; 0: 1 */
	unsigned int rgb;	/* 0xff000000 | 0xBBGGRR: drawn in this colour; 0: the game's */
	float alpha;		/* 0: 1 */
	unsigned char money;	/* a sum: set as -$1,234.5 from the game's own figures */
};

/* After hdtext_frame: the runs it found, top to bottom and left to right
 * on each line, for hdui to style.  Valid until the next hdtext_frame. */
int hdtext_runs (struct hdtext_run **runs);

/* Run r's glyphs as ASCII ('?' for anything else), and whether a word
 * space comes before each; returns how many (at most max). */
int hdtext_chars (const struct hdtext_run *r, char *c, unsigned char *sp, int max);

/* Where the game drew run r's glyph k (ST x) */
int hdtext_glyph_x (const struct hdtext_run *r, int k);

/* Make run i two runs, before its glyph k: each keeps its place (left
 * aligned at its first ink) and the runs after it move up one.  Call
 * hdtext_runs again for the count.  Returns 0 when it cannot. */
int hdtext_split (int i, int k);

/* After hdtext_frame: a run of the renderer's own, s (ASCII; a space is
 * a word space) set as though the game had drawn it at ST (x, y) in
 * palette colour col, nothing in the picture taken out for it.  hdui
 * styles it like any other run.  Returns its index, or -1.  Valid until
 * the next hdtext_frame. */
int hdtext_add (const char *s, int x, int y, int col);

/* A run's width set in its role (and scale), ST pixels, at the size the
 * last frame was drawn (1600x1200 before the first) */
float hdtext_measure (const struct hdtext_run *r);

#endif /* _HDTEXT_H */
