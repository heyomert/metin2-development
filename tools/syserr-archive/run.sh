#!/bin/sh
# Tests the syserr.log archiving of libthecore (roadmap T-1) in throwaway directories, never in a live process directory.
# Every case runs the real libthecore logger through harness.cpp; failures are real (chflags schg, a file where log/
# should be, a directory and a symlink with archive names), not injected.
#
#   sh tools/syserr-archive/run.sh [cases|load]      (as root on the build host, FreeBSD; default: cases)
#   SRC=<server-src> BUILD=<cmake build dir> LINES=200000 REPS=5
set -u
MODE=${1:-cases}
SRC=${SRC:-/usr/local/m2dev-acceptance/server-src}
BUILD=${BUILD:-/root/build-verify}
LINES=${LINES:-200000}
REPS=${REPS:-5}
W=/var/tmp/m2syserr-test
HERE=$(cd "$(dirname "$0")" && pwd)
H=$W/harness
FAIL=0
ok() { echo "  [ok] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
cleanup() { chflags -R noschg "$W" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
ulimit -c 0 # the crash cases must not leave core files
cleanup
mkdir -p "$W"

c++ -std=c++20 -O1 -DOS_FREEBSD -I"$SRC/src" -I"$SRC/include" -I"$SRC/vendor/spdlog-1.15.3/include" \
	-DSPDLOG_COMPILED_LIB -o "$H" "$HERE/harness.cpp" "$BUILD/lib/liblibthecore.a" "$BUILD/lib/libspdlog.a" -lmd -lpthread \
	|| { echo "harness build failed"; exit 1; }

case_dir() { D=$W/$1; mkdir -p "$D"; cd "$D" || exit 1; }
run() { "$H" run "$1" > /dev/null 2>&1; }
warnings() { grep -c 'SYSERR_ARCHIVE' syserr.log 2>/dev/null || true; } # grep -c prints 0 itself
archives() { ls log 2>/dev/null | grep -c '^syserr_' || true; }
old_syserr() { printf '[2026-10-05 22:59:29.000] [error] [x()] OLD RUN %s\n' "$1" > syserr.log; touch -t 202610052259.30 syserr.log; }
STAMP=2026-10-05_22-59-30 # local time of the touch above, the name the archive must get

if [ "$MODE" = load ]; then
	echo "producer cost of $LINES syserr lines, flush_on(err) off/on, $REPS alternating runs each (fresh dir per run)"
	for r in $(jot "$REPS"); do
		for f in 0 1; do
			case_dir "load-$r-$f"
			"$H" load "$LINES" $f
			n=$(grep -c 'UNKNOWN HEADER' syserr.log)
			[ "$n" = "$LINES" ] || bad "flush=$f run $r: $n of $LINES lines on disk"
			case_dir "total-$r-$f"
			"$H" load "$LINES" $f total
			n=$(grep -c 'UNKNOWN HEADER' syserr.log)
			[ "$n" = "$LINES" ] || bad "flush=$f total run $r: $n of $LINES lines on disk"
			rm -rf "$W/load-$r-$f" "$W/total-$r-$f" # ~50 MB each
		done
	done
	echo; [ $FAIL -eq 0 ] && echo "LOAD COMPLETE (every line on disk)" || echo "LOAD FAILED ($FAIL)"
	exit $FAIL
fi

echo "1. nothing to archive"
case_dir none; run c1
grep -q 'RUN c1' syserr.log && [ ! -e log ] && [ "$(warnings)" = 0 ] && ok "no syserr.log: new file, no log/ created, no warning" || bad "no syserr.log"
case_dir empty; : > syserr.log; mkdir log; run c2
grep -q 'RUN c2' syserr.log && [ "$(archives)" = 0 ] && [ "$(warnings)" = 0 ] && ok "empty syserr.log: not archived" || bad "empty syserr.log"

echo "2. the previous run is archived, syserr.log holds only this run"
case_dir archive; mkdir log; old_syserr a; sum=$(sha256 -q syserr.log); run c3
[ "$(sha256 -q log/syserr_$STAMP.log 2>/dev/null)" = "$sum" ] && ok "archived as log/syserr_$STAMP.log (named after its last write), content identical" || bad "archive missing or different: $(ls log)"
grep -q 'RUN c3' syserr.log && ! grep -q 'OLD RUN' syserr.log && ok "syserr.log holds only this run" || bad "syserr.log content"
grep -q "SYSERR_ARCHIVE: previous run moved to log/syserr_$STAMP.log" syslog.log && [ "$(warnings)" = 0 ] \
	&& ok "success noted in syslog, nothing added to syserr" || bad "notice/warning placement"
case_dir nologdir; old_syserr b; sum=$(sha256 -q syserr.log); run c4
[ -d log ] && [ "$(sha256 -q log/syserr_$STAMP.log 2>/dev/null)" = "$sum" ] && ok "log/ missing: created, archived" || bad "log/ missing"
case_dir collision; mkdir log; printf 'other\n' > log/syserr_$STAMP.log; other=$(sha256 -q log/syserr_$STAMP.log)
old_syserr c; sum=$(sha256 -q syserr.log); run c5
[ "$(sha256 -q log/syserr_${STAMP}_2.log 2>/dev/null)" = "$sum" ] && [ "$(sha256 -q log/syserr_$STAMP.log)" = "$other" ] \
	&& ok "name taken: archived as _2, the existing archive untouched" || bad "collision: $(ls log)"

echo "3. archiving impossible -> nothing is lost, appended, warned"
for c in logfile logimmutable; do
	case_dir $c; old_syserr $c; size=$(stat -f %z syserr.log); sum=$(sha256 -q syserr.log)
	if [ $c = logfile ]; then printf 'not a dir\n' > log; else mkdir log; chflags schg log; fi
	run $c
	[ "$(head -c "$size" syserr.log | sha256 -q)" = "$sum" ] && grep -q "RUN $c" syserr.log && [ "$(warnings)" = 1 ] \
		&& ok "$c: previous run intact at the start of syserr.log, this run appended, one warning ($(grep -o 'SYSERR_ARCHIVE: [^;]*' syserr.log | cut -c1-70))" \
		|| bad "$c: $(cat syserr.log)"
	chflags noschg log 2>/dev/null
done

echo "4. retention: newest $((30)) archives, nothing else touched"
case_dir prune; mkdir log
printf 'x\n' > log/syserr_2026-01-01_00-00-00.log; printf 'x\n' > log/syserr_2026-01-01_00-00-00_2.log; printf 'x\n' > log/syserr_2026-01-01_00-00-00_10.log
for d in $(jot 28 2); do printf 'x\n' > "log/syserr_2026-01-$(printf %02d "$d")_00-00-00.log"; done # 01-02 .. 01-29
for f in syslog_2020-01-01.log metrics_2020-01-01.log sql_2020-01-01.log syserr_x.log syserr_2020-01-01_00-00-00.log.bak \
	SYSERR_2020-01-01_00-00-00.log syserr_2020-01-01_00-00-00_0.log syserr_2020-1-01_00-00-00.log; do printf 'keep\n' > "log/$f"; done
mkdir log/syserr_2019-01-01_00-00-00.log; printf 'target\n' > "$W/prune-target"; ln -s "$W/prune-target" log/syserr_2019-02-02_00-00-00.log
old_syserr p; run c8 # 31 archives + the new one = 32 -> the 2 oldest go: 01-01 and 01-01_2 (numeric suffix order, not string)
[ "$(ls log | grep -c '^syserr_2026-')" = 30 ] && [ ! -e log/syserr_2026-01-01_00-00-00.log ] && [ ! -e log/syserr_2026-01-01_00-00-00_2.log ] \
	&& [ -e log/syserr_2026-01-01_00-00-00_10.log ] && [ -e log/syserr_$STAMP.log ] \
	&& ok "32 -> 30: removed 01-01 and 01-01_2, kept 01-01_10 (numeric suffix order) and the new archive" || bad "retention: $(ls log | tr '\n' ' ')"
kept=0; for f in syslog_2020-01-01.log metrics_2020-01-01.log sql_2020-01-01.log syserr_x.log syserr_2020-01-01_00-00-00.log.bak \
	SYSERR_2020-01-01_00-00-00.log syserr_2020-01-01_00-00-00_0.log syserr_2020-1-01_00-00-00.log; do [ -f "log/$f" ] && kept=$((kept + 1)); done
[ $kept = 8 ] && [ -d log/syserr_2019-01-01_00-00-00.log ] && [ -L log/syserr_2019-02-02_00-00-00.log ] && [ -f "$W/prune-target" ] \
	&& ok "untouched: syslog_/metrics_/sql_, 5 near-miss names, a directory and a symlink with archive names, the symlink target" \
	|| bad "something else was removed (kept $kept/8): $(ls log | tr '\n' ' ')"
grep -q 'RUN c8' syserr.log && [ "$(warnings)" = 0 ] && ok "active syserr.log untouched by pruning, no warning" || bad "prune run"

case_dir clockback; mkdir log # 30 archives named in the "future" (clock set back since): the new one sorts oldest
for d in $(jot 30 1); do printf 'x\n' > "log/syserr_2030-01-$(printf %02d "$d")_00-00-00.log"; done
old_syserr cb; sum=$(sha256 -q syserr.log); run cb
[ "$(sha256 -q log/syserr_$STAMP.log 2>/dev/null)" = "$sum" ] && [ ! -e log/syserr_2030-01-01_00-00-00.log ] && [ "$(archives)" = 30 ] \
	&& ok "clock set back: the just-archived run is kept, the oldest other archive goes (30 total)" || bad "clock back: $(ls log | tr '\n' ' ')"

case_dir prunefail; mkdir log
for d in $(jot 31 1); do printf 'x\n' > "log/syserr_2026-02-$(printf %02d "$d")_00-00-00.log"; done
chflags schg log/syserr_2026-02-01_00-00-00.log
old_syserr pf; run c9 # 32 -> remove 2, the oldest cannot be removed
[ -e log/syserr_2026-02-01_00-00-00.log ] && [ ! -e log/syserr_2026-02-02_00-00-00.log ] && [ -e log/syserr_$STAMP.log ] \
	&& grep -q 'RUN c9' syserr.log && grep -q 'SYSERR_ARCHIVE: 1 old archive(s) could not be removed' syserr.log \
	&& ok "a removal fails: the others still pruned, one warning, the run continues" || bad "prune failure: $(cat syserr.log)"
chflags noschg log/syserr_2026-02-01_00-00-00.log

echo "5. consecutive runs"
case_dir runs; mkdir log; run r1; sleep 1; run r2; sleep 1; run r3
[ "$(archives)" = 2 ] && grep -l 'RUN r1' log/syserr_* > /dev/null && grep -l 'RUN r2' log/syserr_* > /dev/null \
	&& grep -q 'RUN r3' syserr.log && ! grep -q 'RUN r[12]' syserr.log && ok "3 runs: r1 and r2 archived, syserr.log = r3" || bad "consecutive runs: $(ls log)"

echo "6. crash evidence (flush_on(err))"
for m in segv abort-now; do
	n=0; for i in $(jot 20); do case_dir "$m-$i"; "$H" $m "crash-$i" > /dev/null 2>&1; grep -q "RUN crash-$i" syserr.log 2>/dev/null && n=$((n + 1)); done
	if [ $m = segv ]; then [ $n = 20 ] && ok "SIGSEGV 50 ms after the line: $n/20 on disk" || bad "SIGSEGV: only $n/20 on disk"
	else echo "  [info] abort() right after the line (the CHECKPOINT path): $n/20 on disk; known limit, roadmap technical debt"; fi
done
case_dir segv-1; run after
grep -l 'RUN crash-1' log/syserr_* > /dev/null 2>&1 && grep -q 'RUN after' syserr.log && ok "the crashed run's syserr.log is archived by the next start" || bad "crash run not archived"

echo
[ $FAIL -eq 0 ] && echo "PASSED" || echo "FAILED ($FAIL)"
exit $FAIL
