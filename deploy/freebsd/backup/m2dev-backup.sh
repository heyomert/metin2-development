#!/bin/sh
# Database backup for the m2dev game server (docs/backup.md).
#
#   m2dev-backup.sh hot          while the game runs (scheduled)
#   m2dev-backup.sh consistent   only while the game server is stopped: db flushed every cache on shutdown
#                                (server-src/src/db/ClientManager.cpp:191-233), so the copy is exactly the game
#                                state at shutdown. Use before deploying binaries or touching the database.
#
# Logical dump (mariadb-dump), not a physical copy: player tables are Aria, and mariadb-backup cannot restore hot
# Aria copies reliably (no Aria redo in --prepare, MDEV-18573; reproduced here, see docs/backup.md).
#
#   1. a separate session takes FLUSH TABLES WITH READ LOCK (writes wait, reads go on; gives up after
#      $LOCK_WAIT_TIMEOUT s instead of piling up behind a long query)
#   2. under that lock: row counts and CHECKSUM TABLE of $COUNT_TABLES, gold sums, the table list, and the dump of
#      users/grants + $LOCKED_DATABASES. The manifest and the dump are the same instant, so the restore test can
#      compare them exactly, hot or not
#   3. lock released; $UNLOCKED_DATABASES (append-only logs) dumped without the global lock
#   4. tar | zstd | age (public key only), .meta, .sha256 last
#
# The game's db process caches player/item data for minutes (server-src/src/db/Main.cpp:29-30): a hot backup does
# not contain the newest minutes of play. Nothing here writes to the live database.
set -eu
set -o pipefail
umask 077
# cron's PATH depends on the installation; mariadb*, age live in /usr/local/bin
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
export PATH

MODE=${1:-}
case "$MODE" in
	hot|consistent) ;;
	*) echo "usage: $0 hot|consistent" >&2; exit 2 ;;
esac

CONF=${M2DEV_BACKUP_CONF:-/usr/local/etc/m2dev-backup.conf}
. "$CONF"

# Client options file read after the global ones (socket, or a dedicated backup user and its password): MariaDB
# clients take --defaults-extra-file only as their first argument, and the environment (MYSQL_UNIX_PORT) does not
# override my.cnf. Empty: root over the default socket.
DB_OPTS=
[ -z "${MARIADB_EXTRA_FILE:-}" ] || DB_OPTS="--defaults-extra-file=$MARIADB_EXTRA_FILE"

# One run at a time: lockf re-executes this script while holding the lock, and fails at once if it is taken
if [ -z "${M2DEV_BACKUP_LOCKED:-}" ]; then
	mkdir -p "$BACKUP_DIR"
	export M2DEV_BACKUP_LOCKED=1
	exec lockf -k -t 0 "$BACKUP_DIR/.lock" "$0" "$@"
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME="m2dev-$STAMP-$MODE"
WORK="$BACKUP_DIR/.work-$STAMP"
PART="$BACKUP_DIR/.partial-$NAME"
START=$(date +%s)
STEP=init
FILE=none
SIZE=0
DUMP_KB=0
LOCK_MS=-1
LOCKER=

log() {
	printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >> "$LOG_FILE"
}

release_lock() {
	if [ -n "$LOCKER" ]; then
		# Closing the session's input ends it, and the server drops the lock with the session
		exec 3>&- 2> /dev/null || true
		wait "$LOCKER" 2> /dev/null || true
		LOCKER=
	fi
}

finish() {
	rc=$?
	trap - EXIT INT TERM HUP
	release_lock
	rm -rf "$WORK" "$PART".*
	result=ok
	[ "$rc" -eq 0 ] || result=fail
	line="time=$(date '+%Y-%m-%dT%H:%M:%S%z') kind=backup mode=$MODE result=$result step=$STEP"
	line="$line duration_s=$(( $(date +%s) - START )) lock_ms=$LOCK_MS dump_kb=$DUMP_KB size_bytes=$SIZE file=$FILE"
	printf '%s\n' "$line" > "$BACKUP_DIR/.status-backup-$MODE.tmp" &&
		mv -f "$BACKUP_DIR/.status-backup-$MODE.tmp" "$BACKUP_DIR/status-backup-$MODE"
	log "$line"
	exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT TERM HUP

fail() {
	echo "m2dev-backup: $STEP: $*" >&2
	log "m2dev-backup $MODE: $STEP: $*"
	exit 1
}

q() {
	mariadb $DB_OPTS -N -e "$1"
}

# Leftovers of a run that was killed before its cleanup could run (we hold the lock, so none is in progress)
rm -rf "$BACKUP_DIR"/.work-* "$BACKUP_DIR"/.partial-*

STEP=check
for c in mariadb mariadb-admin mariadb-dump age zstd sha256 lockf; do
	command -v "$c" > /dev/null 2>&1 || fail "command not found: $c"
done
[ -r "$AGE_RECIPIENTS_FILE" ] || fail "no age recipients file $AGE_RECIPIENTS_FILE"
mariadb-admin $DB_OPTS ping > /dev/null 2>&1 || fail "MariaDB is not running"
if [ "$MODE" = consistent ] && service m2dev status > /dev/null 2>&1; then
	fail "m2dev is running: stop it first (service m2dev stop) so db flushes its caches"
fi

# The dump is smaller than the data files; their size bounds work copy + archive
DATA_KB=$(du -sk "$(q 'SELECT @@datadir')" | cut -f1)
FREE_KB=$(df -k "$BACKUP_DIR" | awk 'NR == 2 { print $4 }')
[ "$FREE_KB" -ge $(( DATA_KB * SPACE_FACTOR )) ] ||
	fail "not enough space in $BACKUP_DIR: ${FREE_KB} KB free, need $(( DATA_KB * SPACE_FACTOR )) KB"
mkdir -p "$WORK"

# FLUSH TABLES WITH READ LOCK blocks new writes at once, then waits for writes already running. Measured on the
# test VM: behind an 8 s no-op UPDATE a probe write waited 7494 ms (a long SELECT does not hold it up: 25 ms). The
# game has a few synchronous writes on its main thread (e.g. game/guild.cpp:77, game/char_change_empire.cpp:169),
# so such a wait would freeze a core. Two guards (with them the probe waited 1806 ms and the backup gave up):
#   1. skip this run, without locking, while a statement has been running for $LONG_QUERY_SKIP_S s or more
#   2. if the lock is still not held after $LOCK_WAIT_TIMEOUT s (a long statement started after the check),
#      KILL QUERY the lock request: writes are held at most that long and the backup retries next run
STEP=precheck
LONG=$(q "SELECT COUNT(*) FROM information_schema.processlist WHERE id <> CONNECTION_ID()
	AND command NOT IN ('Sleep', 'Daemon', 'Binlog Dump') AND time >= $LONG_QUERY_SKIP_S")
[ "$LONG" = 0 ] || fail "skipped: $LONG statement(s) running for ${LONG_QUERY_SKIP_S}s or more"

STEP=lock
mkfifo "$WORK/lock.fifo"
mariadb $DB_OPTS -N --unbuffered < "$WORK/lock.fifo" > "$WORK/lock.out" 2>&1 &
LOCKER=$!
exec 3> "$WORK/lock.fifo"
echo "SELECT CONCAT('CONN ', CONNECTION_ID()); FLUSH TABLES WITH READ LOCK;
	SELECT CONCAT('LOCKED ', ROUND(UNIX_TIMESTAMP(NOW(3)) * 1000));" >&3
waited=0
until grep -q '^LOCKED ' "$WORK/lock.out"; do
	kill -0 "$LOCKER" 2> /dev/null || { cat "$WORK/lock.out" >&2; fail "could not take the global read lock"; }
	if [ "$waited" -ge $(( LOCK_WAIT_TIMEOUT * 20 )) ]; then
		conn=$(awk '$1 == "CONN" { print $2 }' "$WORK/lock.out")
		[ -n "$conn" ] && mariadb $DB_OPTS -e "KILL QUERY $conn" 2> /dev/null || true
		fail "global read lock not acquired in ${LOCK_WAIT_TIMEOUT}s (a running write held it up); request cancelled, writes released"
	fi
	sleep 0.05
	waited=$(( waited + 1 ))
done

# Everything below until the unlock must be fast: writes are waiting
STEP=manifest
all_in=$(printf "'%s'," $LOCKED_DATABASES $UNLOCKED_DATABASES)
{
	echo "SELECT CONCAT('mariadb_version ', VERSION());"
	echo "SELECT CONCAT('table ', table_schema, '.', table_name) FROM information_schema.tables
		WHERE table_type = 'BASE TABLE' AND table_schema IN (${all_in%,}) ORDER BY 1;"
	for t in $COUNT_TABLES; do
		echo "SELECT CONCAT('count $t ', COUNT(*)) FROM \`${t%%.*}\`.\`${t#*.}\`;"
	done
	for t in $CHECKSUM_TABLES; do
		echo "CHECKSUM TABLE \`${t%%.*}\`.\`${t#*.}\` EXTENDED;"
	done
	echo "SELECT CONCAT('sum player.player.gold ', COALESCE(SUM(gold), 0)) FROM player.player;"
	echo "SELECT CONCAT('sum player.safebox.gold ', COALESCE(SUM(gold), 0)) FROM player.safebox;"
} > "$WORK/manifest.sql"
{
	echo "manifest_version 2"
	echo "name $NAME"
	echo "mode $MODE"
	echo "created $(date '+%Y-%m-%dT%H:%M:%S%z')"
	echo "host $(hostname)"
	# One session for every query (a client start per query lengthened the lock); CHECKSUM TABLE prints
	# "<db>.<table><TAB><checksum>", turned into "checksum <db>.<table> <checksum>"
	mariadb $DB_OPTS -N < "$WORK/manifest.sql" | awk -F '\t' 'NF == 2 { print "checksum " $1 " " $2; next } { print }'
} > "$WORK/m2dev-manifest" || fail "could not read the table list, counts or checksums"
# A failed query inside $(...) does not stop the script: every number must be present
awk '$1 == "table" { tables++ } ($1 == "count" || $1 == "checksum" || $1 == "sum") && $3 !~ /^[0-9]+$/ { bad++ }
	END { exit (tables == 0 || bad > 0) }' "$WORK/m2dev-manifest" ||
	fail "manifest incomplete (no tables or a count/checksum/sum query failed)"
echo "SELECT CONCAT('MANIFEST ', ROUND(UNIX_TIMESTAMP(NOW(3)) * 1000));" >&3

# binary character set: strings are written and reloaded byte for byte (latin1 tables hold raw client bytes).
# --insert-ignore makes users "CREATE USER IF NOT EXISTS": restoring onto a fresh server adds the game's users and
# leaves the accounts it already has (root, mysql, mariadb.sys) alone; --replace would recreate root mid-restore
# and drop its privileges before its GRANT runs (seen in the restore test: ERROR 1698).
STEP=dump
mariadb-dump $DB_OPTS --system=users --insert-ignore --databases $LOCKED_DATABASES --skip-lock-tables \
	--routines --events --triggers --hex-blob --default-character-set=binary \
	> "$WORK/locked.sql" 2> "$WORK/dump.err" ||
	{ cat "$WORK/dump.err" >&2; fail "mariadb-dump failed"; }

STEP=unlock
echo "SELECT CONCAT('UNLOCKED ', ROUND(UNIX_TIMESTAMP(NOW(3)) * 1000)); UNLOCK TABLES;" >&3
release_lock
LOCK_MS=$(awk '$1 == "LOCKED" { a = $2 } $1 == "UNLOCKED" { b = $2 } END { if (a && b) print b - a; else print -1 }' "$WORK/lock.out")
[ "$LOCK_MS" -ge 0 ] || fail "lock session did not confirm the unlock"
log "m2dev-backup $MODE: lock held $LOCK_MS ms ($(awk '$1 == "LOCKED" { a = $2 } $1 == "MANIFEST" { m = $2 }
	$1 == "UNLOCKED" { u = $2 } END { printf "manifest %d ms, dump %d ms", m - a, u - m }' "$WORK/lock.out"))"

# Logs are append-only: dumped without the global lock so their size never lengthens it
STEP=dump-unlocked
if [ -n "$UNLOCKED_DATABASES" ]; then
	nice -n "$NICE" mariadb-dump $DB_OPTS --databases $UNLOCKED_DATABASES --single-transaction --skip-lock-tables \
		--routines --events --triggers --hex-blob --default-character-set=binary \
		> "$WORK/unlocked.sql" 2>> "$WORK/dump.err" ||
		{ cat "$WORK/dump.err" >&2; fail "mariadb-dump of $UNLOCKED_DATABASES failed"; }
else
	: > "$WORK/unlocked.sql"
fi
for f in locked.sql unlocked.sql; do
	[ ! -s "$WORK/$f" ] || tail -n 1 "$WORK/$f" | grep -q '^-- Dump completed' || fail "$f is not complete"
done
DUMP_KB=$(du -sk "$WORK/locked.sql" "$WORK/unlocked.sql" | awk '{ s += $1 } END { print s }')

STEP=archive
tar -C "$WORK" -cf - m2dev-manifest locked.sql unlocked.sql dump.err |
	nice -n "$NICE" zstd -q -T0 -3 |
	age -R "$AGE_RECIPIENTS_FILE" -o "$PART.tar.zst.age" ||
	fail "archive/encrypt failed"

SIZE=$(stat -f %z "$PART.tar.zst.age")
HASH=$(sha256 -q "$PART.tar.zst.age")
printf 'name=%s\nmode=%s\ndump_kb=%s\nsize_bytes=%s\nlock_ms=%s\n' "$NAME" "$MODE" "$DUMP_KB" "$SIZE" "$LOCK_MS" > "$PART.meta"
printf '%s  %s\n' "$HASH" "$NAME.tar.zst.age" > "$PART.sha256"

# Publish: the .sha256 goes last, so a reader never takes a file that is still being written
STEP=publish
mv -f "$PART.tar.zst.age" "$BACKUP_DIR/$NAME.tar.zst.age"
mv -f "$PART.meta" "$BACKUP_DIR/$NAME.meta"
mv -f "$PART.sha256" "$BACKUP_DIR/$NAME.tar.zst.age.sha256"
FILE="$NAME.tar.zst.age"

# Keep the newest $LOCAL_KEEP here; the backup host keeps the history
STEP=retention
ls -1t "$BACKUP_DIR"/m2dev-*.tar.zst.age | tail -n +$(( LOCAL_KEEP + 1 )) | while read -r old; do
	rm -f "$old" "$old.sha256" "${old%.tar.zst.age}.meta"
done

STEP=done
