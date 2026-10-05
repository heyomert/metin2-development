#!/bin/sh
# Pulls finished m2dev backups from the game server to this machine and applies retention (docs/backup.md).
# Runs on the backup host, never on the game server: the server cannot reach or delete these copies.
#
#   m2dev-pull-backups.sh <ssh host> <local dir>
#
#   REMOTE_DIR=/var/backups/m2dev  KEEP_HOURS=48  KEEP_DAYS=14  KEEP_WEEKS=8  KEEP_CONSISTENT=5
#   DRY_RUN=1 (retention: print what would be removed, remove nothing)
#
# Only backups whose .sha256 exists on the server are taken (the server writes it last), and a copy is kept only
# if its hash matches. Retention (tiers by the UTC time in the name): every backup of the last KEEP_HOURS hours;
# then the newest of each day up to KEEP_DAYS days; then the newest of each 7-day bucket up to KEEP_WEEKS weeks;
# and always the newest KEEP_CONSISTENT consistent (pre-maintenance) backups. With hourly backups that is about
# 48 + 12 + 6 + 5 files.
# POSIX sh + ssh/scp + sha256sum or sha256: FreeBSD, Linux, or Git Bash on Windows.
set -eu

HOST=${1:-}
LOCAL=${2:-}
if [ -z "$HOST" ] || [ -z "$LOCAL" ]; then
	echo "usage: $0 <ssh host> <local dir>" >&2
	exit 2
fi
REMOTE_DIR=${REMOTE_DIR:-/var/backups/m2dev}
KEEP_HOURS=${KEEP_HOURS:-48}
KEEP_DAYS=${KEEP_DAYS:-14}
KEEP_WEEKS=${KEEP_WEEKS:-8}
KEEP_CONSISTENT=${KEEP_CONSISTENT:-5}
DRY_RUN=${DRY_RUN:-0}

umask 077
mkdir -p "$LOCAL"

hash_of() {
	if command -v sha256sum > /dev/null 2>&1; then
		sha256sum "$1" | cut -d ' ' -f 1
	else
		sha256 -q "$1"
	fi
}

pulled=0
failed=0
for sum in $(ssh "$HOST" "cd '$REMOTE_DIR' && ls -1 m2dev-*.tar.zst.age.sha256 2> /dev/null" | tr -d '\r'); do
	file=${sum%.sha256}
	meta=${file%.tar.zst.age}.meta
	[ -f "$LOCAL/$sum" ] && continue

	rm -f "$LOCAL/.part-$file" "$LOCAL/.part-$meta" "$LOCAL/.part-$sum"
	if scp -q "$HOST:$REMOTE_DIR/$file" "$LOCAL/.part-$file" &&
		scp -q "$HOST:$REMOTE_DIR/$meta" "$LOCAL/.part-$meta" &&
		scp -q "$HOST:$REMOTE_DIR/$sum" "$LOCAL/.part-$sum" &&
		[ "$(hash_of "$LOCAL/.part-$file")" = "$(cut -d ' ' -f 1 "$LOCAL/.part-$sum" | tr -d '\r')" ]; then
		mv -f "$LOCAL/.part-$file" "$LOCAL/$file"
		mv -f "$LOCAL/.part-$meta" "$LOCAL/$meta"
		mv -f "$LOCAL/.part-$sum" "$LOCAL/$sum" # last: marks the copy complete
		pulled=$(( pulled + 1 ))
	else
		echo "m2dev-pull-backups: $file: copy or hash check failed, will retry next run" >&2
		rm -f "$LOCAL/.part-$file" "$LOCAL/.part-$meta" "$LOCAL/.part-$sum"
		failed=$(( failed + 1 ))
	fi
done

# Retention by the UTC date in the name (m2dev-YYYYMMDDTHHMMSSZ-<mode>...): day numbers are computed in awk so the
# same code works with BSD and GNU date
now=$(date -u +%Y%m%d%H)
removed=0
for victim in $(ls -1 "$LOCAL" | grep -E '^m2dev-[0-9]{8}T[0-9]{6}Z-(hot|consistent)\.tar\.zst\.age$' | sort -r |
	awk -v now="$now" -v kh="$KEEP_HOURS" -v kd="$KEEP_DAYS" -v kw="$KEEP_WEEKS" -v kc="$KEEP_CONSISTENT" '
	function days(ymd,   y, m, d) { # days since 1970-01-01 (proleptic Gregorian)
		y = substr(ymd, 1, 4) + 0; m = substr(ymd, 5, 2) + 0; d = substr(ymd, 7, 2) + 0
		if (m <= 2) { y--; m += 12 }
		return 365 * y + int(y / 4) - int(y / 100) + int(y / 400) + int((153 * (m - 3) + 2) / 5) + d - 719469
	}
	{
		# newest first; split("m2dev-20261005T203925Z-hot.tar.zst.age") -> f[2] = stamp, f[3] = "hot.tar.zst.age"
		split($0, f, "-"); day = days(substr(f[2], 1, 8)); mode = substr(f[3], 1, index(f[3], ".") - 1)
		age_h = (days(substr(now, 1, 8)) * 24 + substr(now, 9, 2)) - (day * 24 + substr(f[2], 10, 2))
		keep = 0
		if (age_h < kh) keep = 1
		else if (age_h < kd * 24) { if (!(day in seen_day)) { seen_day[day] = 1; keep = 1 } }
		else if (age_h < kw * 7 * 24) { week = int(day / 7); if (!(week in seen_week)) { seen_week[week] = 1; keep = 1 } }
		if (mode == "consistent" && ++nc <= kc) keep = 1
		if (!keep) print
	}'); do
	base=${victim%.tar.zst.age}
	if [ "$DRY_RUN" = 1 ]; then
		echo "would remove $victim"
	else
		rm -f "$LOCAL/$victim" "$LOCAL/$victim.sha256" "$LOCAL/$base.meta"
	fi
	removed=$(( removed + 1 ))
done

kept=$(ls -1 "$LOCAL" | grep -c -E '\.tar\.zst\.age$' || true)
line="time=$(date '+%Y-%m-%dT%H:%M:%S%z') kind=pull host=$HOST pulled=$pulled failed=$failed removed=$removed kept=$kept dry_run=$DRY_RUN"
printf '%s\n' "$line" > "$LOCAL/status-pull"
echo "$line"
[ "$failed" -eq 0 ]
