/*
 * HD instruments for the GL renderer: the scanner's contacts, the gun
 * sight and the target's lock boxes drawn again as smooth vector shapes.
 *
 * Same contract as hdtext and hdstill: the 68k code draws into the
 * 320x200 ST screen, this layer only notes what was drawn (and where),
 * checks it against the picture being shown, takes those pixels out of
 * the copy uploaded to GL and draws its own on top.  Nothing here writes
 * ST memory (the oracle hashes that).  Off under --classic-ui, and under
 * --old-renderer; the contacts also under --pixel-art (the game's own
 * scanner bitmap is shown then).
 *
 * Coordinates: ST pixel (x, y), y down from the top, is at (x, 200 - y)
 * in the ortho view the draw call gets (0..320 x 0..200, y up).
 */
#ifndef _HDINST_H
#define _HDINST_H

/* Call_BlitBmp: a bitmap w x h pixels at (x, y) on the screen at ST
 * address scr.  The scanner's starts noting its contacts; any other ends
 * that. */
void hdinst_blit (int x, int y, int w, int h, unsigned long scr);

/* Call_OldHLine (mask 0: all n pixels) and Call_BackHLine (16 pixels,
 * bit 15 the first: the ones the game means to set): a line of the
 * scanner's contacts at ST address addr (the 320 byte a row screen). */
void hdinst_run (unsigned long addr, int n, unsigned mask, int col);

/* Once a frame, before hdtext_frame: find the gun sight on the screen
 * being shown (so hdtext still knows the glyphs it is drawn over). */
void hdinst_mask (const unsigned char *raster);

/* After the other layers' _frame: take what this layer draws out of out
 * (255 is transparent), and move the target's label clear of the sight. */
void hdinst_frame (const unsigned char *raster, unsigned char *out);

/* Draw it, in the ortho view, before the HD text.  pal is the RGBA
 * palette of rows 0..167, ctrl_pal of 168..199. */
void hdinst_draw (const unsigned int *pal, const unsigned int *ctrl_pal);

#endif /* _HDINST_H */
