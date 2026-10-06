#!/bin/sh
# Tests server-src/cmake/BuildIdentity.cmake in throwaway repositories (never the real one). Needs git and cmake;
# runs on the developer machine (Git Bash) or any host with both.
#   sh tools/build/test-build-identity.sh            CMAKE=<cmake binary> to override
# Each case states what it proves. Exit status = number of failures.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SCRIPT="$ROOT/server-src/cmake/BuildIdentity.cmake"
GITIGNORE="$ROOT/server-src/.gitignore"
CMAKE=${CMAKE:-cmake}
FAIL=0
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

ok() { echo "  [ok] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
# identity <src dir> <bin dir> [extra -D...] -> "commit dirty src" (+ warnings in $T/out.txt)
identity() {
	s=$1; b=$2; shift 2
	"$CMAKE" -DSRC_DIR="$s" -DBIN_DIR="$b" -DOUT="$T/id.h" "$@" -P "$SCRIPT" > "$T/out.txt" 2>&1
	c=$(sed -n 's/^#define M2_BUILD_COMMIT "\(.*\)"/\1/p' "$T/id.h")
	d=$(sed -n 's/^#define M2_BUILD_DIRTY "\(.*\)"/\1/p' "$T/id.h")
	r=$(sed -n 's/^#define M2_BUILD_SRC "\(.*\)"/\1/p' "$T/id.h")
	echo "$c $d $r"
}
expect() { # got want what
	if [ "$1" = "$2" ]; then ok "$3 [$1]"; else bad "$3 [got: $1, want: $2]"; fi
}

# A repository shaped like this one: server-src with the real .gitignore and the export-subst marker, plus client/
R="$T/repo"
mkdir -p "$R/server-src/src/game" "$R/server-src/cmake" "$R/client"
cd "$R" && git init -q && git config user.email t@t && git config user.name t && git config core.autocrlf false
cp "$GITIGNORE" server-src/.gitignore
cp "$SCRIPT" server-src/cmake/
printf '$Format:%%H$\n' > server-src/SOURCE_COMMIT
printf '/server-src/SOURCE_COMMIT export-subst\n' > .gitattributes
printf 'int a;\n' > server-src/src/game/a.cpp
printf '#!/bin/sh\n' > server-src/run.sh
printf 'x\n' > client/serverinfo.py
git add -A && git update-index --chmod=+x server-src/run.sh && git commit -q -m base
HEAD=$(git rev-parse HEAD)
S="$R/server-src"

echo "1. git source (the only production source with dirty=0)"
expect "$(identity "$S" "$T/outside-build")" "$HEAD 0 git" "clean work tree -> HEAD, clean"
printf 'int b;\n' >> "$S/src/game/a.cpp"
expect "$(identity "$S" "$T/outside-build")" "$HEAD 1 git" "tracked .cpp modified -> dirty"
git -C "$R" checkout -q -- server-src/src/game/a.cpp
printf 'int c;\n' > "$S/src/game/new.cpp"
expect "$(identity "$S" "$T/outside-build")" "$HEAD 1 git" "untracked .cpp (GLOB_RECURSE compiles it) -> dirty"
rm "$S/src/game/new.cpp"
mkdir -p "$S/src/game/old_BK" && printf 'int d;\n' > "$S/src/game/old_BK/x.cpp"
expect "$(identity "$S" "$T/outside-build")" "$HEAD 1 git" "git-ignored old_BK/x.cpp that GLOB_RECURSE compiles -> dirty"
rm -rf "$S/src/game/old_BK"
mkdir -p "$S/build/obj" && printf 'o\n' > "$S/build/obj/game.o"
expect "$(identity "$S" "$S/build")" "$HEAD 0 git" "this build's own in-tree build dir -> not dirty"
mkdir -p "$S/buildfoo" && printf 'int e;\n' > "$S/buildfoo/z.cpp"
expect "$(identity "$S" "$S/build")" "$HEAD 1 git" "a sibling dir that is not the build dir -> dirty"
rm -rf "$S/build" "$S/buildfoo"
printf 'y\n' >> "$R/client/serverinfo.py"
expect "$(identity "$S" "$T/outside-build")" "$HEAD 0 git" "client/ change only -> server build stays clean"
git -C "$R" checkout -q -- client/serverinfo.py
printf 'int f;\n' >> "$S/src/game/a.cpp"
got=$(cd / && identity "$S" "$T/outside-build")
expect "$got" "$HEAD 1 git" "run from another directory (wrong-pathspec regression: must still see the change)"
wrong=$(cd "$S" && git status --porcelain -- server-src | wc -l | tr -d ' ')
expect "$wrong" "0" "control: the naive 'git status -- server-src' from server-src misses it (what this guards against)"
git -C "$R" checkout -q -- server-src/src/game/a.cpp

echo "2. archive source (git archive with export-subst; never production provenance)"
A="$T/archive"
mkdir -p "$A" && git -C "$R" -c tar.umask=022 archive --format=tar HEAD server-src | tar -xf - -C "$A"
expect "$(identity "$A/server-src" "$T/outside-build")" "$HEAD 0 archive" "fresh extraction -> exported commit, clean"
expect "$(ls -l "$A/server-src/src/game/a.cpp" | awk '{print $1}')" "-rw-r--r--" "tar.umask=022 -> source file 0644 (no group/world write)"
expect "$(ls -l "$A/server-src/run.sh" | awk '{print $1}')" "-rwxr-xr-x" "tar.umask=022 -> executable 0755"
printf 'int g;\n' >> "$A/server-src/src/game/a.cpp"
expect "$(identity "$A/server-src" "$T/outside-build")" "$HEAD 0 archive" "LIMIT: .cpp changed after extraction is NOT detected (why production rejects archive)"
printf '%s\n' 0123456789abcdef0123456789abcdef01234567 > "$A/server-src/SOURCE_COMMIT"
expect "$(identity "$A/server-src" "$T/outside-build")" "0123456789abcdef0123456789abcdef01234567 0 archive" "LIMIT: forged 40-hex SOURCE_COMMIT is believed (why production rejects archive)"
printf 'not-a-commit\n' > "$A/server-src/SOURCE_COMMIT"
expect "$(identity "$A/server-src" "$T/outside-build")" "unknown unknown none" "malformed SOURCE_COMMIT -> unknown"
grep -q "SOURCE_COMMIT is not a 40-hex commit" "$T/out.txt" && ok "malformed SOURCE_COMMIT -> visible warning" || bad "no warning for malformed SOURCE_COMMIT"

echo "3. injected and none"
N="$T/plain/server-src"
mkdir -p "$N/src" && printf 'int h;\n' > "$N/src/a.cpp"
expect "$(identity "$N" "$T/outside-build")" "unknown unknown none" "no git, no SOURCE_COMMIT -> unknown"
grep -q "M2 build identity: UNKNOWN" "$T/out.txt" && ok "unknown -> visible warning" || bad "no warning for unknown identity"
expect "$(identity "$N" "$T/outside-build" -DM2_BUILD_COMMIT="$HEAD" -DM2_BUILD_DIRTY=0)" "$HEAD 0 injected" "injected -> labelled injected (unverified)"
expect "$(identity "$N" "$T/outside-build" -DM2_BUILD_COMMIT=xyz)" "unknown unknown none" "invalid injected commit -> unknown"
expect "$(identity "$S" "$T/outside-build" -DM2_BUILD_COMMIT=0123456789abcdef0123456789abcdef01234567)" "$HEAD 0 git" "injection cannot override a git work tree"
grep -q "M2_BUILD_COMMIT ignored" "$T/out.txt" && ok "ignored injection -> visible warning" || bad "no warning for ignored injection"

echo "4. header rewritten only when the identity changes (no-op builds recompile nothing)"
identity "$S" "$T/outside-build" > /dev/null; m1=$(stat -c %Y "$T/id.h" 2>/dev/null || stat -f %m "$T/id.h"); sleep 2
identity "$S" "$T/outside-build" > /dev/null; m2=$(stat -c %Y "$T/id.h" 2>/dev/null || stat -f %m "$T/id.h")
expect "$m2" "$m1" "same identity -> header untouched (mtime unchanged)"
printf 'int i;\n' >> "$S/src/game/a.cpp"; identity "$S" "$T/outside-build" > /dev/null
m3=$(stat -c %Y "$T/id.h" 2>/dev/null || stat -f %m "$T/id.h")
[ "$m3" != "$m1" ] && ok "identity changed -> header rewritten" || bad "header not rewritten after identity change"

echo
[ "$FAIL" -eq 0 ] && echo "PASSED" || echo "FAILED ($FAIL)"
exit "$FAIL"
