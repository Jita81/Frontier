/*
 * The green ship's cockpit in the opening video (docs/hd/hulls.md, "The
 * cockpit"): a cabin, a small pilot and a glass cap drawn in the painter's
 * slot of the game's black oval, the game's wire lines left to draw over
 * them as struts.  Sub-model keys only ever carry HULL_TAB_SCENE off
 * hull_intro_base, so every flag that keeps the base look keeps this off.
 */
#ifndef _COCKPIT_H
#define _COCKPIT_H

#include "hull.h"

#define COCKPIT_OVAL	(0x80000000u | HULL_TAB_INTRO | HULL_TAB_SCENE | 88)
#define COCKPIT_PILOT	(0x80000000u | HULL_TAB_INTRO | HULL_TAB_SCENE | 90)
#define COCKPIT_PILOT_CIRCLE	(COCKPIT_PILOT & 0x3ffffu)	/* as Nu_PutCircle keeps it */

/* the engine's 3D view is starting: this frame's pilots are forgotten */
void cockpit_begin (void);
/* Put: model 90's circle, eye space (z negated as the display list has it) */
void cockpit_pilot_put (const int e[3], int rgb444);
/* draw: a curve of the oval's shape, its four control points */
void cockpit_arc (float cp[4][3]);
/* draw: the oval has been filled; lt towards the light in view space */
void cockpit_draw (const short lt[3], const double proj[16]);
/* draw: this part of model 90 at v is replaced, so skip it */
int cockpit_replaced (float x, float y, float z);

#endif
