#!/bin/sh
# Which suspend trigger actually suspends this Kindle, and can we tell afterwards that it did?
#
# Run it ON THE DEVICE with Sumiyomi closed. It tries each trigger in turn, waits, and records the
# clocks either side, so the log says both which trigger worked and which clock noticed:
#
#   ssh root@192.168.15.244 'nohup sh /mnt/us/extensions/sumiyomi/bin/suspend-probe.sh >/dev/null 2>&1 &'
#   ...wait two minutes (the network drops while it's asleep; that's the point)...
#   ssh root@192.168.15.244 'cat /mnt/us/sumiyomi/suspend-probe.log'
#
# Reading it: `real` is the RTC-backed wall clock, `mono` is /proc/uptime. If a trigger suspended the
# device, real advances by the full wait while mono lags behind (monotonic time stops while
# suspended). If both advance together, nothing was suspended. `dmesg` is the ground truth either
# way: the kernel logs "PM: suspend entry" every time it really goes down.
LOG=/mnt/us/sumiyomi/suspend-probe.log
WAIT=25

mkdir -p /mnt/us/sumiyomi
: >"$LOG"

stamp() { echo "  $1 real=$(date +%s) mono=$(cut -d' ' -f1 /proc/uptime) suspends=$(dmesg | grep -ci 'suspend entry')" >>"$LOG"; }

try() {   # $1 = name, $2 = command
    echo "=== $1: $2" >>"$LOG"
    stamp before
    sh -c "$2" >>"$LOG" 2>&1
    echo "  rc=$?" >>"$LOG"
    sleep "$WAIT"
    stamp "after (waited ${WAIT}s)"
}

echo "suspend probe, $(date)" >>"$LOG"
echo "powerd status: $(lipc-get-prop com.lab126.powerd status 2>&1)" >>"$LOG"

try powerd_test  'powerd_test -s'
try powerButton  'lipc-set-prop com.lab126.powerd powerButton 1'
try sysfs        'echo mem > /sys/power/state'

echo "done" >>"$LOG"
