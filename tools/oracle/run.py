#!/usr/bin/env python3
"""Game-intact oracle driver (docs/07-game-intact-oracle.md).

Standard library only.  Sub-commands:

  lint TAPE...                 fe2rep 1 lint rules (07 section 6)
  filter FILE...               output contains only tags, decimal VBLs, hex (07 4.3)
  rundir DIR [--saves D]       build a run directory (07 3.6)
  replay --bin B --tape T --out O [--args "..."] [--detail A:B]
                               replay a tape in a fresh run directory
  compare A B                  pairwise: IDENTICAL / DIVERGENT-GAME /
                               DIVERGENT-FRAME / INVALID, with the report (7.2)
  verdict --baseline G RUN1 RUN2
                               PASS / FAIL-GAME / FAIL-FRAME / FAIL-NONDET /
                               ERROR-INFRA (07 7.1)
  diverge --tape T --bin-a B [--args-a ..] --bin-b B [--args-b ..] --work D
                               replay under two builds or flag sets, compare,
                               and on divergence re-run with --oracle-detail
                               v-5:v+1 to name the first divergent hcall (7.2)

Nothing here reads or prints game bytes: stream files hold only tags,
VBL indices and hashes, and the run directory is a temporary directory
that holds a symlink to fe2.s.bin, never a copy.
"""
import argparse
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

GAME_STREAMS = "gmcsrw"      # differences here are FAIL-GAME
FRAME_STREAMS = "fpd"        # differences here (only) are FAIL-FRAME
STREAM_NAMES = {
    "g": "G-trace", "m": "RAM", "c": "checkpoints", "s": "S_save",
    "r": "Fread", "w": "watchdog", "f": "fb8", "p": "fbp", "d": "dl",
}
COUNTERS_GAME = ("flagged", "hostupdate", "wd", "wdv", "keyovf", "a6",
                 "a6chg", "draws", "screens", "idle")


# --------------------------------------------------------------------------
# hcall names, read from the host source (no game data)

def hcall_names(path=None, defines=("WITH_GL",)):
    path = path or os.path.join(REPO, "src", "hostcall.c")
    try:
        with open(path, encoding="latin-1") as f:
            src = f.read()
    except OSError:
        return {}
    m = re.search(r"HOSTCALL\s+hcalls\s*\[\s*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return {}
    body = m.group(1)
    names, stack = [], []
    for raw in body.split("\n"):
        line = re.sub(r"/\*.*?\*/", "", raw).strip()
        if line.startswith("#"):
            d = line[1:].strip().split()
            if not d:
                continue
            if d[0] == "ifdef":
                stack.append(d[1] in defines)
            elif d[0] == "ifndef":
                stack.append(d[1] not in defines)
            elif d[0] == "else" and stack:
                stack[-1] = not stack[-1]
            elif d[0] == "endif" and stack:
                stack.pop()
            continue
        if not all(stack):
            continue
        for item in line.split(","):
            item = item.strip()
            if item:
                names.append(item.lstrip("&"))
    return {i: n for i, n in enumerate(names)}


# --------------------------------------------------------------------------
# tape lint (07 section 6); mirrors tape_load() in src/oracle.c

HEADER_RE = re.compile(
    r"^fe2rep 1 mode=(soft|gl-rgl|gl-rold) seed=(-?\d+) size=(\d+)x(\d+) requires=(\S+)$")
KNOWN_H_ACTIONS = set()      # format 1 build: no h actions


def lint_tape(path):
    errs = []
    header = False
    last_v = -1
    keys_in_v = {}
    end_seen = None
    with open(path, encoding="ascii", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue
            if end_seen is not None:
                errs.append("line %d: record after end" % n)
                continue
            if not header:
                m = HEADER_RE.match(line)
                if not m:
                    errs.append("line %d: bad or missing fe2rep 1 header" % n)
                    return errs
                if m.group(5) != "-":
                    for req in m.group(5).split(","):
                        if req not in KNOWN_H_ACTIONS:
                            errs.append("line %d: requires %s, unknown to this build" % (n, req))
                header = True
                continue
            t = line.split()
            if t[0] not in ("k", "m", "h", "end") or len(t) < 2 or not t[1].isdigit():
                errs.append("line %d: bad record" % n)
                continue
            v = int(t[1])
            if v < last_v:
                errs.append("line %d: v decreases" % n)
            last_v = v
            if t[0] == "end":
                if len(t) != 2:
                    errs.append("line %d: bad end" % n)
                end_seen = v
            elif t[0] == "h":
                if len(t) < 3 or t[2] not in KNOWN_H_ACTIONS:
                    errs.append("line %d: h action unknown to this build" % n)
            elif t[0] == "k":
                codes = t[2:]
                if not codes:
                    errs.append("line %d: k with no keys" % n)
                for c in codes:
                    if not re.fullmatch(r"[0-9a-fA-F]{1,2}", c):
                        errs.append("line %d: bad key code %s" % (n, c))
                keys_in_v[v] = keys_in_v.get(v, 0) + len(codes)
                if keys_in_v[v] > 15:
                    errs.append("line %d: more than 15 keys in VBL %d" % (n, v))
            else:
                if len(t) != 8 or not (t[2] == "*" or t[2].isdigit()):
                    errs.append("line %d: m needs v ord dx dy ax ay buttons" % n)
                    continue
                try:
                    dx, dy, ax, ay, b = (int(x) for x in t[3:])
                except ValueError:
                    errs.append("line %d: m values must be integers" % n)
                    continue
                if not (0 <= ax <= 319 and 0 <= ay <= 199):
                    errs.append("line %d: ax/ay out of range" % n)
    if not header:
        errs.append("no header")
    if end_seen is None:
        errs.append("no end record")
    return errs


# --------------------------------------------------------------------------
# stream files

LINE_RES = [
    re.compile(r"^# fe2gold 1 mode=[a-z-]+ oracle=(replay|record) seed=-?\d+$"),
    re.compile(r"^[gmfpd] \d+ [0-9a-f]{16}$"),
    re.compile(r"^c \d+ [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{4}$"),
    re.compile(r"^s \d+ [0-9a-f]{8} [0-9a-f]{16} [0-9a-f]{16}$"),
    re.compile(r"^r \d+ [0-9a-f]{8} [0-9a-f]{16}$"),
    re.compile(r"^w \d+ hc=[0-9a-f]{2}$"),
    re.compile(r"^G \d+ \d+ [0-9a-f]{2} [ex] [0-9a-f]{16}$"),
    re.compile(r"^stat( [a-z0-9]+=[0-9a-f,:+-]+)+$"),
    re.compile(r"^end \d+( [a-z]=[0-9a-f]{16})+$"),
    re.compile(r"^invalid reason=[a-z-]+ v=\d+ hc=[0-9a-f]{2}$"),
]


def filter_ok(path):
    bad = []
    with open(path, encoding="ascii", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not any(r.match(line) for r in LINE_RES):
                bad.append(n)
    return bad


class Trace:
    def __init__(self, path):
        self.path = path
        self.streams = {k: [] for k in "gmcsrwfpd"}
        self.detail = []
        self.stat = {}
        self.end = None
        self.invalid = None
        self.header = ""
        with open(path, encoding="ascii", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                if line.startswith("#"):
                    self.header = line
                    continue
                tag = line.split(" ", 1)[0]
                if tag in self.streams:
                    self.streams[tag].append(line)
                elif tag == "G":
                    self.detail.append(line)
                elif tag == "stat":
                    self.stat = dict(kv.split("=", 1) for kv in line.split()[1:])
                elif tag == "end":
                    t = line.split()
                    self.end = {"v": int(t[1])}
                    self.end.update(kv.split("=", 1) for kv in t[2:])
                elif tag == "invalid":
                    self.invalid = line

    @property
    def mode(self):
        m = re.search(r"mode=(\S+)", self.header)
        return m.group(1) if m else "?"


def vbl_of(line):
    return int(line.split()[1])


def first_diff(a, b):
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else None
        lb = b[i] if i < len(b) else None
        if la != lb:
            return i, la, lb
    return None


def compare(a, b, names=None):
    """Return (verdict, report_lines, first_divergence) for traces a and b."""
    rep = []
    if a.invalid or b.invalid or a.end is None or b.end is None:
        for t in (a, b):
            if t.invalid:
                rep.append("%s: %s" % (t.path, t.invalid))
            elif t.end is None:
                rep.append("%s: no end line (crash or kill before end)" % t.path)
        return "INVALID", rep, None
    if a.mode != b.mode:
        rep.append("note: modes differ: %s vs %s" % (a.mode, b.mode))
    divs = []
    for tag in "gmcsrwfpd":
        d = first_diff(a.streams[tag], b.streams[tag])
        if d:
            i, la, lb = d
            v = min(vbl_of(x) for x in (la, lb) if x)
            divs.append((v, tag, i, la, lb))
        elif a.end.get(tag) != b.end.get(tag):
            divs.append((a.end["v"], tag, -1, "cumulative " + str(a.end.get(tag)),
                         "cumulative " + str(b.end.get(tag))))
    counters = []
    for k in COUNTERS_GAME:
        if a.stat.get(k) != b.stat.get(k):
            counters.append((k, a.stat.get(k), b.stat.get(k)))
    if a.end["v"] != b.end["v"]:
        counters.append(("end-v", a.end["v"], b.end["v"]))
    if not divs and not counters:
        return "IDENTICAL", rep, None
    divs.sort()
    game = [d for d in divs if d[1] in GAME_STREAMS]
    frame = [d for d in divs if d[1] in FRAME_STREAMS]
    verdict = "DIVERGENT-GAME" if (game or counters) else "DIVERGENT-FRAME"
    first = divs[0] if divs else None
    if first:
        rep.append("first divergent VBL %d in stream %s (%s)" %
                   (first[0], first[1], STREAM_NAMES[first[1]]))
    for v, tag, i, la, lb in divs:
        rep.append("  %-12s first at VBL %-6d line %-6d  A: %s" % (STREAM_NAMES[tag], v, i, la))
        rep.append("  %-12s %-28s  B: %s" % ("", "", lb))
        if tag == "c" and la and lb and not la.startswith("cumulative"):
            fa, fb = la.split()[2:], lb.split()[2:]
            deltas = [int(y, 16) - int(x, 16) for x, y in zip(fa, fb)]
            rep.append("  checkpoint deltas (B-A): t=%d l548=%d l552=%d w14482=%d" % tuple(deltas))
    for k, x, y in counters:
        rep.append("  counter %-10s A=%s B=%s" % (k, x, y))
    rep.append("  divergent game streams: %s; frame streams: %s" % (
        "".join(sorted({d[1] for d in game})) or "-", "".join(sorted({d[1] for d in frame})) or "-"))
    return verdict, rep, first


def counters_healthy(t):
    probs = []
    try:
        if int(t.stat.get("keyovf", "0")) != 0:
            probs.append("key-ring overflow %s" % t.stat["keyovf"])
        fl, hu = int(t.stat["flagged"]), int(t.stat["hostupdate"])
        if abs(fl - hu) > 1:
            probs.append("n_flagged %d != n_hostupdate %d (+-1)" % (fl, hu))
        if int(t.stat.get("a6chg", "0")) != 0:
            probs.append("A6 changed %s times" % t.stat["a6chg"])
    except (KeyError, ValueError):
        probs.append("stat line missing or malformed")
    return probs


def verdict(baseline, run1, run2):
    b, r1, r2 = Trace(baseline), Trace(run1), Trace(run2)
    rep = []
    for t in (b, r1, r2):
        bad = filter_ok(t.path)
        if bad:
            return "ERROR-INFRA", ["%s: %d lines fail the artefact filter" % (t.path, len(bad))]
    v12, rep12, _ = compare(r1, r2)
    if v12 == "INVALID":
        return "ERROR-INFRA", rep12
    if v12 != "IDENTICAL":
        return "FAIL-NONDET", ["the two runs of the double run differ:"] + rep12
    vb, repb, _ = compare(b, r1)
    if vb == "INVALID":
        return "ERROR-INFRA", repb
    probs = counters_healthy(r1)
    if vb == "DIVERGENT-GAME" or probs:
        return "FAIL-GAME", repb + ["  " + p for p in probs]
    if vb == "DIVERGENT-FRAME":
        return "FAIL-FRAME", repb
    return "PASS", rep


# --------------------------------------------------------------------------
# running the binary

def make_rundir(path, binfile, saves=None):
    os.makedirs(path, exist_ok=True)
    link = os.path.join(path, "fe2.s.bin")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(os.path.abspath(binfile), link)
    sv = os.path.join(path, "savs")
    if saves:
        shutil.copytree(saves, sv, dirs_exist_ok=True)
    else:
        os.makedirs(sv, exist_ok=True)
    # no sfx/ and no music/: IsMusicPlaying stays 0 (07 3.2, I-17)
    return path


def run_binary(binary, args, cwd, timeout, log):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEO_WINDOW_POS", "1500,800")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    t0 = time.time()
    with open(log, "w") as lf:
        p = subprocess.Popen([os.path.abspath(binary)] + args, cwd=cwd, stdout=lf,
                             stderr=subprocess.STDOUT, env=env)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.send_signal(signal.SIGKILL)   # only our own child, by PID
            p.wait()
            rc = "timeout"
    return rc, time.time() - t0


def replay(binary, tape, out, args=(), detail=None, timeout=300, keep=False, saves=None):
    bindir = os.path.dirname(os.path.abspath(binary))
    work = tempfile.mkdtemp(prefix="fe2oracle-")
    try:
        make_rundir(work, os.path.join(bindir, "fe2.s.bin"), saves)
        a = ["--nosound", "--oracle-replay", os.path.abspath(tape),
             "--oracle-out", os.path.abspath(out), "--oracle-timeout", str(int(timeout))]
        if detail:
            a += ["--oracle-detail", detail]
        a += list(args)
        rc, secs = run_binary(binary, a, work, timeout + 30, out + ".log")
        return rc, secs
    finally:
        if keep:
            print("run directory kept: %s" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


def localise(trA, trB, names):
    """First divergent G line between two detail traces."""
    d = first_diff(trA.detail, trB.detail)
    if not d:
        return ["detail traces agree in the window (divergence is outside a hcall boundary "
                "or only in a cumulative hash)"]
    i, la, lb = d
    out = []
    for lab, l in (("A", la), ("B", lb)):
        if l:
            t = l.split()
            hc = int(t[3], 16)
            out.append("  %s: VBL %s hcall #%s 0x%02x %s %s  %s" % (
                lab, t[1], t[2], hc, names.get(hc, "?"), "entry" if t[4] == "e" else "exit", t[5]))
        else:
            out.append("  %s: (no line)" % lab)
    return ["first divergent hcall boundary:"] + out


# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("lint"); p.add_argument("tapes", nargs="+")
    p = sp.add_parser("filter"); p.add_argument("files", nargs="+")
    p = sp.add_parser("rundir"); p.add_argument("dir"); p.add_argument("--bin", default=os.path.join(REPO, "fe2.s.bin")); p.add_argument("--saves")
    p = sp.add_parser("replay")
    p.add_argument("--bin", required=True); p.add_argument("--tape", required=True)
    p.add_argument("--out", required=True); p.add_argument("--args", default="")
    p.add_argument("--detail"); p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--keep", action="store_true"); p.add_argument("--saves")
    p = sp.add_parser("compare"); p.add_argument("a"); p.add_argument("b")
    p = sp.add_parser("verdict"); p.add_argument("--baseline", required=True); p.add_argument("run1"); p.add_argument("run2")
    p = sp.add_parser("diverge")
    p.add_argument("--tape", required=True); p.add_argument("--work", required=True)
    p.add_argument("--bin-a", required=True); p.add_argument("--args-a", default="")
    p.add_argument("--bin-b", required=True); p.add_argument("--args-b", default="")
    p.add_argument("--timeout", type=float, default=300); p.add_argument("--saves")
    ns = ap.parse_args(argv)

    if ns.cmd == "lint":
        rc = 0
        for t in ns.tapes:
            errs = lint_tape(t)
            print("%s: %s" % (t, "ok" if not errs else "FAIL"))
            for e in errs:
                print("  " + e)
            rc |= bool(errs)
        return rc
    if ns.cmd == "filter":
        rc = 0
        for fpath in ns.files:
            bad = filter_ok(fpath)
            print("%s: %s" % (fpath, "ok" if not bad else "FAIL lines %s" % bad[:10]))
            rc |= bool(bad)
        return rc
    if ns.cmd == "rundir":
        make_rundir(ns.dir, ns.bin, ns.saves)
        print(ns.dir)
        return 0
    if ns.cmd == "replay":
        rc, secs = replay(ns.bin, ns.tape, ns.out, shlex.split(ns.args), ns.detail, ns.timeout, ns.keep, ns.saves)
        print("rc=%s wall=%.1fs out=%s" % (rc, secs, ns.out))
        return 0 if rc == 0 else 1
    if ns.cmd == "compare":
        v, rep, _ = compare(Trace(ns.a), Trace(ns.b))
        print(v)
        for line in rep:
            print(line)
        return 0 if v == "IDENTICAL" else 1
    if ns.cmd == "verdict":
        v, rep = verdict(ns.baseline, ns.run1, ns.run2)
        print(v)
        for line in rep:
            print(line)
        return 0 if v == "PASS" else 1
    if ns.cmd == "diverge":
        os.makedirs(ns.work, exist_ok=True)
        oa, ob = os.path.join(ns.work, "a.out"), os.path.join(ns.work, "b.out")
        for b, a, o in ((ns.bin_a, ns.args_a, oa), (ns.bin_b, ns.args_b, ob)):
            rc, secs = replay(b, ns.tape, o, shlex.split(a), None, ns.timeout, False, ns.saves)
            print("replay %s %s -> rc=%s wall=%.1fs" % (b, a, rc, secs))
        v, rep, first = compare(Trace(oa), Trace(ob))
        print(v)
        for line in rep:
            print(line)
        if first and v != "INVALID":
            fv = first[0]
            window = "%d:%d" % (max(0, fv - 5), fv + 1)
            da, db = os.path.join(ns.work, "a.detail"), os.path.join(ns.work, "b.detail")
            replay(ns.bin_a, ns.tape, da, shlex.split(ns.args_a), window, ns.timeout, False, ns.saves)
            replay(ns.bin_b, ns.tape, db, shlex.split(ns.args_b), window, ns.timeout, False, ns.saves)
            print("detail re-run --oracle-detail %s:" % window)
            for line in localise(Trace(da), Trace(db), hcall_names()):
                print(line)
        return 0 if v == "IDENTICAL" else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
