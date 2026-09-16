#!/bin/sh
# Build the Kindle binary and assemble the KUAL extension folder:
#   build/package/sumiyomi/{config.xml, menu.json, run.sh, bin/sumiyomi, assets/fonts/}
# which deploys to /mnt/us/extensions/sumiyomi on the device.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/build/package/sumiyomi"
# One version for everything: CMakeLists.txt's project(... VERSION).
VERSION="$(sed -n 's/^project(sumiyomi VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

"$ROOT/tools/kbuild.sh"

rm -rf "$PKG"
mkdir -p "$PKG/bin"
cp "$ROOT/tools/kual/config.xml" "$ROOT/tools/kual/menu.json" "$ROOT/tools/kual/run.sh" "$PKG/"
sed -i.bak "s|<version>.*</version>|<version>$VERSION</version>|" "$PKG/config.xml" && rm -f "$PKG/config.xml.bak"
cp "$ROOT/README.md" "$PKG/README.md"
chmod +x "$PKG/run.sh"

# UI fonts (design doc §3.3 layout: assets/fonts/), with their licenses.
mkdir -p "$PKG/assets/fonts"
cp "$ROOT"/assets/fonts/*.ttf "$ROOT"/assets/fonts/LICENSE-*.txt "$PKG/assets/fonts/"
# CA bundle for TLS (Mozilla's, via curl.se; MPL 2.0). The Kindle's own store is too old.
mkdir -p "$PKG/assets/certs"
cp "$ROOT/assets/certs/cacert.pem" "$PKG/assets/certs/"

# Bundled source extensions (design doc §3.3: sources/<id>/{manifest.json,source.lua}).
cp -R "$ROOT/sources" "$PKG/sources"

# Strip inside the toolchain container (paths are relative to the repo mounted at /src).
"$ROOT/tools/kbuild.sh" sh -c 'for b in sumiyomi http_smoke page_bench aix_runner; do arm-kindlehf-linux-gnueabihf-strip -o build/package/sumiyomi/bin/$b build/kindle/bin/$b; done'
chmod +x "$PKG/bin/sumiyomi" "$PKG/bin/http_smoke" "$PKG/bin/page_bench" "$PKG/bin/aix_runner"

# A zip to hand out: unzip it into /mnt/us/extensions/ on a jailbroken Kindle with KUAL.
ZIP="$ROOT/build/package/sumiyomi-$VERSION.zip"
rm -f "$ZIP"
(cd "$ROOT/build/package" && zip -qr "$ZIP" sumiyomi)

echo "packaged: $PKG"
echo "release:  $ZIP"
ls -l "$PKG" "$PKG/bin"
