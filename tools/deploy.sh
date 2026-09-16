#!/bin/sh
# Copy the packaged KUAL extension to the device over USBNet.
#   ./tools/deploy.sh             # package + deploy
#   ./tools/deploy.sh --no-build  # deploy the existing build/package/sumiyomi
# KINDLE_HOST overrides the device address (DEVICE_FACTS: 192.168.15.244; .201 is the Mac).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${KINDLE_HOST:-192.168.15.244}"
DEST=/mnt/us/extensions/sumiyomi
PKG="$ROOT/build/package/sumiyomi"

if [ "${1:-}" != "--no-build" ]; then
    "$ROOT/tools/package-kual.sh" >/dev/null
fi
[ -x "$PKG/bin/sumiyomi" ] || { echo "no package at $PKG; run without --no-build" >&2; exit 1; }

# One SSH connection for all steps, so a password is asked for at most once.
CTL="${TMPDIR:-/tmp}/sumiyomi-ssh-%r@%h"
SSH_OPTS="-o ControlMaster=auto -o ControlPath=$CTL -o ControlPersist=30"

start=$(date +%s)
ssh $SSH_OPTS "root@$HOST" "mkdir -p $DEST/bin"
if ssh $SSH_OPTS "root@$HOST" 'command -v rsync >/dev/null'; then
    rsync -a --delete -e "ssh $SSH_OPTS" "$PKG/" "root@$HOST:$DEST/"
else
    # rsync isn't on the device. Send the whole package as a tar stream rather than naming files:
    # a hand-maintained list silently drops whatever was added since (which is how the uninstall
    # scripts reached v1.0.2 on the device as menu entries pointing at nothing).
    tar -C "$PKG" -cf - . | ssh $SSH_OPTS "root@$HOST" "mkdir -p $DEST && tar -C $DEST -xf -"
fi
ssh $SSH_OPTS "root@$HOST" "chmod +x $DEST/*.sh $DEST/bin/*; ls -l $DEST $DEST/bin"
echo "deployed to $HOST:$DEST in $(( $(date +%s) - start ))s (excluding build)"
