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
    # rsync isn't on the device: copy the (small) package with scp instead.
    scp $SSH_OPTS -q -r "$PKG/config.xml" "$PKG/menu.json" "$PKG/run.sh" "root@$HOST:$DEST/"
    scp $SSH_OPTS -q "$PKG/bin/sumiyomi" "root@$HOST:$DEST/bin/sumiyomi"
    ssh $SSH_OPTS "root@$HOST" "mkdir -p $DEST/assets/fonts"
    scp $SSH_OPTS -q "$PKG"/assets/fonts/* "root@$HOST:$DEST/assets/fonts/"
    ssh $SSH_OPTS "root@$HOST" "mkdir -p $DEST/assets/certs"
    scp $SSH_OPTS -q "$PKG/assets/certs/cacert.pem" "root@$HOST:$DEST/assets/certs/"
fi
ssh $SSH_OPTS "root@$HOST" "chmod +x $DEST/run.sh $DEST/bin/sumiyomi; ls -l $DEST $DEST/bin"
echo "deployed to $HOST:$DEST in $(( $(date +%s) - start ))s (excluding build)"
