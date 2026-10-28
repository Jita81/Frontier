/*
  Hatari - screen.c

  This file is distributed under the GNU Public License, version 2 or at your
  option any later version. Read the file gpl.txt for details.
*/

/*
 * Provenance (docs/05 section 4.3). This stream was assigned picks #7, #8,
 * #10, #11 and #12, but B's 5e5c602 (#8) is written against B's own gl.c
 * and does not apply to the base's: git apply --check of its gl.c diff
 * fails (at the hunk for line 1535). So this file is B's gl.c at the tip,
 * which also carries the gl.c parts of #1-#3 (981d304, b53521f, 03a5942)
 * and #9 (756647f), with these differences:
 *  - not taken: #6's SDL_RESIZABLE window (84b737b) and #1's in_atmosphere;
 *  - the projection is the base's 1.9 aspect, not #1's window aspect;
 *  - the S-2 S-6 S-7 S-9 S-11 S-23 fixes and V-1 of this stream.
 * The hostcall.c, screen.h and main.c parts of #1-#6 and #9 were not
 * taken.
 */

#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
/* macOS's glu.h doesn't provide the "_GLUfuncptr" callback typedef that
 * Linux/Mesa's GL/glu.h does; declare a compatible one here. */
typedef void (*_GLUfuncptr)();
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glu.h>
#endif

/* the shader tiers of --hull-style need GL 2.0 */
#if defined (__APPLE__) || defined (GL_VERSION_2_0)
#define HULL_SHADERS 1
#endif

#include "main.h"
#include "../m68000.h"
#include "screen.h"
#include "planet.h"
#include "hdtext.h"
#include "hdstill.h"
#include "hdface.h"
#include "hdui.h"
#include "hull.h"
#include "art.h"
#include "hdback.h"
#include "procfx.h"
#include "bindings.h"
#include "hdtip.h"
/* MEM_SIZE, and the engine's variables by label address (__D<label>) */
#include "../host.h"
#include "../fixups.h"

unsigned long VideoBase;                        /* Base address in ST Ram for screen(read on each VBL) */
unsigned char *VideoRaster;                      /* Pointer to Video raster, after VideoBase in PC address space. Use to copy data on HBL */

int len_main_palette;
unsigned short MainPalette[256];
unsigned short CtrlPalette[16];
int fe2_bgcol;

unsigned int MainRGBPalette[256];
unsigned int CtrlRGBPalette[16];

unsigned long logscreen, logscreen2, physcreen, physcreen2;


static SDL_Surface *sdlscrn;                             /* The SDL screen surface */
BOOL bGrabMouse = FALSE;                          /* Grab the mouse cursor in the window */
BOOL bInFullScreen = FALSE;

/* new stuff */
enum RENDERERS use_renderer = R_GL;
/* --flat-hulls: no hull texture on ship faces (see hull_begin) */
int flat_hulls = 0;
/* --hull-style N, how a ship face is painted (every style keeps the face's
 * own colour and brightness; the textures are made at run time by hull.c):
 *	0 the fixed-function plating below
 *	1 a 1024 plating texture, two scales blended, no visible tiling,
 *	  anisotropic filtering
 *	2 + irregular recessed panels, hatches, vents, hazard stripes,
 *	  greebles and edge occlusion taken from the face's own edges
 *	3 + bump map, specular and weathering (grime, chips), lit from the
 *	  game's own light direction
 *	4 + a material profile per model (hull.c, hull_models)
 *	5 a material per class of ship (the default): each class's own grey
 *	  tile (art/hull/), laid on the model's own frame so panels keep their
 *	  size and run on across faces, with its own bump, sheen, grime and
 *	  wear (hull.c, hull_classes); docs/hd/hulls.md
 * Styles 1 to 4 need GLSL 1.20; without it they fall back to 0. Style 5
 * without it (or with FE2_HULL_FIXED set) draws its class tiles in fixed
 * function. A face that hosts decals (a sign, a registration) keeps the
 * quiet plating, or is flat for the station's own signs. Ctrl+Shift+H
 * steps through the styles. */
int hull_style = 5;
/* mouse shown this frame? */
int mouse_shown = 0;
/* --pixel-text: no HD text, see hdtext.c */
int pixel_text = 0;
/* --pixel-art, --pixel-faces, --classic-ui: no HD stills, faces or
 * restyled screens, see hdstill.c, hdface.c, hdui.c */
int pixel_art = 0;
int pixel_faces = 0;
int classic_ui = 0;
/* fe2 UI blits are done to old screen memory and copied to this texture. */
static unsigned int screen_tex;
static GLuint galaxy_tex;	/* the galaxy picture, smoothed (see galaxy_draw) */

static GLUquadricObj *qobj;
static GLUtesselator *tobj;

float hack;

#define SCR_TEX_W	512
#define SCR_TEX_H	256

#define RAD_2_DEG	57.295779513082323f

/*-----------------------------------------------------------------------*/
/*
  Set window size
*/
/* The window's size in SDL coordinates, which mouse input comes in.
 * --size sets it; 0 picks one that fits the desktop, see pick_window_size. */
int screen_w = 0;
int screen_h = 0;
/* The window's framebuffer in pixels, which every glViewport is in: the
 * same as screen_w x screen_h unless the window has a higher pixel density
 * (sdl12-compat with SDL12COMPAT_HIGHDPI=1 and SDL12COMPAT_OPENGL_SCALING=0
 * on a Retina Mac). */
static int fb_w = 640, fb_h = 480;
/* The windowed size (from --size, or picked) and the desktop's, taken
 * before the first mode set: fullscreen uses the whole desktop. */
static int win_w, win_h, desk_w, desk_h;
/* The picture: the largest 4:3 rectangle that fits the framebuffer,
 * centred, in framebuffer pixels from the bottom left (pic_*) and in SDL
 * coordinates from the top left (view_*, for the mouse). On a wider
 * screen the sides are black bars. */
static int pic_x, pic_y, pic_w = 640, pic_h = 480;
int view_x = 0, view_y = 0, view_w = 0, view_h = 0;
#define GLERR { printf ("GL: %s\n", gluErrorString (glGetError ()));}

#ifndef CALLBACK
# ifdef WIN32
#  define CALLBACK __attribute__ ((__stdcall__))
# else
#  define CALLBACK
# endif
#endif /* CALLBACK */

void CALLBACK beginCallback(GLenum which);
void CALLBACK errorCallback(GLenum errorCode);
void CALLBACK endCallback(void);
void CALLBACK vertexCallback(GLvoid *vertex, GLvoid *poly_data);
void CALLBACK combineCallback(GLdouble coords[3], 
                     GLdouble *vertex_data[4],
                     GLfloat weight[4], GLdouble **dataOut );

/* The 3D view, framebuffer pixels: the picture's top 168 rows, or wider
 * (wide_3d) while draw_wide_flight draws a flight view past the picture. */
static int wide_3d, v3d_x, v3d_y, v3d_w, v3d_h;

static void set_main_viewport ()
{
	int ctrl_h = 32*pic_h/200;
	if (wide_3d) glViewport (v3d_x, v3d_y, v3d_w, v3d_h);
	else glViewport (pic_x, pic_y + ctrl_h, pic_w, pic_h - ctrl_h);
}

static void set_ctrl_viewport ()
{
	glViewport (pic_x, pic_y, pic_w, pic_h);
}

static void set_picture_rect ()
{
	if (fb_w * 3 > fb_h * 4) {
		pic_h = fb_h;
		pic_w = fb_h * 4 / 3;
	} else {
		pic_w = fb_w;
		pic_h = fb_w * 3 / 4;
	}
	pic_x = (fb_w - pic_w) / 2;
	pic_y = (fb_h - pic_h) / 2;
	view_x = pic_x * screen_w / fb_w;
	view_y = pic_y * screen_h / fb_h;
	view_w = pic_w * screen_w / fb_w;
	view_h = pic_h * screen_h / fb_h;
}

void gl_picture_rect (int *x, int *y, int *w, int *h)
{
	*x = pic_x;
	*y = pic_y;
	*w = pic_w;
	*h = pic_h;
}

/* Point sizes were chosen for a 640x480 window: keep them the same share
 * of the view at any picture size. */
static float px_scale (void)
{
	return pic_h > 480 ? pic_h / 480.0f : 1.0f;
}

/* The original's 4:3 shape (640x480) as large as fits on the desktop
 * below the menu bar, title bar and dock (128 pixels kept free), in whole
 * steps of 64x48 and never smaller than 640x480. */
static void pick_window_size ()
{
	int k = 10;

	if (desk_w > 0 && desk_h > 0) {
		int kw = desk_w / 64;
		int kh = (desk_h - 128) / 48;
		k = kw < kh ? kw : kh;
		if (k < 10) k = 10;
	}
	win_w = 64 * k;
	win_h = 48 * k;
}

/*
 * The near plane is what actually does the near clipping in the GL path,
 * because the engine skips its own: in L3aef8_ProjectTriangle and its
 * siblings in fe2.s, a set gl_renderer_on jumps over the whole
 * clip/reject stage and hands us the primitive raw. Their near test does
 * not clip anything either: l398c8 only stamps a sentinel over the *2D*
 * coords at (a0), while the *3D* viewing coords at 4(a0) - exactly what
 * znode_wrvertex reads - were already stored and stay valid.
 *
 * So primitives with z < 64, and even z < 0 (behind the camera), do reach
 * us. Keep the near plane small or near geometry (station docking bay
 * walls, a ship right alongside) gets sliced off. There is no depth
 * buffer to trade precision against, so there is nothing to gain by
 * pushing it out.
 */
#define GL_NEAR_PLANE	1.0f
#define GL_FAR_PLANE	10000000000.0f

static void planet_forget_textures (void);
static void hull_forget_texture (void);

static void change_vidmode ()
{
	const SDL_VideoInfo *info = NULL;
	int modes;

	/* On some platforms (Windows, macOS) SDL 1.2 creates a new GL context
	 * on every SDL_SetVideoMode - when the window goes fullscreen or
	 * back - and every texture of the old one is gone. Release ours
	 * while that context is still current, they get made again when
	 * needed. */
	planet_forget_textures ();
	hull_forget_texture ();
	hdtext_forget ();
	hdstill_forget ();
	hdface_forget ();
	hdui_forget ();
	controls_page_forget ();
	hdback_forget ();
	if (galaxy_tex) {
		glDeleteTextures (1, &galaxy_tex);
		galaxy_tex = 0;
	}
	procfx_forget ();
	hdtip_forget ();
	if (screen_tex) {
		glDeleteTextures (1, &screen_tex);
		screen_tex = 0;
	}

	info = SDL_GetVideoInfo ();
	if (info == NULL) {
		fprintf (stderr, "SDL_GetVideoInfo failed: %s\n", SDL_GetError ());
		SDL_Quit ();
		exit (-1);
	}

	if (desk_w <= 0) {
		desk_w = info->current_w;
		desk_h = info->current_h;
	}
	if (win_w <= 0) {
		if (screen_w > 0 && screen_h > 0) {
			win_w = screen_w;
			win_h = screen_h;
		} else {
			pick_window_size ();
		}
	}
	/* Fullscreen at the desktop's own size: the picture is drawn 1:1,
	 * not scaled up from a smaller mode. */
	if (bInFullScreen && desk_w > 0 && desk_h > 0) {
		screen_w = desk_w;
		screen_h = desk_h;
	} else {
		screen_w = win_w;
		screen_h = win_h;
	}

	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	/* No depth buffer is requested: the 3D view is drawn with a pure
	 * painter's algorithm, exactly like the software renderer. See
	 * draw_3dview. */
	/* 4x multisampling smooths polygon and line edges. It only changes
	 * how the host draws, so no oracle stream sees it. */
	SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, 1);
	SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 4);

	modes = SDL_OPENGL | SDL_ANYFORMAT | (bInFullScreen ? SDL_FULLSCREEN : 0);

	sdlscrn = SDL_SetVideoMode (screen_w, screen_h, info->vfmt->BitsPerPixel, modes);
	if (sdlscrn == 0) {
		/* no multisampled format: carry on without it */
		SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 0);
		sdlscrn = SDL_SetVideoMode (screen_w, screen_h, info->vfmt->BitsPerPixel, modes);
	}
	if (sdlscrn == 0) {
		fprintf (stderr, "Video mode set failed: %s\n", SDL_GetError ());
		SDL_Quit ();
		exit (-1);
	}

	/* A new context's viewport is its whole framebuffer, in pixels: that
	 * is how big the window really is. */
	{
		GLint vp[4] = { 0, 0, 0, 0 };
		glGetIntegerv (GL_VIEWPORT, vp);
		fb_w = vp[2] > 0 ? vp[2] : screen_w;
		fb_h = vp[3] > 0 ? vp[3] : screen_h;
	}
	set_picture_rect ();
	if (getenv ("FE2_GL_INFO")) {
		GLint samples = 0;
		glGetIntegerv (GL_SAMPLES, &samples);
		fprintf (stderr, "gl: %s %dx%d, framebuffer %dx%d, picture %dx%d at %d,%d, %d samples\n",
			 bInFullScreen ? "fullscreen" : "window", screen_w, screen_h,
			 fb_w, fb_h, pic_w, pic_h, pic_x, pic_y, (int) samples);
	}
	glEnable (GL_MULTISAMPLE);

	glDisable (GL_CULL_FACE);
	glShadeModel (GL_FLAT);
	glDisable (GL_DEPTH_TEST);
	glClearColor (0, 0, 0, 0);

	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	/* Aspect ratio of frontier's 3d view is 320/168 = 1.90, in ST
	 * pixels, whatever the window's shape: the software renderer
	 * stretches its 320x200 frame over the whole window, so the GL view
	 * has to stretch the same way or the two renderers frame different
	 * parts of the sky. With the window's own aspect instead (screen_w /
	 * (screen_h - ctrl_h), taken over with B's 981d304), a 640x480 window
	 * sees about 32 degrees either side in the software renderer but only
	 * 28 in GL, so a planet near the edge - Lave from Lave Station - is on
	 * the screen in one and off it in the other. This is the base's
	 * projection again. */
	gluPerspective (36.5f, 1.9f, GL_NEAR_PLANE, GL_FAR_PLANE);

	glEnable (GL_TEXTURE_2D);
	glGenTextures (1, &screen_tex);
	glBindTexture (GL_TEXTURE_2D, screen_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, SCR_TEX_W, SCR_TEX_H, 0, GL_RGBA, GL_INT, 0);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable (GL_TEXTURE_2D);
	
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	/* GL_DEPTH_TEST stays off for good: the 3D view is a painter's
	 * algorithm, see draw_3dview. */
}

void Screen_Init(void)
{
	change_vidmode ();

	/* called again on a video mode change: the GLU objects do not depend
	 * on the GL context, keep them */
	if (qobj) return;

	qobj = gluNewQuadric ();

	tobj = gluNewTess ();
		
	gluTessCallback(tobj, GLU_TESS_VERTEX_DATA, (_GLUfuncptr) vertexCallback);
	gluTessCallback(tobj, GLU_TESS_BEGIN, (_GLUfuncptr) beginCallback);
	gluTessCallback(tobj, GLU_TESS_END, (_GLUfuncptr) endCallback);
	gluTessCallback(tobj, GLU_TESS_ERROR, (_GLUfuncptr) errorCallback);
	gluTessCallback(tobj, GLU_TESS_COMBINE, (_GLUfuncptr) combineCallback);
	
	/* Configure some SDL stuff: */
	SDL_WM_SetCaption(PROG_NAME, "Frontier");
	SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
	SDL_ShowCursor(SDL_ENABLE);
}

void Screen_UnInit(void)
{
}

void Screen_ToggleFullScreen ()
{
	bInFullScreen = !bInFullScreen;
	change_vidmode ();
	//SDL_WM_ToggleFullScreen (sdlscrn);
}

static const unsigned char font_bmp[] = {
	0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x2,0x80,0x80,0x80,0x80,0x80,0x0,
	0x80,0x0,0x0,0x2,0xa0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x4,0x0,0x50,
	0xf8,0x50,0x50,0xf8,0x50,0x0,0x0,0x6,0x20,0xf0,0xa0,0xa0,0xa0,0xa0,0xf0,0x20,
	0x0,0x5,0x0,0xc8,0xd8,0x30,0x60,0xd8,0x98,0x0,0x0,0x6,0xa0,0x0,0xe0,0xa0,
	0xa0,0xa0,0xe0,0x0,0x0,0x4,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x2,
	0xc0,0x80,0x80,0x80,0x80,0x80,0x80,0xc0,0x0,0x3,0xc0,0x40,0x40,0x40,0x40,0x40,
	0x40,0xc0,0x0,0x3,0x0,0x0,0x20,0xf8,0x50,0xf8,0x20,0x0,0x0,0x6,0x0,0x0,
	0x40,0xe0,0x40,0x0,0x0,0x0,0x0,0x4,0x0,0x0,0x0,0x0,0x0,0x0,0x80,0x80,
	0x0,0x2,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x3,0x0,0x0,0x0,0x0,
	0x0,0x0,0x80,0x0,0x0,0x2,0x0,0x8,0x18,0x30,0x60,0xc0,0x80,0x0,0x0,0x6,
	0xe0,0xa0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x40,0xc0,0x40,0x40,0x40,0x40,
	0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0xe0,0x80,0x80,0xe0,0x0,0x0,0x4,0xe0,0x20,
	0x20,0xe0,0x20,0x20,0xe0,0x0,0x0,0x4,0x80,0x80,0xa0,0xa0,0xe0,0x20,0x20,0x0,
	0x0,0x4,0xe0,0x80,0x80,0xe0,0x20,0x20,0xe0,0x0,0x0,0x4,0xe0,0x80,0x80,0xe0,
	0xa0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0x20,0x20,0x20,0x20,0x0,0x0,0x4,
	0xe0,0xa0,0xa0,0xe0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0xa0,0xa0,0xe0,0x20,0x20,
	0xe0,0x0,0x0,0x4,0x0,0x0,0x0,0x80,0x0,0x80,0x0,0x0,0x0,0x2,0x0,0x0,
	0x0,0x80,0x0,0x0,0x80,0x80,0x0,0x2,0xe0,0x0,0xe0,0xa0,0xa0,0xa0,0xa0,0x0,
	0x0,0x4,0x0,0x0,0xe0,0x0,0xe0,0x0,0x0,0x0,0x0,0x4,0xc0,0x0,0xe0,0xa0,
	0xe0,0x80,0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0xe0,0x80,0x0,0x80,0x0,0x0,0x4,
	0xfe,0x82,0xba,0xa2,0xba,0x82,0xfe,0x0,0x0,0x8,0xf0,0x90,0x90,0x90,0xf0,0x90,
	0x90,0x0,0x0,0x5,0xf0,0x90,0x90,0xf8,0x88,0x88,0xf8,0x0,0x0,0x6,0xe0,0x80,
	0x80,0x80,0x80,0x80,0xe0,0x0,0x0,0x4,0xf8,0x48,0x48,0x48,0x48,0x48,0xf8,0x0,
	0x0,0x6,0xf0,0x80,0x80,0xe0,0x80,0x80,0xf0,0x0,0x0,0x5,0xf0,0x80,0x80,0xe0,
	0x80,0x80,0x80,0x0,0x0,0x4,0xf0,0x80,0x80,0x80,0xb0,0x90,0xf0,0x0,0x0,0x5,
	0x90,0x90,0x90,0xf0,0x90,0x90,0x90,0x0,0x0,0x5,0xe0,0x40,0x40,0x40,0x40,0x40,
	0xe0,0x0,0x0,0x4,0xf0,0x20,0x20,0x20,0x20,0x20,0xe0,0x0,0x0,0x4,0x90,0xb0,
	0xe0,0xc0,0xe0,0xb0,0x90,0x0,0x0,0x5,0x80,0x80,0x80,0x80,0x80,0x80,0xe0,0x0,
	0x0,0x4,0x88,0xd8,0xf8,0xa8,0x88,0x88,0x88,0x0,0x0,0x6,0x90,0xd0,0xf0,0xb0,
	0x90,0x90,0x90,0x0,0x0,0x5,0xf0,0x90,0x90,0x90,0x90,0x90,0xf0,0x0,0x0,0x5,
	0xf0,0x90,0x90,0xf0,0x80,0x80,0x80,0x0,0x0,0x5,0xf0,0x90,0x90,0x90,0x90,0xb0,
	0xf0,0x18,0x0,0x5,0xf0,0x90,0x90,0xf0,0xe0,0xb0,0x90,0x0,0x0,0x5,0xf0,0x80,
	0x80,0xf0,0x10,0x10,0xf0,0x0,0x0,0x5,0xe0,0x40,0x40,0x40,0x40,0x40,0x40,0x0,
	0x0,0x3,0x90,0x90,0x90,0x90,0x90,0x90,0xf0,0x0,0x0,0x5,0x90,0x90,0x90,0xb0,
	0xe0,0xc0,0x80,0x0,0x0,0x5,0x88,0x88,0x88,0xa8,0xf8,0xd8,0x88,0x0,0x0,0x6,
	0x88,0xd8,0x70,0x20,0x70,0xd8,0x88,0x0,0x0,0x6,0x90,0x90,0x90,0xf0,0x20,0x20,
	0x20,0x0,0x0,0x5,0xf0,0x10,0x30,0x60,0xc0,0x80,0xf0,0x0,0x0,0x5,0xa0,0x0,
	0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x80,0xc0,0x60,0x30,0x18,0x8,0x0,
	0x0,0x6,0xe0,0xa0,0xa0,0xe0,0xa0,0xa0,0xe0,0x80,0x80,0x4,0xe0,0xa0,0xe0,0x0,
	0x0,0x0,0x0,0x0,0x0,0x4,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xf8,0x0,0x6,
	0xa0,0x0,0xe0,0x20,0xe0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,0xe0,0x20,0xe0,0xa0,
	0xe0,0x0,0x0,0x4,0x80,0x80,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,
	0xc0,0x80,0x80,0x80,0xc0,0x0,0x0,0x3,0x20,0x20,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,
	0x0,0x4,0x0,0x0,0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xc0,0x80,0x80,0xc0,
	0x80,0x80,0x80,0x0,0x0,0x3,0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x20,0xe0,0x4,
	0x80,0x80,0xe0,0xa0,0xa0,0xa0,0xa0,0x0,0x0,0x4,0x80,0x0,0x80,0x80,0x80,0x80,
	0x80,0x0,0x0,0x2,0x40,0x0,0x40,0x40,0x40,0x40,0x40,0xc0,0x0,0x3,0x80,0x80,
	0xb0,0xe0,0xe0,0xb0,0x90,0x0,0x0,0x5,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x0,
	0x0,0x2,0x0,0x0,0xf8,0xa8,0xa8,0xa8,0xa8,0x0,0x0,0x6,0x0,0x0,0xe0,0xa0,
	0xa0,0xa0,0xa0,0x0,0x0,0x4,0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,
	0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x80,0x80,0x4,0x0,0x0,0xe0,0xa0,0xa0,0xa0,
	0xe0,0x20,0x30,0x4,0x0,0x0,0xc0,0x80,0x80,0x80,0x80,0x0,0x0,0x3,0x0,0x0,
	0xc0,0x80,0xc0,0x40,0xc0,0x0,0x0,0x3,0x80,0x80,0xc0,0x80,0x80,0x80,0xc0,0x0,
	0x0,0x3,0x0,0x0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,0xa0,0xa0,
	0xe0,0xc0,0x80,0x0,0x0,0x4,0x0,0x0,0x88,0xa8,0xf8,0xd8,0x88,0x0,0x0,0x6,
	0x0,0x0,0xa0,0xe0,0x40,0xe0,0xa0,0x0,0x0,0x4,0x0,0x0,0xa0,0xa0,0xa0,0xa0,
	0xe0,0x20,0xe0,0x4,0x0,0x0,0xf0,0x30,0x60,0xc0,0xf0,0x0,0x0,0x5,0x81,0x8d,
	0xe1,0xa0,0xa0,0xa0,0xa0,0x0,0x0,0x9,0x2,0x1a,0xc2,0x80,0xc0,0x40,0xc0,0x0,
	0x0,0x8,0xfe,0xfc,0xf8,0xfc,0xfe,0xdf,0x8e,0x4,0x0,0x7,0x7f,0x3f,0x1f,0x3f,
	0x7f,0xfb,0x71,0x20,0x0,0x8,0x4,0x8e,0xdf,0xfe,0xfc,0xf8,0xfc,0xfe,0x0,0x8,
	0x20,0x71,0xfb,0x7f,0x3f,0x1f,0x3f,0x7f,0x0,0x7,0xff,0x81,0x81,0x81,0x81,0x81,
	0x81,0xff,0x0,0x9,0x0,0x0,0xe0,0x80,0x80,0x80,0xe0,0x40,0xc0,0x4,0x60,0x0,
	0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xc0,0x0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,
	0x0,0x4,0x40,0xa0,0x40,0x40,0x40,0x40,0x40,0x0,0x0,0x4,0x40,0xa0,0xe0,0x20,
	0xe0,0xa0,0xe0,0x0,0x0,0x4,0x40,0xa0,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,
	0x40,0xa0,0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xe0,0x0,0xa0,0xa0,0xa0,0xa0,
	0xe0,0x0,0x0,0x4,0xc0,0x0,0xe0,0x20,0xe0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0xa0,
	0xa0,0xa0,0xe0,0xa0,0xa0,0x0,0x0,0x4,0xc0,0xa0,0xa0,0xc0,0xa0,0xa0,0xc0,0x0,
	0x0,0x4,0xe0,0x80,0x80,0x80,0x80,0x80,0xe0,0x0,0x0,0x4,0xc0,0xa0,0xa0,0xa0,
	0xa0,0xa0,0xc0,0x0,0x0,0x4,0xe0,0x80,0x80,0xe0,0x80,0x80,0xe0,0x0,0x0,0x4,
	0xe0,0x80,0x80,0xe0,0x80,0x80,0x80,0x0,0x0,0x4
};

static int DrawChar (int col, int xoffset, char *scrline, int chr)
{
	const char *font_pos;
	char *pix;
	int i;
	
	font_pos = font_bmp;
	font_pos += (chr&0xff)*10;
	scrline += xoffset;
	
	if (xoffset < 0) {
		font_pos += 9;
		return xoffset + *font_pos;
	}
	
	for (i=0; i<8; i++, font_pos++, scrline += SCREENBYTES_LINE) {
		pix = scrline;
		if (xoffset > 319) continue;
		if (*font_pos & 0x80) *pix = col;
		pix++;
		if (xoffset+1 > 319) continue;
		if (*font_pos & 0x40) *pix = col;
		pix++;
		if (xoffset+2 > 319) continue;
		if (*font_pos & 0x20) *pix = col;
		pix++;
		if (xoffset+3 > 319) continue;
		if (*font_pos & 0x10) *pix = col;
		pix++;
		if (xoffset+4 > 319) continue;
		if (*font_pos & 0x8) *pix = col;
		pix++;
		if (xoffset+5 > 319) continue;
		if (*font_pos & 0x4) *pix = col;
		pix++;
		if (xoffset+6 > 319) continue;
		if (*font_pos & 0x2) *pix = col;
		pix++;
		if (xoffset+7 > 319) continue;
		if (*font_pos & 0x1) *pix = col;
	}
	/* width of character */
	font_pos++;
	i = *font_pos;
	return xoffset + i;
}

#define MAX_QUEUED_STRINGS	200
struct QueuedString {
	int x, y, col;
	unsigned char str[64];
} queued_strings[MAX_QUEUED_STRINGS];
int queued_string_pos;

void Nu_QueueDrawStr ()
{
	/* S-6: was an assert; a full queue drops the string */
	if (queued_string_pos >= MAX_QUEUED_STRINGS) return;
	strncpy (queued_strings[queued_string_pos].str, GetReg (REG_A0) + STRam, 64);
	queued_strings[queued_string_pos].str[63] = 0;
	queued_strings[queued_string_pos].x = GetReg (REG_D1);
	queued_strings[queued_string_pos].y = GetReg (REG_D2);
	queued_strings[queued_string_pos++].col = GetReg (REG_D0);
}

int DrawStr (int xpos, int ypos, int col, unsigned char *str, bool shadowed)
{
	int x, y, chr;
	char *screen;

	x = xpos;
	y = ypos;
	
	if ((y > 192) || (y<0)) return x;
set_line:
	screen = LOGSCREEN2;
	screen += SCREENBYTES_LINE * y;

	while (*str) {
		chr = *(str++);
		
		if (chr < 0x1e) {
			if (chr == '\r') {
				y += 10;
				x = xpos;
				goto set_line;
			}
			else if (chr == 1) col = *(str++);
			continue;
		} else if (chr == 0x1e) {
			/* read new xpos */
			x = *(str++);
			x *= 2;
			continue;
		} else if (chr < 0x20) {
			/* Read new position */
			x = *(str++);
			x *= 2;
			y = *(str++);
			goto set_line;
		}
		
		//if (x > 316) continue;

		hdtext_glyph (font_bmp, x, y, col, chr-0x20, shadowed);
		if (shadowed) {
			DrawChar (0, x+1, screen+SCREENBYTES_LINE, chr-0x20);
		}
		x = DrawChar (col, x, screen, chr-0x20);
	}

	return x;
}

static void push_ortho ()
{
	glDisable (GL_DEPTH_TEST);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, 320, 0, 200, -1, 1);

	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
}

static void pop_ortho ()
{
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
}

/* Ctrl-E: HD and the old software renderer.  The wireframe debug view
 * joins the cycle only with FE2_WIREFRAME=1. */
void Screen_ToggleRenderer ()
{
	const char *w = getenv ("FE2_WIREFRAME");

	if (w && *w && *w != '0') {
		use_renderer++;
		if (use_renderer >= R_MAX) use_renderer = 0;
	} else
		use_renderer = use_renderer == R_GL ? R_OLD : R_GL;
}

/* --- The galaxy map, redrawn in HD (GL, not under --classic-ui) ------
 *
 * The map's own mode word, read only: 0xff the map, 2 the picture of
 * the whole galaxy, 1 a system's info page (left alone), 0 elsewhere. */
#define A6_GALMAP_MODE	34
static int galmap;	/* 1: the map, 2: the galaxy picture, 0: neither */

static void galmap_update (void)
{
	int m = classic_ui ? 0 : STMemory_ReadByte (__DL5eb6_a6_base + A6_GALMAP_MODE) & 0xff;

	/* the old renderer keeps its look: names found, no halo or declutter;
	 * --pixel-art keeps the game's own galaxy picture */
	hdtext_map = m == 0xff || m == 2 ? 1 + (use_renderer == R_GL) : 0;
	hdtext_focus_x = -1;	/* until the selected star is drawn */
	galmap = use_renderer != R_GL ? 0 : m == 0xff ? 1 : m == 2 && !pixel_art ? 2 : 0;
}

/* The galaxy picture is 320x168 of dithered greys.  Where hdtext took a
 * name out, fill from the picture ten rows away (not with one flat
 * colour, which left a dark box behind each name), then blur the greys
 * into a smooth density, coloured from deep blue-violet where stars are
 * few to warm white where they are thick, drawn filtered under the
 * raster.  The raster keeps only what is not grey (the scale bar); the
 * magenta marker pixels (where you are) become smooth rings. */
#define GAL_H	168
static unsigned char galaxy_rgba[GAL_H][320][4];
static short galaxy_mark[8][2];
static int galaxy_marks;
static const float galaxy_ramp[6][4] = {	/* density, r, g, b */
	{ 0.00f,   4,   3,  14 }, { 0.15f,  22,  14,  58 }, { 0.35f,  70,  40, 130 },
	{ 0.60f, 170, 100, 150 }, { 0.80f, 240, 190, 160 }, { 1.00f, 255, 246, 228 } };

static void galaxy_soften (const unsigned char *r, unsigned char *out)
{
	static unsigned char grey[256];
	int x, y, i, j, dx, dy;

	for (i = 0; i < 256; i++) {
		unsigned int p = MainRGBPalette[i];
		int a = p & 0xff, b = p >> 8 & 0xff, c = p >> 16 & 0xff;
		int hi = a > b ? (a > c ? a : c) : (b > c ? b : c);
		int lo = a < b ? (a < c ? a : c) : (b < c ? b : c);
		grey[i] = i != 255 && hi - lo < 24;
	}
	galaxy_marks = 0;
	for (i = 0; i < GAL_H * 320; i++) {
		unsigned int p = MainRGBPalette[out[i]];
		j = i < 10 * 320 ? i + 10 * 320 : i - 10 * 320;
		if (out[i] != r[i] && out[j] == r[j]) out[i] = r[j];
		else if ((p & 0xff) == (p >> 16 & 0xff) && (p & 0xff) >= (p >> 8 & 0xff) + 64 &&
			 galaxy_marks < 8) {
			galaxy_mark[galaxy_marks][0] = i % 320;
			galaxy_mark[galaxy_marks++][1] = i / 320;
			out[i] = 255;
		}
	}
	for (y = 0; y < GAL_H; y++)
		for (x = 0; x < 320; x++) {
			float s = 0, w = 0, t;
			for (dy = -2; dy <= 2; dy++)
				for (dx = -2; dx <= 2; dx++) {
					int yy = y + dy, xx = x + dx, k = (3 - abs (dx)) * (3 - abs (dy));
					if (yy < 0 || yy >= GAL_H || xx < 0 || xx >= 320 ||
					    !grey[out[yy * 320 + xx]]) continue;
					s += (MainRGBPalette[out[yy * 320 + xx]] >> 8 & 0xff) * k;
					w += k;
				}
			t = w ? s / w / 160 : 0;
			for (i = 1; i < 5 && t > galaxy_ramp[i][0]; i++);
			t = (t - galaxy_ramp[i - 1][0]) / (galaxy_ramp[i][0] - galaxy_ramp[i - 1][0]);
			if (t > 1) t = 1;
			for (j = 1; j < 4; j++)
				galaxy_rgba[y][x][j - 1] = galaxy_ramp[i - 1][j] +
					t * (galaxy_ramp[i][j] - galaxy_ramp[i - 1][j]);
			galaxy_rgba[y][x][3] = 255;
		}
	for (i = 0; i < GAL_H * 320; i++)
		if (grey[out[i]]) out[i] = 255;
}

static void disc_strips (float r0, float r1, float r2, float r3,
			 float r, float g, float b, float a);

static void galaxy_draw (void)
{
	int i;

	if (!galaxy_tex) {
		glGenTextures (1, &galaxy_tex);
		glBindTexture (GL_TEXTURE_2D, galaxy_tex);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, galaxy_tex);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 320, GAL_H, GL_RGBA, GL_UNSIGNED_BYTE, galaxy_rgba);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	/* texel centres at the edges: nothing outside the picture blends in */
	glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (0.5f / 512, (GAL_H - 0.5f) / 256);
		glVertex2i (0, 200 - GAL_H);
		glTexCoord2f (319.5f / 512, (GAL_H - 0.5f) / 256);
		glVertex2i (320, 200 - GAL_H);
		glTexCoord2f (0.5f / 512, 0.5f / 256);
		glVertex2i (0, 200);
		glTexCoord2f (319.5f / 512, 0.5f / 256);
		glVertex2i (320, 200);
	glEnd ();
	glDisable (GL_TEXTURE_2D);
	/* where you are: a smooth ring and glow, round on the screen */
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (i = 0; i < galaxy_marks; i++) {
		glPushMatrix ();
		glTranslatef (galaxy_mark[i][0] + 0.5f, 200 - galaxy_mark[i][1] - 0.5f, 0);
		glScalef (1, 200.0f * pic_w / (320.0f * pic_h), 1);
		disc_strips (0, 0, 2.4f, 3.4f, 0.94f, 0.5f, 0.94f, 0.18f);
		disc_strips (1.7f, 2.0f, 2.4f, 2.7f, 1, 0.62f, 1, 1);
		disc_strips (0, 0, 0.5f, 1.0f, 1, 0.9f, 1, 1);
		glPopMatrix ();
	}
	glDisable (GL_BLEND);
}

/* The map's sky: the game's colour lit in the middle and falling away to
 * near black at the edges, instead of one flat fill. */
static void galmap_backdrop (void)
{
	unsigned int p = MainRGBPalette[fe2_bgcol];
	float r = (p & 0xff) / 255.0f, g = (p >> 8 & 0xff) / 255.0f, b = (p >> 16 & 0xff) / 255.0f;
	int i;

	push_ortho ();
	glBegin (GL_TRIANGLE_FAN);
	glColor3f (r * 1.25f + 0.03f, g * 1.25f + 0.02f, b * 1.25f + 0.06f);
	glVertex2f (160, 100);
	glColor3f (r * 0.2f, g * 0.2f, b * 0.25f);
	for (i = 0; i <= 32; i++)
		glVertex2f (160 + 232 * cosf (i * (float) M_PI / 16), 100 + 150 * sinf (i * (float) M_PI / 16));
	glEnd ();
	pop_ortho ();
}

static void draw_control_panel ()
{
	int x, y;
	unsigned char *scr;
	unsigned int line[320];
	unsigned int *pal;
	static unsigned char hd_screen[320*200];

	set_ctrl_viewport ();
	
	/* this is a big fucking hack to make starsystem names
	 * in the starmap show up. they are the only bitmap text
	 * things drawn within the fe2 3d renderer, which makes
	 * them fucking annoying. */
	y = logscreen2;
	logscreen2 = physcreen;
	for (x=0; x<queued_string_pos; x++) {
		hdtext_queue = x + 1;	/* which name, for hdtext */
		DrawStr (	queued_strings[x].x,
				queued_strings[x].y,
				queued_strings[x].col,
				queued_strings[x].str,
				FALSE);
	}
	hdtext_queue = 0;
	logscreen2 = y;
	/****************************************************/
	
	/* the screen with what the HD layers redraw taken out */
	scr = VideoRaster;
	if (!pixel_text || !pixel_art || !pixel_faces || !classic_ui) {
		hdtext_frame (font_bmp, VideoRaster, hd_screen);
		hdui_frame (VideoRaster, hd_screen, MainRGBPalette, CtrlRGBPalette);
		/* before hdstill: the face parts and the message thumbnail
		 * it takes out are hdstill's to leave be (and log deferred) */
		hdface_frame (VideoRaster, hd_screen, MainRGBPalette, CtrlRGBPalette);
		hdstill_frame (VideoRaster, hd_screen, MainRGBPalette, CtrlRGBPalette);
		hdback_frame (VideoRaster, hd_screen);
		scr = hd_screen;
		if (galmap == 2) galaxy_soften (VideoRaster, hd_screen);
	}
	
	/* intro likes black at the bottom */
	/* hack hack hack what a pile of shite this is */
	push_ortho ();
	glColor3f (0.0f, 0.0f, 0.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glVertex3f (0, 32, 0);
		glVertex3f (319, 32, 0);
		glVertex3f (0, 0, 0);
		glVertex3f (319, 0, 0);
	glEnd ();
	
	hdui_draw_under (MainRGBPalette, CtrlRGBPalette);
	if (galmap == 2 && scr == hd_screen) galaxy_draw ();
	
	glEnable (GL_TEXTURE_2D);
	
	glBindTexture (GL_TEXTURE_2D, screen_tex);

	pal = MainRGBPalette;
	
	/* copy whole 320x200 screen to texture */
	for (y=0; y<200; y++) {
		/* the control panel at the bottom has its own palette */
		if (y >= 168) pal = CtrlRGBPalette;
		
		for (x=0; x<320; x++) {
			/* in gl mode the ui texture has transparent crap where no shit is */
			if ((*(scr)) == 255) {
				scr++;
				line[x] = 0;
			} else {
				line[x] = pal[*(scr++)];
			}
		}
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y, 320, 1, GL_RGBA, GL_UNSIGNED_BYTE, line);
	}
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	
	if (use_renderer == R_OLD) {
		glBegin (GL_TRIANGLE_STRIP);
			glTexCoord2f (0.0f, 200.0f/SCR_TEX_H);
			glVertex2i (0, 0);
			glTexCoord2f (320.0f/SCR_TEX_W, 200.0f/SCR_TEX_H);
			glVertex2i (320, 0);
			glTexCoord2f (0.0f, 0.0f);
			glVertex2i (0, 200);
			glTexCoord2f (320.0f/SCR_TEX_W, 0.0f);
			glVertex2i (320, 200);
		glEnd ();
	} else {
		glEnable (GL_BLEND);
		glBegin (GL_TRIANGLE_STRIP);
			glTexCoord2f (0.0f, 200.0f/SCR_TEX_H);
			glVertex2i (0, 0);
			glTexCoord2f (320.0f/SCR_TEX_W, 200.0f/SCR_TEX_H);
			glVertex2i (320, 0);
			glTexCoord2f (0.0f, 0.0f);
			glVertex2i (0, 200);
			glTexCoord2f (320.0f/SCR_TEX_W, 0.0f);
			glVertex2i (320, 200);
		glEnd ();
		glDisable (GL_BLEND);
	}
	hdstill_draw (MainRGBPalette, CtrlRGBPalette);
	hdface_draw (MainRGBPalette, CtrlRGBPalette);
	hdtext_draw (font_bmp, MainRGBPalette, CtrlRGBPalette);
	hdui_draw_over (MainRGBPalette, CtrlRGBPalette);
	endcard_draw ();
	controls_page_draw ();
	hdtip_draw ();
	
	glDisable (GL_TEXTURE_2D);
	
	pop_ortho ();
}

static void _BuildRGBPalette (unsigned int *rgb, unsigned short *st, int len)
{
	int i;
	int st_col, r, g, b;

	for (i=0; i<len; i++, st++) {
		st_col = *st;
		b = (st_col & 0xf)<<4;
		g = (st_col & 0xf0);
		r = (st_col & 0xf00)>>4;
		rgb[i] = 0xff000000 | (b<<16) | (g<<8) | (r);
	}
}

static inline void split_rgb444b (int rgb, int *r, int *g, int *b)
{
	*r = (rgb & 0xf00) >> 4;
	*g = (rgb & 0xf0);
	*b = (rgb & 0xf) << 4;
}

static inline void split_rgb444i (unsigned int rgb, unsigned int *r, unsigned int *g, unsigned int *b)
{
	*r = (rgb & 0xf00) << 20;
	*g = (rgb & 0xf0) << 24;
	*b = (rgb & 0xf) << 28;
}

static inline void read_m68k_vertex (int st_vptr, int output[3])
{
	output[0] = STMemory_ReadLong (st_vptr);
	output[1] = STMemory_ReadLong (st_vptr+4);
	output[2] = -STMemory_ReadLong (st_vptr+8);
}

struct ZNode {
	unsigned int z;
	struct ZNode *less, *more;
	void *data;
};

#define MAX_OBJ_DATA	(4<<20)
static unsigned char obj_data_area[MAX_OBJ_DATA];
static int obj_data_pos;
#define MAX_ZNODES	16384
static struct ZNode znode_buf[MAX_ZNODES];
static int znode_buf_pos;
static struct ZNode *znode_start;
static struct ZNode *znode_cur;

static inline void znode_databegin ()
{
	znode_cur->data = &obj_data_area[obj_data_pos];
}

/*
 * S-6: the scene buffers are bounded. When a frame does not fit in
 * obj_data_area or znode_buf, scene_overflow is set, every later write of
 * the frame is dropped, and Nu_DrawScreen does not walk the (now cut
 * short) display lists: the 3D view of that one frame is lost, nothing
 * else. Nu_3DViewInit clears it for the next frame.
 */
static int scene_overflow;
static unsigned int scene_overflows;

static void ztree_sync (void);
static void hull_latch (void);

static inline int znode_room (int n)
{
	/* a sub-tree the engine opened goes in before anything is written */
	ztree_sync ();
	if (scene_overflow) return 0;
	if (n > MAX_OBJ_DATA - obj_data_pos) {
		scene_overflow = 1;
		return 0;
	}
	return 1;
}

static inline void znode_wrlong (int val)
{
	if (!znode_room (4)) return;
	*((int*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos+=4;
}
static inline void znode_wrword (short val)
{
	if (!znode_room (2)) return;
	*((short*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos+=2;
}
static inline void znode_wrbyte (char val)
{
	if (!znode_room (1)) return;
	*((char*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos++;
}
static inline void znode_wrblock (const void *data, int n)
{
	if (!znode_room (n)) return;
	memcpy (obj_data_area + obj_data_pos, data, n);
	obj_data_pos += n;
}

static inline void znode_wrnormal (p68K loc)
{
	znode_wrword (STMemory_ReadWord (loc));
	znode_wrword (STMemory_ReadWord (loc+2));
	znode_wrword (STMemory_ReadWord (loc+4));
}

static void znode_wrmatrix (p68K loc)
{
	znode_wrword (STMemory_ReadWord (loc));
	znode_wrword (STMemory_ReadWord (loc+2));
	znode_wrword (STMemory_ReadWord (loc+4));
	znode_wrword (STMemory_ReadWord (loc+6));
	znode_wrword (STMemory_ReadWord (loc+8));
	znode_wrword (STMemory_ReadWord (loc+10));
	znode_wrword (STMemory_ReadWord (loc+12));
	znode_wrword (STMemory_ReadWord (loc+14));
	znode_wrword (STMemory_ReadWord (loc+16));
}

static inline void znode_wrvertex (p68K loc)
{
	znode_wrlong (STMemory_ReadLong (loc));
	znode_wrlong (STMemory_ReadLong (loc+4));
	znode_wrlong (-STMemory_ReadLong (loc+8));
}

static inline void znode_wrlightsource (p68K loc)
{
	znode_wrlong (-STMemory_ReadWord (loc));
	znode_wrlong (-STMemory_ReadWord (loc+2));
	znode_wrlong (STMemory_ReadWord (loc+4));
}

static inline void znode_wrcolor (int rgb444col)
{
	int r,g,b;
	split_rgb444b (rgb444col, &r, &g, &b);
	znode_wrbyte (r);
	znode_wrbyte (g);
	znode_wrbyte (b);
	znode_wrbyte (0);
}

static inline int znode_rdlong (void **data)
{
	int val = *((int*)(*data));
	(*data) += 4;
	return val;
}
static inline short znode_rdword (void **data)
{
	short val = *((short*)(*data));
	(*data) += 2;
	return val;
}
static inline char znode_rdbyte (void **data)
{
	char val = *((char*)(*data));
	(*data)++;
	return val;
}

static void znode_rdmatrix (void **data, GLfloat m[16])
{
	short val;

#define rdmatrixval(idx)	\
	{	\
		val = znode_rdword (data);	\
		m[idx] = ((float)val)/-32768.0;	\
	}
	
	rdmatrixval (0);
	rdmatrixval (1);
	rdmatrixval (2);
	m[3] = 0.0f;
	rdmatrixval (4);
	rdmatrixval (5);
	rdmatrixval (6);
	m[7] = 0.0f;
	rdmatrixval (8);
	rdmatrixval (9);
	rdmatrixval (10);
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;

	//m[0] = -m[0];
	//m[5] = -m[5];
	//m[10] = -m[10];
}

static inline void znode_rdnormal (void **data, short normal[3])
{
	normal[0] = znode_rdword (data);
	normal[1] = znode_rdword (data);
	normal[2] = znode_rdword (data);
}

static inline void znode_rdvertex (void **data, int vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdvertexf (void **data, float vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdvertexd (void **data, GLdouble vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdcolorv (void **data, int *rgb)
{
	rgb[0] = (unsigned char) znode_rdbyte (data);
	rgb[1] = (unsigned char) znode_rdbyte (data);
	rgb[2] = (unsigned char) znode_rdbyte (data);
	(*data)++;
}

static inline void znode_rdcolor (void **data, int *r, int *g, int *b)
{
	*r = znode_rdbyte (data);
	*g = znode_rdbyte (data);
	*b = znode_rdbyte (data);
	(*data)++;
}

enum NuPrimitive {
	NU_END,
	NU_TRIANGLE,
	NU_QUAD,
	NU_LINE,
	NU_BEZIER_LINE,
	NU_TEARDROP,
	NU_COMPLEX_SNEXT,
	NU_COMPLEX_START,
	NU_COMPLEX_END,
	NU_COMPLEX_INNER,
	NU_COMPLEX_BEZIER,
	NU_TWINKLYCIRCLE,
	NU_PLANET,
	NU_CIRCLE,
	NU_CYLINDER,
	NU_BLOB,
	NU_OVALTHINGY,
	NU_POINT,
	NU_2DLINE,
	NU_SUBTREE,
	NU_ATMOSPHERE,
	NU_SKYBAND,
	NU_COMPLEX_OVAL,
	NU_MAX
};

static inline void end_node ()
{
	znode_wrlong (0);
}

/* where the writers point once znode_buf is full (S-6) */
static struct ZNode znode_dummy;

static void add_node (struct ZNode **node, unsigned int zval)
{
	/* end previous znode display list!!!!!!! */
	if (znode_cur) end_node ();
	if (znode_buf_pos >= MAX_ZNODES) {
		/* S-6: was an assert. The frame is dropped (see
		 * scene_overflow); leave *node alone */
		scene_overflow = 1;
		znode_cur = &znode_dummy;
		return;
	}
	
	*node = znode_cur = &znode_buf[znode_buf_pos++];
	znode_cur->z = zval;
	znode_cur->less = NULL;
	znode_cur->more = NULL;
	znode_databegin ();
}

static void znode_insert (struct ZNode *node, unsigned int zval)
{
	if (zval > node->z) {
		if (node->more) {
			znode_insert (node->more, zval);
		} else {
			add_node (&node->more, zval);
		}
	} else {
		if (node->less) {
			znode_insert (node->less, zval);
		} else {
			add_node (&node->less, zval);
		}
	}
}

/*
 * Detached znodes: a node whose data does not go into the obj_data_area
 * stream, so that it can be added to the tree in the middle of another
 * primitive without cutting the current node's data short (add_node ends
 * the current node). Their data is written in one go.
 */
#define MAX_DETACHED_DATA	(1<<16)
static unsigned char detached_data[MAX_DETACHED_DATA];
static int detached_pos;

static void *add_detached_node (struct ZNode **root, unsigned int zval, int size)
{
	struct ZNode *n, **link = root;
	void *data;

	if (znode_buf_pos >= MAX_ZNODES || detached_pos + size + 4 > MAX_DETACHED_DATA)
		return NULL;
	n = &znode_buf[znode_buf_pos++];
	n->z = zval;
	n->less = n->more = NULL;
	n->data = data = &detached_data[detached_pos];
	detached_pos += size + 4;
	/* the list end */
	*(int *) ((unsigned char *) data + size) = 0;
	/* same ordering as znode_insert */
	while (*link) link = zval > (*link)->z ? &(*link)->more : &(*link)->less;
	*link = n;
	return data;
}

static bool no_znodes_kthx;

/*
 * Nested z-trees.
 *
 * The engine does not sort everything in one tree: object command $15
 * (fe2.s L3a4b4) inserts a node and makes it the root of a new tree, into
 * which everything that follows is sorted, until the matching pop
 * (l3a494). The whole sub-tree is then painted as one block, at the place
 * of that node in the parent tree. Starports rely on it: their ground
 * decals (runways, roads, lakes) and the big terrain polygons around them
 * only come out in the right order that way.
 *
 * The engine pushes and pops with no hcall. It keeps the heads of its trees
 * in L385d0_3dview_thing2, the current one first: l3a50a pushes right after
 * its Nu_InsertZNode, l3a494 pops. The host follows that head before every
 * insert and every write (ztree_sync): a head it has on its stack means
 * pops back to it, a new one a push. So a push is seen before anything is
 * written into the node that holds the new tree.
 */
#define MAX_SUBTREES		256
#define MAX_ZTREE_DEPTH		16
static struct ZNode *subtree_root[MAX_SUBTREES];
static int num_subtrees;
/* the tree Nu_InsertZNode sorts into */
static struct ZNode **cur_ztree = &znode_start;
static struct ZNode **ztree_stack[MAX_ZTREE_DEPTH];
static int ztree_depth;
/* the engine's head for each level; ztree_heads[ztree_depth] is the current */
static unsigned int ztree_heads[MAX_ZTREE_DEPTH + 1];
static bool ztree_syncing;

/* counted for FE2_PLANET_STATS */
static unsigned int ztree_pushes, complex_starts, complex_aborts;

static void ztree_push (unsigned int head)
{
	ztree_pushes++;
	ztree_stack[ztree_depth++] = cur_ztree;
	ztree_heads[ztree_depth] = head;
	/* out of sub-trees, or nothing to hang it on: keep sorting flat */
	if (num_subtrees == MAX_SUBTREES || !znode_cur) return;
	subtree_root[num_subtrees] = NULL;
	/* the node the engine just inserted draws the sub-tree */
	znode_wrlong (NU_SUBTREE);
	znode_wrlong (num_subtrees);
	cur_ztree = &subtree_root[num_subtrees++];
}

static void ztree_sync (void)
{
	unsigned int head;
	int d;

	if (use_renderer == R_OLD || ztree_syncing) return;
	head = STMemory_ReadLong (__DL385d0_3dview_thing2);
	if (head == ztree_heads[ztree_depth]) return;
	for (d = ztree_depth - 1; d >= 0; d--) {
		if (ztree_heads[d] == head) {
			ztree_depth = d;
			cur_ztree = ztree_stack[d];
			return;
		}
	}
	/* deeper than the engine's own stack (4) can go: keep sorting here */
	if (ztree_depth == MAX_ZTREE_DEPTH) {
		ztree_heads[ztree_depth] = head;
		return;
	}
	ztree_syncing = TRUE;
	ztree_push (head);
	ztree_syncing = FALSE;
}

void Nu_InsertZNode ()
{
	unsigned int zval = GetReg (4);
	if (use_renderer == R_OLD) return;
	ztree_sync ();
	if (no_znodes_kthx) return;
	if (*cur_ztree == NULL) {
		add_node (cur_ztree, zval);
	} else {
		znode_insert (*cur_ztree, zval);
	}
}

void Nu_3DViewInit ()
{
	queued_string_pos = 0;
	//printf ("3dviewinit()\n");
	znode_buf_pos = 0;
	//printf ("%d bytes object data\n", obj_data_pos);
	obj_data_pos = 0;
	detached_pos = 0;
	procfx_sky_seen = 0;

	//add_node (&znode_start, 0);
	znode_start = NULL;
	znode_cur = NULL;
	no_znodes_kthx = FALSE;
	num_subtrees = 0;
	cur_ztree = &znode_start;
	ztree_depth = 0;
	/* L385ea_Clear3DView has just set it to the base */
	ztree_heads[0] = STMemory_ReadLong (__DL385d0_3dview_thing2);
	scene_overflow = 0;
	hull_latch ();
}

static void lighting_on (float light_vec[4], int rgb444_light_col, int rgb444_extra_col, int rgb444_obj_col)
{
	bool do_not_light;
	unsigned int extra_col[4], obj_col[4], light_col[4];

	do_not_light = rgb444_obj_col & (1<<8);
	
	/* object color bit 0x8 set means DO NOT LIGHT */
	if (do_not_light) {
		rgb444_obj_col ^= (1<<8);
	} else {
		split_rgb444i (rgb444_light_col, &light_col[0], &light_col[1], &light_col[2]);
		light_col[3] = 0;
		light_vec[3] = 0.0f;
	}

	if (rgb444_obj_col & (1<<4)) {
		rgb444_obj_col ^= (1<<4);
		split_rgb444i (rgb444_obj_col, &obj_col[0], &obj_col[1], &obj_col[2]);
		split_rgb444i (rgb444_extra_col, &extra_col[0], &extra_col[1], &extra_col[2]);
		obj_col[0] += extra_col[0];
		obj_col[1] += extra_col[1];
		obj_col[2] += extra_col[2];
	} else {
		split_rgb444i (rgb444_obj_col, &obj_col[0], &obj_col[1], &obj_col[2]);
	}
	obj_col[3] = 0;

	if (do_not_light) {
		glDisable (GL_LIGHTING);
		glDisable (GL_LIGHT0);
		glColor3ui (obj_col[0], obj_col[1], obj_col[2]);
	} else {
		glLightfv (GL_LIGHT0, GL_POSITION, light_vec);
		glLightiv (GL_LIGHT0, GL_DIFFUSE, light_col);
		glLightiv (GL_LIGHT0, GL_AMBIENT, obj_col);
		glEnable (GL_LIGHTING);
		glEnable (GL_LIGHT0);
	}
}

static void lighting_off ()
{
	glDisable (GL_LIGHTING);
	glDisable (GL_LIGHT0);
}

/*
 * Billboard helpers: several 2D "sprite-like" primitives (twinkly
 * circles/stars, distant planet dots, glow blobs...) are just a
 * gluDisk() translated to a world position, with no attempt to keep
 * them facing the camera. Viewed close to edge-on they degenerate to a
 * near-invisible line, which is one of the main things making the GL
 * renderer look incomplete compared to the ST software renderer (where
 * these were always flat, screen-aligned sprites).
 *
 * billboard_begin() pushes a modelview matrix translated to (x,y,z)
 * with the camera's rotation cancelled out, so that anything drawn in
 * the local XY plane (e.g. gluDisk, which faces local +Z) ends up
 * facing the viewer regardless of the current camera orientation.
 */
static void billboard_begin (float x, float y, float z)
{
	GLfloat m[16];

	glPushMatrix ();
	glTranslatef (x, y, z);
	glGetFloatv (GL_MODELVIEW_MATRIX, m);

	/* Cancel the rotation (and any scale) part of the matrix, keep the
	 * translation: this leaves the local axes aligned with the camera's
	 * eye-space axes, i.e. always facing the viewer. */
	m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f;
	m[4] = 0.0f; m[5] = 1.0f; m[6] = 0.0f;
	m[8] = 0.0f; m[9] = 0.0f; m[10] = 1.0f;

	glLoadMatrixf (m);
}

static void billboard_end ()
{
	glPopMatrix ();
}

/* Object-space position of the eye, i.e. the inverse modelview applied
 * to the origin (rotation plus uniform scale and translation assumed).
 * The scene's vertices arrive already in eye space, so this is normally
 * the origin; it is computed rather than assumed so that a flare still
 * faces the viewer under any modelview it is drawn with. */
static void get_eye_point (float eye[3])
{
	GLfloat m[16];
	float s2;
	int i;

	glGetFloatv (GL_MODELVIEW_MATRIX, m);
	s2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
	if (!(s2 > 0.0f)) s2 = 1.0f;
	for (i = 0; i < 3; i++)
		eye[i] = -(m[4*i]*m[12] + m[4*i+1]*m[13] + m[4*i+2]*m[14]) / s2;
}

static void cross3 (const float a[3], const float b[3], float out[3])
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

void CALLBACK beginCallback(GLenum which)
{
   glBegin(which);
}

void CALLBACK errorCallback(GLenum errorCode)
{
   const GLubyte *estring;

   estring = gluErrorString(errorCode);
   fprintf(stderr, "Tessellation Error: %s\n", estring);
}

void CALLBACK endCallback(void)
{
   glEnd();
}

static int complex_col[3];
void CALLBACK vertexCallback(GLvoid *vertex, GLvoid *poly_data)
{
   const GLdouble *pointer;

   pointer = (GLdouble *) vertex;
   glColor3ub (complex_col[0], complex_col[1], complex_col[2]);
   glVertex3dv(pointer);
}
/*  combineCallback is used to create a new vertex when edges
 *  intersect.  coordinate location is trivial to calculate,
 *  but weight[4] may be used to average color, normal, or texture
 *  coordinate data.  In this program, color is weighted.
 */
/* S-2: the vertices made here have to stay alive until gluTessEndPolygon;
 * they come from a pool emptied before each gluTessBeginPolygon (see
 * Nu_DrawComplexStart) instead of a malloc nothing ever freed. When the
 * pool runs out we return NULL, which makes GLU report
 * GLU_TESS_NEED_COMBINE_CALLBACK and drop the polygon. */
#define MAX_COMBINE_VERTICES	4096
static GLdouble combine_vertices[MAX_COMBINE_VERTICES][3];
static int combine_vpos;

void CALLBACK combineCallback(GLdouble coords[3], 
                     GLdouble *vertex_data[4],
                     GLfloat weight[4], GLdouble **dataOut )
{
   GLdouble *vertex;

   if (combine_vpos >= MAX_COMBINE_VERTICES) {
      *dataOut = NULL;
      return;
   }
   vertex = combine_vertices[combine_vpos++];

   vertex[0] = coords[0];
   vertex[1] = coords[1];
   vertex[2] = coords[2];
   *dataOut = vertex;
}
/* Room for one complex shape. Curves and circles get more segments the
 * bigger they are on screen (bezier_steps, oval_steps), so a shape that
 * fills the view can use far more than the 10 per curve it used to. */
#define MAX_TESS_VERTICES	4096
static GLdouble tess_vertices[MAX_TESS_VERTICES][3];
static int tess_vpos;

static GLdouble tessModelMatrix[16];
static GLdouble tessProjMatrix[16];
static GLint tessViewport[4];

static bool do_start_complex;
static int complex_col_rgb444;
/* where the current complex shape starts in obj_data_area, -1 if nothing
 * written yet; and whether the engine dropped it (complex_check_abort) */
static int complex_data_start = -1;
static bool complex_aborted;
/* where the engine's own primitive for the shape starts (primitives_end at
 * Nu_ComplexStart), 0 if there is no shape */
static unsigned int complex_st_start;

/* style 5 plates complex shapes too: see hull_put_cctx */
static void hull_put_cctx (void);
static void hull_rdcctx (void **data);
static int hull_complex_begin (GLdouble c[][3], int n, void **data);
static void hull_complex_end (void);

/* procfx sky band: complex_sky when Nu_ComplexStart meets the background
 * object; sky_pending when the next polygon drawn is the Milky Way */
static int complex_sky, sky_pending, sky_active;
static float complex_sky_m[9], sky_m[9];

static void put_complex_start_4real ()
{
	complex_data_start = obj_data_pos;
	if (complex_sky) {	/* in the shape's data, so an abort forgets it too */
		znode_wrlong (NU_SKYBAND);
		znode_wrblock (complex_sky_m, sizeof complex_sky_m);
	}
	znode_wrlong (NU_COMPLEX_START);
	znode_wrcolor (complex_col_rgb444);
	hull_put_cctx ();
	no_znodes_kthx = TRUE;
}

/*
 * Some complex shapes are flagged in their model to be dropped altogether
 * when they cross the near plane (fe2.s l3b87e -> L3b78e -> L3b7ba): the
 * engine then throws away the 2D primitives it had already pushed for the
 * shape, with no hcall. It sets primitives_end back to where the shape
 * started and writes $065e there, over the $2a the shape began with, so
 * each complex hcall after the start checks that word. Forget whatever we
 * were given for the shape as well: nothing else can have been written
 * since it started, as no_znodes_kthx keeps it in the current znode.
 * Nu_ComplexEnd still follows.
 */
static void complex_check_abort (void)
{
	if (!complex_st_start || complex_aborted) return;
	if (complex_st_start <= MEM_SIZE - 2 &&
	    (STMemory_ReadWord (complex_st_start) & 0xffff) == 0x2a) return;
	if (complex_data_start >= 0) obj_data_pos = complex_data_start;
	complex_data_start = -1;
	do_start_complex = FALSE;
	complex_aborted = TRUE;
	complex_aborts++;
}

/*
 * Complex shapes are tessellated in SCREEN space: every contour vertex is
 * pushed through gluProject and the resulting triangles are drawn under an
 * ortho projection (see Nu_DrawComplexStart).
 *
 * That only works for vertices actually in front of the camera. A vertex at
 * or behind the eye plane (eye z >= 0 - znode_wrvertex negates z, so eye z is
 * negative in front) has no meaningful projection.
 *
 * This code used to simply `return` on those vertices, dropping them from the
 * contour. Dropping a vertex does NOT clip a polygon, it corrupts its outline:
 * the shape collapses into whatever the remaining points happen to describe.
 * That is why the player ship's own hull vanished in rear view (its contour
 * wraps around the camera) while the decals on it - separate triangles/quads -
 * still drew. The software renderer gets this right because fe2.s clips those
 * polygons properly in 2D (L3b276/L3b30a, handling the $80028002 sentinels),
 * a stage the GL path deliberately skips.
 *
 * So do the clipping for real: accumulate the contour in eye space, clip it
 * against the near plane (Sutherland-Hodgman), and only then project.
 */
static GLdouble contour_v[MAX_TESS_VERTICES][3];
static int contour_n;

static inline void push_contour_vertex (GLdouble v[3])
{
	if (contour_n > 0) {
		const GLdouble *p = contour_v[contour_n-1];
		if ((p[0]==v[0]) && (p[1]==v[1]) && (p[2]==v[2])) return;
	}
	if (contour_n >= MAX_TESS_VERTICES) return;

	contour_v[contour_n][0] = v[0];
	contour_v[contour_n][1] = v[1];
	contour_v[contour_n][2] = v[2];
	contour_n++;
}

/* Clip polygon `in` (n vertices) to the half space in front of the near
 * plane, i.e. eye z <= -GL_NEAR_PLANE. Returns the vertex count written to
 * `out`, which needs room for n+1. */
static int clip_contour_near (GLdouble in[][3], int n, GLdouble out[][3], int maxout)
{
	const double zlim = -(double) GL_NEAR_PLANE;
	int i, k, m = 0;

	for (i = 0; i < n; i++) {
		const GLdouble *a = in[i];
		const GLdouble *b = in[(i+1) % n];
		int a_in = (a[2] <= zlim);
		int b_in = (b[2] <= zlim);

		if (a_in && m < maxout) {
			for (k = 0; k < 3; k++) out[m][k] = a[k];
			m++;
		}
		if ((a_in != b_in) && m < maxout) {
			double t = (zlim - a[2]) / (b[2] - a[2]);
			for (k = 0; k < 3; k++) out[m][k] = a[k] + t*(b[k] - a[k]);
			m++;
		}
	}

	return m;
}

/* Clip, project and hand the current contour to the tessellator. The
 * projected coordinates must stay alive until gluTessEndPolygon, which is
 * why they go into the persistent tess_vertices[] pool. */
static void flush_contour ()
{
	static GLdouble clipped[MAX_TESS_VERTICES+1][3];
	int n, i, first = tess_vpos;

	n = clip_contour_near (contour_v, contour_n, clipped, MAX_TESS_VERTICES+1);
	contour_n = 0;
	if (n < 3) return;

	for (i = 0; i < n; i++) {
		GLdouble *d;

		if (tess_vpos >= MAX_TESS_VERTICES) break;
		d = tess_vertices[tess_vpos];

		if (!gluProject (clipped[i][0], clipped[i][1], clipped[i][2],
				 tessModelMatrix, tessProjMatrix, tessViewport,
				 &d[0], &d[1], &d[2]))
			continue;

		tess_vpos++;
		gluTessVertex (tobj, d, d);
	}
	if (sky_active) procfx_sky_feather (tess_vertices + first, tess_vpos - first);
}

void Nu_ComplexSNext ()
{
	if (use_renderer == R_OLD) return;
	complex_check_abort ();
	if (complex_aborted) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_SNEXT);
	znode_wrvertex (GetReg (REG_A0)+4);
}
void Nu_DrawComplexSNext (void **data)
{
	if (use_renderer == R_GLWIRE) {
		/* S-6: drawn at once, no need to keep it */
		GLdouble v[3];
		znode_rdvertexd (data, v);
		glVertex3dv (v);
	} else {
		GLdouble v[3];
		znode_rdvertexd (data, v);
		push_contour_vertex (v);
	}
}
void Nu_ComplexSBegin ()
{
	Nu_ComplexSNext ();
}

void Nu_ComplexStart ()
{
	if (use_renderer == R_OLD) return;
	do_start_complex = TRUE;
	complex_col_rgb444 = GetReg (REG_D6);
	complex_data_start = -1;
	complex_aborted = FALSE;
	complex_st_start = STMemory_ReadLong (__DL385c8_primitives_end);
	complex_starts++;
	complex_sky = procfx_sky_put (complex_sky_m);
}
void Nu_DrawSkyBand (void **data)
{
	memcpy (sky_m, *data, sizeof sky_m);
	*data += sizeof sky_m;
	sky_pending = 1;
}

void Nu_DrawComplexStart (void **data)
{
	tess_vpos = 0;
	combine_vpos = 0;
	
	if (use_renderer == R_GL) {
		glGetDoublev (GL_MODELVIEW_MATRIX, tessModelMatrix);
		glGetDoublev (GL_PROJECTION_MATRIX, tessProjMatrix);
		glGetIntegerv (GL_VIEWPORT, tessViewport);
		
		glMatrixMode (GL_PROJECTION);
		glPushMatrix ();
		glLoadIdentity ();
		glOrtho (tessViewport[0], tessViewport[0]+tessViewport[2], tessViewport[1], tessViewport[1]+tessViewport[3], -1, 1);

		glMatrixMode (GL_MODELVIEW);
		glPushMatrix ();
		glLoadIdentity ();
		
		gluTessNormal (tobj, 0, 0, 1);
		gluTessProperty(tobj, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
		gluTessBeginPolygon (tobj, NULL);
		gluTessBeginContour (tobj);
		contour_n = 0;
		if (sky_pending) sky_active = procfx_sky_begin (sky_m, tessProjMatrix, tessViewport);
	} else {
		glBegin (GL_LINE_STRIP);
	}
	znode_rdcolor (data, &complex_col[0], &complex_col[1], &complex_col[2]);
	glColor3ub (complex_col[0], complex_col[1], complex_col[2]);
	hull_rdcctx (data);
}


void Nu_ComplexEnd ()
{
	if (use_renderer == R_OLD) return;
	complex_check_abort ();
	complex_st_start = 0;
	if (complex_aborted) {
		/* already forgotten, see complex_check_abort */
		complex_aborted = FALSE;
		do_start_complex = FALSE;
		no_znodes_kthx = FALSE;
		return;
	}
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_END);
	do_start_complex = FALSE;
	no_znodes_kthx = FALSE;
}
void Nu_DrawComplexEnd (void **data)
{
	if (use_renderer == R_GL) {
		/* style 5's plating, from the last contour before it goes */
		int plated = hull_complex_begin (contour_v, contour_n, data);
		flush_contour ();
		gluTessEndContour (tobj);
		gluTessEndPolygon (tobj);
		if (plated) hull_complex_end ();
		
		glMatrixMode (GL_PROJECTION);
		glPopMatrix ();
		glMatrixMode (GL_MODELVIEW);
		glPopMatrix ();
	} else if (use_renderer == R_GLWIRE) {
		glVertex3dv (tess_vertices[0]);
		glEnd ();
	}
	if (sky_active) procfx_sky_end ();
	sky_pending = sky_active = 0;
}

void Nu_ComplexStartInner ()
{
	if (use_renderer == R_OLD) return;
	complex_check_abort ();
	if (complex_aborted) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_INNER);
}
void Nu_DrawComplexStartInner (void **data)
{
	if (use_renderer == R_GL) {
		flush_contour ();
		gluTessEndContour (tobj);
		gluTessBeginContour (tobj);
	} else if (use_renderer == R_GLWIRE) {
		glEnd ();
		glBegin (GL_LINE_STRIP);
		tess_vpos = 0;
	}
}

#define BEZIER_STEPS	10
static void eval_bezier (GLdouble *out, float _t, float ctrlpoints[4][3])
{
	float a,b,c,d,t2;
	t2 = _t*_t;
	c = 1.0f-_t;
	d = t2*_t;
	b = c*c;
	a = b*c;
	b = b*_t*3.0f;
	c = c*3.0f*t2;
	/* x */	
	out[0] =
	    ctrlpoints[0][0] * a +
	    ctrlpoints[1][0] * b +
	    ctrlpoints[2][0] * c +
	    ctrlpoints[3][0] * d;
	/* y */	
	out[1] =
	    ctrlpoints[0][1] * a +
	    ctrlpoints[1][1] * b +
	    ctrlpoints[2][1] * c +
	    ctrlpoints[3][1] * d;
	/* y */	
	out[2] =
	    ctrlpoints[0][2] * a +
	    ctrlpoints[1][2] * b +
	    ctrlpoints[2][2] * c +
	    ctrlpoints[3][2] * d;
}

void Nu_ComplexBezier ()
{
	if (use_renderer == R_OLD) return;
	complex_check_abort ();
	if (complex_aborted) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_BEZIER);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
}
/*
 * Segments for one curve of a complex shape: 10 (as before) for a small
 * curve, then one per 12 pixels of its control polygon on screen, at most
 * BEZIER_STEPS_MAX. The intro's FRONTIER logo ring is two such curves a
 * shape across most of the picture, and at a fixed 10 its ends showed
 * corners 80-200 pixels apart. Only the GL renderer has the projection
 * (tessProjMatrix, kept by Nu_DrawComplexStart: the ortho one is loaded
 * by then); a curve reaching behind the near plane gets the maximum.
 */
#define BEZIER_STEPS_MAX	64
static int bezier_steps_for (GLfloat cp[4][3])
{
	GLdouble w[4][3], len = 0.0;
	int i, n;

	if (use_renderer != R_GL) return BEZIER_STEPS;
	for (i = 0; i < 4; i++) {
		if (cp[i][2] > -(double) GL_NEAR_PLANE) return BEZIER_STEPS_MAX;
		if (!gluProject (cp[i][0], cp[i][1], cp[i][2],
				 tessModelMatrix, tessProjMatrix, tessViewport,
				 &w[i][0], &w[i][1], &w[i][2]))
			return BEZIER_STEPS_MAX;
	}
	for (i = 1; i < 4; i++) {
		double dx = w[i][0] - w[i-1][0], dy = w[i][1] - w[i-1][1];
		len += sqrt (dx*dx + dy*dy);
	}
	n = (int) (len / 12.0);
	if (n <= BEZIER_STEPS) return BEZIER_STEPS;
	if (n >= BEZIER_STEPS_MAX) return BEZIER_STEPS_MAX;
	return n;
}

void Nu_DrawComplexBezier (void **data)
{
	int i, bezier_steps;
	float delta;
	double v[3];
	GLfloat ctrlpoints[4][3];
	
	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdvertexf (data, ctrlpoints[1]);
	znode_rdvertexf (data, ctrlpoints[2]);
	znode_rdvertexf (data, ctrlpoints[3]);
	
	bezier_steps = bezier_steps_for (ctrlpoints);
	
	/* S-6: was an assert. The GL path is bounded in
	 * push_contour_vertex; the wireframe one stores one vertex below */
	if (use_renderer == R_GLWIRE && tess_vpos >= MAX_TESS_VERTICES) return;
	delta = 1.0f/bezier_steps;

	if (use_renderer == R_GLWIRE) {
		tess_vertices[tess_vpos][0] = ctrlpoints[0][0];
		tess_vertices[tess_vpos][1] = ctrlpoints[0][1];
		tess_vertices[tess_vpos++][2] = ctrlpoints[0][2];
		for (i=0; i<=bezier_steps; i++) {
			eval_bezier (v, i*delta, ctrlpoints);
			glVertex3dv (v);
		}
		return;
	}
	for (i=0; i<=bezier_steps; i++) {
		eval_bezier (v, i*delta, ctrlpoints);
		push_contour_vertex (v);
	}
}


/* For engines and industry chimney flares.
 *
 * fe2.s draws this teardrop in 2D (the code after the Nu_PutTeardrop
 * hostcall): a closed Bezier that starts and ends at the projected tail,
 * with both control points a third of the plume's length beyond the
 * projected engine, offset to either side by HALF THE ENGINE'S PROJECTED
 * RADIUS. That radius is in D7 at the hostcall: radius * 256 / engine z,
 * in ST pixels (256 is also the ST's projection constant, which the GL
 * projection matches: 36.5 degrees over the 168-line view).
 *
 * The GL path rebuilds the same teardrop in eye space so it is drawn in
 * perspective. Its width used to be a fraction of the plume's 3D LENGTH,
 * broadened across the eye's Z axis. Seen end-on - your own exhaust in the
 * Rear View, which points straight away from the camera - the plume is
 * tens of thousands of units long but crosses only a few pixels, so the
 * flare ballooned into large cyan blobs that span the screen and spin
 * frame to frame (the cross product with the eye Z axis then depends on
 * the plume's tiny sideways jitter). The width now comes from the game's
 * own radius, and the side vector is perpendicular to both the plume and
 * the line of sight to the engine, so the flare faces the viewer the way
 * the ST's screen-space one does. */
void Nu_PutTeardrop ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TEARDROP);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrcolor (GetReg (REG_D6));
	/* projected engine radius, ST pixels (fe2.s only gets here with > 8) */
	znode_wrlong ((short) (GetReg (REG_D7) & 0xffff));
}
void Nu_DrawTeardrop (void **data)
{
	int i, k;
	float delta;
	GLfloat ctrlpoints[4][3];
	GLfloat eng[3], dir[3], ppd[3], sight[3];
	GLdouble out[3];
	int r, g, b, rad_st;
	float len_dir, len_ppd, half_w;

#define TD_STRETCH	1.3333333333
#define TD_BEZIER_STEPS	40
	
	if (use_renderer == R_OLD) return;
	znode_rdvertexf (data, eng);
	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdcolor (data, &r, &g, &b);
	rad_st = znode_rdlong (data);
	
	/* dir: tail -> engine (ctrlpoints[0] is the pointed tail) */
	dir[0] = eng[0] - ctrlpoints[0][0];
	dir[1] = eng[1] - ctrlpoints[0][1];
	dir[2] = eng[2] - ctrlpoints[0][2];
	len_dir = sqrt (dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
	
	/* Side vector: across the plume and across the line of sight to the
	 * engine (eye at the origin), i.e. the 3D direction that projects
	 * perpendicular to the plume's on-screen axis. Looking straight down
	 * the plume it is degenerate: any vector across the plume will do. */
	get_eye_point (sight);
	sight[0] = eng[0] - sight[0];
	sight[1] = eng[1] - sight[1];
	sight[2] = eng[2] - sight[2];
	cross3 (dir, sight, ppd);
	len_ppd = sqrt (ppd[0]*ppd[0] + ppd[1]*ppd[1] + ppd[2]*ppd[2]);
	if (len_ppd <= 1e-6f * (len_dir * sqrt (sight[0]*sight[0] + sight[1]*sight[1] + sight[2]*sight[2]) + 1.0f)) {
		const float up[3] = { 0.0f, 1.0f, 0.0f }, side[3] = { 1.0f, 0.0f, 0.0f };
		cross3 (dir, (fabsf (dir[1]) < 0.9f * len_dir) ? up : side, ppd);
		len_ppd = sqrt (ppd[0]*ppd[0] + ppd[1]*ppd[1] + ppd[2]*ppd[2]);
	}
	if (len_ppd <= 0.0f) return;
	
	/* Half the projected radius, taken back to eye space at the engine's
	 * depth (the distance along the line of sight; with the usual identity
	 * modelview that is -eng[2], znode_wrvertex having negated z). Capped at the
	 * plume's length so a bad radius cannot fill the screen. */
	half_w = 0.5f * (float) rad_st * fabsf (sight[2]) / 256.0f;
	if (!(half_w > 0.0f) || half_w > len_dir) half_w = len_dir;
	ppd[0] *= half_w / len_ppd;
	ppd[1] *= half_w / len_ppd;
	ppd[2] *= half_w / len_ppd;

	for (i = 0; i < 3; i++) {
		ctrlpoints[1][i] = ctrlpoints[0][i] + TD_STRETCH*dir[i] + ppd[i];
		ctrlpoints[2][i] = ctrlpoints[0][i] + TD_STRETCH*dir[i] - ppd[i];
		ctrlpoints[3][i] = ctrlpoints[0][i];
	}

	delta = 1.0f/TD_BEZIER_STEPS;
	glColor3ub (r, g, b);
	glBegin (GL_TRIANGLE_FAN);
	/* the tessellator prefers it :-) */
	for (i=0; i<=TD_BEZIER_STEPS; i++) {
		eval_bezier (out, i*delta, ctrlpoints);
		glVertex3dv (out);
	}
	glEnd ();
}


void Nu_PutBezierLine ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_BEZIER_LINE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawBezierLine (void **data)
{
	int i, r, g, b;
	GLfloat ctrlpoints[4][3];
	GLfloat delta;
	GLdouble out[3];

	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdvertexf (data, ctrlpoints[1]);
	znode_rdvertexf (data, ctrlpoints[2]);
	znode_rdvertexf (data, ctrlpoints[3]);
	znode_rdcolor (data, &r, &g, &b);
	
	delta = 1.0f/20;
	glLineWidth (px_scale ());	/* as Nu_DrawLine */
	glColor3ub (r, g, b);
	glBegin (GL_LINE_STRIP);
	for (i=0; i<=20; i++) {
		eval_bezier (out, i*delta, ctrlpoints);
		glVertex3dv (out);
	}
	glEnd ();
	glLineWidth (1.0f);
}

/*
 * Hull plating on flat faces (GL renderer only; --flat-hulls turns it off).
 *
 * The 68k lights every polygon flat and hands us its colour, so a ship is
 * a set of flat-coloured faces. Triangles and quads get a plating texture
 * over that colour: it multiplies the colour by 1.0 on average, so a face
 * keeps the colour and brightness the game gave it, and adds only the
 * seams, rivets and panel-to-panel shading of plated metal. Nothing the
 * game reads changes; it is how the host paints a polygon.
 *
 * The texture is pinned to the polygon: its frame starts at the first
 * vertex and runs along the first edge, and one tile is the longest edge
 * scaled down to a power of two (the engine scales objects by powers of
 * two, so a face keeps its plating as it moves). Complex polygons, which
 * carry the decals, logos and text, stay flat and read as paint on top.
 */
#define HULL_TEX_N	512	/* texels per tile side */
#define HULL_ROWS	4	/* rows of plates per tile */
static GLuint hull_tex;
static int hull_on;

static unsigned int hull_hash (unsigned int x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

/* The plating tile, HULL_TEX_N squared bytes of grey (tools/art/hull_tile.py
 * makes them): HULL_TILE, or the file FE2_HULL_TILE names to try another
 * without a rebuild (FE2_HULL_TILE set empty: the built-in plating). Its sum
 * into *sum; 0 if there is no tile. */
#define HULL_TILE "hull/plating.lum"
static int hull_load_tile (float *v, double *sum)
{
	static unsigned char buf[HULL_TEX_N * HULL_TEX_N + 1];
	const char *path = getenv ("FE2_HULL_TILE");
	FILE *f;
	size_t got;
	int i;

	if (!path) path = art_path (HULL_TILE);
	if (!*path || !(f = fopen (path, "rb"))) return 0;
	got = fread (buf, 1, sizeof buf, f);
	fclose (f);
	if (got != HULL_TEX_N * HULL_TEX_N) {
		fprintf (stderr, "hull plating: %s is not a %dx%d tile, using the built-in plating\n",
			 path, HULL_TEX_N, HULL_TEX_N);
		return 0;
	}
	for (i = 0; i < HULL_TEX_N * HULL_TEX_N; i++)
		*sum += v[i] = buf[i] + 1;
	return 1;
}

/* repeat, mipmaps, and anisotropic filtering (up to aniso, 1 for none)
 * where there is any */
static void hull_tex_params_a (int aniso)
{
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
	{
		const char *ext = (const char *) glGetString (GL_EXTENSIONS);
		if (ext && strstr (ext, "GL_EXT_texture_filter_anisotropic")) {
			GLfloat a = 1;
			glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &a);
			glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a < aniso ? a : aniso);
		}
	}
#endif
}

static void hull_tex_params (void)
{
	hull_tex_params_a (8);
}

static void hull_make_texture (void)
{
	static float v[HULL_TEX_N * HULL_TEX_N];
	static unsigned char lum[HULL_TEX_N * HULL_TEX_N];
	const int rh = HULL_TEX_N / HULL_ROWS;
	double sum = 0, scale;
	int x, y, i;

	if (!hull_load_tile (v, &sum))
	for (y = 0; y < HULL_TEX_N; y++) {
		int r = y / rh, ty = y % rh, by = rh - 1 - ty;
		/* plates of this row: widths of 96..192 texels from a fixed
		 * hash, the last one taking the rest; rows start staggered */
		int cut[16], n = 0, off = hull_hash (r + 1) % HULL_TEX_N;
		cut[n++] = 0;
		while (cut[n-1] < HULL_TEX_N - 160 && n < 15)
			cut[n] = cut[n-1] + 96 + 32 * (hull_hash (r * 64 + n) % 4), n++;
		cut[n] = HULL_TEX_N;
		for (x = 0; x < HULL_TEX_N; x++) {
			int dx = (x - off + HULL_TEX_N) % HULL_TEX_N, p = 0, lx, rx;
			double f;
			while (dx >= cut[p+1]) p++;
			lx = dx - cut[p];
			rx = cut[p+1] - 1 - dx;
			/* each plate a little lighter or darker, a slight bow */
			f = 1.0 + 0.05 * ((int) (hull_hash (r * 64 + p + 4096) % 201) - 100) / 100.0;
			f *= 1.0 + 0.03 * (ty - rh / 2) / (double) rh;
			/* seam: dark groove on the top and left edges, a bright
			 * lip below it, a shadow on the bottom and right edges */
			if (lx < 2 || ty < 2) f *= 0.62;
			else if (lx == 2 || ty == 2) f *= 1.12;
			else if (rx == 0 || by == 0) f *= 0.88;
			/* rivets in from the top and left edges */
			else if (((ty >= 7 && ty < 9 && lx >= 8 && rx >= 8 && lx % 16 >= 7 && lx % 16 < 9) ||
				  (lx >= 7 && lx < 9 && ty >= 8 && by >= 8 && ty % 16 >= 7 && ty % 16 < 9)))
				f *= 0.8;
			else if ((ty == 6 && lx >= 8 && rx >= 8 && lx % 16 >= 7 && lx % 16 < 9) ||
				 (lx == 6 && ty >= 8 && by >= 8 && ty % 16 >= 7 && ty % 16 < 9))
				f *= 1.1;
			/* grain */
			f *= 1.0 + 0.02 * ((int) (hull_hash (y * HULL_TEX_N + x + 0x10000) % 201) - 100) / 100.0;
			v[y * HULL_TEX_N + x] = f;
			sum += f;
		}
	}
	/* mean 128: with RGB_SCALE 2 the average texel multiplies by 1.0 */
	scale = 128.0 / (sum / (HULL_TEX_N * HULL_TEX_N));
	for (i = 0; i < HULL_TEX_N * HULL_TEX_N; i++) {
		double l = v[i] * scale + 0.5;
		lum[i] = l < 0 ? 0 : l > 255 ? 255 : (unsigned char) l;
	}
	glGenTextures (1, &hull_tex);
	glBindTexture (GL_TEXTURE_2D, hull_tex);
	hull_tex_params ();
	gluBuild2DMipmaps (GL_TEXTURE_2D, GL_LUMINANCE, HULL_TEX_N, HULL_TEX_N,
			   GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
}

static void hull_forget_shader (void);
static void hull_forget_classes (void);

static void hull_forget_texture (void)
{
	if (hull_tex) glDeleteTextures (1, &hull_tex);
	hull_tex = 0;
	hull_forget_shader ();
	hull_forget_classes ();
	hull_on = 0;
}

/* Texture coordinates of the n (3 or 4) vertices p of a polygon on the
 * plating, in the polygon's own frame. 0 for a degenerate polygon. */
static int hull_coords (double p[][3], int n, float st[][2])
{
	double e1[3], e2[3], nm[3], a[3], b[3], l, lmax = 0, le1, lnm, tile;
	int i, k;

	for (i = 0; i < n; i++) {
		for (l = 0, k = 0; k < 3; k++)
			l += (p[(i+1) % n][k] - p[i][k]) * (p[(i+1) % n][k] - p[i][k]);
		if (l > lmax) lmax = l;
	}
	/* the normal: two edges of a triangle, the diagonals of a quad */
	for (k = 0; k < 3; k++) {
		a[k] = p[n == 4 ? 2 : 1][k] - p[0][k];
		b[k] = p[n == 4 ? 3 : 2][k] - p[n == 4 ? 1 : 0][k];
		e1[k] = p[1][k] - p[0][k];
	}
	nm[0] = a[1]*b[2] - a[2]*b[1];
	nm[1] = a[2]*b[0] - a[0]*b[2];
	nm[2] = a[0]*b[1] - a[1]*b[0];
	e2[0] = nm[1]*e1[2] - nm[2]*e1[1];
	e2[1] = nm[2]*e1[0] - nm[0]*e1[2];
	e2[2] = nm[0]*e1[1] - nm[1]*e1[0];
	le1 = sqrt (e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]);
	lnm = sqrt (nm[0]*nm[0] + nm[1]*nm[1] + nm[2]*nm[2]);
	l = sqrt (e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2]);
	if (!(lmax > 1) || !(lnm > 1e-6 * lmax) || !(le1 > 1e-3 * sqrt (lmax)) || !(l > 0))
		return 0;
	/* a tile of 1.3 to 2.6 per longest edge; the cut is off the powers of
	 * two that model edges tend to be, so no face jumps between sizes */
	tile = ldexp (1.0, (int) floor (log2 (0.77 * sqrt (lmax))));
	for (k = 0; k < 3; k++) {
		e1[k] /= le1 * tile;
		e2[k] /= l * tile;
	}
	for (i = 0; i < n; i++) {
		double s = 0, t = 0;
		for (k = 0; k < 3; k++) {
			s += (p[i][k] - p[0][k]) * e1[k];
			t += (p[i][k] - p[0][k]) * e2[k];
		}
		st[i][0] = s;
		st[i][1] = t;
	}
	return 1;
}

static GLuint hull_bound;	/* the plating bound now, in hull_on == 1 */
static void hull_end (void);
static int hull_log_on (void);

/* Plating tex on, for the polygon drawn next (texture times primary
 * colour, times 2). Stays on through a run of triangles and quads. */
static void hull_fixed_on (GLuint tex)
{
	if (hull_on == 1 && hull_bound == tex) return;
	if (hull_on == 2) hull_end ();
	if (hull_on != 1) {
		glEnable (GL_TEXTURE_2D);
		glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi (GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi (GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
		glTexEnvi (GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi (GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi (GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvf (GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
		hull_on = 1;
	}
	glBindTexture (GL_TEXTURE_2D, tex);
	hull_bound = tex;
}

/* the plating of style 0 */
static void hull_begin (void)
{
	if (!hull_tex) hull_make_texture ();
	hull_fixed_on (hull_tex);
}

/*
 * Style 5's textures, per class of ship (hull.c, hull_classes): made from
 * the class's grey tile the first time the class is drawn, kept until the
 * GL context goes (hull_forget_texture), never per frame. The shader takes
 * two RGBA maps; without the shader one grey plating that carries the
 * tile's paint, grooves and grime is drawn in fixed function.
 */
static struct {
	GLuint t[2];		/* the shader's maps, in hull_bake's layout */
	GLuint lum;		/* the fixed-function plating */
	float grime;		/* the maps' mean grime */
	char failed;		/* out of memory: style 0's plating instead */
} hull_cls[HC_N];

/* The grey tile of a class, n x n and malloc'd, or NULL to bake the seeded
 * plating of the class's profile instead. FE2_HULL_TILE, when it is set,
 * names a raw grey tile (64 to 2048 a side, a power of two) every class
 * takes, or bakes the seeded ones when it is empty. */
static unsigned char *hull_class_tile (int cls, int *n)
{
	static char told[HC_N + 1];
	const struct HullClass *c = &hull_classes[cls];
	const char *env = getenv ("FE2_HULL_TILE");
	unsigned char *rgba, *lum = NULL;
	int w = 0, h = 0, i;

	if (env) {
		FILE *f = *env ? fopen (env, "rb") : NULL;
		long sz = -1;

		if (f && fseek (f, 0, SEEK_END) == 0) sz = ftell (f);
		for (w = 64; w <= 2048 && (long) w * w != sz; w *= 2)
			;
		if (f && w <= 2048 && fseek (f, 0, SEEK_SET) == 0 && (lum = malloc ((size_t) w * w)) &&
		    fread (lum, 1, (size_t) w * w, f) != (size_t) w * w) {
			free (lum);
			lum = NULL;
		}
		if (f) fclose (f);
		if (!lum && *env && !told[HC_N]) {
			fprintf (stderr, "hull: FE2_HULL_TILE %s is not a raw square grey tile, "
				 "baking the seeded plating\n", env);
			told[HC_N] = 1;
		}
		*n = w;
		return lum;
	}
	rgba = art_load_rgba (c->tile, &w, &h);
	if (!rgba || w != h || w < 64 || w > 2048 || (w & (w - 1))) {
		free (rgba);
		if (!told[cls])
			fprintf (stderr, "hull: no square tile at %s for the %s class, "
				 "baking the seeded plating\n", art_path (c->tile), c->name);
		told[cls] = 1;
		return NULL;
	}
	if ((lum = malloc ((size_t) w * w)))
		for (i = 0; i < w * w; i++) lum[i] = rgba[4 * i];
	free (rgba);
	*n = w;
	return lum;
}

/* bake a class's maps, for the shader (two RGBA maps) or not (one grey) */
static void hull_make_class (int cls, int shader)
{
	const struct HullClass *c = &hull_classes[cls];
	int n = 512, i;
	unsigned char *lum = hull_class_tile (cls, &n), *buf;
	int tiled = lum != NULL;
	Uint32 t = SDL_GetTicks ();

	if (!lum) n = 512;
	buf = malloc ((size_t) n * n * 8);
	if (!buf) {
		free (lum);
		hull_cls[cls].failed = 1;
		return;
	}
	if (lum) hull_bake_tile (cls, lum, n, buf, buf + (size_t) n * n * 4, &hull_cls[cls].grime);
	else hull_bake (c->prof, 2, n, buf, buf + (size_t) n * n * 4, &hull_cls[cls].grime);
	free (lum);
#ifdef HULL_SHADERS
	glActiveTexture (GL_TEXTURE0);
#endif
	if (shader) {
		glGenTextures (2, hull_cls[cls].t);
		for (i = 0; i < 2; i++) {
			glBindTexture (GL_TEXTURE_2D, hull_cls[cls].t[i]);
			hull_tex_params ();
			gluBuild2DMipmaps (GL_TEXTURE_2D, GL_RGBA8, n, n, GL_RGBA, GL_UNSIGNED_BYTE,
					   buf + (size_t) i * n * n * 4);
		}
	} else {
		/* paint times grooves, darkened by the grime the way the shader
		 * does it, 128 = the face's own colour */
		const unsigned char *t0 = buf, *t1 = buf + (size_t) n * n * 4;
		double keep = 1.0 / (1.0 - 0.6 * hull_cls[cls].grime);
		for (i = 0; i < n * n; i++) {
			double l = 256.0 * (t0[4*i+2] / 255.0) * (t0[4*i+3] / 255.0) *
				   (1.0 - 0.6 * t1[4*i+2] / 255.0) * keep;
			buf[i] = l > 255 ? 255 : (unsigned char) (l + 0.5);
		}
		glGenTextures (1, &hull_cls[cls].lum);
		glBindTexture (GL_TEXTURE_2D, hull_cls[cls].lum);
		/* no shader to fade it: plain mipmaps take a face seen edge on
		 * to the plating's mean, the face's own colour, instead */
		hull_tex_params_a (1);
		gluBuild2DMipmaps (GL_TEXTURE_2D, GL_LUMINANCE, n, n, GL_LUMINANCE, GL_UNSIGNED_BYTE, buf);
	}
	free (buf);
	hull_bound = 0;	/* the texture bound now is not the one it names */
	if (hull_log_on ())
		fprintf (stderr, "hull: class %s, %s, %dx%d %s made in %u ms\n", c->name,
			 tiled ? "its tile" : "seeded", n, n, shader ? "shader maps" : "plating",
			 (unsigned) (SDL_GetTicks () - t));
}

static void hull_forget_classes (void)
{
	int c;

	for (c = 0; c < HC_N; c++) {
		if (hull_cls[c].t[0]) glDeleteTextures (2, hull_cls[c].t);
		if (hull_cls[c].lum) glDeleteTextures (1, &hull_cls[c].lum);
		memset (&hull_cls[c], 0, sizeof hull_cls[c]);
	}
	hull_bound = 0;
}

/* style 5 without the shader: the class's plating, 0 if there is none */
static int hull_fixed5 (int cls)
{
	if (!hull_cls[cls].lum && !hull_cls[cls].failed) hull_make_class (cls, 0);
	if (!hull_cls[cls].lum) return 0;
	hull_fixed_on (hull_cls[cls].lum);
	return 1;
}

/* Plating off, texture state back to the GL defaults the rest expects */
static void hull_end (void)
{
	if (!hull_on) return;
#ifdef HULL_SHADERS
	if (hull_on == 2) {
		glUseProgram (0);
		hull_on = 0;
		return;
	}
#endif
	glTexEnvf (GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
	glTexEnvi (GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable (GL_TEXTURE_2D);
	hull_on = 0;
}

/* Plating for this polygon: 1 with st filled in, 0 to draw it flat */
static int hull_use (double p[][3], int n, float st[][2])
{
	if (flat_hulls || !hull_coords (p, n, st)) {
		hull_end ();
		return 0;
	}
	hull_begin ();
	return 1;
}

/*
 * --hull-style 1..4: the same flat faces, painted by a fragment shader.
 *
 * Style 0 (above) is the fixed-function plating. The four tiers build on
 * each other and all keep the game's own colour and brightness for a face
 * (the shader multiplies it, never replaces it):
 *	1  a bigger texture (1024, FE2_HULL_TEX=2048), a second, blurred
 *	   scale at an unrelated ratio so the tile never shows, and
 *	   anisotropic filtering;
 *	2  irregular panels, hatches, vents, greebles, hazard stripes and
 *	   edge occlusion that runs along each polygon's own edges;
 *	3  a normal map lit by the game's own light direction (bump and
 *	   specular), grime, chipped paint and scratches;
 *	4  a material profile per model (hull_models in hull.c).
 * All of it is made at run time (hull.c); nothing is read from the game's
 * data. Faces that carry decals (the engine opens a complex shape right
 * after them) stay flat, so signs and logos are as the game drew them.
 *
 * The 68k side only adds a few bytes to each triangle and quad in the
 * stream, when a style is on: the model's key, the face's own seed and the
 * light. The key is found as in hull_model_key. FE2_HULL_LOG=1 prints each
 * model number the first time it is seen, how long each texture took to make,
 * each face of a listed model drawn flat and why, once (a sign or marking, a
 * lamp, a part, a face with no frame: hull_log_face), and the time the 3D
 * view takes to draw, every 300 frames.
 */
struct HullCtx {
	unsigned int key, seed;
	short light[3];
	short wok;		/* style 5: w holds the face in the model's frame (bit 0)
				 * and the face is out of a docking bay's box (bit 1,
				 * hull_bay_outside); the face changes shape against its
				 * model in a way the tile cannot follow, so it is flat
				 * (bit 2, hull_anim_bind); a ship's part that is not
				 * its hull, flat (bit 3) */
	float w[4][3];
};

/* this frame's triangles and quads carry a HullCtx: 1 for styles 0 to 4,
 * 2 for style 5 (with the face in the model's frame) */
static int hull_ctx;
static unsigned int hull_frame_no;	/* the engine's frames, for hull_anim_bind */
static int hull_logging = -1;

static int hull_log_on (void)
{
	if (hull_logging < 0) hull_logging = getenv ("FE2_HULL_LOG") != NULL;
	return hull_logging;
}

/* Latched when the engine starts a frame: the stream of one frame is all
 * with the context or all without, whatever the keyboard does meanwhile.
 * Every style carries the model's key: only a model listed in
 * hull_models[] is plated (see hull_paint). */
static void hull_latch (void)
{
	hull_ctx = !flat_hulls && use_renderer != R_OLD ? (hull_style >= 5 ? 2 : 1) : 0;
	hull_frame_no++;
}

static inline int st_readable (unsigned int addr, unsigned int len);

/*
 * The model of the object being drawn, from the 68k's state: a6 is the
 * object's frame and -212(a6) its record, whose word at +90 is the model's
 * offset into the model table the game data pointer names while it draws
 * (twice the model's number). The intro (its title, its scenes and the
 * menu's backdrop) draws from a table of its own, whose words name other
 * models: its 40 to 52 are whole scenes of the intro, the city among them,
 * where the game's are ships. So the key is the word, the table (hull.h,
 * HULL_TAB_*) and the top bit; 0 when the state is not what that expects
 * (every read is checked: the 68k's memory has no guard).
 */
static unsigned int hull_model_key (void)
{
	unsigned int a6 = GetReg (REG_A6), rec, gd, tab;

	if (a6 > MEM_SIZE || !st_readable (a6 - 212, 4) || !st_readable (__DL973c_game_data, 4))
		return 0;
	rec = STMemory_ReadLong (a6 - 212);
	if (rec > MEM_SIZE || !st_readable (rec + 90, 2)) return 0;
	gd = STMemory_ReadLong (__DL973c_game_data);
	tab = gd == __DL1afae_gamedata1 ? 0 : gd == __DL685d0_intro_gamedata ? HULL_TAB_INTRO : HULL_TAB_OTHER;
	return 0x80000000u | tab | STMemory_ReadWord (rec + 90);
}

static int hull_model_unit (unsigned int key);

#define HULL_SEEN	64
static void hull_log_model (unsigned int key)
{
	static unsigned int seen[HULL_SEEN];
	static int nseen;
	int i;

	for (i = 0; i < nseen; i++)
		if (seen[i] == key) return;
	if (nseen < HULL_SEEN) seen[nseen++] = key;
	const char *tab = key & HULL_TAB_INTRO ? " (intro's table)" : key & HULL_TAB_OTHER ? " (other table)" : "";
	if (hull_key_listed (key)) {
		float sc, off[2];
		fprintf (stderr, "hull: model %u%s -> class %s (style 4 profile %s), unit 2^%d\n",
			 key & 0xffff, tab, hull_classes[hull_class_of_key (key, &sc, off)].name,
			 hull_profiles[hull_profile_of_key (key)].name, hull_model_unit (key));
	} else
		fprintf (stderr, "hull: model %u%s -> not listed in hull_models, drawn flat, unit 2^%d\n",
			 key & 0xffff, tab, hull_model_unit (key));
}

/* FE2_HULL_LOG: a model as the log names it, its number and its table */
static const char *hull_model_str (unsigned int key)
{
	static char buf[4][48];
	static int k;
	char *b = buf[k++ & 3];

	snprintf (b, sizeof buf[0], "%u%s", key & 0xffff, key & HULL_TAB_INTRO ? " (intro's table)" :
		  key & HULL_TAB_OTHER ? " (other table)" : "");
	return b;
}

/* FE2_HULL_LOG: what was decided for one face (or complex shape) of a
 * listed model and why, once per model, face and reason, so the log
 * explains the screen without repeating itself. rgb may be NULL. */
static void hull_log_face (unsigned int key, unsigned int face, const int rgb[3], const char *why)
{
	static struct { unsigned int key, face; const char *why; } seen[2048];
	static int nseen;
	int i;

	for (i = 0; i < nseen; i++)
		if (seen[i].key == key && seen[i].face == face && seen[i].why == why) return;
	if (nseen == 2048) return;
	seen[nseen].key = key;
	seen[nseen].face = face;
	seen[nseen++].why = why;
	if (rgb)
		fprintf (stderr, "hull: model %s face %08x rgb %d,%d,%d: %s\n", hull_model_str (key), face,
			 rgb[0], rgb[1], rgb[2], why);
	else
		fprintf (stderr, "hull: model %s face %08x: %s\n", hull_model_str (key), face, why);
}

/*
 * Style 5: the model's own frame. The 68k keeps the model's turn at
 * -36(a6) (nine words, 1.0 = 32768, a row per model axis) and its origin
 * at -16(a6), both in view units; a vertex is in view units at +4 of its
 * record. The view units are the model's own scaled by a power of two: the
 * word at -208(a6) against the one at +10 of the model's header. So a
 * vertex v (view units, as the 68k keeps it) is at M (v - o) in the
 * model's own units, whatever its distance.
 *
 * A part of the model (a sub-object: the hangar's walls and doors, a
 * ship's gear) is drawn in a frame of its own, linked under the object's:
 * its turn and origin are its own and its -208 word its own scale, but the
 * view units, and the record at -212, are the object's. So the scale is
 * always the object's (the top frame, the one linked straight from the
 * record), and the turn and origin the part's own: the tile is bound to
 * the part and moves with it (a sliding door, a lift, a turning dish),
 * whether it is square to the object or not. Panels then do not run on
 * from a part across the object's faces around it. Mo and oo, when asked
 * for, are the object's own frame (the top one), which hull_bay_outside
 * measures against. 0 when any of it is not what that expects. Only
 * reads.
 */
static int hull_frame_words (unsigned int f, int V[9], int o[3], int *s)
{
	int j;

	if (f > MEM_SIZE || !st_readable (f - 36, 32) || !st_readable (f - 208, 2)) return 0;
	for (j = 0; j < 9; j++) V[j] = (short) STMemory_ReadWord (f - 36 + 2 * j);
	for (j = 0; j < 3; j++) o[j] = (int) STMemory_ReadLong (f - 16 + 4 * j);
	*s = (short) STMemory_ReadWord (f - 208);
	for (j = 0; j < 3; j++) {
		double r = (double) V[3*j] * V[3*j] + (double) V[3*j+1] * V[3*j+1] +
			   (double) V[3*j+2] * V[3*j+2];
		if (r < 0.81 * 1073741824.0 || r > 1.21 * 1073741824.0) return 0;
	}
	return 1;
}

static int hull_model_frame (unsigned int key, double M[9], int o[3], double Mo[9], int oo[3])
{
	unsigned int a6 = GetReg (REG_A6), gd, mh, rec, top;
	int V[9], Vt[9], ot[3], s, h10, j, depth;

	if (!HULL_KEY_OK (key) || a6 > MEM_SIZE || !st_readable (a6 - 212, 4) ||
	    !st_readable (__DL973c_game_data, 4))
		return 0;
	gd = STMemory_ReadLong (__DL973c_game_data);
	if (!st_readable (gd + (key & 0xffff), 2)) return 0;
	mh = gd + STMemory_ReadWord (gd + (key & 0xffff));
	if (!st_readable (mh + 10, 2)) return 0;
	h10 = (short) STMemory_ReadWord (mh + 10);
	if (!hull_frame_words (a6, V, o, &s)) return 0;
	/* the object's own frame: up the links while they carry its record */
	rec = STMemory_ReadLong (a6 - 212);
	top = a6;
	for (depth = 0; depth < 8; depth++) {
		unsigned int up;
		if (!st_readable (top, 4)) return 0;
		up = STMemory_ReadLong (top);
		if (up == rec) break;
		if (up > MEM_SIZE || up <= top || !st_readable (up - 212, 4) ||
		    STMemory_ReadLong (up - 212) != rec)
			return 0;
		top = up;
	}
	if (depth == 8) return 0;
	if (top != a6) {
		/* a part: the object's frame for its scale (and Mo, oo) */
		if (!hull_frame_words (top, Vt, ot, &s)) return 0;
	} else {
		memcpy (Vt, V, sizeof Vt);
		memcpy (ot, o, sizeof ot);
	}
	if (h10 < -4 || h10 > 24 || s < -8 || s > 24) return 0;
	for (j = 0; j < 9; j++) M[j] = ldexp (V[j] / 32768.0, h10 - s);
	if (Mo) {
		for (j = 0; j < 9; j++) Mo[j] = ldexp (Vt[j] / 32768.0, h10 - s);
		for (j = 0; j < 3; j++) oo[j] = ot[j];
	}
	return 1;
}

/*
 * The model a frame draws: -4(a6) holds the header of the model the 68k is
 * drawing there, the object's own or a part's (a sub-object: a missile on
 * its pylon, the hangar's walls). Its word in the model table, found by
 * its offset and kept (only reads). -1 when the frame draws the object's
 * own model, -2 when the part is from another table, -3 when it cannot be
 * read.
 */
static int hull_part_word (unsigned int key)
{
	static unsigned int kh[32];
	static int kw[32], kn;
	unsigned int a6 = GetReg (REG_A6), gd, hdr, mh;
	int i, w = -2;

	if (!HULL_KEY_OK (key) || a6 > MEM_SIZE || !st_readable (a6 - 4, 4) ||
	    !st_readable (__DL973c_game_data, 4))
		return -3;
	gd = STMemory_ReadLong (__DL973c_game_data);
	hdr = STMemory_ReadLong (a6 - 4);
	if (!st_readable (gd + (key & 0xffff), 2)) return -3;
	mh = gd + STMemory_ReadWord (gd + (key & 0xffff));
	if (hdr == mh) return -1;
	for (i = 0; i < kn && i < 32; i++)
		if (kh[i] == hdr) return kw[i];
	for (i = 0; i < 1024 && st_readable (gd + i, 2); i += 2)
		if (gd + STMemory_ReadWord (gd + i) == hdr) {
			w = i;
			break;
		}
	kh[kn & 31] = hdr;
	kw[kn & 31] = w;
	kn++;
	return w;
}

/* the model's unit (the word at +10 of its header, see hull_model_frame);
 * -99 when it cannot be read */
static int hull_model_unit (unsigned int key)
{
	unsigned int gd, mh;

	if (!HULL_KEY_OK (key) || !st_readable (__DL973c_game_data, 4)) return -99;
	gd = STMemory_ReadLong (__DL973c_game_data);
	if (!st_readable (gd + (key & 0xffff), 2)) return -99;
	mh = gd + STMemory_ReadWord (gd + (key & 0xffff));
	if (!st_readable (mh + 10, 2)) return -99;
	return (short) STMemory_ReadWord (mh + 10);
}

/* Style 5: the face's n vertices (records at va) in the frame of the
 * model or part that draws it (see hull_model_frame): 1, or 3 when the
 * face is out of a docking bay's box (measured in the object's own frame,
 * hull_bay_outside); 0 (and the face is mapped on its own) when that
 * cannot be had. A face that moves against its model (a bay's door) is
 * followed from frame to frame by seed and a6, so that its tile moves
 * with it (hull_anim_bind); 4 is added when it cannot be (drawn flat). */
static int hull_obj_coords (unsigned int key, unsigned int seed, unsigned int a6, int n,
			    const unsigned int *va, float w[][3])
{
	double M[9], Mo[9], q = 0;
	float wo[4][3], sc, off[2];
	int o[3], oo[3], i, j, k, wok, a;

	if (!hull_model_frame (key, M, o, Mo, oo)) return 0;
	for (i = 0; i < n; i++) {
		int v[3];
		if (!st_readable (va[i] + 4, 12)) return 0;
		for (k = 0; k < 3; k++) v[k] = (int) STMemory_ReadLong (va[i] + 4 + 4 * k);
		for (j = 0; j < 3; j++) {
			double m = 0, mo = 0;
			for (k = 0; k < 3; k++) {
				m += M[3*j+k] * ((double) v[k] - o[k]);
				mo += Mo[3*j+k] * ((double) v[k] - oo[k]);
			}
			if (!(fabs (m) < 16777216.0) || !(fabs (mo) < 16777216.0)) return 0;
			w[i][j] = (float) m;
			wo[i][j] = (float) mo;
		}
	}
	wok = hull_bay_outside (key, (const float (*)[3]) wo, n) ? 3 : 1;
	if (!hull_key_listed (key)) return wok;		/* drawn flat anyway */
	/* a ship's part from another model (the missiles on its pylons) is
	 * not its hull: flat, like a missile in flight. The buildings' parts
	 * (the hangar's doors and walls) are theirs. */
	if (!HULL_BUILDING (hull_class_of_key (key, &sc, off)) && (a = hull_part_word (key)) >= 0) {
		if (hull_log_on ())
			hull_log_face (key, 0xff000000u | a, NULL, "a part from another model (a missile on a pylon), not hull, drawn flat");
		return wok | 8;
	}
	for (j = 0; j < 3; j++) {
		double r = sqrt (M[3*j] * M[3*j] + M[3*j+1] * M[3*j+1] + M[3*j+2] * M[3*j+2]);
		if (r > q) q = r;
	}
	a = hull_anim_bind (key, seed, a6, hull_frame_no, n, w, q);
	if (hull_log_on () && (a & (HULL_ANIM_TELL_MOVE | HULL_ANIM_TELL_FLAT)))
		hull_log_face (key, seed, NULL, a & HULL_ANIM_TELL_FLAT ?
			       "changes shape against its model, drawn flat while it does" :
			       "moves against its model, the tile bound to it");
	if (a & HULL_ANIM_FLAT) wok |= 4;
	return wok;
}

/* the light, the way znode_wrlightsource gives it to GL: towards the
 * light in view space */
static void hull_read_light (short lt[3])
{
	unsigned int a6 = GetReg (REG_A6);

	lt[0] = 0; lt[1] = 0; lt[2] = 1;
	if (st_readable (a6 - 198, 6)) {
		lt[0] = -(short) STMemory_ReadWord (a6 - 198);
		lt[1] = -(short) STMemory_ReadWord (a6 - 196);
		lt[2] = (short) STMemory_ReadWord (a6 - 194);
	}
}

/* what the draw side needs to know about the triangle or quad just put:
 * n vertices, in a0 to a(n-1) */
static void hull_put_ctx (int n)
{
	unsigned int a0 = GetReg (REG_A0), a1 = GetReg (REG_A1), a2 = GetReg (REG_A2);
	unsigned int a3 = GetReg (REG_A3), a6 = GetReg (REG_A6);
	unsigned int key = hull_model_key (), seed;
	short lt[3];
	int k;

	hull_read_light (lt);
	/* the face's own number: the ST's polygon is told by which of the
	 * object's vertices it uses, which does not change as it turns
	 * (from the type word's low byte, as styles 1 to 4 always had it) */
	seed = hull_mix ((key & 255) + 0x5bd1e995u);
	seed = hull_mix (seed ^ (a0 - a6));
	seed = hull_mix (seed ^ (a1 - a0));
	seed = hull_mix (seed ^ (a2 - a0));
	if (n == 4) seed = hull_mix (seed ^ (a3 - a0));
	if (HULL_KEY_OK (key) && hull_log_on ()) hull_log_model (key);
	znode_wrlong (key);
	znode_wrlong (seed);
	for (k = 0; k < 3; k++) znode_wrword (lt[k]);
	if (hull_ctx == 2) {
		unsigned int va[4] = { a0, a1, a2, a3 };
		float w[4][3];
		short wok = hull_obj_coords (key, seed, a6, n, va, w);
		znode_wrword (wok);
		if (wok) znode_wrblock (w, n * 3 * sizeof (float));
	}
}

static void hull_rdctx (void **data, struct HullCtx *cx, int n)
{
	int k;

	cx->key = znode_rdlong (data);
	cx->seed = znode_rdlong (data);
	for (k = 0; k < 3; k++) cx->light[k] = znode_rdword (data);
	cx->wok = 0;
	if (hull_ctx == 2) {
		cx->wok = znode_rdword (data);
		if (cx->wok) {
			memcpy (cx->w, *data, n * 3 * sizeof (float));
			(*data) += n * 3 * sizeof (float);
		}
	}
}

/*
 * Style 5 on complex shapes (a face of more than four sides, a decal): put
 * after the shape's start, what the draw side needs to lay the class's
 * tile on it: the model's key, the light and the model's frame (see
 * hull_model_frame), for a model listed in hull_models[]. Nothing when
 * style 5 is not on, so the stream is the game's own then.
 */
static struct {
	unsigned int key;
	short light[3], ok;
	float M[9];
	int o[3];
} hull_cc;
static int hull_cc_on;		/* the shape being drawn carries one */

static void hull_put_cctx (void)
{
	unsigned int key;
	short lt[3], ok;
	double M[9];
	float m[9];
	int o[3], k;

	if (hull_ctx != 2) return;
	key = hull_model_key ();
	hull_read_light (lt);
	ok = HULL_KEY_OK (key) && hull_key_listed (key) && hull_model_frame (key, M, o, NULL, NULL);
	if (HULL_KEY_OK (key) && hull_log_on ()) hull_log_model (key);
	znode_wrlong (key);
	for (k = 0; k < 3; k++) znode_wrword (lt[k]);
	znode_wrword (ok);
	if (ok) {
		for (k = 0; k < 9; k++) m[k] = (float) M[k];
		znode_wrblock (m, sizeof m);
		znode_wrblock (o, sizeof o);
	}
}

static void hull_rdcctx (void **data)
{
	int k;

	hull_cc_on = 0;
	if (hull_ctx != 2) return;
	hull_cc.key = znode_rdlong (data);
	for (k = 0; k < 3; k++) hull_cc.light[k] = znode_rdword (data);
	hull_cc.ok = znode_rdword (data);
	if (hull_cc.ok) {
		memcpy (hull_cc.M, *data, sizeof hull_cc.M);
		(*data) += sizeof hull_cc.M;
		memcpy (hull_cc.o, *data, sizeof hull_cc.o);
		(*data) += sizeof hull_cc.o;
	}
	hull_cc_on = hull_cc.ok;
}

/* FE2_HULL_LOG: the time the 3D view takes to draw, over 300 frames. The
 * glFinish makes it the GPU's time too, which is why it is only on then. */
static void hull_log_frame (Uint32 t0)
{
	static Uint32 total;
	static int frames;

	glFinish ();
	total += SDL_GetTicks () - t0;
	if (++frames == 300) {
		fprintf (stderr, "hull: style %d, 3D view draws in %.3f ms a frame (%d frames)\n",
			 hull_style, total / 300.0, frames);
		total = 0;
		frames = 0;
	}
}

/* does the engine open a complex shape (a sign, a logo) on the polygon
 * just read? Then it is a host of decals. A known ship's get the quiet
 * plating, so a decal does not sit on busy panels; see hull_paint5 for
 * the rest. */
static int hull_decal_host (void **data)
{
	return *(int *) *data == NU_COMPLEX_START;
}

#ifdef HULL_SHADERS

static const char hull_vs[] =
	"#version 120\n"
	"varying vec4 vUV, vEd, vVT, vLT, vCol;\n"
	"void main ()\n"
	"{\n"
	"	gl_Position = ftransform ();\n"
	"	vUV = gl_MultiTexCoord0; vEd = gl_MultiTexCoord1;\n"
	"	vVT = gl_MultiTexCoord2; vLT = gl_MultiTexCoord3;\n"
	"	vCol = gl_Color;\n"
	"}\n";

/* uP: bump, gloss, shine, edge occlusion; uQ: warmth of grime, mean grime;
 * uT: edge occlusion, bump and specular, weathering, hazard colour (0/1) */
static const char hull_fs[] =
	"#version 120\n"
	"uniform sampler2D tA, tB;\n"
	"uniform vec4 uP, uQ, uT;\n"
	"varying vec4 vUV, vEd, vVT, vLT, vCol;\n"
	"void main ()\n"
	"{\n"
	"	vec2 uv = vUV.xy, uv2 = uv * 0.4142 + vUV.zw;\n"
	"	vec4 a = texture2D (tA, uv), b = texture2D (tB, uv);\n"
	"	vec4 a2 = texture2D (tA, uv2, 5.0), b2 = texture2D (tB, uv2, 5.0);\n"
	"	float tone = a.b * a.a * 2.0 * mix (1.0, a2.b * a2.a * 2.0, 0.6);\n"
	"	float ratio = 1.0, sp = 0.0, ao = 1.0;\n"
	"	vec3 L = normalize (vLT.xyz), V = normalize (vVT.xyz);\n"
	"	float lum = dot (vCol.rgb, vec3 (0.3, 0.59, 0.11));\n"
	"	if (uT.y > 0.5) {\n"
	"		vec3 n = normalize (vec3 ((a.xy - 0.5) * 2.0 * uP.x, 1.0));\n"
	"		float d0 = max (L.z, 0.0), d1 = max (dot (n, L), 0.0);\n"
	"		ratio = clamp ((0.35 + 0.65 * d1) / (0.35 + 0.65 * d0), 0.4, 1.8);\n"
	"		sp = pow (max (dot (n, normalize (L + V)), 0.0), uP.z)\n"
	"		   * b.g * uP.y * smoothstep (0.0, 0.3, L.z);\n"
	"	}\n"
	"	if (uT.x > 0.5) {\n"
	"		float d = min (min (vEd.x, vEd.y), min (vEd.z, vEd.w));\n"
	"		ao = 1.0 - uP.w * 0.6 * (1.0 - smoothstep (0.0, 0.045, d));\n"
	"	}\n"
	"	vec3 col = vCol.rgb * tone * ratio * ao;\n"
	"	if (uT.z > 0.5) {\n"
	"		float g = clamp (b.b * (0.5 + 0.5 * b2.b / max (uQ.y, 0.02)), 0.0, 1.0);\n"
	"		col *= (1.0 - 0.6 * g) / (1.0 - 0.6 * uQ.y);\n"
	"		col *= mix (vec3 (1.0), vec3 (1.12, 0.97, 0.78), uQ.x * g);\n"
	"		col = mix (col, vec3 (lum * 1.1 * tone * ratio), 0.75 * b.a);\n"
	"	}\n"
	"	if (uT.w > 0.5)\n"
	"		col = mix (col, vec3 (1.0, 0.72, 0.1) * lum * 1.6 * tone * ratio * ao, 0.6 * b.r);\n"
	"	col += sp * (0.5 + 0.5 * vCol.rgb / max (max (vCol.r, vCol.g), max (vCol.b, 0.05)));\n"
	"	gl_FragColor = vec4 (col, vCol.a);\n"
	"}\n";

/*
 * Style 5's fragment shader. The class's tile is laid on the model, so the
 * light and the eye are in the frame of the model's box face. The maps are
 * hull_bake's; uP: bump, gloss, shine, edge occlusion; uQ: warmth of grime,
 * mean grime, weathering (0 on a face that hosts decals), chipped paint;
 * uG: the class's graze. The second scale (vUV.zw, a blurred lookup)
 * breaks up the repeat. The plating fades to the face's own colour where a
 * texel is smaller than a pixel (fd) and, in a class with a graze, on a
 * face seen edge on (fa: the footprint's long side over its short side,
 * e / det = r + 1/r, from uG to 1.5 uG; the station's 2.3 is r 1.8 to 3,
 * about 56 to 70 degrees off square), where the anisotropic filtering would
 * otherwise smear the plating into stripes like boards: it keeps the
 * seams that run down the slope sharp, averages the ones across it away
 * and pulls the plates' grain out along it. A floor seen from just above
 * it is edge on too and keeps its plating (no graze in its class). Paint:
 * a grey face weathers fully, a coloured one less. Only a lit face gets the
 * bump's shading and the sheen, so the dark side stays the game's own. The
 * game already lit the face (vCol), so the bump only tilts that light a
 * little either way, a colour too bright for the screen keeps its hue
 * instead of going to white (and rolls off from 0.85 of full, so a white
 * face keeps its plating instead of a flat clipped white), and the highlight is the glint of the bumps
 * (what a flat face would not have) with a faint broad sheen, less of both
 * on a face that is bright already.
 */
static const char hull_fs5[] =
	"#version 120\n"
	"uniform sampler2D tA, tB;\n"
	"uniform vec4 uP, uQ;\n"
	"uniform float uG;\n"
	"varying vec4 vUV, vEd, vVT, vLT, vCol;\n"
	"void main ()\n"
	"{\n"
	"	vec4 a = texture2D (tA, vUV.xy), b = texture2D (tB, vUV.xy);\n"
	"	vec4 a2 = texture2D (tA, vUV.zw, 4.0), b2 = texture2D (tB, vUV.zw, 4.0);\n"
	"	vec2 jx = dFdx (vUV.xy), jy = dFdy (vUV.xy);\n"
	"	float e = dot (jx, jx) + dot (jy, jy), det = abs (jx.x * jy.y - jx.y * jy.x);\n"
	"	float fa = uG > 0.0 ? 1.0 - smoothstep (uG, 1.5 * uG, e / max (det, 1e-4 * e + 1e-30)) : 1.0;\n"
	"	float fd = (1.0 - smoothstep (0.015, 0.06, sqrt (max (dot (jx, jx), dot (jy, jy))))) * fa;\n"
	"	float tone = mix (1.0, a.b * a.a * 2.0, fd) * mix (1.0, a2.b * a2.a * 2.0, 0.5 * fa);\n"
	"	vec3 L = normalize (vLT.xyz), V = normalize (vVT.xyz);\n"
	"	float mx = max (max (vCol.r, vCol.g), vCol.b), mn = min (min (vCol.r, vCol.g), vCol.b);\n"
	"	float lum = dot (vCol.rgb, vec3 (0.3, 0.59, 0.11));\n"
	"	float paint = 1.0 - smoothstep (0.2, 0.6, (mx - mn) / max (mx, 0.02));\n"
	"	vec3 n = normalize (vec3 ((a.xy - 0.5) * 2.0 * uP.x * fd, 1.0));\n"
	"	float lit = smoothstep (0.0, 0.2, L.z);\n"
	"	float d0 = max (L.z, 0.0), d1 = max (dot (n, L), 0.0);\n"
	"	float ratio = mix (1.0, clamp ((0.35 + 0.65 * d1) / (0.35 + 0.65 * d0), 0.7, 1.3), lit);\n"
	"	float d = min (min (vEd.x, vEd.y), min (vEd.z, vEd.w));\n"
	"	float ao = 1.0 - uP.w * 0.5 * (1.0 - smoothstep (0.0, 0.03, d))\n"
	"		 * smoothstep (0.5, 2.0, 0.03 / max (fwidth (d), 1e-5));\n"
	"	vec3 col = vCol.rgb * tone * ratio * ao;\n"
	"	float w = uQ.z * mix (0.4, 1.0, paint) * fd;\n"
	"	float g = clamp (b.b * (0.5 + 0.5 * b2.b / max (uQ.y, 0.02)), 0.0, 1.0) * w;\n"
	"	col *= (1.0 - 0.6 * g) / (1.0 - 0.6 * uQ.y * w);\n"
	"	col *= mix (vec3 (1.0), vec3 (1.10, 0.97, 0.82), uQ.x * g);\n"
	"	col = mix (col, vec3 (lum * 1.15 * tone * ratio * ao), 0.75 * b.a * uQ.w * w);\n"
	"	float top = max (max (col.r, col.g), col.b);\n"
	"	if (top > 0.85) col *= (0.85 + 0.15 * (1.0 - exp ((0.85 - top) / 0.15))) / top;\n"
	"	vec3 H = normalize (L + V);\n"
	"	float s0 = pow (max (H.z, 0.0), uP.z);\n"
	"	float sp = (max (pow (max (dot (n, H), 0.0), uP.z) - s0, 0.0) * b.g + 0.15 * s0)\n"
	"		 * uP.y * lit * clamp (lum * 3.0, 0.0, 1.0) * (1.0 - 0.8 * smoothstep (0.5, 0.95, mx));\n"
	"	col += sp * (0.5 + 0.5 * vCol.rgb / max (mx, 0.05));\n"
	"	gl_FragColor = vec4 (col, vCol.a);\n"
	"}\n";

static GLuint hull_prog;
static GLint hull_uloc[3];	/* uP, uQ, uT */
static int hull_prog_failed;
/* one set of textures per profile and level (1: plating, 2: full panels) */
static struct { GLuint t[2]; float grime; } hull_set[HP_N][2];
static int hull_cur;		/* the set bound now, in hull_on == 2 */

static GLuint hull_compile (GLenum type, const char *src)
{
	GLuint sh = glCreateShader (type);
	GLint ok = 0;

	glShaderSource (sh, 1, &src, NULL);
	glCompileShader (sh);
	glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog (sh, sizeof log, NULL, log);
		fprintf (stderr, "GL: hull shader does not compile: %s\n", log);
		glDeleteShader (sh);
		return 0;
	}
	return sh;
}

/* hull_vs with a fragment shader, tA on unit 0 and tB on unit 1; 0 if it
 * does not compile or link */
static GLuint hull_link (const char *fs_src)
{
	GLuint vs, fs, prog = 0;
	GLint ok = 0;

	vs = hull_compile (GL_VERTEX_SHADER, hull_vs);
	fs = vs ? hull_compile (GL_FRAGMENT_SHADER, fs_src) : 0;
	if (fs) {
		prog = glCreateProgram ();
		glAttachShader (prog, vs);
		glAttachShader (prog, fs);
		glLinkProgram (prog);
		glGetProgramiv (prog, GL_LINK_STATUS, &ok);
		if (!ok) {
			glDeleteProgram (prog);
			prog = 0;
		}
	}
	if (vs) glDeleteShader (vs);
	if (fs) glDeleteShader (fs);
	if (prog) {
		glUseProgram (prog);
		glUniform1i (glGetUniformLocation (prog, "tA"), 0);
		glUniform1i (glGetUniformLocation (prog, "tB"), 1);
		glUseProgram (0);
	}
	return prog;
}

/* the shader program, 0 (and the style falls back to 0) if it is not there */
static GLuint hull_program (void)
{
	if (hull_prog || hull_prog_failed) return hull_prog;
	hull_prog = hull_link (hull_fs);
	if (!hull_prog) {
		fprintf (stderr, "GL: --hull-style %d needs GLSL 1.20, using the plain plating\n", hull_style);
		hull_prog_failed = 1;
		return 0;
	}
	hull_uloc[0] = glGetUniformLocation (hull_prog, "uP");
	hull_uloc[1] = glGetUniformLocation (hull_prog, "uQ");
	hull_uloc[2] = glGetUniformLocation (hull_prog, "uT");
	return hull_prog;
}

static GLuint hull_prog5;
static GLint hull_uloc5[3];	/* uP, uQ, uG */
static int hull_prog5_failed;

/* style 5's shader, 0 if it is not there (or FE2_HULL_FIXED is set): then
 * the class's plating is drawn in fixed function */
static GLuint hull_program5 (void)
{
	if (hull_prog5 || hull_prog5_failed) return hull_prog5;
	/* FE2_HULL_DBG=1: style 5 paints each face with its graze instead
	 * (red: the fade fa, 1 plated and 0 flat; green: the class's graze / 3;
	 * blue: the footprint's ratio e / det / 8), to see which class and fade
	 * a face gets */
	if (getenv ("FE2_HULL_DBG")) {
		static char dbg[sizeof hull_fs5 + 400];
		char *q;
		strcpy (dbg, hull_fs5);
		q = strstr (dbg, "	gl_FragColor = vec4 (col, vCol.a);");
		if (q) {
			strcpy (q, "	gl_FragColor = vec4 (fa, uG / 3.0, clamp (e / max (det, 1e-4 * e + 1e-30) / 8.0, 0.0, 1.0), vCol.a);\n}\n");
			hull_prog5 = hull_link (dbg);
		}
	} else
	if (!getenv ("FE2_HULL_FIXED")) hull_prog5 = hull_link (hull_fs5);
	if (!hull_prog5) {
		fprintf (stderr, "GL: hull style 5 %s, using the per-class plating without the shader\n",
			 getenv ("FE2_HULL_FIXED") ? "told FE2_HULL_FIXED" : "needs GLSL 1.20");
		hull_prog5_failed = 1;
		return 0;
	}
	hull_uloc5[0] = glGetUniformLocation (hull_prog5, "uP");
	hull_uloc5[1] = glGetUniformLocation (hull_prog5, "uQ");
	hull_uloc5[2] = glGetUniformLocation (hull_prog5, "uG");
	return hull_prog5;
}

static int hull_tex_n (void)
{
	const char *e = getenv ("FE2_HULL_TEX");
	return e && atoi (e) == 2048 ? 2048 : 1024;
}

static void hull_make_set (int prof, int level)
{
	int n = hull_tex_n (), i;
	unsigned char *buf = malloc ((size_t) n * n * 8);
	Uint32 t = SDL_GetTicks ();

	if (!buf) {
		hull_prog_failed = 1;
		return;
	}
	hull_bake (prof, level, n, buf, buf + (size_t) n * n * 4, &hull_set[prof][level-1].grime);
	glActiveTexture (GL_TEXTURE0);
	glGenTextures (2, hull_set[prof][level-1].t);
	for (i = 0; i < 2; i++) {
		glBindTexture (GL_TEXTURE_2D, hull_set[prof][level-1].t[i]);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
		{
			const char *ext = (const char *) glGetString (GL_EXTENSIONS);
			if (ext && strstr (ext, "GL_EXT_texture_filter_anisotropic")) {
				GLfloat a = 1;
				glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &a);
				glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a < 8 ? a : 8);
			}
		}
#endif
		gluBuild2DMipmaps (GL_TEXTURE_2D, GL_RGBA8, n, n, GL_RGBA, GL_UNSIGNED_BYTE,
				   buf + (size_t) i * n * n * 4);
	}
	free (buf);
	if (hull_log_on ())
		fprintf (stderr, "hull: %s level %d textures %dx%d made in %u ms\n",
			 hull_profiles[prof].name, level, n, n, (unsigned) (SDL_GetTicks () - t));
}

static void hull_forget_shader (void)
{
	int p, l;

	for (p = 0; p < HP_N; p++)
		for (l = 0; l < 2; l++) {
			if (hull_set[p][l].t[0]) glDeleteTextures (2, hull_set[p][l].t);
			hull_set[p][l].t[0] = hull_set[p][l].t[1] = 0;
		}
	if (hull_prog) glDeleteProgram (hull_prog);
	if (hull_prog5) glDeleteProgram (hull_prog5);
	hull_prog = hull_prog5 = 0;
	hull_cur = -1;
}

/* Shader on for this profile and level, the texture sets are made the
 * first time. hull_on is 2 while it is. */
static void hull_begin_shader (int prof, int level, int calm)
{
	int tier = hull_style > 4 ? 4 : hull_style;
	const struct HullProfile *pr = &hull_profiles[prof];

	if (hull_on == 2 && hull_cur == (prof * 2 + level - 1) * 2 + calm) return;
	if (!hull_set[prof][level-1].t[0]) hull_make_set (prof, level);
	if (hull_prog_failed) return;
	if (hull_on == 1) hull_end ();
	glUseProgram (hull_prog);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, hull_set[prof][level-1].t[1]);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, hull_set[prof][level-1].t[0]);
	glUniform4f (hull_uloc[0], pr->bump, pr->gloss, pr->shine, pr->ao);
	glUniform4f (hull_uloc[1], pr->tint, hull_set[prof][level-1].grime, 0, 0);
	if (calm) tier = 1;
	glUniform4f (hull_uloc[2], tier >= 2, tier >= 3, tier >= 3, tier >= 2);
	hull_on = 2;
	hull_cur = (prof * 2 + level - 1) * 2 + calm;
}

/* style 5's shader on for this class, its maps made the first time; calm
 * for a face that hosts decals (its lettering and markings): less bump,
 * half the weathering and no chipped paint. 0 if the maps could not be
 * made. */
static int hull_begin5 (int cls, int calm)
{
	const struct HullClass *c = &hull_classes[cls];
	int cur = 1000 + cls * 2 + calm;

	if (hull_on == 2 && hull_cur == cur) return 1;
	if (!hull_cls[cls].t[0] && !hull_cls[cls].failed) hull_make_class (cls, 1);
	if (!hull_cls[cls].t[0]) return 0;
	if (hull_on == 1) hull_end ();
	glUseProgram (hull_prog5);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, hull_cls[cls].t[1]);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, hull_cls[cls].t[0]);
	glUniform4f (hull_uloc5[0], c->bump * (calm ? 0.7f : 1.0f), c->gloss, c->shine, c->ao);
	glUniform4f (hull_uloc5[1], c->tint, hull_cls[cls].grime, calm ? 0.5f : 1.0f, calm ? 0.0f : 1.0f);
	glUniform1f (hull_uloc5[2], c->graze);
	hull_on = 2;
	hull_cur = cur;
	return 1;
}

/* the shader's per-vertex data: see hull_vs */
static void hull_attr (const struct HullFrame *f, int i)
{
	if (hull_cur >= 1000)
		glMultiTexCoord4f (GL_TEXTURE0, f->uv[i][0], f->uv[i][1], f->mu[i][0], f->mu[i][1]);
	else
		glMultiTexCoord4f (GL_TEXTURE0, f->uv[i][0], f->uv[i][1], f->seed[0], f->seed[1]);
	glMultiTexCoord4f (GL_TEXTURE1, f->ed[i][0], f->ed[i][1], f->ed[i][2], f->ed[i][3]);
	glMultiTexCoord4f (GL_TEXTURE2, f->vt[i][0], f->vt[i][1], f->vt[i][2], f->inr);
	glMultiTexCoord4f (GL_TEXTURE3, f->lt[0], f->lt[1], f->lt[2], 0);
}

/* Shader for this polygon: 1 with hf filled in, 0 to draw it flat */
static int hull_shade (double p[][3], int n, const struct HullCtx *cx, int host, struct HullFrame *hf)
{
	double light[3];
	int prof = hull_style >= 4 ? hull_profile_of_key (cx->key) : HP_GENERIC;
	int pk = hull_profile_of_key (cx->key);
	int calm = host && pk != HP_STATION && pk != HP_GENERIC;

	if (calm) prof = HP_GENERIC;
	light[0] = cx->light[0]; light[1] = cx->light[1]; light[2] = cx->light[2];
	if ((host && !calm) || !hull_program () || !hull_frame (p, n, light, cx->seed, hf)) {
		hull_end ();
		return 0;
	}
	hull_begin_shader (prof, calm || hull_style == 1 ? 1 : 2, calm);
	return hull_prog && !hull_prog_failed;
}
#else
static int hull_shade (double p[][3], int n, const struct HullCtx *cx, int host, struct HullFrame *hf)
{
	return 0;
}
static void hull_forget_shader (void) { }
static void hull_attr (const struct HullFrame *f, int i) { }
#endif


/*
 * The hangar's lights. Its lamps, the windows in its doors and its lit
 * signs were flat colour on flat walls; each is now a light: after the
 * face is drawn, a hot core is added over it (brighter toward its middle,
 * under the lettering, which is drawn after it) and a glow round it in
 * its own plane, bright at its edge and fading out over the wall, added
 * to what is there in painter's order, so what stands in front of it
 * (drawn later) still covers it. hangar_view says the camera is in the
 * hangar (its faces all round the view), for hull_hangar_grade.
 */
static int hull_glow;		/* the face just painted: 1 a lamp, 2 a lit sign */
static int hull_glow_rgb[3];
static int hangar_faces;	/* the hangar's own faces drawn this frame */
static double hangar_dir[2][2];	/* their extent across the view, x/z and y/z */
static int hangar_behind;	/* one of them reaches behind the camera */

static void hangar_note (double p[][3], int n)
{
	int i, k;

	if (!hangar_faces++) {
		hangar_dir[0][0] = hangar_dir[1][0] = 1e9;
		hangar_dir[0][1] = hangar_dir[1][1] = -1e9;
		hangar_behind = 0;
	}
	for (i = 0; i < n; i++) {
		if (!(p[i][2] < -1)) {
			hangar_behind = 1;
			continue;
		}
		for (k = 0; k < 2; k++) {
			double t = p[i][k] / -p[i][2];
			if (t < hangar_dir[k][0]) hangar_dir[k][0] = t;
			if (t > hangar_dir[k][1]) hangar_dir[k][1] = t;
		}
	}
}

/* the hangar fills the view: its faces are behind the camera or spread
 * wider than the view across, not seen through the slot from outside */
static int hangar_view (void)
{
	return hangar_faces >= 8 && (hangar_behind || hangar_dir[0][1] - hangar_dir[0][0] > 1.6);
}

static void glow_ring (double p[][3], const double m[][3], int n, double d0, double d1,
		       const float rgb[3], float a0, float a1, int steps)
{
	int i, j;

	for (j = 0; j < steps; j++) {
		double t0 = (double) j / steps, t1 = (double) (j + 1) / steps;
		/* a smooth falloff: (1 - t)^2 */
		float b0 = a0 + (a1 - a0) * (1 - (1 - t0) * (1 - t0)), b1 = a0 + (a1 - a0) * (1 - (1 - t1) * (1 - t1));
		double r0 = d0 + (d1 - d0) * t0, r1 = d0 + (d1 - d0) * t1;
		glBegin (GL_TRIANGLE_STRIP);
		for (i = 0; i <= n; i++) {
			const double *v = p[i % n], *o = m[i % n];
			glColor3f (rgb[0] * b0, rgb[1] * b0, rgb[2] * b0);
			glVertex3d (v[0] + o[0] * r0, v[1] + o[1] * r0, v[2] + o[2] * r0);
			glColor3f (rgb[0] * b1, rgb[1] * b1, rgb[2] * b1);
			glVertex3d (v[0] + o[0] * r1, v[1] + o[1] * r1, v[2] + o[2] * r1);
		}
		glEnd ();
	}
}

static void hull_glow_draw (double p[][3], int n)
{
	double c[3] = { 0, 0, 0 }, N[3] = { 0, 0, 0 }, m[4][3], en[4][3], area, len, s, lim = 1e30;
	float col[3], core[3];
	int kind = hull_glow, i, k;

	hull_glow = 0;
	if (n < 3 || n > 4) return;
	for (i = 0; i < n; i++)
		for (k = 0; k < 3; k++) c[k] += p[i][k] / n;
	/* the plane's normal (Newell), its area */
	for (i = 0; i < n; i++) {
		const double *a = p[i], *b = p[(i + 1) % n];
		N[0] += (a[1] - b[1]) * (a[2] + b[2]);
		N[1] += (a[2] - b[2]) * (a[0] + b[0]);
		N[2] += (a[0] - b[0]) * (a[1] + b[1]);
	}
	len = sqrt (N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
	area = len / 2;
	if (!(len > 0)) return;
	for (k = 0; k < 3; k++) N[k] /= len;
	/* each edge's outward normal in the plane */
	for (i = 0; i < n; i++) {
		const double *a = p[i], *b = p[(i + 1) % n];
		double e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, o[3], mid[3], l;
		o[0] = e[1] * N[2] - e[2] * N[1];
		o[1] = e[2] * N[0] - e[0] * N[2];
		o[2] = e[0] * N[1] - e[1] * N[0];
		l = sqrt (o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
		if (!(l > 0)) return;
		for (k = 0; k < 3; k++) mid[k] = (a[k] + b[k]) / 2 - c[k];
		if (o[0] * mid[0] + o[1] * mid[1] + o[2] * mid[2] < 0) l = -l;
		for (k = 0; k < 3; k++) en[i][k] = o[k] / l;
		l = sqrt (e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
		if (l < lim) lim = l;
	}
	/* each corner's mitre: the two edges' normals' sum over 1 + their dot */
	for (i = 0; i < n; i++) {
		const double *a = en[(i + n - 1) % n], *b = en[i];
		double dt = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		if (dt < -0.3) dt = -0.3;
		for (k = 0; k < 3; k++) m[i][k] = (a[k] + b[k]) / (1 + dt);
	}
	s = sqrt (area);
	for (k = 0; k < 3; k++) {
		col[k] = hull_glow_rgb[k] / 255.0f;
		/* toward white, as a light's middle is */
		core[k] = kind == 1 ? 0.10f * col[k] + 0.22f : 0.16f * col[k] + 0.07f;
	}

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_LIGHTING_BIT);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE);
	glBegin (GL_TRIANGLE_FAN);
	glColor3fv (core);
	glVertex3dv (c);
	glColor3f (0, 0, 0);
	for (i = 0; i <= n; i++) glVertex3dv (p[i % n]);
	glEnd ();
	if (kind == 1) {
		/* a lamp: a tight bright rim and a wide soft spill */
		double d = 0.55 * s;
		if (d > 2.2 * lim) d = 2.2 * lim;
		glow_ring (p, (const double (*)[3]) m, n, 0, 0.18 * d, col, 0.42f, 0.10f, 3);
		glow_ring (p, (const double (*)[3]) m, n, 0.18 * d, d, col, 0.10f, 0, 5);
	} else {
		/* a lit sign: its light on the wall round it */
		double d = 0.22 * s;
		glow_ring (p, (const double (*)[3]) m, n, 0, 0.2 * d, col, 0.30f, 0.09f, 3);
		glow_ring (p, (const double (*)[3]) m, n, 0.2 * d, d, col, 0.09f, 0, 5);
	}
	glPopAttrib ();
}

/* The camera in the hangar: the picture graded as a lit interior, darker
 * toward its corners, so the eye goes to the lit middle and the lamps */
static void hull_hangar_grade (void)
{
	int i, j;
	const int G = 12;

	if (!hangar_view ()) return;
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_LIGHTING_BIT | GL_TRANSFORM_BIT);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, 1, 0, 1, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_DST_COLOR, GL_ZERO);
	for (j = 0; j < G; j++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (i = 0; i <= G; i++) {
			int jj;
			for (jj = j + 1; jj >= j; jj--) {
				/* the view's middle is above the console: 0.58 up */
				double x = (double) i / G, y = (double) jj / G;
				double dx = (x - 0.5) / 0.62, dy = (y - 0.58) / 0.55, r = sqrt (dx * dx + dy * dy), t;
				float g;
				t = (r - 0.45) / 0.75;
				t = t < 0 ? 0 : t > 1 ? 1 : t;
				g = 1.0f - 0.42f * (float) (t * t * (3 - 2 * t));
				glColor3f (g, g, g * 1.02f > 1 ? 1 : g * 1.02f);
				glVertex2d (x, y);
			}
		}
		glEnd ();
	}
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}

/* Style 5 for this polygon: 2 with the shader (hf filled in), 1 with the
 * class's plating in fixed function (st filled in), 0 flat. A lamp, window
 * or lit sign (a bright face of nearly pure colour, one channel all but
 * off, or a near-white face in the hangar, its light strips) keeps its flat
 * colour, while bright paint (an orange or a pink) is plated; a marking
 * on the station, its bay or its hangar (hull_marking) is flat; a face
 * that hosts decals gets calm plating on a known ship, the full plating on
 * a station (its lettering is big, and the weathering keeps a face of
 * many tiles from looking like wallpaper), and is flat in the hangar (its
 * signs). Only a model listed in hull_models[] gets here (hull_paint). */
/* a marking: yellow paint (red and green up, blue all but off), the
 * hazard stripes, the floor markings and the yellow signs of the
 * station, its bay and its hangar. Paint, not hull: drawn flat. By the
 * channels' ratios, so a stripe in shade is still one. */
static int hull_marking (const int rgb[3])
{
	int lo = rgb[0] < rgb[1] ? rgb[0] : rgb[1], hi = rgb[0] > rgb[1] ? rgb[0] : rgb[1];

	return hi >= 64 && rgb[2] < 0.3 * lo && lo >= 0.6 * hi;
}

static int hull_paint5 (double p[][3], int n, const struct HullCtx *cx, int host,
			const int rgb[3], float st[][2], struct HullFrame *hf)
{
	float sc, off[2];
	int cls = hull_class_of_key (cx->key, &sc, off), i;
	int mx = rgb[0] > rgb[1] ? rgb[0] : rgb[1], mn = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
	int calm = host && !HULL_BUILDING (cls);
	int lamp, pure, mark = HULL_BUILDING (cls) && hull_marking (rgb);
	double light[3];

	if (rgb[2] > mx) mx = rgb[2];
	if (rgb[2] < mn) mn = rgb[2];
	/* the docking bay's model also draws the station's outside around the
	 * slot (its corner chamfers, the slot's surround, the docking arms):
	 * those faces are the station's plating, with its graze, not the
	 * hangar's, which a slanting chamfer smears into boards */
	if (cls == HC_HANGAR && (cx->wok & 2)) cls = HC_STATION;
	lamp = cls == HC_HANGAR && mn >= 208;
	pure = mx > 191 && mn < 0.2 * mx;
	/* the hangar's lights: a lamp, a small face of pure colour (a lamp,
	 * a door's window) or a sign of strong colour that carries
	 * lettering, drawn flat and then lit (hull_glow_draw) */
	if (cls == HC_HANGAR && cx->wok) {
		hangar_note (p, n);
		if (hull_obj_axis (cx->w, n) != HULL_DECK_AXIS &&
		    (lamp || (pure && hull_obj_span (cx->w, n) < 0.4 * hull_classes[cls].panel * sc) ||
		     (host && mx >= 160 && mn < 0.6 * mx))) {
			hull_glow = lamp || !host ? 1 : 2;
			for (i = 0; i < 3; i++) hull_glow_rgb[i] = rgb[i];
			hull_end ();
			return 0;
		}
	}
	/* but in the hangar a face of pure colour whose longest side is more
	 * than about 0.4 of a tile (the red doors, which keep that span as
	 * they slide; the lamps and small plaques are under a quarter of one)
	 * is paint, plated calm when it carries decals; not a marking */
	if (pure && !mark && cls == HC_HANGAR && cx->wok &&
	    hull_obj_span (cx->w, n) >= 0.4 * hull_classes[cls].panel * sc) {
		pure = 0;
		calm |= host;
	}
	/* the hangar's floor and ceiling are its deck plating */
	if (cls == HC_HANGAR && cx->wok && hull_obj_axis (cx->w, n) == HULL_DECK_AXIS) cls = HC_DECK;
	if (hull_log_on () && (mark || pure || lamp || (host && !calm && cls != HC_STATION)))
		hull_log_face (cx->key, cx->seed, rgb, mark ? "sign/marking (yellow paint), drawn flat" :
			       pure ? "lamp, window or lit sign (pure colour), drawn flat" :
			       lamp ? "lamp or light strip (near white), drawn flat" :
			       "sign/marking (hosts lettering), drawn flat");
	light[0] = cx->light[0]; light[1] = cx->light[1]; light[2] = cx->light[2];
	if (mark || pure || lamp || (host && !calm && cls != HC_STATION)) {
		hull_end ();
		return 0;
	}
	if (!(cx->wok && hull_frame_obj (p, cx->w, n, light, hull_classes[cls].panel * sc, off, hf)) &&
	    !hull_frame (p, n, light, cx->seed, hf)) {
		if (hull_log_on ())
			hull_log_face (cx->key, cx->seed, rgb, "no frame for the tile (a sliver or a face edge-on), drawn flat");
		hull_end ();
		return 0;
	}
#ifdef HULL_SHADERS
	if (hull_program5 () && hull_begin5 (cls, calm)) return 2;
#endif
	if (hull_cls[cls].failed) return hull_use (p, n, st);
	for (i = 0; i < n; i++) {
		st[i][0] = hf->uv[i][0];
		st[i][1] = hf->uv[i][1];
	}
	if (hull_fixed5 (cls)) return 1;
	hull_end ();
	return 0;
}

/* How to paint a polygon: 2 with the shader (hf filled in), 1 with the
 * fixed plating (st filled in), 0 flat */
static int hull_chain;		/* the last polygon hosts decals: see hull_complex_begin */

static int hull_paint (double p[][3], int n, int have_ctx, const struct HullCtx *cx, int host,
		       const int rgb[3], float st[][2], struct HullFrame *hf)
{
	hull_chain = host;
	hull_glow = 0;
	/* the plating belongs to the ships and stations listed in
	 * hull_models[]: anything else (the intro's titles and banners,
	 * text, planets, buildings, missiles, debris, cargo) is drawn flat,
	 * as with --flat-hulls */
	if (flat_hulls || !have_ctx || !hull_key_listed (cx->key)) {
		hull_end ();
		return 0;
	}
	if (hull_style >= 5 && have_ctx == 2) {
		/* a face that changes shape against its model (hull_anim_bind),
		 * or a ship's part that is not its hull (hull_obj_coords) */
		if (cx->wok & 12) {
			hull_end ();
			return 0;
		}
		return hull_paint5 (p, n, cx, host, rgb, st, hf);
	}
	if (hull_style > 0) {
#ifdef HULL_SHADERS
		if (!hull_prog_failed) return hull_shade (p, n, cx, host, hf) ? 2 : 0;
#endif
	}
	return hull_use (p, n, st);
}

/*
 * Style 5 on a complex shape (see hull_put_cctx). The shape's points are
 * projected to the screen before GLU cuts it into triangles (it is drawn
 * under an orthographic projection), so each point the tessellator hands
 * back is taken back to view space, given the tile's coordinates from the
 * shape's plane in the model's frame (hull_plane_obj), and sent with the
 * clip w it had: the plating is then interpolated in perspective like any
 * other face's. A decal, and a shape that hosts decals, gets the calm
 * plating of the face it sits on, so lettering reads as paint on the
 * panels; a lamp's colour, the hangar's own shapes (its light strips and
 * grey signs) and an unlisted model's stay flat.
 */
static struct HullPlane hull_cplane;
static int hull_cmode;		/* 2 shader, 1 fixed function */
static GLdouble (*hull_cdoor)[3];	/* a door's contour, for its edge */
static int hull_cdoor_n;
static double hull_cw[4];	/* clip w from a view space point */

static void CALLBACK hull_tess_vertex (GLvoid *vertex, GLvoid *poly_data)
{
	const GLdouble *d = (const GLdouble *) vertex;
	double e[3], w;
	float uv[2], mu[2], vt[3];

	glColor3ub (complex_col[0], complex_col[1], complex_col[2]);
	if (!gluUnProject (d[0], d[1], d[2], tessModelMatrix, tessProjMatrix, tessViewport,
			   &e[0], &e[1], &e[2])) {
		glVertex3dv (d);
		return;
	}
	w = hull_cw[0] * e[0] + hull_cw[1] * e[1] + hull_cw[2] * e[2] + hull_cw[3];
	if (!(w > 0)) w = 1;
	hull_plane_at (&hull_cplane, e, uv, mu, vt);
#ifdef HULL_SHADERS
	if (hull_cmode == 2) {
		glMultiTexCoord4f (GL_TEXTURE0, uv[0], uv[1], mu[0], mu[1]);
		glMultiTexCoord4f (GL_TEXTURE1, 1000, 1000, 1000, 1000);
		glMultiTexCoord4f (GL_TEXTURE2, vt[0], vt[1], vt[2], 0);
		glMultiTexCoord4f (GL_TEXTURE3, hull_cplane.lt[0], hull_cplane.lt[1], hull_cplane.lt[2], 0);
	} else
#endif
		glTexCoord2f (uv[0], uv[1]);
	glVertex4d (d[0] * w, d[1] * w, d[2] * w, w);
}

/* before the shape's last contour goes to the tessellator: 1 when the
 * shape is plated (hull_complex_end after gluTessEndPolygon) */
static int hull_complex_begin (GLdouble c[][3], int n, void **data)
{
	float sc, off[2], M[9];
	double light[3];
	int cls, calm, mx, mn, k, j, door = 0;
	const int c8[3] = { complex_col[0] & 255, complex_col[1] & 255, complex_col[2] & 255 };	/* signed */

	hull_cmode = 0;
	hull_cdoor = NULL;
	if (hull_ctx != 2) return 0;	/* no model being read: no word after it */
	calm = hull_chain || hull_decal_host (data);
	hull_chain = calm;
	if (!hull_cc_on || hull_style < 5 || flat_hulls || n < 3) return 0;
	cls = hull_class_of_key (hull_cc.key, &sc, off);
	mx = mn = c8[0];
	for (k = 1; k < 3; k++) {
		if (c8[k] > mx) mx = c8[k];
		if (c8[k] < mn) mn = c8[k];
	}
	/* a lamp's colour, a marking (hull_marking) and the hangar's own
	 * shapes, its lamps and grey signs, are kept flat; its dark grey ones
	 * (the alcove doors: its grey lettering is lighter) are its plating;
	 * a coloured one is the station's outside around the slot (the
	 * docking bay's model carries the face it opens in): the station's */
	if ((mx > 191 && mn < 0.2 * mx) || (HULL_BUILDING (cls) && hull_marking (c8)) ||
	    (cls == HC_HANGAR && ((mx - mn < 24 && (mx >= 80 || mx < 48)) || mn >= 208))) {
		if (hull_log_on ())	/* its points and colour: a shape has no face number */
			hull_log_face (hull_cc.key, (unsigned) n << 24 | c8[0] << 16 | c8[1] << 8 | c8[2], c8,
				       mx > 191 && mn < 0.2 * mx ? "complex shape: lamp or lit sign (pure colour), drawn flat" :
				       HULL_BUILDING (cls) && hull_marking (c8) ? "complex shape: sign/marking (yellow paint), drawn flat" :
				       mn >= 208 ? "complex shape: hangar sign/marking or lamp (white), drawn flat" :
				       "complex shape: hangar sign/marking (grey), drawn flat");
		return 0;
	}
	if (cls == HC_HANGAR && mx - mn >= 24) cls = HC_STATION;
	else if (cls == HC_HANGAR) {
		/* an alcove door: solid (calm is for lettering), in finer
		 * panels than the wall's, off its seams, and given an edge
		 * (hull_complex_end), so it reads as a door, not a hole */
		calm = 0;
		door = 1;
		sc *= 0.55f;
		off[0] += 0.37f;
		off[1] += 0.5f;
	}
	for (k = 0; k < 9; k++) M[k] = hull_cc.M[k];
	for (k = 0; k < 3; k++) light[k] = hull_cc.light[k];
	if (!hull_plane_obj ((const double (*)[3]) c, n, M, hull_cc.o, light,
			     hull_classes[cls].panel * sc, off, &hull_cplane))
		return 0;
	/* w = row 4 of projection x modelview */
	for (j = 0; j < 4; j++) {
		hull_cw[j] = 0;
		for (k = 0; k < 4; k++) hull_cw[j] += tessProjMatrix[3 + 4 * k] * tessModelMatrix[k + 4 * j];
	}
#ifdef HULL_SHADERS
	if (cls == HC_STATION) calm = 0;	/* as hull_paint5 */
	if (hull_program5 () && hull_begin5 (cls, calm)) hull_cmode = 2;
#endif
	if (!hull_cmode && !hull_cls[cls].failed && hull_fixed5 (cls)) hull_cmode = 1;
	if (!hull_cmode) {
		hull_end ();
		return 0;
	}
	gluTessCallback (tobj, GLU_TESS_VERTEX_DATA, (_GLUfuncptr) hull_tess_vertex);
	if (door) {
		hull_cdoor = c;
		hull_cdoor_n = n;
	}
	return 1;
}

/* A door's edge, on the screen (the shape is drawn in window
 * coordinates): a bevel lit from the top left, a dark line round it */
static void hull_door_edge (GLdouble c[][3], int n)
{
	double cx = 0, cy = 0, w = tessViewport[3] / 400.0;
	int i, k;

	if (n < 3 || n > 64 || !(w > 0)) return;
	for (i = 0; i < n; i++) {
		cx += c[i][0] / n;
		cy += c[i][1] / n;
	}
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (i = 0; i < n; i++) {
		const double *a = c[i], *b = c[(i + 1) % n];
		double ex = b[0] - a[0], ey = b[1] - a[1], l = sqrt (ex * ex + ey * ey), nx, ny, lit;
		if (!(l > 0.5)) continue;
		/* the inward normal */
		nx = -ey / l;
		ny = ex / l;
		if (nx * (cx - (a[0] + b[0]) / 2) + ny * (cy - (a[1] + b[1]) / 2) < 0) {
			nx = -nx;
			ny = -ny;
		}
		/* outward toward the top left is lit */
		lit = -(nx * -0.6 + ny * 0.8);
		for (k = 0; k < 2; k++) {
			double w0 = k ? 1.2 * w : 0, w1 = k ? 3.4 * w : 1.2 * w;
			if (k) glColor4f (lit > 0 ? 1 : 0, lit > 0 ? 1 : 0, lit > 0 ? 1 : 0, (float) (fabs (lit) * (lit > 0 ? 0.16 : 0.32)));
			else glColor4f (0, 0, 0, 0.55f);
			glBegin (GL_QUADS);
			glVertex3d (a[0] + nx * w0, a[1] + ny * w0, a[2]);
			glVertex3d (b[0] + nx * w0, b[1] + ny * w0, b[2]);
			glVertex3d (b[0] + nx * w1, b[1] + ny * w1, b[2]);
			glVertex3d (a[0] + nx * w1, a[1] + ny * w1, a[2]);
			glEnd ();
		}
	}
	glPopAttrib ();
}

static void hull_complex_end (void)
{
	gluTessCallback (tobj, GLU_TESS_VERTEX_DATA, (_GLUfuncptr) vertexCallback);
	hull_cmode = 0;
	hull_end ();
	if (hull_cdoor) hull_door_edge (hull_cdoor, hull_cdoor_n);
	hull_cdoor = NULL;
}

void Nu_PutTriangle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TRIANGLE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrcolor (GetReg (REG_D6));
	if (hull_ctx) hull_put_ctx (3);
}
void Nu_DrawTriangle (void **data)
{
	float v1[3], v2[3], v3[3];
	int rgb[3];
	struct HullCtx cx = { 0 };
	
	znode_rdvertexf (data, v1);
	znode_rdvertexf (data, v2);
	znode_rdvertexf (data, v3);
	znode_rdcolorv (data, rgb);
	if (hull_ctx) hull_rdctx (data, &cx, 3);
	glColor3ub (rgb[0], rgb[1], rgb[2]);
	if (use_renderer == R_GLWIRE) {
		glBegin (GL_LINE_STRIP);
			glVertex3fv (v1);
			glVertex3fv (v2);
			glVertex3fv (v3);
			glVertex3fv (v1);
		glEnd ();
	} else {
		double p[3][3] = { { v1[0], v1[1], v1[2] }, { v2[0], v2[1], v2[2] },
				   { v3[0], v3[1], v3[2] } };
		float st[3][2];
		struct HullFrame hf;
		int tex = hull_paint (p, 3, hull_ctx, &cx, hull_ctx && hull_decal_host (data), rgb, st, &hf);
		glBegin (GL_TRIANGLES);
			if (tex == 1) glTexCoord2fv (st[0]); else if (tex == 2) hull_attr (&hf, 0);
			glVertex3fv (v1);
			if (tex == 1) glTexCoord2fv (st[1]); else if (tex == 2) hull_attr (&hf, 1);
			glVertex3fv (v2);
			if (tex == 1) glTexCoord2fv (st[2]); else if (tex == 2) hull_attr (&hf, 2);
			glVertex3fv (v3);
		glEnd ();
		if (hull_glow) hull_glow_draw (p, 3);
	}
}

void Nu_PutQuad ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_QUAD);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrcolor (GetReg (REG_D6));
	if (hull_ctx) hull_put_ctx (4);
}
void Nu_DrawQuad (void **data)
{
	int v1[3], v2[3], v3[3], v4[3];
	int r, g, b;
	struct HullCtx cx = { 0 };
	
	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);
	znode_rdvertex (data, v3);
	znode_rdvertex (data, v4);
	znode_rdcolor (data, &r, &g, &b);
	if (hull_ctx) hull_rdctx (data, &cx, 4);
	
	glColor3ub (r, g, b);
	if (use_renderer == R_GLWIRE) {
		glBegin (GL_LINE_STRIP);
			glVertex3iv (v1);
			glVertex3iv (v2);
			glVertex3iv (v3);
			glVertex3iv (v4);
			glVertex3iv (v1);
		glEnd ();
	} else {
		double p[4][3] = { { v1[0], v1[1], v1[2] }, { v2[0], v2[1], v2[2] },
				   { v3[0], v3[1], v3[2] }, { v4[0], v4[1], v4[2] } };
		float st[4][2];
		struct HullFrame hf;
		int rgb[3] = { r & 255, g & 255, b & 255 };	/* znode_rdcolor's are signed */
		int tex = hull_paint (p, 4, hull_ctx, &cx, hull_ctx && hull_decal_host (data), rgb, st, &hf);
		glBegin (GL_TRIANGLE_STRIP);
			if (tex == 1) glTexCoord2fv (st[0]); else if (tex == 2) hull_attr (&hf, 0);
			glVertex3iv (v1);
			if (tex == 1) glTexCoord2fv (st[1]); else if (tex == 2) hull_attr (&hf, 1);
			glVertex3iv (v2);
			if (tex == 1) glTexCoord2fv (st[3]); else if (tex == 2) hull_attr (&hf, 3);
			glVertex3iv (v4);
			if (tex == 1) glTexCoord2fv (st[2]); else if (tex == 2) hull_attr (&hf, 2);
			glVertex3iv (v3);
		glEnd ();
		if (hull_glow) hull_glow_draw (p, 4);
	}
}
/* procfx corona: which star a disc belongs to, after the disc's data */
static void put_star (void)
{
	struct procfx_star s = { 0, 0 };

	if (use_renderer == R_GL) procfx_star_put (&s);
	znode_wrblock (&s, sizeof s);
}

/* under the disc of radius size at v: a square facing the eye, texture
 * coordinates -1..1 across it */
static void draw_corona (void **data, const int v[3], float size)
{
	static const float q[4][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, 1} };
	struct procfx_star s;
	float c[3] = { v[0], v[1], v[2] }, h;
	int i;

	memcpy (&s, *data, sizeof s);
	*data += sizeof s;
	if (!s.model || (h = procfx_corona_begin (&s, c, size > 0 ? size : 0)) <= 0) return;
	billboard_begin (c[0], c[1], c[2]);
	glBegin (GL_QUADS);
	for (i = 0; i < 4; i++) {
		glTexCoord2fv (q[i]);
		glVertex2f (h * q[i][0], h * q[i][1]);
	}
	glEnd ();
	billboard_end ();
	procfx_corona_end ();
}

void Nu_PutTwinklyCircle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TWINKLYCIRCLE);
	znode_wrlong (GetReg (REG_D2));
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D6));
	put_star ();
}
void Nu_DrawTwinklyCircle (void **data)
{
	int v1[3];
	unsigned int dreg2, isize;
	float size;
	int r, g, b;

	dreg2 = znode_rdlong (data);
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);

	isize = (dreg2 << 16) | (dreg2 >> 16);
	//printf ("%x (%x)\n", GetReg (2), isize);
	
	size = -0.002*((short)dreg2)*v1[2];
	draw_corona (data, v1, size);
	
	/* billboard: always face the camera, otherwise this twinkle/star
	 * dot degenerates to an invisible line when viewed edge-on. */
	billboard_begin (v1[0], v1[1], v1[2]);
	
	if (size > 0.0f) gluDisk (qobj, 0.0, size, 32, 1);
	
	size = -0.002*((short) dreg2)*v1[2] - 0.016*v1[2];
	
	//printf ("Size %.2f\n", size);
	if (size > 0.0f) {
		glBegin (GL_LINES);
			glVertex3f (-size, 0.0f, 0.0f);
			glVertex3f (+size, 0.0f, 0.0f);
			glVertex3f (0.0f, -size, 0.0f);
			glVertex3f (0.0f, +size, 0.0f);
		glEnd ();
	}
	billboard_end ();
}

void Nu_Put2DLine ()
{
	if (use_renderer == R_OLD) return;

	if (znode_start == NULL) {
		add_node (&znode_start, 0);
	} else {
		znode_insert (znode_start, 0);
	}
	znode_wrlong (NU_2DLINE);
	znode_wrword (GetReg (REG_D0));
	znode_wrword (GetReg (REG_D1));
	znode_wrword (GetReg (REG_D2));
	znode_wrword (GetReg (REG_D3));
	znode_wrword (GetReg (REG_D4));
	///printf ("%x\n",(GetReg (REG_D4)&0xffff)>>2);
	/* what about color!!!!!! */
}
void Nu_Draw2DLine (void **data)
{
	short x1,y1,x2,y2;
	int col;

	x1 = znode_rdword (data);
	y1 = znode_rdword (data);
	x2 = znode_rdword (data);
	y2 = znode_rdword (data);
	col = MainRGBPalette[(znode_rdword (data)&0xffff)>>2];

	//printf ("%x,%x,%x,%x\n", col, col&0xff, (col>>8)&0xff, (col>>16)&0xff);
	
	push_ortho ();
	set_ctrl_viewport ();
	//glColor3ub (col&0xff, (col>>8)&0xff, (col>>16)&0xff);
	glColor3ub (0,255,0);
	glBegin (GL_LINES);
		glVertex2i (x1, 199-y1);
		glVertex2i (x2, 199-y2);
	glEnd ();
	set_main_viewport ();
	pop_ortho ();
}

/*
 * Planets.
 *
 * fe2.s L3cd9c_ProjectPlanet always ends up in fuck_planet -> hcall
 * Nu_PutPlanet, both when the planet is a distant disc and when we are
 * flying in its atmosphere, and hands us everything the ST renderer uses:
 * position, radius, orientation, lighting vector, the 16 colour table at
 * 0-30(a3) and the planet's surface feature list (continents, seas, ice
 * caps...). planet.c turns the features into maps of the very same XOR'ed
 * colour codes the ST scan converter produces, and we paint them onto the
 * sphere as textures:
 *
 * - seen from afar, the planet is a cube sphere with one texture per face;
 * - close to the surface, a single mesh cannot do (see draw_horizon_cap),
 *   so the ground is the cone of directions in which the surface is
 *   visible, textured with a local high resolution patch centred under
 *   the camera. The patch is rasterized from the same outlines, just
 *   subdivided further, so seas and continents are exactly where they
 *   are when seen from space - only with more coast line detail.
 *
 * Lighting is done the way the ST does it: the day/night terminator and
 * the twilight bands are more XOR codes (psurf_light_code), so every point
 * of the surface shows one of the 16 colours of the table - see the light
 * zones below.
 *
 * The atmosphere layers of the model are drawn too, see
 * draw_planet_atmosphere.
 */

/* ==== PLANET BEGIN ==== (tests/planet build this block on its own) */
struct PlanetPrim {
	/* viewing coordinates of the 68k engine (z forward) */
	double centre[3];
	double R;
	double light[3];
	/* model -> viewing rotation, A[row][model axis] */
	double A[3][3];
	unsigned int feat_addr, seed;
	int radius_word, detail, flags;
	unsigned short table[16];
	/* atmosphere layers, innermost first: scale and colour, and the
	 * half angle of the cone of directions about the planet centre that
	 * the layer covers (planet_atmosphere_band) */
	int natm;
	double atm_scale[8];
	unsigned short atm_col[8];
	double atm_th[8];
	/* the innermost layer's colour, -1 if none: the smooth limb (procfx) */
	int rim;
};

#define PLANET_FACE_RES		512
#define PLANET_PATCH_RES	1024
#define PLANET_FACE_GRID	48
/* planets whose textures are kept: up to 6*4 face textures each, baked
 * lazily for the faces and light zones actually seen. S-9: sized together
 * with PSURF_CACHE in planet.c, which must stay at least this big so that
 * the surfaces of the cached planets are not parsed again and again. */
#define PLANET_TEX_CACHE	6

/* Use the textured sphere down to this distance/radius ratio, the ground
 * cone with a local patch below it (horizon 37 degrees away).
 * Host constant, not ST data (docs/09 §3). */
#define PLANET_CAP_RATIO	1.25
/* Beyond this ratio we are not in the planet's sky any more: sets
 * planet_ground_seen. Host constant, not ST data (docs/09 §3). */
#define PLANET_SKY_RATIO	2.0

static const double planet_faces[6][3][3] = {
	/* c, u, v */
	{ { 1, 0, 0}, { 0, 1, 0}, { 0, 0, 1} },
	{ {-1, 0, 0}, { 0,-1, 0}, { 0, 0, 1} },
	{ { 0, 1, 0}, {-1, 0, 0}, { 0, 0, 1} },
	{ { 0,-1, 0}, { 1, 0, 0}, { 0, 0, 1} },
	{ { 0, 0, 1}, { 0, 1, 0}, {-1, 0, 0} },
	{ { 0, 0,-1}, { 0, 1, 0}, { 1, 0, 0} },
};

/*
 * Light zones.
 *
 * The ST XORs a light code over the feature codes according to the angle
 * to the sun (psurf_light_code): night, the terminator band(s), day. The
 * zone boundaries are small circles of constant cos(sun), i.e. the
 * intersections of the sphere with planes, so rather than baking the
 * lighting into the textures (and baking them again whenever the planet
 * turns) every zone gets its own set of textures, baked once, and is cut
 * out of the geometry exactly (emit_zone_quad).
 */
#define PLANET_MAX_ZONES	4

struct LightZone {
	int code;
	/* cos(sun) range */
	double lo, hi;
};

static int planet_zones (int flags, struct LightZone z[PLANET_MAX_ZONES])
{
	/* every boundary psurf_light_code may use */
	static const double b[] = { -2.0, 0.0, 0.125, 0.25, 2.0 };
	int i, n = 0;

	for (i = 0; i < 4; i++) {
		int code = psurf_light_code (flags, 0.5 * (b[i] + b[i+1]));
		if (n && z[n-1].code == code) {
			z[n-1].hi = b[i+1];
			continue;
		}
		z[n].code = code;
		z[n].lo = b[i];
		z[n].hi = b[i+1];
		n++;
	}
	return n;
}

struct PlanetTex {
	int used;
	unsigned int lru;
	/* key */
	unsigned int feat_addr, seed;
	int radius_word, detail;
	/* S-9: no struct PSurf * here - planet.c may free it at any time;
	 * ask psurf_get by the key above whenever it is needed */
	/* colours all the textures below were baked with */
	unsigned short table[16];

	unsigned char *face_codes[6];
	GLuint face_tex[6][PLANET_MAX_ZONES];
	/* light code each texture holds, -1 if none yet */
	int face_code[6][PLANET_MAX_ZONES];

	unsigned char *patch_codes;
	struct PPatch patch;
	int patch_valid;
	GLuint patch_tex[PLANET_MAX_ZONES];
	int patch_code[PLANET_MAX_ZONES];
};

static struct PlanetTex planet_tex[PLANET_TEX_CACHE];
static unsigned int planet_tex_clock;

/* Set while drawing a frame whenever a planet surface is close enough to
 * be the ground under us (cleared in Nu_DrawScreen). This is the sky
 * state of docs/09; nothing consumes it yet. */
static int planet_ground_seen;

static inline double pdot (const double a[3], const double b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* viewing -> model: A is a rotation, so its inverse is its transpose */
static void to_model (const struct PlanetPrim *p, const double v[3], double m[3])
{
	int i;
	for (i = 0; i < 3; i++)
		m[i] = p->A[0][i]*v[0] + p->A[1][i]*v[1] + p->A[2][i]*v[2];
}

static void to_view (const struct PlanetPrim *p, const double m[3], double v[3])
{
	int i;
	for (i = 0; i < 3; i++)
		v[i] = p->A[i][0]*m[0] + p->A[i][1]*m[1] + p->A[i][2]*m[2];
}

/* Direction of the sun, in viewing coordinates. The ST lights the side of
 * the planet facing away from -198(a6): its terminator circles (L3dcbc)
 * are centred on minus the lighting vector. */
static void planet_sun (const struct PlanetPrim *p, double sun[3])
{
	double len = sqrt (pdot (p->light, p->light));
	int i;

	if (len <= 0.0) {
		sun[0] = 0.0; sun[1] = 0.0; sun[2] = -1.0;
		return;
	}
	for (i = 0; i < 3; i++) sun[i] = -p->light[i] / len;
}

/* Turn a patch of feature codes into an RGB texture, for one light zone. */
static void planet_bake (GLuint tex, const unsigned char *codes, int n, int light_code,
			 const unsigned short table[16])
{
	static unsigned char *rgb;
	static int rgb_size;
	unsigned char pal[16][3];
	unsigned char *o;
	int i;

	for (i = 0; i < 16; i++) {
		int r, g, b, idx = ((i << 2) ^ light_code) >> 2;
		split_rgb444b (table[idx & 15] & 0xfff, &r, &g, &b);
		pal[i][0] = r; pal[i][1] = g; pal[i][2] = b;
	}
	if (rgb_size < n*n*3) {
		/* S-11: keep the old buffer if this fails, and the texture as
		 * it was */
		unsigned char *nrgb = realloc (rgb, n*n*3);
		if (!nrgb) return;
		rgb = nrgb;
		rgb_size = n*n*3;
	}
	o = rgb;
	for (i = 0; i < n*n; i++) {
		const unsigned char *c = pal[(*codes++ >> 2) & 15];
		*o++ = c[0];
		*o++ = c[1];
		*o++ = c[2];
	}

	glBindTexture (GL_TEXTURE_2D, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, n, n, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
}

static void planet_new_texture (GLuint *tex)
{
	glGenTextures (1, tex);
	glBindTexture (GL_TEXTURE_2D, *tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#ifdef GL_GENERATE_MIPMAP
	glTexParameteri (GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
#else
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
#endif
}

/* Drop every planet texture (see change_vidmode). The parsed surfaces,
 * which live in planet.c, stay. */
static void planet_forget_textures (void)
{
	int i, f;

	for (i = 0; i < PLANET_TEX_CACHE; i++) {
		struct PlanetTex *t = &planet_tex[i];
		if (!t->used) continue;
		for (f = 0; f < 6; f++) {
			glDeleteTextures (PLANET_MAX_ZONES, t->face_tex[f]);
			free (t->face_codes[f]);
		}
		glDeleteTextures (PLANET_MAX_ZONES, t->patch_tex);
		free (t->patch_codes);
		memset (t, 0, sizeof (*t));
	}
}

static void planet_face_patch (int f, struct PPatch *pt)
{
	memcpy (pt->c, planet_faces[f][0], sizeof (pt->c));
	memcpy (pt->u, planet_faces[f][1], sizeof (pt->u));
	memcpy (pt->v, planet_faces[f][2], sizeof (pt->v));
	pt->half = 1.0;
	pt->n = PLANET_FACE_RES;
}

static void planet_forget_colours (struct PlanetTex *t)
{
	int f, z;
	for (z = 0; z < PLANET_MAX_ZONES; z++) {
		for (f = 0; f < 6; f++) t->face_code[f][z] = -1;
		t->patch_code[z] = -1;
	}
}

static struct PlanetTex *planet_get_tex (const struct PlanetPrim *p)
{
	struct PlanetTex *t, *victim = NULL;
	int i, z;

	for (i = 0; i < PLANET_TEX_CACHE; i++) {
		t = &planet_tex[i];
		if (t->used && t->feat_addr == p->feat_addr && t->seed == p->seed &&
		    t->radius_word == p->radius_word && t->detail == p->detail) {
			t->lru = ++planet_tex_clock;
			if (memcmp (t->table, p->table, sizeof (t->table))) {
				memcpy (t->table, p->table, sizeof (t->table));
				planet_forget_colours (t);
			}
			return t;
		}
		if (!victim || !t->used || (victim->used && t->lru < victim->lru))
			victim = t;
	}

	t = victim;
	if (!t->used) {
		int ok;
		/* S-11: no planet rather than a crash when memory runs out */
		t->patch_codes = malloc (PLANET_PATCH_RES * PLANET_PATCH_RES);
		ok = t->patch_codes != NULL;
		for (i = 0; i < 6; i++) {
			t->face_codes[i] = malloc (PLANET_FACE_RES * PLANET_FACE_RES);
			if (!t->face_codes[i]) ok = 0;
		}
		if (!ok) {
			for (i = 0; i < 6; i++) free (t->face_codes[i]);
			free (t->patch_codes);
			memset (t, 0, sizeof (*t));
			return NULL;
		}
		for (i = 0; i < 6; i++)
			for (z = 0; z < PLANET_MAX_ZONES; z++)
				planet_new_texture (&t->face_tex[i][z]);
		for (z = 0; z < PLANET_MAX_ZONES; z++)
			planet_new_texture (&t->patch_tex[z]);
		t->used = 1;
	}
	t->lru = ++planet_tex_clock;
	t->feat_addr = p->feat_addr;
	t->seed = p->seed;
	t->radius_word = p->radius_word;
	t->detail = p->detail;
	memcpy (t->table, p->table, sizeof (t->table));
	{
		/* NULL (out of memory) rasterizes as an empty surface */
		struct PSurf *s = psurf_get (p->feat_addr, p->seed, p->radius_word, p->detail);
		for (i = 0; i < 6; i++) {
			struct PPatch pt;
			planet_face_patch (i, &pt);
			psurf_raster (s, &pt, t->face_codes[i]);
		}
	}
	t->patch_valid = 0;
	planet_forget_colours (t);
	return t;
}

/* The texture of cube face f (or of the local patch, f == -1) for light
 * zone slot z, baked on first use. */
static GLuint planet_zone_texture (struct PlanetTex *t, int f, int z, int light_code)
{
	if (f < 0) {
		if (t->patch_code[z] != light_code) {
			planet_bake (t->patch_tex[z], t->patch_codes, t->patch.n, light_code, t->table);
			t->patch_code[z] = light_code;
		}
		return t->patch_tex[z];
	}
	if (t->face_code[f][z] != light_code) {
		planet_bake (t->face_tex[f][z], t->face_codes[f], PLANET_FACE_RES, light_code, t->table);
		t->face_code[f][z] = light_code;
	}
	return t->face_tex[f][z];
}

/* Make sure the local patch covers the whole visible ground: the cap of
 * half angle 'horizon' around model direction 'down' (from the planet
 * centre to the point under the camera). */
static void planet_update_patch (struct PlanetTex *t, const double down[3], double horizon)
{
	double want = tan (horizon * 1.25 + 1e-6);
	int z;

	if (horizon * 1.25 > 1.2) want = tan (1.2);
	if (t->patch_valid) {
		double cover = atan (t->patch.half);
		double moved = acos (fmin (1.0, pdot (down, t->patch.c)));
		/* still on the patch, and not zoomed in so much that it has got
		 * too coarse? */
		if (moved + horizon < 0.98 * cover && t->patch.half < 3.0 * want)
			return;
	}

	{
		double *c = t->patch.c, *u = t->patch.u, *v = t->patch.v, l;
		memcpy (c, down, 3 * sizeof (double));
		/* any orthonormal frame will do */
		if (fabs (c[0]) < 0.6) { u[0] = 0.0; u[1] = c[2]; u[2] = -c[1]; }
		else { u[0] = -c[2]; u[1] = 0.0; u[2] = c[0]; }
		l = sqrt (pdot (u, u));
		u[0] /= l; u[1] /= l; u[2] /= l;
		v[0] = c[1]*u[2] - c[2]*u[1];
		v[1] = c[2]*u[0] - c[0]*u[2];
		v[2] = c[0]*u[1] - c[1]*u[0];
	}
	t->patch.half = want;
	t->patch.n = PLANET_PATCH_RES;
	psurf_raster (psurf_get (t->feat_addr, t->seed, t->radius_word, t->detail),
		      &t->patch, t->patch_codes);
	t->patch_valid = 1;
	for (z = 0; z < PLANET_MAX_ZONES; z++) t->patch_code[z] = -1;
}

/* A vertex of the planet surface: position (68k viewing coordinates),
 * texture coordinates and cos(sun) of the surface there; for the smooth
 * path also the view cosine and the model direction. */
struct PVert {
	double p[3], t[2], c, mu, m[3];
};

/* Keep the part of a polygon with lo <= c (sign 1) or c <= hi (sign -1).
 * c is (p - centre).sun / R, an affine function of the position, so
 * interpolating it linearly along the edges cuts along the exact plane of
 * the zone boundary. */
static int clip_poly (const struct PVert *in, int n, struct PVert *out, double lim, double sign)
{
	int i, k, m = 0;

	for (i = 0; i < n; i++) {
		const struct PVert *a = &in[i], *b = &in[(i+1) % n];
		double da = sign * (a->c - lim), db = sign * (b->c - lim);

		if (da >= 0.0) out[m++] = *a;
		if ((da >= 0.0) != (db >= 0.0)) {
			double f = da / (da - db);
			struct PVert *o = &out[m++];
			for (k = 0; k < 3; k++) o->p[k] = a->p[k] + f * (b->p[k] - a->p[k]);
			o->t[0] = a->t[0] + f * (b->t[0] - a->t[0]);
			o->t[1] = a->t[1] + f * (b->t[1] - a->t[1]);
			o->c = lim;
		}
	}
	return m;
}

/* 68k viewing coordinates -> GL eye coordinates */
static inline void planet_vertex (const double v[3])
{
	glVertex3d (v[0], v[1], -v[2]);
}

/* Draw the part of quad a b c d (in that order) that lies in the zone. */
static void emit_zone_quad (const struct LightZone *z, const struct PVert *a, const struct PVert *b,
			    const struct PVert *c, const struct PVert *d)
{
	struct PVert q[4], t1[8], t2[12];
	const struct PVert *v = q;
	double lo = fmin (fmin (a->c, b->c), fmin (c->c, d->c));
	double hi = fmax (fmax (a->c, b->c), fmax (c->c, d->c));
	int n = 4, i;

	if (hi < z->lo || lo > z->hi) return;
	q[0] = *a; q[1] = *b; q[2] = *c; q[3] = *d;
	if (lo < z->lo) {
		n = clip_poly (v, n, t1, z->lo, 1.0);
		v = t1;
	}
	if (hi > z->hi) {
		n = clip_poly (v, n, t2, z->hi, -1.0);
		v = t2;
	}
	if (n < 3) return;
	glBegin (GL_POLYGON);
	for (i = 0; i < n; i++) {
		glTexCoord2dv (v[i].t);
		planet_vertex (v[i].p);
	}
	glEnd ();
}

/*
 * The smooth path (procfx layer 2, off with --flat-planets): no cuts. All
 * the zones whose soft range meets cos(sun) range [lo, hi] are bound, one
 * texture unit each, and the shader blends them.
 */
static void planet_bind_zones (struct PlanetTex *t, int f, const struct LightZone *zones, int nzones,
			       double lo, double hi, double soft)
{
	GLuint tex[PLANET_MAX_ZONES] = { 0 }, any = 0;
	int z;

	for (z = 0; z < nzones; z++)
		if (hi >= zones[z].lo - soft && lo <= zones[z].hi + soft)
			any = tex[z] = planet_zone_texture (t, f, z, zones[z].code);
	for (z = 0; z < PLANET_MAX_ZONES; z++)
		procfx_planet_bind (z, tex[z] ? tex[z] : any);
}

static void planet_smooth_vertex (const struct PVert *v)
{
	glTexCoord2dv (v->t);
	procfx_planet_attr (v->c, v->mu, v->m);
	planet_vertex (v->p);
}

static double planet_ramp (double a, double b, double x)
{
	if (!(b > a)) return x >= b;
	x = fmin (1.0, fmax (0.0, (x - a) / (b - a)));
	return x * x * (3.0 - 2.0 * x);
}

/* 0 at the ground, 1 out in space: limb darkening and the halo's night
 * side fade in with it, the same on the sphere and the cap, so nothing
 * pops where one takes over from the other. */
static double planet_space (const struct PlanetPrim *p, double d)
{
	return planet_ramp (1.02, 1.5, d / p->R);
}

static int planet_smooth_begin (const struct PlanetPrim *p, double d, const struct LightZone *z,
				int n, double *soft)
{
	float lo[4] = { 9, 9, 9, 9 }, hi[4] = { 9, 9, 9, 9 }, rim[4] = { 0, 0, 0, 0 }, look[3];
	int i, r, g, b;

	/* half the narrowest inner zone, so every zone still shows */
	*soft = 0.1;
	for (i = 0; i < n; i++) {
		lo[i] = z[i].lo;
		hi[i] = z[i].hi;
		if (i && i < n - 1) *soft = fmin (*soft, 0.5 * (z[i].hi - z[i].lo));
	}
	if (p->rim >= 0) {
		split_rgb444b (p->rim, &r, &g, &b);
		rim[0] = r / 255.0f; rim[1] = g / 255.0f; rim[2] = b / 255.0f; rim[3] = 1.0f;
	}
	look[0] = p->flags & 0x80 ? 1.0f : 0.0f;
	look[1] = 0.25 * planet_space (p, d);
	look[2] = 0.08f;
	return procfx_planet_begin (lo, hi, *soft, rim, look, p->seed);
}

/* The whole planet, seen from a distance: a cube sphere. */
static void draw_planet_sphere (struct PlanetTex *t, const struct PlanetPrim *p, const double sun[3],
				const struct LightZone *zones, int nzones, double soft)
{
	static struct PVert v[PLANET_FACE_GRID+1][PLANET_FACE_GRID+1];
	static char front[PLANET_FACE_GRID][PLANET_FACE_GRID];
	int f, i, j, k, z;

	for (f = 0; f < 6; f++) {
		const double (*F)[3] = planet_faces[f];
		static double dir[PLANET_FACE_GRID+1][PLANET_FACE_GRID+1][3];
		int any = 0;

		for (j = 0; j <= PLANET_FACE_GRID; j++) {
			double y = -1.0 + 2.0 * j / PLANET_FACE_GRID;
			for (i = 0; i <= PLANET_FACE_GRID; i++) {
				double x = -1.0 + 2.0 * i / PLANET_FACE_GRID;
				double m[3], l;
				for (k = 0; k < 3; k++) m[k] = F[0][k] + x*F[1][k] + y*F[2][k];
				l = sqrt (pdot (m, m));
				for (k = 0; k < 3; k++) m[k] /= l;
				to_view (p, m, dir[j][i]);
				for (k = 0; k < 3; k++) v[j][i].p[k] = p->centre[k] + p->R * dir[j][i][k];
				v[j][i].t[0] = 0.5 * (x + 1.0);
				v[j][i].t[1] = 0.5 * (y + 1.0);
				v[j][i].c = pdot (dir[j][i], sun);
				v[j][i].mu = -pdot (dir[j][i], v[j][i].p) / sqrt (pdot (v[j][i].p, v[j][i].p));
				memcpy (v[j][i].m, m, sizeof (m));
			}
		}
		/* no depth buffer: drop the far side by hand. A cell faces us
		 * when the camera (origin) is above its tangent plane. */
		for (j = 0; j < PLANET_FACE_GRID; j++) {
			for (i = 0; i < PLANET_FACE_GRID; i++) {
				double n[3];
				for (k = 0; k < 3; k++)
					n[k] = dir[j][i][k] + dir[j][i+1][k] + dir[j+1][i][k] + dir[j+1][i+1][k];
				front[j][i] = pdot (p->centre, n) + p->R * sqrt (pdot (n, n)) < 0.0;
				any |= front[j][i];
			}
		}
		if (!any) continue;

		if (soft > 0.0) {
			double lo = 2.0, hi = -2.0;
			for (j = 0; j < PLANET_FACE_GRID; j++)
				for (i = 0; i < PLANET_FACE_GRID; i++)
					if (front[j][i])
						for (k = 0; k < 4; k++) {
							lo = fmin (lo, v[j + k/2][i + k%2].c);
							hi = fmax (hi, v[j + k/2][i + k%2].c);
						}
			planet_bind_zones (t, f, zones, nzones, lo, hi, soft);
			glBegin (GL_QUADS);
			for (j = 0; j < PLANET_FACE_GRID; j++)
				for (i = 0; i < PLANET_FACE_GRID; i++)
					if (front[j][i]) {
						planet_smooth_vertex (&v[j][i]);
						planet_smooth_vertex (&v[j][i+1]);
						planet_smooth_vertex (&v[j+1][i+1]);
						planet_smooth_vertex (&v[j+1][i]);
					}
			glEnd ();
			continue;
		}
		for (z = 0; z < nzones; z++) {
			int bound = 0;
			for (j = 0; j < PLANET_FACE_GRID; j++) {
				for (i = 0; i < PLANET_FACE_GRID; i++) {
					double lo, hi;
					if (!front[j][i]) continue;
					lo = fmin (fmin (v[j][i].c, v[j][i+1].c), fmin (v[j+1][i].c, v[j+1][i+1].c));
					hi = fmax (fmax (v[j][i].c, v[j][i+1].c), fmax (v[j+1][i].c, v[j+1][i+1].c));
					if (hi < zones[z].lo || lo > zones[z].hi) continue;
					if (!bound) {
						glBindTexture (GL_TEXTURE_2D, planet_zone_texture (t, f, z, zones[z].code));
						bound = 1;
					}
					emit_zone_quad (&zones[z], &v[j][i], &v[j][i+1], &v[j+1][i+1], &v[j+1][i]);
				}
			}
		}
	}
}

/*
 * The ground, when the camera is near the surface.
 *
 * A fixed sphere mesh cannot do that job: at an altitude of a few
 * thousandths of the planet radius the entire visible surface falls
 * inside a single cell of the mesh, whose chord passes underneath the
 * camera. So we tessellate the *cone of directions in which the surface
 * is visible*, rather than a patch of surface positioned in space.
 * Tessellating the visible spherical cap itself, out to the tangent
 * horizon at acos(R/d), collapses to a single point when landed (d == R),
 * and its vertices centre + R*n are a difference of two quantities of
 * magnitude R, pure cancellation once cast to float. The direction cone
 * has neither problem - see theta_max below - and the vertex along each
 * direction is placed at the exact ray/sphere hit, computed in double
 * relative to the camera. Rings are concentrated towards the horizon,
 * where the silhouette needs the resolution.
 *
 * Every vertex knows exactly which point of the surface it shows, which
 * gives its coordinates in the local surface patch texture.
 */
#define HORIZON_CAP_RINGS	64
#define HORIZON_CAP_SLICES	64

/* Closest distance a ground vertex is emitted at: right under a landed
 * ship the surface is closer than the near plane. Only the direction of
 * such a vertex matters. */
#define GROUND_MIN_DIST		16.0

/* Where the atmosphere discs are emitted: only the *direction* of each
 * vertex matters, and there is no depth buffer to care about the
 * distance (see draw_3dview). Any value well inside the frustum will do.
 * Host constant, not ST data (docs/09 §3). */
#define GROUND_DOME_RADIUS	1.0e6

static void draw_horizon_cap (struct PlanetTex *t, const struct PlanetPrim *p, double d,
			      const double sun[3], const struct LightZone *zones, int nzones, double soft)
{
	static struct PVert v[HORIZON_CAP_RINGS+1][HORIZON_CAP_SLICES+1];
	double up[3], e1[3], e2[3];
	double theta_max, dot, len;
	int i, j, k, z;

	/* 'up' points from the planet centre towards the camera (origin) */
	up[0] = -p->centre[0]/d;
	up[1] = -p->centre[1]/d;
	up[2] = -p->centre[2]/d;

	/* any axis not parallel to 'up', made orthonormal to it */
	if (fabs (up[0]) < 0.9) {
		e1[0] = 1.0; e1[1] = 0.0; e1[2] = 0.0;
	} else {
		e1[0] = 0.0; e1[1] = 1.0; e1[2] = 0.0;
	}
	dot = pdot (up, e1);
	for (k = 0; k < 3; k++) e1[k] -= up[k]*dot;
	len = sqrt (pdot (e1, e1));
	for (k = 0; k < 3; k++) e1[k] /= len;

	/* e2 = up x e1 */
	e2[0] = up[1]*e1[2] - up[2]*e1[1];
	e2[1] = up[2]*e1[0] - up[0]*e1[2];
	e2[2] = up[0]*e1[1] - up[1]*e1[0];

	/* Half-angle of the cone of directions that actually hit the surface.
	 *
	 * This is NOT acos(R/d), the angular radius of the visible cap seen
	 * from the planet centre, which collapses to 0 when landed (d == R).
	 * Seen from the camera the very same surface is a cone of half-angle
	 * asin(R/d), which is perfectly behaved: at d == R it is exactly 90
	 * degrees, i.e. the ground fills everything below the true
	 * horizontal, which is what standing on a planet looks like. */
	theta_max = asin (p->R/d > 1.0 ? 1.0 : p->R/d);

	for (i = 0; i <= HORIZON_CAP_RINGS; i++) {
		double f = (double) i / HORIZON_CAP_RINGS;
		/* denser towards theta_max, i.e. towards the horizon line */
		double th = theta_max * (1.0 - (1.0-f)*(1.0-f));
		double ct = cos (th), st = sin (th);
		/* Exact ray/sphere hit distance along the ring. Kept in double:
		 * it is a difference of quantities of magnitude R, which float
		 * cannot resolve for planet-sized radii. */
		double disc = p->R*p->R - d*d*st*st, l;

		if (disc < 0.0) disc = 0.0;
		l = d*ct - sqrt (disc);

		for (j = 0; j <= HORIZON_CAP_SLICES; j++) {
			double a = 2.0*M_PI*(double) j / HORIZON_CAP_SLICES;
			double ca = cos (a), sa = sin (a);
			double dir[3], n[3], m[3], cm;

			for (k = 0; k < 3; k++) {
				/* -up is 'down', towards the planet centre */
				dir[k] = -up[k]*ct + (e1[k]*ca + e2[k]*sa)*st;
				n[k] = (l*dir[k] - p->centre[k]) / p->R;
				v[i][j].p[k] = dir[k] * (l > GROUND_MIN_DIST ? l : GROUND_MIN_DIST);
			}
			/* from the emitted position, so the zone cuts stay affine */
			v[i][j].c = (pdot (v[i][j].p, sun) - pdot (p->centre, sun)) / p->R;
			to_model (p, n, m);
			/* the smooth path: exact, not from the emitted position */
			v[i][j].mu = -pdot (n, dir);
			memcpy (v[i][j].m, m, sizeof (m));
			if (soft > 0.0) v[i][j].c = pdot (n, sun);
			cm = pdot (m, t->patch.c);
			if (cm < 1e-6) cm = 1e-6;
			v[i][j].t[0] = 0.5 + 0.5 * pdot (m, t->patch.u) / (cm * t->patch.half);
			v[i][j].t[1] = 0.5 + 0.5 * pdot (m, t->patch.v) / (cm * t->patch.half);
		}
	}

	/* The cap is an open, convex surface: every screen pixel it covers is
	 * covered exactly once, so face culling is unnecessary here. */
	glDisable (GL_CULL_FACE);

	if (soft > 0.0) {
		double lo = 2.0, hi = -2.0;
		for (i = 0; i <= HORIZON_CAP_RINGS; i++)
			for (j = 0; j <= HORIZON_CAP_SLICES; j++) {
				lo = fmin (lo, v[i][j].c);
				hi = fmax (hi, v[i][j].c);
			}
		planet_bind_zones (t, -1, zones, nzones, lo, hi, soft);
		glBegin (GL_QUADS);
		for (i = HORIZON_CAP_RINGS - 1; i >= 0; i--)
			for (j = 0; j < HORIZON_CAP_SLICES; j++) {
				planet_smooth_vertex (&v[i][j]);
				planet_smooth_vertex (&v[i][j+1]);
				planet_smooth_vertex (&v[i+1][j+1]);
				planet_smooth_vertex (&v[i+1][j]);
			}
		glEnd ();
		return;
	}
	for (z = 0; z < nzones; z++) {
		glBindTexture (GL_TEXTURE_2D, planet_zone_texture (t, -1, z, zones[z].code));
		/* Walk the rings from the horizon inwards, i.e. far to near:
		 * there is no depth buffer (see draw_3dview), so anything that
		 * could overlap must be emitted back to front. */
		for (i = HORIZON_CAP_RINGS - 1; i >= 0; i--)
			for (j = 0; j < HORIZON_CAP_SLICES; j++)
				emit_zone_quad (&zones[z], &v[i][j], &v[i][j+1], &v[i+1][j+1], &v[i+1][j]);
	}
}

/*
 * Atmosphere layers.
 *
 * Between the planet header and its feature list comes the atmosphere:
 *	colour, { scale, colour }*, 0
 * (read by l3d716 / L3d9ec in fe2.s, skipped at l3d2d4). When the planet
 * is near, the ST draws each layer as a ring between the planet outline
 * and the outline scaled by 'scale' / $4000 (L3d8f4), in the layer's
 * colour plus a tint that depends on where the sun is
 * (L3da2e_AtmosphereColNShit, 204(a3)).
 *
 * The scaling is not about the planet's own centre but about a point up
 * to 2*$600 pixels away in its direction (114(a3)), so what matters is
 * that a layer of scale f is a band (f-1) * that distance wide, whatever
 * the size of the planet on screen: a thin rim around a planet seen from
 * orbit, and bands of sky over the horizon when we are down in it. We
 * draw each layer as the disc of directions around the planet centre
 * that reaches that far beyond the planet's limb.
 */
/* S-23: add an atmosphere tint to a colour one 4-bit channel at a time,
 * saturating, instead of letting a carry run from one channel into the
 * next (and out of the colour). A negative tint word darkens each
 * channel by the digits of its magnitude. */
static unsigned short rgb444_add_sat (unsigned short col, unsigned short tint)
{
	int neg = (short) tint < 0;
	int t = neg ? -(short) tint : tint;
	int out = 0, sh;

	for (sh = 0; sh < 12; sh += 4) {
		int c = (col >> sh) & 15, d = (t >> sh) & 15;
		c = neg ? c - d : c + d;
		if (c < 0)
			c = 0;
		if (c > 15)
			c = 15;
		out |= c << sh;
	}
	return out;
}

static void planet_read_atmosphere (struct PlanetPrim *p, int model, int a3, int a6, int detail3)
{
	int ptr = model, tint, d0, i, n = 0;
	unsigned short scale[8], col[8];

	p->natm = 0;
	ptr += 8;
	/* the flag bits are the high byte of the first model word */
	if (p->flags & 0x10) ptr += 2 + 56;
	else if (!(p->flags & 0x40)) ptr += 8;
	if (!STMemory_ReadWord (ptr)) return;
	ptr += 2;
	for (;;) {
		unsigned short sc = STMemory_ReadWord (ptr);
		if (!sc || n == 8) break;
		scale[n] = sc;
		col[n++] = STMemory_ReadWord (ptr + 2);
		ptr += 4;
	}
	/* layout sanity check: the features follow */
	if (ptr + 2 != p->feat_addr) return;

	/* L3da2e: tint from the light vector and the planet position */
	d0 = 0;
	for (i = 0; i < 3; i++)
		d0 += STMemory_ReadWord (a6 - 198 + 2*i) * STMemory_ReadWord (a3 + 122 + 2*i);
	d0 = (short) ((d0 * 2) >> 16);
	d0 >>= 10;
	d0 = -d0 & ~1;
	if (d0 <= -8) d0 = -8;
	if (d0 >= 6) d0 = 6;
	tint = STMemory_ReadWord (a6 - 104 + 8 + d0);

	/* innermost first */
	for (i = 0; i < n; i++) {
		if ((short) scale[i] <= detail3) continue;
		p->atm_scale[p->natm] = scale[i] / 16384.0;
		p->atm_col[p->natm++] = rgb444_add_sat (col[i], tint);
	}
}

/*
 * V-1: where the ST draws the atmosphere, and how far out (docs/09 §3).
 *
 * In ST pixels (focal length 256) on the line from the centre of the
 * screen through the planet's: near and far limb (l3d274, l3d2d4).
 * - near limb at 178 or more: the planet is off the screen (l3cd84);
 * - the view plane cuts the planet, or far + near >= $600: big path with
 *   the far limb taken as $600 (l3d710);
 * - far + near >= $199: big path (l3d716);
 * - otherwise the planet is small on the screen: the ST draws a circle
 *   and no atmosphere at all;
 * - big path: the atmosphere is drawn iff near - 3 > -178 (the $ff4e test
 *   after l3d716), i.e. not when the planet fills the whole screen.
 * How wide each band is was measured on the screen, not derived: the ST
 * scales the planet outline by f = scale/$4000 about 2*far along that
 * line (114(a3)), which on paper puts the band's edge (f-1)*(2*far -
 * near) pixels in, but the bands the software renderer draws are about
 * three times that. Sampled along the line through the planet centre,
 * the ST's band edges sit at near - (f-1)*6*far, far taken as $600 on the
 * l3d710 paths: Lave from Lave Station (l3d716, near 159, far 569, four
 * layers at 138.1 145.2 150.3 153.4 pixels, the formula gives 138.0
 * 145.2 150.3 153.4) and landed on Mars (l3d710: the widths agree, but
 * the ST's bands start about 5 pixels nearer the horizon than these, an
 * offset this does not model).
 * That gives the half angle of the layer's cone of directions about the
 * planet centre. Worked out from the same numbers the ST uses (the
 * position and the radius before planet_fix_radius), in doubles: the
 * ST's 16 bit wrap of near - 3 and far + near at saturation is not
 * copied. No host constants.
 */
#define ST_FOCAL	256.0

/* view angle off the screen centre -> ST pixels, saturating as the ST's
 * words do */
static double st_pixels (double a)
{
	if (a >= M_PI/2) return 32767.0;
	if (a <= -M_PI/2) return -32768.0;
	a = ST_FOCAL * tan (a);
	return a > 32767.0 ? 32767.0 : a < -32768.0 ? -32768.0 : a;
}

static void planet_atmosphere_band (struct PlanetPrim *p)
{
	double x = p->centre[0], y = p->centre[1], z = p->centre[2], r = p->R;
	double d = sqrt (x*x + y*y + z*z), gam, thp, edge, near, far;
	int l, n = p->natm;

	if (!n) return;
	p->natm = 0;
	if (r <= 0.0 || d <= 0.0) return;
	gam = atan2 (sqrt (x*x + y*y), z);
	thp = asin (r < d ? r/d : 1.0);
	edge = st_pixels (gam - thp);
	near = floor (edge);
	if (near >= 178.0) return;
	if (z*z - r*r <= 0.0) {
		far = 1536.0;
	} else {
		far = floor (st_pixels (gam + thp));
		if (far + near >= 1536.0) far = 1536.0;
		else if (far + near < 409.0) return;
	}
	if (near - 3.0 <= -178.0) return;
	for (l = 0; l < n; l++) {
		double w = (p->atm_scale[l] - 1.0) * 6.0 * far;
		p->atm_th[l] = gam - atan ((edge - w) / ST_FOCAL);
	}
	p->natm = n;
}

#define ATMOSPHERE_RINGS	24
#define ATMOSPHERE_SLICES	64

/* The smooth path (procfx layer 2): one soft halo for all the layers.
 * Each visible layer's colour sits in the middle of its band and blends
 * into the next; opaque out to the outermost band, which fades to
 * nothing, unless it wraps the whole sky (the ST's clamp near pi, low
 * down): then it stays opaque overhead like the flat discs, and the fade
 * comes in smoothly as the edge sinks below the zenith.  Seen from space
 * the night side of the halo is dimmer: the sun's cos at the point of the
 * planet nearest each ray.  From inside the atmosphere that point is
 * underfoot for the whole sky, and the ST's own tint already does that,
 * so it fades out there. */
struct Halo {
	const struct PlanetPrim *p;
	double down[3], e1[3], e2[3], sun[3], space, edge, th[9], mid[8];
	float col[8][3];
	int n;
};

static void halo_vertex (const struct Halo *h, double t, double a)
{
	double w[3], q[3], s, ql, f, al;
	float c[3];
	int k, l, m;

	for (k = 0; k < 3; k++) w[k] = h->down[k]*cos (t) + (h->e1[k]*cos (a) + h->e2[k]*sin (a))*sin (t);
	for (l = 0; l < h->n - 1 && t > h->mid[l+1]; l++) ;
	m = l < h->n - 1 ? l + 1 : l;
	f = planet_ramp (h->mid[l], h->mid[m] + 1e-9, t);
	for (k = 0; k < 3; k++) c[k] = h->col[l][k] + f * (h->col[m][k] - h->col[l][k]);
	al = 1.0 - h->edge * planet_ramp (h->th[h->n-1], h->th[h->n], t);
	if (h->p->flags & 0x80) {
		s = fmax (0.0, pdot (w, h->p->centre));
		for (k = 0; k < 3; k++) q[k] = s * w[k] - h->p->centre[k];
		ql = sqrt (pdot (q, q));
		if (ql > 0.0)
			al *= 1.0 - 0.6 * h->space * (1.0 - planet_ramp (-0.3, 0.3, pdot (q, h->sun) / ql));
	}
	glColor4f (c[0], c[1], c[2], (float) al);
	glVertex3d (GROUND_DOME_RADIUS * w[0], GROUND_DOME_RADIUS * w[1], -GROUND_DOME_RADIUS * w[2]);
}

static void draw_planet_halo (const struct PlanetPrim *p, double d, double theta_p, const double down[3],
			      const double e1[3], const double e2[3])
{
	struct Halo h;
	int l, i, j, r, g, b;

	h.p = p;
	memcpy (h.down, down, sizeof (h.down));
	memcpy (h.e1, e1, sizeof (h.e1));
	memcpy (h.e2, e2, sizeof (h.e2));
	planet_sun (p, h.sun);
	h.space = planet_space (p, d);
	h.th[0] = theta_p;
	for (h.n = l = 0; l < p->natm; l++) {
		double th = fmin (p->atm_th[l], M_PI - 0.01);
		if (th <= h.th[h.n]) continue;
		h.th[h.n+1] = th;
		h.mid[h.n] = 0.5 * (h.th[h.n] + h.th[h.n+1]);
		split_rgb444b (p->atm_col[l], &r, &g, &b);
		h.col[h.n][0] = r / 255.0f; h.col[h.n][1] = g / 255.0f; h.col[h.n][2] = b / 255.0f;
		h.n++;
	}
	if (!h.n) return;
	h.edge = 1.0 - planet_ramp (M_PI - 0.5, M_PI - 0.01, h.th[h.n]);
	glPushAttrib (GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	/* the disc behind the planet fills any gap at its outline */
	glBegin (GL_TRIANGLE_FAN);
	halo_vertex (&h, 0.0, 0.0);
	for (j = 0; j <= ATMOSPHERE_SLICES; j++)
		halo_vertex (&h, theta_p, 2.0*M_PI*j / ATMOSPHERE_SLICES);
	glEnd ();
	for (i = 0; i < ATMOSPHERE_RINGS; i++) {
		double t0 = theta_p + (h.th[h.n] - theta_p) * i / ATMOSPHERE_RINGS;
		double t1 = theta_p + (h.th[h.n] - theta_p) * (i + 1) / ATMOSPHERE_RINGS;
		glBegin (GL_QUAD_STRIP);
		for (j = 0; j <= ATMOSPHERE_SLICES; j++) {
			halo_vertex (&h, t0, 2.0*M_PI*j / ATMOSPHERE_SLICES);
			halo_vertex (&h, t1, 2.0*M_PI*j / ATMOSPHERE_SLICES);
		}
		glEnd ();
	}
	glPopAttrib ();
}

static void draw_planet_atmosphere (const struct PlanetPrim *p, double d)
{
	double down[3], e1[3], e2[3], dot, len, theta_p;
	int l, i, j, k;

	if (!p->natm) return;
	theta_p = asin (p->R/d > 1.0 ? 1.0 : p->R/d);
	for (k = 0; k < 3; k++) down[k] = p->centre[k]/d;
	if (fabs (down[0]) < 0.9) { e1[0] = 1.0; e1[1] = 0.0; e1[2] = 0.0; }
	else { e1[0] = 0.0; e1[1] = 1.0; e1[2] = 0.0; }
	dot = pdot (down, e1);
	for (k = 0; k < 3; k++) e1[k] -= down[k]*dot;
	len = sqrt (pdot (e1, e1));
	for (k = 0; k < 3; k++) e1[k] /= len;
	e2[0] = down[1]*e1[2] - down[2]*e1[1];
	e2[1] = down[2]*e1[0] - down[0]*e1[2];
	e2[2] = down[0]*e1[1] - down[1]*e1[0];

	glDisable (GL_TEXTURE_2D);
	glDisable (GL_CULL_FACE);
	if (!flat_planets) {
		draw_planet_halo (p, d, theta_p, down, e1, e2);
		return;
	}
	/* outermost first, so that the inner layers stay visible (the ST
	 * inserts them all at the same depth, and they come out the other way
	 * round) */
	for (l = p->natm - 1; l >= 0; l--) {
		/* how far out: planet_atmosphere_band, from the ST's band */
		double th = p->atm_th[l];
		int r, g, b;

		if (th <= theta_p) continue;
		if (th > M_PI - 0.01) th = M_PI - 0.01;
		split_rgb444b (p->atm_col[l], &r, &g, &b);
		glColor3ub (r, g, b);
		for (i = ATMOSPHERE_RINGS - 1; i >= 0; i--) {
			/* denser towards the rim */
			double f0 = (double) i / ATMOSPHERE_RINGS, f1 = (double) (i+1) / ATMOSPHERE_RINGS;
			double t0 = th * (1.0 - (1.0-f0)*(1.0-f0)), t1 = th * (1.0 - (1.0-f1)*(1.0-f1));
			double c0 = cos (t0), s0 = sin (t0), c1 = cos (t1), s1 = sin (t1);

			glBegin (GL_QUAD_STRIP);
			for (j = 0; j <= ATMOSPHERE_SLICES; j++) {
				double a = 2.0*M_PI*(double) j / ATMOSPHERE_SLICES;
				double ca = cos (a), sa = sin (a), v[3];
				for (k = 0; k < 3; k++) v[k] = GROUND_DOME_RADIUS * (down[k]*c0 + (e1[k]*ca + e2[k]*sa)*s0);
				glVertex3d (v[0], v[1], -v[2]);
				for (k = 0; k < 3; k++) v[k] = GROUND_DOME_RADIUS * (down[k]*c1 + (e1[k]*ca + e2[k]*sa)*s1);
				glVertex3d (v[0], v[1], -v[2]);
			}
			glEnd ();
		}
	}
}

/* De-quantize the radius.
 *
 * planet_rad (fe2.s L3ce38) is rebuilt by shifting it right by d5 and
 * then left by d4, so its low d4 bits are gone: what we get is a multiple of 2^d4 and the
 * true radius lies somewhere in [R, R + 2^d4). Landed on Mars that is a
 * step of 131072, while the whole apparent "altitude" fits inside a single
 * quantization step. So recover the step from the trailing zero bits and
 * pick the value of the interval that is still physically possible: we can
 * never be below the surface, so clamp to d. */
static void planet_fix_radius (struct PlanetPrim *p, double d)
{
	unsigned int q = (unsigned int) p->R;
	double step = 1.0;
	while (q && !(q & 1)) { q >>= 1; step *= 2.0; }
	if (p->R + step > d) p->R = d;
	else p->R += step;
}

/* The atmosphere layers, in their own znode: see Nu_PutPlanet. */
void Nu_DrawAtmosphere (void **data)
{
	struct PlanetPrim p;
	double d;

	memcpy (&p, *data, sizeof (p));
	*data += sizeof (p);
	d = sqrt (pdot (p.centre, p.centre));
	if (p.R <= 0.0 || d <= 0.0) return;
	planet_fix_radius (&p, d);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	draw_planet_atmosphere (&p, d);
}

void Nu_DrawPlanet (void **data)
{
	struct PlanetPrim p;
	struct PlanetTex *t;
	struct LightZone zones[PLANET_MAX_ZONES];
	double d, sun[3], soft = 0.0;
	int nzones, smooth;

	memcpy (&p, *data, sizeof (p));
	*data += sizeof (p);

	d = sqrt (pdot (p.centre, p.centre));
	if (p.R <= 0.0 || d <= 0.0) return;
	planet_fix_radius (&p, d);

	/* We are close enough to a planet that its surface is the ground
	 * under us - that is exactly the condition for the sky backdrop too,
	 * so derive it from here rather than from a game flag. Picked up by
	 * the next frame. */
	if (d < p.R * PLANET_SKY_RATIO) planet_ground_seen = 1;

	t = planet_get_tex (&p);
	if (!t) return;
	planet_sun (&p, sun);
	nzones = planet_zones (p.flags, zones);

	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);

	glEnable (GL_TEXTURE_2D);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor3f (1.0f, 1.0f, 1.0f);
	smooth = planet_smooth_begin (&p, d, zones, nzones, &soft);
	if (!smooth) soft = 0.0;

	if (d < p.R * PLANET_CAP_RATIO) {
		double up[3] = { -p.centre[0]/d, -p.centre[1]/d, -p.centre[2]/d };
		double down[3];
		to_model (&p, up, down);
		planet_update_patch (t, down, acos (p.R/d > 1.0 ? 1.0 : p.R/d));
		draw_horizon_cap (t, &p, d, sun, zones, nzones, soft);
	} else {
		draw_planet_sphere (t, &p, sun, zones, nzones, soft);
	}

	if (smooth) procfx_planet_end ();
	glDisable (GL_TEXTURE_2D);
}
/* ==== PLANET END ==== */

/*
 * P-2: the hcall passes neither the planet model nor its feature list, and
 * the 68000 core is left as it is, so they are found from what it does
 * pass. a5 is just past the feature list, evened (l3d51e). The model is the
 * one, after a planet command (low 5 bits $1f, ProjectPlanet) in the
 * object's code, whose feature list, found the way L3d3f0 finds it, ends
 * there.
 */
#define PLANET_SCAN_BYTES	0x10000
#define PLANET_MAX_ATM		64
#define PLANET_CACHE		16

static inline int st_readable (unsigned int addr, unsigned int len)
{
	return addr <= MEM_SIZE && len <= MEM_SIZE - addr;
}

/* the feature list of the model at model, as L3d3f0 skips to it; 0 if the
 * layout does not hold */
static unsigned int planet_features_at (unsigned int model)
{
	unsigned int ptr = model + 8;
	int flags, i;

	if (!st_readable (model, 8)) return 0;
	flags = STMemory_ReadByte (model) & 0xff;
	if (flags & 0x10) ptr += 58;
	else if (!(flags & 0x40)) ptr += 8;
	if (!st_readable (ptr, 2)) return 0;
	if (STMemory_ReadWord (ptr) == 0) return ptr + 2;
	/* atmosphere layers: scale and colour words, up to a zero scale */
	for (ptr += 6, i = 0; i < PLANET_MAX_ATM; i++, ptr += 4) {
		if (!st_readable (ptr, 2)) return 0;
		if (STMemory_ReadWord (ptr) == 0) return ptr + 2;
	}
	return 0;
}

static int planet_model_fits (unsigned int model, unsigned int end, unsigned int *feat)
{
	unsigned int f, e;

	if (model < 6 || !st_readable (model - 6, 2)) return 0;
	if ((STMemory_ReadWord (model - 6) & 0x1f) != 0x1f) return 0;
	f = planet_features_at (model);
	if (!f || f >= end) return 0;
	e = psurf_features_end (f, end);
	if (!e || ((e + 1) & ~1u) != end) return 0;
	*feat = f;
	return 1;
}

static unsigned int planet_cache[PLANET_CACHE];
static int planet_cache_next;
static unsigned int planet_found, planet_missed;

static void planet_stats (void)
{
	fprintf (stderr, "planet: models found %u, missed %u; z-tree pushes %u, complex shapes %u aborted %u\n",
		 planet_found, planet_missed, ztree_pushes, complex_starts, complex_aborts);
}

/* the planet model, 0 if none fits; *feat gets its feature list */
static unsigned int planet_find_model (unsigned int a6, unsigned int end, unsigned int *feat)
{
	unsigned int start = 0, m;
	int i;

	if (end < 8 || end > MEM_SIZE) return 0;
	for (i = 0; i < PLANET_CACHE; i++)
		if (planet_cache[i] && planet_model_fits (planet_cache[i], end, feat))
			return planet_cache[i];
	/* from the start of the object's code: its header is at -4(a6) */
	if (st_readable (a6 - 4, 4)) {
		unsigned int h = STMemory_ReadLong (a6 - 4);
		if (st_readable (h, 2)) start = h + (short) STMemory_ReadWord (h);
	}
	if (start >= end || end - start > PLANET_SCAN_BYTES)
		start = end > PLANET_SCAN_BYTES ? end - PLANET_SCAN_BYTES : 0;
	for (m = (start + 7) & ~1u; m < end; m += 2) {
		if (planet_model_fits (m, end, feat)) {
			planet_cache[planet_cache_next] = m;
			planet_cache_next = (planet_cache_next + 1) % PLANET_CACHE;
			return m;
		}
	}
	return 0;
}

void Nu_PutPlanet ()
{
	static bool stats_init;
	struct PlanetPrim p;
	int a3 = GetReg (REG_A3), a6 = GetReg (REG_A6);
	int a0 = GetReg (REG_A0), a1 = GetReg (REG_A1);
	unsigned int feat = 0, model;
	int i, r, c;

	if (use_renderer == R_OLD) return;
	if (!stats_init) {
		stats_init = TRUE;
		if (getenv ("FE2_PLANET_STATS")) atexit (planet_stats);
	}
	model = planet_find_model (a6, GetReg (REG_A5), &feat);
	if (model) planet_found++;
	else planet_missed++;

	for (i = 0; i < 3; i++) {
		p.centre[i] = (double) STMemory_ReadLong (a0 + 4 + 4*i);
		/* lighting vector, -198(a6) */
		p.light[i] = (double) STMemory_ReadWord (a1 + 2*i);
	}
	p.R = (double) (unsigned int) GetReg (REG_D0);

	/* the object rotation matrix at -36(a6): the ST computes viewing
	 * x as M0*x + M3*y + M6*z (L3d452_PlanetFeatureLoop) */
	for (c = 0; c < 3; c++)
		for (r = 0; r < 3; r++)
			p.A[r][c] = STMemory_ReadWord (a6 - 36 + 2*(3*c + r)) / 32768.0;

	/* the planet object's random seed (L3d3f0) */
	p.seed = STMemory_ReadLong (STMemory_ReadLong (a6 - 212) + 118);
	p.detail = (short) STMemory_ReadWord (__DL60d4_optdetail2);
	for (i = 0; i < 16; i++)
		p.table[i] = STMemory_ReadWord (a3 + 2*i);
	if (model) {
		p.feat_addr = feat;
		p.radius_word = (STMemory_ReadLong (model - 4) >> 16) & 0xffff;
		/* the flag bits are the high byte of the first model word */
		p.flags = STMemory_ReadByte (model) & 0xff;
		planet_read_atmosphere (&p, model, a3, a6,
			(short) STMemory_ReadWord (__DL60d6_optdetail3));
	} else {
		/* not found: a plain planet, with no features and no sky */
		p.feat_addr = 0;
		p.radius_word = 0x4000;
		p.flags = 0;
		p.natm = 0;
	}
	p.rim = p.natm ? p.atm_col[0] : -1;
	/* V-1: only where the ST draws it */
	planet_atmosphere_band (&p);

	znode_wrlong (NU_PLANET);
	/* S-7: through the checked writer */
	znode_wrblock (&p, sizeof (p));

	/* The ST draws each atmosphere layer as a primitive of its own,
	 * inserted at the very back (the largest depth, in L3d8f4): it is a
	 * backdrop, painted before everything else, other planets and their
	 * rings included. Painted with the planet surface, the sky bands of
	 * the planet we are standing on would hide the planets in its sky. */
	if (p.natm) {
		unsigned char *a = add_detached_node (cur_ztree, 0xffffffff, 4 + sizeof (p));
		if (a) {
			*(int *) a = NU_ATMOSPHERE;
			memcpy (a + 4, &p, sizeof (p));
		}
	}
}

/*
 * Edges for a filled circle of eye-space radius size at depth z: 32 (as
 * before) up to a radius of about 80 pixels, then one edge per 16 pixels
 * of rim, at most 256. Circles are drawn outside complex shapes, so the
 * projection loaded here is the perspective one. The intro's blue star fills the screen
 * (about 800 pixels radius at 1600 wide) and was a visible 32-gon.
 */
static int circle_slices (double size, double z)
{
	GLdouble pm[16];
	double rpx;

	if (z >= 0.0 || size <= 0.0) return 32;
	glGetDoublev (GL_PROJECTION_MATRIX, pm);
	rpx = size / -z * pm[0] * (wide_3d ? v3d_w : pic_w) * 0.5;
	if (rpx * 2.0 * M_PI / 16.0 <= 32.0) return 32;
	if (rpx * 2.0 * M_PI / 16.0 >= 256.0) return 256;
	return (int) (rpx * 2.0 * M_PI / 16.0);
}

void Nu_PutCircle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_CIRCLE);
	znode_wrlong (GetReg (REG_D2));
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D6));
	put_star ();
}
/* The galaxy map's stars and markers: a camera-facing disc whose alpha
 * rises from 0 at r0 to a at r1, holds to r2 and falls to 0 at r3 (a
 * soft glow, or with r0 > 0 a smooth ring).  Blending is already on. */
static void disc_strips (float r0, float r1, float r2, float r3,
			 float r, float g, float b, float a)
{
	const float rad[4] = { r0, r1, r2, r3 }, al[4] = { 0, a, a, 0 };
	int i, k;

	for (k = 0; k < 3; k++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (i = 0; i <= 32; i++) {
			float c = cosf (i * (float) M_PI / 16), s = sinf (i * (float) M_PI / 16);
			glColor4f (r, g, b, al[k]);
			glVertex2f (rad[k] * c, rad[k] * s);
			glColor4f (r, g, b, al[k + 1]);
			glVertex2f (rad[k + 1] * c, rad[k + 1] * s);
		}
		glEnd ();
	}
}

static void soft_disc (const int *v, float r0, float r1, float r2, float r3,
		       float r, float g, float b, float a)
{
	billboard_begin (v[0], v[1], v[2]);
	disc_strips (r0, r1, r2, r3, r, g, b, a);
	billboard_end ();
}

/* a ring with a faint fill: the selected star, and where you are */
static void soft_ring (const int *v, float s, float r, float g, float b)
{
	soft_disc (v, 0, 0, 0.7f * s, 0.8f * s, r, g, b, 0.12f);
	soft_disc (v, 0.68f * s, 0.76f * s, 0.86f * s, 0.94f * s, r, g, b, 1);
}

/* where v falls in the window, GL pixels (0: behind the eye) */
static int project (const int *v, double *xy)
{
	GLdouble mv[16], pm[16], z;
	GLint vp[4];

	glGetDoublev (GL_MODELVIEW_MATRIX, mv);
	glGetDoublev (GL_PROJECTION_MATRIX, pm);
	glGetIntegerv (GL_VIEWPORT, vp);
	return gluProject (v[0], v[1], v[2], mv, pm, vp, xy, xy + 1, &z);
}

/* where you are on the map: its label's strokes and its marker, projected */
static double here_seg[64][4], here_mark[2];
static int here_n;

void Nu_DrawCircle (void **data)
{
	int v1[3];
	unsigned int dreg2, isize;
	float size;
	int r, g, b;

	dreg2 = znode_rdlong (data);
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);

	isize = (dreg2 << 16) | (dreg2 >> 16);
	//printf ("%x (%x)\n", GetReg (2), isize);
	
	size = -0.002*((short)dreg2)*v1[2];
	draw_corona (data, v1, size);
	if (galmap == 1) {
		/* a star as a glow in its class's colour, white hot at the
		 * middle; the selected one (the game's big disc) as a ring */
		float R = (unsigned char) r / 255.0f, G = (unsigned char) g / 255.0f,
		      B = (unsigned char) b / 255.0f;
		if ((short) dreg2 >= 8) {
			double w[2];	/* where its name is, for hdtext (ST pixels) */
			if (project (v1, w)) {
				hdtext_focus_x = (w[0] - pic_x) * 320 / pic_w;
				hdtext_focus_y = (pic_y + pic_h - w[1]) * 200 / pic_h;
			}
			soft_ring (v1, size, R, G, B);
		} else {
			soft_disc (v1, 0, 0, 0.3f * size, 2.4f * size, R, G, B, 0.35f);
			soft_disc (v1, 0, 0, 0.55f * size, 0.85f * size,
				   (R + 1) / 2, (G + 1) / 2, (B + 1) / 2, 1);
		}
		return;
	}
	
	/* billboard: this is used for distant planets/stars rendered as a
	 * flat shaded dot, which must always face the camera. */
	billboard_begin (v1[0], v1[1], v1[2]);
	gluDisk (qobj, 0.0, size, circle_slices (size, v1[2]), 1);
	billboard_end ();
}

/* life is so strange */
void Nu_PutCylinder ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_CYLINDER);
	znode_wrlightsource (GetReg (REG_A4));
	znode_wrlong (GetReg (REG_D3));
	znode_wrlong (GetReg (REG_D2));
	znode_wrlong (GetReg (REG_D6));
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrlong (GetReg (REG_D0));
	znode_wrlong (GetReg (REG_D1));
	znode_wrlong (GetReg (REG_D5));
	znode_wrlong (GetReg (REG_D4));
}
void Nu_DrawCylinder (void **data)
{
	float light_vec[4];
	int v1[3], v2[3];
	float vdiff[3];
	int rad1, rad2;
	int light_col, obj_col, extra_col;
	float h;

	znode_rdvertexf (data, light_vec);
	light_col = znode_rdlong (data);
	obj_col = znode_rdlong (data);
	extra_col = znode_rdlong (data);

	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);

	vdiff[0] = v2[0] - v1[0];
	vdiff[1] = v2[1] - v1[1];
	vdiff[2] = v2[2] - v1[2];
	
	h = sqrt (vdiff[0]*vdiff[0] + vdiff[1]*vdiff[1] + vdiff[2]*vdiff[2]);
	
	rad1 = znode_rdlong (data) & 0xffff;
	rad2 = znode_rdlong (data) & 0xffff;
	
	glShadeModel (GL_SMOOTH);
	
	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (-RAD_2_DEG * (atan2 (vdiff[2], vdiff[0]) - M_PI/2), 0.0f, 1.0f, 0.0f);
	glRotatef (-RAD_2_DEG * asin (vdiff[1]/h), 1.0f, 0.0f, 0.0f);
#define CYLINDER_POOP	20
	
	lighting_on (light_vec, light_col, extra_col, znode_rdlong (data));
	gluDisk (qobj, 0.0, rad1, CYLINDER_POOP, 1);
	glTranslatef (0, 0, h);
	
	lighting_on (light_vec, light_col, extra_col, znode_rdlong (data));
	gluDisk (qobj, 0.0, rad2, CYLINDER_POOP, 1);
	glTranslatef (0, 0, -h);
	
	glEnable (GL_CULL_FACE);
	lighting_on (light_vec, light_col, extra_col, obj_col);
	gluCylinder (qobj, rad1, rad2, h, CYLINDER_POOP, 1);
	glDisable (GL_CULL_FACE);
		
	glPopMatrix ();
	lighting_off ();
}

/*
 * Filled circles: fe2.s L3b9aa_FilledOvalThingy -> L37fb2_ProjectOvalXYZ,
 * which hcalls Nu_PutOval. The only user seen is the intro's FRONTIER logo
 * (its planet, the planet's lighter face and the orbit ring).
 *
 * The hcall gets a circle in viewing coordinates: its centre in d0-d2 (the
 * same as 4(a0)), the normal of its plane in d3-d5 (words, length 32768)
 * and its radius in d6. The ST turns it into two Bezier half circles
 * (ProjectOvalXYZ: axis A = (-ny, nx, 0)*r/|nxy|, B = n x A at 4/3 r) and
 * puts them into the complex shape the model has started, so they are
 * filled with that shape's colour by its even-odd rule: two circles in
 * one shape make the orbit ring an annulus.
 *
 * So inside a complex shape the circle is one more contour of it
 * (NU_COMPLEX_OVAL). This used to be a black disc facing the camera
 * whatever the shape's colour (no colour comes with the hcall): the
 * intro's logo planet came out black.
 *
 * Outside a complex shape there is no colour to fill with; that case has
 * not been seen and keeps the old disc (NU_OVALTHINGY).
 */
void Nu_PutOval ()
{
	if (use_renderer == R_OLD) return;
	if (complex_st_start) {
		complex_check_abort ();
		if (complex_aborted) return;
		if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
		znode_wrlong (NU_COMPLEX_OVAL);
		/* viewing -> GL eye coordinates: z flips, as in znode_wrvertex */
		znode_wrlong (GetReg (REG_D0));
		znode_wrlong (GetReg (REG_D1));
		znode_wrlong (-GetReg (REG_D2));
		znode_wrlong ((short) GetReg (REG_D3));
		znode_wrlong ((short) GetReg (REG_D4));
		znode_wrlong (-(short) GetReg (REG_D5));
		znode_wrlong ((short) GetReg (REG_D6));
		return;
	}
	znode_wrlong (NU_OVALTHINGY);
	znode_wrvertex (GetReg (REG_A0)+4);
	
	znode_wrlong (GetReg (REG_D3));
	znode_wrlong (GetReg (REG_D4));
	znode_wrlong (GetReg (REG_D5));

	znode_wrlong (GetReg (REG_D6));
}

/* Segments per circle: one per 16 pixels of rim, from 64 up to 180. The
 * orbit ring passes the camera wider than the picture, where a fixed
 * 64-gon showed its corners. An oval is always drawn inside a complex
 * shape, where Nu_DrawComplexStart has loaded an ortho projection: the
 * perspective one is the copy it kept in tessProjMatrix (GL renderer;
 * the wireframe one leaves the perspective loaded). */
#define OVAL_STEPS_MIN	64
#define OVAL_STEPS_MAX	180

static int oval_steps (const GLdouble c[3], double r)
{
	GLdouble pm[16];
	double rpx, n;

	if (-c[2] <= r) return OVAL_STEPS_MAX;	/* at or around the eye */
	if (use_renderer == R_GL) pm[0] = tessProjMatrix[0];
	else glGetDoublev (GL_PROJECTION_MATRIX, pm);
	rpx = r / -c[2] * pm[0] * (wide_3d ? v3d_w : pic_w) * 0.5;
	n = rpx * 2.0 * M_PI / 16.0;
	if (n <= OVAL_STEPS_MIN) return OVAL_STEPS_MIN;
	if (n >= OVAL_STEPS_MAX) return OVAL_STEPS_MAX;
	return (int) n;
}

void Nu_DrawComplexOval (void **data)
{
	GLdouble c[3], n[3], u[3], w[3], v[3];
	double len, lxy, r;
	int i, k, steps;

	znode_rdvertexd (data, c);
	znode_rdvertexd (data, n);
	r = fabs ((double) znode_rdlong (data));
	steps = oval_steps (c, r);

	len = sqrt (n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
	if (len <= 0.0) { n[0] = 0.0; n[1] = 0.0; n[2] = 1.0; len = 1.0; }
	for (k = 0; k < 3; k++) n[k] /= len;
	/* the ST's first axis, (-ny, nx, 0); face on, any will do */
	lxy = sqrt (n[0]*n[0] + n[1]*n[1]);
	if (lxy < 1e-6) { u[0] = 1.0; u[1] = 0.0; }
	else { u[0] = -n[1]/lxy; u[1] = n[0]/lxy; }
	u[2] = 0.0;
	/* w = n x u */
	w[0] = n[1]*u[2] - n[2]*u[1];
	w[1] = n[2]*u[0] - n[0]*u[2];
	w[2] = n[0]*u[1] - n[1]*u[0];

	if (use_renderer == R_GL && contour_n > 0) {
		/* a contour of its own */
		flush_contour ();
		gluTessEndContour (tobj);
		gluTessBeginContour (tobj);
	} else if (use_renderer == R_GLWIRE) {
		/* its own line loop, not joined to the outline before it */
		glEnd ();
		glBegin (GL_LINE_STRIP);
	}
	for (i = 0; i <= steps; i++) {
		double a = 2.0 * M_PI * (i % steps) / steps;
		double ca = r * cos (a), sa = r * sin (a);
		for (k = 0; k < 3; k++) v[k] = c[k] + ca*u[k] + sa*w[k];
		if (use_renderer == R_GLWIRE) glVertex3dv (v);
		else if (i < steps) push_contour_vertex (v);
	}
	if (use_renderer == R_GL) {
		flush_contour ();
		gluTessEndContour (tobj);
		gluTessBeginContour (tobj);
	} else if (use_renderer == R_GLWIRE) {
		glEnd ();
		glBegin (GL_LINE_STRIP);
	}
}

void Nu_DrawOval (void **data)
{
	int v1[3];
	int rad, r, g, b;
	unsigned short d,e,f;

	znode_rdvertex (data, v1);

	r = 0;
	g = 0;
	b = 0;
	
	d = (short) znode_rdlong (data);
	e = (short) znode_rdlong (data);
	f = (short) znode_rdlong (data);
	rad = (short) znode_rdlong (data);
	
	glColor3ub (r, g, b);
	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	//printf ("%d,%d,%d\n", d,e,f);
	//glRotatef (RAD_2_DEG*M_PI*(d/32768.0f), 0.0f, 1.0f, 0.0f);
	//glRotatef (RAD_2_DEG*M_PI*(e/32768.0f), 1.0f, 0.0f, 0.0f);
	//glRotatef (-RAD_2_DEG*M_PI*(f/65536.0f), 0.0f, 1.0f, 0.0f);
	gluDisk (qobj, 0.0, rad, 32, 1);
	glPopMatrix ();
}

void Nu_PutBlob ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_BLOB);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrlong (GetReg (REG_D0));
	znode_wrlong (GetReg (REG_D1));
}
void Nu_DrawBlob (void **data)
{
	int v1[3];
	unsigned int r, g, b, c;
	int rad;
	int edges;
	
	znode_rdvertex (data, v1);
	c = znode_rdlong (data);
	split_rgb444i (c, &r, &g, &b);
	rad = znode_rdlong (data) & 0xffff;
	edges = rad+4;
	
	if (galmap == 1 && rad >= 3) {
		/* the map: where you are (magenta) as a ring, a nebula as a
		 * soft cloud */
		float s = -0.002f * rad * v1[2];
		float R = (c >> 8 & 15) / 15.0f, G = (c >> 4 & 15) / 15.0f, B = (c & 15) / 15.0f;
		if ((c & 0xfff) == 0xa0a) {
			project (v1, here_mark);
			soft_ring (v1, s, R, G, B);
		} else
			soft_disc (v1, 0, 0, 0.35f * s, 1.2f * s, R, G, B, 0.6f);
		return;
	}
	glColor3ui (r, g, b);
	if (rad < 3) {
		glPointSize (((rad/2)+1) * px_scale ());
		glBegin (GL_POINTS);
			glVertex3iv (v1);
		glEnd ();
	} else {
		/* billboard: keep the glow disk facing the camera. */
		billboard_begin (v1[0], v1[1], v1[2]);
		gluDisk (qobj, 0.0, -0.002*(rad)*v1[2], edges, 1);
		billboard_end ();
	}
}
void Nu_PutColoredPoint ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_POINT);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D0));
	znode_wrlong (2);
}

void Nu_PutPoint ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_POINT);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (0xfff);
	znode_wrlong (1);
}
void Nu_DrawPoint (void **data)
{
	int v1[3];
	int point_size, r, g, b;

	if (use_renderer == R_OLD) return;
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	point_size = znode_rdlong (data);

	glPointSize (point_size * px_scale ());
	glColor3ub (r, g, b);
	glBegin (GL_POINTS);
		glVertex3iv (v1);
	glEnd ();
}

void Nu_PutLine ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_LINE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrcolor (GetReg (REG_D6));
}
/* The 'you are here' label (the game's own stroke text) set like the HD
 * map names: scaled about its end by the marker to their cap height,
 * narrowed to their condensed face, light grey in their weight over a
 * dark halo, round ends.  A lone stroke (the far map's pointer) stays. */
static void here_draw (void)
{
	double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, k = 1, kx = 1, ax;
	float cap = 0.016f * pic_h, wid[2] = { 0.4f * cap, 0.18f * cap };
	GLint vp[4];
	int i, j, pass;

	for (i = 0; i < here_n; i++)
		for (j = 0; j < 4; j += 2) {
			x0 = fmin (x0, here_seg[i][j]);
			x1 = fmax (x1, here_seg[i][j]);
			y0 = fmin (y0, here_seg[i][j + 1]);
			y1 = fmax (y1, here_seg[i][j + 1]);
		}
	if (here_n == 1) wid[1] = 0.4f * px_scale ();
	else if (y1 > y0) kx = 0.8 * (k = cap / (y1 - y0));
	ax = fabs (here_mark[0] - x1) < fabs (here_mark[0] - x0) ? x1 : x0;
	glGetIntegerv (GL_VIEWPORT, vp);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (vp[0], vp[0] + vp[2], vp[1], vp[1] + vp[3], -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glTranslated (ax, (y0 + y1) / 2, 0);
	glScaled (kx, k, 1);
	glTranslated (-ax, -(y0 + y1) / 2, 0);
	glPushAttrib (GL_ENABLE_BIT | GL_LINE_BIT | GL_POINT_BIT);
	glDisable (GL_DEPTH_TEST);
	glEnable (GL_POINT_SMOOTH);
	/* the halo pass over every stroke first, so none dims another's core */
	for (pass = here_n == 1; pass < 2; pass++) {
		if (pass) glColor4ub (222, 222, 222, 255);
		else glColor4f (0, 0, 0, 0.6f);
		glLineWidth (wid[pass]);
		glPointSize (wid[pass]);
		for (j = 0; j < 2; j++) {
			glBegin (j ? GL_LINES : GL_POINTS);
			for (i = 0; i < here_n; i++) {
				glVertex2dv (here_seg[i]);
				glVertex2dv (here_seg[i] + 2);
			}
			glEnd ();
		}
	}
	glPopAttrib ();
	glPopMatrix ();
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	here_n = 0;
}

void Nu_DrawLine (void **data)
{
	int v1[3], v2[3];
	int r, g, b;
	
	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);
	znode_rdcolor (data, &r, &g, &b);
	
	if (galmap == 1 && r == b && (unsigned char) r > 3 * (unsigned char) g + 40) {
		/* the map's magenta, where you are: kept for here_draw */
		if (here_n < 64 && project (v1, here_seg[here_n]) && project (v2, here_seg[here_n] + 2))
			here_n++;
		return;
	}
	/* the engine's lines (lasers, aerials, wires) are one ST pixel wide;
	 * keep them the same share of the view at any picture size, as the
	 * points are, instead of a hairline at 1600x1200 */
	glLineWidth ((galmap == 1 ? 0.6f : 1.0f) * px_scale ());
	glColor3ub (r, g, b);
	glBegin (GL_LINES);
		glVertex3iv (v1);
		glVertex3iv (v2);
	glEnd ();
	glLineWidth (1.0f);
}

void Nu_IsGLRenderer ()
{
	if (use_renderer == R_OLD) {
		SetReg (0, 0);
	} else {
		SetReg (0, 1);
	}
}

void Nu_GLClearArea ()
{
	unsigned char *screen, *screen2;
	int x,y,x1,x2,y1,y2;

	if (use_renderer == R_OLD) return;
	x1 = GetReg (0)&0xffff;
	y1 = GetReg (1)&0xffff;
	x2 = GetReg (2)&0xffff;
	y2 = GetReg (3)&0xffff;
	/* black behind the view, unless the HD layer has given it a card */
	if (!hdui_clear_area (x1, y1, x2, y2)) {
		push_ortho ();
		set_ctrl_viewport ();
		glColor3f (0.0f, 0.0f, 0.0f);
		glBegin (GL_TRIANGLE_STRIP);
			glVertex3f (x1, 200-y1, 0);
			glVertex3f (x2, 200-y1, 0);
			glVertex3f (x1, 200-y2, 0);
			glVertex3f (x2, 200-y2, 0);
		glEnd ();
		set_main_viewport ();
		pop_ortho ();
	}

	/* and then we wipe the bit of the ST framebuffer (on both buffers)
	 * to transparent/unset */
	screen = (unsigned char *)PHYSCREEN;
	screen += SCREENBYTES_LINE * y1;
	screen2 = (unsigned char *)LOGSCREEN;
	screen2 += SCREENBYTES_LINE * y1;

	for (y=y1; y<y2; y++) {
		for (x=x1; x<x2; x++) {
			*(screen+x) = 255;
			*(screen2+x) = 255;
		}
		screen += SCREENBYTES_LINE;
		screen2 += SCREENBYTES_LINE;
	}
}

typedef void (*NU_DRAWFUNC) (void **);

/*
 * Widescreen flight (wide_3d): the core sends a planet only while its disc
 * overlaps its own 4:3 picture (L3cd9c_ProjectPlanet), so a planet whose
 * disc runs on into a side strip would vanish there as a whole disc the
 * moment its rim leaves the picture. Instead it fades out over the last
 * PLANET_FADE_PX of overlap (and in again the same way), with the surface
 * and its atmosphere layers alike. A planet well inside, and any 4:3 window,
 * is drawn exactly as before.
 */
#define PLANET_FADE_PX	(24.0 * pic_h / 720.0)

static void planet_faded (NU_DRAWFUNC draw, void **data)
{
	const struct PlanetPrim *p = *data;
	double t = tan (36.5 * M_PI / 360.0), lim[2] = { 1.9*t, t };
	double z = p->centre[2], R2 = p->R * p->R, a = 1.0;
	/* pixels per unit of x/z and y/z, as in draw_wide_flight */
	double px[2] = { pic_w/2.0/lim[0], (pic_h - 32*pic_h/200)/2.0/lim[1] };
	int i;

	if (!wide_3d || z <= 0.0 || z*z <= R2) {
		draw (data);
		return;
	}
	for (i = 0; i < 2; i++) {
		/* the disc's extent in x/z (y/z): the tangent planes */
		double c = p->centre[i], q = p->R * sqrt (c*c + z*z - R2);
		double s0 = (c*z - q) / (z*z - R2), s1 = (c*z + q) / (z*z - R2);
		a = MIN (a, MIN (lim[i] - s0, s1 + lim[i]) * px[i] / PLANET_FADE_PX);
	}
	if (a >= 1.0) {
		draw (data);
	} else if (a <= 0.0) {
		*data += sizeof (struct PlanetPrim);
	} else {
		glPushAttrib (GL_COLOR_BUFFER_BIT);
		glEnable (GL_BLEND);
		glBlendColor (0.0f, 0.0f, 0.0f, (GLfloat) a);
		glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
		draw (data);
		glPopAttrib ();
	}
}
static void Nu_DrawPlanetFaded (void **data) { planet_faded (Nu_DrawPlanet, data); }
static void Nu_DrawAtmosphereFaded (void **data) { planet_faded (Nu_DrawAtmosphere, data); }

NU_DRAWFUNC nu_drawfuncs[NU_MAX] = {
	NULL,
	&Nu_DrawTriangle,
	&Nu_DrawQuad,
	&Nu_DrawLine,
	&Nu_DrawBezierLine,
	&Nu_DrawTeardrop,
	&Nu_DrawComplexSNext, // 6
	&Nu_DrawComplexStart,
	&Nu_DrawComplexEnd,
	&Nu_DrawComplexStartInner, // 9
	&Nu_DrawComplexBezier,
	&Nu_DrawTwinklyCircle,
	&Nu_DrawPlanetFaded,
	&Nu_DrawCircle,
	&Nu_DrawCylinder,
	&Nu_DrawBlob,
	&Nu_DrawOval,
	&Nu_DrawPoint,
	&Nu_Draw2DLine,
	NULL,	/* NU_SUBTREE, see Nu_DrawPrimitive */
	&Nu_DrawAtmosphereFaded,
	&Nu_DrawSkyBand,
	&Nu_DrawComplexOval
};

/*
 * Primitives inside a znode are already in the exact order the engine
 * wants them painted (hull face first, then the decals that lie on it:
 * logos, panels, vector text, complex shape fills...). Just replay that
 * order - see draw_3dview for why there is no depth test to fight with.
 */
static void draw_3dview (struct ZNode *node);

static void Nu_DrawPrimitive (void *data)
{
	int fnum;
	
	for (;;) {
		fnum = znode_rdlong (&data);
		//fprintf (stderr, "%d ", fnum);
		if (!fnum) return;
		if (fnum != NU_TRIANGLE && fnum != NU_QUAD) hull_end ();
		if (fnum == NU_SUBTREE) {
			draw_3dview (subtree_root[znode_rdlong (&data)]);
			continue;
		}
		nu_drawfuncs[fnum] (&data);
	}
}

/*
 * znode_start is the head of a btree of znodes, each with a linked list
 * of GL display lists to draw (in list order).
 *
 * Draw this crap starting from biggest value znodes.
 *
 * NO DEPTH BUFFER IS USED HERE, on purpose. This engine is a painter's
 * algorithm engine: fe2.s sorts every single primitive into a z tree
 * (L38594_InsertIntoZTree, mirrored here by Nu_InsertZNode) and paints
 * back to front. The software renderer has no depth buffer at all and
 * gets the right picture, so we get it the same way.
 *
 * This used to glClear(GL_DEPTH_BUFFER_BIT) before every node with the
 * depth test on. That was worse than useless: Nu_InsertZNode allocates
 * one node per primitive, so the depth buffer was wiped between
 * primitives and never actually compared two of them - it only ever
 * applied *within* a node, which is exactly where the engine emits
 * deliberately coplanar decals (ship logos, panels, vector text, complex
 * shape fills) on top of the face they belong to. Those are ties, so
 * they fought and dropped out at random no matter how much depth
 * precision we threw at it. Removing the depth test makes z-fighting
 * impossible by construction, and drops thousands of full screen depth
 * clears per frame.
 *
 * Note the zvals are NOT real 3D distances: some primitives (distant
 * backdrops, cloud layers) get small/arbitrary vertex Z regardless of
 * their intended "far away" zval, which is another reason a global depth
 * buffer cannot work here.
 */
static void draw_3dview (struct ZNode *node)
{
	if (node == NULL) return;
	if (node->more) draw_3dview (node->more);
	
	if (use_renderer) {
		//fprintf (stderr, "Z=%d ", node->z);
		Nu_DrawPrimitive (node->data);
	}

	if (node->less) draw_3dview (node->less);
}

static void set_gl_clear_col (int rgb)
{
	float r,g,b;
	r = (rgb&0xff)/255.0f;
	g = (rgb&0xff00)/65280.0f;
	b = (rgb&0xff0000)/16711680.0f;
	glClearColor (r,g,b,0);
}

/*
 * A flight view in a window wider (or taller) than 4:3: the 3D view goes
 * on past the picture where the black bars were, and the console's glass
 * past the panel.
 *
 * One pass through a wider frustum with the same centre and scale, so the
 * picture keeps its place and size and there is no seam to see. The core
 * hands over every object inside its cull cone (L39646_3DObjClipOffscreen:
 * |x| <= z, 2|y| <= z, less the object's size), which is wider than the
 * picture, and the star blobs too (Nu_PutBlob comes before the core's
 * 320x168 test, which only feeds its own 2D list): ships, stations,
 * planets and stars all carry on into the strips. The view stops at that
 * cone, so nothing pops in at the window's edge; a window wider than about
 * 2.1:1 keeps the rest of its bars. Only what the core drops one primitive
 * at a time off its 320x168 screen never reaches us: a circle whose centre
 * is off it (L3a93c_project_real_circle: the small lights) and a line
 * wholly off it (L3ace2_project_line).
 *
 * The console: beside the panel and under the picture, dark glass in
 * hdui's base colours (C_BG1 at the top to C_BG0), the panel's top line
 * run on.
 */
static int draw_wide_flight (void)
{
	int ctrl_h = 32*pic_h/200, i;
	int ox = pic_x, oy = pic_y + ctrl_h, ow = pic_w, oh = pic_h - ctrl_h;
	double t = tan (36.5 * M_PI / 360.0);		/* gluPerspective's */
	double cx = ox + ow/2.0, cy = oy + oh/2.0, hx = ow/2.0, hy = oh/2.0;
	int x0 = MAX (0, (int) ceil (cx - hx/(1.9*t)));
	int x1 = MIN (fb_w, (int) floor (cx + hx/(1.9*t)));
	int y1 = MIN (fb_h, (int) floor (cy + hy*0.5/t));
	int e = MAX (1, pic_h/400);
	/* the sky left, right and above the picture; the glass left and
	 * right of the panel and below the picture */
	int r[6][4] = {
		{ x0, oy, ox - x0, y1 - oy }, { ox + ow, oy, x1 - ox - ow, y1 - oy },
		{ ox, oy + oh, ow, y1 - oy - oh },
		{ 0, 0, ox, oy }, { ox + ow, 0, fb_w - ox - ow, oy }, { ox, 0, ow, pic_y } };
	double sx = 320.0/fb_w, sy = 200.0/fb_h;
	GLdouble proj[16];

	if (pic_w >= fb_w && pic_h >= fb_h) return 0;
	glEnable (GL_SCISSOR_TEST);
	/* the sky colour (still the clear colour) past the picture only */
	for (i = 0; i < 3; i++) {
		if (r[i][2] <= 0 || r[i][3] <= 0) continue;
		glScissor (r[i][0], r[i][1], r[i][2], r[i][3]);
		glClear (GL_COLOR_BUFFER_BIT);
	}
	glScissor (x0, oy, x1 - x0, y1 - oy);
	glGetDoublev (GL_PROJECTION_MATRIX, proj);
	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	glFrustum (1.9*t*GL_NEAR_PLANE*(x0 - cx)/hx, 1.9*t*GL_NEAR_PLANE*(x1 - cx)/hx,
		   -t*GL_NEAR_PLANE, t*GL_NEAR_PLANE*(y1 - cy)/hy,
		   GL_NEAR_PLANE, GL_FAR_PLANE);
	glMatrixMode (GL_MODELVIEW);
	wide_3d = 1;
	v3d_x = x0; v3d_y = oy; v3d_w = x1 - x0; v3d_h = y1 - oy;
	set_main_viewport ();
	draw_3dview (znode_start);
	hull_end ();
	glDisable (GL_SCISSOR_TEST);
	wide_3d = 0;
	glMatrixMode (GL_PROJECTION);
	glLoadMatrixd (proj);
	glMatrixMode (GL_MODELVIEW);

	glViewport (0, 0, fb_w, fb_h);
	push_ortho ();
	glPushAttrib (GL_ENABLE_BIT | GL_LIGHTING_BIT);
	glDisable (GL_LIGHTING);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_BLEND);
	glShadeModel (GL_SMOOTH);
	glBegin (GL_QUADS);
	for (i = 3; i < 6; i++) {
		int *q = r[i];
		if (q[2] <= 0 || q[3] <= 0) continue;
		glColor3ub (0x0b, 0x10, 0x16);
		glVertex2d (q[0]*sx, 0);
		glVertex2d ((q[0] + q[2])*sx, 0);
		glColor3ub (0x14, 0x1c, 0x26);
		glVertex2d ((q[0] + q[2])*sx, q[3]*sy);
		glVertex2d (q[0]*sx, q[3]*sy);
		if (i == 5) continue;
		glColor3ub (0x36, 0x3f, 0x49);
		glVertex2d (q[0]*sx, (oy - e)*sy);
		glVertex2d ((q[0] + q[2])*sx, (oy - e)*sy);
		glVertex2d ((q[0] + q[2])*sx, oy*sy);
		glVertex2d (q[0]*sx, oy*sy);
	}
	glEnd ();
	glPopAttrib ();
	pop_ortho ();
	set_main_viewport ();
	return 1;
}

/*
 * There is deliberately no backdrop drawing here.
 *
 * Nothing paints the backdrop under GL: Fn_Draw3DView, the engine's 2D
 * primitive-list interpreter, is skipped outright while gl_renderer_on
 * is set, and the full width spans the software renderer uses for
 * the sky bands (Call_FillLine) are never issued. The flat sky colour left
 * by glClear(MainRGBPalette[fe2_bgcol]) is all we get, so the ground has to
 * come from the planet primitive itself - see draw_ground_dome.
 */

void Nu_DrawScreen ()
{
	/* build RGB palettes */
	_BuildRGBPalette (MainRGBPalette, MainPalette, len_main_palette);
	_BuildRGBPalette (CtrlRGBPalette, CtrlPalette, 16);
	
	//fprintf (stderr, "Render: ");
	if (znode_cur) end_node ();
	//printf ("Frame: %d znodes.\n", znode_buf_pos);
	planet_ground_seen = 0;
	galmap_update ();
	if (scene_overflow) {
		/* S-6: the display lists are cut short somewhere: drop this
		 * frame's 3D view rather than walk them */
		if ((scene_overflows++ & 255) == 0)
			fprintf (stderr, "GL: 3D scene too big for the scene buffers, frame dropped (%u so far)\n",
				 scene_overflows);
	} else if (galmap == 1) {
		/* the map's grid and stalks: thin smooth lines, not pixel wide */
		glEnable (GL_LINE_SMOOTH);
		glEnable (GL_BLEND);
		draw_3dview (znode_start);
		here_draw ();
		glDisable (GL_BLEND);
		glDisable (GL_LINE_SMOOTH);
	} else {
		Uint32 t = hull_log_on () ? SDL_GetTicks () : 0;
		hangar_faces = 0;
		/* a flight view with the panel up and no HD backdrop */
		if (use_renderer != R_GL || classic_ui || !znode_start
		    || !STMemory_ReadByte (__DL60cf_hblank_do_pal) || hdback_active ()
		    || !bindings_in_flight_view (0, 0) || !draw_wide_flight ())
			draw_3dview (znode_start);
		if (t) hull_log_frame (t);
	}
	hull_end ();
	if (hangar_faces) hull_hangar_grade ();
	//fprintf (stderr, "\n");

#if PRIM_DEBUG
	prim_debug_frame_end ();
#endif

	if (mouse_shown) {
		SDL_ShowCursor (SDL_ENABLE);
		mouse_shown = 0;
	} else {
		SDL_ShowCursor (SDL_DISABLE);
	}
	draw_control_panel ();
	glFlush ();
	
	SDL_GL_SwapBuffers ();

	/* frontier background color... */
	if (use_renderer == R_GLWIRE) {
		glClearColor (0,0,0,0);
	} else {
		set_gl_clear_col (MainRGBPalette[fe2_bgcol]);
	}
	
	glMatrixMode (GL_MODELVIEW);
	if (pic_w < fb_w || pic_h < fb_h) {
		/* black bars beside the picture, the sky colour inside it */
		GLfloat sky[4];
		glGetFloatv (GL_COLOR_CLEAR_VALUE, sky);
		glClearColor (0, 0, 0, 0);
		glClear (GL_COLOR_BUFFER_BIT);
		glClearColor (sky[0], sky[1], sky[2], sky[3]);
		glScissor (pic_x, pic_y, pic_w, pic_h);
		glEnable (GL_SCISSOR_TEST);
		glClear (GL_COLOR_BUFFER_BIT);
		glDisable (GL_SCISSOR_TEST);
	} else {
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	if (use_renderer == R_GL) hdback_draw (fb_w, fb_h);
	glLoadIdentity ();
	
	set_main_viewport ();
	/* a 2D page's backdrop, under everything the next frame draws */
	hdui_backdrop (MainRGBPalette, fe2_bgcol);
	if (galmap == 1) galmap_backdrop ();
	/* the game-over backdrop, under the tombstone */
	endcard_frame ();
}
