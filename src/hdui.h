/*
 * The restyled 2D screens of the GL renderer: modern panels, rules and
 * controls drawn where the game's flat fills and markers are, every one
 * at the same place doing the same job.
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
#ifndef _HDUI_H
#define _HDUI_H

/* --classic-ui: keep the game's own flat screens */
extern int classic_ui;

/* Once a frame, after hdtext_frame: raster is the 320x200 screen being
 * shown, out the copy that will be uploaded to GL (the HD text's glyphs
 * already taken out).  Change out where this layer will draw instead
 * (255 is transparent).  pal is the RGBA palette of rows 0..167,
 * ctrl_pal (16 entries) that of rows 168..199. */
void hdui_frame (const unsigned char *raster, unsigned char *out,
		 const unsigned int *pal, const unsigned int *ctrl_pal);

/* Draw before the screen is: over the 3D view, under the screen's pixels,
 * so it shows where hdui_frame made them transparent (the GL renderer
 * only: the old one draws the screen opaque).  On return GL_TEXTURE_2D
 * is disabled and blending off. */
void hdui_draw_under (const unsigned int *pal, const unsigned int *ctrl_pal);

/* Draw last, over the screen, the HD stills and the HD text.  On return
 * GL_TEXTURE_2D is enabled, blending off, the texture env GL_REPLACE and
 * the colour white. */
void hdui_draw_over (const unsigned int *pal, const unsigned int *ctrl_pal);

/* Just after the frame's glClear (the page colour): the dark base and the
 * glass panels of the page the last hdui_frame recognised, so a photo
 * backdrop another layer draws over the base shows through the glass. */
void hdui_backdrop (const unsigned int *pal, int bgcol);

/* Nonzero when the next hdui_backdrop will draw a page's base and glass
 * (the page the last hdui_frame recognised).  hdback asks, just before it
 * draws its picture: when this layer owns the page, its wash and glass
 * dim the picture and hdback adds no scrim of its own. */
int hdui_owns_page (void);

/* Nonzero while the last hdui_frame's page is one the game waits on
 * without drawing frames (load and save: a name typed, a file chosen) */
int hdui_still_page (void);

/* The HD layer lays this frame's pages out (not --classic-ui, --old-renderer
 * or --pixel-text) */
int hdui_layer_on (void);

/* The game cleared the ST area (x1, y1)-(x2, y2) for a 3D viewer this
 * frame: the next hdui_frame leaves it alone and frames it. */
void hdui_clear_area (int x1, int y1, int x2, int y2);

/* The pointer, in ST pixels, as input.c is about to give it to the game,
 * and the buttons held (2: the left one).  While the left button is held
 * on a control this layer moved, *ax and *ay are changed to a point inside
 * that control's original click region; otherwise they are left alone.
 * Never called by an oracle replay (it writes the tape's values itself). */
void hdui_mouse (short *ax, short *ay, int buttons);

/* The GL context is going away: drop this layer's textures. */
void hdui_forget (void);

/* For hdtip: the restyled screens' glass panel at framebuffer pixel
 * (x, y) from the picture's top left, w x h pixels, drawn in the caller's
 * 2D set-up (y down, premultiplied blending). */
void hdui_glass_px (float x, float y, int w, int h, float a);

/* From gl.c: the game's 4:3 picture in the framebuffer, in pixels from
 * the bottom left; the whole window unless it is letterboxed. */
void gl_picture_rect (int *x, int *y, int *w, int *h);

#endif /* _HDUI_H */
