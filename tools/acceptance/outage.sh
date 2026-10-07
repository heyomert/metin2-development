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
# - every option that takes a value must get a non-empty value that does not start with '-';
# - once MariaDB is stopped, STOPPED is cleared only after a separate status check shows it running again; if it does
#   not come back, the run stops collecting, one last start is tried on exit, and the script ends non-zero with
#   "result=FAILED" and the manual recovery command.
# Exit codes: 0 ok; 2 refused (nothing changed); 3 MariaDB did not stop or did not start again; 130 interrupted.
# Evidence (directory mode 0700): processes and CPU time, per-second syserr line counts, health/SQL telemetry lines,
# new failure-ledger lines (metadata only), and AsyncSQL/[SLOW-*] syserr lines whose payload is kept only when it is
# exactly one of the step 2a metadata formats; any other payload (raw SQL from older binaries included) is masked.
# Other syserr lines are counted, not copied: they can carry character names or IP addresses.
# Tests: tools/acceptance/test-outage.sh (mocks; never touches MariaDB).
set -u

ALLOWED_HOSTS="m2dev-acceptance"	# the test VM (docs/build-and-run.md); never add a production host
AVOID_MINUTES="15 16 17 18 19"		# hourly hot backup runs at :17 (docs/backup.md)
MAX_SECONDS=300						# tool safety bound, not a product limit
START_WAIT=60						# seconds to wait for MariaDB to report running after a start
MANUAL="service mysql-server start"

die() { echo "outage: $*" >&2; exit 2; }

TEST_VM= SECONDS_DOWN= LABEL= AFTER=30
SERVER_DIR=/usr/local/m2dev-acceptance/server
OUT_BASE=/root/acceptance
while [ $# -gt 0 ]; do
	opt=$1
	case $opt in
		--test-vm|--seconds|--label|--after|--server-dir|--out) ;;
		*) die "unknown argument: $opt" ;;
	esac
	[ $# -ge 2 ] || die "$opt needs a value"
	val=$2
	case $val in ''|-*) die "$opt needs a value" ;; esac
	case $opt in
		--test-vm) TEST_VM=$val ;;
		--seconds) SECONDS_DOWN=$val ;;
		--label) LABEL=$val ;;
		--after) AFTER=$val ;;
		--server-dir) SERVER_DIR=$val ;;
		--out) OUT_BASE=$val ;;
	esac
	shift 2
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

mysql_running() { service mysql-server status > /dev/null 2>&1; }
mysql_running || die "MariaDB is not running before the test; refusing"

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

# Keeps the payload after "AsyncSQL: " or "[SLOW-" only when it is exactly one of the step 2a formats
# (libsql/AsyncSQL.cpp, db/DBManager.cpp, game/db.cpp); everything else becomes <masked>. Values allowed in kept lines
# are labels, family names (verb.table, no values), numbers and enum names. A line whose text before the marker is not
# exactly the syserr header (the marker inside other data) is masked whole.
mask_sql() {
	awk '
	function keep(p,   V, N) {
		V = "[A-Za-z0-9_.-]+"; N = "[0-9]+"
		if (p ~ ("^(failed|attempt failed, retrying) role=" V " family=" V " id=-?" N " phase=" V " errno=" N " result=" V " policy=" V " attempts=" N " age_ms=" N "$")) return 1
		if (p ~ ("^recovered role=" V " after policy=" V ", " N " repeated failure\\(s\\) counted but not logged$")) return 1
		if (p ~ ("^quit role=" V " drained applied=" N " failed=" N " unexecuted=" N "$")) return 1
		if (p ~ ("^quit role=" V " " N " message\\(s\\) queued after the worker stopped, not executed$")) return 1
		if (p ~ ("^session check failed role=" V " autocommit=[A-Za-z0-9_.-]* charset=[A-Za-z0-9_.-]* expected_charset=" V "$")) return 1
		if (p ~ ("^escape buffer too small \\(dstSize " N " srcSize " N "\\)$")) return 1
		if (p ~ ("^(DB\\] DirectQuery\\(-?" N "\\)|GAME\\] DirectQuery) took " N " ms: family=" V " id=-?" N "$")) return 1
		return 0
	}
	{
		i = index($0, "AsyncSQL: "); m = 10
		j = index($0, "[SLOW-")
		if (j > 0 && (i == 0 || j < i)) { i = j; m = 6 }
		if (i == 0) next
		# Text before the marker must be the syserr header "[time] [level] [function] " and nothing else
		head = substr($0, 1, i - 1)
		if (head !~ /^\[[0-9: .-]+\] \[[a-z]+\] \[[^]]*\] $/) { print "<masked>"; next }
		p = substr($0, i + m)
		print head substr($0, i, m) (keep(p) ? p : "<masked>")
	}'
}

# --- recovery: STOPPED is cleared only when MariaDB is proven running -----------------------------------------------
STOPPED=0
RESULT=ok
recover() {
	[ "$STOPPED" = 1 ] || return 0
	service mysql-server start >> "$OUT/mysql-start.txt" 2>&1
	i=0
	while [ "$i" -lt "$START_WAIT" ]; do
		if mysql_running; then
			STOPPED=0
			echo "mysql_started=$(iso)" >> "$OUT/summary.txt"
			return 0
		fi
		i=$((i + 1))
		sleep 1
	done
	echo "mysql_start_attempt=FAILED at $(iso)" >> "$OUT/summary.txt"
	return 1
}
on_exit() {
	rc=$?
	trap - EXIT INT TERM HUP
	if [ "$STOPPED" = 1 ] && ! recover; then
		echo "result=FAILED mariadb_not_running manual_recovery='$MANUAL'" >> "$OUT/summary.txt"
		echo "outage: FAILED: MariaDB is not running; start it by hand: $MANUAL" >&2
		[ "$rc" = 0 ] && rc=3
	elif [ "$rc" = 0 ]; then
		echo "result=$RESULT" >> "$OUT/summary.txt"
	fi
	exit "$rc"
}
on_signal() {
	echo "result=INTERRUPTED at $(iso)" >> "$OUT/summary.txt"
	exit 130
}
trap on_exit EXIT
trap on_signal INT TERM HUP

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
if mysql_running; then
	echo "mysql_stop=FAILED (still running; nothing injected)" >> "$OUT/summary.txt"
	STOPPED=0
	echo "outage: MariaDB did not stop; nothing injected (evidence in $OUT)" >&2
	exit 3
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
if ! recover; then
	echo "outage: MariaDB did not start again; not collecting after-evidence" >&2
	exit 3
fi
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
	tail -n +"$((b + 1))" "$C/$p/syserr.log" | mask_sql > "$OUT/syserr-sql-$k.log"
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
