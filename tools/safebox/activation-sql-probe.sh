#!/bin/sh
# TEST/PROBE TOOL ONLY. Runs activation-sql-probe.cpp (A-19 account safebox activation, docs/engineering/safebox-activation.md)
# against a TEMPORARY MariaDB it starts itself in a new /var/tmp/m2sbprobe-<UTC time> directory on 127.0.0.1:$RT_PORT, with
# the real safebox (Aria) and item (InnoDB) definitions from server/sql/player.sql (read only). Never touches the live server:
# temporary datadir, random per-run password and token, port must be free, guard self-test before the probe. The trap only
# shuts the temporary server down; it deletes nothing (the directory is printed for a later, approved cleanup).
#
#   sh tools/safebox/activation-sql-probe.sh      (as root on the build host)
#   SRC=<server-src> BUILD=<cmake build dir> PLAYER_SQL=<server/sql/player.sql> RUNS=3 RT_PORT=3398
set -eu
SRC=${SRC:-/usr/local/m2dev-acceptance/server-src}
BUILD=${BUILD:-/root/build-verify}
PLAYER_SQL=${PLAYER_SQL:-/usr/local/m2dev-acceptance/server/sql/player.sql}
RUNS=${RUNS:-3}
RT_PORT=${RT_PORT:-3398}
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/m2sbprobe-$(date -u +%Y%m%dT%H%M%SZ)
SOCK=$W/run/mysqld.sock

[ ! -e "$W" ] || { echo "$W exists; refusing"; exit 1; }
if sockstat -4 -6 -l | awk '{ print $6 }' | grep -q -E "[:.]$RT_PORT\$"; then
	echo "port $RT_PORT is already in use; refusing (set RT_PORT)"; exit 1
fi
[ -f "$PLAYER_SQL" ] || { echo "PLAYER_SQL $PLAYER_SQL not found"; exit 1; }
[ -f "$SRC/src/db/SafeboxActivation.h" ] || { echo "SRC $SRC has no A-19 sources"; exit 1; }

stop_tmp() { [ -S "$SOCK" ] && mariadb-admin --socket="$SOCK" shutdown > /dev/null 2>&1 || true; }
trap stop_tmp EXIT

mkdir -p "$W/data" "$W/run" "$W/out"
chmod 0711 "$W"
chown mysql:mysql "$W/data" "$W/run"
echo "workdir: $W"

c++ -std=c++20 -O1 -DOS_FREEBSD -DSPDLOG_COMPILED_LIB -I"$SRC/vendor/spdlog-1.15.3/include" -I"$SRC/src" -I"$SRC/include" \
	-I"$BUILD/vendor/mariadb-connector-c-3.4.5/include" -I"$SRC/vendor/mariadb-connector-c-3.4.5/include" \
	-o "$W/activation-sql-probe" "$HERE/activation-sql-probe.cpp" \
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
RT_RO_PW=$(openssl rand -hex 16)
RT_NOITEM_PW=$(openssl rand -hex 16)
RT_TOKEN=$(openssl rand -hex 32)
mariadb --socket="$SOCK" -e "CREATE DATABASE m2sqlrt_guard; CREATE TABLE m2sqlrt_guard.token (token VARCHAR(64) NOT NULL) ENGINE=InnoDB;
	INSERT INTO m2sqlrt_guard.token VALUES ('$RT_TOKEN'); CREATE DATABASE player"
for t in safebox item; do
	awk -v t="$t" 'index($0, "CREATE TABLE `" t "`") { p = 1 } p { print } p && /;[[:space:]]*$/ { exit }' "$PLAYER_SQL" > "$W/ddl-$t.sql"
	mariadb --socket="$SOCK" player < "$W/ddl-$t.sql"
done
mariadb --socket="$SOCK" -e "CREATE USER rt@'127.0.0.1' IDENTIFIED BY '$RT_PW'; GRANT ALL ON *.* TO rt@'127.0.0.1';
	CREATE USER rt_ro@'127.0.0.1' IDENTIFIED BY '$RT_RO_PW'; GRANT SELECT ON *.* TO rt_ro@'127.0.0.1';
	CREATE USER rt_noitem@'127.0.0.1' IDENTIFIED BY '$RT_NOITEM_PW'; GRANT SELECT ON player.safebox TO rt_noitem@'127.0.0.1'"
echo "ddl: $(mariadb --socket="$SOCK" -N -e "SELECT GROUP_CONCAT(CONCAT(table_name, '=', engine)) FROM information_schema.tables WHERE table_schema='player'")"
echo "version: $(mariadb --socket="$SOCK" -N -e 'SELECT @@version')  sql_mode(tmp): $(mariadb --socket="$SOCK" -N -e 'SELECT @@GLOBAL.sql_mode')"
export RT_PORT RT_PW RT_RO_PW RT_NOITEM_PW RT_TOKEN RT_SOCK="$SOCK"

# Guard self-test (fail-closed): each case must be refused before the probe writes anything
guard_case() {
	name=$1; expect=$2; shift 2
	out=$(cd "$W/out" && env "$@" "$W/activation-sql-probe" 2>&1) && rc=0 || rc=$?
	if [ "$rc" -eq 3 ] && echo "$out" | grep -q "$expect"; then echo "guard self-test: $name refused ok"
	else echo "guard self-test: $name NOT refused (rc=$rc)"; exit 1; fi
}
guard_case "no token" "RT_TOKEN missing" RT_TOKEN=
guard_case "wrong token" "run token does not match" RT_TOKEN=$(openssl rand -hex 32)
if [ -S /var/run/mysql/mysql.sock ]; then
	guard_case "live server's socket" "refusing to run against datadir" RT_SOCK=/var/run/mysql/mysql.sock
fi

for r in $(seq 1 "$RUNS"); do
	mkdir -p "$W/out/run$r"
	( cd "$W/out/run$r" && "$W/activation-sql-probe" > out.txt 2>&1 ) || echo "run $r exit $?" >> "$W/out/run$r/out.txt"
done
same=1
for r in $(seq 2 "$RUNS"); do cmp -s "$W/out/run1/out.txt" "$W/out/run$r/out.txt" || same=0; done
if [ "$same" -eq 1 ]; then echo "=== all $RUNS runs identical: yes"; else echo "=== all $RUNS runs identical: NO"; fi
cat "$W/out/run1/out.txt"
[ "$same" -eq 1 ] || for r in $(seq 2 "$RUNS"); do echo "--- run $r"; cat "$W/out/run$r/out.txt"; done
