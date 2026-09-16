#!/bin/sh
# KUAL action: remove the app and all its data. See uninstall-run.sh.
exec /bin/sh "$(dirname "$0")/uninstall-run.sh" all
