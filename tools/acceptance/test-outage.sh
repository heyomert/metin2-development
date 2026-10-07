#!/bin/sh
# Tests for outage.sh with mocks: never touches MariaDB or the game server.
# Mocks come first in PATH (service, procstat, hostname, sleep, date +%M); every case checks that "service" resolves to
# the mock before running, and stops otherwise. The server directory is a throwaway tree under a temporary directory.
#
#   sh tools/acceptance/test-outage.sh      (FreeBSD sh, as root: outage.sh refuses non-root before any other check)
#
# Cases: A start fails; B start reports success but status stays down; C normal; D1 SIGTERM during the outage with a
# working start; D2 SIGTERM with a failing start; E masking of raw SQL and other data; F options without a value.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/outage.sh
[ "$(id -u)" = 0 ] || { echo "run as root (outage.sh refuses non-root first)"; exit 2; }
T=$(mktemp -d /tmp/outage-test.XXXXXX) || exit 2
trap 'rm -rf "$T"' EXIT
M=$T/mock
mkdir -p "$M"
FAILS=0
ok() { echo "  [ok] $1"; }
bad() { echo "  [FAIL] $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }

# --- mocks ----------------------------------------------------------------------------------------------------------
cat > "$M/service" <<'EOF'
#!/bin/sh
# mock: service mysql-server status|stop|start; state in $MOCK_DIR/state, behaviour in $MOCK_DIR/mode
echo "$*" >> "$MOCK_DIR/calls"
[ "$1" = mysql-server ] || exit 1
case $2 in
	status) [ "$(cat "$MOCK_DIR/state")" = running ] ;;
	stop) echo stopped > "$MOCK_DIR/state" ;;
	start)
		case $(cat "$MOCK_DIR/mode") in
			normal) echo running > "$MOCK_DIR/state" ;;
			start_fails) exit 1 ;;
			start_ok_status_down) exit 0 ;;
		esac ;;
	*) exit 1 ;;
esac
EOF
cat > "$M/procstat" <<'EOF'
#!/bin/sh
echo "  PID COMM PATH"
echo " 1001 db /usr/local/m2dev-acceptance/server/share/bin/db"
echo " 1002 game /usr/local/m2dev-acceptance/server/share/bin/game"
EOF
cat > "$M/hostname" <<'EOF'
#!/bin/sh
echo m2dev-acceptance
EOF
cat > "$M/date" <<'EOF'
#!/bin/sh
# "+%M" -> a minute outside the hot-backup window; everything else -> the real date
[ "$*" = "+%M" ] && { echo 42; exit 0; }
exec /bin/date "$@"
EOF
cat > "$M/sleep" <<'EOF'
#!/bin/sh
# Injects test lines into the fake syserr on the first call; sleeps for real only while MOCK_DIR/real_sleep exists
# and no start was issued yet (so a SIGTERM can arrive during the outage, but recovery waits are instant)
if [ -f "$MOCK_DIR/inject" ] && [ ! -f "$MOCK_DIR/injected" ]; then
	cat "$MOCK_DIR/inject" >> "$MOCK_SYSERR"; : > "$MOCK_DIR/injected"
fi
if [ -f "$MOCK_DIR/real_sleep" ] && ! grep -q start "$MOCK_DIR/calls" 2>/dev/null; then exec /bin/sleep "$@"; fi
exit 0
EOF
chmod 0755 "$M"/*

fake_server() {
	rm -rf "$T/server" "$T/out"
	for p in db auth channel1/core1; do
		mkdir -p "$T/server/channels/$p/log"
		printf '[2026-10-07 10:00:00.000] [error] [int pid_init()()] \nStart of pid: 1\n\n' > "$T/server/channels/$p/syserr.log"
		echo "component=x commit=0000000000000000000000000000000000000000 dirty=0 src=archive" > "$T/server/channels/$p/version.txt"
		echo "$(date +%Y-%m-%dT%H:%M:%S)+03:00 schema=1 src=sql kind=sum host=x q=0" > "$T/server/channels/$p/log/sql_2026-10-07.log"
	done
}

# run <case> <mode> [real_sleep] -- runs outage.sh with mocks; sets RC and OUTDIR
run() {
	name=$1 mode=$2
	fake_server
	rm -f "$M/calls" "$M/injected" "$M/real_sleep"
	echo running > "$M/state"
	echo "$mode" > "$M/mode"
	export MOCK_DIR=$M MOCK_SYSERR=$T/server/channels/db/syserr.log
	resolved=$(PATH=$M:$PATH command -v service)
	[ "$resolved" = "$M/service" ] || { echo "ABORT: service resolves to $resolved, not the mock"; exit 2; }
	PATH=$M:$PATH sh "$SCRIPT" --test-vm m2dev-acceptance --seconds 3 --label "$name" --after 10 \
		--server-dir "$T/server" --out "$T/out" > "$T/stdout-$name" 2> "$T/stderr-$name"
	RC=$?
	OUTDIR=$(ls -d "$T"/out/"$name"-* 2>/dev/null | head -1)
}

echo "A. start fails"
run a start_fails
check '[ "$RC" -ne 0 ]' "exit non-zero (rc=$RC)"
check 'grep -q "result=FAILED" "$OUTDIR/summary.txt"' "summary result=FAILED"
check 'grep -q "manual_recovery=" "$OUTDIR/summary.txt"' "summary names the manual recovery command"
check '[ "$(grep -c "^mysql-server start" "$M/calls")" -eq 2 ]' "two start attempts (normal path + last attempt on exit)"
check '! grep -q "^pids_after=" "$OUTDIR/summary.txt"' "no after-evidence collected"
check '! grep -q "^result=ok" "$OUTDIR/summary.txt"' "no result=ok"

echo "B. start reports success, status stays down"
run b start_ok_status_down
check '[ "$RC" -ne 0 ]' "exit non-zero (rc=$RC)"
check 'grep -q "result=FAILED" "$OUTDIR/summary.txt"' "summary result=FAILED"
check '! grep -q "^mysql_started=" "$OUTDIR/summary.txt"' "never reported as started"

echo "C. normal recovery"
run c normal
check '[ "$RC" -eq 0 ]' "exit 0 (rc=$RC)"
check '[ "$(cat "$M/state")" = running ]' "MariaDB (mock) running at the end"
check 'grep -q "^mysql_started=" "$OUTDIR/summary.txt" && grep -q "^result=ok" "$OUTDIR/summary.txt"' "started, result=ok"
check 'grep -q "^pids_after=" "$OUTDIR/summary.txt"' "after-evidence collected"
check '[ "$(stat -f %Lp "$OUTDIR")" = 700 ]' "evidence directory mode 0700"
check '[ -z "$(ls -A "$OUTDIR" | grep "^\.")" ]' "no hidden leftovers"

for d in d1:normal d2:start_fails; do
	name=${d%%:*} mode=${d#*:}
	echo "D. SIGTERM during the outage ($mode)"
	fake_server
	rm -f "$M/calls" "$M/injected"
	echo running > "$M/state"; echo "$mode" > "$M/mode"; : > "$M/real_sleep"
	export MOCK_DIR=$M MOCK_SYSERR=$T/server/channels/db/syserr.log
	resolved=$(PATH=$M:$PATH command -v service)
	[ "$resolved" = "$M/service" ] || { echo "ABORT: service resolves to $resolved"; exit 2; }
	PATH=$M:$PATH sh "$SCRIPT" --test-vm m2dev-acceptance --seconds 5 --label "$name" --after 10 \
		--server-dir "$T/server" --out "$T/out" > "$T/stdout-$name" 2> "$T/stderr-$name" &
	pid=$!
	/bin/sleep 2
	kill -TERM "$pid"
	wait "$pid"; RC=$?
	rm -f "$M/real_sleep"
	OUTDIR=$(ls -d "$T"/out/"$name"-* | head -1)
	check '[ "$RC" -ne 0 ]' "exit non-zero (rc=$RC)"
	check 'grep -q "result=INTERRUPTED" "$OUTDIR/summary.txt"' "summary result=INTERRUPTED"
	check 'grep -q "^mysql-server start" "$M/calls"' "recovery start was attempted"
	if [ "$mode" = normal ]; then
		check '[ "$(cat "$M/state")" = running ] && ! grep -q "result=FAILED" "$OUTDIR/summary.txt"' "MariaDB (mock) running, no FAILED"
	else
		check 'grep -q "result=FAILED" "$OUTDIR/summary.txt"' "down MariaDB reported as FAILED, not hidden"
	fi
done

echo "E. masking (lines injected into the fake db syserr during the outage)"
H='[2026-10-07 10:00:01.000] [error] [void CAsyncSQL::ChildLoop()()] '
G='[2026-10-07 10:00:01.000] [error] [std::unique_ptr<SQLMsg> CDBManager::DirectQuery(const char *, int)()] '
cat > "$M/inject" <<EOF
${H}AsyncSQL: query failed: Can't connect (36) (query: SELECT id FROM player WHERE name='S3CR3T')
${H}AsyncSQL: query failed: x (query: select password from account where login='S3CR3T')
${H}AsyncSQL: query failed: x (query: UPDATE player SET gold=1 WHERE name='S3CR3T')
${H}AsyncSQL: query failed: x (query: insert into log values('S3CR3T'))
${H}AsyncSQL: query failed: x (query: FLUSH TABLES S3CR3T)
${H}AsyncSQL: query failed: x (query: flush privileges S3CR3T)
${H}AsyncSQL: retrying
${G}[SLOW-DB] DirectQuery(0) took 284 ms: SELECT MAX(id) FROM item WHERE x='S3CR3T'
${G}[SLOW-DB] DirectQuery(0) took 284 ms: flush S3CR3T
${G}[SLOW-DB] DirectQuery(0) took 284 ms: family=select.player id=7 S3CR3T
${H}AsyncSQL: failed role=db.player.main family=select.player id=1 phase=send errno=2002 result=not_delivered policy=transient_connection attempts=1 age_ms=0 'S3CR3T'
[2026-10-07 10:00:01.000] [error] [void CHARACTER::StateMove()()] name=S3CR3T from 192.0.2.7 AsyncSQL: failed role=x family=y id=1
${H}AsyncSQL: failed role=db.player.main family=select.player id=1 phase=send errno=2002 result=not_delivered policy=transient_connection attempts=1 age_ms=0
${H}AsyncSQL: attempt failed, retrying role=db.player.main family=select.player id=2 phase=send errno=2002 result=not_delivered policy=transient_connection attempts=1 age_ms=3
${H}AsyncSQL: recovered role=db.player.main after policy=transient_connection, 55 repeated failure(s) counted but not logged
${G}[SLOW-DB] DirectQuery(0) took 284 ms: family=select.player id=7
EOF
run e normal
rm -f "$M/inject"
F=$OUTDIR/syserr-sql-db.log
check '[ "$RC" -eq 0 ]' "run ok (rc=$RC)"
check '[ "$(grep -c S3CR3T "$OUTDIR"/* | awk -F: "{s+=\$2} END {print s}")" -eq 0 ]' "secret marker in evidence: 0"
check '[ "$(grep -c "192\.0\.2\.7" "$OUTDIR"/* | awk -F: "{s+=\$2} END {print s}")" -eq 0 ]' "IP in evidence: 0"
check '[ "$(grep -ciE "(select|update|insert|flush) " "$F")" -eq 0 ]' "raw SQL verbs (any case) in captured lines: 0"
check '[ "$(grep -c "<masked>" "$F")" -eq 12 ]' "12 lines masked (6 query:, old retrying, 3 old/forged SLOW, 1 forged 2a, 1 marker inside other data)"
check '[ "$(grep -c "family=select.player" "$F")" -eq 3 ]' "3 genuine 2a metadata lines kept with family=select.player"
check 'grep -q "recovered role=db.player.main after policy=transient_connection, 55 repeated" "$F"' "2a recovered line kept"

echo "F. options without a value (each must be refused with rc=2, nothing changed)"
for o in --test-vm --seconds --label --after --server-dir --out; do
	for form in end empty dash; do
		rm -f "$M/calls"
		case $form in
			end) PATH=$M:$PATH sh "$SCRIPT" --label x $o > /dev/null 2>&1 ;;
			empty) PATH=$M:$PATH sh "$SCRIPT" $o "" --label x > /dev/null 2>&1 ;;
			dash) PATH=$M:$PATH sh "$SCRIPT" $o --label x > /dev/null 2>&1 ;;
		esac
		rc=$?
		check '[ "$rc" -eq 2 ] && [ ! -f "$M/calls" ]' "$o ($form): rc=$rc, no service call"
	done
done
rm -f "$M/calls"
PATH=$M:$PATH sh "$SCRIPT" --bogus > /dev/null 2>&1; rc=$?
check '[ "$rc" -eq 2 ] && [ ! -f "$M/calls" ]' "unknown option: rc=$rc"

echo
[ "$FAILS" -eq 0 ] && echo "PASSED" || echo "FAILED ($FAILS)"
exit "$FAILS"
