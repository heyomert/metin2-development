#!/usr/bin/env python3
"""Summarise game server health lines (log/metrics_YYYY-MM-DD.log, fields in docs/monitoring.md).

Read-only. Python 3 standard library only.

    python3 m2metrics.py                       # last hour, all processes under the default channels dir
    python3 m2metrics.py --hours 24
    python3 m2metrics.py --dir /path/to/channels --host channel1_1
    python3 m2metrics.py --raw --hours 1       # print the matching lines instead of a summary

Fields are read by name (key=value), never by position, so new fields do not break this tool.
"""
import argparse
import glob
import os
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

DEFAULT_DIR = "/usr/local/m2dev-acceptance/server/channels"

INT_FIELDS = {
    "schema", "ch", "port", "pid", "uptime_s", "window_ms", "users_local", "descs_total", "chars_total", "pcs",
    "fsm_chars", "iters", "pulses", "late_pulses", "late_iters", "max_late_pulses", "work_us", "work_max_us",
    "iter_gap_max_us", "event_us", "hb_us", "chr_us", "io_us", "other_us", "events", "sent_bytes", "metrics_dropped",
    "metrics_write_errors",
}
FLOAT_FIELDS = {"busy_pct"}
SECTIONS = ("event_us", "hb_us", "chr_us", "io_us", "other_us")
# Two ticks at PASSES_PER_SEC 60: a longer gap between iteration starts means the loop stalled for at least one tick
STALL_GAP_US = 33334


def parse_line(line):
    parts = line.split()
    if len(parts) < 2:
        return None
    try:
        ts = datetime.strptime(parts[0], "%Y-%m-%dT%H:%M:%S%z")
    except ValueError:
        return None
    rec = {"ts": ts}
    for token in parts[1:]:
        key, sep, value = token.partition("=")
        if not sep:
            continue
        try:
            if key in INT_FIELDS:
                rec[key] = int(value)
            elif key in FLOAT_FIELDS:
                rec[key] = float(value)
            else:
                rec[key] = value
        except ValueError:
            rec[key] = value
    return rec if "host" in rec else None


def find_files(base, days):
    pattern_dirs = [os.path.join(base, "log"), os.path.join(base, "*", "log"), os.path.join(base, "*", "*", "log")]
    wanted = {(datetime.now() - timedelta(days=d)).strftime("%Y-%m-%d") for d in range(days + 1)}
    files = []
    for d in pattern_dirs:
        for path in glob.glob(os.path.join(d, "metrics_*.log")):
            day = os.path.basename(path)[len("metrics_"):-len(".log")]
            if day in wanted:
                files.append(path)
    return sorted(set(files))


def fmt_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} GB"


def summarise(records, lag_list_limit):
    by_host = defaultdict(list)
    for r in records:
        by_host[r["host"]].append(r)

    for host in sorted(by_host):
        rows = sorted(by_host[host], key=lambda r: r["ts"])
        first, last = rows[0]["ts"], rows[-1]["ts"]
        pids = []
        for r in rows:
            if r.get("pid") is not None and (not pids or pids[-1] != r["pid"]):
                pids.append(r["pid"])

        total_window_ms = sum(r.get("window_ms", 0) for r in rows) or 1
        total_work = sum(r.get("work_us", 0) for r in rows)
        late_rows = [r for r in rows if r.get("late_pulses", 0) > 0 or r.get("iter_gap_max_us", 0) >= STALL_GAP_US]

        print(f"== {host} (ch {rows[-1].get('ch', '?')}, port {rows[-1].get('port', '?')})  "
              f"{first:%Y-%m-%d %H:%M:%S} .. {last:%H:%M:%S}  windows={len(rows)}  "
              f"process starts seen={len(pids)}")
        print(f"   users_local max {max(r.get('users_local', 0) for r in rows)}, "
              f"pcs max {max(r.get('pcs', 0) for r in rows)}, "
              f"chars_total max {max(r.get('chars_total', 0) for r in rows)}, "
              f"fsm_chars max {max(r.get('fsm_chars', 0) for r in rows)}, "
              f"descs_total max {max(r.get('descs_total', 0) for r in rows)}")
        print(f"   busy {100.0 * total_work / (total_window_ms * 1000):.2f}% avg, "
              f"{max(r.get('busy_pct', 0.0) for r in rows):.2f}% worst window; "
              f"longest iteration {max(r.get('work_max_us', 0) for r in rows) / 1000:.2f} ms, "
              f"longest gap between iterations {max(r.get('iter_gap_max_us', 0) for r in rows) / 1000:.2f} ms")
        if total_work > 0:
            shares = ", ".join(f"{s[:-3]} {100.0 * sum(r.get(s, 0) for r in rows) / total_work:.1f}%" for s in SECTIONS)
            print(f"   work split: {shares}")
        sent = sum(r.get("sent_bytes", 0) for r in rows)
        print(f"   sent {fmt_bytes(sent)} total, {fmt_bytes(1000.0 * sent / total_window_ms)}/s avg; "
              f"events {sum(r.get('events', 0) for r in rows)}")
        print(f"   late pulses {sum(r.get('late_pulses', 0) for r in rows)}; {len(late_rows)} window(s) with late pulses "
              f"or a gap >= {STALL_GAP_US / 1000:.1f} ms; metrics_dropped {rows[-1].get('metrics_dropped', 0)}, "
              f"metrics_write_errors {rows[-1].get('metrics_write_errors', 0)} (since process start)")
        for r in late_rows[-lag_list_limit:]:
            # gap = real stall; work_max close to the gap = the loop's own work, much smaller = the process did not run
            print(f"     {r['ts']:%Y-%m-%d %H:%M:%S}  gap_max {r.get('iter_gap_max_us', 0) / 1000:.1f} ms  "
                  f"work_max {r.get('work_max_us', 0) / 1000:.1f} ms  late_pulses={r.get('late_pulses')} "
                  f"max_late_pulses={r.get('max_late_pulses')}  users_local={r.get('users_local')}")
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help=f"channels directory (default {DEFAULT_DIR})")
    ap.add_argument("--hours", type=float, default=1.0, help="look back this many hours (default 1)")
    ap.add_argument("--host", help="only this host, e.g. channel1_1")
    ap.add_argument("--lags", type=int, default=10, help="late windows to list per host (default 10)")
    ap.add_argument("--raw", action="store_true", help="print matching lines instead of a summary")
    args = ap.parse_args()

    since = datetime.now(timezone.utc) - timedelta(hours=args.hours)
    files = find_files(args.dir, int(args.hours // 24) + 1)
    if not files:
        print(f"no metrics files under {args.dir}", file=sys.stderr)
        return 1

    records = []
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as fp:
            for line in fp:
                rec = parse_line(line)
                if not rec or rec["ts"] < since or (args.host and rec["host"] != args.host):
                    continue
                if args.raw:
                    sys.stdout.write(line)
                else:
                    records.append(rec)

    if not args.raw:
        if not records:
            print(f"no lines in the last {args.hours:g} h", file=sys.stderr)
            return 1
        summarise(records, args.lags)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # Output piped into head/less that exited early; point stdout at devnull so the interpreter's final flush
        # does not raise again (https://docs.python.org/3/library/signal.html#note-on-sigpipe)
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(1)
