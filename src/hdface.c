/*
 * hdface: see hdface.h.  Nothing yet: the screen is uploaded as it is.
 */
#include "hdface.h"

void hdface_blit (const unsigned char *planes, int width_words, int height,
		  int x, int y, unsigned int bmp, unsigned int scr)
{
}

void hdface_frame (const unsigned char *raster, unsigned char *out,
		   const unsigned int *pal, const unsigned int *ctrl_pal)
{
}

void hdface_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
}

void hdface_forget (void)
{
}
