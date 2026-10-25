/*
 * Provenance (S-5): copied unchanged from remileonard/glfrontier, commit
 * 981d304 ("opengl wip"), and vendored here in a36733d.  Its licence is
 * UNKNOWN: the file as copied carries no licence or copyright notice, and
 * no grant has been obtained yet (docs/05 section 6, item 4).  Do not
 * relicense it, and do not ship a release that includes it, until the grant
 * is recorded here or the file is replaced by a clean-room reader with the
 * same API.
 */
/*
 * Minimal INI file reader, vendored as a drop-in replacement for the
 * system "libini" dependency (struct INI / ini_open / ini_close /
 * ini_next_section / ini_read_pair), which is not commonly available.
 *
 * Usage:
 *   struct INI *ini = ini_open(path);
 *   while (ini_next_section(ini, &name, &len) > 0) {
 *       while (ini_read_pair(ini, &key, &lkey, &val, &lval) > 0) {
 *           ...
 *       }
 *   }
 *   ini_close(ini);
 *
 * All returned pointers (name/key/val) point inside the internal
 * buffer owned by the struct INI and are only valid until ini_close().
 */
#ifndef GLFRONTIER_INI_H
#define GLFRONTIER_INI_H

#include <stddef.h>

struct INI;

struct INI *ini_open(const char *path);
void ini_close(struct INI *ini);

/* Returns 1 and sets name/len on success, 0 when there are no more
 * sections, negative on error. */
int ini_next_section(struct INI *ini, const char **name, size_t *len);

/* Returns 1 and sets key/lkey/val/lval on success, 0 when the
 * current section has no more pairs (the next section, if any, is
 * still pending and will be returned by the next ini_next_section()
 * call), negative on error. */
int ini_read_pair(struct INI *ini, const char **key, size_t *lkey,
		   const char **val, size_t *lval);

#endif
