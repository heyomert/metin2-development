#!/bin/sh
# Installs a game + db binary pair from ONE build, all or nothing, and records which exact files went live
# (roadmap 1.9 / T-2; docs/build-and-run.md -> "Derleme kimliği ve kurulum").
#
#   m2dev-install-binaries.sh [--policy production|test-vm|dev] [--bin-dir DIR] [--log FILE] <game> <db>
#
# Policies (fail closed; production is the default):
#   production  src=git, dirty=0, a real commit. Nothing else.
#   test-vm     also src=archive with dirty=0 (exported source; changes after extraction cannot be seen).
#   dev         any well-formed identity (dirty, injected, unknown), recorded as policy=dev.
# Every policy requires: exactly one well-formed marker per binary, game and db built from the same identity
# (commit, dirty, src), and no game/db process running from the target directory. Stopping and starting the
# server is NOT done here (docs/build-and-run.md: stop -> m2dev-backup consistent -> install -> start).
#
# Order: validate (nothing touched) -> stage -> back up current pair -> install (rename) -> verify SHA-256 ->
# write BUILD and the deploy.log line. If anything after the first rename fails, the previous pair (and BUILD) is
# put back and verified; result=installed is written only when everything, provenance included, succeeded.
# The marker is the binary's own claim about its source, the SHA-256 the exact file; neither is a signature.
set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin

POLICY=production
BIN=/usr/local/m2dev-acceptance/server/share/bin
LOG=/var/db/m2dev/deploy.log

while [ $# -gt 0 ]; do
	case "$1" in
		--policy) POLICY=${2:-}; shift 2 ;;
		--bin-dir) BIN=${2:-}; shift 2 ;;
		--log) LOG=${2:-}; shift 2 ;;
		--) shift; break ;;
		-*) echo "unknown option $1" >&2; exit 2 ;;
		*) break ;;
	esac
done
[ $# -eq 2 ] || { echo "usage: $0 [--policy production|test-vm|dev] [--bin-dir DIR] [--log FILE] <game> <db>" >&2; exit 2; }
NEW_GAME=$1
NEW_DB=$2
case "$POLICY" in production|test-vm|dev) ;; *) echo "bad policy: $POLICY" >&2; exit 2 ;; esac

die() { echo "m2dev-install: REFUSED: $*" >&2; exit 1; }
now() { date '+%Y-%m-%dT%H:%M:%S%z'; }

# --- 1. validate, nothing is touched yet ---------------------------------------------------------------------------
[ -f "$NEW_GAME" ] && [ -f "$NEW_DB" ] || die "both <game> and <db> must be regular files"
[ -d "$BIN" ] || die "bin dir $BIN does not exist"
for t in game db; do [ ! -d "$BIN/$t" ] || die "$BIN/$t is a directory"; done # mv would move INTO it

# marker <file> <component>: sets M_COMMIT M_DIRTY M_SRC M_DESCRIBE or refuses
marker() {
	lines=$(strings -a "$1" | grep '^M2BUILD|' || true)
	count=$(printf '%s' "$lines" | grep -c '^M2BUILD|' || true)
	[ "$count" = "1" ] || die "$1: expected exactly one M2BUILD marker, found $count"
	printf '%s' "$lines" | grep -Eq '^M2BUILD\|component=(game|db)\|commit=([0-9a-f]{40}|unknown)\|dirty=(0|1|unknown)\|src=(git|archive|injected|none)\|describe=[A-Za-z0-9._+-]*\|END$' \
		|| die "$1: malformed marker: $lines"
	field() { printf '%s' "$lines" | sed -n "s/.*|$1=\([^|]*\)|.*/\1/p"; }
	[ "$(field component)" = "$2" ] || die "$1: marker says component=$(field component), expected $2"
	M_COMMIT=$(field commit); M_DIRTY=$(field dirty); M_SRC=$(field src); M_DESCRIBE=$(field describe)
}
marker "$NEW_GAME" game
G_COMMIT=$M_COMMIT; G_DIRTY=$M_DIRTY; G_SRC=$M_SRC; G_DESCRIBE=$M_DESCRIBE
marker "$NEW_DB" db
[ "$M_COMMIT|$M_DIRTY|$M_SRC" = "$G_COMMIT|$G_DIRTY|$G_SRC" ] \
	|| die "game and db are not from the same build: game commit=$G_COMMIT dirty=$G_DIRTY src=$G_SRC, db commit=$M_COMMIT dirty=$M_DIRTY src=$M_SRC"

case "$POLICY" in
	production) [ "$G_SRC" = "git" ] && [ "$G_DIRTY" = "0" ] || die "production accepts only src=git dirty=0 (got src=$G_SRC dirty=$G_DIRTY)" ;;
	test-vm) { [ "$G_SRC" = "git" ] || [ "$G_SRC" = "archive" ]; } && [ "$G_DIRTY" = "0" ] \
		|| die "test-vm accepts only src=git|archive dirty=0 (got src=$G_SRC dirty=$G_DIRTY); use --policy dev for development builds" ;;
	dev) ;;
esac

# No game/db process may be running from this directory (checked directly, not through `service m2dev status`, which
# reports "running" while any one process is alive: roadmap T-4)
BIN_REAL=$(realpath "$BIN") || die "cannot resolve $BIN"
running=$(procstat -b -a 2>/dev/null | awk -v g="$BIN_REAL/game" -v d="$BIN_REAL/db" '$NF == g || $NF == d { print $1 " " $NF }')
[ -z "$running" ] || die "processes still running from $BIN_REAL: $(echo $running)"

# Provenance must be writable before anything is replaced
LOGDIR=$(dirname "$LOG")
[ -d "$LOGDIR" ] || install -d -m 0750 "$LOGDIR" || die "cannot create $LOGDIR"
[ -e "$LOG" ] || install -m 0640 /dev/null "$LOG" || die "cannot create $LOG"
: >> "$LOG" || die "$LOG is not writable"

# --- 2. stage -------------------------------------------------------------------------------------------------------
TS=$(date -u '+%Y%m%dT%H%M%SZ')
STAGE="$BIN/.stage.$$"
PREV="$BIN/.prev.$TS.$$" # pid: two installs within one second must not collide
mkdir -m 0700 "$STAGE" || die "cannot create $STAGE"
trap 'rm -rf "$STAGE"' EXIT
install -m 0755 "$NEW_GAME" "$STAGE/game" && install -m 0755 "$NEW_DB" "$STAGE/db" || die "staging failed"
GAME_SHA=$(sha256 -q "$STAGE/game"); DB_SHA=$(sha256 -q "$STAGE/db")
[ "$GAME_SHA" = "$(sha256 -q "$NEW_GAME")" ] && [ "$DB_SHA" = "$(sha256 -q "$NEW_DB")" ] || die "staged copies differ from the inputs"

# --- 3. back up the current pair and BUILD ---------------------------------------------------------------------------
mkdir -m 0700 "$PREV" || die "cannot create $PREV"
PREV_GAME_SHA=none; PREV_DB_SHA=none
if [ -e "$BIN/game" ]; then cp -p "$BIN/game" "$PREV/game" || die "backup of game failed"; PREV_GAME_SHA=$(sha256 -q "$PREV/game"); fi
if [ -e "$BIN/db" ]; then cp -p "$BIN/db" "$PREV/db" || die "backup of db failed"; PREV_DB_SHA=$(sha256 -q "$PREV/db"); fi
[ -f "$BIN/BUILD" ] && { cp -p "$BIN/BUILD" "$PREV/BUILD" || die "backup of BUILD failed"; }

RECORD="policy=$POLICY commit=$G_COMMIT dirty=$G_DIRTY src=$G_SRC describe=$G_DESCRIBE game_sha256=$GAME_SHA db_sha256=$DB_SHA prev_game_sha256=$PREV_GAME_SHA prev_db_sha256=$PREV_DB_SHA prev_dir=$PREV"
printf 'time=%s %s\n' "$(now)" "$RECORD" > "$STAGE/BUILD" && chmod 0644 "$STAGE/BUILD" || die "cannot prepare BUILD"

# --- 4. install; from here on every failure restores the previous pair ----------------------------------------------
restore_one() { # name prev_sha: only a file that actually changed is put back
	if [ "$2" = none ]; then
		rm -f "$BIN/$1"
	elif [ "$(sha256 -q "$BIN/$1" 2>/dev/null)" != "$2" ]; then
		cp -p "$PREV/$1" "$BIN/.restore.$1.$$" && mv -f "$BIN/.restore.$1.$$" "$BIN/$1"
	fi
}
rollback() { # reason
	ok=1
	restore_one game "$PREV_GAME_SHA" || ok=0
	restore_one db "$PREV_DB_SHA" || ok=0
	if [ -f "$PREV/BUILD" ]; then cp -p "$PREV/BUILD" "$BIN/.restore.BUILD.$$" && mv -f "$BIN/.restore.BUILD.$$" "$BIN/BUILD" || ok=0
	elif [ ! -d "$BIN/BUILD" ]; then rm -f "$BIN/BUILD"; fi
	[ "$PREV_GAME_SHA" = none ] || [ "$(sha256 -q "$BIN/game" 2>/dev/null)" = "$PREV_GAME_SHA" ] || ok=0
	[ "$PREV_DB_SHA" = none ] || [ "$(sha256 -q "$BIN/db" 2>/dev/null)" = "$PREV_DB_SHA" ] || ok=0
	if [ $ok -eq 1 ]; then state="rolled_back=1"; else state="rolled_back=0 ROLLBACK_INCOMPLETE=1"; fi
	printf 'time=%s result=failed reason=%s %s %s\n' "$(now)" "$1" "$state" "$RECORD" >> "$LOG" 2>/dev/null \
		|| echo "m2dev-install: could not write the failure to $LOG" >&2
	echo "m2dev-install: FAILED ($1); $state. Previous binaries are in $PREV" >&2
	exit 1
}

mv -f "$STAGE/game" "$BIN/game" || rollback install_game
mv -f "$STAGE/db" "$BIN/db" || rollback install_db

# --- 5. verify what is now in place --------------------------------------------------------------------------------
[ "$(sha256 -q "$BIN/game")" = "$GAME_SHA" ] && [ "$(sha256 -q "$BIN/db")" = "$DB_SHA" ] || rollback verify_sha256

# --- 6. provenance: part of success, not an afterthought -------------------------------------------------------------
[ ! -d "$BIN/BUILD" ] || rollback build_is_directory # mv would move INTO it and look successful
mv -f "$STAGE/BUILD" "$BIN/BUILD" || rollback write_build
printf 'time=%s result=installed %s\n' "$(now)" "$RECORD" >> "$LOG" || rollback write_log

echo "m2dev-install: installed commit=$G_COMMIT dirty=$G_DIRTY src=$G_SRC policy=$POLICY"
echo "  game $GAME_SHA"
echo "  db   $DB_SHA"
echo "  previous pair kept in $PREV"
