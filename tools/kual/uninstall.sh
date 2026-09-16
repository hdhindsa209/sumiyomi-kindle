#!/bin/sh
# Sumiyomi uninstaller (KUAL action). Two modes, because they lose very different things:
#
#   app    removes the program only (/mnt/us/extensions/sumiyomi). The library, read progress,
#          downloads and installed sources stay, so reinstalling picks up exactly where you were.
#   all    also removes the data (/mnt/us/sumiyomi): library, progress, categories, settings,
#          downloads, page cache, installed sources. There is no backup format yet, so this is final.
#
# Menu entries are two taps deep and spell out what goes, since a single stray tap should never be
# able to delete a library.
set -u

APP=/mnt/us/extensions/sumiyomi
DATA=/mnt/us/sumiyomi
MODE="${1:-app}"

say() {   # a line on the Kindle's own toast, and in the log if the data directory is still there
    eips -q "$1" 2>/dev/null || true
    [ -d "$DATA/logs" ] && echo "$(date) uninstall: $1" >>"$DATA/logs/stderr.log"
}

# Don't pull the binary out from under a running app: it holds the framebuffer and the framework's
# UI is stopped while it runs.
if pidof sumiyomi >/dev/null 2>&1; then
    say "Sumiyomi is running. Exit it first (More -> Exit Sumiyomi), then try again."
    exit 1
fi

if [ "$MODE" = "all" ]; then
    rm -rf "$DATA"
    say "Sumiyomi and its library removed."
else
    say "Sumiyomi removed. Library and downloads kept in $DATA."
fi

# Last, because it deletes this script while it's running: the shell has already read the file, so
# rm -rf of our own directory is safe here, but nothing may follow it.
rm -rf "$APP"
exit 0
