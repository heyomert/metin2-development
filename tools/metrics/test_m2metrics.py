#!/usr/bin/env python3
"""Tests m2metrics.py against real telemetry lines (testdata/lines-before-build-identity.txt, taken from the test VM
on 2026-10-06 before the build identity existed) and the same lines with the identity fields appended exactly as
game/server_metrics.cpp and common/sql_metrics.h append them (M2BuildFields()).

    python3 tools/metrics/test_m2metrics.py

Proves: old lines still parse and summarise; new lines show their build; a build change inside the period is listed;
the added fields do not change any existing field. DB step 2a: the result/policy/phase fields, inserted before
metrics_dropped exactly as common/sql_metrics.h writes them, are summed; older lines are reported as having no such
fields (never as zero); the failure ledger (sql_failures_*.log) is summarised by --failures and not read by --sql.
Python 3 standard library only.
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
# Step 2a sum fields, same order as common/sql_metrics.h; written between direct_max_ms and metrics_dropped
FIELDS_2A = (" q_bytes=330 res_not_delivered=2 res_rolled_back=0 res_ambiguous=1 res_permanent=3"
             " res_unexecuted_at_quit=0 pol_transient=3 pol_config_fatal=0 pol_resource_limit=0"
             " pol_internal=0 pol_query_permanent=3 pol_ambiguous=0 fail_send=4 fail_read=1"
             " session_check_fail=0 log_suppressed=7 ledger_events=6 ledger_dropped=0 ledger_write_errors=0")
LEDGER = ("schema=1 src=sql_failure host=channel1_1 pid=30717 role=game.player.main family=replace.item id=0 "
          "phase=send errno=2013 result=not_delivered policy=transient_connection attempts=1 age_ms=12")


def with_2a(line):
    i = line.index(" metrics_dropped=")
    return line[:i] + FIELDS_2A + line[i:]

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

print("4. step 2a SQL fields")
sql_2a = with_2a(sql_old) + FIELDS
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, "channel1", "core1", "log")
    os.makedirs(log)
    day = datetime.now().strftime("%Y-%m-%d")
    t0 = datetime.now() - timedelta(minutes=10)
    with open(os.path.join(log, f"sql_{day}.log"), "w", encoding="utf-8") as f:
        f.write(restamp(sql_old, t0) + "\n")
        f.write(restamp(sql_2a, t0 + timedelta(seconds=10)) + "\n")
        f.write(restamp(sql_2a, t0 + timedelta(seconds=20)) + "\n")
    with open(os.path.join(log, f"sql_failures_{day}.log"), "w", encoding="utf-8") as f:
        for i in range(3):
            f.write(restamp("x " + LEDGER + FIELDS, t0 + timedelta(seconds=i)).replace(" x ", " ", 1) + "\n")
        f.write(restamp("x " + LEDGER.replace("phase=send", "phase=read").replace("result=not_delivered",
                "result=ambiguous").replace("transient_connection", "ambiguous_no_retry") + FIELDS,
                t0 + timedelta(seconds=5)).replace(" x ", " ", 1) + "\n")
    out = run(["--sql", "--dir", d, "--hours", "1"])
    check("windows=3" in out, "--sql reads the three sum lines and not the ledger file")
    check("not_delivered 4, rolled_back 0, ambiguous 2, permanent 6, unexecuted_at_quit 0" in out,
          "result fields summed over the 2a windows only")
    check("(1 window(s) from an older binary have no such fields)" in out, "older window reported, not counted as 0")
    check("transient 6," in out and "query_permanent 6" in out and "send 8, read 2" in out, "policy and phase sums")
    check("ledger events 12" in out and "syserr lines suppressed 14" in out and "q_bytes max 330" in out,
          "ledger/suppression/queue bytes")

    with open(os.path.join(log, f"sql_{day}.log"), "w", encoding="utf-8") as f:
        f.write(restamp(sql_old, t0) + "\n")
    out = run(["--sql", "--dir", d, "--hours", "1"])
    check("n/a (no step 2a fields; older binary)" in out, "old-only period says n/a, not zeros")

    out = run(["--failures", "--dir", d, "--hours", "1"])
    check("4 line(s), 2 group(s)" in out, "ledger lines grouped")
    check("     3  host=channel1_1 role=game.player.main family=replace.item phase=send errno=2013 "
          "result=not_delivered policy=transient_connection" in out, "largest group first, metadata fields only")
    check(f"{COMMIT[:12]} dirty=0 src=archive" in out, "ledger build shown")

print("5. appended fields leave every existing field unchanged")
kv = lambda line: dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
old, new = kv(health_old), kv(health_old + FIELDS)
check(all(new[k] == v for k, v in old.items()) and set(new) - set(old) == {"build", "build_dirty", "build_src"},
      "health: same keys and values + build, build_dirty, build_src")
old, new = kv(sql_old), kv(with_2a(sql_old))
check(all(new[k] == v for k, v in old.items()) and set(new) - set(old) == set(kv("x" + FIELDS_2A)),
      "SQL sum: same keys and values + the step 2a fields")
print(f"   bytes per line: health {len(health_old)} -> {len(health_old + FIELDS)} (+{len(FIELDS)}), "
      f"SQL sum {len(sql_old)} -> {len(sql_old + FIELDS)} (+{len(FIELDS)}), with 2a fields {len(sql_2a)} "
      f"(+{len(FIELDS_2A)} for 2a)")

print()
print("PASSED" if failures == 0 else f"FAILED ({failures})")
sys.exit(failures)
