#!/bin/sh
# Run a command inside the Kindle cross-build container, with the repo at /src.
#   ./tools/kbuild.sh                                   # configure + build preset "kindle"
#   ./tools/kbuild.sh file build/kindle/bin/sumiyomi    # any other command
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=sumiyomi-kindle-tc:2026.08

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/tools/docker"
fi

if [ $# -eq 0 ]; then
    set -- sh -c 'cmake --preset kindle && cmake --build --preset kindle'
fi

exec docker run --rm --platform linux/amd64 -v "$ROOT:/src" "$IMAGE" "$@"
