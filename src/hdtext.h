/*
 * HD text for the GL renderer.
 *
 * The game draws all its text with DrawStr into the 320x200 ST screen.
 * hdtext records each glyph as it is drawn, finds the ones that are still
 * on the screen when a frame is shown, takes their pixels out of the
 * picture uploaded to GL (never out of ST memory: the oracle hashes that)
 * and draws them again on top as smooth, full resolution type.  See
 * hdtext.c.
 */
#ifndef _HDTEXT_H
#define _HDTEXT_H

/* --pixel-text: keep the original 8x8 bitmap font */
extern int pixel_text;

/* DrawStr drew glyph chr of font (the 10 byte a glyph bitmap font
 * DrawChar draws with) (character code - 0x20) with its top left
 * at ST pixel (x, y) in palette colour col, with a shadow at (x+1, y+1)
 * when shadowed. */
void hdtext_glyph (const unsigned char *font, int x, int y, int col, int chr,
		   int shadowed);

/* Once a frame: check the recorded glyphs against the 320x200 screen
 * raster and write to out a copy of it with the pixels of the ones still
 * on it replaced by the colour behind them. */
void hdtext_frame (const unsigned char *font, const unsigned char *raster,
		   unsigned char *out);

/* Draw the glyphs hdtext_frame found, in the current viewport, with ST
 * coordinates (0..320, 0..200 from the bottom) as the modelview.  pal is
 * the RGBA palette of rows 0..167, ctrl_pal (16 entries) of 168..199. */
void hdtext_draw (const unsigned char *font, const unsigned int *pal,
		  const unsigned int *ctrl_pal);

/* The GL context is going away: drop the glyph texture. */
void hdtext_forget (void);

#endif /* _HDTEXT_H */
