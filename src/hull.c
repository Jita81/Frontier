/*
 * Hull material for the GL renderer: procedural textures, profiles and the
 * per-polygon frame. See hull.h; gl.c has the shaders and the draw code.
 *
 * Nothing here comes from the game's data or from any image: a texture is
 * a height field of panels built from a seed (a kd-tree layout on a
 * torus, so the tile repeats without a visible edge), and the maps the
 * shader reads are worked out from that height field.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hull.h"

/*
 * Profiles. Style 1 to 3 use HP_GENERIC for every ship; style 4 picks one
 * of the other four per model (hull_models, below). The averages are kept
 * the same everywhere: a profile changes the look of the plating, not how
 * bright the game made a face.
 */
const struct HullProfile hull_profiles[HP_N] = {
/*	name		seed	pmin  pmax  recess hatch vent  greeb hazrd rivet wear  contr bump gloss shine ao    tint */
	{ "generic",	0x1001,	0.08, 0.30, 0.60, 0.14, 0.12, 0.18, 0.06, 0.40, 0.45, 0.06, 1.00, 0.40, 24,  0.40, 0.15 },
	{ "small craft", 0x2002, 0.12, 0.45, 0.30, 0.10, 0.08, 0.10, 0.00, 0.30, 0.15, 0.04, 0.70, 0.35, 28,  0.30, 0.05 },
	{ "trader",	0x3003,	0.08, 0.32, 0.60, 0.18, 0.14, 0.24, 0.10, 0.60, 0.85, 0.08, 1.00, 0.30, 18,  0.35, 0.35 },
	{ "military",	0x4004,	0.07, 0.28, 0.80, 0.16, 0.20, 0.08, 0.02, 0.15, 0.12, 0.03, 1.25, 0.60, 60,  0.50, 0.00 },
	{ "station",	0x5005,	0.08, 0.32, 1.00, 0.16, 0.16, 0.26, 0.12, 0.45, 0.60, 0.07, 1.10, 0.25, 14,  0.45, 0.30 },
};

/*
 * Style 4: which profile a model gets, by the type word the 68k keeps at
 * object+90 (a byte offset into the model table, so twice the model's
 * number; FE2_HULL_LOG=1 prints the ones a session meets). One line per
 * model; edit freely. A model not listed gets HP_GENERIC.
 */
static const struct { unsigned char type; unsigned char prof; } hull_models[] = {
	{ 20, HP_SMALL },	/* Interplanetary Shuttle */
	{ 32, HP_STATION },	/* the hangar interior (walls and signs) */
	{ 40, HP_MILITARY },	/* Krait Assault Craft */
	{ 42, HP_SMALL },	/* Gecko */
	{ 44, HP_TRADER },	/* Adder */
	{ 46, HP_MILITARY },	/* Viper Defence Craft */
	{ 50, HP_TRADER },	/* Moray Starboat */
	{ 56, HP_TRADER },	/* Asp Explorer */
	{ 66, HP_TRADER },	/* Python Freighter */
	{ 72, HP_STATION },	/* the station, outside and in the hangar */
};

int hull_profile_of_key (unsigned int key)
{
	unsigned int i;
	if (!HULL_KEY_OK (key)) return HP_GENERIC;
	for (i = 0; i < sizeof hull_models / sizeof hull_models[0]; i++)
		if (hull_models[i].type == (key & 255)) return hull_models[i].prof;
	return HP_GENERIC;
}

unsigned int hull_mix (unsigned int x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static float frand (unsigned int *s)
{
	*s = *s * 1664525u + 1013904223u;
	return (*s >> 8) * (1.0f / 16777216.0f);
}

static float sstep (float a, float b, float x)
{
	float t = (x - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

static float clampf (float x, float a, float b)
{
	return x < a ? a : x > b ? b : x;
}

static int imin4 (int a, int b, int c, int d)
{
	int m = a < b ? a : b;
	if (c < m) m = c;
	return d < m ? d : m;
}

/* the fields of the tile being made; N is a power of two */
static int N, NM;
static float SC;			/* N / 1024: lengths below are in 1024ths */
static float *Hh, *AL, *CV, *HZ, *GS, *CH;
#define IX(x, y)	((((y) & NM) * N) + ((x) & NM))
#define S(v)		((int) ((v) * SC + 0.5f))

/* Tileable value noise: a g x g lattice of random values, smooth between */
static float lattice (int x, int y, unsigned int seed)
{
	return (hull_mix ((unsigned int) (x * 73856093) ^ (unsigned int) (y * 19349663) ^ seed) >> 8) * (1.0f / 16777216.0f);
}

static float vnoise (float u, float v, int gx, int gy, unsigned int seed)
{
	float x = u * gx, y = v * gy, fx = x - floorf (x), fy = y - floorf (y);
	int ix = (int) floorf (x) % gx, iy = (int) floorf (y) % gy;
	int jx = (ix + 1) % gx, jy = (iy + 1) % gy;
	float a = lattice (ix, iy, seed), b = lattice (jx, iy, seed);
	float c = lattice (ix, jy, seed), d = lattice (jx, jy, seed);
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static float fbm (float u, float v, int g, unsigned int seed)
{
	float sum = 0, amp = 0.5f, norm = 0;
	int o;
	for (o = 0; o < 4; o++, g *= 2, amp *= 0.5f) {
		sum += amp * vnoise (u, v, g, g, seed + o * 977);
		norm += amp;
	}
	return sum / norm;
}

/* ---- layout ---- */

struct Panel { int x, y, w, h; };
#define MAX_PANELS	4096
static struct Panel panels[MAX_PANELS];
static int npanels;
static unsigned int lay;

static void add_panel (int x, int y, int w, int h)
{
	if (npanels >= MAX_PANELS) return;
	panels[npanels].x = x; panels[npanels].y = y;
	panels[npanels].w = w; panels[npanels].h = h;
	npanels++;
}

/* Level 1: rows of plates, staggered, like the plating of the first
 * style but with a longer run of plate widths */
static void layout_rows (void)
{
	int rows = 4, rh = N / rows, r;
	for (r = 0; r < rows; r++) {
		int off = hull_mix (r + 1) % N, pos = 0, k = 0;
		while (pos < N) {
			int w = (int) (N * (0.19f + 0.0625f * (hull_mix (r * 64 + k++) % 4)));
			if (N - pos < w + N / 6) w = N - pos;
			add_panel (off + pos, r * rh, w, rh);
			pos += w;
		}
	}
}

/* Level 2: irregular panels, a kd-tree split of the tile */
static void layout_kd (int x, int y, int w, int h, const struct HullProfile *pr, int depth)
{
	int minw = (int) (pr->pmin * N), maxw = (int) (pr->pmax * N), vertical, c;
	int can_x = w >= 2 * minw, can_y = h >= 2 * minw;
	int big = w > maxw || h > maxw;
	float f;
	if (npanels >= MAX_PANELS - 2) can_x = can_y = 0;
	if ((!can_x && !can_y) || depth > 10 || (!big && frand (&lay) < 0.35f)) {
		add_panel (x, y, w, h);
		return;
	}
	if (can_x && can_y)
		vertical = w > h * 1.3f ? 1 : h > w * 1.3f ? 0 : frand (&lay) < 0.5f;
	else
		vertical = can_x;
	f = 0.32f + 0.36f * frand (&lay);
	if (vertical) {
		c = (int) (w * f);
		if (c < minw) c = minw;
		if (w - c < minw) c = w - minw;
		layout_kd (x, y, c, h, pr, depth + 1);
		layout_kd (x + c, y, w - c, h, pr, depth + 1);
	} else {
		c = (int) (h * f);
		if (c < minw) c = minw;
		if (h - c < minw) c = h - minw;
		layout_kd (x, y, w, c, pr, depth + 1);
		layout_kd (x, y + c, w, h - c, pr, depth + 1);
	}
}

/* ---- painting ---- */

/* A rectangle raised by dh (negative: sunk) with a sloped edge of width
 * bevel, its albedo scaled by al and its gloss pulled towards gl. */
static void box (int x0, int y0, int w, int h, float dh, float bevel, float al, float gl)
{
	int x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			float d = imin4 (x, w - 1 - x, y, h - 1 - y) + 0.5f;
			float s = sstep (0, bevel * SC, d);
			int i = IX (x0 + x, y0 + y);
			Hh[i] += dh * s;
			AL[i] *= 1 + (al - 1) * s;
			if (gl >= 0) GS[i] += (gl - GS[i]) * s;
		}
}

/* a round bolt head or rivet */
static void dome (float cx, float cy, float r, float hgt, float al)
{
	int x, y, R = (int) ceilf (r) + 1;
	for (y = -R; y <= R; y++)
		for (x = -R; x <= R; x++) {
			float dx = x - (cx - floorf (cx)), dy = y - (cy - floorf (cy));
			float u = 1 - (dx * dx + dy * dy) / (r * r);
			int i = IX ((int) floorf (cx) + x, (int) floorf (cy) + y);
			if (u <= 0) continue;
			Hh[i] += hgt * sqrtf (u);
			AL[i] *= 1 + (al - 1) * u;
			GS[i] += (0.8f - GS[i]) * u;
		}
}

/* a pipe: a half-round bar along x or y */
static void pipe (int x0, int y0, int len, int r, int horiz, float hgt, unsigned int *rs)
{
	int a, b, clamps = 0;
	for (a = 0; a < len; a++)
		for (b = -r; b <= r; b++) {
			float u = (float) b / r, hh = hgt * sqrtf (1 - u * u);
			int i = horiz ? IX (x0 + a, y0 + b) : IX (x0 + b, y0 + a);
			if (a % S (30) < S (3)) hh *= 1.5f, clamps = 1;
			Hh[i] += hh;
			AL[i] *= 0.9f + 0.2f * (1 - fabsf (u)) + (a % S (30) < S (3) ? 0.15f : 0);
			GS[i] += (0.75f - GS[i]) * (1 - fabsf (u));
		}
	(void) clamps; (void) rs;
}

static void rivets (const struct Panel *pl)
{
	int sp = S (18), in = S (6), k;
	for (k = S (10); k < pl->w - S (10); k += sp) {
		dome (pl->x + k, pl->y + in, 2.4f * SC, 1.3f, 1.12f);
		dome (pl->x + k, pl->y + pl->h - 1 - in, 2.4f * SC, 1.3f, 1.12f);
	}
	for (k = S (10); k < pl->h - S (10); k += sp) {
		dome (pl->x + in, pl->y + k, 2.4f * SC, 1.3f, 1.12f);
		dome (pl->x + pl->w - 1 - in, pl->y + k, 2.4f * SC, 1.3f, 1.12f);
	}
}

static void hatch (const struct Panel *pl, unsigned int *rs)
{
	int mn = pl->w < pl->h ? pl->w : pl->h, m = (int) (0.16f * mn) > S (8) ? (int) (0.16f * mn) : S (8);
	int hx = pl->x + m, hy = pl->y + m, hw = pl->w - 2 * m, hh = pl->h - 2 * m;
	int hand = hw > hh, k;
	/* the gap round the door, the door itself, bolts, a handle, a hinge */
	box (hx - S (2), hy - S (2), hw + S (4), hh + S (4), -2.0f, 1.5f, 0.5f, 0.3f);
	box (hx, hy, hw, hh, 1.0f, 2.0f, 1.0f + 0.12f * frand (rs), 0.6f);
	for (k = 0; k < 4; k++)
		dome (hx + ((k & 1) ? hw - S (7) : S (7)), hy + ((k & 2) ? hh - S (7) : S (7)), 2.2f * SC, 1.4f, 1.1f);
	if (hand)
		box (hx + hw - S (30), hy + hh / 2 - S (3), S (20), S (6), 1.6f, 1.2f, 1.25f, 0.9f);
	else
		box (hx + hw / 2 - S (3), hy + hh - S (30), S (6), S (20), 1.6f, 1.2f, 1.25f, 0.9f);
	if (hand)
		box (hx + S (3), hy + hh / 4, S (2), hh / 2, -1.0f, 0.8f, 0.5f, -1);
	else
		box (hx + hw / 4, hy + S (3), hw / 2, S (2), -1.0f, 0.8f, 0.5f, -1);
}

/* a louvred grille: slats with dark slits between them */
static void vent (const struct Panel *pl, unsigned int *rs)
{
	int mn = pl->w < pl->h ? pl->w : pl->h, m = (int) (0.14f * mn) > S (7) ? (int) (0.14f * mn) : S (7);
	int vx = pl->x + m, vy = pl->y + m, vw = pl->w - 2 * m, vh = pl->h - 2 * m;
	int horiz = frand (rs) < 0.6f, per = S (7 + (int) (frand (rs) * 3)), x, y;
	box (vx - S (2), vy - S (2), vw + S (4), vh + S (4), -1.5f, 1.5f, 0.6f, 0.3f);
	for (y = 0; y < vh; y++)
		for (x = 0; x < vw; x++) {
			int a = horiz ? y : x, i = IX (vx + x, vy + y);
			float ph = (a % per) / (float) per;
			float edge = sstep (0, 3 * SC, (float) imin4 (x, vw - 1 - x, y, vh - 1 - y));
			if (ph < 0.58f) {
				float r = sinf (3.14159f * ph / 0.58f);
				Hh[i] += (1.8f * r - 2.6f) * edge;
				AL[i] *= 1 - 0.2f * edge * (1 - r);
				GS[i] += (0.7f - GS[i]) * edge * r;
			} else {
				Hh[i] += -4.0f * edge;
				AL[i] *= 1 - 0.7f * edge;
				CV[i] *= 1 - 0.55f * edge;
			}
		}
}

/* a diagonal warning band along one edge of the panel */
static void hazard (const struct Panel *pl)
{
	int mn = pl->w < pl->h ? pl->w : pl->h;
	int bh = (int) (0.3f * mn), x, y, top = (pl->x + pl->y) & 1;
	int per = S (10), bx = pl->x + S (6), bw = pl->w - S (12), by;
	if (bh < S (10)) bh = S (10);
	if (bh > S (24)) bh = S (24);
	by = top ? pl->y + S (6) : pl->y + pl->h - S (6) - bh;
	box (bx - S (2), by - S (2), bw + S (4), bh + S (4), -1.2f, 1.2f, 0.7f, -1);
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++) {
			float u = fmodf ((float) (x + y), (float) (2 * per));
			float tri = fabsf (u - per) / per, st = sstep (0.45f, 0.55f, tri);
			float edge = sstep (0, 2 * SC, (float) imin4 (x, bw - 1 - x, y, bh - 1 - y));
			int i = IX (bx + x, by + y);
			HZ[i] = st * edge;
			AL[i] *= 1 + (-0.35f + 0.65f * st - 0.3f) * edge * 0.0f + (st ? 0 : 0);
			AL[i] *= 1 + edge * (st * 0.3f - (1 - st) * 0.45f);
		}
}

/* small raised boxes and pipes */
static void greebles (const struct Panel *pl, unsigned int *rs)
{
	int k, cnt = 3 + (int) (frand (rs) * 6);
	for (k = 0; k < cnt; k++) {
		int mnw = pl->w - S (24), mnh = pl->h - S (24);
		int bw, bh, bx, by;
		if (mnw < S (14) || mnh < S (14)) return;
		if (frand (rs) < 0.25f) {
			int r = S (3 + (int) (frand (rs) * 2)), len = (int) (mnw * (0.3f + 0.5f * frand (rs)));
			int horiz = frand (rs) < 0.5f;
			if (!horiz) len = (int) (mnh * (0.3f + 0.5f * frand (rs)));
			bx = pl->x + S (12) + (int) (frand (rs) * (horiz ? mnw - len : mnw - 2 * r));
			by = pl->y + S (12) + (int) (frand (rs) * (horiz ? mnh - 2 * r : mnh - len));
			pipe (bx + (horiz ? 0 : r), by + (horiz ? r : 0), len, r, horiz, 2.6f, rs);
		} else {
			bw = S (6) + (int) (frand (rs) * 0.2f * mnw);
			bh = S (6) + (int) (frand (rs) * 0.2f * mnh);
			if (bw > mnw) bw = mnw;
			if (bh > mnh) bh = mnh;
			bx = pl->x + S (12) + (int) (frand (rs) * (mnw - bw));
			by = pl->y + S (12) + (int) (frand (rs) * (mnh - bh));
			box (bx, by, bw, bh, 1.5f + 3.0f * frand (rs), 1.6f, 0.92f + 0.2f * frand (rs), 0.55f);
		}
	}
}

static void paint_panel (const struct Panel *pl, const struct HullProfile *pr, int level, unsigned int *rs)
{
	int w = pl->w, h = pl->h, x, y, mn = w < h ? w : h;
	float tone = 1 + pr->contrast * (2 * frand (rs) - 1);
	float gl0 = 0.45f + 0.35f * frand (rs);
	float rec = pr->recess * (0.3f + 0.7f * frand (rs)) * 2.5f;
	float plate = level < 2 ? 0.0f : (frand (rs) < 0.3f ? 0.9f : -rec);
	float pick = frand (rs), c0, c1, c2, c3;
	/* the plate: a groove round its edge, rising to its own level */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			float d = (float) imin4 (x, w - 1 - x, y, h - 1 - y) / SC;
			float s = sstep (1.5f, 4.0f, d);
			int i = IX (pl->x + x, pl->y + y);
			Hh[i] = -4.0f + (plate + 4.0f) * s;
			AL[i] = tone * (0.55f + 0.45f * sstep (0.5f, 3.0f, d));
			GS[i] = gl0 * (0.3f + 0.7f * s);
		}
	if (level >= 2) {
		c0 = pr->hatch;
		c1 = c0 + pr->vent;
		c2 = c1 + pr->hazard;
		c3 = c2 + pr->greeble;
		if (pick < c0) { if (mn >= S (56)) hatch (pl, rs); }
		else if (pick < c1) { if (w >= S (48) && h >= S (48)) vent (pl, rs); }
		else if (pick < c2) { if (mn >= S (28)) hazard (pl); }
		else if (pick < c3) greebles (pl, rs);
	}
	if (level < 2 ? frand (rs) < 0.9f : frand (rs) < pr->rivet)
		rivets (pl);
}

/* ---- maps ---- */

/* a box blur of radius r on the torus */
static void blur (const float *src, float *dst, float *tmp, int r)
{
	int x, y, k;
	for (y = 0; y < N; y++) {
		float sum = 0;
		for (k = -r; k <= r; k++) sum += src[IX (k, y)];
		for (x = 0; x < N; x++) {
			tmp[y * N + x] = sum / (2 * r + 1);
			sum += src[IX (x + r + 1, y)] - src[IX (x - r, y)];
		}
	}
	for (x = 0; x < N; x++) {
		float sum = 0;
		for (k = -r; k <= r; k++) sum += tmp[IX (x, k)];
		for (y = 0; y < N; y++) {
			dst[y * N + x] = sum / (2 * r + 1);
			sum += tmp[IX (x, y + r + 1)] - tmp[IX (x, y - r)];
		}
	}
}

static unsigned char byte (float v)
{
	v = v * 255.0f + 0.5f;
	return v < 0 ? 0 : v > 255 ? 255 : (unsigned char) v;
}

void hull_bake (int prof, int level, int n, unsigned char *t0, unsigned char *t1, float *mean_grime)
{
	const struct HullProfile *pr = &hull_profiles[prof];
	size_t cnt = (size_t) n * n, i;
	float *Hb, *tmp, *GR, *cav, sum = 0, gsum = 0, k;
	unsigned int rs, seed = pr->seed + level * 7919;
	int x, y, p, scratches;

	N = n; NM = n - 1; SC = n / 1024.0f;
	Hh = malloc (cnt * sizeof *Hh); AL = malloc (cnt * sizeof *AL);
	CV = malloc (cnt * sizeof *CV); HZ = calloc (cnt, sizeof *HZ);
	GS = malloc (cnt * sizeof *GS); CH = calloc (cnt, sizeof *CH);
	Hb = malloc (cnt * sizeof *Hb); tmp = malloc (cnt * sizeof *tmp);
	GR = calloc (cnt, sizeof *GR); cav = malloc (cnt * sizeof *cav);
	if (!Hh || !AL || !CV || !HZ || !GS || !CH || !Hb || !tmp || !GR || !cav) {
		memset (t0, 128, cnt * 4); memset (t1, 0, cnt * 4);
		*mean_grime = 0;
		goto out;
	}
	for (i = 0; i < cnt; i++) { Hh[i] = 0; AL[i] = 1; CV[i] = 1; GS[i] = 0.5f; }

	npanels = 0;
	lay = hull_mix (seed);
	if (level < 2) layout_rows ();
	else layout_kd (0, 0, N, N, pr, 0);
	for (p = 0; p < npanels; p++) {
		rs = hull_mix (seed + 31 * p);
		paint_panel (&panels[p], pr, level, &rs);
	}

	/* fine grain in the paint and a little tooth in the surface */
	for (i = 0; i < cnt; i++) {
		unsigned int h = hull_mix ((unsigned int) i + seed);
		AL[i] *= 1 + 0.02f * (((h & 255) / 127.5f) - 1);
		Hh[i] += 0.12f * ((((h >> 8) & 255) / 127.5f) - 1);
	}

	/* cavity: how far below its surroundings a texel sits */
	blur (Hh, Hb, tmp, S (12));
	for (i = 0; i < cnt; i++)
		cav[i] = clampf (1 - 0.17f * (Hb[i] - Hh[i]), 0.3f, 1.0f) * CV[i];

	if (level >= 2) {
		/* grime: stains, streaks that run down the tile, and dirt that
		 * collects in the cavities */
		for (y = 0; y < N; y++)
			for (x = 0; x < N; x++) {
				float u = (float) x / N, v = (float) y / N;
				float stain = fbm (u, v, 5, seed), streak = vnoise (u, v, 80, 2, seed + 5);
				float g = 0.9f * sstep (0.5f, 0.85f, stain)
					+ 0.8f * sstep (0.6f, 0.9f, streak) * (0.3f + stain)
					+ 1.1f * (1 - cav[y * N + x]);
				GR[y * N + x] = clampf (pr->wear * g, 0, 1);
			}
		/* paint chipped off convex edges, and scratches */
		for (y = 0; y < N; y++)
			for (x = 0; x < N; x++) {
				float gx = (Hh[IX (x + 1, y)] - Hh[IX (x - 1, y)]) * 0.5f * SC;
				float gy = (Hh[IX (x, y + 1)] - Hh[IX (x, y - 1)]) * 0.5f * SC;
				unsigned int h = hull_mix ((unsigned int) (y * N + x) + seed * 3);
				i = (size_t) y * N + x;
				if (fabsf (gx) + fabsf (gy) > 1.0f && Hh[i] > Hb[i] &&
				    (h & 255) / 255.0f < 0.25f + 0.6f * pr->wear)
					CH[i] = 1;
			}
		scratches = (int) (160 * pr->wear * SC * SC);
		rs = hull_mix (seed + 99);
		for (p = 0; p < scratches; p++) {
			float px = frand (&rs) * N, py = frand (&rs) * N, a = frand (&rs) * 6.2832f;
			int len = S (15) + (int) (frand (&rs) * S (55)), t;
			for (t = 0; t < len; t++) {
				int xx = (int) (px + cosf (a) * t), yy = (int) (py + sinf (a) * t);
				CH[IX (xx, yy)] = 0.8f;
				Hh[IX (xx, yy)] -= 0.35f;
			}
		}
	}

	/* mean of albedo x cavity -> 0.5, so the shader's x2 averages 1.0 */
	for (i = 0; i < cnt; i++) sum += AL[i] * cav[i];
	k = 0.5f / (sum / cnt);
	for (i = 0; i < cnt; i++) {
		float sx, sy;
		int xx = (int) (i % N), yy = (int) (i / N);
		sx = -(Hh[IX (xx + 1, yy)] - Hh[IX (xx - 1, yy)]) * 0.5f * SC;
		sy = -(Hh[IX (xx, yy + 1)] - Hh[IX (xx, yy - 1)]) * 0.5f * SC;
		t0[i * 4 + 0] = byte (clampf (sx, -2, 2) * 0.25f + 0.5f);
		t0[i * 4 + 1] = byte (clampf (sy, -2, 2) * 0.25f + 0.5f);
		t0[i * 4 + 2] = byte (clampf (AL[i] * k, 0, 1));
		t0[i * 4 + 3] = byte (cav[i]);
		t1[i * 4 + 0] = byte (HZ[i]);
		t1[i * 4 + 1] = byte (clampf (GS[i] * (1 - 0.8f * GR[i]) + 0.9f * CH[i], 0, 1));
		t1[i * 4 + 2] = byte (GR[i]);
		t1[i * 4 + 3] = byte (CH[i]);
		gsum += GR[i];
	}
	*mean_grime = gsum / cnt;
out:
	free (Hh); free (AL); free (CV); free (HZ); free (GS); free (CH);
	free (Hb); free (tmp); free (GR); free (cav);
}

/* ---- the frame of one polygon ---- */

static double dot3 (const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3 (const double *a, const double *b, double *o)
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static int unit3 (double *a)
{
	double l = sqrt (dot3 (a, a));
	if (!(l > 0)) return 0;
	a[0] /= l; a[1] /= l; a[2] /= l;
	return 1;
}

int hull_frame (const double p[][3], int n, const double light[3], unsigned int seed,
		struct HullFrame *f)
{
	double a[3], b[3], N3[3], T[3], B[3], d[3], c[3] = { 0, 0, 0 }, L[3], m[3], e[3], t;
	double lmax = 0, l, tile, ox, oy, near = 1e30;
	int i, j, k;

	for (i = 0; i < n; i++) {
		for (l = 0, k = 0; k < 3; k++)
			l += (p[(i+1) % n][k] - p[i][k]) * (p[(i+1) % n][k] - p[i][k]);
		if (l > lmax) lmax = l;
		for (k = 0; k < 3; k++) c[k] += p[i][k] / n;
	}
	/* the normal: two edges of a triangle, the diagonals of a quad */
	for (k = 0; k < 3; k++) {
		a[k] = p[n == 4 ? 2 : 1][k] - p[0][k];
		b[k] = p[n == 4 ? 3 : 2][k] - p[n == 4 ? 1 : 0][k];
		T[k] = p[1][k] - p[0][k];
	}
	cross3 (a, b, N3);
	if (!(lmax > 1) || !(sqrt (dot3 (N3, N3)) > 1e-6 * lmax) || !(sqrt (dot3 (T, T)) > 1e-3 * sqrt (lmax)))
		return 0;
	unit3 (N3);
	if (dot3 (N3, p[0]) > 0) for (k = 0; k < 3; k++) N3[k] = -N3[k];	/* towards the eye */
	if (!unit3 (T)) return 0;
	cross3 (N3, T, B);
	if (!unit3 (B)) return 0;
	/* this face's own turn and offset on the tile (the 8 ways round a square) */
	if (seed & 1) for (k = 0; k < 3; k++) { t = T[k]; T[k] = B[k]; B[k] = t; }
	if (seed & 2) for (k = 0; k < 3; k++) T[k] = -T[k];
	if (seed & 4) for (k = 0; k < 3; k++) B[k] = -B[k];
	ox = ((seed >> 3) & 1023) / 1024.0;
	oy = ((seed >> 13) & 1023) / 1024.0;
	tile = ldexp (1.0, (int) floor (log2 (0.77 * sqrt (lmax))));

	for (i = 0; i < n; i++) {
		for (k = 0; k < 3; k++) d[k] = p[i][k] - p[0][k];
		f->uv[i][0] = (float) (dot3 (d, T) / tile + ox);
		f->uv[i][1] = (float) (dot3 (d, B) / tile + oy);
		for (k = 0; k < 3; k++) d[k] = -p[i][k];
		if (!unit3 (d)) { d[0] = d[1] = 0; d[2] = 1; }
		f->vt[i][0] = (float) dot3 (d, T);
		f->vt[i][1] = (float) dot3 (d, B);
		f->vt[i][2] = (float) dot3 (d, N3);
		for (j = 0; j < 4; j++) f->ed[i][j] = 1000.0f;
	}
	/* the distance of each vertex from each edge's line, inward positive:
	 * it varies linearly over the polygon, so interpolating it is exact */
	for (j = 0; j < n; j++) {
		for (k = 0; k < 3; k++) e[k] = p[(j+1) % n][k] - p[j][k];
		cross3 (N3, e, m);
		if (!unit3 (m)) continue;
		for (k = 0; k < 3; k++) d[k] = c[k] - p[j][k];
		if (dot3 (m, d) < 0) for (k = 0; k < 3; k++) m[k] = -m[k];
		t = dot3 (m, d) / tile;
		if (t < near) near = t;
		for (i = 0; i < n; i++) {
			for (k = 0; k < 3; k++) d[k] = p[i][k] - p[j][k];
			t = dot3 (m, d) / tile;
			f->ed[i][j] = (float) (t < 0 ? 0 : t);
		}
	}
	f->inr = (float) near;
	for (k = 0; k < 3; k++) L[k] = light[k];
	if (!unit3 (L)) { L[0] = L[1] = 0; L[2] = 1; }
	f->lt[0] = (float) dot3 (L, T);
	f->lt[1] = (float) dot3 (L, B);
	f->lt[2] = (float) dot3 (L, N3);
	f->seed[0] = (hull_mix (seed ^ 0x9e3779b9u) & 1023) / 1024.0f;
	f->seed[1] = (hull_mix (seed ^ 0x85ebca6bu) & 1023) / 1024.0f;
	return 1;
}
