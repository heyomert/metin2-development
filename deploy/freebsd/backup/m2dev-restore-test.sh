#!/bin/sh
# Restore test for an m2dev backup (docs/backup.md). Proves that a backup file can actually be restored:
#   1. checks the .sha256, decrypts and unpacks it into a fresh temporary directory
#   2. initialises an empty, temporary MariaDB there (own socket, no network, never the live data directory)
#      and loads the dumps into it, as a real restore would
#   3. CHECK TABLE on every table (mariadb-check); every table of the manifest must exist; row counts, CHECKSUM
#      TABLE and gold sums must equal the manifest exactly (the backup read them under the same lock as the dump)
#   4. stops the temporary server and deletes everything
#
#   m2dev-restore-test.sh <m2dev-...tar.zst.age> <age identity file | ->
#
# '-' reads the private key from stdin, so it never has to be stored on the machine running the test:
#   ssh server 'm2dev-restore-test.sh /var/backups/m2dev/<file> -' < m2dev-backup.agekey
# Ends with one key=value line in $BACKUP_DIR/status-restore-test (if that directory exists) and $LOG_FILE.
set -eu
set -o pipefail
umask 077
# cron's PATH depends on the installation; mariadb*, age live in /usr/local/bin
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
export PATH

ARCHIVE=${1:-}
IDENTITY=${2:-}
if [ -z "$ARCHIVE" ] || [ -z "$IDENTITY" ]; then
	echo "usage: $0 <backup.tar.zst.age> <age identity file | ->" >&2
	exit 2
fi

CONF=${M2DEV_BACKUP_CONF:-/usr/local/etc/m2dev-backup.conf}
. "$CONF"

START=$(date +%s)
STEP=init
WORK=
MYSQLD_PID=
MODE=unknown
TABLES=0
COMPARED=0
MISMATCH=0
CHECK_WARNINGS=0

log() {
	printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >> "$LOG_FILE"
}

stop_server() {
	if [ -n "$MYSQLD_PID" ] && kill -0 "$MYSQLD_PID" 2> /dev/null; then
		mariadb-admin --socket="$WORK/run/mysqld.sock" shutdown > /dev/null 2>&1 || kill "$MYSQLD_PID" 2> /dev/null || true
		i=0
		while kill -0 "$MYSQLD_PID" 2> /dev/null && [ "$i" -lt 60 ]; do
			sleep 1
			i=$(( i + 1 ))
		done
		kill -9 "$MYSQLD_PID" 2> /dev/null || true
	fi
	MYSQLD_PID=
}

finish() {
	rc=$?
	trap - EXIT INT TERM HUP
	stop_server
	[ -n "$WORK" ] && rm -rf "$WORK"
	result=ok
	[ "$rc" -eq 0 ] || result=fail
	line="time=$(date '+%Y-%m-%dT%H:%M:%S%z') kind=restore-test mode=$MODE result=$result step=$STEP"
	line="$line duration_s=$(( $(date +%s) - START )) tables=$TABLES compared=$COMPARED mismatch=$MISMATCH"
	line="$line check_warnings=$CHECK_WARNINGS file=$(basename "$ARCHIVE")"
	if [ -d "$BACKUP_DIR" ]; then
		printf '%s\n' "$line" > "$BACKUP_DIR/.status-restore-test.tmp" &&
			mv -f "$BACKUP_DIR/.status-restore-test.tmp" "$BACKUP_DIR/status-restore-test"
	fi
	log "$line"
	echo "$line"
	exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT TERM HUP

fail() {
	echo "m2dev-restore-test: $STEP: $*" >&2
	log "m2dev-restore-test: $STEP: $*"
	exit 1
}

STEP=verify
for c in mariadb mariadb-admin mariadb-check mariadb-install-db "$MARIADBD" age zstd sha256; do
	command -v "$c" > /dev/null 2>&1 || fail "command not found: $c"
done
[ -r "$ARCHIVE" ] || fail "cannot read $ARCHIVE"
[ -r "$ARCHIVE.sha256" ] || fail "no $ARCHIVE.sha256 (the backup was not published completely)"
[ "$(sha256 -q "$ARCHIVE")" = "$(cut -d ' ' -f 1 "$ARCHIVE.sha256")" ] || fail "sha256 mismatch"

# The restored copy must fit with room to spare, so the test can never fill the disk under a live server
META="${ARCHIVE%.tar.zst.age}.meta"
[ -r "$META" ] || fail "no $META"
DUMP_KB=$(awk -F= '$1 == "dump_kb" { print $2 }' "$META")
case "$DUMP_KB" in ''|*[!0-9]*) fail "no dump_kb in $META" ;; esac
FREE_KB=$(df -k "$RESTORE_WORK_DIR" | awk 'NR == 2 { print $4 }')
NEED_KB=$(( DUMP_KB * RESTORE_SPACE_FACTOR + RESTORE_RESERVE_KB ))
[ "$FREE_KB" -ge "$NEED_KB" ] || fail "not enough space in $RESTORE_WORK_DIR: ${FREE_KB} KB free, need $NEED_KB KB"

STEP=unpack
WORK=$(mktemp -d "$RESTORE_WORK_DIR/m2dev-restore.XXXXXX")
if [ "$IDENTITY" = - ]; then
	age -d -i /dev/stdin "$ARCHIVE"
else
	age -d -i "$IDENTITY" "$ARCHIVE"
fi | zstd -dq | tar -C "$WORK" -xf - || fail "decrypt/unpack failed"
for f in m2dev-manifest locked.sql unlocked.sql; do
	[ -f "$WORK/$f" ] || fail "archive has no $f"
done
MODE=$(awk '$1 == "mode" { print $2 }' "$WORK/m2dev-manifest")

# The server runs as mysql: it may pass through the work directory (0711, not list it) and owns only its data and
# run directories; the decrypted dumps stay readable by root only
STEP=init-db
chmod 0711 "$WORK"
mkdir -m 0700 "$WORK/data" "$WORK/run"
chown mysql:mysql "$WORK/data" "$WORK/run"
mariadb-install-db --no-defaults --user=mysql --datadir="$WORK/data" --auth-root-authentication-method=socket \
	--auth-root-socket-user=root --skip-test-db > "$WORK/install.log" 2>&1 ||
	{ tail -n 20 "$WORK/install.log" >&2; fail "mariadb-install-db failed"; }

STEP=start
SOCK="$WORK/run/mysqld.sock"
nice -n "$NICE" "$MARIADBD" --no-defaults --user=mysql --datadir="$WORK/data" --socket="$SOCK" \
	--pid-file="$WORK/run/mysqld.pid" --log-error="$WORK/run/mysqld.err" --skip-networking \
	--innodb-buffer-pool-size=64M --sql-mode=NO_ENGINE_SUBSTITUTION > /dev/null 2>&1 &
MYSQLD_PID=$!
i=0
until mariadb-admin --socket="$SOCK" ping > /dev/null 2>&1; do
	kill -0 "$MYSQLD_PID" 2> /dev/null || { tail -n 20 "$WORK/run/mysqld.err" >&2; fail "temporary server exited"; }
	[ "$i" -lt 120 ] || fail "temporary server did not start in 120 s"
	sleep 1
	i=$(( i + 1 ))
done

q() {
	mariadb --socket="$SOCK" -N -e "$1"
}

STEP=load
for f in locked.sql unlocked.sql; do
	mariadb --socket="$SOCK" --default-character-set=binary < "$WORK/$f" 2> "$WORK/load.err" ||
		{ head -n 20 "$WORK/load.err" >&2; fail "loading $f failed"; }
done

STEP=check
mariadb-check --socket="$SOCK" --all-databases > "$WORK/check.txt" 2>&1 || fail "mariadb-check could not run"
if grep -q -E '^(error|Error)' "$WORK/check.txt"; then
	grep -B 1 -E '^(error|Error)' "$WORK/check.txt" >&2
	fail "CHECK TABLE reported errors"
fi
CHECK_WARNINGS=$(grep -c '^warning' "$WORK/check.txt" || true)

STEP=tables
for t in $(awk '$1 == "table" { print $2 }' "$WORK/m2dev-manifest"); do
	TABLES=$(( TABLES + 1 ))
	[ "$(q "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '${t%%.*}' AND table_name = '${t#*.}'")" = 1 ] ||
		fail "table $t is missing"
done
[ "$TABLES" -gt 0 ] || fail "manifest lists no tables"

STEP=compare
compare() { # what expected actual
	# A failed query inside $(...) does not stop the script: a missing number is an error, not a mismatch
	case "$3" in ''|*[!0-9]*) fail "query for $1 failed" ;; esac
	COMPARED=$(( COMPARED + 1 ))
	if [ "$2" != "$3" ]; then
		MISMATCH=$(( MISMATCH + 1 ))
		echo "  $1: manifest $2, restored $3" >&2
	fi
}
while read -r kind name value; do
	tbl=$name
	[ "$kind" = sum ] && tbl=${name%.*}
	ident="\`${tbl%%.*}\`.\`${tbl#*.}\`"
	case "$kind" in
		count) compare "count $name" "$value" "$(q "SELECT COUNT(*) FROM $ident")" ;;
		checksum) compare "checksum $name" "$value" "$(q "CHECKSUM TABLE $ident EXTENDED" | cut -f 2)" ;;
		sum) compare "sum $name" "$value" "$(q "SELECT COALESCE(SUM(\`${name##*.}\`), 0) FROM $ident")" ;;
	esac
done < "$WORK/m2dev-manifest"
[ "$COMPARED" -gt 0 ] || fail "manifest has nothing to compare"
[ "$MISMATCH" -eq 0 ] || fail "$MISMATCH of $COMPARED values differ from the manifest"

STEP=done
