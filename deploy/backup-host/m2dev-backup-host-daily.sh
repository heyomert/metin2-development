#!/bin/sh
# Daily job of the backup host (docs/backup.md): pull new backups, then prove the newest one restores.
#
#   m2dev-backup-host-daily.sh <ssh host> <local dir> <age identity file>
#
#   RESTORE_ON=server (default)  run m2dev-restore-test on the game server against its copy of the same file (the
#                                pull checked that both have the same sha256); the private key is streamed over ssh
#                                and never stored there. For the test VM, where the backup host has no MariaDB.
#   RESTORE_ON=local             run m2dev-restore-test here on the pulled copy (production backup host with
#                                MariaDB and deploy/freebsd/backup installed).
#
# Writes one key=value line to <local dir>/status-daily; exit status is non-zero if the pull or the test failed.
set -eu
# The restore test's result is the status of a pipeline (... | tail): without pipefail a failed test reads as ok
set -o pipefail

HOST=${1:-}
LOCAL=${2:-}
KEY=${3:-}
if [ -z "$HOST" ] || [ -z "$LOCAL" ] || [ -z "$KEY" ]; then
	echo "usage: $0 <ssh host> <local dir> <age identity file>" >&2
	exit 2
fi
REMOTE_DIR=${REMOTE_DIR:-/var/backups/m2dev}
RESTORE_ON=${RESTORE_ON:-server}
[ -r "$KEY" ] || { echo "cannot read $KEY" >&2; exit 2; }

pull=ok
REMOTE_DIR="$REMOTE_DIR" sh "$(dirname "$0")/m2dev-pull-backups.sh" "$HOST" "$LOCAL" || pull=fail

newest=$(ls -1 "$LOCAL" | grep -E '^m2dev-[0-9]{8}T[0-9]{6}Z-(hot|consistent)\.tar\.zst\.age$' | sort | tail -n 1 || true)
restore=fail
detail=
if [ -z "$newest" ]; then
	detail="no backup pulled yet"
elif [ "$RESTORE_ON" = local ]; then
	detail=$(m2dev-restore-test "$LOCAL/$newest" "$KEY" 2>&1 | tail -n 1) && restore=ok
else
	# Keys can carry Windows line ends when edited there; age wants plain lines
	detail=$(tr -d '\r' < "$KEY" | ssh "$HOST" "/usr/local/sbin/m2dev-restore-test '$REMOTE_DIR/$newest' -" 2>&1 | tail -n 1) &&
		restore=ok
fi

line="time=$(date '+%Y-%m-%dT%H:%M:%S%z') kind=daily host=$HOST pull=$pull restore_test=$restore file=${newest:-none}"
printf '%s\n%s\n' "$line" "$detail" > "$LOCAL/status-daily"
echo "$line"
echo "$detail"
[ "$pull" = ok ] && [ "$restore" = ok ]
