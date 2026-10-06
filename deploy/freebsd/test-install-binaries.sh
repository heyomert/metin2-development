#!/bin/sh
# Tests m2dev-install-binaries.sh in a throwaway directory (never the live share/bin). FreeBSD, as root (chflags).
#   sh deploy/freebsd/test-install-binaries.sh <path to m2dev-install-binaries.sh>
# Failures are real, not injected: an immutable file (chflags schg) makes the second rename fail, a BUILD that is a
# directory makes the provenance step fail, a real process runs from the target directory.
set -u
INSTALLER=${1:?usage: test-install-binaries.sh <m2dev-install-binaries.sh>}
W=/var/tmp/m2inst-test
FAIL=0
ok() { echo "  [ok] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
cleanup() { chflags -R noschg "$W" 2>/dev/null; pkill -f "$W/bin/game" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
cleanup; mkdir -p "$W/bin" "$W/in"
C1=1111111111111111111111111111111111111111
C2=2222222222222222222222222222222222222222
SEQ=0

fake() { # path component commit dirty src [extra marker line]
	SEQ=$((SEQ + 1)); { printf 'binary payload %s\n' "$SEQ";printf 'M2BUILD|component=%s|commit=%s|dirty=%s|src=%s|describe=x|END\n' "$2" "$3" "$4" "$5"
	  [ $# -ge 6 ] && printf '%s\n' "$6"; printf 'trailer\n'; } > "$1"
}
state() { echo "$(sha256 -q "$W/bin/game" 2>/dev/null)/$(sha256 -q "$W/bin/db" 2>/dev/null)"; }
installed_lines() { [ -f "$W/deploy.log" ] && grep -c 'result=installed' "$W/deploy.log"; true; } # grep -c prints 0 itself
run() { sh "$INSTALLER" --bin-dir "$W/bin" --log "$W/deploy.log" "$@" > "$W/out.txt" 2>&1; }
refused() { # what, args...: must exit non-zero and leave the pair and the installed count unchanged
	what=$1; shift; before=$(state); n=$(installed_lines)
	if run "$@"; then bad "$what: was accepted"; return; fi
	[ "$(state)" = "$before" ] && [ "$(installed_lines)" = "$n" ] && ok "$what: refused, nothing changed ($(grep -o 'REFUSED: .*' "$W/out.txt" | cut -c1-90))" \
		|| bad "$what: refused but files or log changed"
}

echo "0. a current pair to replace"
fake "$W/bin/game" game $C1 0 git; fake "$W/bin/db" db $C1 0 git; chmod 0755 "$W/bin/game" "$W/bin/db"
OLD=$(state)

echo "1. production policy refuses everything but src=git dirty=0"
for v in "archive 0" "injected 0" "none unknown" "git 1"; do
	set -- $v; c=$C2; [ "$1" = none ] && c=unknown
	fake "$W/in/game" game $c $2 $1; fake "$W/in/db" db $c $2 $1
	refused "production + src=$1 dirty=$2" "$W/in/game" "$W/in/db"
done

echo "2. markers"
printf 'no marker here\n' > "$W/in/game"; fake "$W/in/db" db $C2 0 git
refused "game without marker" "$W/in/game" "$W/in/db"
printf 'M2BUILD|component=game|commit=%s|dirty=0|src=git|describe=x\n' $C2 > "$W/in/game"
refused "marker without END" "$W/in/game" "$W/in/db"
printf 'M2BUILD|component=game|commit=xyz|dirty=0|src=git|describe=x|END\n' > "$W/in/game"
refused "marker with a bad commit" "$W/in/game" "$W/in/db"
fake "$W/in/game" game $C2 0 git "M2BUILD|component=game|commit=$C1|dirty=0|src=git|describe=y|END"
refused "two markers in one binary" "$W/in/game" "$W/in/db"
fake "$W/in/game" db $C2 0 git
refused "db binary given as game" "$W/in/game" "$W/in/db"

echo "3. game and db must come from the same build"
fake "$W/in/game" game $C2 0 git; fake "$W/in/db" db $C1 0 git
refused "different commits" "$W/in/game" "$W/in/db"
fake "$W/in/game" game $C2 0 git; fake "$W/in/db" db $C2 1 git
refused "same commit, db dirty" --policy dev "$W/in/game" "$W/in/db"

echo "4. a process still running from the target directory"
fake "$W/in/game" game $C2 0 git; fake "$W/in/db" db $C2 0 git
cp /bin/sleep "$W/bin/game"; "$W/bin/game" 60 & SPID=$!; sleep 0.5
before_game=$(sha256 -q "$W/bin/game")
if run "$W/in/game" "$W/in/db"; then bad "install while running: accepted"
else grep -q 'processes still running' "$W/out.txt" && [ "$(sha256 -q "$W/bin/game")" = "$before_game" ] \
	&& ok "install while a process runs from bin/game: refused, file untouched" || bad "install while running: wrong reason or file changed"; fi
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
fake "$W/bin/game" game $C1 0 git; chmod 0755 "$W/bin/game"; OLD=$(state)

echo "5. second rename fails (db immutable) -> the previous pair is back"
chflags schg "$W/bin/db"
if run "$W/in/game" "$W/in/db"; then bad "immutable db: accepted"
else
	[ "$(state)" = "$OLD" ] && ok "game rolled back, db untouched: pair equals the previous pair" || bad "pair differs from the previous pair after rollback"
	grep -q 'result=failed reason=install_db rolled_back=1' "$W/deploy.log" && ok "failure recorded (result=failed rolled_back=1)" || bad "no failure record"
	[ "$(installed_lines)" = "0" ] && ok "no result=installed line" || bad "result=installed written for a failed install"
fi
chflags noschg "$W/bin/db"

echo "6. provenance cannot be written after the binaries were replaced -> rollback"
mkdir "$W/bin/BUILD"
if run "$W/in/game" "$W/in/db"; then bad "BUILD unwritable: accepted"
else
	[ "$(state)" = "$OLD" ] && ok "binaries rolled back after the provenance step failed" || bad "binaries not rolled back"
	grep -q 'reason=build_is_directory rolled_back=1' "$W/deploy.log" && ok "failure recorded" || bad "no failure record"
	[ "$(installed_lines)" = "0" ] && ok "still no result=installed line" || bad "result=installed written"
fi
rmdir "$W/bin/BUILD"

echo "7. success (production, src=git dirty=0)"
if run "$W/in/game" "$W/in/db"; then
	[ "$(sha256 -q "$W/bin/game")" = "$(sha256 -q "$W/in/game")" ] && [ "$(sha256 -q "$W/bin/db")" = "$(sha256 -q "$W/in/db")" ] \
		&& ok "installed files are the inputs (SHA-256)" || bad "installed files differ from inputs"
	line=$(grep 'result=installed' "$W/deploy.log" | tail -1)
	echo "$line" | grep -q "commit=$C2 dirty=0 src=git" && echo "$line" | grep -q "game_sha256=$(sha256 -q "$W/bin/game")" \
		&& echo "$line" | grep -q "db_sha256=$(sha256 -q "$W/bin/db")" && echo "$line" | grep -q "prev_game_sha256=${OLD%/*}" \
		&& ok "deploy.log: identity, both SHA-256 and the previous pair's SHA-256" || bad "deploy.log line incomplete: $line"
	grep -q "game_sha256=$(sha256 -q "$W/bin/game")" "$W/bin/BUILD" && ok "BUILD written" || bad "BUILD missing or wrong"
	modes="$(stat -f %Lp "$W/bin/game") $(stat -f %Lp "$W/bin/db") $(stat -f %Lp "$W/bin/BUILD") $(stat -f %Lp "$W/deploy.log")"
	[ "$modes" = "755 755 644 640" ] && ok "modes game/db 755, BUILD 644, deploy.log 640" || bad "modes: $modes"
	ww=$(find "$W/bin" "$W/deploy.log" -perm -0002 | wc -l | tr -d ' ')
	[ "$ww" = "0" ] && ok "nothing world-writable created" || bad "$ww world-writable paths"
else bad "valid install refused: $(cat "$W/out.txt")"; fi

echo "8. other policies"
fake "$W/in/game" game $C1 0 archive; fake "$W/in/db" db $C1 0 archive
run --policy test-vm "$W/in/game" "$W/in/db" && ok "test-vm accepts src=archive dirty=0" || bad "test-vm refused archive"
fake "$W/in/game" game $C1 0 injected; fake "$W/in/db" db $C1 0 injected
refused "test-vm + src=injected" --policy test-vm "$W/in/game" "$W/in/db"
run --policy dev "$W/in/game" "$W/in/db" && tail -1 "$W/deploy.log" | grep -q 'result=installed policy=dev .*src=injected' \
	&& ok "dev accepts src=injected, recorded as policy=dev" || bad "dev policy: $(cat "$W/out.txt")"

echo "9. marker fields must agree with the source type (syntax alone is not enough)"
inconsistent() { # policy src commit dirty: refused for the consistency reason, under every policy
	fake "$W/in/game" game $3 $4 $2; fake "$W/in/db" db $3 $4 $2
	refused "--policy $1 + src=$2 commit=$3 dirty=$4" --policy $1 "$W/in/game" "$W/in/db"
	grep -Eq 'requires a real commit|inconsistent marker' "$W/out.txt" || bad "  ...refused for another reason: $(cat "$W/out.txt")"
}
inconsistent production git unknown 0
inconsistent test-vm archive unknown 0
inconsistent dev git unknown 0
inconsistent dev archive unknown 0
inconsistent dev injected unknown 0
inconsistent dev git $C2 unknown
inconsistent dev archive $C2 1
inconsistent dev archive $C2 unknown
inconsistent dev none $C2 unknown
inconsistent dev none unknown 0
inconsistent dev none $C2 0
fake "$W/in/game" game unknown unknown none; fake "$W/in/db" db unknown unknown none
refused "test-vm + src=none commit=unknown" --policy test-vm "$W/in/game" "$W/in/db"
run --policy dev "$W/in/game" "$W/in/db" && tail -1 "$W/deploy.log" | grep -q 'result=installed policy=dev commit=unknown dirty=unknown src=none' \
	&& ok "dev accepts the consistent src=none commit=unknown dirty=unknown" || bad "dev refused src=none: $(cat "$W/out.txt")"
fake "$W/in/game" game $C2 unknown injected; fake "$W/in/db" db $C2 unknown injected
run --policy dev "$W/in/game" "$W/in/db" && ok "dev accepts src=injected dirty=unknown (no -DM2_BUILD_DIRTY)" || bad "dev refused injected dirty=unknown: $(cat "$W/out.txt")"

echo
[ $FAIL -eq 0 ] && echo "PASSED" || echo "FAILED ($FAIL)"
exit $FAIL
