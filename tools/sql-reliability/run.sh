#!/bin/sh
# Runs the AsyncSQL characterization scenarios (sqlrt.cpp) against a TEMPORARY MariaDB on 127.0.0.1:$RT_PORT.
# The live server and its schemas are never touched. Each scenario runs $RUNS times, each run in its own process,
# working directory and freshly created schema; the script reports whether every run produced the same lines.
#
#   sh tools/sql-reliability/run.sh [scenario ...]        (as root on the build host; default: all)
#   SRC=<server-src> BUILD=<cmake build dir> RUNS=10 RT_PORT=3399
set -eu
SRC=${SRC:-/usr/local/m2dev-acceptance/server-src}
BUILD=${BUILD:-/root/build-verify}
RUNS=${RUNS:-10}
RT_PORT=${RT_PORT:-3399}
W=/var/tmp/m2sqlrt
SOCK=$W/run/mysqld.sock
HERE=$(cd "$(dirname "$0")" && pwd)
SCENARIOS=${*:-"S1 S2 S3 S4 S5 S6 S7 S8 S9 S9b S10 S11 S12"}

cleanup() {
	[ -S "$SOCK" ] && mariadb-admin --socket="$SOCK" shutdown > /dev/null 2>&1 || true
	sleep 2
	rm -rf "$W"
}
trap cleanup EXIT

# Fail closed: never share a port with another server (the AsyncSQL side connects over TCP)
if sockstat -4 -6 -l | awk '{ print $6 }' | grep -q -E "[:.]$RT_PORT\$"; then
	echo "port $RT_PORT is already in use; refusing to start (set RT_PORT)"
	exit 1
fi

rm -rf "$W"
mkdir -p "$W/data" "$W/run" "$W/runs"
chmod 0711 "$W"
chown mysql:mysql "$W/data" "$W/run"

# Build against the same libraries the game links (libsql, libthecore, vendored Connector/C)
c++ -std=c++20 -O1 -DOS_FREEBSD -I"$SRC/src" -I"$SRC/include" \
	-I"$BUILD/vendor/mariadb-connector-c-3.4.5/include" -I"$SRC/vendor/mariadb-connector-c-3.4.5/include" \
	-o "$W/sqlrt" "$HERE/sqlrt.cpp" \
	"$BUILD/lib/liblibsql.a" "$BUILD/lib/liblibthecore.a" "$BUILD/lib/libmariadbclient.a" "$BUILD/lib/libspdlog.a" \
	-lssl -lcrypto -lmd -lpthread -lm

mariadb-install-db --no-defaults --user=mysql --datadir="$W/data" --auth-root-authentication-method=socket \
	--auth-root-socket-user=root --skip-test-db > "$W/install.log" 2>&1
/usr/local/libexec/mariadbd --no-defaults --user=mysql --datadir="$W/data" --socket="$SOCK" \
	--pid-file="$W/run/mysqld.pid" --log-error="$W/run/mysqld.err" --port="$RT_PORT" --bind-address=127.0.0.1 \
	--sql-mode=NO_ENGINE_SUBSTITUTION > /dev/null 2>&1 &
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
	INSERT INTO m2sqlrt_guard.token VALUES ('$RT_TOKEN')"
export RT_PORT RT_PW RT_TOKEN
export RT_SOCK="$SOCK"

# Guard self-test: each case must be refused before the tool writes anything; otherwise stop everything
guard_case() { # name, expected message, env assignments...
	name=$1; expect=$2; shift 2
	out=$(cd "$W" && env "$@" "$W/sqlrt" versions 2>&1) && rc=0 || rc=$?
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
	guard_case "other server's socket" "refusing to run against datadir" RT_SOCK=/var/run/mysql/mysql.sock
fi

echo "versions: $("$W/sqlrt" versions)"
for s in $SCENARIOS; do
	for r in $(seq 1 "$RUNS"); do
		d="$W/runs/$s-$r"
		mkdir -p "$d"
		( cd "$d" && timeout 120 "$W/sqlrt" "$s" > out.txt 2>&1 ) || echo "$s run $r: exit $?" >> "$d/out.txt"
	done
	# Deterministic = every run printed the same lines once the per-run counter "finished=" is ignored
	for r in $(seq 1 "$RUNS"); do
		sed 's/finished=[0-9]*//' "$W/runs/$s-$r/out.txt" > "$W/runs/$s-$r/norm.txt"
	done
	first="$W/runs/$s-1/norm.txt"
	same=0
	for r in $(seq 1 "$RUNS"); do
		cmp -s "$first" "$W/runs/$s-$r/norm.txt" && same=$((same + 1))
	done
	echo "=== $s: $same/$RUNS runs identical"
	cat "$W/runs/$s-1/out.txt"
	if [ "$same" -ne "$RUNS" ]; then
		for r in $(seq 2 "$RUNS"); do
			cmp -s "$first" "$W/runs/$s-$r/norm.txt" || { echo "--- run $r differs:"; cat "$W/runs/$s-$r/out.txt"; }
		done
	fi
done
