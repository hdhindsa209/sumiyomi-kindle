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

# Colima only shares $HOME with its VM by default: a checkout elsewhere mounts as an empty /src.
if ! docker run --rm --platform linux/amd64 -v "$ROOT:/src" "$IMAGE" test -f /src/CMakeLists.txt; then
    echo "kbuild: $ROOT is not visible inside the container." >&2
    echo "        Keep the checkout under \$HOME, or add its path to Colima's mounts (colima start --edit)." >&2
    exit 1
fi

exec docker run --rm --platform linux/amd64 -v "$ROOT:/src" "$IMAGE" "$@"
