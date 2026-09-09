"""Tests for tools/emubench.py's parsing: the probe's rows, and the
guest-clock total those rows add up to."""

import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "tools", "emubench.py")


def load():
    spec = importlib.util.spec_from_file_location("emubench", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ParseRows(unittest.TestCase):
    """Each printed row becomes a dict keyed by the test's name."""

    def test_rows(self):
        emu = load()
        text = (
            "emubench: alu        iters=20000000 ns=86000000 ns/iter=4.3 mips=1163.4\r\n"
            "emubench: cold2k/p1  iters=1 ns=2000000 ns/iter=2000000.0 mips=8.0\r\n"
        )
        rows = emu.parse_rows(text)
        self.assertEqual(rows["alu"]["mips"], 1163.4)
        self.assertEqual(rows["cold2k/p1"]["ns"], 2000000)


class GuestSeconds(unittest.TestCase):
    """The guest's own account of how long the probe took: the sum of
    every row's nanoseconds. Compared with the host's wall clock it says
    whether the guest's clock can be trusted."""

    def test_sums_the_rows(self):
        emu = load()
        rows = {"a": {"ns": 1_500_000_000}, "b": {"ns": 500_000_000}}
        self.assertAlmostEqual(emu.guest_seconds(rows), 2.0)


class ClockRatio(unittest.TestCase):
    """Each row's guest nanoseconds over the host seconds between its
    appearance and the previous row's; rows too short to time are
    skipped, and the median is taken."""

    def test_median_over_long_rows(self):
        emu = load()
        rows = {"a": {"ns": 1_000_000_000}, "b": {"ns": 2_000_000_000}, "c": {"ns": 10_000_000}}
        # a took 1 s on the host, b 4 s, c 0.01 s: ratios 1.0, 0.5, skipped
        arrivals = {"a": 1.0, "b": 5.0, "c": 5.01}
        self.assertAlmostEqual(emu.clock_ratio(rows, arrivals), 0.75)

    def test_nothing_long_enough(self):
        emu = load()
        self.assertIsNone(emu.clock_ratio({"a": {"ns": 1}}, {"a": 0.01}))


class Crashed(unittest.TestCase):
    """A probe the shell reports as dead is reported, not waited for."""

    def test_illegal_instruction(self):
        emu = load()
        self.assertEqual(emu.crashed("~ # emubench all\r\nIllegal instruction\r\n~ # "), "Illegal instruction")

    def test_a_healthy_transcript(self):
        emu = load()
        self.assertIsNone(emu.crashed("emubench: alu iters=1 ns=1 ns/iter=1.0 mips=1.0\r\n"))
