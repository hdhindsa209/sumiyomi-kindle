#!/bin/sh
# §2 criterion 6 on device: SIGTERM and abort() must leave the native UI restored.
# Deploys, then runs both tests back to back over one SSH connection, checking:
#   exit status (143 / 134), "caught signal N" + "power restored" in the log,
#   awesome not left stopped, statusbar running (if the job exists), and powerd's screensaver not
#   left prevented (the app no longer takes that lock at all, so it must never be set).
# Prints PASS/FAIL per test and overall. Look at the screen too: it should flash clear and return home.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${KINDLE_HOST:-192.168.15.244}"
CTL="${TMPDIR:-/tmp}/sumiyomi-ssh-%r@%h"
SSH_OPTS="-o ControlMaster=auto -o ControlPath=$CTL -o ControlPersist=120"
export KINDLE_HOST="$HOST"

"$ROOT/tools/deploy.sh"

ssh $SSH_OPTS "root@$HOST" sh -s <<'REMOTE'
BIN=/mnt/us/extensions/sumiyomi/bin/sumiyomi
LOG=/mnt/us/sumiyomi/logs/crash-tests.log
mkdir -p /mnt/us/sumiyomi/logs
: >"$LOG"
overall=PASS

check_state() {   # $1 = test name; prints failures, returns 1 if any
    bad=0
    for p in $(pidof awesome); do
        if grep -q '^State:.*T' /proc/$p/status; then echo "  FAIL $1: awesome (pid $p) still stopped"; bad=1; fi
    done
    if [ -f /etc/upstart/statusbar.conf ] && ! status statusbar 2>/dev/null | grep -q 'start/running'; then
        echo "  FAIL $1: statusbar not running ($(status statusbar 2>&1))"; bad=1
    fi
    if ! lipc-get-prop com.lab126.powerd status 2>/dev/null | grep -qi 'prevent_screen_saver:0'; then
        echo "  FAIL $1: preventScreenSaver not restored"; bad=1
    fi
    return $bad
}

run_test() {   # $1 = name, $2 = expected rc, $3 = signal number, remaining = how to end it
    name=$1; want=$2; sig=$3; shift 3
    echo "=== $name" | tee -a "$LOG"
    before=$(wc -l <"$LOG")
    eval "$@"
    got=$rc
    sleep 12   # let the home screen relaunch settle
    out=$(tail -n +"$((before + 1))" "$LOG")
    ok=1
    [ "$got" = "$want" ] || { echo "  FAIL $name: rc=$got, expected $want"; ok=0; }
    echo "$out" | grep -q "caught signal $sig" || { echo "  FAIL $name: no 'caught signal $sig' in log"; ok=0; }
    echo "$out" | grep -q "power restored"     || { echo "  FAIL $name: no 'power restored' in log"; ok=0; }
    check_state "$name" || ok=0
    if [ $ok = 1 ]; then echo "  PASS $name (rc=$got)"; else overall=FAIL; fi
}

run_test SIGTERM 143 15 '$BIN 2>>"$LOG" & P=$!; sleep 8; kill -TERM $P; wait $P; rc=$?'
run_test abort   134 6  '$BIN --crash=abort 2>>"$LOG"; rc=$?'

echo "=== OVERALL: $overall"
echo "--- log ---"
cat "$LOG"
REMOTE
