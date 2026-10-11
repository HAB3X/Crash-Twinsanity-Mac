#!/usr/bin/env python3
"""Frame-time benchmark: boots the native build to the beach (--quick), lets it run for a while with TWIN_FRAME_STATS and
prints the average frame time, the render thread's time and the game's wait for it over named stretches of frames.

    python native/tools/bench_frames.py [--seconds 60] [--runs 2] [--app PATH] [-- extra args for the game]

The beach's opening flyover (frames ~220-520 once the stats start) is the heavy stretch; the run's last 500 frames are the idle beach.
(Frame numbers shift between builds: a slow frame advances the game's time by up to two, so compare stretches by content.)
Environment passes through (TWIN_RENDER_THREAD, TWIN_GS, TWIN_GS_THREADS...), so two settings can be compared run for run.
"""
import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "build/native/cmake/Crash Twinsanity.app/Contents/MacOS/Crash Twinsanity")
LINE = re.compile(r"^frame: ([0-9.]+) ms, .*?render ([0-9.]+) ms, waited ([0-9.]+) ms")

# (name, first frame, end): frame numbers count the stats lines of the run (negative: from the end)
STRETCHES = [("flyover", 220, 520), ("idle (last 500)", -500, None)]


def run_once(app, seconds, extra):
    env = dict(os.environ, TWIN_FRAME_STATS="1", TWINSANITY_AUDIO="off")
    with tempfile.TemporaryFile("w+") as err:
        proc = subprocess.Popen([app, "--quick", "--quiet-stubs"] + extra, env=env, stdout=subprocess.DEVNULL, stderr=err)
        time.sleep(seconds)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        err.seek(0)
        frames = [LINE.match(line) for line in err]
    return [tuple(float(g) for g in m.groups()) for m in frames if m]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--app", default=APP)
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()

    rows = {name: [] for name, _, _ in STRETCHES}
    for run in range(args.runs):
        frames = run_once(args.app, args.seconds, args.extra)
        print(f"run {run + 1}: {len(frames)} frames", file=sys.stderr)
        for name, first, last in STRETCHES:
            part = frames[first:last]
            if part:
                rows[name].append(tuple(statistics.fmean(c) for c in zip(*part)))

    print(f"{'stretch':<16} {'frame ms':>9} {'render ms':>10} {'waited ms':>10} {'fps':>6}")
    for name, _, _ in STRETCHES:
        if rows[name]:
            frame, render, waited = (statistics.fmean(c) for c in zip(*rows[name]))
            print(f"{name:<16} {frame:9.1f} {render:10.1f} {waited:10.1f} {1000 / frame:6.1f}")


main()
