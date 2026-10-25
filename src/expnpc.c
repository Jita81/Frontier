/*
 * E4 NPC personality experiment (docs/experiments/EXP-04-npc-llm.md).
 * Default OFF.
 *
 *   FE2_NPC_LLM=1            rewrite the body of comms messages
 *                            ("Message from <speaker>:" + body) in a
 *                            personality chosen from the speaker's name
 *   FE2_NPC_CACHE=<file>     keep finished rewrites across runs (one line
 *                            per entry: speaker, template, status, rewrite)
 *   FE2_NPC_STUB_DELAY_MS=N  make the stub rewriter take N ms, to show that
 *                            a slow rewriter never stalls a frame
 *   FE2_NPC_SHOW_PERSONAS=1  also log what each stub persona would say for
 *                            every new message (same checks, not drawn)
 *   FE2_NPC_API_URL=<url>    test only: send the API request to a local mock
 *                            server instead of api.anthropic.com
 *   FE2_NPC_SELFTEST=1       also run rewrites that break a game fact through
 *                            the checks and log that each one is refused
 *   FE2_NPC_PERSONA=<name>   test only: give every speaker this persona
 *                            (brusque, formal, weary, friendly)
 *   FE2_NPC_MAX_CALLS=N      make at most N API requests in this run, then
 *                            treat every further one as failed
 *   FE2_NPC_TEST_LINES=<file> test only: queue these messages (speaker TAB
 *                            body per line) at start-up, as if drawn, wrapped
 *                            at FE2_NPC_TEST_WIDTH pixels (default: the comms
 *                            panel's)
 *   FE2_NPC_LOG_PROMPTS=1    log the prompts sent to the API (never the key)
 *
 * The rewriter is claude-haiku-5-5 through the Anthropic Messages API when
 * ANTHROPIC_API_KEY is set in the environment (the key is only ever read
 * with getenv and handed to libcurl in memory), else a deterministic stub.
 *
 * The game builds the message text and calls DrawStr; nothing in fe2.s is
 * changed. The hook only ever changes the pixels of that one DrawStr call:
 *   - the body is turned into a template: numbers ("$500") become {0},
 *     {1}, ... and the cache key is (speaker, template), so the same line
 *     with another amount reuses the rewrite
 *   - a miss queues the template for a worker thread and draws the
 *     original, so the frame never waits for the rewriter
 *   - a rewrite is only accepted if it keeps every game fact: each number
 *     exactly once, no other digits or '$', every substring that the game
 *     inserted from its own string table (the crime name) verbatim, no new
 *     names, a negation if the original had one, and it fits the original's
 *     pixel width and line count (DrawStr has no bounds check below the
 *     first line). Anything else falls back to the original text
 *   - on a hit the original call still runs (its return value is kept),
 *     its pixels are put back and the rewrite is drawn in their place
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef WITH_CURL
#include <curl/curl.h>
#endif

#include "main.h"
#include "../m68000.h"
#include "../host.h"
#include "screen.h"
#include "exp.h"

#define NPC_MAX_ENTRIES	256
#define NPC_MAX_NUMS	8
#define NPC_MAX_FACTS	8
#define NPC_TXT		512
#define NPC_MODEL	"claude-haiku-5-5"
#define NPC_PANEL_W	114	/* widest comms line the game drew (the fine), for test lines */

enum { E_PENDING, E_READY, E_FAILED };
static const char *status_name[] = { "pending", "ready", "failed" };

struct entry {
	char speaker[64];
	char tmpl[NPC_TXT];	/* body with numbers as {n} */
	char rewrite[NPC_TXT];	/* accepted rewrite, same placeholders */
	int status;
	/* the instance seen first, for the worker */
	char nums[NPC_MAX_NUMS][24];
	int nnum;
	int budget_w, budget_lines;
};

int expnpc_on;
static int use_api;
static int stub_delay_ms;
static int show_personas;
static int forced_persona = -1;
static int max_calls = -1, n_calls, log_prompts;
static int self_test;
static FILE *cache_fp;
static struct entry entries[NPC_MAX_ENTRIES];
static int nentries;
static int queue[NPC_MAX_ENTRIES], q_head, q_tail;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static unsigned char screen_save[SCREENBYTES_LINE * 200];

/* main-thread cost and counts */
static unsigned long n_seen, n_hit, n_miss, n_pending, n_failed, n_nofit, n_queued;
static double hook_us_total, hook_us_max;
static unsigned long worker_done, worker_ok;
static double worker_ms_max;

static double now_us (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ------------------------------------------------------------ parsing */

struct msg {
	unsigned char c1, c2;
	char speaker[64];
	char body[NPC_TXT];	/* wraps joined with spaces */
	char tmpl[NPC_TXT];
	char nums[NPC_MAX_NUMS][24];
	int nnum;
	int budget_w, budget_lines;
};

static int text_width (const char *s, int n)
{
	int w = 0;
	while (n-- > 0 && *s) w += Font_CharWidth ((unsigned char) *s++);
	return w;
}

static int templatize (struct msg *m);

/* "\1 c1 Message from <speaker>:\r \1 c2 <body with \r wraps>\0" */
static int parse_msg (u32 adr, struct msg *m)
{
	static const char head[] = "Message from ";
	const unsigned char *p;
	int i, n, line_w, lines;

	if (adr + 2 * NPC_TXT >= MEM_SIZE) return 0;
	p = (const unsigned char *) STRam + adr;
	if (p[0] != 1) return 0;
	m->c1 = p[1];
	p += 2;
	if (strncmp ((const char *) p, head, sizeof (head) - 1)) return 0;
	p += sizeof (head) - 1;
	for (n = 0; *p != ':'; p++) {
		if (*p < 0x20 || *p > 0x7e || n >= (int) sizeof (m->speaker) - 1) return 0;
		m->speaker[n++] = *p;
	}
	m->speaker[n] = 0;
	if (!n || p[1] != '\r' || p[2] != 1) return 0;
	m->c2 = p[3];
	p += 4;
	lines = 1;
	line_w = m->budget_w = 0;
	for (n = 0; *p; p++) {
		if (n >= NPC_TXT - 1) return 0;
		if (*p == '\r') {
			if (line_w > m->budget_w) m->budget_w = line_w;
			line_w = 0;
			lines++;
			m->body[n++] = ' ';
			continue;
		}
		if (*p < 0x20 || *p > 0x7e) return 0;
		line_w += Font_CharWidth (*p);
		m->body[n++] = *p;
	}
	if (line_w > m->budget_w) m->budget_w = line_w;
	m->body[n] = 0;
	m->budget_lines = lines;
	if (!n) return 0;
	return templatize (m);
}

/* numbers in m->body, optionally with a leading '$', become placeholders */
static int templatize (struct msg *m)
{
	int i, n;

	m->nnum = 0;
	for (i = n = 0; m->body[i]; ) {
		const char *s = m->body + i;
		int len = 0;
		if (s[0] == '$' && isdigit ((unsigned char) s[1])) len = 1;
		if (isdigit ((unsigned char) s[len])) {
			while (isdigit ((unsigned char) s[len]) ||
			       ((s[len] == ',' || s[len] == '.') && isdigit ((unsigned char) s[len + 1])))
				len++;
			if (m->nnum >= NPC_MAX_NUMS || len >= 24) return 0;
			memcpy (m->nums[m->nnum], s, len);
			m->nums[m->nnum][len] = 0;
			n += snprintf (m->tmpl + n, NPC_TXT - n, "{%d}", m->nnum++);
			i += len;
		} else {
			m->tmpl[n++] = m->body[i++];
		}
		if (n >= NPC_TXT - 8) return 0;
	}
	m->tmpl[n] = 0;
	return 1;
}

/* template + numbers -> text; returns 0 if it would overflow */
static int instantiate (const char *t, char nums[][24], int nnum, char *out, int size)
{
	int n = 0;
	while (*t) {
		if (t[0] == '{' && isdigit ((unsigned char) t[1]) && t[2] == '}') {
			int k = t[1] - '0', len;
			if (k >= nnum) return 0;
			len = strlen (nums[k]);
			if (n + len >= size) return 0;
			memcpy (out + n, nums[k], len);
			n += len;
			t += 3;
		} else {
			if (n + 1 >= size) return 0;
			out[n++] = *t++;
		}
	}
	out[n] = 0;
	return 1;
}

/* greedy word wrap with the game's font; '\r' between lines.
 * Returns the number of lines, or 0 if a line or the count does not fit */
static int wrap (const char *s, int max_w, int max_lines, char *out, int size)
{
	int n = 0, lines = 1, line_w = 0, space_w = Font_CharWidth (' ');

	while (*s == ' ') s++;
	while (*s) {
		const char *e = s;
		int w;
		while (*e && *e != ' ') e++;
		w = text_width (s, e - s);
		if (w > max_w) return 0;
		if (line_w && line_w + space_w + w > max_w) {
			if (++lines > max_lines || n + 1 >= size) return 0;
			out[n++] = '\r';
			line_w = 0;
		} else if (line_w) {
			if (n + 1 >= size) return 0;
			out[n++] = ' ';
			line_w += space_w;
		}
		if (n + (e - s) >= size) return 0;
		memcpy (out + n, s, e - s);
		n += e - s;
		line_w += w;
		s = e;
		while (*s == ' ') s++;
	}
	out[n] = 0;
	return lines;
}

/* ------------------------------------------------------------- facts */

/* Substrings of the body that the game inserted from its own string table:
 * a run of whole words that, in ST RAM, is a complete string of its own
 * (control byte or NUL before it, NUL after it), but is not the whole body.
 * Capitalised words that do not start a sentence (names) are facts too. */
static int find_facts (const char *tmpl, char facts[][96], const unsigned char *ram)
{
	int starts[64], ends[64], nw = 0, i, j, nf = 0;
	const char *s = tmpl;
	char key[100];

	while (*s && nw < 64) {
		while (*s == ' ') s++;
		if (!*s) break;
		starts[nw] = s - tmpl;
		while (*s && *s != ' ') s++;
		ends[nw++] = s - tmpl;
	}
	for (i = 0; i < nw && nf < NPC_MAX_FACTS; ) {
		int found = 0;
		if (!isalpha ((unsigned char) tmpl[starts[i]])) { i++; continue; }
		for (j = nw - 1; j >= i; j--) {
			int len = ends[j] - starts[i];
			const unsigned char *p = ram, *hit;
			/* strip trailing punctuation of the last word */
			while (len > 0 && ispunct ((unsigned char) tmpl[starts[i] + len - 1])) len--;
			if (len < 4 || len > 90) continue;
			if (i == 0 && j == nw - 1) continue;	/* whole body */
			if (memchr (tmpl + starts[i], '{', len)) continue;
			memcpy (key, tmpl + starts[i], len);
			key[len] = 0;
			while ((hit = memmem (p, ram + MEM_SIZE - p, key, len))) {
				if (hit > ram && (hit[-1] < 0x20 || hit[-1] >= 0x7f) &&
				    hit + len < ram + MEM_SIZE && hit[len] == 0) {
					found = 1;
					break;
				}
				p = hit + 1;
			}
			if (found) {
				strcpy (facts[nf++], key);
				i = j + 1;
				break;
			}
		}
		if (!found) i++;
	}
	/* names: capitalised words not at a sentence start */
	for (i = 1; i < nw && nf < NPC_MAX_FACTS; i++) {
		const char *w = tmpl + starts[i];
		int len = ends[i] - starts[i], k, dup = 0;
		char prev = tmpl[ends[i - 1] - 1];
		if (!isupper ((unsigned char) w[0]) || prev == '.' || prev == '!' || prev == '?') continue;
		while (len > 0 && ispunct ((unsigned char) w[len - 1])) len--;
		if (len < 2 || len > 90) continue;
		memcpy (key, w, len);
		key[len] = 0;
		for (k = 0; k < nf; k++) if (strstr (facts[k], key)) dup = 1;
		if (!dup) strcpy (facts[nf++], key);
	}
	return nf;
}

/* --------------------------------------------------------- validation */

static const char *negations[] = { "not", "no", "never", "n't", "forbidden",
	"prohibited", "illegal", "banned", "denied", "refused", NULL };

/* is w (lower case) a whole word of s, ignoring case */
static int has_word (const char *s, const char *w)
{
	int len = strlen (w), i;

	for (; *s; s++) {
		for (i = 0; i < len && tolower ((unsigned char) s[i]) == w[i]; i++) ;
		if (i == len && !isalpha ((unsigned char) s[len]) && s[len] != '\'')
			return 1;
		while (isalpha ((unsigned char) *s) || *s == '\'') s++;
		if (!*s) break;
	}
	return 0;
}

static int has_negation (const char *s)
{
	char low[NPC_TXT];
	int i, k;
	for (i = 0; s[i] && i < NPC_TXT - 1; i++) low[i] = tolower ((unsigned char) s[i]);
	low[i] = 0;
	for (k = 0; negations[k]; k++) {
		const char *p = low;
		int len = strlen (negations[k]);
		while ((p = strstr (p, negations[k]))) {
			int before = (negations[k][1] == '\'') ||	/* "n't" ends a word */
				     p == low || !isalpha ((unsigned char) p[-1]);
			int after = !isalpha ((unsigned char) p[len]);
			if (before && after) return 1;
			p++;
		}
	}
	return 0;
}

/* the only new capitalised words allowed, and only at a sentence start */
static const char *sentence_words[] = { "A", "Again", "All", "An", "Attention",
	"Be", "Do", "Don't", "Every", "Fine", "Fly", "Hey", "Honestly", "I", "I'm",
	"It", "It's", "Just", "Kindly", "Listen", "Look", "Move", "No", "Notice",
	"Now", "Pay", "Pilot", "Please", "Regards", "Remit", "Sigh", "Sorry",
	"Thank", "Thanks", "That", "That's", "The", "This", "We", "You", "You're",
	"Your", NULL };

/* why a rewrite was rejected, or NULL */
static const char *validate (const char *r, const struct entry *e, char facts[][96], int nfacts)
{
	char text[NPC_TXT], wrapped[NPC_TXT];
	int i, k, count[NPC_MAX_NUMS] = { 0 };
	const char *p;

	if (!*r) return "empty";
	if (strlen (r) >= NPC_TXT - 32) return "too long";
	for (p = r; *p; p++) {
		if (*p < 0x20 || *p > 0x7e) return "non-ASCII or control character";
		if (*p == '{') {
			if (!isdigit ((unsigned char) p[1]) || p[2] != '}') return "stray brace";
			k = p[1] - '0';
			if (k >= e->nnum) return "unknown placeholder";
			count[k]++;
			p += 2;
			continue;
		}
		if (*p == '}') return "stray brace";
		if (isdigit ((unsigned char) *p)) return "new digit";
		if (*p == '$') return "new currency sign";
	}
	for (k = 0; k < e->nnum; k++)
		if (count[k] != 1) return "placeholder not kept exactly once";
	for (i = 0; i < nfacts; i++)
		if (!strstr (r, facts[i])) return "game fact changed";
	if (has_negation (e->tmpl) && !has_negation (r)) return "negation lost";
	/* capitalised words must be in the original or the speaker's name,
	 * or start a sentence */
	for (p = r; *p; ) {
		const char *w;
		int len, start;
		while (*p && !isalpha ((unsigned char) *p)) p++;
		if (!*p) break;
		w = p;
		while (isalpha ((unsigned char) *p) || *p == '\'') p++;
		len = p - w;
		if (!isupper ((unsigned char) w[0])) continue;
		{
			const char *b = w;
			while (b > r && (b[-1] == ' ' || b[-1] == '"' || b[-1] == '\'')) b--;
			start = (b == r || b[-1] == '.' || b[-1] == '!' || b[-1] == '?');
		}
		{
			char word[64];
			int k, known = 0;
			if (len >= 63) return "new name";
			memcpy (word, w, len);
			word[len] = 0;
			if (strstr (e->tmpl, word) || strstr (e->speaker, word)) continue;
			/* a sentence may start with a common word, never a new name */
			for (k = 0; start && sentence_words[k]; k++)
				if (!strcmp (word, sentence_words[k])) known = 1;
			/* or with a word of the original, capitalised ("Not", "Fine's") */
			if (start && !known) {
				if (len > 2 && !strcmp (word + len - 2, "'s")) word[len - 2] = 0;
				word[0] = tolower ((unsigned char) word[0]);
				known = has_word (e->tmpl, word);
			}
			if (!known) return "new name";
		}
	}
	if (!instantiate (r, (char (*)[24]) e->nums, e->nnum, text, sizeof (text))) return "too long";
	if (!wrap (text, e->budget_w, e->budget_lines, wrapped, sizeof (wrapped)))
		return "does not fit the original width and line count";
	return NULL;
}

/* FE2_NPC_SELFTEST: rewrites that break a game fact must all be refused */
static void selftest (const struct entry *e, char facts[][96], int nfacts)
{
	char bad[10][NPC_TXT], *p;
	const char *label[10];
	int n = 0, i;

	if (!instantiate (e->tmpl, (char (*)[24]) e->nums, 0, bad[0], NPC_TXT)) {
		/* the template has numbers: change, drop and double the first */
		p = strstr (e->tmpl, "{0}");
		snprintf (bad[n], NPC_TXT, "%.*s$50%s", (int) (p - e->tmpl), e->tmpl, p + 3);
		label[n++] = "amount changed";
		snprintf (bad[n], NPC_TXT, "%.*s%s", (int) (p - e->tmpl), e->tmpl, p + 3);
		label[n++] = "amount dropped";
		snprintf (bad[n], NPC_TXT, "%s Twice: {0}.", e->tmpl);
		label[n++] = "amount doubled";
	}
	if (nfacts && (p = strstr (e->tmpl, facts[0]))) {
		const char *sp = strchr (facts[0], ' ');
		snprintf (bad[n], NPC_TXT, "%.*sDocking%s%s", (int) (p - e->tmpl), e->tmpl,
			  sp ? sp : "", p + strlen (facts[0]));
		label[n++] = "crime (game string) changed";
	}
	snprintf (bad[n], NPC_TXT, "Jameson: %s", e->tmpl);
	label[n++] = "new name added";
	if ((p = strstr (e->tmpl, " not "))) {
		snprintf (bad[n], NPC_TXT, "%.*s%s", (int) (p - e->tmpl), e->tmpl, p + 4);
		label[n++] = "negation dropped";
	}
	snprintf (bad[n], NPC_TXT, "%s Please observe station regulations at all times, pilot.", e->tmpl);
	label[n++] = "longer than the original's lines";
	snprintf (bad[n], NPC_TXT, "%s\xe9", e->tmpl);
	label[n++] = "non-ASCII";
	for (i = 0; i < n; i++) {
		const char *why = validate (bad[i], e, facts, nfacts);
		fprintf (stderr, "FE2_NPC: selftest %-34s %s (%s)\n", label[i],
			 why ? "refused" : "ACCEPTED", why ? why : "-");
	}
	fprintf (stderr, "FE2_NPC: selftest original template %s\n",
		 validate (e->tmpl, e, facts, nfacts) ? "REFUSED" : "accepted");
}

/* ---------------------------------------------------------- personas */

struct persona {
	const char *name, *desc;
	const char *prefix[4], *suffix[4];
	const char *swap[8][2];
};

static const struct persona personas[] = {
	{ "brusque", "a curt, overworked traffic controller who wastes no words",
	  { "Pilot! ", "Listen. ", NULL }, { " Now.", " Move it.", NULL },
	  { { "is not permitted here", "is illegal" }, { "Pay a {0} fine.", "Fine: {0}." }, { NULL } } },
	{ "formal", "a stiff, by-the-book station official",
	  { "Attention. ", "Notice. ", NULL }, { " Thank you.", " Regards.", NULL },
	  { { "is not permitted here", "is prohibited" }, { "Pay a ", "Remit a " }, { NULL } } },
	{ "weary", "a tired night-shift operator who has seen it all before",
	  { "Again? ", "Sigh. ", NULL }, { " Every time.", " Honestly.", NULL },
	  { { "is not permitted here", "isn't allowed" }, { "Pay a {0} fine.", "That's {0}." }, { NULL } } },
	{ "friendly", "a warm, chatty dock hand who still enforces the rules",
	  { "Sorry, pilot! ", "Hey there! ", NULL }, { " Fly safe.", " No hard feelings.", NULL },
	  { { "is not permitted here", "isn't allowed" }, { "Pay a {0} fine.", "That's a {0} fine." }, { NULL } } },
};
#define NPERSONAS (int) (sizeof (personas) / sizeof (personas[0]))

static const struct persona *persona_for (const char *speaker)
{
	unsigned h = 2166136261u;
	if (forced_persona >= 0) return &personas[forced_persona];
	while (*speaker) h = (h ^ (unsigned char) *speaker++) * 16777619u;
	return &personas[h % NPERSONAS];
}

static void apply_swaps (const char *in, const struct persona *p, int mask, char *out, int size)
{
	char tmp[NPC_TXT];
	int k;
	strncpy (out, in, size - 1);
	out[size - 1] = 0;
	for (k = 0; p->swap[k][0]; k++) {
		char *hit;
		if (!(mask & (1 << k)) || !(hit = strstr (out, p->swap[k][0]))) continue;
		snprintf (tmp, sizeof (tmp), "%.*s%s%s", (int) (hit - out), out, p->swap[k][1],
			  hit + strlen (p->swap[k][0]));
		strncpy (out, tmp, size - 1);
	}
}

/* deterministic: the first variant, most flavour first, that validates */
static int stub_rewrite (struct entry *e, char facts[][96], int nfacts, char *out)
{
	const struct persona *p = persona_for (e->speaker);
	char swapped[NPC_TXT], cand[NPC_TXT];
	int nswap, mask, pi, si, npre, nsuf;

	for (nswap = 0; p->swap[nswap][0]; nswap++) ;
	for (npre = 0; p->prefix[npre]; npre++) ;
	for (nsuf = 0; p->suffix[nsuf]; nsuf++) ;
	/* prefix and suffix, then one of them, then swaps alone */
	for (pi = 0; pi <= npre; pi++)
		for (si = 0; si <= nsuf; si++)
			for (mask = (1 << nswap) - 1; mask >= 0; mask--) {
				if (pi == npre && si == nsuf && mask == 0) continue;
				apply_swaps (e->tmpl, p, mask, swapped, sizeof (swapped));
				snprintf (cand, sizeof (cand), "%s%s%s",
					  pi < npre ? p->prefix[pi] : "", swapped,
					  si < nsuf ? p->suffix[si] : "");
				if (!validate (cand, e, facts, nfacts)) {
					strcpy (out, cand);
					return 1;
				}
			}
	return 0;
}

/* -------------------------------------------------------------- API */

#ifdef WITH_CURL
struct buf { char *p; size_t n; };

static size_t on_data (void *d, size_t sz, size_t nm, void *u)
{
	struct buf *b = u;
	size_t len = sz * nm;
	char *np = realloc (b->p, b->n + len + 1);
	if (!np) return 0;
	b->p = np;
	memcpy (b->p + b->n, d, len);
	b->n += len;
	b->p[b->n] = 0;
	return len;
}

static void json_str (char *out, int size, const char *s)
{
	int n = 0;
	out[n++] = '"';
	for (; *s && n < size - 8; s++) {
		if (*s == '"' || *s == '\\') { out[n++] = '\\'; out[n++] = *s; }
		else if (*s == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
		else if ((unsigned char) *s < 0x20) n += snprintf (out + n, size - n, "\\u%04x", *s);
		else out[n++] = *s;
	}
	out[n++] = '"';
	out[n] = 0;
}

/* the first text block's text (the API returns compact JSON; a thinking
 * block may come first); non-ASCII becomes 0x7f so validate rejects it */
static int json_text (const char *resp, char *out, int size)
{
	const char *p = strstr (resp, "\"type\":\"text\"");
	int n = 0;

	if (!p || !(p = strstr (p + 13, "\"text\":\""))) return 0;
	for (p += 8; *p && *p != '"' && n < size - 1; p++) {
		if (*p == '\\') {
			p++;
			if (*p == 'n') out[n++] = ' ';
			else if (*p == 'u') { out[n++] = 0x7f; p += 4; }
			else out[n++] = *p;
		} else {
			out[n++] = ((unsigned char) *p < 0x80) ? *p : 0x7f;
		}
	}
	out[n] = 0;
	return *p == '"';
}

/* one Messages API request; sys and msgs are JSON already. The reply's
 * text goes to out, trimmed of spaces and quotes */
static int api_call (const char *sys, const char *msgs, char *out)
{
	struct curl_slist *h = NULL;
	struct buf b = { NULL, 0 };
	char hdr[512], *body;
	const char *key = getenv ("ANTHROPIC_API_KEY"), *url;
	CURL *c;
	CURLcode rc;
	long code = 0;
	int ok = 0;
	double t0;

	if (!key || !*key) return 0;
	if (max_calls >= 0 && n_calls >= max_calls) {
		fprintf (stderr, "FE2_NPC: API call cap (%d) reached, request not sent\n", max_calls);
		return 0;
	}
	if (!(c = curl_easy_init ())) return 0;
	n_calls++;
	body = malloc (32768);
	snprintf (body, 32768,
		"{\"model\":\"%s\",\"max_tokens\":256,\"thinking\":{\"type\":\"disabled\"},"
		"\"output_config\":{\"effort\":\"low\"},\"system\":%s,\"messages\":[%s]}",
		NPC_MODEL, sys, msgs);
	snprintf (hdr, sizeof (hdr), "x-api-key: %s", key);
	h = curl_slist_append (h, hdr);
	memset (hdr, 0, sizeof (hdr));
	h = curl_slist_append (h, "anthropic-version: 2026-10-21");
	h = curl_slist_append (h, "content-type: application/json");
	url = getenv ("FE2_NPC_API_URL");	/* test only: a local mock server */
	curl_easy_setopt (c, CURLOPT_URL, url && *url ? url : "https://api.anthropic.com/v1/messages");
	curl_easy_setopt (c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt (c, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt (c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt (c, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
	t0 = now_us ();
	rc = curl_easy_perform (c);
	if (rc == CURLE_OK) {
		const char *ui = NULL, *uo = NULL, *sr;
		char stop[32] = "?";
		curl_easy_getinfo (c, CURLINFO_RESPONSE_CODE, &code);
		if (b.p && (sr = strstr (b.p, "\"stop_reason\":\""))) sscanf (sr + 15, "%31[a-z_]", stop);
		if (b.p) {
			ui = strstr (b.p, "\"input_tokens\":");
			uo = strstr (b.p, "\"output_tokens\":");
		}
		fprintf (stderr, "FE2_NPC: API call %d: HTTP %ld, stop %s, %.1f ms, usage in %ld out %ld\n",
			 n_calls, code, stop, (now_us () - t0) / 1e3,
			 ui ? strtol (ui + 15, NULL, 10) : -1L, uo ? strtol (uo + 16, NULL, 10) : -1L);
		if (code == 200 && !strcmp (stop, "end_turn"))
			ok = json_text (b.p, out, NPC_TXT);
		else
			fprintf (stderr, "FE2_NPC: API status %ld, stop %s, response not used\n", code, stop);
	} else {
		fprintf (stderr, "FE2_NPC: API call %d: request failed after %.1f ms (%s)\n",
			 n_calls, (now_us () - t0) / 1e3, curl_easy_strerror (rc));
	}
	curl_slist_free_all (h);
	curl_easy_cleanup (c);
	free (body);
	free (b.p);
	if (ok) {
		char *s = out, *t;
		while (*s == ' ' || *s == '"') s++;
		memmove (out, s, strlen (s) + 1);
		for (t = out + strlen (out); t > out && (t[-1] == ' ' || t[-1] == '"'); ) *--t = 0;
	}
	return ok;
}

/* ask once; if the checker refuses the reply, ask once more with the
 * reason, then give up (the original is shown) */
static int api_rewrite (struct entry *e, char facts[][96], int nfacts, char *out)
{
	const struct persona *p = persona_for (e->speaker);
	char sys[2048], user[2048], fl[1024], again[512], js[4][4096], msgs[16384];
	const char *why = NULL;
	int i, n, try, budget = strlen (e->tmpl);	/* the lines are full already */

	for (i = n = 0, fl[0] = 0; i < nfacts; i++)
		n += snprintf (fl + n, sizeof (fl) - n, "%s\"%s\"", i ? ", " : "", facts[i]);
	snprintf (sys, sizeof (sys),
		"You rewrite one radio message in the space game Frontier: Elite II (year 3200) so it "
		"sounds like its speaker. The speaker is %s. Keep the meaning exactly: every "
		"placeholder such as {0} must appear exactly once and unchanged; copy these phrases "
		"verbatim: %s; write no digits, no currency signs and no names that are not in the "
		"message; keep any negation. No longer than the original: at most %d characters, "
		"plain ASCII, one paragraph. "
		"Reply with the rewritten message only.",
		p->desc, nfacts ? fl : "(none)", budget);
	snprintf (user, sizeof (user), "Speaker: %s\nMessage: %s", e->speaker, e->tmpl);
	json_str (js[0], sizeof (js[0]), sys);
	json_str (js[1], sizeof (js[1]), user);
	snprintf (msgs, sizeof (msgs), "{\"role\":\"user\",\"content\":%s}", js[1]);
	if (log_prompts) fprintf (stderr, "FE2_NPC: prompt system: %s\nFE2_NPC: prompt user: %s\n", sys, user);
	for (try = 0; try < 2; try++) {
		if (!api_call (js[0], msgs, out)) return 0;
		if (!(why = validate (out, e, facts, nfacts))) return 1;
		fprintf (stderr, "FE2_NPC: rewrite rejected (%s): \"%s\"%s\n", why, out,
			 try ? "" : ", asking again");
		snprintf (again, sizeof (again), "A checker refused that: %s. It is %d characters; "
			"the limit is %d. Write it again, keep every rule, and reply with the "
			"message only.%s", why, (int) strlen (out), budget, strcmp (why, "new name") ? "" :
			" Use no capitalised word that is not in the original message, except "
			"to start a sentence with one of its own words.");
		if (log_prompts && !try) fprintf (stderr, "FE2_NPC: prompt retry: %s\n", again);
		json_str (js[2], sizeof (js[2]), out);
		json_str (js[3], sizeof (js[3]), again);
		n = strlen (msgs);
		snprintf (msgs + n, sizeof (msgs) - n,
			",{\"role\":\"assistant\",\"content\":%s},{\"role\":\"user\",\"content\":%s}",
			js[2], js[3]);
	}
	return 0;
}
#endif

/* ------------------------------------------------------------ worker */

static void cache_write (const struct entry *e)
{
	if (!cache_fp) return;
	fprintf (cache_fp, "%s\t%s\t%s\t%s\n", e->speaker, e->tmpl,
		 status_name[e->status], e->status == E_READY ? e->rewrite : "");
	fflush (cache_fp);
}

static void *worker (void *arg)
{
	unsigned char *ram = malloc (MEM_SIZE);
	(void) arg;
	if (!ram) return NULL;
	for (;;) {
		struct entry job;
		char facts[NPC_MAX_FACTS][96], out[NPC_TXT];
		int idx, nfacts, ok = 0;
		double t0;

		pthread_mutex_lock (&lock);
		while (q_head == q_tail) pthread_cond_wait (&wake, &lock);
		idx = queue[q_head++ % NPC_MAX_ENTRIES];
		job = entries[idx];
		pthread_mutex_unlock (&lock);

		t0 = now_us ();
		/* the game keeps running: this copy is read without a lock; the
		 * string table it is searched for is constant data */
		memcpy (ram, STRam, MEM_SIZE);
		nfacts = find_facts (job.tmpl, facts, ram);
#ifdef WITH_CURL
		if (use_api) ok = api_rewrite (&job, facts, nfacts, out);
		else
#endif
		{
			if (stub_delay_ms) usleep (stub_delay_ms * 1000);
			ok = stub_rewrite (&job, facts, nfacts, out);
		}

		pthread_mutex_lock (&lock);
		if (ok) strcpy (entries[idx].rewrite, out);
		entries[idx].status = ok ? E_READY : E_FAILED;
		worker_done++;
		worker_ok += ok;
		if ((now_us () - t0) / 1e3 > worker_ms_max) worker_ms_max = (now_us () - t0) / 1e3;
		cache_write (&entries[idx]);
		pthread_mutex_unlock (&lock);

		if (self_test) selftest (&job, facts, nfacts);
		if (show_personas) {
			/* what every persona's stub would say, with the same checks */
			struct entry alt = job;
			int k;
			for (k = 0; k < NPERSONAS; k++) {
				char o[NPC_TXT];
				int tries;
				for (tries = 0; tries < 1000; tries++) {
					snprintf (alt.speaker, sizeof (alt.speaker), "%s#%d", job.speaker, tries);
					if (persona_for (alt.speaker) == &personas[k]) break;
				}
				if (stub_rewrite (&alt, facts, nfacts, o))
					fprintf (stderr, "FE2_NPC: persona %-8s -> \"%s\"\n", personas[k].name, o);
				else
					fprintf (stderr, "FE2_NPC: persona %-8s -> original (nothing fits)\n", personas[k].name);
			}
		}
		fprintf (stderr, "FE2_NPC: %s [%s] \"%s\" -> %s%s%s (fit %d px x %d lines; %.1f ms; facts:",
			 job.speaker, persona_for (job.speaker)->name, job.tmpl, ok ? "\"" : "original",
			 ok ? out : "", ok ? "\"" : "", job.budget_w, job.budget_lines, (now_us () - t0) / 1e3);
		for (idx = 0; idx < nfacts; idx++) fprintf (stderr, " \"%s\"", facts[idx]);
		fprintf (stderr, ")\n");
	}
	return NULL;
}

/* ------------------------------------------------------------ entries */

static void cache_load (const char *path)
{
	FILE *f = fopen (path, "r");
	char line[4 * NPC_TXT];

	if (f) {
		while (fgets (line, sizeof (line), f) && nentries < NPC_MAX_ENTRIES) {
			char *fld[4], *s = line;
			struct entry *e;
			int i, k;
			line[strcspn (line, "\n")] = 0;
			for (k = 0; k < 4; k++) {
				fld[k] = s;
				if (k < 3 && !(s = strchr (s, '\t'))) break;
				if (k < 3) *s++ = 0;
			}
			if (k < 4 || strcmp (fld[2], "ready")) continue;
			for (i = 0; i < nentries; i++)
				if (!strcmp (entries[i].speaker, fld[0]) && !strcmp (entries[i].tmpl, fld[1])) break;
			e = &entries[i];
			if (i == nentries) nentries++;
			memset (e, 0, sizeof (*e));
			strncpy (e->speaker, fld[0], sizeof (e->speaker) - 1);
			strncpy (e->tmpl, fld[1], sizeof (e->tmpl) - 1);
			strncpy (e->rewrite, fld[3], sizeof (e->rewrite) - 1);
			e->status = E_READY;
		}
		fclose (f);
		fprintf (stderr, "FE2_NPC: %d cached rewrites loaded from %s\n", nentries, path);
	}
	cache_fp = fopen (path, "a");
}

/* FE2_NPC_TEST_LINES: queue each "speaker TAB body" line as a new entry,
 * with the budget of the body wrapped at the panel width, exactly as if the
 * game had drawn it; the worker and the checks are the real ones */
static void test_lines (const char *path)
{
	FILE *f = fopen (path, "r");
	const char *s = getenv ("FE2_NPC_TEST_WIDTH");
	int width = s && *s ? atoi (s) : NPC_PANEL_W;
	char line[2 * NPC_TXT], wrapped[NPC_TXT], *tab;

	if (!f) return;
	while (fgets (line, sizeof (line), f) && nentries < NPC_MAX_ENTRIES) {
		struct msg m;
		struct entry *e;
		line[strcspn (line, "\n")] = 0;
		if (line[0] == '#' || !(tab = strchr (line, '\t'))) continue;
		*tab = 0;
		memset (&m, 0, sizeof (m));
		snprintf (m.speaker, sizeof (m.speaker), "%s", line);
		snprintf (m.body, sizeof (m.body), "%s", tab + 1);
		if (!templatize (&m) || !(m.budget_lines = wrap (m.body, width, 8, wrapped, sizeof (wrapped)))) {
			fprintf (stderr, "FE2_NPC: test line not usable: %s\n", tab + 1);
			continue;
		}
		m.budget_w = width;
		e = &entries[nentries++];
		memset (e, 0, sizeof (*e));
		strcpy (e->speaker, m.speaker);
		strcpy (e->tmpl, m.tmpl);
		memcpy (e->nums, m.nums, sizeof (m.nums));
		e->nnum = m.nnum;
		e->budget_w = m.budget_w;
		e->budget_lines = m.budget_lines;
		e->status = E_PENDING;
		queue[q_tail++ % NPC_MAX_ENTRIES] = e - entries;
		n_queued++;
	}
	fclose (f);
}

void ExpNpc_Init (void)
{
	const char *s;
	pthread_t t;

	if (!(s = getenv ("FE2_NPC_LLM")) || !*s || !strcmp (s, "0")) return;
	expnpc_on = 1;
#ifdef WITH_CURL
	use_api = (s = getenv ("ANTHROPIC_API_KEY")) && *s;
	if (use_api) curl_global_init (CURL_GLOBAL_DEFAULT);
#endif
	if ((s = getenv ("FE2_NPC_STUB_DELAY_MS"))) stub_delay_ms = atoi (s);
	show_personas = (s = getenv ("FE2_NPC_SHOW_PERSONAS")) && *s && strcmp (s, "0");
	self_test = (s = getenv ("FE2_NPC_SELFTEST")) && *s && strcmp (s, "0");
	if ((s = getenv ("FE2_NPC_PERSONA")))
		for (forced_persona = NPERSONAS - 1; forced_persona >= 0; forced_persona--)
			if (!strcmp (s, personas[forced_persona].name)) break;
	if ((s = getenv ("FE2_NPC_MAX_CALLS")) && *s) max_calls = atoi (s);
	log_prompts = (s = getenv ("FE2_NPC_LOG_PROMPTS")) && *s && strcmp (s, "0");
	if ((s = getenv ("FE2_NPC_CACHE")) && *s) cache_load (s);
	if ((s = getenv ("FE2_NPC_TEST_LINES")) && *s) test_lines (s);
	pthread_create (&t, NULL, worker, NULL);
	pthread_detach (t);
	fprintf (stderr, "FE2_NPC: on, rewriter %s\n",
		 use_api ? NPC_MODEL " (Anthropic API)" : "deterministic stub");
}

/* in place of DrawStr/DrawStrShadowed; returns 0 to let the caller run the
 * original host call unchanged */
int ExpNpc_DrawStr (HOSTCALL orig, int shadowed)
{
	struct msg m;
	struct entry *e = NULL;
	char text[NPC_TXT], wrapped[NPC_TXT], str[NPC_TXT + 160];
	int i, x, y, col, d1, drawn = 0, status = -1;
	double t0 = now_us (), dt;

	if (!parse_msg (GetReg (REG_A0), &m)) return 0;
	n_seen++;
	pthread_mutex_lock (&lock);
	for (i = 0; i < nentries; i++)
		if (!strcmp (entries[i].speaker, m.speaker) && !strcmp (entries[i].tmpl, m.tmpl)) {
			e = &entries[i];
			break;
		}
	if (!e && nentries < NPC_MAX_ENTRIES) {
		e = &entries[nentries++];
		memset (e, 0, sizeof (*e));
		strcpy (e->speaker, m.speaker);
		strcpy (e->tmpl, m.tmpl);
		memcpy (e->nums, m.nums, sizeof (m.nums));
		e->nnum = m.nnum;
		e->budget_w = m.budget_w;
		e->budget_lines = m.budget_lines;
		e->status = E_PENDING;
		queue[q_tail++ % NPC_MAX_ENTRIES] = e - entries;
		n_queued++;
		n_miss++;
		pthread_cond_signal (&wake);
		e = NULL;
	} else if (e) {
		status = e->status;
		if (status == E_READY) {
			/* budgets of this instance (a cached entry has none yet) */
			e->budget_w = m.budget_w;
			e->budget_lines = m.budget_lines;
			if (instantiate (e->rewrite, m.nums, m.nnum, text, sizeof (text)) &&
			    wrap (text, m.budget_w, m.budget_lines, wrapped, sizeof (wrapped)))
				drawn = 1;
			else
				n_nofit++;
		}
	}
	pthread_mutex_unlock (&lock);
	if (status == E_PENDING) n_pending++;
	if (status == E_FAILED) n_failed++;

	if (drawn) {
		n_hit++;
		snprintf (str, sizeof (str), "\001%cMessage from %s:\r\001%c%s",
			  m.c1, m.speaker, m.c2, wrapped);
		col = GetReg (REG_D0);
		x = GetReg (REG_D1);
		y = GetReg (REG_D2);
		/* the original call, for its return value; its pixels go back */
		memcpy (screen_save, LOGSCREEN2, sizeof (screen_save));
		orig ();
		d1 = GetReg (REG_D1);
		memcpy (LOGSCREEN2, screen_save, sizeof (screen_save));
		DrawStr (x, y, col, (unsigned char *) str, shadowed);
		SetReg (REG_D1, d1);
	}
	dt = now_us () - t0;
	hook_us_total += dt;
	if (dt > hook_us_max) hook_us_max = dt;
	return drawn;
}

void ExpNpc_Stats (FILE *f)
{
	pthread_mutex_lock (&lock);
	fprintf (f, "FE2_NPC: messages %lu: rewritten %lu, first sight (queued) %lu, "
		 "still pending %lu, rejected %lu, cached but no fit %lu; "
		 "worker %lu done %lu accepted, slowest %.1f ms; "
		 "hook on main thread: total %.0f us, max %.0f us\n",
		 n_seen, n_hit, n_queued, n_pending, n_failed, n_nofit,
		 worker_done, worker_ok, worker_ms_max, hook_us_total, hook_us_max);
	pthread_mutex_unlock (&lock);
}
