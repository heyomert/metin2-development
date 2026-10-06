#!/bin/sh
# Integration tests for m2dev-dbstat against a TEMPORARY MariaDB (never the live one). The collector runs as the
# unprivileged OS user nobody with a USAGE-only MariaDB user (unix_socket), exactly the production access model.
#   sh integration-test.sh <path to m2dev-dbstat>        (as root on the DB host)
set -u
BIN=${1:?usage: integration-test.sh <m2dev-dbstat>}
W=/var/tmp/m2dbstat-it
S=$W/run/mysqld.sock
FAIL=0
ok() { echo "  [ok] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
check() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
server_start() {
	/usr/local/libexec/mariadbd --no-defaults --user=mysql --datadir="$W/data" --socket="$S" --pid-file="$W/run/p.pid" \
		--log-error="$W/run/e.err" --skip-networking --sql-mode=NO_ENGINE_SUBSTITUTION \
		--general-log=1 --general-log-file="$W/run/general.log" > /dev/null 2>&1 &
	until mariadb-admin --socket="$S" ping > /dev/null 2>&1; do sleep 0.2; done
}
server_stop() { mariadb-admin --socket="$S" shutdown > /dev/null 2>&1; while [ -S "$S" ]; do sleep 0.2; done; }
cleanup() { [ -S "$S" ] && server_stop; rm -rf "$W"; }
trap cleanup EXIT
q() { mariadb --socket="$S" -N -e "$1" 2>&1; }
sum() { awk -v f="$2" '/kind=db/ { for (i = 1; i <= NF; i++) { split($i, a, "="); if (a[1] == f && a[2] ~ /^[0-9]+$/) s += a[2] } } END { print s + 0 }' "$1"; }
collect() { su -m nobody -c "$BIN --socket $S --stdout $*"; }

rm -rf "$W"; mkdir -p "$W/data" "$W/run" "$W/out"; chmod 0711 "$W"; chown mysql:mysql "$W/data" "$W/run"
chmod 777 "$W/out"
cp "$BIN" "$W/m2dev-dbstat" && chmod 755 "$W/m2dev-dbstat" && BIN="$W/m2dev-dbstat"   # nobody cannot enter /root
mariadb-install-db --no-defaults --user=mysql --datadir="$W/data" --auth-root-authentication-method=socket \
	--auth-root-socket-user=root --skip-test-db > /dev/null 2>&1
server_start
[ "$(q 'SELECT @@datadir')" = "$W/data/" ] || { echo ABORT; exit 1; }
q "CREATE USER nobody@localhost IDENTIFIED VIA unix_socket;
   CREATE DATABASE t; CREATE TABLE t.inno (i INT PRIMARY KEY) ENGINE=InnoDB; INSERT INTO t.inno VALUES (1),(2);
   CREATE TABLE t.aria (i INT) ENGINE=Aria; INSERT INTO t.aria VALUES (1),(2),(3);
   CREATE TABLE t.heavy (i INT PRIMARY KEY) ENGINE=InnoDB"
echo "MariaDB $(q 'SELECT VERSION()'), collector user: $(q 'SHOW GRANTS FOR nobody@localhost')"

echo "T1 one sample as nobody (USAGE)"
collect --samples 1 > "$W/t1.txt"
check "grep -q 'kind=db .* up=1 ' $W/t1.txt" "db line up=1"
check "grep -q 'kind=db .* na=0 ' $W/t1.txt" "na=0: all 27 fields supported on this version"
check "grep -q 'kind=db .* first=1 ' $W/t1.txt" "first sample marked first=1"
check "grep -q 'kind=os .* disk=ada0 r_s=- ' $W/t1.txt" "os line, disk auto-detected from datadir (ada0), first window '-'"
check "grep -q 'kind=proc .* name=mariadbd ' $W/t1.txt && grep -q 'kind=proc .* name=m2dev-dbstat ' $W/t1.txt" "proc lines incl. mariadbd and itself"
grep -q 'name=channel1_core1' "$W/t1.txt" && ok "root-owned game core visible to nobody (name from argv[0])" || echo "  [info] no game core running"
head -c 700 "$W/t1.txt"; echo " ..."

echo "T2 counters: 2 InnoDB row-lock waits, 2 Aria table-lock waits, 1 deadlock, 40 inserts"
collect --interval 1 --samples 14 > "$W/t2.txt" & C=$!
sleep 1.5
for k in 1 2; do
	mariadb --socket="$S" -N -e "BEGIN; SELECT * FROM t.inno WHERE i=1 FOR UPDATE; DO SLEEP(1); COMMIT" > /dev/null & P=$!; sleep 0.2
	q "UPDATE t.inno SET i=1 WHERE i=1" > /dev/null; wait $P
	mariadb --socket="$S" -N -e "SELECT SLEEP(0.4) FROM t.aria" > /dev/null & P=$!; sleep 0.2
	q "UPDATE t.aria SET i = i + 0" > /dev/null; wait $P
done
mariadb --socket="$S" -N -e "SET SESSION max_recursive_iterations = 5000; BEGIN; INSERT INTO t.heavy WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<500) SELECT i FROM s; UPDATE t.inno SET i=2 WHERE i=2; DO SLEEP(1); UPDATE t.inno SET i=1 WHERE i=1; COMMIT" > /dev/null 2>&1 & P=$!
sleep 0.3
q "BEGIN; UPDATE t.inno SET i=1 WHERE i=1; UPDATE t.inno SET i=2 WHERE i=2; COMMIT" > /dev/null 2>&1; wait $P
i=0; while [ $i -lt 40 ]; do q "INSERT INTO t.heavy VALUES ($((1000 + i)))" > /dev/null; i=$((i + 1)); done
wait $C
check "[ $(sum $W/t2.txt row_lock_waits) -ge 3 ]" "row_lock_waits sum >= 3 (2 waits + the deadlock's wait(s); InnoDB may count both sides) [got $(sum $W/t2.txt row_lock_waits)]"
check "[ $(sum $W/t2.txt row_lock_time_ms) -ge 1500 ]" "row_lock_time_ms sum >= 1500 [got $(sum $W/t2.txt row_lock_time_ms)]"
check "[ $(sum $W/t2.txt table_locks_waited) -eq 2 ]" "table_locks_waited sum = 2 [got $(sum $W/t2.txt table_locks_waited)]"
check "[ $(sum $W/t2.txt deadlocks) -eq 1 ]" "deadlocks sum = 1 [got $(sum $W/t2.txt deadlocks)]"
# exactly the 40 single-row INSERTs: the deadlock's INSERT ... SELECT is counted by MariaDB in Com_insert_select
check "[ $(sum $W/t2.txt com_insert) -eq 40 ]" "com_insert sum = 40 [got $(sum $W/t2.txt com_insert)]"
check "! grep -q 'kind=db .*restart=1' $W/t2.txt" "no restart flagged during a normal run"

echo "T3 MariaDB restart while the collector runs (interval 2 s: backoff 2, 4, 8 s)"
collect --interval 2 --samples 22 > "$W/t3.txt" & C=$!
sleep 4
server_stop
sleep 9
server_start
wait $C
check "grep -q 'up=0 err=connect' $W/t3.txt" "up=0 err=connect while MariaDB is down"
grep 'kind=db' "$W/t3.txt" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^(up|err|retry_in_s|restart|first|questions)=/) printf "%s ", $i; print "" }' | sed 's/^/     /'
check "[ $(grep -c 'kind=db .*restart=1' $W/t3.txt) -eq 1 ]" "exactly one restart=1 after MariaDB came back"
check "! grep 'kind=db .*restart=1' $W/t3.txt | grep -q -E 'questions=[0-9]'" "no delta (no spike) on the restart line"

echo "T4 permission loss: collector user dropped, its connection killed"
collect --interval 1 --samples 6 > "$W/t4.txt" & C=$!
sleep 1.5
q "DROP USER nobody@localhost"
q "SELECT id FROM information_schema.processlist WHERE user='nobody'" | while read -r id; do q "KILL CONNECTION $id"; done
wait $C
check "grep -q 'up=0 err=auth' $W/t4.txt" "err=auth after the user is gone"
q "CREATE USER nobody@localhost IDENTIFIED VIA unix_socket"

echo "T5 statements the collector sent (general log, user nobody)"
q "FLUSH LOGS"
# Read the log in order and track who owns each thread id NOW: ids restart from 1 when MariaDB restarts (T3), so
# an id seen earlier for the collector can belong to an admin session later.
awk '{
	for (i = 2; i <= NF; i++) {
		if ($i == "Connect") { owner[$(i - 1)] = $(i + 1); break }
		if ($i == "Quit") { delete owner[$(i - 1)]; break }
		if ($i == "Query") { if (owner[$(i - 1)] == "nobody@localhost") { sub(/.*Query[ \t]+/, ""); print } break }
	}
}' "$W/run/general.log" > "$W/stmts.txt"
sed 's/IN (.*)/IN (...)/' "$W/stmts.txt" | sort | uniq -c | sed 's/^/     /'
check "! grep -v -E '^(SHOW GLOBAL STATUS WHERE Variable_name IN|SELECT VERSION\(\), @@long_query_time, @@datadir)' $W/stmts.txt | grep -q ." "only SHOW GLOBAL STATUS and the info SELECT were sent"

echo "T6 output on a full filesystem: lines dropped and counted, process keeps running"
mkdir -p "$W/full"; mount -t tmpfs -o size=64k tmpfs "$W/full"; chmod 777 "$W/full"
su -m nobody -c "$BIN --socket $S --out $W/full --interval 1 --samples 8" & C=$!
sleep 1.5; dd if=/dev/zero of="$W/full/fill" bs=4k > /dev/null 2>&1
sleep 3; rm -f "$W/full/fill"; wait $C; rc=$?
check "[ $rc -eq 0 ]" "collector exited normally (rc=$rc)"
check "grep -q -E 'write_errors=[1-9]' $W/full/dbstat_*.log" "write_errors > 0 recorded after space came back"
umount "$W/full"

echo "T7 cost: 300 samples at 1 s; collector and temporary mariadbd CPU per sample, collector RSS"
su -m nobody -c "$BIN --socket $S --out $W/out --interval 1 --samples 300" & SU=$!
sleep 2; MPID=$(cat "$W/run/p.pid")
C=$(pgrep -n -f -- "--out $W/out --interval 1 --samples 300")   # the collector itself, not su
rt() { procstat -r "$1" 2>/dev/null | awk '/ user time / || / system time / { split($NF, t, ":"); s += t[1]*3600 + t[2]*60 + t[3] } END { printf "%d\n", s * 1000000 }'; }
rss() { ps -o rss= -p "$1" | tr -d ' '; }
c0=$(rt $C); m0=$(rt $MPID); r0=$(rss $C)
sleep 280
c1=$(rt $C); m1=$(rt $MPID); r1=$(rss $C)
wait $SU
echo "     collector cpu: $(( (c1 - c0) / 280 )) us/sample, rss ${r0} KB -> ${r1} KB; mariadbd cpu: $(( (m1 - m0) / 280 )) us/sample (temporary server, incl. its own background work)"
echo "     output: $(wc -c < $W/out/dbstat_$(date +%Y-%m-%d).log) bytes for ~300 samples -> $(( $(wc -c < $W/out/dbstat_$(date +%Y-%m-%d).log) * 8640 / 300 / 1024 / 1024 )) MB/day at 10 s"
check "[ $(( r1 - r0 )) -le 256 ]" "RSS growth <= 256 KB over 280 samples"
# The collector's own cpu_us lines must agree with procstat (an earlier per-window ms truncation reported ~1/18 of it)
self=$(awk '/kind=proc .*name=m2dev-dbstat / { for (i = 1; i <= NF; i++) if ($i ~ /^cpu_us=[0-9]+$/) { split($i, a, "="); s += a[2]; n++ } } END { printf "%d\n", n ? s / n : 0 }' $W/out/dbstat_*.log)
ext=$(( (c1 - c0) / 280 ))
echo "     self-reported cpu_us/sample ${self}, procstat ${ext}"
check "[ $(( self * 100 )) -ge $(( ext * 80 )) ] && [ $(( self * 100 )) -le $(( ext * 120 )) ]" "self-reported cpu_us within 20% of procstat"

echo
[ $FAIL -eq 0 ] && echo "PASSED" || echo "FAILED ($FAIL)"
exit $FAIL
