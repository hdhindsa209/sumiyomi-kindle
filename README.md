# Sumiyomi

A manga reader for jailbroken Kindle e-ink devices, in the shape of [Mihon](https://mihon.app), drawn
straight to the framebuffer through [FBInk](https://github.com/NiLuJe/FBInk). Not connected with Mihon or
Amazon. Manga come from the sources you install; nothing is hosted by the app.

**Status: v0.1.** Built and tested on a base Kindle (i.MX6 SoloLite, firmware 5.17.1.0.3, WinterBreak + KUAL).

## What it does

- **Library** as a list or a grid of covers, with categories as tabs. Hold a manga to select several, then set
  categories, download unread chapters, mark read or unread, or remove them.
- **Browse** a source's popular and latest listings, search with an on-screen keyboard, and open a manga's page
  with its chapters, sorting, filtering and a per-manga download queue.
- **Reader** with tap zones and page keys, right-to-left or left-to-right, fit page or width, double-page splitting,
  cropping, margins, contrast, darkness, dithering, front light, and an optional progress bar. A chapter loads
  whole, so turning pages doesn't wait.
- **Downloads** you start yourself: the original images are kept on the device and read instead of the network.
- **Updates and History**: check the library (or one category) for new chapters, optionally downloading them,
  and pick up where you left off.
- **Settings and storage**: reader defaults, the page cache's size and contents, and what downloads use.

E-ink first: pure black and white, no swipes (arrows and page keys), one loading screen rather than a
half-drawn one, and a full refresh whenever the screen or page changes.

## Install on a Kindle

Needs a jailbroken Kindle with KUAL. From a release zip:

1. Copy the `sumiyomi` folder out of `sumiyomi-<version>.zip` into `/mnt/us/extensions/` on the Kindle.
2. **KUAL → Sumiyomi → Start Sumiyomi**, with Wi-Fi on.
3. **Browse → WeebCentral** to find manga, then **More → Exit Sumiyomi** to leave.

Your library lives in `/mnt/us/sumiyomi/` (database, downloads, page cache, logs). Deleting the extension folder
leaves it; delete `/mnt/us/sumiyomi/` too for a clean slate.

## Sources

A source is `sources/<id>/{manifest.json,source.lua}` (Lua 5.4, sandboxed; the API is in the design doc §6.3).
WeebCentral ships with the app.

More can be installed from a repository, which is just files on a web server:

```sh
tools/ext/build_index.py sources/ --out repo/     # writes repo/index.json with a SHA-256 per file
```

Host `repo/` anywhere (a GitHub raw path works), then in the app: **Browse → Extensions → repository row**, and
type the address of `index.json`. Installing checks the api level and the checksums, and the source works without
restarting the app.

## Build it yourself (macOS, Apple Silicon)

```sh
brew install cmake colima docker sdl2
colima start --vm-type vz --vz-rosetta --arch aarch64 --cpu 4 --memory 6
```

The Kindle toolchain (koxtoolchain `kindlehf`, Linux x86_64 only) runs in a Docker image `tools/kbuild.sh` builds
on first use. **Keep the checkout under `$HOME`:** Colima only shares your home directory with its VM.

```sh
git clone --recursive <repo> sumiyomi && cd sumiyomi

cmake --preset host && cmake --build --preset host    # unit tests + the SDL simulator
ctest --test-dir build/host                           # 21 test programs
./build/host/sumiyomi                                 # the app on the desktop (mouse = touch, Esc = back)

./tools/kbuild.sh                                     # cross-compile for the Kindle
./tools/package-kual.sh                               # -> build/package/sumiyomi/ and sumiyomi-<version>.zip
```

Tests draw into a fake panel that only changes where a refresh was asked for, so a missing refresh fails a test.
Screens are locked in by golden images in `tests/golden/`; after a deliberate change, look at the new image and
re-record with `SUMI_UPDATE_GOLDENS=1 ./build/host/test_shell`.

## Develop against a device

USBNet must be up; the Kindle is `192.168.15.244` (override with `KINDLE_HOST`).

```sh
./tools/deploy.sh                     # package + copy to /mnt/us/extensions/sumiyomi
./tools/crash-tests.sh                # SIGTERM + abort(): the native UI must come back (PASS/FAIL)
./build/host/ext_runner sources/weebcentral --live --search "frieren"        # run a source against the real site
./build/host/ext_runner sources/weebcentral --record tests/fixtures/x ...    # capture fixtures for tests
```

Documents: design `sumiyomi-design-doc (1).md`, device facts `docs/DEVICE_FACTS.md`, milestone plans `docs/M*-plan.md`,
release steps `docs/RELEASE.md`.

## If the device is left in a bad state

The app coexists with the native framework and puts it back on every exit path. If it is ever killed in a way it
can't clean up after:

```sh
ssh root@192.168.15.244 'killall -CONT awesome; lipc-set-prop com.lab126.pillow disableEnablePillow enable; lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home'
```

Worst case, hold the power button for 30 s.
