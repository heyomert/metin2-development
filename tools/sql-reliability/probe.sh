#!/bin/sh
# TEST/PROBE TOOL ONLY. Runs sqlprobe.cpp (AsyncSQL fix design probes, docs/engineering/db-step2-asyncsql-fix.md) against a
# TEMPORARY MariaDB it starts itself on 127.0.0.1:$RT_PORT, with the real item/quest/guild_member/player definitions from
# server/sql/player.sql and account from account.sql (read only). Never touches the live server: same fail-closed checks
# as run.sh (temporary datadir, random per-run password and token, port must be free, guard self-test before any probe).
# The probes stop and restart only that temporary server.
#
#   sh tools/sql-reliability/probe.sh [scenario ...]      (as root on the build host; default: all)
#   SRC=<server-src> BUILD=<cmake build dir> PLAYER_SQL=<server/sql/player.sql> RUNS=3 RT_PORT=3398
set -eu
SRC=${SRC:-/usr/local/m2dev-acceptance/server-src}
BUILD=${BUILD:-/root/build-verify}
PLAYER_SQL=${PLAYER_SQL:-/usr/local/m2dev-acceptance/server/sql/player.sql}
ACCOUNT_SQL=${ACCOUNT_SQL:-$(dirname "$PLAYER_SQL")/account.sql}
RUNS=${RUNS:-3}
RT_PORT=${RT_PORT:-3398}
W=/var/tmp/m2sqlprobe
SOCK=$W/run/mysqld.sock
HERE=$(cd "$(dirname "$0")" && pwd)
SCENARIOS=${*:-"R1 R2 R3 R4 R5 P0 P1 P1b P1c P2 P3 P4 P5 G1 G2 K7c K7a K7b K7d H1a H1b H1c"}

cleanup() {
	[ -S "$SOCK" ] && mariadb-admin --socket="$SOCK" shutdown > /dev/null 2>&1 || true
	sleep 2
	rm -rf "$W"
}
trap cleanup EXIT

if sockstat -4 -6 -l | awk '{ print $6 }' | grep -q -E "[:.]$RT_PORT\$"; then
	echo "port $RT_PORT is already in use; refusing to start (set RT_PORT)"
	exit 1
fi
[ -f "$PLAYER_SQL" ] || { echo "PLAYER_SQL $PLAYER_SQL not found"; exit 1; }

rm -rf "$W"
mkdir -p "$W/data" "$W/run" "$W/runs"
chmod 0711 "$W"
chown mysql:mysql "$W/data" "$W/run"

c++ -std=c++20 -O1 -DOS_FREEBSD -I"$SRC/src" -I"$SRC/include" \
	-I"$BUILD/vendor/mariadb-connector-c-3.4.5/include" -I"$SRC/vendor/mariadb-connector-c-3.4.5/include" \
	-o "$W/sqlprobe" "$HERE/sqlprobe.cpp" \
	"$BUILD/lib/liblibsql.a" "$BUILD/lib/liblibthecore.a" "$BUILD/lib/libmariadbclient.a" "$BUILD/lib/libspdlog.a" \
	-lssl -lcrypto -lmd -lpthread -lm

mariadb-install-db --no-defaults --user=mysql --datadir="$W/data" --auth-root-authentication-method=socket \
	--auth-root-socket-user=root --skip-test-db > "$W/install.log" 2>&1
RT_START="/usr/local/libexec/mariadbd --no-defaults --user=mysql --datadir=$W/data --socket=$SOCK \
--pid-file=$W/run/mysqld.pid --log-error=$W/run/mysqld.err --port=$RT_PORT --bind-address=127.0.0.1 \
--sql-mode=NO_ENGINE_SUBSTITUTION > /dev/null 2>&1 &"
sh -c "$RT_START"
i=0
until mariadb-admin --socket="$SOCK" ping > /dev/null 2>&1; do
	i=$((i + 1)); [ "$i" -lt 60 ] || { echo "temporary MariaDB did not start"; exit 1; }
	sleep 1
done
[ "$(mariadb --socket="$SOCK" -N -e 'SELECT @@datadir')" = "$W/data/" ] || { echo "ABORT: not the temporary server"; exit 1; }

RT_PW=$(openssl rand -hex 16)
RT_TOKEN=$(openssl rand -hex 32)
mariadb --socket="$SOCK" -e "CREATE USER rt@'127.0.0.1' IDENTIFIED BY '$RT_PW'; GRANT ALL ON *.* TO rt@'127.0.0.1';
	CREATE DATABASE m2sqlrt_guard; CREATE TABLE m2sqlrt_guard.token (token VARCHAR(64) NOT NULL) ENGINE=InnoDB;
	INSERT INTO m2sqlrt_guard.token VALUES ('$RT_TOKEN'); CREATE DATABASE player; CREATE DATABASE account"
# Real definitions, engines included (item/quest InnoDB, guild_member/player Aria)
for t in item quest guild_member player; do
	awk -v t="$t" 'index($0, "CREATE TABLE `" t "`") { p = 1 } p { print } p && /;[[:space:]]*$/ { exit }' "$PLAYER_SQL" > "$W/ddl-$t.sql"
	mariadb --socket="$SOCK" player < "$W/ddl-$t.sql"
done
awk 'index($0, "CREATE TABLE `account`") { p = 1 } p { print } p && /;[[:space:]]*$/ { exit }' "$ACCOUNT_SQL" > "$W/ddl-account.sql"
mariadb --socket="$SOCK" account < "$W/ddl-account.sql"
mariadb --socket="$SOCK" -N -e "SELECT table_name, engine FROM information_schema.tables WHERE table_schema IN ('player', 'account') ORDER BY 1" \
	| tr '\t' '=' | tr '\n' ' ' | sed 's/^/engines: /'; echo
export RT_PORT RT_PW RT_TOKEN RT_START
export RT_SOCK="$SOCK"

# Guard self-test (fail-closed): each case must be refused before the probe writes anything, otherwise stop everything
guard_case() { # name, expected message, env assignments...
	name=$1; expect=$2; shift 2
	out=$(cd "$W" && env "$@" "$W/sqlprobe" R1 2>&1) && rc=0 || rc=$?
	if [ "$rc" -eq 3 ] && echo "$out" | grep -q "$expect"; then
		echo "guard self-test: $name refused ok"
	else
		echo "guard self-test: $name NOT refused (rc=$rc): $out"
		exit 1
	fi
}
guard_case "no token" "RT_TOKEN missing" RT_TOKEN=
guard_case "wrong token" "run token does not match" RT_TOKEN=$(openssl rand -hex 32)
if [ -S /var/run/mysql/mysql.sock ]; then
	guard_case "live server's socket" "refusing to run against datadir" RT_SOCK=/var/run/mysql/mysql.sock
fi

for s in $SCENARIOS; do
	for r in $(seq 1 "$RUNS"); do
		d="$W/runs/$s-$r"
		mkdir -p "$d"
		# Not timeout(1): it reaps every descendant, including a server the probe restarted, and would wait for it
		( cd "$d" && exec "$W/sqlprobe" "$s" > out.txt 2>&1 ) & pid=$!
		( sleep 120; kill -9 "$pid" 2> /dev/null ) & dog=$!
		wait "$pid" && rc=0 || rc=$?
		kill "$dog" 2> /dev/null || true
		[ "$rc" -eq 0 ] || echo "$s run $r: exit $rc" >> "$d/out.txt"
		mariadb-admin --socket="$SOCK" ping > /dev/null 2>&1 || { sh -c "$RT_START"; sleep 3; }
	done
	same=0
	for r in $(seq 1 "$RUNS"); do cmp -s "$W/runs/$s-1/out.txt" "$W/runs/$s-$r/out.txt" && same=$((same + 1)); done
	echo "=== $s: $same/$RUNS runs identical"
	cat "$W/runs/$s-1/out.txt"
	[ "$same" -eq "$RUNS" ] || for r in $(seq 2 "$RUNS"); do
		cmp -s "$W/runs/$s-1/out.txt" "$W/runs/$s-$r/out.txt" || { echo "--- run $r:"; cat "$W/runs/$s-$r/out.txt"; }
	done
done
