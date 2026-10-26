#!/usr/bin/env python3
"""drive.py - run an unmodified GLFrontier binary under libfe2drive.dylib.

Launches the game with the driver library injected, services the driver's
`shot` requests with window-only screenshots of the game's own window (found
by PID), enforces a wall-clock timeout (killing only the game's own PID), and
writes a run manifest.  Also compares two runs' screenshots (--compare).

Shot mechanism: drive.py creates two pipes and passes their descriptors to the
game as FE2_DRIVE_REQ_FD / FE2_DRIVE_ACK_FD.  The driver thread writes
"shot <seq> <name>" and blocks until drive.py writes back "ok <seq> <file>" or
"fail <seq> <reason>".  drive.py finds the window with
CGWindowListCopyWindowInfo (owner PID == game PID, layer 0), captures it with
`screencapture -x -o -l <window id>`, and scales it with ffmpeg to the
window's point size (640x480: drive.py passes --size 640 480 unless --args
sets a size) so a PNG stays under 150 KB.

The game keeps running while a shot is taken: a shot is "about this moment",
not a frame-exact capture.  This tool is a convenience, not an oracle.
"""
import argparse
import ctypes
import ctypes.util
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_PNG = 150 * 1024


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


# --- CoreGraphics window lookup (ctypes, no pyobjc) -------------------------
class _CG:
    def __init__(self):
        self.cg = ctypes.cdll.LoadLibrary(
            "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        self.cf = ctypes.cdll.LoadLibrary(
            "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        cg, cf = self.cg, self.cf
        vp = ctypes.c_void_p
        cg.CGWindowListCopyWindowInfo.restype = vp
        cg.CGWindowListCopyWindowInfo.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        cf.CFArrayGetCount.restype = ctypes.c_long
        cf.CFArrayGetCount.argtypes = [vp]
        cf.CFArrayGetValueAtIndex.restype = vp
        cf.CFArrayGetValueAtIndex.argtypes = [vp, ctypes.c_long]
        cf.CFDictionaryGetValue.restype = vp
        cf.CFDictionaryGetValue.argtypes = [vp, vp]
        cf.CFStringCreateWithCString.restype = vp
        cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_uint32]
        cf.CFNumberGetValue.restype = ctypes.c_bool
        cf.CFNumberGetValue.argtypes = [vp, ctypes.c_int, vp]
        cf.CFRelease.argtypes = [vp]
        self.keys = {k: cf.CFStringCreateWithCString(None, k.encode(), 0x08000100)
                     for k in ("kCGWindowOwnerPID", "kCGWindowNumber", "kCGWindowLayer",
                               "kCGWindowBounds", "Width", "Height", "X", "Y")}

    def _num(self, ref, as_float=False):
        if not ref:
            return None
        if as_float:
            v = ctypes.c_double()
            self.cf.CFNumberGetValue(ref, 13, ctypes.byref(v))  # kCFNumberDoubleType
        else:
            v = ctypes.c_int64()
            self.cf.CFNumberGetValue(ref, 4, ctypes.byref(v))   # kCFNumberSInt64Type
        return v.value

    def windows_of(self, pid):
        """[(window id, (x, y, w, h))] for layer-0 windows owned by pid."""
        arr = self.cg.CGWindowListCopyWindowInfo(0, 0)   # all windows
        out = []
        if not arr:
            return out
        try:
            for i in range(self.cf.CFArrayGetCount(arr)):
                d = self.cf.CFArrayGetValueAtIndex(arr, i)
                g = lambda k: self.cf.CFDictionaryGetValue(d, self.keys[k])
                if self._num(g("kCGWindowOwnerPID")) != pid:
                    continue
                if self._num(g("kCGWindowLayer")) != 0:
                    continue
                b = g("kCGWindowBounds")
                bb = tuple(self._num(self.cf.CFDictionaryGetValue(b, self.keys[k]), True)
                           for k in ("X", "Y", "Width", "Height"))
                out.append((self._num(g("kCGWindowNumber")), bb))
        finally:
            self.cf.CFRelease(arr)
        return out


def find_window(cg, pid, wait_s=5.0):
    end = time.time() + wait_s
    while True:
        wins = [w for w in cg.windows_of(pid) if w[1][2] >= 300 and w[1][3] >= 180]
        if wins:
            return max(wins, key=lambda w: w[1][2] * w[1][3])
        if time.time() > end:
            return None
        time.sleep(0.2)


# --- image helpers (ffmpeg) -------------------------------------------------
def ffmpeg(*args):
    return subprocess.run(["ffmpeg", "-v", "error", "-y", *args],
                          capture_output=True, text=True)


def luma_stats(png):
    """(mean, max) luma of a 64x48 grey thumbnail; (None, None) on error."""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", png, "-vf",
                        "scale=64:48:flags=area,format=gray", "-f", "rawvideo", "-"],
                       capture_output=True)
    if r.returncode or not r.stdout:
        return None, None
    px = r.stdout
    return round(sum(px) / len(px), 2), max(px)


def capture(wid, size, raw, dest):
    """screencapture the window, scale to size, keep under MAX_PNG."""
    r = subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), raw],
                       capture_output=True, text=True)
    if r.returncode or not os.path.exists(raw):
        return "screencapture failed (rc %d): %s" % (r.returncode, r.stderr.strip())
    w, h = size
    r = ffmpeg("-i", raw, "-vf", "scale=%d:%d:flags=area" % (w, h), dest)
    if r.returncode == 0 and os.path.getsize(dest) > MAX_PNG:
        r = ffmpeg("-i", raw, "-vf",
                   "scale=%d:%d:flags=area,split[a][b];[a]palettegen=max_colors=128[p];"
                   "[b][p]paletteuse=dither=none" % (w, h), dest)
    os.unlink(raw)
    if r.returncode:
        return "ffmpeg failed: %s" % r.stderr.strip()
    if os.path.getsize(dest) > MAX_PNG:
        return "PNG still over 150 KB (%d bytes)" % os.path.getsize(dest)
    return None


# --- run --------------------------------------------------------------------
def run(a):
    bin_path = os.path.abspath(a.bin)
    script = os.path.abspath(a.script)
    lib = os.path.abspath(a.lib)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    for p, what in ((bin_path, "--bin"), (script, "--script"), (lib, "--lib")):
        if not os.path.exists(p):
            sys.exit("drive.py: %s %s does not exist" % (what, p))
    cg = _CG()
    # The game opens as large as the desktop allows; a fixed 640x480 keeps
    # shots small and the same on every machine.
    game_args = shlex.split(a.args)
    if "--size" not in game_args:
        game_args = ["--size", "640", "480"] + game_args
    req_r, req_w = os.pipe()
    ack_r, ack_w = os.pipe()
    env = dict(os.environ)
    env.update({
        "DYLD_INSERT_LIBRARIES": lib,
        "FE2_DRIVE_SCRIPT": script,
        "FE2_DRIVE_REQ_FD": str(req_w),
        "FE2_DRIVE_ACK_FD": str(ack_r),
        "SDL_VIDEO_WINDOW_POS": a.pos,
        "SDL_AUDIODRIVER": "dummy",
    })
    manifest = {
        "tool": "tools/drive/drive.py",
        "script": os.path.relpath(script, os.path.dirname(bin_path)),
        "script_sha256": sha256(script),
        "binary": bin_path,
        "binary_sha256": sha256(bin_path),
        "dylib_sha256": sha256(lib),
        "args": game_args,
        "env": {k: env[k] for k in ("SDL_VIDEO_WINDOW_POS", "SDL_AUDIODRIVER")},
        "start_utc": utc_now(),
        "timeout_s": a.timeout,
        "shots": [],
    }
    log_path = os.path.join(out, "game.log")
    logf = open(log_path, "w")
    t0 = time.time()
    # Launch the binary directly: SIP-protected launchers (/usr/bin/env,
    # perl, ...) strip DYLD_* variables.
    proc = subprocess.Popen([bin_path] + game_args, cwd=os.path.dirname(bin_path),
                            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, pass_fds=(req_w, ack_r))
    os.close(req_w)
    os.close(ack_r)
    manifest["pid"] = proc.pid
    print("drive.py: pid %d, script %s, out %s" % (proc.pid, script, out), flush=True)

    def pump():
        for line in iter(proc.stdout.readline, b""):
            s = line.decode("utf-8", "replace")
            logf.write("[%8.3f] %s" % (time.time() - t0, s))
            logf.flush()
            if not a.quiet:
                sys.stdout.write(s)
                sys.stdout.flush()

    shot_errors = []

    def shots():
        f = os.fdopen(req_r, "r")
        ackf = os.fdopen(ack_w, "w")
        for line in f:
            m = re.match(r"shot (\d+) ([A-Za-z0-9._-]+)$", line.strip())
            if not m:
                continue
            seq, name = int(m.group(1)), m.group(2)
            rec = {"seq": seq, "name": name, "t_s": round(time.time() - t0, 3)}
            win = find_window(cg, proc.pid)
            if not win:
                err = "no window owned by pid %d" % proc.pid
            else:
                wid, (x, y, w, h) = win
                rec["window_id"], rec["window_bounds"] = wid, [x, y, w, h]
                dest = os.path.join(out, name + ".png")
                err = capture(wid, (int(w), int(h)), os.path.join(out, "raw-%d.png" % seq), dest)
            if err is None:
                rec["file"] = os.path.basename(dest)
                rec["bytes"] = os.path.getsize(dest)
                rec["sha256"] = sha256(dest)
                rec["luma_mean"], rec["luma_max"] = luma_stats(dest)
                if rec["luma_max"] is not None and rec["luma_max"] < 8:
                    rec["warning"] = ("all black: missing Screen Recording permission, "
                                      "or the game really drew nothing")
                msg = "ok %d %s%s" % (seq, rec["file"], " BLANK" if "warning" in rec else "")
            else:
                rec["error"] = err
                shot_errors.append(name)
                msg = "fail %d %s" % (seq, err)
            manifest["shots"].append(rec)
            try:
                ackf.write(msg + "\n")
                ackf.flush()
            except BrokenPipeError:
                break

    tp = threading.Thread(target=pump, daemon=True)
    ts = threading.Thread(target=shots, daemon=True)
    tp.start()
    ts.start()
    timed_out = False
    try:
        rc = proc.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        print("drive.py: timeout after %ss, SIGTERM to pid %d" % (a.timeout, proc.pid), flush=True)
        os.kill(proc.pid, signal.SIGTERM)
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.kill(proc.pid, signal.SIGKILL)
            rc = proc.wait()
    tp.join(timeout=5)
    ts.join(timeout=5)
    logf.close()
    manifest.update({
        "end_utc": utc_now(),
        "duration_s": round(time.time() - t0, 3),
        "exit_code": rc,
        "timed_out": timed_out,
        "log": "game.log",
        "log_sha256": sha256(log_path),
        "shot_errors": shot_errors,
    })
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    status = 124 if timed_out else 125 if shot_errors else rc
    print("drive.py: exit_code=%s timed_out=%s shots=%d errors=%d -> %s/manifest.json (status %d)"
          % (rc, timed_out, len(manifest["shots"]), len(shot_errors), out, status), flush=True)
    return status


def compare(dir_a, dir_b, threshold):
    """SSIM (ffmpeg) of same-named shots in two run directories."""
    ma = json.load(open(os.path.join(dir_a, "manifest.json")))
    mb = json.load(open(os.path.join(dir_b, "manifest.json")))
    by_b = {s["name"]: s for s in mb["shots"]}
    worst, rows = 1.0, []
    for s in ma["shots"]:
        t = by_b.get(s["name"])
        if not t or "file" not in s or "file" not in t:
            rows.append((s["name"], None))
            worst = 0.0
            continue
        r = subprocess.run(["ffmpeg", "-v", "info", "-i", os.path.join(dir_a, s["file"]),
                            "-i", os.path.join(dir_b, t["file"]), "-lavfi", "ssim",
                            "-f", "null", "-"], capture_output=True, text=True)
        m = re.search(r"All:([0-9.]+)", r.stderr)
        v = float(m.group(1)) if m else 0.0
        rows.append((s["name"], v))
        worst = min(worst, v)
    # a shot only run B has, or no shots at all, is not "the same screens"
    names_a = {s["name"] for s in ma["shots"]}
    for t in mb["shots"]:
        if t["name"] not in names_a:
            rows.append((t["name"], None))
            worst = 0.0
    if not rows:
        print("no shots in either run")
        worst = 0.0
    for name, v in rows:
        print("%-32s %s" % (name, "missing" if v is None else "SSIM %.4f %s" %
                            (v, "same" if v >= threshold else "DIFFERENT")))
    print("worst SSIM %.4f (threshold %.2f): %s" % (worst, threshold,
          "SAME SCREENS" if worst >= threshold else "DIFFERENT"))
    return 0 if worst >= threshold else 1


IKBD_RE = re.compile(r"ikbd: (\S+) (down|up) sym=(-?\d+) kvk=0x([0-9a-f]+) mod=0x([0-9a-f]+)"
                     r" -> ST ([0-9a-f ]+|none)(?:; shortcut key (\d+) latched)?")


def st_names():
    """ST scancode -> name, read from drive.c's ST_NAMES table."""
    text = open(os.path.join(HERE, "drive.c")).read()
    return {int(c, 16): n for n, c in re.findall(r'\{"(\w+)", 0x([0-9a-f]{2})\}', text)}


def ikbd(run_dir):
    """Summarise the 'ikbd:' lines (sdlkey steps) of a run's game.log."""
    names = st_names()
    presses, held, anomalies, events = {}, set(), [], 0
    for line in open(os.path.join(run_dir, "game.log"), encoding="utf-8", errors="replace"):
        m = IKBD_RE.search(line)
        if not m:
            continue
        events += 1
        key, updown, st, sc = m.group(1), m.group(2), m.group(6), m.group(7)
        what = "%s %s mod=0x%s" % (key, updown, m.group(5))
        codes = [] if st == "none" else [int(b, 16) for b in st.split()]
        if len(codes) > 1:
            anomalies.append("%s -> ST %s: one host event sent %d bytes" % (what, st, len(codes)))
        if updown == "down" and not codes and not sc:
            anomalies.append("%s -> nothing: no ST byte and no shortcut latched" % what)
        for c in codes:
            if c & 0x80:
                if c & 0x7f not in held:
                    anomalies.append("%s -> ST %02x: release of 0x%02x (%s) with no press"
                                     % (what, c, c & 0x7f, names.get(c & 0x7f, "?")))
                held.discard(c & 0x7f)
            else:
                held.add(c)
                presses.setdefault(c, set()).add(key)
    print("ikbd events: %d (from %s)" % (events, os.path.join(run_dir, "game.log")))
    print("ST press codes produced: %d" % len(presses))
    for c in sorted(presses):
        print("  0x%02x %-11s <- %s" % (c, names.get(c, "?"), " ".join(sorted(presses[c]))))
    never = [c for c in sorted(names) if c not in presses]
    print("ST table codes never produced: %d" % len(never))
    print("  " + " ".join("0x%02x(%s)" % (c, names[c]) for c in never))
    print("anomalies: %d" % len(anomalies))
    for a in anomalies:
        print("  " + a)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--bin", default=os.path.join(HERE, "..", "..", "frontier"),
                   help="game binary (its directory is the working directory)")
    p.add_argument("--script", help="driver script (.drv)")
    p.add_argument("--out", help="output directory for shots, game.log, manifest.json")
    p.add_argument("--timeout", type=float, default=120, help="wall-clock limit, seconds")
    p.add_argument("--args", default="", help="extra game arguments, one string "
                   "(--size 640 480 is added unless it sets --size)")
    p.add_argument("--lib", default=os.path.join(HERE, "libfe2drive.dylib"))
    p.add_argument("--pos", default="1300,640", help="SDL_VIDEO_WINDOW_POS")
    p.add_argument("--quiet", action="store_true", help="do not echo the game log")
    p.add_argument("--compare", nargs=2, metavar=("RUN_A", "RUN_B"),
                   help="compare the shots of two run directories and exit")
    p.add_argument("--ssim", type=float, default=0.90, help="--compare threshold")
    p.add_argument("--ikbd", metavar="RUN_DIR",
                   help="summarise the ST scancodes that sdlkey steps produced in a run, and exit")
    a = p.parse_args()
    if a.ikbd:
        sys.exit(ikbd(a.ikbd))
    if a.compare:
        sys.exit(compare(a.compare[0], a.compare[1], a.ssim))
    if not a.script or not a.out:
        p.error("--script and --out are required")
    sys.exit(run(a))


if __name__ == "__main__":
    main()
