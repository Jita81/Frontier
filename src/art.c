/*
 * The art folder and an image loader for the HD layers (see art.h).
 */
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"

const char *art_path (const char *rel)
{
	static char buf[4096];
	const char *dir = getenv ("FE2_ART_DIR");

	snprintf (buf, sizeof buf, "%s/%s", dir && *dir ? dir : "art", rel);
	return buf;
}

unsigned char *art_load_rgba (const char *rel, int *w, int *h)
{
#ifdef __APPLE__
	const char *path = art_path (rel);
	CFURLRef url;
	CGImageSourceRef src;
	CGImageRef img = NULL;
	CGColorSpaceRef rgb;
	CGContextRef ctx = NULL;
	unsigned char *px;

	url = CFURLCreateFromFileSystemRepresentation (NULL, (const UInt8 *) path,
						       strlen (path), false);
	if (!url) return NULL;
	src = CGImageSourceCreateWithURL (url, NULL);
	CFRelease (url);
	if (!src) return NULL;
	if (CGImageSourceGetCount (src) > 0)
		img = CGImageSourceCreateImageAtIndex (src, 0, NULL);
	CFRelease (src);
	if (!img) return NULL;

	*w = (int) CGImageGetWidth (img);
	*h = (int) CGImageGetHeight (img);
	px = calloc ((size_t) *w * *h, 4);
	rgb = CGColorSpaceCreateDeviceRGB ();
	if (px)
		ctx = CGBitmapContextCreate (px, *w, *h, 8, (size_t) *w * 4, rgb,
					     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease (rgb);
	if (ctx) {
		/* the context's first row in memory is the image's top */
		CGContextDrawImage (ctx, CGRectMake (0, 0, *w, *h), img);
		CGContextRelease (ctx);
	} else {
		free (px);
		px = NULL;
	}
	CGImageRelease (img);
	return px;
#else
	(void) rel; (void) w; (void) h;
	return NULL;
#endif
}
