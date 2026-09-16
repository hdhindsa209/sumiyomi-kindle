# Developing Sumiyomi

macOS on Apple Silicon is the setup this was built with; the Kindle toolchain runs in Docker.

```sh
brew install cmake colima docker sdl2
colima start --vm-type vz --vz-rosetta --arch aarch64 --cpu 4 --memory 6
```

The Kindle toolchain (koxtoolchain `kindlehf`, Linux x86_64 only) lives in a Docker image `tools/kbuild.sh` builds
on first use. **Keep the checkout under `$HOME`:** Colima only shares your home directory with its VM.

```sh
git clone --recursive <repo> sumiyomi-kindle && cd sumiyomi-kindle

cmake --preset host && cmake --build --preset host    # unit tests + the SDL simulator
ctest --test-dir build/host                           # 21 test programs
./build/host/sumiyomi                                 # the app on the desktop (mouse = touch, Esc = back)

./tools/kbuild.sh                                     # cross-compile for the Kindle
./tools/package-kual.sh                               # -> build/package/sumiyomi/ and sumiyomi-<version>.zip
```

Tests draw into a fake panel that only changes where a refresh was asked for, so a missing refresh fails a test.
Screens are locked in by golden images in `tests/golden/`; after a deliberate change, look at the new image and
re-record with `SUMI_UPDATE_GOLDENS=1 ./build/host/test_shell`.

## On a device

USBNet must be up; the Kindle is `192.168.15.244` by default (override with `KINDLE_HOST`).

```sh
./tools/deploy.sh                     # package + copy to /mnt/us/extensions/sumiyomi
./tools/crash-tests.sh                # SIGTERM + abort(): the native UI must come back (PASS/FAIL)
./build/host/ext_runner sources/weebcentral --live --search "frieren"        # run a source against the real site
./build/host/ext_runner sources/weebcentral --record tests/fixtures/x ...    # capture fixtures for tests
```

If a device is ever left with the native UI stopped:

```sh
ssh root@192.168.15.244 'killall -CONT awesome; lipc-set-prop com.lab126.pillow disableEnablePillow enable; lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home'
```

Worst case, hold the power button for 30 s.

## Debugging an Aidoku source without the device

The Kindle build of `aix_runner` is statically linked, so it runs under ARM emulation on a development machine —
which is how the "malformed Wasm binary" bug above was found:

```sh
./tools/kbuild.sh                 # builds build/kindle/bin/aix_runner
docker run --rm --platform linux/amd64 -v "$PWD:/src" sumiyomi-kindle-tc:2026.08 sh -c \
  'apt-get update -qq && apt-get install -y -qq qemu-user && cd /src && qemu-arm build/kindle/bin/aix_runner path/to/source.aix'
```

On the device itself: `/mnt/us/extensions/sumiyomi/bin/aix_runner /mnt/us/sumiyomi/sources/<id>.aix`.

## Sources

A source is `sources/<id>/{manifest.json,source.lua}` (Lua 5.4, sandboxed; API in the design doc §6.3).
WeebCentral ships with the app. More can be installed in-app from a repository, which is just files on a web
server:

```sh
tools/ext/build_index.py sources/ --out repo/     # writes repo/index.json with a SHA-256 per file
```

Host `repo/` anywhere (a GitHub raw path works), then in the app: **Browse → Extensions → repository row**.

## Documents

Design `sumiyomi-design-doc (1).md`, device facts `docs/DEVICE_FACTS.md`, milestone plans `docs/M*-plan.md`,
release steps `docs/RELEASE.md`, changes `docs/CHANGELOG.md`.
