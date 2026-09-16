#!/bin/sh
# Sumiyomi uninstaller. Two modes, because they lose very different things:
#
#   app    removes the program only (/mnt/us/extensions/sumiyomi). The library, read progress,
#          downloads and installed sources stay, so reinstalling picks up exactly where you were.
#   all    also removes the data (/mnt/us/sumiyomi): library, progress, categories, settings,
#          downloads, page cache, installed sources. There is no backup format yet, so this is final.
#
# KUAL menu entries can't pass arguments, so each mode gets its own one-line script next to this one.
#
# It copies itself to /tmp and runs from there before deleting anything: /bin/sh reads a script in
# chunks as it goes, so a script that deletes its own file can die halfway through — on the line
# that matters most here.
set -u

APP=/mnt/us/extensions/sumiyomi
DATA=/mnt/us/sumiyomi
LOG=/mnt/us/sumiyomi-uninstall.log      # outside both directories, so it survives either mode
MODE="${1:-app}"

say() {
    echo "$(date) [$MODE] $1" >>"$LOG"
    eips -q "$1" 2>/dev/null || true
}

# Re-exec from /tmp unless we're already there.
case "$0" in
/tmp/*) ;;
*)
    cp "$0" /tmp/sumiyomi-uninstall.sh || { say "could not copy the uninstaller to /tmp"; exit 1; }
    exec /bin/sh /tmp/sumiyomi-uninstall.sh "$MODE"
    ;;
esac

say "starting"

# Don't pull the binary out from under a running app: it holds the framebuffer, and the framework's
# own UI is stopped until it exits.
if pidof sumiyomi >/dev/null 2>&1; then
    say "Sumiyomi is running. Exit it first (More -> Exit Sumiyomi), then try again."
    exit 1
fi

rm -rf "$APP" || say "could not remove $APP"
if [ "$MODE" = "all" ]; then
    rm -rf "$DATA" || say "could not remove $DATA"
    say "Sumiyomi and its library removed. Reopen KUAL to refresh the menu."
else
    say "Sumiyomi removed; library kept in $DATA. Reopen KUAL to refresh the menu."
fi
exit 0
