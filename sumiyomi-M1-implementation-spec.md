# Sumiyomi M1 — Implementation Specification

**Milestone 1: Skeleton.** Companion to `sumiyomi-design-doc.md`. Where the two disagree, this document wins — it was written against the real FBInk API rather than from memory.

---

## 0. How to use this document

**For the human (you):** Section 1 is blocking. Do it first, on the device, before any code is written. It takes 20 minutes and it determines whether several assumptions below are true. Paste the results into `docs/DEVICE_FACTS.md` in the repo.

**For the coding agent:** Work through §11 in task order. Each task lists its files, its acceptance criteria, and its dependencies. Do not skip ahead — later tasks assume earlier ones landed.

**Markers used throughout:**

- 🔴 **VERIFY** — a claim that must be confirmed on the actual device before code depends on it. Do not implement around an unverified 🔴.
- 🟡 **ASSUMPTION** — believed true, low risk if wrong, cheap to change.
- ✅ **CONFIRMED** — read directly from FBInk's public header (`fbink.h`, master branch). Trustworthy.

**Global rule for the agent:** if a 🔴 item has no recorded answer in `docs/DEVICE_FACTS.md`, stop and ask rather than guessing. A wrong guess here produces code that compiles, runs, and silently corrupts the display.

---

## 1. Phase 0 — Device facts (BLOCKING, human, ~20 min)

Jailbreak the device and get a shell (USBNet + SSH, or the KUAL "Terminal" extension). Then run each command and record the output verbatim.

Create `docs/DEVICE_FACTS.md` from this template:

```markdown
# Device facts
Recorded: <date>
Recorded by: <you>

## Identity
Model (Settings → Device Info):
Generation / codename:
Firmware version:
Jailbreak method used (WinterBreak / AdBreak / other):
Hotfix installed (y/n):

## `cat /proc/version`
<paste>

## `uname -a`
<paste>

## `cat /proc/cpuinfo`
<paste — note core count and whether 'neon' appears in Features>

## `free -m`  (BEFORE stopping the framework)
<paste>

## `free -m`  (AFTER stopping the framework)
<paste>

## `fbink -v` and `fbink -e`   [fbink -e dumps full state]
<paste>

## `ls -l /dev/fb*`
<paste>

## `cat /sys/class/graphics/fb0/virtual_size`
## `cat /sys/class/graphics/fb0/bits_per_pixel`
## `cat /sys/class/graphics/fb0/rotate`
<paste>

## `ls -l /dev/input/`
<paste>

## For each /dev/input/eventN:  `cat /sys/class/input/eventN/device/name`
<paste — label which is the touchscreen>

## Framework stop test
Command used:
Did the screen stop being repainted by the native UI? (y/n)
Did `free -m` show the expected memory freed? (y/n)
Command used to restore:
Did the device return to normal? (y/n)

## lipc test
`lipc-get-prop com.lab126.powerd status` output:
`lipc-set-prop com.lab126.powerd preventScreenSaver 1` — did it error? (y/n)
Is `lipc-set-prop` present at all? (y/n)

## Storage
`df -h /mnt/us`
<paste>
```

### 1.1 Why each of these matters

| Fact | What it decides |
|---|---|
| NEON in cpuinfo | Whether §7 of the design doc's SIMD plan is viable at all |
| `free -m` before/after framework stop | The entire memory budget. If stopping the framework frees <100 MB, budgets need rework |
| `fbink -e` state dump | Native bpp, rotation, `is_kindle_legacy`, touch axis quirks — feeds directly into T03/T05 |
| Input device names | Which `/dev/input/eventN` is the touchscreen, and whether page-turn buttons exist |
| lipc availability | Whether the power/wakelock layer (T07) is `lipc-set-prop` shell-outs or needs another route |
| Framework stop behavior | Whether the app can own the screen at all, or must coexist |

### 1.2 The one dangerous step

Stopping the framework is the only step here that can leave the device looking bricked. Before running it, know your restore command and be ready to hold the power button for 30 s to force a reboot if the screen goes dead. Do this while the device is charged and while you have time to recover. Nothing in this project is worth doing on a device you can't afford to reset.

---

## 2. M1 definition of done

M1 ships when **all** of the following are true on real hardware:

1. `./bin/sumiyomi` launches from a KUAL menu entry and takes over the screen.
2. It draws a test pattern: 16 gray steps, a centered filled rectangle, and a text line rendered with FBInk's OpenType path (not our own text stack — that's M2).
3. Tapping the rectangle inverts it within **150 ms** (measured, not eyeballed), using `WFM_A2`.
4. Tapping outside it cycles the whole screen through `WFM_GC16` / `WFM_GL16` / `WFM_DU`, printing measured latency for each to the log.
5. A long-press (>800 ms) anywhere exits cleanly.
6. On exit — normal, `SIGTERM`, or crash — the screen is left clean (one `GC16` full refresh), the DB-less state is flushed, and the native UI is restored (pillow, window manager, status bar). **The device is usable without a reboot.**
7. The same code, compiled for the host with the SDL backend, runs on desktop and shows the same test pattern with simulated e-ink quantization and latency.
8. `tools/bench/m1_bench` outputs a CSV of measured refresh latencies per waveform mode.
9. `docs/DEVICE_FACTS.md` is filled in and committed.

Explicitly **not** in M1: widget tree, layout engine, our own text rendering, SQLite, network, Lua, any Mihon-shaped screen. Those are M2+.

---

## 3. Toolchain

### 3.1 koxtoolchain

```bash
git clone https://github.com/koreader/koxtoolchain.git
cd koxtoolchain
./gen-tc.sh kindlehf      # hard-float; for PW3 and earlier use `kindle` (soft-float)
```

🔴 **VERIFY:** which target applies to your device. `kindlehf` is correct for PW4/PW5-era hardware; `kindle5` / `kindle` for older. Check the koxtoolchain README against your generation before running — regenerating takes ~40 minutes.

Output lands in `~/x-tools/arm-kindlehf-linux-gnueabihf/`. Add `bin/` to `PATH`.

### 3.2 CMake toolchain file

`cmake/kindle-toolchain.cmake`:

```cmake
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ENV{KINDLE_TC_ROOT})
  message(FATAL_ERROR "Set KINDLE_TC_ROOT to your x-tools target dir")
endif()
set(TC_ROOT   $ENV{KINDLE_TC_ROOT})
set(TC_PREFIX "${TC_ROOT}/bin/arm-kindlehf-linux-gnueabihf-")

set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++")
set(CMAKE_AR           "${TC_PREFIX}ar")
set(CMAKE_RANLIB       "${TC_PREFIX}ranlib")
set(CMAKE_STRIP        "${TC_PREFIX}strip")

set(CMAKE_SYSROOT "${TC_ROOT}/arm-kindlehf-linux-gnueabihf/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -mcpu: 🔴 VERIFY against /proc/cpuinfo. cortex-a53 for PW5-class,
# cortex-a7 for PW4-class. Wrong value = SIGILL at runtime.
set(SUMI_ARCH_FLAGS "-mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT   "${SUMI_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SUMI_ARCH_FLAGS}")

# Static-link libstdc++/libgcc: the Kindle's runtime is too old to rely on.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
```

### 3.3 FBInk build

FBInk is a submodule, built with its own Makefile (it does not use CMake). For M1, build the **full** feature set — we need `FBINK_FEATURE_OPENTYPE` for the placeholder text and `FBINK_FEATURE_INPUT` for device classification:

```bash
cd third_party/fbink
make -j4 CROSS_TC=arm-kindlehf-linux-gnueabihf KINDLE=1 static
```

🟡 **ASSUMPTION:** `KINDLE=1 static` is the correct invocation. Check FBInk's README for the current flag set; NiLuJe changes build targets occasionally. The artifact wanted is `libfbink.a` plus `fbink.h`.

CMake consumes it as an imported static library:

```cmake
add_library(fbink STATIC IMPORTED GLOBAL)
set_target_properties(fbink PROPERTIES
  IMPORTED_LOCATION            "${CMAKE_SOURCE_DIR}/third_party/fbink/libfbink.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/fbink")
```

---

## 4. Repository scaffold for M1

Only what M1 needs. Do not create empty directories for later milestones.

```
sumiyomi/
├── CMakeLists.txt
├── README.md
├── .gitmodules
├── cmake/
│   └── kindle-toolchain.cmake
├── docs/
│   ├── DEVICE_FACTS.md            # filled in Phase 0
│   └── M1-notes.md                # running log of surprises
├── third_party/
│   └── fbink/                     # submodule, pinned tag
├── src/
│   ├── main.cpp
│   ├── platform/
│   │   ├── display.h              # backend-agnostic interface
│   │   ├── display_fbink.cpp      # device implementation
│   │   ├── display_sdl.cpp        # simulator implementation
│   │   ├── input.h
│   │   ├── input_evdev.cpp
│   │   ├── input_sdl.cpp
│   │   ├── power.h
│   │   ├── power_kindle.cpp
│   │   ├── power_stub.cpp
│   │   ├── paths.h
│   │   └── paths.cpp
│   ├── core/
│   │   ├── canvas.h / canvas.cpp        # 8-bit backbuffer + draw primitives
│   │   ├── dirty.h  / dirty.cpp         # dirty rect accumulator + merge
│   │   ├── refresh_policy.h / .cpp      # rect → waveform mode decision
│   │   ├── gesture.h / gesture.cpp      # raw touch → tap/longpress/swipe
│   │   ├── loop.h   / loop.cpp          # epoll event loop
│   │   └── log.h    / log.cpp
│   └── app/
│       └── m1_testcard.cpp        # the M1 demo screen
├── tools/
│   ├── package-kual.sh
│   ├── deploy.sh
│   └── bench/
│       └── m1_bench.cpp
└── tests/
    └── unit/
        ├── test_dirty.cpp
        └── test_gesture.cpp
```

---

## 5. Platform layer — display

### 5.1 Facts established from `fbink.h` ✅

These are read directly from the header and can be relied on:

- `int fbink_open(void)` → fd; `int fbink_close(int fbfd)`.
- `int fbink_init(int fbfd, const FBInkConfig*)` — **must** be called before any draw/refresh call.
- `void fbink_get_state(const FBInkConfig*, FBInkState*)` — gives `screen_width`, `screen_height`, `scanline_stride`, `bpp`, `inverted_grayscale`, `screen_dpi`, `device_name`, `device_id`, `is_kindle_legacy`, `unreliable_wait_for`, `can_hw_invert`, `touch_swap_axes`, `touch_mirror_x`, `touch_mirror_y`, `current_rota`, `rotation_map[4]`, `pixel_format`.
- `unsigned char* fbink_get_fb_pointer(int fbfd, size_t* buffer_size)` — direct mmap'd framebuffer pointer. **This is the primary drawing path for Sumiyomi.** We write pixels ourselves and ask FBInk only to refresh.
- `int fbink_set_fb_info(int fbfd, uint32_t rota, uint8_t bpp, uint8_t grayscale, const FBInkConfig*)` — supported bpp values are 4, 8, 16, 32. Use `KEEP_CURRENT_ROTATE` (1<<7) to leave rotation alone. For 8bpp set `grayscale` to `GRAYSCALE_8BIT` (1). Returns the same change-flags as `fbink_reinit`, so **state must be re-fetched after a successful call**.
- `int fbink_refresh(int fbfd, uint32_t top, uint32_t left, uint32_t width, uint32_t height, const FBInkConfig*)` — note the order is **top, left, width, height**, inherited from `mxcfb_rect`. Easy to get backwards.
- `int fbink_refresh_rect(int fbfd, const FBInkRect*, const FBInkConfig*)` — same thing, taking `FBInkRect { left, top, width, height }`. Note `FBInkRect` orders **left before top**, the opposite of `fbink_refresh`. This inconsistency is a documented footgun; prefer `fbink_refresh_rect` everywhere and never hand-roll the broken-out call.
- `uint32_t fbink_get_last_marker(void)`, `int fbink_wait_for_complete(int fbfd, uint32_t marker)`, `int fbink_wait_for_submission(int fbfd, uint32_t marker)`. `LAST_MARKER` is `0U`. `fbink_wait_for_submission` is noted as implemented on Kindle K5+.
- `FBInkConfig` fields that matter to us: `wfm_mode`, `is_flashing`, `no_refresh`, `is_nightmode`, `dithering_mode`, `is_inverted`, `is_quiet`, `to_syslog`.
- `int fbink_invert_screen(int fbfd, const FBInkConfig*)` — full-screen HW inversion **with** refresh.
- `FBINK_TARGET_KINDLE` / `FBINK_TARGET_KINDLE_LEGACY` from `fbink_target()`.

### 5.2 Waveform mode latencies ✅

From FBInk's own annotations — these supersede the estimates in the design document:

| Mode | Documented latency | Notes from header |
|---|---|---|
| `WFM_A2` | ~120 ms | B&W → B&W only, some ghosting |
| `WFM_DU` | ~260 ms | any → B&W, light ghosting |
| `WFM_GC4` | ~290 ms | any → B/W/GRAYA/GRAY5 |
| `WFM_GL16` | ~450 ms | white → any, some ghosting |
| `WFM_GC16` | ~450 ms | any → any, highest fidelity |
| `WFM_REAGL` | ~450 ms | ghosting *and* flashing reduction |
| `WFM_INIT` | ~2000 ms | may flash several times |

**Correction to the design doc:** GC16 is ~450 ms, not ~800 ms. The flash is a visual event, not primarily a latency cost. This makes the "full refresh every N pages" policy cheaper than budgeted — good news.

`WFM_GC16HQ` is documented as an i.MX-only alias for REAGL (or REAGLD when flashing). Since Kindles are i.MX, **REAGL is available and is likely the best mode for page content.** T04 must measure REAGL vs GL16 vs GC16 on the actual panel and record the result; the design doc's default should be revisited based on what comes back.

### 5.3 The display interface

`src/platform/display.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace sumi {

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;

    bool     empty()      const { return w <= 0 || h <= 0; }
    int32_t  area()       const { return empty() ? 0 : w * h; }
    int32_t  right()      const { return x + w; }
    int32_t  bottom()     const { return y + h; }
    bool     intersects(const Rect& o) const;
    Rect     united(const Rect& o) const;
    Rect     clipped(const Rect& o) const;
};

// Mirrors FBInk's WFM_* subset we actually use. Kept as our own enum so the
// SDL backend and the unit tests don't need to include fbink.h.
enum class Wave : uint8_t {
    A2,       // ~120ms, B&W only. Tap feedback, drag.
    DU,       // ~260ms, any->B&W. Lists, menus, scroll.
    GL16,     // ~450ms, white->any. Page content.
    REAGL,    // ~450ms, reduced ghosting+flashing. Candidate page-content default.
    GC16,     // ~450ms, full fidelity. Screen entry, periodic clear.
    GC16_FLASH, // GC16 with is_flashing -> full black flash. Ghost clear.
};

struct DisplayInfo {
    int32_t     width       = 0;
    int32_t     height      = 0;
    int32_t     stride      = 0;   // bytes per scanline, padding included
    int32_t     bpp         = 0;
    int32_t     dpi         = 0;
    bool        inverted_gray = false; // FBInkState::inverted_grayscale
    bool        legacy_einkfb = false; // FBInkState::is_kindle_legacy
    bool        unreliable_wait = false;
    bool        touch_swap_axes = false;
    bool        touch_mirror_x  = false;
    bool        touch_mirror_y  = false;
    std::string device_name;
};

class Display {
public:
    virtual ~Display() = default;

    // Opens fb, inits FBInk, forces 8bpp grayscale if needed, fetches state.
    // Returns false and fills `err` on failure. Must be idempotent-safe.
    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;

    virtual const DisplayInfo& info() const = 0;

    // Pointer to the live framebuffer. Writes here are visible only after
    // refresh(). Never null between open() and close().
    virtual uint8_t* framebuffer() = 0;

    // Request an e-ink refresh of `r` using `mode`.
    // `r` is clipped to screen bounds internally; empty rect = no-op.
    // Returns the update marker, or 0 if the backend has no markers.
    virtual uint32_t refresh(const Rect& r, Wave mode) = 0;

    // Block until `marker` (or the last update if 0) has completed.
    // No-op on backends without markers, or when info().unreliable_wait.
    virtual void wait(uint32_t marker = 0) = 0;

    // One GC16 flashing full-screen refresh. Called on entry and on exit.
    virtual void clear_screen() = 0;
};

Display* make_display();   // returns the backend compiled in

} // namespace sumi
```

### 5.4 `display_fbink.cpp` — implementation requirements

1. `fbink_open()` → keep the fd for the process lifetime (the header notes this keeps one mmap alive, which is what we want).
2. Zero-initialize `FBInkConfig` (the header explicitly states zero-init is sane). Set `is_quiet = true`, `to_syslog = false`.
3. `fbink_init(fbfd, &cfg)`; abort on negative return.
4. Fetch state. If `bpp != 8`, call `fbink_set_fb_info(fbfd, KEEP_CURRENT_ROTATE, 8, 1 /*GRAYSCALE_8BIT*/, &cfg)`. **Re-fetch state afterwards** — the header warns the call reinits and may change layout. Record the original bpp so it can be restored on exit.
   - 🔴 **VERIFY:** whether the device comes up at 8bpp already. `fbink -e` output from Phase 0 answers this. If it's already 8bpp, skip the call entirely — fewer mode switches, fewer chances to strand the fb in a bad state.
5. `fbink_get_fb_pointer(fbfd, &size)`; store pointer and size. **Assert `size >= stride * height`** before any write.
6. `refresh()` builds an `FBInkRect`, sets `cfg.wfm_mode` from the `Wave` mapping, sets `cfg.is_flashing` only for `GC16_FLASH`, sets `cfg.no_refresh = false`, calls `fbink_refresh_rect`, then returns `fbink_get_last_marker()`.
7. `wait()` skips entirely when `info().unreliable_wait` is set (the header flags that `MXCFB_WAIT_FOR_UPDATE_COMPLETE` may time out on some devices). Otherwise `fbink_wait_for_complete`.
8. `close()` restores the original bpp if it was changed, does a final `GC16` flashing clear, then `fbink_close`.

**Grayscale polarity trap.** `FBInkState::inverted_grayscale` indicates the fb is `GRAYSCALE_8BIT_INVERTED`, meaning 0 is white and 255 is black — the reverse of the tone scale in the design doc. The `Canvas` layer must normalize this **once**, at open, by storing a polarity flag and inverting on final blit rather than scattering `if (inverted)` through drawing code. 🔴 **VERIFY** which polarity your device reports.

---

## 6. Platform layer — input

### 6.1 FBInk does the device classification ✅

This is the single biggest simplification available, and it removes most of the guesswork:

```c
FBInkInputDevice* fbink_input_scan(INPUT_DEVICE_TYPE_T match_types,
                                   INPUT_DEVICE_TYPE_T exclude_types,
                                   INPUT_SETTINGS_TYPE_T settings,
                                   size_t* dev_count);
// struct FBInkInputDevice { INPUT_DEVICE_TYPE_T type; int fd; bool matched;
//                           char name[256]; char path[4096]; };
```

Relevant type bits: `INPUT_TOUCHSCREEN`, `INPUT_POWER_BUTTON`, `INPUT_PAGINATION_BUTTONS`, `INPUT_HOME_BUTTON`, `INPUT_MENU_BUTTON`, `INPUT_SLEEP_COVER`, `INPUT_ROTATION_EVENT`. Settings bits: `SCAN_ONLY`, `OPEN_BLOCKING`, `MATCH_ALL`, `EXCLUDE_ALL`, `NO_RECAP`.

So: scan for `INPUT_TOUCHSCREEN | INPUT_PAGINATION_BUTTONS | INPUT_POWER_BUTTON`, default settings (non-blocking `O_RDONLY|O_NONBLOCK|O_CLOEXEC` fds, which is exactly right for epoll). **Free the returned array** — the header says it's heap-allocated and caller-owned. Note the array contains *all* devices; filter on `matched`.

This removes the need to hand-roll `EVIOCGBIT` probing. It does not remove the need to record device names in Phase 0 — cross-check FBInk's classification against what you saw, and log a warning if they disagree.

### 6.2 Coordinate transform ✅

`FBInkState` carries `touch_swap_axes`, `touch_mirror_x`, `touch_mirror_y`. The header specifies the order explicitly: **swap axes first, then mirror.**

```cpp
// ts_max_x / ts_max_y come from ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo)
Point transform(int32_t rx, int32_t ry, const DisplayInfo& d,
                int32_t ts_max_x, int32_t ts_max_y)
{
    int32_t x = rx, y = ry;
    int32_t mx = ts_max_x, my = ts_max_y;

    if (d.touch_swap_axes) { std::swap(x, y); std::swap(mx, my); }
    if (d.touch_mirror_x)  { x = mx - x; }
    if (d.touch_mirror_y)  { y = my - y; }

    // Scale digitizer range to panel range. Often 1:1 but do NOT assume it.
    return { (x * d.width) / (mx + 1), (y * d.height) / (my + 1) };
}
```

🔴 **VERIFY:** the touch device's `ABS_MT_POSITION_X/Y` maxima via `evtest` or `EVIOCGABS`. If they equal the panel dimensions the scaling is a no-op, but hardcoding that assumption is how you get a UI that works on your device and nobody else's.

### 6.3 Multitouch protocol

🔴 **VERIFY:** whether the touchscreen uses MT protocol **A** (`ABS_MT_*` + `SYN_MT_REPORT`) or **B** (`ABS_MT_SLOT` + `ABS_MT_TRACKING_ID`). Run `evtest` on the touch device and tap once; protocol B emits `ABS_MT_SLOT`, protocol A does not.

M1 only needs single-touch, so implement the simplest thing that works for whichever protocol the device reports, and structure it so adding the other is a separate code path rather than a rewrite. Multi-finger (pinch-zoom in the reader) is M4's problem.

### 6.4 The input interface

`src/platform/input.h`:

```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace sumi {

struct Point { int32_t x = 0, y = 0; };

enum class RawKind : uint8_t { Down, Move, Up, Cancel, Key };
enum class Key : uint8_t { None, PagePrev, PageNext, Power, Home, Menu, Back };

struct RawEvent {
    RawKind  kind = RawKind::Down;
    Point    pos;
    Key      key       = Key::None;
    bool     pressed   = false;   // for Key events
    uint64_t t_ms      = 0;       // CLOCK_MONOTONIC millis
};

class Input {
public:
    virtual ~Input() = default;

    // Scans, classifies, opens. Populates fds() for the event loop.
    virtual bool open(const DisplayInfo& d, std::string& err) = 0;
    virtual void close() = 0;

    // Non-blocking fds for epoll registration.
    virtual const std::vector<int>& fds() const = 0;

    // Drain one ready fd, appending decoded events. Call when epoll says ready.
    virtual void drain(int fd, std::vector<RawEvent>& out) = 0;
};

Input* make_input();

} // namespace sumi
```

### 6.5 Gesture recognition (M1 scope)

`src/core/gesture.h` — a pure state machine, no I/O, fully unit-testable:

```cpp
enum class GestureKind : uint8_t { Tap, LongPress, SwipeL, SwipeR, SwipeU, SwipeD };

struct Gesture { GestureKind kind; Point at; uint64_t t_ms; };

class GestureRecognizer {
public:
    // Thresholds. Tuned for 300ppi; adjust after real-device testing.
    static constexpr int32_t  kTapSlopPx      = 24;   // movement still counted as a tap
    static constexpr int32_t  kSwipeMinPx     = 90;
    static constexpr uint64_t kLongPressMs    = 800;
    static constexpr uint64_t kSwipeMaxMs     = 600;

    // Returns the gesture if one completed on this event.
    std::optional<Gesture> feed(const RawEvent& e);

    // Must be called each loop tick so long-press can fire without further
    // input events. Returns a LongPress if the timer elapsed.
    std::optional<Gesture> tick(uint64_t now_ms);
};
```

**Down-feedback is separate from gesture completion.** The app paints the A2 invert on `RawKind::Down`, *before* the recognizer decides what the gesture was. This is what buys the 150 ms feedback target. Do not wait for `Tap` to fire.

---

## 7. Core — canvas, dirty rects, refresh policy

### 7.1 Canvas

`src/core/canvas.h`. Owns nothing; wraps the framebuffer pointer.

```cpp
class Canvas {
public:
    Canvas(uint8_t* fb, int32_t w, int32_t h, int32_t stride, bool inverted);

    // All values are "logical" gray: 0 = black, 255 = white, ALWAYS.
    // Polarity inversion is applied here if the panel needs it.
    void fill_rect(const Rect& r, uint8_t gray);
    void stroke_rect(const Rect& r, uint8_t gray, int32_t thickness);
    void invert_rect(const Rect& r);          // for A2 tap feedback
    void blit_gray8(const Rect& dst, const uint8_t* src, int32_t src_stride);

    // Quantize a rect's contents to N levels. Used before A2 refreshes,
    // which require true B&W content to avoid artifacts.
    void quantize_rect(const Rect& r, int levels);

    int32_t width()  const;
    int32_t height() const;
};
```

**A2 correctness note.** FBInk documents A2 as "from B&W to B&W". Issuing an A2 refresh over a region containing intermediate grays produces artifacts. So `invert_rect` followed by an A2 refresh is only safe if the region was already 1-bit. For M1's test rectangle this is true by construction. For M2 onwards, the refresh policy must downgrade A2 → DU whenever the region isn't provably B&W. Encode this as a rule now, in `refresh_policy`, not as a comment.

### 7.2 Dirty rect accumulator

`src/core/dirty.h`:

```cpp
class DirtyTracker {
public:
    static constexpr int   kMaxRects      = 6;
    static constexpr int32_t kMergeGapPx  = 16;
    static constexpr float kFullThreshold = 0.60f;  // of screen area

    void add(const Rect& r);
    void clear();

    // Returns the merged set to actually refresh. If the union exceeds
    // kFullThreshold of the screen, returns a single full-screen rect.
    // If more than kMaxRects remain after gap-merging, repeatedly merges
    // the two whose union adds the least area until the cap is met.
    std::vector<Rect> resolve(int32_t screen_w, int32_t screen_h) const;
};
```

Fully unit-testable with no device. `tests/unit/test_dirty.cpp` must cover: disjoint rects stay separate; rects within the gap merge; exceeding the cap merges least-cost pairs; exceeding the area threshold promotes to full-screen; empty rects are ignored; rects are clipped to screen.

### 7.3 Refresh policy

`src/core/refresh_policy.h` — the one place that decides waveform mode. Centralizing this is what makes the ghosting/latency tradeoff tunable later instead of scattered.

```cpp
struct RefreshRequest {
    Rect    rect;
    Wave    preferred;
    bool    region_is_bw = false;  // caller asserts content is 1-bit
};

class RefreshPolicy {
public:
    struct Decision { Rect rect; Wave mode; bool flash; };

    // Applies: A2 downgrade when !region_is_bw; periodic GC16 flash after
    // kFlashEvery non-flashing full refreshes; full-screen promotion.
    Decision decide(const RefreshRequest& req, int32_t sw, int32_t sh);

    void     set_flash_interval(int n);   // 0 = never
private:
    int flash_interval_ = 6;
    int since_flash_    = 0;
};
```

---

## 8. Lifecycle and crash safety

This is the part that determines whether a bug costs you a debugging session or a device reset. Implement it in T02, before anything draws.

**Strategy (revised in T02): coexist with the native framework, don't stop it.** On the target device (FW 5.17.1.0.3), stopping and restarting `framework`/`lab126_gui` left the native UI unrecoverable without a reboot (see `docs/M1-notes.md`). Sumiyomi instead follows KOReader's default Upstart path: leave the framework running and silence what would draw over us.

**Memory:** with the framework running, ~200 MB is available on the target device (vs ~369 MB with it stopped). That is still comfortable against the design doc's ~108 MB peak budget (§10.2).

| acquire (in order) | restore (reverse order) |
|---|---|
| `lipc-set-prop com.lab126.powerd preventScreenSaver 1` | `start statusbar` (only if we stopped it) |
| `lipc-set-prop com.lab126.pillow disableEnablePillow disable` | `SIGCONT` awesome |
| `SIGSTOP` awesome (window manager) | `disableEnablePillow enable`, then `lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home` |
| `stop statusbar`, if `/etc/upstart/statusbar.conf` exists | `preventScreenSaver 0` |

`src/platform/power.h`:

```cpp
class PowerGuard {
public:
    // Takes the wakelock and silences the native UI so it can't draw over us.
    // Records what it changed so restore() can undo exactly that.
    bool acquire(std::string& err);

    // Idempotent. Safe to call from a signal handler context
    // (uses only async-signal-safe operations: fork/exec, kill, write, _exit).
    void restore() noexcept;
};
```

✅ `lipc-set-prop` is present at `/usr/bin/lipc-set-prop` (DEVICE_FACTS U4).

**Signal handling contract:**

- Install handlers for `SIGTERM`, `SIGINT`, `SIGHUP`, `SIGSEGV`, `SIGBUS`, `SIGABRT`, `SIGILL`, `SIGFPE`.
- Fatal handlers must be **async-signal-safe**: no `malloc`, no `printf`, no C++ destructors. Write a fixed string to the log fd with `write(2)`, issue one direct `fbink_cls`-equivalent full clear, call `PowerGuard::restore()`, then `_exit(128 + sig)`.
- 🟡 **ASSUMPTION:** calling into FBInk from a fatal handler is acceptable in practice because the alternative (a stranded framebuffer) is worse. If it deadlocks, fall back to `restore()` only and accept a dirty screen. Record which happens in `docs/M1-notes.md`.
- The `run.sh` wrapper must also restore unconditionally after the binary exits, as a belt-and-braces layer. Use a `trap`:

```sh
#!/bin/sh
set -u
HERE="$(dirname "$0")"
DATA=/mnt/us/sumiyomi
mkdir -p "$DATA/logs"

# The binary acquires via PowerGuard; this only undoes it if the binary died
# without restoring (e.g. SIGKILL / OOM kill). Every step is harmless if already restored.
restore() {
    [ -f /etc/upstart/statusbar.conf ] && start statusbar          2>/dev/null
    killall -CONT awesome                                           2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable      2>/dev/null
    lipc-set-prop com.lab126.powerd preventScreenSaver 0            2>/dev/null
    true
}
trap restore EXIT INT TERM

LD_LIBRARY_PATH="$HERE/lib" "$HERE/bin/sumiyomi" \
    >>"$DATA/logs/stdout.log" 2>>"$DATA/logs/stderr.log"
```

🟡 **ASSUMPTION (T13):** whether `run.sh`'s fallback should also relaunch home via `appmgrd`. Relaunching home when the binary already restored may be visible; decide when packaging.

---

## 9. Event loop

`src/core/loop.h`. Single-threaded for M1 (worker pool arrives in M3).

```cpp
class EventLoop {
public:
    using TickFn  = std::function<void(uint64_t now_ms)>;
    using InputFn = std::function<void(const RawEvent&)>;

    void add_fd(int fd, std::function<void(int)> on_ready);
    void set_tick(TickFn fn, uint32_t interval_ms);  // timerfd-backed
    void run();     // blocks until stop()
    void stop();
};
```

Requirements:

- `epoll_wait` with **no timeout** (-1). The tick is a `timerfd` registered as a normal fd. This means a fully idle app makes zero syscalls — the battery requirement from the design doc depends on it.
- The tick fires at 100 ms only while a gesture is in progress (to drive long-press). When idle, `timerfd_settime` disarms it. Do not run a free-running timer.
- A `eventfd` is registered for `stop()`, so the loop can be woken from a signal handler by a single `write(2)` of 8 bytes — async-signal-safe.
- Monotonic time via `clock_gettime(CLOCK_MONOTONIC, …)`, never `gettimeofday`.

---

## 10. SDL simulator backend

Built with the host compiler, same source tree, selected by `-DSUMI_BACKEND=sdl`. This is the highest-leverage tool in the project and is part of M1, not a nice-to-have.

`display_sdl.cpp` must simulate, not merely display:

1. **Quantization.** Before presenting, quantize the 8-bit buffer to 16 levels. Otherwise smooth gradients look fine on desktop and band badly on device.
2. **Latency.** `refresh()` sleeps for the documented duration of the requested mode (A2 120 ms, DU 260 ms, GL16/REAGL/GC16 450 ms) before presenting. This makes bad refresh policy *feel* bad on desktop, which is the whole point.
3. **Ghosting.** Maintain a shadow buffer of what's physically "on the panel". Non-flashing modes blend toward the new content with a residual (e.g. 8 % of the previous frame retained per non-flashing refresh, accumulating); flashing modes reset it. Crude, but it surfaces "we never flash and it's now unreadable" bugs on desktop.
4. **A2 restriction.** If an A2 refresh is issued over a region with more than 2 distinct values, draw the region with visible artifacts *and* log a warning. Make the bug loud.
5. Window scaled to fit the host screen; render at true device resolution internally.

`input_sdl.cpp` maps mouse down/move/up to `RawEvent`, and keyboard keys to `Key::PagePrev`/`PageNext`/`Back`.

---

## 11. Task breakdown

Each task is sized for one focused working session. Dependencies are strict.

| # | Task | Depends | Files | Acceptance |
|---|---|---|---|---|
| **T00** | Phase 0 device facts | — | `docs/DEVICE_FACTS.md` | Every field filled, committed. **Human-only, blocking.** |
| **T01** | Repo + toolchain + hello-world cross-compile | T00 | `CMakeLists.txt`, `cmake/`, `.gitmodules`, `src/main.cpp` | `cmake --preset kindle && cmake --build` produces an ARM binary; `file` confirms ARM EABI5; it runs on device and prints to stdout |
| **T02** | Logging + PowerGuard + signal handling | T01 | `core/log.*`, `platform/power*.{h,cpp}` | Binary acquires wakelock and silences the native UI (coexist, §8), holds, restores on exit. Native UI must not draw over the hold, and the device must be usable afterward (hands-on check, not just exit codes). `kill -TERM` / `abort()` tests deferred to a later pass, once the coexist strategy itself is validated |
| **T03** | Display backend (FBInk) | T02 | `platform/display.h`, `display_fbink.cpp` | Opens fb, logs full `DisplayInfo`, forces 8bpp if needed, fills the screen mid-gray, one GC16 refresh, exits clean. Logged dimensions match Phase 0 |
| **T04** | Waveform benchmark | T03 | `tools/bench/m1_bench.cpp` | CSV to stdout: mode, rect size, measured ms (median of 20), for A2/DU/GL16/REAGL/GC16/GC16-flash at full-screen, half, and 200×200. **Commit the results into `docs/M1-notes.md`** — they set the real budgets |
| **T05** | Input backend (evdev via `fbink_input_scan`) | T03 | `platform/input.h`, `input_evdev.cpp` | Logs classified devices; logs transformed tap coordinates. Tapping each screen corner yields coordinates within 20 px of the expected corner. **This is the transform correctness test** |
| **T06** | Canvas | T03 | `core/canvas.*` | `fill_rect`, `stroke_rect`, `invert_rect`, `quantize_rect` correct at edges and under both polarities. Host unit tests over a fake buffer |
| **T07** | DirtyTracker + unit tests | — (host only) | `core/dirty.*`, `tests/unit/test_dirty.cpp` | All cases in §7.2 pass on host. No device needed |
| **T08** | GestureRecognizer + unit tests | — (host only) | `core/gesture.*`, `tests/unit/test_gesture.cpp` | Tap, long-press, four swipes, slop rejection, and swipe-timeout all covered by synthetic event sequences |
| **T09** | RefreshPolicy | T06, T07 | `core/refresh_policy.*` | A2→DU downgrade when `!region_is_bw`; flash counter fires at interval; full-screen promotion above threshold. Host unit tests |
| **T10** | EventLoop | T05 | `core/loop.*` | epoll over input fds + eventfd + timerfd. Verified idle: process `voluntary_ctxt_switches` + `nonvoluntary_ctxt_switches` in `/proc/<pid>/status` do not increase during 10 s of no input (substitute for `strace -c`, which is not installed on the device — see DEVICE_FACTS) |
| **T11** | SDL backend | T06, T08 | `display_sdl.cpp`, `input_sdl.cpp` | Same test card renders on desktop with quantization, simulated latency, ghosting accumulation, and a loud A2 misuse warning |
| **T12** | M1 test card app | T09, T10 | `app/m1_testcard.cpp` | All of §2's criteria 2–5. Tap-feedback latency logged per tap; median over 20 taps **must be ≤150 ms** |
| **T13** | KUAL packaging + deploy script | T12 | `tools/package-kual.sh`, `tools/deploy.sh` | `./tools/deploy.sh` rsyncs to device in <10 s; app appears and launches from the KUAL menu |
| **T14** | M1 wrap-up | T13 | `docs/M1-notes.md`, `README.md` | Measured numbers recorded; every 🔴 in this doc resolved to a confirmed answer; build+deploy documented from a clean checkout |

### 11.1 KUAL `menu.json` (T13)

🟡 **ASSUMPTION** on exact schema — confirm against the KindleModding wiki for your launcher version:

```json
{
  "items": [
    {
      "name": "Sumiyomi",
      "priority": 1,
      "action": "./run.sh",
      "status": "file /mnt/us/extensions/sumiyomi/bin/sumiyomi"
    }
  ]
}
```

Newer jailbreaks use KPM rather than KUAL. If `docs/DEVICE_FACTS.md` shows KPM, package for that instead and note the change.

---

## 12. Coding conventions

- **C++17.** No exceptions across the platform boundary — platform functions return `bool` + `std::string& err`. Exceptions are permitted in higher layers from M2 onward, but the platform layer must be usable from a context where unwinding is unsafe.
- **No RTTI, no dynamic_cast.** Compile with `-fno-rtti`. Exceptions stay on (`-fexceptions`) for the app layer.
- **No allocation in the input or refresh path.** `drain()` appends to a caller-owned vector that is reused across ticks. `DirtyTracker::resolve` returns into a reused buffer in the hot path (the test-facing version returning a vector is fine).
- **Naming:** `snake_case` for functions and variables, `PascalCase` for types, `kConstantCase` for constants, `trailing_` for private members.
- **Warnings:** `-Wall -Wextra -Wshadow -Wconversion -Werror`. `-Wconversion` is deliberately included — this codebase does a lot of integer width juggling between `int32_t`, `uint16_t` (FBInkRect), and `uint32_t` (fbink_refresh), and silent truncation there produces display corruption that is very hard to diagnose.
- **Logging:** one line per event, `LEVEL ts_ms subsystem message`. Levels E/W/I/D. Debug compiled out in release. Never log inside a fatal signal handler except via raw `write(2)`.
- **Every FBInk call's return value is checked.** They return `-(EXIT_FAILURE)` or a negative errno on failure. Ignoring them is how you get a silently blank screen.

---

## 13. Known unknowns register

Carry this forward; resolve each into `docs/M1-notes.md` as it's settled.

| ID | Unknown | Resolved by | Risk if wrong |
|---|---|---|---|
| U1 | Correct koxtoolchain target + `-mcpu` for your device | T00, T01 | SIGILL at startup |
| U2 | Native bpp and grayscale polarity | T00, T03 | Inverted or garbled display |
| U3 | ~~Framework stop/start command~~ → superseded: coexist strategy (§8) must leave native UI fully usable | T00, T02 | Device appears bricked; needs reboot (observed with stop/start) |
| U4 | `lipc-set-prop` availability | T00, T02 | Screensaver interrupts the app |
| U5 | MT protocol A vs B | T00, T05 | No touch input at all |
| U6 | Touch digitizer range vs panel range | T05 | Taps land in the wrong place |
| U7 | Whether `unreliable_wait_for` is set | T03 | Hangs waiting on markers |
| U8 | Real waveform latencies on this panel | T04 | Every budget in the design doc is wrong |
| U9 | Whether REAGL beats GL16 for page content here | T04 | Suboptimal but not broken |
| U10 | FBInk fatal-handler safety | T02 | Dirty screen on crash (acceptable fallback exists) |
| U11 | Memory available with framework running (coexist) — measured ~200 MB | T00 | Memory budget in design doc §10.2 needs rework (it doesn't: ~108 MB peak) |
| U12 | KUAL vs KPM packaging for your jailbreak | T00, T13 | App won't launch from the menu |

**U8 is the important one.** The design document's entire performance model is built on assumed refresh latencies. T04 replaces assumptions with measurements, and if the numbers come back materially different, §10.1 of the design doc should be revised before M2 begins rather than after.

---

## 14. What M1 deliberately proves

M1 looks like very little output for a lot of setup. What it actually buys:

- That the toolchain produces binaries this device will run (U1).
- That the app can own and release the screen without stranding the device (U3, U10) — the failure mode most likely to make you abandon the project.
- That touch coordinates are correct (U5, U6) — silently wrong here and every UI screen built afterward is wrong.
- That the **150 ms tap-feedback target is achievable** — the single perception-level claim the whole "faster than Rakuyomi" premise rests on. If T12's median comes back at 400 ms, that's worth knowing in week three, not month five.
- That the simulator faithfully predicts device behavior — which is what makes M2's large body of UI work possible without constant device round-trips.

If T12 shows tap feedback can't get under ~200 ms on this hardware, stop and reconsider the interaction model before building the UI on top of it.
