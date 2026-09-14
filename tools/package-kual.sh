#!/bin/sh
# Build the Kindle binary and assemble the KUAL extension folder:
#   build/package/sumiyomi/{config.xml, menu.json, run.sh, bin/sumiyomi, assets/fonts/}
# which deploys to /mnt/us/extensions/sumiyomi on the device.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/build/package/sumiyomi"

"$ROOT/tools/kbuild.sh"

rm -rf "$PKG"
mkdir -p "$PKG/bin"
cp "$ROOT/tools/kual/config.xml" "$ROOT/tools/kual/menu.json" "$ROOT/tools/kual/run.sh" "$PKG/"
chmod +x "$PKG/run.sh"

# UI fonts (design doc §3.3 layout: assets/fonts/), with their licenses.
mkdir -p "$PKG/assets/fonts"
cp "$ROOT"/assets/fonts/*.ttf "$ROOT"/assets/fonts/LICENSE-*.txt "$PKG/assets/fonts/"

# Strip inside the toolchain container (paths are relative to the repo mounted at /src).
"$ROOT/tools/kbuild.sh" arm-kindlehf-linux-gnueabihf-strip -o build/package/sumiyomi/bin/sumiyomi build/kindle/bin/sumiyomi
chmod +x "$PKG/bin/sumiyomi"

echo "packaged: $PKG"
ls -l "$PKG" "$PKG/bin"
