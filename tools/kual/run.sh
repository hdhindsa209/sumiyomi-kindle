#!/bin/sh
# Sumiyomi launcher (KUAL action). Runs the app in the foreground, like KOReader's koreader.sh.
#
# The binary acquires and restores the native UI itself (PowerGuard), including from every
# catchable signal. The one case it can't handle is SIGKILL (e.g. the OOM killer): exit
# status 137. Only then does this script undo PowerGuard's changes, so the home screen is
# never relaunched twice.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
DATA=/mnt/us/sumiyomi
LOGS="$DATA/logs"
mkdir -p "$LOGS"

# Keep logs bounded (last 500 KB each).
for f in "$LOGS/stdout.log" "$LOGS/stderr.log"; do
    if [ -f "$f" ]; then
        tail -c 500000 "$f" >"$f.new" && mv -f "$f.new" "$f"
    fi
done

# From KOReader: let KUAL settle, and undo the Kindlet's nice value of 5.
usleep 250000 2>/dev/null || sleep 1
if [ "$(nice)" = "5" ]; then
    renice -n -5 $$ >/dev/null 2>&1
fi

echo "=== $(date) run.sh start" >>"$LOGS/stderr.log"
"$HERE/bin/sumiyomi" >>"$LOGS/stdout.log" 2>>"$LOGS/stderr.log"
rc=$?
echo "=== $(date) run.sh: sumiyomi exited rc=$rc" >>"$LOGS/stderr.log"

if [ "$rc" -eq 137 ]; then
    echo "=== SIGKILL: restoring native UI from run.sh" >>"$LOGS/stderr.log"
    [ -f /etc/upstart/statusbar.conf ] && start statusbar >/dev/null 2>&1
    killall -CONT awesome 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
fi
exit 0
