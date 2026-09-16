#!/bin/sh
# KUAL action: remove the app, keep the library. See uninstall-run.sh.
exec /bin/sh "$(dirname "$0")/uninstall-run.sh" app
