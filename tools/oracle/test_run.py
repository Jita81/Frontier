#!/usr/bin/env python3
"""Unit tests for tools/oracle/run.py: tape lint, artefact filter, pairwise
compare, the 7.1 verdicts and hcall-name parsing.  Synthetic stream files
only; no game, no game data.  Run: python3 tools/oracle/test_run.py"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run  # noqa: E402

H = "0123456789abcdef"
GOOD_TAPE = """fe2rep 1 mode=gl-rgl seed=0 size=640x480 requires=-
# comment
k 10 39 b9
m 12 0 1 -1 100 50 0
m 12 * 0 0 100 50 2
end 20
"""


def stream(v_end=3, g=H, f=H, keyovf=0, flagged=None, invalid=None, cstr="00000001 00000002 00000003 0004",
           tape_end=None, early_exit=False):
    head = "# fe2gold 1 mode=gl-rgl oracle=replay seed=0"
    if tape_end is not None:
        head += " tape_end=%d" % tape_end
    lines = [head]
    for v in range(v_end):
        lines.append("m %d %s" % (v + 1, H))
        lines.append("c %d %s" % (v + 1, cstr))
        lines.append("f %d %s" % (v, f))
        lines.append("p %d %s" % (v, H))
        lines.append("g %d %s" % (v, g))
    if invalid:
        lines.append("invalid reason=%s v=%d hc=07" % (invalid, v_end))
    else:
        fl = v_end if flagged is None else flagged
        lines.append("stat flagged=%d hostupdate=%d wd=0 wdv=- keyovf=%d a6=0000015e a6chg=0 draws=%d "
                     "screens=3 idle=036f70:%d" % (fl, v_end, keyovf, v_end, v_end))
        lines.append("end %d g=%s m=%s c=%s s=%s r=%s f=%s p=%s" % ((v_end,) + (H,) * 7))
        if early_exit:
            lines.append("invalid reason=early-exit v=%d hc=07" % v_end)
    return "\n".join(lines) + "\n"


class Tmp:
    def __init__(self):
        self.d = tempfile.mkdtemp(prefix="oracle-test-")

    def file(self, name, text):
        p = os.path.join(self.d, name)
        with open(p, "w") as f:
            f.write(text)
        return p


class LintTests(unittest.TestCase):
    def setUp(self):
        self.t = Tmp()

    def lint(self, text):
        return run.lint_tape(self.t.file("t.fe2rep", text))

    def test_good_tape(self):
        self.assertEqual(self.lint(GOOD_TAPE), [])

    def test_fixture_route_tape(self):
        here = os.path.dirname(os.path.abspath(__file__))
        self.assertEqual(run.lint_tape(os.path.join(here, "fixtures", "route.fe2rep")), [])

    def test_missing_header(self):
        self.assertTrue(self.lint("k 1 39\nend 2\n"))

    def test_bad_mode(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("gl-rgl", "vulkan")))

    def test_missing_end(self):
        self.assertIn("no end record", self.lint(GOOD_TAPE.replace("end 20\n", "")))

    def test_record_after_end(self):
        self.assertTrue(self.lint(GOOD_TAPE + "k 21 39\n"))

    def test_v_decreases(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("m 12 0", "m 9 0")))

    def test_h_refused(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("end 20", "h 15 warp\nend 20")))

    def test_requires_unknown(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("requires=-", "requires=warp")))

    def test_more_than_15_keys(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("k 10 39 b9", "k 10 " + " ".join(["39"] * 16))))
        self.assertEqual(self.lint(GOOD_TAPE.replace("k 10 39 b9", "k 10 " + " ".join(["39"] * 15))), [])

    def test_abs_range(self):
        self.assertTrue(self.lint(GOOD_TAPE.replace("100 50 0", "320 50 0")))
        self.assertTrue(self.lint(GOOD_TAPE.replace("100 50 0", "100 200 0")))


class FilterTests(unittest.TestCase):
    def test_stream_passes(self):
        t = Tmp()
        self.assertEqual(run.filter_ok(t.file("a", stream())), [])

    def test_tape_end_header_passes(self):
        t = Tmp()
        self.assertEqual(run.filter_ok(t.file("a", stream(tape_end=3, early_exit=True))), [])

    def test_free_text_fails(self):
        t = Tmp()
        self.assertEqual(run.filter_ok(t.file("a", stream() + "COMMANDER JAMESON\n")), [19])


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.t = Tmp()

    def cmp(self, a, b):
        return run.compare(run.Trace(self.t.file("a", a)), run.Trace(self.t.file("b", b)))

    def test_identical(self):
        self.assertEqual(self.cmp(stream(), stream())[0], "IDENTICAL")

    def test_game_divergence(self):
        v, rep, first = self.cmp(stream(), stream(g="f" * 16))
        self.assertEqual(v, "DIVERGENT-GAME")
        self.assertEqual(first[:2], (0, "g"))

    def test_frame_only_divergence(self):
        v, rep, first = self.cmp(stream(), stream(f="e" * 16))
        self.assertEqual(v, "DIVERGENT-FRAME")
        self.assertEqual(first[1], "f")

    def test_checkpoint_deltas_reported(self):
        v, rep, _ = self.cmp(stream(), stream(cstr="00000001 00000002 00000005 0004"))
        self.assertEqual(v, "DIVERGENT-GAME")
        self.assertTrue(any("l552=2" in line for line in rep))

    def test_counter_divergence_is_game(self):
        self.assertEqual(self.cmp(stream(), stream(flagged=2))[0], "DIVERGENT-GAME")

    def test_invalid(self):
        self.assertEqual(self.cmp(stream(), stream(invalid="hung-hcall"))[0], "INVALID")

    def test_truncated_is_invalid(self):
        self.assertEqual(self.cmp(stream(), stream().rsplit("stat", 1)[0])[0], "INVALID")

    def test_end_before_tape_end_is_invalid(self):
        # a replay quit early (SIGTERM -> SDL_QUIT) writes a normal end line
        # at a VBL short of the tape's end: INVALID, not a game divergence
        v, rep, _ = self.cmp(stream(v_end=5, tape_end=5), stream(v_end=3, tape_end=5))
        self.assertEqual(v, "INVALID")
        self.assertTrue(any("ended at VBL 3, the tape ends at 5" in line for line in rep))

    def test_end_at_tape_end_is_identical(self):
        self.assertEqual(self.cmp(stream(tape_end=3), stream(tape_end=3))[0], "IDENTICAL")

    def test_record_header_has_no_tape_end(self):
        self.assertEqual(self.cmp(stream(v_end=5), stream(v_end=5))[0], "IDENTICAL")

    def test_early_exit_line_is_invalid(self):
        self.assertEqual(self.cmp(stream(tape_end=3), stream(tape_end=3, early_exit=True))[0], "INVALID")


class VerdictTests(unittest.TestCase):
    def setUp(self):
        self.t = Tmp()

    def verdict(self, base, r1, r2):
        return run.verdict(self.t.file("b", base), self.t.file("r1", r1), self.t.file("r2", r2))[0]

    def test_pass(self):
        self.assertEqual(self.verdict(stream(), stream(), stream()), "PASS")

    def test_nondet(self):
        self.assertEqual(self.verdict(stream(), stream(), stream(g="f" * 16)), "FAIL-NONDET")

    def test_fail_game(self):
        self.assertEqual(self.verdict(stream(), stream(g="f" * 16), stream(g="f" * 16)), "FAIL-GAME")

    def test_fail_frame(self):
        self.assertEqual(self.verdict(stream(), stream(f="e" * 16), stream(f="e" * 16)), "FAIL-FRAME")

    def test_key_overflow_is_fail_game(self):
        s = stream(keyovf=1)
        self.assertEqual(self.verdict(s, s, s), "FAIL-GAME")

    def test_flag_mismatch_is_fail_game(self):
        s = stream(v_end=5, flagged=3)
        self.assertEqual(self.verdict(s, s, s), "FAIL-GAME")

    def test_invalid_is_infra(self):
        self.assertEqual(self.verdict(stream(), stream(invalid="timeout"), stream()), "ERROR-INFRA")

    def test_truncated_run_is_infra(self):
        full = stream(v_end=5, tape_end=5)
        short = stream(v_end=3, tape_end=5)
        self.assertEqual(self.verdict(full, short, short), "ERROR-INFRA")
        self.assertEqual(self.verdict(full, full, short), "ERROR-INFRA")


class HcallNameTests(unittest.TestCase):
    def test_names(self):
        n = run.hcall_names()
        self.assertEqual(len(n), 122)
        self.assertEqual(n[0x07], "Call_HostUpdate")
        self.assertEqual(n[0x01], "Call_Memset")
        self.assertEqual(n[0x1f], "Call_Idle")
        self.assertEqual(n[0x73], "Nu_IsGLRenderer")


if __name__ == "__main__":
    unittest.main()
