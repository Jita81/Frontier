/*
 * New art the GL renderer draws in place of the game's own pictures.
 *
 * It lives in art/ (tracked: new art only, never the game's) and is found
 * through FE2_ART_DIR (the app's launcher points that into the bundle),
 * else as "art" in the current directory.
 */
#ifndef _ART_H
#define _ART_H

/* The path of rel in the art folder, in a static buffer that the next
 * call overwrites. */
const char *art_path (const char *rel);

/* An image file in the art folder (PNG, JPEG, whatever ImageIO reads) as
 * 8 bit RGBA, alpha premultiplied, top row first: a malloc'd buffer of
 * *w x *h pixels, or NULL when the file is missing or unreadable (or the
 * platform has no ImageIO). */
unsigned char *art_load_rgba (const char *rel, int *w, int *h);

#endif /* _ART_H */
