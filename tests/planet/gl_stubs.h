/*
 * Just enough of OpenGL for the planet block of gl.c to run with no
 * context: texture names are tracked, so that binding, filling or
 * deleting one that does not exist is reported, and uploads are read
 * in full so that AddressSanitizer checks the pixel buffers.
 */
#ifndef GL_STUBS_H
#define GL_STUBS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEX (1 << 16)
static unsigned char tex_live[MAX_TEX];
static GLuint tex_next = 1, tex_bound;
static int gl_errors, tex_count, tex_uploads, unpack_align = 4;
static long vertex_count;
static unsigned long pixel_sum;

static void gl_error (const char *what, GLuint t)
{
	if (gl_errors++ < 10) printf ("GL misuse: %s %u\n", what, t);
}

void glGenTextures (GLsizei n, GLuint *t)
{
	int i;
	for (i = 0; i < n; i++) {
		if (tex_next >= MAX_TEX) { gl_error ("out of names", 0); t[i] = 0; continue; }
		t[i] = tex_next++;
		tex_live[t[i]] = 1;
		tex_count++;
	}
}
void glDeleteTextures (GLsizei n, const GLuint *t)
{
	int i;
	for (i = 0; i < n; i++) {
		if (!t[i]) continue;
		if (t[i] >= MAX_TEX || !tex_live[t[i]]) { gl_error ("delete of a dead texture", t[i]); continue; }
		tex_live[t[i]] = 0;
		tex_count--;
		if (tex_bound == t[i]) tex_bound = 0;
	}
}
void glBindTexture (GLenum target, GLuint t)
{
	(void) target;
	if (t && (t >= MAX_TEX || !tex_live[t])) gl_error ("bind of a dead texture", t);
	tex_bound = t;
}
void glPixelStorei (GLenum pname, GLint v) { if (pname == GL_UNPACK_ALIGNMENT) unpack_align = v; }
void glTexImage2D (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
		   GLint border, GLenum fmt, GLenum type, const GLvoid *pixels)
{
	int bpp = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE ? 1 : 4;
	long row, size, i;
	const unsigned char *px = pixels;
	(void) target; (void) level; (void) ifmt; (void) border; (void) type;
	if (!tex_bound) gl_error ("upload with no texture bound", 0);
	row = ((long) w * bpp + unpack_align - 1) / unpack_align * unpack_align;
	size = row * (h - 1) + (long) w * bpp;
	if (px) for (i = 0; i < size; i++) pixel_sum += px[i];
	tex_uploads++;
}
void glTexParameteri (GLenum a, GLenum b, GLint c) { (void) a; (void) b; (void) c; }
void glTexEnvi (GLenum a, GLenum b, GLint c) { (void) a; (void) b; (void) c; }
void glEnable (GLenum a) { (void) a; }
void glDisable (GLenum a) { (void) a; }
void glShadeModel (GLenum a) { (void) a; }
void glBegin (GLenum a) { (void) a; }
void glEnd (void) { }
void glColor3f (GLfloat r, GLfloat g, GLfloat b) { (void) r; (void) g; (void) b; }
void glColor3ub (GLubyte r, GLubyte g, GLubyte b) { (void) r; (void) g; (void) b; }
void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void) r; (void) g; (void) b; (void) a; }
void glColor4ub (GLubyte r, GLubyte g, GLubyte b, GLubyte a) { (void) r; (void) g; (void) b; (void) a; }
void glTexCoord2dv (const GLdouble *v) { pixel_sum += (v[0] == v[0]) + (v[1] == v[1]); }
void glTexCoord2d (GLdouble s, GLdouble t) { (void) s; (void) t; }
void glVertex3d (GLdouble x, GLdouble y, GLdouble z) { (void) x; (void) y; (void) z; vertex_count++; }
void glVertex3dv (const GLdouble *v) { (void) v[2]; vertex_count++; }
void glBlendFunc (GLenum a, GLenum b) { (void) a; (void) b; }
void glDepthMask (GLboolean a) { (void) a; }
void glPushAttrib (GLbitfield a) { (void) a; }
void glPopAttrib (void) { }
void glMultiTexCoord2d (GLenum u, GLdouble s, GLdouble t) { (void) u; pixel_sum += (s == s) + (t == t); }
void glMultiTexCoord3dv (GLenum u, const GLdouble *v) { (void) u; pixel_sum += (v[0] == v[0]) + (v[2] == v[2]); }

/* procfx layer 2: test_render.c runs both paths, smooth and exact */
int flat_planets;
static int planet_shader = 1;
int procfx_planet_begin (const float lo[4], const float hi[4], float soft,
			 const float rim[4], const float look[3], unsigned int seed)
{
	(void) lo; (void) hi; (void) soft; (void) rim; (void) look; (void) seed;
	return planet_shader && !flat_planets;
}
void procfx_planet_bind (int unit, unsigned int tex) { (void) unit; glBindTexture (GL_TEXTURE_2D, tex); }
void procfx_planet_end (void) { }

#endif
