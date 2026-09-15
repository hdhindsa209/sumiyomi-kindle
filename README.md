# Sumiyomi

A native manga reader for jailbroken Kindle e-ink devices, modeled on Mihon's UI/UX and rendered
directly to the framebuffer via [FBInk](https://github.com/NiLuJe/FBInk). Not affiliated with Mihon
or Amazon. Status: **M1 skeleton and M2 render engine done; M3 (data + WeebCentral) in device testing.**

- Design: `sumiyomi-design-doc (1).md` · M1 spec: `sumiyomi-M1-implementation-spec.md`
- Target device facts: `docs/DEVICE_FACTS.md` · Measurements and decisions: `docs/M1-notes.md`

## Prerequisites (macOS, Apple Silicon)

```sh
brew install cmake colima docker sdl2
colima start --vm-type vz --vz-rosetta --arch aarch64 --cpu 4 --memory 6
```

The Kindle toolchain (koxtoolchain `kindlehf`, Linux x86_64 only) runs in a Docker image that
`tools/kbuild.sh` builds on first use. **Keep the checkout under `$HOME`:** Colima only shares your
home directory with its VM.

## Build from a clean checkout

```sh
git clone --recursive <repo> sumiyomi && cd sumiyomi

# Host: unit tests + the SDL e-ink simulator
cmake --preset host && cmake --build --preset host
ctest --test-dir build/host
./build/host/sumiyomi                 # the M1 test card on desktop (mouse = touch, Esc = exit)

# Kindle: cross-compile (ARM EABI5 hard-float, Cortex-A9)
./tools/kbuild.sh                     # -> build/kindle/bin/{sumiyomi,m1_bench}
```

## Deploy and run on the device

USBNet must be up; the Kindle is `192.168.15.244` (override with `KINDLE_HOST`).

```sh
./tools/deploy.sh                     # package + copy to /mnt/us/extensions/sumiyomi
```

Then on the Kindle (with Wi-Fi on): **KUAL → Sumiyomi → Start Sumiyomi**. Browse → WeebCentral to find manga,
add them to the library; exit with **More → Exit Sumiyomi**. Library data: `/mnt/us/sumiyomi/sumiyomi.db`,
logs: `/mnt/us/sumiyomi/logs/`. The M1 latency test card is still built as `build/kindle/bin/m1_testcard`.

## Extensions

Sources live in `sources/<id>/{manifest.json,source.lua}` (Lua 5.4, sandboxed; API in design doc §6.3).

```sh
./build/host/ext_runner sources/weebcentral --live --search "frieren"        # run a source against the real site
./build/host/ext_runner sources/weebcentral --record tests/fixtures/x ...    # capture fixtures for tests
```

## Device checks

```sh
./tools/crash-tests.sh                # SIGTERM + abort(): native UI must be restored (PASS/FAIL)
scp build/kindle/bin/m1_bench root@192.168.15.244:/mnt/us/ && \
  ssh root@192.168.15.244 '/mnt/us/m1_bench' > docs/m1_bench.csv   # waveform latency CSV
```

## Recovering a stuck device

The app coexists with the native framework and restores it on every exit path. If it is ever
killed in a way it can't clean up after:

```sh
ssh root@192.168.15.244 'killall -CONT awesome; lipc-set-prop com.lab126.pillow disableEnablePillow enable; lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home'
```

Worst case, hold the power button for 30 s.
