#!/bin/sh
# Builds m2dev-dbstat and its unit tests on the DB host. Needs only the installed MariaDB package (mariadb_config,
# libmariadb) and FreeBSD base (libkvm, libdevstat); nothing from the game build tree.
#   sh build.sh [out dir]        -> <out>/m2dev-dbstat, <out>/dbstat_test (and runs the unit tests)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
mkdir -p "$OUT"
LIBDIR=$(mariadb_config --libs | sed -n 's/.*-L\([^ ]*\).*/\1/p')
c++ -std=c++20 -O2 -Wall -Wextra $(mariadb_config --cflags) -o "$OUT/m2dev-dbstat" "$HERE/m2dev-dbstat.cpp" \
	$(mariadb_config --libs) -Wl,-rpath,"$LIBDIR" -lkvm -ldevstat
c++ -std=c++20 -O2 -Wall -Wextra -o "$OUT/dbstat_test" "$HERE/dbstat_test.cpp"
"$OUT/dbstat_test"
