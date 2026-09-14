# M1 notes

Running log of surprises and resolved unknowns (spec §13).

## T01

- FBInk submodule: first pinned to `v1.25.0`, then **re-pinned in T03 to master `886f25f1`**
  (2026-08-06). v1.25.0 dates from Dec 2022 and lacks `fbink_refresh_rect`, `fbink_input_scan`
  and the `touch_*` fields in `FBInkState`, which the spec (checked against master) relies on. There
  is no newer tag. The device's own fbink reports `84bfe3b`, which isn't an upstream commit
  (probably a libkh build), so it can't be matched exactly.
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

## T03

- `libfbink.a` is built by CMake via FBInk's own Makefile: `make staticlib CROSS_TC=arm-kindlehf-linux-gnueabihf KINDLE=true`.
  (The spec's `KINDLE=1 static` also builds the CLI, which we don't need.) Output is `third_party/fbink/Release/libfbink.a`,
  and the default full feature set includes OpenType and input scan (checked with nm).
- Uses `fbink_refresh_rect` everywhere, as the spec asks.
- The bpp switch is not implemented. `open()` refuses anything other than 8bpp instead of guessing
  (DEVICE_FACTS: the device boots at 8bpp). So there's no bpp restore in `close()` either.
- `Rect` methods are defined inline in `display.h` (the spec only declares them).
- Signal handler now calls `display->clear_screen()` before `restore()` (U10). Untested until the
  deferred crash tests.
- Host preset builds no app until T11 (SDL backend); `power_stub.cpp` is kept for then.
- Test pattern includes a 150×150 black square at the top-left, to check orientation (DEVICE_FACTS rotate=3).

### T03 device run — PASSED
- `display open:` matched DEVICE_FACTS in every field: 'PaperWhite 4' 1072x1448 stride=1088 bpp=8
  dpi=300 inverted=0 legacy=0 unreliable_wait=0 touch_swap/mirror=0 rota=3 fb_size=6782976.
- The top-left marker appeared top-left. **Display orientation resolved:** FBInk's reported
  dimensions are already screen-correct, so no rotation math is needed when drawing.
  (Touch orientation is a separate question, still open until T05.)
- First measured refresh: GC16 full screen, 480 ms (FBInk documents ~450 ms). Wait-for-complete works.
- Even gray, nothing drew over it; one flash on clear; rc=0; home screen usable afterward.

## T04 — waveform benchmark (2026-09-14)

Raw data: `docs/m1_bench.csv` (20 timed refreshes per case, after one warm-up). Latency is measured from
`fbink_refresh_rect` to the return of `fbink_wait_for_complete`.
Two stray `statusbar …` lines from upstart were removed from the CSV. Since then, PowerGuard's helper processes send stdout to stderr.

| Mode | full 1072×1448 | half 1072×724 | 200×200 | FBInk doc |
|---|---|---|---|---|
| A2 | 148.4 | 134.1 | 120.9 | ~120 |
| DU | 289.5 | 275.2 | 262.0 | ~260 |
| GL16 | 477.7 | 463.4 | 450.1 | ~450 |
| REAGL ⚠️ invalid | 493.8 | 471.5 | 450.7 | ~450 |
| GC16 | 477.7 | 463.4 | 450.2 | ~450 |
| GC16_FLASH | 477.7 | 463.4 | 450.2 | — |

Median ms. Spread is tiny: min is within 0.3 ms of the median, and max is up to ~40 ms higher on a few cases.

### Does wait_for_complete really tell the modes apart? Yes, but only across tiers
- **Evidence that it does:**
  - There are three clearly separate tiers: A2 ~120–150, DU ~260–290, and the 16-gray modes ~450–495. A timer, or a completion signal that ignores the mode, couldn't produce that.
  - REAGL also differed from GL16 at full size (+16 ms).
- **GC16 == GC16_FLASH is expected, not a measurement artifact.** In FBInk's Kindle path (`refresh_kindle_rex`),
  both send `WAVEFORM_MODE_GC16`. `is_flashing` only switches `update_mode` from PARTIAL to FULL, which changes
  *which pixels* get driven, not the waveform's length. Also, the benchmark changes nearly every pixel on each refresh,
  so FULL vs PARTIAL drives almost the same pixels here.
- **GL16 == GC16 (within 0.2 ms at every size) is NOT explained.** FBInk sends different mode numbers
  (`WAVEFORM_MODE_ZELDA_GL16` vs `WAVEFORM_MODE_GC16`). Either this panel's waveform file gives both the same
  frame count, or the kernel substitutes one for the other. Timing alone can't tell which → **U13, open.** Deciding it
  needs a visual check: GL16 should leave visible ghosting on gray→gray changes, and GC16 should not.
- FBInk's "auto-upgrade to REAGL" (`fbink_mtk_toggle_auto_reagl`) is MediaTek-only, and this device has `isMTK=0`.
  So it can't be merging modes here.

### REAGL (U9): measurement invalid, still open
fbink.h: "On … all Kindle … REAGL & REAGLD generally expect to *always* be flashing". `display_fbink.cpp` sent
REAGL as a PARTIAL update. **Fixed:** REAGL now always sets `is_flashing` (REAGL itself suppresses the visible flash).
Re-measure with `m1_bench --mode=REAGL`. Even then, latency won't settle U9: REAGL vs GL16 is a quality question
(ghosting on real manga pages), so it belongs in M4 with real content.

### Does refresh size matter? Barely
- Going from 200×200 to full screen adds ~27 ms in every mode (about 14 ms per size step), whatever the waveform.
- Very low variance → deterministic EPDC timing. The size cost looks like per-pixel preparation on the single-core A9
  (a hypothesis, not verified).
- **For DirtyTracker / RefreshPolicy (§7.2/§7.3):**
  - Merging rects or promoting to full screen costs ≤ ~30 ms of latency. Rect size matters for *what visibly
    flashes/ghosts*, not for speed.
  - The "cap at 6 refresh calls because of per-ioctl overhead" rationale is untested: every refresh here was
    waited on in sequence, and concurrent submission wasn't measured.

### What this means for the §7.3 refresh policy
- "Pick the fastest mode the content allows" holds **across tiers**: B&W content → A2/DU buys 2–4× speed.
- **Within the 16-gray tier, latency is flat.** GL16 vs GC16 vs flash vs (probably) REAGL is purely a
  ghosting/flash trade-off. So the periodic GC16 flash (kFlashEvery) costs no extra latency, only the visible flash.
  The policy should reason about gray content by ghosting, not speed.

### Against design doc §10.1 budgets (panel time only; input, paint and I/O come on top)
| Budget | Refresh used | Measured | Verdict |
|---|---|---|---|
| Tap feedback 150 ms | A2, element-sized | 121 (200×200) … 148 (full) | ✅ for small elements, ~29 ms left for input+paint; tight. T12 measures end-to-end |
| Library tab switch 400 ms | DU, content area | ~275–290 | ✅ ~110 ms left |
| Library scroll page 400 ms | DU | ~275–290 | ✅ |
| Open manga detail 600 ms | GC16 full | 478 | ✅ ~120 ms left, tight |
| **Page turn, RAM cache 350 ms** | GL16 full | **478** | ❌ **physically infeasible**. Design doc §7.2 already says "under 500 ms, dominated by the panel" and the risk register calls the ~450 ms floor physics; §10.1 contradicts both |
| **Page turn, disk cache 500 ms** | ~25 ms read + GL16 | ~503 | ❌ marginal |
| **Reader menu open 350 ms** | 2× GL16 bars | ≥450 | ❌ with GL16. ✅ if menu chrome uses DU (~262 small) |
| Cold start 2500 ms | GC16 | 478 | ✅ |

**Proposed §10.1 revisions (not applied yet):** page turn RAM ≈ 500 ms, page turn disk ≈ 550 ms,
reader menu 350 ms kept but with DU for the menu bars. Spec §13 says to revise §10.1 before M2 when U8 differs materially.
