#!/usr/bin/env python3
"""Tests m2metrics.py against real telemetry lines (testdata/lines-before-build-identity.txt, taken from the test VM
on 2026-10-06 before the build identity existed) and the same lines with the identity fields appended exactly as
game/server_metrics.cpp and common/sql_metrics.h append them (M2BuildFields()).

    python3 tools/metrics/test_m2metrics.py

Proves: old lines still parse and summarise; new lines show their build; a build change inside the period is listed;
the added fields do not change any existing field. Python 3 standard library only.
"""
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "m2metrics.py")
OLD = os.path.join(HERE, "testdata", "lines-before-build-identity.txt")
COMMIT = "19392ac24f344b7878e99d1d21ce59e1043defe1"
FIELDS = f" build={COMMIT} build_dirty=0 build_src=archive"

failures = 0


def check(cond, what):
    global failures
    print(f"  [{'ok' if cond else 'FAIL'}] {what}")
    if not cond:
        failures += 1


def restamp(line, ts):
    return ts.strftime("%Y-%m-%dT%H:%M:%S") + "+03:00" + line[line.index(" "):]


def run(args):
    return subprocess.run([sys.executable, TOOL] + args, capture_output=True, text=True).stdout


health_old, sql_old = [l.rstrip("\n") for l in open(OLD, encoding="utf-8") if l.strip()]
check("build=" not in health_old and "build=" not in sql_old, "test data really predates the identity fields")

with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "channel1", "core1", "log")
    os.makedirs(log)
    day = datetime.now().strftime("%Y-%m-%d")
    t0 = datetime.now() - timedelta(minutes=10)
    with open(os.path.join(log, f"metrics_{day}.log"), "w", encoding="utf-8") as f:
        f.write(restamp(health_old, t0) + "\n")                                         # older binary
        f.write(restamp(health_old + FIELDS, t0 + timedelta(seconds=10)) + "\n")         # new binary
    with open(os.path.join(log, f"sql_{day}.log"), "w", encoding="utf-8") as f:
        f.write(restamp(sql_old, t0) + "\n")
        f.write(restamp(sql_old + FIELDS, t0 + timedelta(seconds=10)) + "\n")

    print("1. health summary (old and new lines in one period)")
    out = run(["--dir", d, "--hours", "1"])
    check("windows=2" in out, "both lines are read as windows")
    check("no build field (older binary)" in out and f"{COMMIT[:12]} dirty=0 src=archive" in out,
          "older line reported as 'no build field', new line shows its build")
    check("builds in this period (2)" in out, "the build change inside the period is listed")

    print("2. SQL summary")
    out = run(["--sql", "--dir", d, "--hours", "1"])
    check("windows=2" in out and f"{COMMIT[:12]} dirty=0 src=archive" in out, "SQL lines read, build shown")

    print("3. --raw passes lines through unchanged")
    out = run(["--raw", "--dir", d, "--hours", "1"])
    check(FIELDS.strip() in out, "identity fields present in raw output")

print("4. appended fields leave every existing field unchanged")
kv = lambda line: dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
old, new = kv(health_old), kv(health_old + FIELDS)
check(all(new[k] == v for k, v in old.items()) and set(new) - set(old) == {"build", "build_dirty", "build_src"},
      "health: same keys and values + build, build_dirty, build_src")
print(f"   bytes per line: health {len(health_old)} -> {len(health_old + FIELDS)} (+{len(FIELDS)}), "
      f"SQL sum {len(sql_old)} -> {len(sql_old + FIELDS)} (+{len(FIELDS)})")

print()
print("PASSED" if failures == 0 else f"FAILED ({failures})")
sys.exit(failures)
