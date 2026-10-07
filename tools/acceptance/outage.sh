#!/bin/sh
# Controlled MariaDB outage on the TEST VM, with evidence (docs/engineering/regression-baseline.md).
# Fault injection and evidence collection only: stops and starts mysql-server and reads processes, logs and telemetry.
# It changes no game/db binary, configuration, schema or data.
#
#   sh outage.sh --test-vm <hostname> --seconds <1-300> --label <name> [--after <10-300>] [--server-dir DIR] [--out DIR]
#
# Fail-closed:
# - runs only as root on a host listed in ALLOWED_HOSTS, and only if --test-vm repeats that host name;
# - refuses when MariaDB is not running before, when no game/db process runs, and inside the hourly hot-backup minutes;
# - once it has stopped MariaDB it always tries to start it again (normal end, error, Ctrl-C, SIGTERM, SIGHUP).
# Evidence (directory mode 0700): processes and CPU time, per-second syserr line counts, health/SQL telemetry lines,
# AsyncSQL/[SLOW-*] lines with any SQL text masked (older binaries log raw SQL), new failure-ledger lines (metadata only).
# Other syserr lines are counted, not copied: they can carry character names or IP addresses.
set -u

ALLOWED_HOSTS="m2dev-acceptance"	# the test VM (docs/build-and-run.md); never add a production host
AVOID_MINUTES="15 16 17 18 19"		# hourly hot backup runs at :17 (docs/backup.md)
MAX_SECONDS=300						# tool safety bound, not a product limit

die() { echo "outage: $*" >&2; exit 2; }

TEST_VM= SECONDS_DOWN= LABEL= AFTER=30
SERVER_DIR=/usr/local/m2dev-acceptance/server
OUT_BASE=/root/acceptance
while [ $# -gt 0 ]; do
	case $1 in
		--test-vm) TEST_VM=${2:-}; shift 2 ;;
		--seconds) SECONDS_DOWN=${2:-}; shift 2 ;;
		--label) LABEL=${2:-}; shift 2 ;;
		--after) AFTER=${2:-}; shift 2 ;;
		--server-dir) SERVER_DIR=${2:-}; shift 2 ;;
		--out) OUT_BASE=${2:-}; shift 2 ;;
		*) die "unknown argument: $1" ;;
	esac
done

# --- target checks (nothing has been changed yet) -------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "must run as root"
HOST=$(hostname -s)
case " $ALLOWED_HOSTS " in *" $HOST "*) ;; *) die "host '$HOST' is not an allowed test VM; refusing" ;; esac
[ "$TEST_VM" = "$HOST" ] || die "--test-vm must repeat this host's name ($HOST); refusing"
case $SECONDS_DOWN in ''|*[!0-9]*) die "--seconds must be a whole number" ;; esac
[ "$SECONDS_DOWN" -ge 1 ] && [ "$SECONDS_DOWN" -le "$MAX_SECONDS" ] || die "--seconds must be 1-$MAX_SECONDS"
case $AFTER in ''|*[!0-9]*) die "--after must be a whole number" ;; esac
[ "$AFTER" -ge 10 ] && [ "$AFTER" -le 300 ] || die "--after must be 10-300"
case $LABEL in ''|*[!a-z0-9-]*) die "--label must be [a-z0-9-]+" ;; esac
C=$SERVER_DIR/channels
[ -d "$C" ] || die "no channels directory under $SERVER_DIR"
case " $AVOID_MINUTES " in *" $(date +%M) "*) die "inside the hot-backup minutes ($AVOID_MINUTES); retry later" ;; esac
service mysql-server status > /dev/null 2>&1 || die "MariaDB is not running before the test; refusing"

pids() { procstat -b -a 2>/dev/null | awk '$NF ~ /share\/bin\/(game|db)$/ { print $1 }' | sort -n | tr '\n' ' '; }
[ -n "$(pids)" ] || die "no game/db process is running; nothing to observe"

PROCS=$(cd "$C" && for d in db auth */core*; do [ -f "$d/syserr.log" ] && echo "$d"; done)
OUT=$OUT_BASE/$LABEL-$(date -u +%Y%m%dT%H%M%SZ)
[ -e "$OUT" ] && die "$OUT exists"
umask 077
mkdir -p "$OUT" || die "cannot create $OUT"

iso() { date +%Y-%m-%dT%H:%M:%S%z; }
key() { echo "$1" | tr / _; }
cpu() { for p in $(pids); do ps -o pid= -o time= -o comm= -p "$p"; done; }
counts() { for p in $PROCS; do printf '%s %s %s\n' "$1" "$p" "$(wc -l < "$C/$p/syserr.log" | tr -d ' ')"; done; }
ledger_lines() { cat "$C/$1"/log/sql_failures_*.log 2>/dev/null | wc -l | tr -d ' '; }

# --- recovery guarantee --------------------------------------------------------------------------------------------
STOPPED=0
recover() {
	if [ "$STOPPED" = 1 ]; then
		service mysql-server start > "$OUT/mysql-start.txt" 2>&1
		i=0
		until service mysql-server status > /dev/null 2>&1; do
			i=$((i + 1)); [ "$i" -ge 60 ] && break
			sleep 1
		done
		if service mysql-server status > /dev/null 2>&1; then
			echo "mysql_started=$(iso)" >> "$OUT/summary.txt"
		else
			echo "mysql_started=FAILED (start it by hand: service mysql-server start)" >> "$OUT/summary.txt"
			echo "outage: MariaDB did NOT start again; run: service mysql-server start" >&2
		fi
		STOPPED=0
	fi
}
trap 'recover' EXIT
trap 'recover; exit 130' INT TERM HUP

# --- before ---------------------------------------------------------------------------------------------------------
{
	echo "label=$LABEL host=$HOST seconds_down=$SECONDS_DOWN after=$AFTER"
	echo "pids_before=$(pids)"
	for p in $PROCS; do
		echo "build_$(key $p)=$(cat "$C/$p/version.txt" "$C/$p/VERSION.txt" 2>/dev/null | head -1)"
	done
} > "$OUT/summary.txt"
cpu > "$OUT/cpu-before.txt"
counts before > "$OUT/syserr-counts.txt"
for p in $PROCS; do
	wc -l < "$C/$p/syserr.log" | tr -d ' ' > "$OUT/.base-$(key $p)"
	ledger_lines "$p" > "$OUT/.ledger-$(key $p)"
done

# --- outage ---------------------------------------------------------------------------------------------------------
echo "mysql_stop_issued=$(iso)" >> "$OUT/summary.txt"
STOPPED=1
service mysql-server stop > "$OUT/mysql-stop.txt" 2>&1
if service mysql-server status > /dev/null 2>&1; then
	echo "mysql_stop=FAILED" >> "$OUT/summary.txt"
	die "MariaDB did not stop; nothing injected (evidence in $OUT)"
fi
echo "mysql_stopped=$(iso)" >> "$OUT/summary.txt"
T_STOP=$(date +%s)
echo "outage: MariaDB stopped at $(date +%H:%M:%S), starts again in ${SECONDS_DOWN}s"
i=1
while [ "$i" -le "$SECONDS_DOWN" ]; do
	sleep 1
	counts "down+$i" >> "$OUT/syserr-counts.txt"
	i=$((i + 1))
done
recover
T_START=$(date +%s)
echo "outage: MariaDB started at $(date +%H:%M:%S); collecting for ${AFTER}s"
i=1
while [ "$i" -le "$AFTER" ]; do
	sleep 1
	counts "up+$i" >> "$OUT/syserr-counts.txt"
	i=$((i + 1))
done

# --- after ----------------------------------------------------------------------------------------------------------
FROM=$(date -r "$((T_STOP - 60))" +%Y-%m-%dT%H:%M:%S)
TO=$(date -r "$((T_START + AFTER))" +%Y-%m-%dT%H:%M:%S)
echo "pids_after=$(pids)" >> "$OUT/summary.txt"
cpu > "$OUT/cpu-after.txt"
for p in $PROCS; do
	k=$(key "$p")
	b=$(cat "$OUT/.base-$k")
	n=$(wc -l < "$C/$p/syserr.log" | tr -d ' ')
	# AsyncSQL and [SLOW-*] lines only, SQL text masked (older binaries write it after "query:" or "ms: ")
	tail -n +"$((b + 1))" "$C/$p/syserr.log" | grep -E 'AsyncSQL|\[SLOW-' \
		| sed -E 's/(query: ).*/\1<masked>/; s/(\[SLOW-[A-Z]+\][^:]*ms: )([^f].*)/\1<masked>/' > "$OUT/syserr-sql-$k.log"
	lb=$(cat "$OUT/.ledger-$k")
	cat "$C/$p"/log/sql_failures_*.log 2>/dev/null | tail -n +"$((lb + 1))" > "$OUT/ledger-new-$k.log"
	for f in metrics sql; do
		cat "$C/$p"/log/${f}_*.log 2>/dev/null | awk -v a="$FROM" -v z="$TO" 'substr($1,1,19) >= a && substr($1,1,19) <= z' \
			> "$OUT/$f-$k.log"
	done
	echo "$p syserr_added=$((n - b)) sql_lines=$(wc -l < "$OUT/syserr-sql-$k.log" | tr -d ' ') ledger_new=$(wc -l < "$OUT/ledger-new-$k.log" | tr -d ' ')" \
		>> "$OUT/summary.txt"
done
rm -f "$OUT"/.base-* "$OUT"/.ledger-*
echo "down_s=$((T_START - T_STOP)) window=$FROM..$TO evidence=$OUT" >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
