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

**§10.1 revisions (applied to the design doc 2026-09-14):** page turn RAM ≈ 500 ms, page turn disk ≈ 550 ms,
reader menu 350 ms kept but with DU for the menu bars. Spec §13 says to revise §10.1 before M2 when U8 differs materially.

## T05 — input (code notes, pending device verification)

- Devices come from `fbink_input_scan(TOUCHSCREEN | PAGINATION_BUTTONS | POWER_BUTTON)`. The fds of unmatched
  devices are closed.
- Protocol B only. If there's no `ABS_MT_SLOT`, `open()` fails instead of guessing.
  Single-touch: the first contact's slot is tracked. `SYN_DROPPED` → Cancel, then ignore events until the next `SYN_REPORT`.
- Transform per §6.2, with one correction: it subtracts `absinfo.minimum` before scaling. The spec's formula assumes min=0.
  Digitizer X/Y ranges are logged at open (U6).
- `EVIOCSCLOCKID(CLOCK_MONOTONIC)`, so `RawEvent::t_ms` shares a clock with `mono_ms()` (needed for T12's 150 ms target).
- **Touchscreen is EVIOCGRAB'd.** Since we coexist, the framework is still running and would otherwise act on our
  taps (e.g. open things on the hidden home screen). The kernel releases the grab automatically on exit or crash.
  Power button is *not* grabbed, so system sleep keeps working.
- `main.cpp` is now the T05 corner-tap harness. The T02/T03 `--crash=` flags are gone; they'll be re-added for the
  deferred crash tests.

### T05 device run — PASSED
- Digitizer `ABS_MT_POSITION_X 0..1072, Y 0..1448`: 1:1 with the panel. No swap, mirror or real scaling
  (the formula's max+1 divisor costs at most 1 px).
- Corner and center taps landed under the finger. The two "FAR" logs were finger imprecision (repeat taps nearby were OK).
- A drag produces exactly one down/up pair, with no stray events. Home screen fine afterward.
- U5 (protocol B) and U6 (range) confirmed; the touch-transform open question is closed.

## T06–T09 — host-only core (2026-09-14)

All four built and tested on the host (`cmake --preset host && cmake --build --preset host && ctest --test-dir build/host`).
The core library also builds cleanly with the Kindle GCC. Test harness: `tests/unit/check.h`, no dependencies.
I checked that it really reports failures.

| Task | Tests | Decisions worth knowing |
|---|---|---|
| T06 Canvas | 14 | Polarity is converted when pixels are written (the fb holds panel values). `invert_rect` needs no conversion. `quantize_rect` uses a nearest-level LUT with exact endpoints; levels are clamped to 2..256, and 256 = no-op. `stroke_rect` draws inside the rect, and becomes a solid fill once 2×thickness ≥ w or h |
| T07 DirtyTracker | 14 | "Within 16 px" means the gap on **both** axes is ≤ 16 (touching or overlapping included). Gap merges repeat until nothing changes, and run again after every cap merge, so output rects never overlap. The 60% check sums the areas of those disjoint rects |
| T08 GestureRecognizer | 17 | Leaving the slop at any point disqualifies Tap/LongPress. LongPress fires on release if `tick()` missed it. Swipe boundaries are inclusive (≥ 90 px, ≤ 600 ms), and a diagonal tie counts as horizontal. **Added `wants_tick()`** (not in spec) so T10 can arm its timerfd only while a long-press is still possible |
| T09 RefreshPolicy | 12 | The Nth non-flashing full-screen refresh becomes GC16_FLASH (N=6 by default, so every 6th flashes). Rects promoted to full screen count too; partial refreshes don't. **The flash is deferred past A2** full-screen requests, so tap feedback is never turned into a 478 ms flash. An explicit GC16_FLASH resets the count |

Open interpretation: DU full-screen refreshes count toward the flash and can be upgraded. Revisit in M2 if flashing on a list scroll feels wrong.

## T11 — SDL simulator backend (2026-09-14)

- `display_sdl.cpp` / `input_sdl.cpp`, built on the host when SDL2 is found (Homebrew `sdl2` is now `sdl2-compat`,
  the SDL2 API on top of SDL3). Device geometry is 1072×1448 with **stride 1088**, as on the device, so stride bugs show up on desktop.
- **Latency uses the T04 measurements, not FBInk's documented values:** A2 121 / DU 262 / 16-gray 450 ms
  at 200×200, plus up to +27 ms at full screen.
- **Ghosting:**
  - A shadow panel buffer keeps 8% of the previous value per non-flashing refresh.
  - GC16_FLASH shows black for a third of its duration, then resets the residual.
  - Pixels outside the refreshed rect never change.
- **A2 and DU drive pixels to pure black/white.** A2 over more than 2 distinct values logs `A2 MISUSE` and draws diagonal stripes.
- **Input:** SDL has no pollable fd, so `Input::fds()` is empty and the owner must call `drain(Input::kNoFd, …)` periodically
  (documented in input.h). **T10's EventLoop needs a polling fallback when there are no fds.** The host simulator
  therefore won't be idle-silent; that requirement is device-only.
- `tools/sim/sim_testcard --selftest=DIR` runs a scripted pass headless (`SDL_VIDEODRIVER=dummy`) and saves BMP
  snapshots. Verified: quantization banding, DU binarization, ghost after a GL16 move, A2 misuse stripes + warning,
  policy A2→DU downgrade, clean exit.
- SDL mouse→device coordinate mapping isn't covered by the self-test (pushed mouse events are rescaled by SDL, so the
  script injects RawEvents). **Checked interactively by the user on the Mac:** a real click inverted the box; ghosting,
  the A2 misuse stripes + warning, and clean exit all matched.
- T12 note: the spec's test card has an FBInk OpenType text line. That has no SDL equivalent, so the simulator
  test card will have to skip it or draw a placeholder.

## T10 — EventLoop (code notes, pending device verification)

- **Two implementations of `core/loop.h`** (the spec names a single `loop.cpp`):
  - `loop_linux.cpp` is the spec's design: epoll with no timeout, eventfd for `stop()`, timerfd for the tick. Used on the device.
  - `loop_posix.cpp` uses poll() + self-pipe with the same behavior, because macOS (the SDL simulator host) has no epoll/eventfd/timerfd.
    CMake picks by `CMAKE_SYSTEM_NAME`.
- **API additions to §9:**
  - `init()` (fallible setup, no exceptions).
  - `arm_tick(bool)`: the tick starts disarmed, and the owner arms it from `GestureRecognizer::wants_tick()`.
    Only a state change costs a `timerfd_settime`.
  - `add_poll(fn, ms)`: the fallback for fd-less input (SDL, T11). Device code never calls it.
  - The spec's unused `InputFn` alias is dropped.
- `stop()` is a single `write(2)` to the eventfd, so it's safe in a signal handler (tested with SIGALRM).
  The existing handlers still restore + `_exit`. Graceful SIGTERM-via-loop can come in T12 if wanted.
- test_loop (7 tests): fd dispatch, stop from a callback / another thread / a signal handler, tick only while armed,
  disarmed by default, poll fallback. **All tests (35 + 7) pass on macOS, and natively on Linux arm64 with GCC**
  in a throwaway Debian container, which exercises the real epoll/eventfd/timerfd code.
- Idle check: the `/proc/<pid>/status` context-switch counters, per the spec change (strace isn't on the device).

### T10 device run — PASSED
- **Idle check:** `voluntary_ctxt_switches` 31 → 31 and `nonvoluntary_ctxt_switches` 26 → 26 over 10 s with no input.
  The process never woke while idle.
- Touch-down dots appeared on every tap. Log: 5× Tap, SwipeL, SwipeR, LongPress, and the long-press exited as designed.
- rc=0, "power restored", "clean exit"; home screen normal afterward.
- **Not verified on device:** step 4 (tick disarms after a gesture) was skipped, because the long-press had already ended the run.
  Covered by `test_tick_only_while_armed` on macOS and natively on Linux (the real timerfd path). T12's own
  idle behavior will cover it on device again.

## T12 — M1 test card (code notes, pending device verification)

- `app/m1_testcard.{h,cpp}` is backend-agnostic. `main.cpp` wires platform + loop + app, and is **the same file for
  both backends**. The host `sumiyomi` target is the SDL simulator build (§2 criterion 7).
- **Tap latency (decided with the user):** each tap logs event→submit (refresh ioctl returned) and
  event→complete (`fbink_wait_for_complete` returned). **Pass/fail is the median of event→complete over 20 taps, ≤150 ms.**
  A `SUMMARY … PASS|FAIL` line is logged after every 20 taps. Design doc §10.1 and spec T12 are updated to match.
- The text line uses `Display::draw_label()`, a new M1-only virtual. FBInk backend: `fbink_print_ot` with
  `/usr/java/lib/fonts/Amazon-Ember-Regular.ttf` (read at runtime, not bundled). SDL backend: placeholder bar.
- The mode cycle runs through RefreshPolicy with `flash_interval = 0`, so the logged modes are exactly GC16/GL16/DU.
- Key::Back (simulator Esc / window close) also exits. The device has no back key.
- Host smoke test: the simulator build starts, draws, and handles SIGTERM (clear + rc 143).

### T12 device run #1 (2026-09-14)
- **§2 criterion 3 (tap feedback): PASS, clearly.**
  - Taps #1–34 (no other work queued): event→submit **3–4 ms**, event→complete **124–126 ms**.
  - First SUMMARY: median complete **125 ms**, max 126 ms, budget 150 ms.
  - A2 itself is ~121 ms (T04), so our own overhead (touch event → refresh submitted) is ~4 ms.
- **Criterion 4: PASS.** Full repaints: GC16 482–485 ms, GL16 482–484 ms, DU 293–295 ms (draw+submit 4–8 ms).
- **Criterion 5: PASS.** Long-press exited; rc=0, power restored, clean exit.
- **Criterion 2 (text line):** no `text line:` warning was logged (FBInk font load + print succeeded). Visual rendering not yet confirmed.
- **FINDING — input backlog under rapid taps (taps #35–44: 2.2–3.4 s).**
  - Each full repaint runs synchronously inside the input handler and blocks on `wait_for_complete` (~480 ms).
  - Taps arriving faster than that queue in the kernel. "From tap" latencies grew to ~4.1 s, and box taps queued
    behind the repaints were handled 2–3 s late.
  - Timestamps come from the kernel's input event, so the metric correctly counted the queueing.
  - The second SUMMARY still said PASS: its 20-tap window (#21–40) had 14 good taps, so the median stayed 125 ms,
    even though max was 2819 ms. **The summary line hides this.**
  - Not an M1 acceptance failure (criterion 3 is met), but it is exactly the "slow and awkward" failure mode
    the design doc warns about. It has to be solved before M2 builds screens on this loop: don't block the loop on
    refresh completion, and coalesce queued input and refresh requests.
- Decision (user): commit T12 as passing; fix the backlog before M2. Tracked as **M1-F1: non-blocking refresh scheduling +
  input/refresh coalescing + stale-event dropping** (needs its own tests).
- SUMMARY now reports the over-budget count and FAILs if any tap in the window exceeds 500 ms, so a good median can't hide stalls.
- Open (not answered yet): (a) how many outside taps were made. 29 repaints were logged, spaced like rapid real taps;
  if only ~3 were made, there's a spurious-Tap bug. (b) Visual confirmation that the Amazon Ember text line rendered.

## T13 — KUAL packaging + deploy (code notes, pending device verification)

- Templates in `tools/kual/`: `config.xml`, `menu.json`, `run.sh`.
  - **`config.xml` is required by KUAL** (it points at menu.json) and isn't in spec §11.1.
  - The menu shape copies KOReader's `platform/kindle/extensions/koreader/menu.json`: a nested item with an absolute `action` and `"status": false`.
- `run.sh` also follows KOReader's `--kual` path: the app runs in the foreground, after a 250 ms settle, and resets
  nice back to 0 if KUAL's Kindlet launched it at nice 5.
  - **Fallback restore only when rc = 137 (SIGKILL).** That's the one case PowerGuard can't handle; every catchable
    signal already restores inside the binary. This settles spec §8's 🟡 about the fallback relaunching home: it does,
    but only on that path, so home is never relaunched twice.
  - Logs go to `/mnt/us/sumiyomi/logs/{stdout,stderr}.log`, capped at 500 KB each.
- `tools/package-kual.sh`: builds, then assembles `build/package/sumiyomi/` with a stripped binary (1.26 MB).
- `tools/deploy.sh`:
  - Target is `root@192.168.15.244` (override with `KINDLE_HOST`).
  - Uses one multiplexed SSH connection (at most one password prompt).
  - Uses rsync if the device has it, otherwise scp. rsync on the device is unrecorded in DEVICE_FACTS, so it's detected rather than assumed.

### T13 device run — PASSED
- Deployed; "Sumiyomi → Start Sumiyomi" appeared in KUAL and launched the test card. Box taps worked, long-press exited,
  and the home screen came back normally.
- Not reported: deploy timing (<10 s target), the log tail, and the optional SIGKILL-fallback test (rc=137 path).

## T14 — crash tests + M1 wrap-up (2026-09-14)

### Crash tests (`tools/crash-tests.sh`, run by the user)
- **SIGTERM: PASS.** rc=143, "caught signal 15", screen clear, power restored 264 ms later; awesome running, statusbar running,
  preventScreenSaver 0.
- **abort(): PASS.** rc=134, "caught signal 6", power restored 249 ms later; same state checks pass.
- The black flash / return to home on screen wasn't reported for these runs (the script checks state, not pixels).
- Deploy took 3 s (T13's < 10 s target confirmed).
- SIGSEGV was not run on device. It uses the same handler (verified on host in T02).

### Clean-checkout build
Fresh `git clone --recursive` → host build + 5/5 test suites, Kindle build, and package all succeed.
Found and fixed: a checkout outside `$HOME` mounts as an empty `/src` under Colima. `kbuild.sh` now detects this and
explains, and the README says so.

## M1 status

### Definition of done (spec §2)
| # | Criterion | Status |
|---|---|---|
| 1 | Launches from KUAL, takes over the screen | ✅ T13 |
| 2 | Test pattern: 16 gray steps, centered rect, FBInk OpenType text line | ✅ pattern · ⚠️ text: font load + `fbink_print_ot` succeeded (no warning), **not yet visually confirmed** |
| 3 | Rect inverts ≤150 ms, measured, A2 | ✅ median **125 ms** event→panel complete (max 126 ms), T12 |
| 4 | Outside tap cycles GC16/GL16/DU, latency logged | ✅ 482 / 482 / 294 ms, T12 |
| 5 | Long-press exits cleanly | ✅ T10, T12, T13 |
| 6 | Clean screen + native UI restored on normal exit, SIGTERM, crash | ✅ normal (T12/T13), SIGTERM + abort (T14). No persistent state exists yet to flush |
| 7 | Same code on desktop with SDL: quantization + latency | ✅ T11, T12 (`build/host/sumiyomi`) |
| 8 | `m1_bench` CSV of latencies per mode | ✅ T04, `docs/m1_bench.csv` |
| 9 | DEVICE_FACTS filled in and committed | ✅ (hotfix field blank, non-blocking) |

### Known-unknowns register (spec §13)
| ID | Resolution |
|---|---|
| U1 | ✅ `kindlehf`, `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard` (single-core i.MX6 SoloLite, glibc 2.20) |
| U2 | ✅ Boots at 8 bpp; standard polarity (0 = black) |
| U3 | ✅ **Superseded:** stopping the framework broke the UI until reboot. Sumiyomi coexists (pillow off, awesome SIGSTOP, statusbar stop); verified T02, T10, T12, T13, T14 |
| U4 | ✅ `lipc-set-prop` present and working |
| U5 | ✅ MT protocol B |
| U6 | ✅ Digitizer 0..1072 × 0..1448, 1:1 with panel, no swap/mirror |
| U7 | ✅ `unreliable_wait_for = 0`; wait-for-complete works |
| U8 | ✅ Measured: A2 121–148, DU 262–290, 16-gray 450–494 ms (+≤27 ms for size). Design doc §10.1 revised |
| U9 | ⏳ **Open.** REAGL row invalid (sent PARTIAL; fixed). Latency won't decide it: needs a visual ghosting comparison on real pages (M4) |
| U10 | ✅ FBInk screen clear inside the fatal handler did not deadlock for SIGTERM or SIGABRT on device |
| U11 | ✅ ~200 MB available with the framework running (coexist); budget peak ~108 MB |
| U12 | ✅ KUAL (not KPM) |
| U13 | ⏳ **Open.** GL16 and GC16 time identically on this panel; whether they're distinct waveforms needs a visual check |

### Every 🔴 VERIFY in the M1 spec
| Spec location | Answer |
|---|---|
| §3.1 koxtoolchain target | `kindlehf` (U1) |
| §3.2 `-mcpu` | `cortex-a9`, `neon-vfpv3` (U1) |
| §5.4 device already 8 bpp? | Yes, so no bpp switch (U2) |
| §5.4 grayscale polarity | Standard (U2) |
| §6.2 touch ABS maxima | 1072 / 1448 = panel (U6) |
| §6.3 MT protocol | B (U5) |
| §8 lipc + framework commands | lipc present; framework is no longer stopped (U3/U4, coexist) |
| §8 run.sh framework paths | Superseded by the coexist sequence (spec §8 rewritten in T02) |

### Carried into M2 (in priority order)
1. **M1-F1 (must fix first):** full repaints block the loop on wait-for-complete; rapid input queues up for seconds.
   Needs non-blocking refresh scheduling, coalescing of queued input and refreshes, and dropping of stale events.
2. Visual confirmation of the FBInk text line (criterion 2), at the next device check.
3. T12 question: did the user really tap outside ~29 times? If not, there's a spurious-Tap bug.
4. Single core (DEVICE_FACTS): design doc §3.1's "two workers because two cores" rationale doesn't hold here.
5. U9 (REAGL, M4) and U13 (GL16 vs GC16 visual). The SIGKILL fallback path in `run.sh` is untested on device.

## M1-F1 fix (M2 stage S1)
- `core/frame.{h,cpp}` — **FrameScheduler**: handlers draw and call `damage(rect, fastest_mode, is_bw)`; the owner calls
  `flush()` once per loop wake. Damage is merged per (mode, bw) group through DirtyTracker, decided through RefreshPolicy,
  submitted fastest mode first, and **never waited on**. Unit test: a 30-tap burst → 2 refreshes, 0 waits.
- The simulated panel is now asynchronous, like the EPDC: `refresh()` snapshots and returns; `Display::pump(now)` (a no-op on
  FBInk) applies updates in submission order once their latency elapses; `wait()` blocks until applied. Test: three
  submissions take < 100 ms, and the last lands only after the first GC16's ~477 ms.
- The M1 test card stays synchronous on purpose: it's the measurement tool for criterion 3 (event → panel complete).
  M2 screens use FrameScheduler.
- Stale-event dropping isn't needed yet: with non-blocking handlers a backlog can't build. Revisit if S6 shows otherwise.

# M2 notes

## S2–S6 decisions worth knowing
- **dp vs px:** design doc §8 gives "48×48 touch targets, 24 px glyphs". At 300 ppi 24 px is ~2 mm, so these are read as
  Material dp through `Fonts::sp` (48 dp ≈ 90 px, 24 dp ≈ 45 px). Frame heights the doc lays out for the 1072-wide
  panel (app bar 112, nav 128, rows 112, grid math) stay px.
- **Gray content isn't refreshed with DU.** §8.2 says library tab switches and scroll pages use DU, but DU drives pixels
  to pure B&W, which destroys gray cover art. The library body and grid list use GL16; text lists (Updates, History,
  Browse, More, chapter rows) use DU as designed. **To judge on device:** antialiased secondary text under DU.
- **"Content area only" vs the 60% rule:** the library body is 83% of the screen, so RefreshPolicy (§7.2) promotes tab
  swaps and page turns to full-screen GL16. That costs ≤ ~27 ms (T04) and unchanged pixels don't visibly redraw.
- **Press feedback:** a pressed node is binarized then inverted, which makes A2 valid on any content. Release repaints
  with GL16 (or DU if the node is B&W). A tap runs after its feedback is submitted, in the same loop wake.
- **Rows are transparent** so they show whatever they sit on (page or sheet); repainting goes through the nearest opaque ancestor.
- **Fixed-width / wrap-height measurement bug** found by the widget golden review and fixed (captions were clipped to one line).
- **Long-press no longer exits** (in Mihon it means selection). Exit is More → "Exit Sumiyomi", or Esc at top level in the simulator.
- The M1 test card lives on as the `m1_testcard` binary (tap-latency re-measurement); `sumiyomi` is the shell.
- Startup and slow-frame (> 30 ms) timings are logged, for the single-core A9.

## Test inventory (host, `ctest --test-dir build/host`): 12 suites
canvas, dirty, gesture, refresh_policy, loop, frame, display_sdl, text_smoke, text, ui (golden), widgets (7 goldens),
shell (8 screen goldens). Every golden was visually reviewed before it was committed.
