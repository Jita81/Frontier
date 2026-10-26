/*
 * hdstill: see hdstill.h.  Nothing yet: the screen is uploaded as it is.
 */
#include "hdstill.h"

void hdstill_blit (const unsigned char *planes, int width_words, int height,
		   int x, int y, unsigned int bmp, unsigned int scr)
{
}

void hdstill_frame (const unsigned char *raster, unsigned char *out,
		    const unsigned int *pal, const unsigned int *ctrl_pal)
{
}

void hdstill_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
}

void hdstill_forget (void)
{
}
