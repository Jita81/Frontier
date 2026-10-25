#!/usr/bin/env python3
"""Label-anchored hooks patcher for fe2.s (docs/05 section 4.3 #7).

Reads the user's pristine fe2.s and writes a *separate* patched copy that
carries the host-call hooks the GL renderer needs (planet surfaces, nested
z-trees, complex-polygon abort, detail boost). The user's fe2.s is never
rewritten.

Every place is found by its label, plus a line offset from that label, and
the line found there must have the SHA-256 recorded in hooks.json, so the
patch holds only the inserted/replaced lines and label anchors - never the
surrounding game code. The input must be lineage A (CR-stripped SHA-256
fa7699c1...) and the output must be lineage D (77bd74fa...): anything else
is refused, and nothing is written.

Usage: fe2s-hooks.py [--spec hooks.json] fe2.s OUTPUT
Stdlib only.
"""
import hashlib
import json
import os
import re
import sys

LABEL = re.compile(r'^\s*([A-Za-z_.][A-Za-z0-9_.]*):|^([A-Za-z_][A-Za-z0-9_]*)\s+equ\b')
LABEL_PREFIX = r'^(\s*)%s:(.*)$'


def sha(text):
    return hashlib.sha256(text.encode('latin-1')).hexdigest()


def fail(msg):
    sys.stderr.write('fe2s-hooks: %s\n' % msg)
    sys.exit(1)


def label_index(lines):
    defs = {}
    for i, line in enumerate(lines):
        m = LABEL.match(line)
        if m:
            defs.setdefault(m.group(1) or m.group(2), []).append(i)
    return defs


def patch(text, spec):
    if sha(text) != spec['input_sha256']:
        fail('input is not the pristine lineage-A fe2.s (CR-stripped SHA-256 %s expected, got %s)'
             % (spec['input_sha256'], sha(text)))
    lines = text.split('\n')
    defs = label_index(lines)

    # resolve every place on the pristine text first, then apply bottom-up so
    # earlier insertions never move a later target
    resolved = []
    for n, h in enumerate(spec['hunks']):
        where = defs.get(h['anchor'], [])
        if len(where) != 1:
            fail('hunk %d: label %s is defined %d times, expected once' % (n, h['anchor'], len(where)))
        i = where[0] + h['offset']
        if i >= len(lines) or sha(lines[i]) != h['line_sha256']:
            fail('hunk %d: the line at %s+%d is not the expected one' % (n, h['anchor'], h['offset']))
        resolved.append((i, n, h))

    for i, n, h in sorted(resolved, key=lambda r: (r[0], r[1]), reverse=True):
        op, new = h['op'], list(h['lines'])
        if op == 'insert_after':
            lines[i + 1:i + 1] = new
        elif op == 'replace':
            lines[i:i + 1] = new
        elif op == 'move_label':
            # the label moves onto the last inserted line; the instruction it
            # was on stays where it is, unlabelled
            m = re.match(LABEL_PREFIX % re.escape(h['anchor']), lines[i])
            if not m:
                fail('hunk %d: %s is not at the start of its line' % (n, h['anchor']))
            lines[i:i + 1] = new + [m.group(1) + m.group(2)]
        else:
            fail('hunk %d: unknown op %s' % (n, op))

    out = '\n'.join(lines)
    if sha(out) != spec['output_sha256']:
        fail('output SHA-256 %s is not the expected %s' % (sha(out), spec['output_sha256']))
    return out


def main(argv):
    spec_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hooks.json')
    args = argv[1:]
    if len(args) >= 2 and args[0] == '--spec':
        spec_path = args[1]
        args = args[2:]
    if len(args) != 2:
        fail('usage: fe2s-hooks.py [--spec hooks.json] fe2.s OUTPUT')
    src, dst = args
    if os.path.exists(dst) and os.path.samefile(src, dst):
        fail('refusing to rewrite the input in place')
    with open(spec_path, encoding='utf-8') as f:
        spec = json.load(f)
    with open(src, 'rb') as f:
        text = f.read().replace(b'\r', b'').decode('latin-1')
    out = patch(text, spec)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(out.encode('latin-1'))
    os.replace(tmp, dst)
    sys.stderr.write('fe2s-hooks: wrote %s (sha256 %s, %d hunks)\n'
                     % (dst, spec['output_sha256'][:16], len(spec['hunks'])))


if __name__ == '__main__':
    main(sys.argv)
