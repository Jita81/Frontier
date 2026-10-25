#ifndef _EXP_H
#define _EXP_H

/* Phase 2 experiment harness (docs/experiments). Everything behind these
 * calls is default OFF: unless one of the FE2_* environment variables is
 * set, the calls return without touching any game or host state. */

extern int exp_any;	/* non-zero when any FE2_* experiment flag is set */

void Exp_Init (void);
void Exp_HostUpdate (void);	/* after Main_EventHandler in Call_HostUpdate */
void Exp_EndFrame (void);	/* in Nu_DrawScreen just before the flip/swap */

/* implemented by the renderer (soft.c / gl.c): reads the frame about to be
 * shown as packed RGB24, top row first. Returns 0 on success. */
int Screen_ReadRGB (unsigned char **rgb, int *w, int *h);

/* E1 FE2_TEXT=hires (exptext.c, GL build only) */
extern int exp_text_on;
#ifdef WITH_GL
#include <stdio.h>
void ExpText_Init (void);
void ExpText_GlyphBegin (unsigned char *scrline, int x, int y, int chr, int col, int shadowed);
void ExpText_GlyphEnd (int next_x);
void ExpText_Clear (unsigned int adr, unsigned int count);
void ExpText_Memcpy (unsigned int dest, unsigned int src, unsigned int count);
const unsigned char *ExpText_Peel (const unsigned char *raster);
void ExpText_Draw (int screen_w, int screen_h);
void ExpText_Stats (FILE *f);
#endif

#endif /* _EXP_H */
