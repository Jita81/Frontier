/*
 * puffs: the game's own low clouds (model 198) lit as cumulus, see
 * docs/hd/clouds.md, "Low clouds".
 *
 * Far away a low cloud is a few outlines of Bezier curves (complex
 * shapes), close by a few quads and triangles.  The game's fill draws
 * every one of them as it always did (the same tessellation, the same
 * vertices, so the same pixels covered); a fragment program bound around
 * it shades inside that coverage.  The put side (gl.c) tags the shape or
 * face with NU_PUFF; nothing else is added to the stream.  A new file
 * because it is one shader with its own bind, as skies.c is for the rings.
 * Off (puff_on): the old renderer, --flat-clouds, --classic-ui, --pixel-*.
 */
#include <math.h>
#include <string.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "main.h"
#include "screen.h"
#include "procfx.h"
#include "skies.h"

int puff_on (void)
{
	return use_renderer == R_GL && !flat_clouds && !classic_ui && !pixel_art && !pixel_text && !pixel_faces;
}

/* step 0: the game's colour, as the fixed pipeline gives it */
static const char puff_glsl[] =
	"void main () {\n"
	"	gl_FragColor = vec4 (gl_Color.rgb, 1.0);\n"
	"}\n";

static GLuint prog;
static int failed, bound, pending;
static struct puff_tag cur;
static float corner[4][3];

/* once for a run of a record's shapes and faces, not for each one */
static int puff_bind (void)
{
	if (bound) return 1;
	if (failed) return 0;
	if (!prog) {
		prog = procfx_program ("puffs", puff_glsl);
		if (!prog) { failed = 1; return 0; }
	}
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram (prog);
	bound = 1;
	return 1;
}

void puff_end (void)
{
	if (!bound) return;
	glUseProgram (0);
	glPopAttrib ();
	bound = 0;
}

void puff_draw_tag (void **data)
{
	memcpy (&cur, *data, sizeof cur);
	*data += sizeof cur;
	pending = 1;
}

/* Before every primitive but the ones inside a shape: 1 when the tag just
 * read is for it (kind 0 a complex shape, 1 a face); the shader goes with
 * any other one */
int puff_next (int kind)
{
	int use = pending && cur.kind == kind;

	pending = 0;
	if (!use) puff_end ();
	return use;
}

/* a GL eye point in the model's space */
static void puff_obj (const double e[3], double w[3])
{
	double v[3] = { e[0] - cur.o[0], e[1] - cur.o[1], -e[2] - cur.o[2] };
	int j;

	for (j = 0; j < 3; j++) w[j] = cur.M[3*j] * v[0] + cur.M[3*j+1] * v[1] + cur.M[3*j+2] * v[2];
}

/*
 * One shape, just before the game's tessellator draws it: e its n points
 * in GL eye space (every contour), win the tessellator's np points in
 * window space.  0 when the game's flat fill is to be used.
 */
int puff_shape (const double (*e)[3], int n, const double (*win)[3], int np, const int vp[4])
{
	(void) e; (void) n; (void) win; (void) np; (void) vp;
	return puff_bind ();
}

/* A face (its n corners p in GL eye space): 0 when flat; else
 * puff_corner (i) before each corner's glVertex */
int puff_face (const double (*p)[3], int n)
{
	int i, k;

	for (i = 0; i < n; i++) {
		double w[3];
		puff_obj (p[i], w);
		for (k = 0; k < 3; k++) corner[i][k] = (float) w[k];
	}
	return puff_bind ();
}

void puff_corner (int i)
{
	glMultiTexCoord3f (GL_TEXTURE1, corner[i][0], corner[i][1], corner[i][2]);
}
