#!/usr/bin/env python3
"""Micro-benchmark of the plugin's own work during a long drag (nested test instance).

usage: bench.py --plugin build/hyprdropper.so --tester <hyprtestplugin.so> [--moves 400]
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_tests as rt  # noqa: E402


def perf(I):
    out = {}
    for line in I.ctl("hyprdropper").splitlines():
        if line.startswith("perf "):
            name, rest = line[5:].split(":", 1)
            out[name] = rest.strip()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--tester", required=True)
    ap.add_argument("--moves", type=int, default=400)
    a = ap.parse_args()

    I = rt.Instance(a.plugin, a.tester)
    try:
        I.start()
        I.eval("hl.config({ animations = { enabled = true } })")
        I.reset(("a", "b", "c", "d"))

        # idle: no drag, just let frames happen
        I.ctl("hyprdropper-perfreset")
        for i in range(60):
            I.move(300 + i * 5, 400)
        print("== idle (no drag, 60 pointer moves)")
        for k, v in perf(I).items():
            print(f"  {k:13s} {v}")

        I.ctl("hyprdropper-perfreset")
        rt.begin_drag(I, I.order()[0])
        t0 = time.time()
        for i in range(a.moves):
            x = 960 + 700 * math.sin(i / 25.0)
            y = 540 + 380 * math.sin(i / 11.0)
            I.move(int(x), int(y))
        dur = time.time() - t0
        time.sleep(0.5)
        print(f"== drag ({a.moves} moves in {dur:.1f}s, animations on)")
        for k, v in perf(I).items():
            print(f"  {k:13s} {v}")
        I.eval("hl.plugin.test.keybind(1, 0, 9)")
        I.eval("hl.plugin.test.keybind(0, 0, 9)")
        I.release()
    finally:
        I.stop()


if __name__ == "__main__":
    main()
