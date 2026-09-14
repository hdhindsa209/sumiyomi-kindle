# M1 notes

Running log of surprises and resolved unknowns (spec §13).

## T01

- FBInk submodule pinned to `v1.25.0`. The device's own fbink binary reports `84bfe3b`,
  which isn't an upstream commit (probably a libkh build), so it can't be pinned exactly.
- Toolchain flags: `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard` (U1, see DEVICE_FACTS).
- Device glibc is 2.20 (`ld-2.20.so`): the cross sysroot's glibc must not be newer,
  or binaries fail with `GLIBC_2.xx not found`.

## T02

- Added `src/platform/signals.{h,cpp}` (not in §4's file list) so handler installation
  is reusable by `m1_bench` later rather than living in `main.cpp`.
- `stop`/`start`/`lipc-set-prop` are resolved to absolute paths in `acquire()`, so the
  signal handler can `execve` without a PATH search (execvp isn't async-signal-safe).
- `restore()` blocks all signals while it runs, so a SIGTERM mid-restore can't `_exit`
  with the framework half-started.
- Restore order: `start lab126_gui`, then `start framework` (its nonzero "already running"
  is ignored). Only jobs that `acquire()` actually stopped are restarted.
- U10 (FBInk in fatal handler) can't be tested until T03 — there is no display code yet.

### T02 device run #1 (2026-09-14) — FAILED on usability, passed on logged criteria
- All rc values, job states, and `prevent_screen_saver` correct after 3 back-to-back tests.
- But the UI did not really recover: after sleep/wake, the home screen shows briefly, then
  the screen goes blank on its own. A manual stop/start didn't fix it; only `reboot` did.
- **Lesson:** "jobs report start/running" does not prove the display pipeline came back.
  Acceptance now needs a hands-on usability check (including sleep/wake), on an isolated
  run with a fresh reboot beforehand.
- Reference: KOReader's `platform/kindle/koreader.sh` (master, fetched 2026-09-14) on Upstart:
  - It runs only `stop lab126_gui`, waits 1.25 s for teardown, and later runs only
    `start lab126_gui`. It never stops or starts `framework` separately.
  - It notes that the framework job sends SIGTERM on stop, and traps it.
  - Stopping the framework is opt-in (`--framework_stop`). By default KOReader keeps the
    framework running: it disables pillow, SIGSTOPs `awesome`, and stops a list of jobs
    (incl. `statusbar`, verified on PW6 @ 5.17.1.0.4).
  - That's evidence, not proof, that our separate `stop framework` / `start framework`
    steps are the problem.

### T02 strategy change: coexist with the framework
- Decision: adopt KOReader's default instead of diagnosing the stop/start failure.
  Nothing stops `framework` or `lab126_gui` any more, and that code has been removed.
- acquire: preventScreenSaver 1 → pillow disable → SIGSTOP awesome → stop statusbar (if the job exists).
- restore (reverse): start statusbar → SIGCONT awesome → pillow enable + `appmgrd start
  app://com.lab126.booklet.home` (KOReader does this relaunch too) → preventScreenSaver 0.
- awesome is paused with `kill(pid, SIGSTOP)` on pids found via `/proc/*/comm` (the same effect
  as `killall -STOP`). Resuming is a plain `kill()`, which is safe in a signal handler.
- Not copied from KOReader: its framebuffer dump/restore on exit (we draw nothing yet;
  revisit in T03).
- Known gap until T13's `run.sh` trap: if the binary is SIGKILLed, awesome stays paused.
  Recover with `killall -CONT awesome; lipc-set-prop com.lab126.pillow disableEnablePillow enable`.
- Memory: ~200 MB available with the framework running (vs ~369 MB stopped); budget is still fine.
- SIGTERM/abort device tests deferred until the strategy itself is validated.

### T02 device run #2 (coexist) — PASSED
- Held 60 s: nothing drew over the screen (no clock, status bar, or popups). rc=0, "power restored".
- After exit the home screen worked normally. Sleep/wake plus 2 min idle stayed usable
  (this is the case that failed in run #1).
- Side effect: the `appmgrd start app://com.lab126.booklet.home` relaunch closes KUAL and
  returns to home. Acceptable. Relevant for T13 (launched from KUAL, you exit to home, not KUAL).
- Still to do: SIGTERM / abort() device tests (deferred).
