#!/bin/sh
# Runs multistmt.cpp (CLIENT_MULTI_STATEMENTS dependency test) against a TEMPORARY MariaDB on a unix socket only
# (no TCP listener). The live server and its schemas are never touched.
#
#   sh tools/sql-reliability/multistmt.sh       (as root on the build host)
#   SRC=<server-src> BUILD=<cmake build dir>
set -eu
SRC=${SRC:-/usr/local/m2dev-acceptance/server-src}
BUILD=${BUILD:-/root/build-verify}
W=/var/tmp/m2multistmt
SOCK=$W/run/mysqld.sock
HERE=$(cd "$(dirname "$0")" && pwd)

cleanup() {
	[ -S "$SOCK" ] && mariadb-admin --socket="$SOCK" shutdown > /dev/null 2>&1 || true
	sleep 2
	rm -rf "$W"
}
trap cleanup EXIT

rm -rf "$W"
mkdir -p "$W/data" "$W/run"
chmod 0711 "$W"
chown mysql:mysql "$W/data" "$W/run"

c++ -std=c++20 -O1 -I"$BUILD/vendor/mariadb-connector-c-3.4.5/include" \
	-I"$SRC/vendor/mariadb-connector-c-3.4.5/include" -o "$W/multistmt" "$HERE/multistmt.cpp" \
	"$BUILD/lib/libmariadbclient.a" -lssl -lcrypto -lpthread -lm

mariadb-install-db --no-defaults --user=mysql --datadir="$W/data" --auth-root-authentication-method=socket \
	--auth-root-socket-user=root --skip-test-db > "$W/install.log" 2>&1
/usr/local/libexec/mariadbd --no-defaults --user=mysql --datadir="$W/data" --socket="$SOCK" \
	--pid-file="$W/run/mysqld.pid" --log-error="$W/run/mysqld.err" --skip-networking \
	--sql-mode=NO_ENGINE_SUBSTITUTION > /dev/null 2>&1 &
i=0
until mariadb-admin --socket="$SOCK" ping > /dev/null 2>&1; do
	i=$((i + 1)); [ "$i" -lt 60 ] || { echo "temporary MariaDB did not start"; exit 1; }
	sleep 1
done
[ "$(mariadb --socket="$SOCK" -N -e 'SELECT @@datadir')" = "$W/data/" ] || { echo "ABORT: not the temporary server"; exit 1; }
mariadb --socket="$SOCK" -e "CREATE DATABASE ms"

# root over the socket (unix_socket authentication), no password
"$W/multistmt" "$SOCK" root "" ms
