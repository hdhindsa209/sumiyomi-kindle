# Sumiyomi — Design Document

**A native manga reader for jailbroken Kindle e-ink devices**

*Working name. Version 0.1 of this document. Target devices: Kindle Paperwhite 4 (PW4), Paperwhite 5 / Signature (PW5), Kindle 10th/11th gen, Oasis 2/3. Scribe is out of scope for v1.*

---

## 1. Goals and non-goals

### 1.1 What this is

A standalone native application for jailbroken Kindles that replicates Mihon's model of manga consumption — source extensions, a persistent library, per-chapter read state, downloads, trackers — with a UI that is visually and structurally recognizable as Mihon, rendered directly to the e-ink framebuffer.

It does not depend on KOReader. It does not embed a KOReader plugin API. It runs as its own process launched from KUAL/KPM, taking over the framebuffer and input devices for the duration of its session.

### 1.2 Primary goals

| Goal | Concrete target |
|---|---|
| Feel fast | Page turn ≤ 350 ms from tap to visible content on a cached page |
| Look like Mihon | Same information architecture, same screen layout, same terminology |
| Survive on 512 MB RAM | Steady-state RSS ≤ 120 MB, hard ceiling 180 MB |
| Not destroy battery | ≤ 4 %/hour active reading with wifi idle; app fully yields on suspend |
| Mechanical extension porting | A Tachiyomi/Mihon Kotlin extension should be transcribable to a Sumiyomi source in under an hour by someone who can read both languages |

### 1.3 Explicit non-goals for v1

- No WebView. Anything requiring a full browser engine (Cloudflare interactive challenges, JS-rendered pages) is out of scope for the on-device client. See §9.4 for mitigations.
- No color. The pipeline is grayscale end to end.
- No animation. No ripples, no slide transitions, no crossfades. See §5.4 for what replaces them.
- No Amazon integration. Sumiyomi does not touch the Kindle library database, Whispersync, or the native reader.
- No backup format compatibility with Mihon's protobuf backups in v1 (see §12 roadmap).

---

## 2. Hardware and platform constraints

These constraints drive nearly every decision below, so they come first.

### 2.1 The device

| | PW4 (2018) | PW5 (2021) |
|---|---|---|
| SoC | NXP i.MX7D, 2× Cortex-A7 @ ~1 GHz | NXP i.MX7D class, 2× Cortex-A53 |
| RAM | 512 MB | 512 MB (1 GB on some SKUs) |
| Panel | 1072×1448, 6", 300 ppi | 1236×1648, 6.8", 300 ppi |
| Grays | 16 levels (4-bit) addressable via 8-bit framebuffer | same |
| Storage | 8/32 GB eMMC | 8/16/32 GB eMMC |
| GPU | None usable | None usable |
| FPU | VFP, NEON available | NEON available |

Practical readings of this:

- **NEON is available and matters.** Grayscale conversion, resizing, and dithering should be NEON-accelerated. A naive C scalar resize of a 1600×2400 manga page is measurable in hundreds of milliseconds; a NEON box filter is tens.
- **RAM is the hard wall.** The native Kindle framework (`lab126_gui`, `cvm`, the Java stack) idles at a substantial chunk of the 512 MB. If Sumiyomi runs alongside it, budget is tight. If Sumiyomi stops it (§3.4), budget is comfortable. Design assumes it stops it.
- **There is no compositor.** Whatever is written to `/dev/fb0` is what appears. There is no z-ordering, no clipping, no damage tracking except what Sumiyomi implements.

### 2.2 The e-ink panel is the real design constraint

E-ink is not a slow LCD. It is a fundamentally different display model, and pretending otherwise is the single biggest cause of the "slow and awkward" feel the existing options have.

Relevant waveform modes, exposed through FBInk:

| Mode | Latency | Quality | Ghosting | Use for |
|---|---|---|---|---|
| `A2` | ~120 ms | 1-bit, black/white only | Heavy | Immediate tap feedback, drag |
| `DU` | ~250 ms | 1-bit-ish, fast | Moderate | Scrolling text lists, menus |
| `GL16` / `REAGL` | ~450 ms | 16 gray, no flash | Light, accumulates | Page content, images |
| `GC16` | ~800 ms + flash | 16 gray, full quality | Clears | Every Nth page, screen entry |

**The core UI performance rule of this project:** never use a slower mode than the content requires, and never refresh a larger rectangle than changed. Almost all perceived slowness in e-ink apps comes from full-screen `GC16` refreshes on every interaction.

---

## 3. System architecture

### 3.1 Process model

Sumiyomi is a single multithreaded native process. No IPC, no daemons, no client/server split. This is deliberate: IPC costs RAM, adds latency, and on a device with no process isolation benefit to gain, buys nothing.

```
┌─────────────────────────────────────────────────────────┐
│                     sumiyomi (PID 1 of the app)          │
│                                                          │
│  ┌────────────┐   ┌──────────────┐   ┌───────────────┐  │
│  │ UI thread  │   │ Worker pool  │   │  Net thread   │  │
│  │            │   │  (2 threads) │   │  (libcurl     │  │
│  │ - input    │   │              │   │   multi)      │  │
│  │ - layout   │   │ - decode     │   │               │  │
│  │ - paint    │   │ - resize     │   │ - HTTP        │  │
│  │ - fbink    │   │ - dither     │   │ - rate limit  │  │
│  │            │   │ - sqlite     │   │ - cookie jar  │  │
│  └─────┬──────┘   └──────┬───────┘   └───────┬───────┘  │
│        │                 │                   │          │
│        └────────┬────────┴─────────┬─────────┘          │
│                 │                  │                    │
│          ┌──────▼──────┐    ┌──────▼──────┐             │
│          │  App state  │    │ Lua runtime │             │
│          │  (single    │    │ (source     │             │
│          │   owner,    │    │  extensions,│             │
│          │   msg-      │    │  one VM per │             │
│          │   passed)   │    │  worker)    │             │
│          └─────────────┘    └─────────────┘             │
└─────────────────────────────────────────────────────────┘
```

**Threading rules, enforced by convention and asserts:**

1. The UI thread never blocks. No SQLite queries, no network, no image decode on the UI thread. Ever.
2. All mutation of app state happens on the UI thread, via messages posted from workers. Workers produce immutable result objects; the UI thread installs them.
3. Two worker threads, not more. The device has two cores; a third worker only adds context-switch overhead and memory pressure.

### 3.2 Language and toolchain

**Core: C++17.** Compiled with `koxtoolchain` targeting `arm-kindle5-linux-gnueabi` (PW4/PW5 use the `kindlehf` hard-float target). The Kindle's glibc is old; static-link everything that isn't in the base image, or ship the libs in the extension directory with a wrapper script setting `LD_LIBRARY_PATH`.

Rust was considered and rejected for v1: `armv7-unknown-linux-gnueabihf` works, but the glibc version pinning against Kindle's sysroot is fragile, and the FFI surface to FBInk/FreeType/libcurl is the majority of the code anyway. Revisit if the project grows.

**Dependencies (all statically linked unless noted):**

| Library | Purpose | Notes |
|---|---|---|
| **FBInk** | Framebuffer output, waveform control, device detection | Non-negotiable. Also handles device-specific quirks (rotation, bpp, `einkfb` vs `mxcfb` ioctl differences) |
| **FreeType 2** | Glyph rasterization | With light autohinting; see §5.2 |
| **HarfBuzz** | Text shaping | Needed for CJK and for correct kerning at small sizes |
| **libcurl** | HTTP/1.1 + HTTP/2, cookies, TLS | Linked against a bundled modern **mbedTLS** or **BearSSL** — the Kindle's system OpenSSL is ancient and will fail TLS handshakes against modern manga sites |
| **SQLite 3** | All persistent structured data | Amalgamation build, WAL mode |
| **lexbor** | HTML parsing + CSS selector engine | This is the Jsoup replacement. Critical to §6 |
| **Lua 5.4** | Source extension runtime | Small, fast, sandboxable |
| **libjpeg-turbo** | JPEG decode | NEON-accelerated; the overwhelming majority of manga pages |
| **libpng + zlib** | PNG decode | |
| **libwebp** | WebP decode | Increasingly common on manga CDNs; do not skip this |
| **libarchive** (optional) | CBZ/ZIP local source | For local library support |

Total static binary: expect 8–14 MB. Fine.

### 3.3 Directory layout on device

```
/mnt/us/extensions/sumiyomi/
├── menu.json                 # KUAL entry
├── bin/
│   └── sumiyomi              # the binary
├── lib/                      # any non-static .so
├── assets/
│   ├── fonts/
│   │   ├── Inter-Regular.ttf
│   │   ├── Inter-Medium.ttf
│   │   ├── Inter-SemiBold.ttf
│   │   ├── NotoSansCJK-Regular.otf   # subset; see §5.2
│   │   └── MaterialSymbols.ttf       # subset to used codepoints
│   └── waveform-profiles.json
├── sources/                  # installed extensions
│   ├── mangadex/
│   │   ├── source.lua
│   │   ├── manifest.json
│   │   └── icon.png
│   └── ...
└── run.sh                    # launcher wrapper

/mnt/us/sumiyomi/             # user data, deliberately separate from the app
├── sumiyomi.db               # SQLite
├── sumiyomi.db-wal
├── covers/                   # processed cover thumbnails, 4-bit raw
├── downloads/
│   └── <source_id>/<manga_id>/<chapter_id>/001.jpg ...
├── cache/
│   └── pages/                # processed page cache, LRU, capped
├── logs/
└── config.json               # things too awkward for SQLite
```

Keeping user data out of the extension directory means updating the app is a matter of replacing one folder, and uninstalling doesn't eat someone's library.

### 3.4 Startup and shutdown sequence

This is where most homebrew Kindle apps get it wrong and end up with a corrupted screen or a device that won't sleep.

**Startup (`run.sh`):**

```sh
#!/bin/sh
# 1. Take a wakelock so powerd doesn't suspend us mid-launch
lipc-set-prop com.lab126.powerd preventScreenSaver 1

# 2. Stop the native framework. This frees ~150MB and stops it
#    from repainting over us.
/etc/init.d/framework stop        # or: stop lab126_gui

# 3. Kill the screensaver / suspend timer
lipc-set-prop com.lab126.powerd deferSuspend 1

# 4. Ensure wifi stays available but idle-capable
lipc-set-prop com.lab126.cmd wirelessEnable 1

# 5. Run
cd "$(dirname "$0")"
LD_LIBRARY_PATH=./lib ./bin/sumiyomi 2>> /mnt/us/sumiyomi/logs/stderr.log

# 6. Restore, always, even on crash
lipc-set-prop com.lab126.powerd preventScreenSaver 0
/etc/init.d/framework start
```

**Critical detail:** the binary installs `SIGTERM`/`SIGINT`/`SIGSEGV` handlers that (a) flush SQLite, (b) do a final `GC16` full-screen refresh to leave a clean panel, and (c) `_exit()`. A crashed app that leaves a half-drawn screen and a stopped framework is a device the user thinks is bricked.

**Suspend handling:** Sumiyomi listens on the lipc event channel for `com.lab126.powerd goingToScreenSaver` / `resumeFromSuspend`. On suspend: cancel in-flight downloads, checkpoint the DB, release the framebuffer mapping. On resume: remap fb, full `GC16` repaint, re-establish network lazily (do not eagerly reconnect wifi — that is the main battery killer).

---

## 4. Data model

The schema deliberately mirrors Mihon's domain model, both because it's a proven design and because it makes backup interop (§12) tractable later.

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;   -- eMMC; FULL is too slow for page-turn writes
PRAGMA foreign_keys = ON;
PRAGMA cache_size = -4000;     -- 4 MB page cache, tuned to RAM budget

CREATE TABLE sources (
    id              INTEGER PRIMARY KEY,   -- stable hash of pkg name+lang
    name            TEXT NOT NULL,
    lang            TEXT NOT NULL,
    version         TEXT NOT NULL,
    enabled         INTEGER NOT NULL DEFAULT 1,
    nsfw            INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE mangas (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id       INTEGER NOT NULL REFERENCES sources(id),
    url             TEXT NOT NULL,
    title           TEXT NOT NULL,
    artist          TEXT,
    author          TEXT,
    description     TEXT,
    genre           TEXT,                  -- newline-separated, as Mihon does
    status          INTEGER NOT NULL DEFAULT 0,  -- 0 unknown,1 ongoing,2 completed,3 licensed,4 pub-finished,5 cancelled,6 hiatus
    thumbnail_url   TEXT,
    favorite        INTEGER NOT NULL DEFAULT 0,
    last_update     INTEGER NOT NULL DEFAULT 0,
    date_added      INTEGER NOT NULL DEFAULT 0,
    viewer_flags    INTEGER NOT NULL DEFAULT 0,  -- per-manga reader overrides
    chapter_flags   INTEGER NOT NULL DEFAULT 0,  -- sort/filter, bitfield
    cover_last_mod  INTEGER NOT NULL DEFAULT 0,
    UNIQUE(source_id, url)
);
CREATE INDEX idx_mangas_favorite ON mangas(favorite) WHERE favorite = 1;

CREATE TABLE chapters (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    url             TEXT NOT NULL,
    name            TEXT NOT NULL,
    scanlator       TEXT,
    read            INTEGER NOT NULL DEFAULT 0,
    bookmark        INTEGER NOT NULL DEFAULT 0,
    last_page_read  INTEGER NOT NULL DEFAULT 0,
    pages_total     INTEGER NOT NULL DEFAULT 0,
    chapter_number  REAL NOT NULL DEFAULT -1,
    source_order    INTEGER NOT NULL DEFAULT 0,
    date_fetch      INTEGER NOT NULL DEFAULT 0,
    date_upload     INTEGER NOT NULL DEFAULT 0,
    UNIQUE(manga_id, url)
);
CREATE INDEX idx_chapters_manga ON chapters(manga_id, source_order);
CREATE INDEX idx_chapters_unread ON chapters(manga_id) WHERE read = 0;

CREATE TABLE categories (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT NOT NULL,
    sort_order      INTEGER NOT NULL DEFAULT 0,
    flags           INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE manga_categories (
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    category_id     INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE,
    PRIMARY KEY(manga_id, category_id)
);

CREATE TABLE history (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    chapter_id      INTEGER NOT NULL REFERENCES chapters(id) ON DELETE CASCADE,
    last_read       INTEGER NOT NULL,
    time_read       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_history_last_read ON history(last_read DESC);

CREATE TABLE page_cache (
    source_id       INTEGER NOT NULL,
    chapter_id      INTEGER NOT NULL,
    page_index      INTEGER NOT NULL,
    variant         INTEGER NOT NULL,   -- encodes target width + fit mode + gamma
    path            TEXT NOT NULL,
    bytes           INTEGER NOT NULL,
    last_access     INTEGER NOT NULL,
    PRIMARY KEY(chapter_id, page_index, variant)
);
CREATE INDEX idx_page_cache_lru ON page_cache(last_access);

CREATE TABLE downloads (
    chapter_id      INTEGER PRIMARY KEY REFERENCES chapters(id) ON DELETE CASCADE,
    state           INTEGER NOT NULL,   -- 0 queued,1 active,2 done,3 error
    pages_done      INTEGER NOT NULL DEFAULT 0,
    pages_total     INTEGER NOT NULL DEFAULT 0,
    error           TEXT
);

CREATE TABLE tracks (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    manga_id        INTEGER NOT NULL REFERENCES mangas(id) ON DELETE CASCADE,
    tracker_id      INTEGER NOT NULL,
    remote_id       TEXT NOT NULL,
    title           TEXT,
    last_chapter_read REAL NOT NULL DEFAULT 0,
    total_chapters  INTEGER NOT NULL DEFAULT 0,
    status          INTEGER NOT NULL DEFAULT 0,
    score           REAL NOT NULL DEFAULT 0,
    remote_url      TEXT,
    UNIQUE(manga_id, tracker_id)
);
```

**Write policy.** Page-turn progress updates (`chapters.last_page_read`) are the highest-frequency write in the app. They are coalesced: held in memory and flushed on chapter change, app background, suspend, or a 30-second timer — whichever comes first. Writing to eMMC on every page turn is both slow and bad for flash endurance.

---

## 5. Rendering engine

### 5.1 The paint model

Sumiyomi maintains an **8-bit grayscale backbuffer** in RAM matching panel dimensions (1072×1448 = 1.5 MB on PW4; 1236×1648 = 2.0 MB on PW5). All drawing goes into this buffer. FBInk is then asked to blit and refresh only the changed rectangles.

```
 App state  ──▶  Widget tree  ──▶  Layout pass  ──▶  Paint pass
                    │                  │                 │
                    │                  │                 ▼
                    │                  │          8-bit backbuffer
                    │                  │                 │
                    └──────────────────┴────▶ Dirty rect accumulator
                                                         │
                                                         ▼
                                           Rect merge + mode selection
                                                         │
                                                         ▼
                                              FBInk: blit + refresh
                                                         │
                                                         ▼
                                                     /dev/fb0
```

**Widget tree:** retained, not immediate-mode. Each screen builds a tree of nodes once; interaction mutates node properties and marks them dirty. This matters because on e-ink you want to know precisely which 40×40 pixels changed, and immediate-mode redraw-everything makes that determination expensive.

Node structure, roughly:

```cpp
struct Node {
    Rect      frame;          // absolute, post-layout
    NodeKind  kind;           // Text, Image, Rect, Icon, Row, Column, List
    uint8_t   fg, bg;         // 0..255 gray, quantized at paint time
    bool      dirty;
    Node*     first_child;
    Node*     next_sibling;
    // kind-specific payload in a union
};
```

**Dirty rect accumulator:** collects dirty node frames per frame. Merge policy:

- If total dirty area > 60 % of screen → promote to full-screen refresh.
- If two rects overlap or are within 16 px → merge into bounding box.
- Cap at 6 separate refresh calls per frame; beyond that, merge the smallest until under the cap. Each FBInk refresh ioctl has fixed overhead; six small ones beat one full-screen, but twenty small ones do not.

### 5.2 Text

FreeType + HarfBuzz, with a glyph cache keyed on `(face, size, codepoint, hinting)` storing 8-bit alpha bitmaps. Cache is an LRU capped at 4 MB — plenty for the UI's limited size/face combinations.

**Rendering settings that matter on 300 ppi e-ink:**

- **Light hinting (`FT_LOAD_TARGET_LIGHT`)**, not full hinting. At 300 ppi full hinting distorts shapes for no legibility gain.
- **Grayscale antialiasing, then quantize to 16 levels at blit time.** Do not antialias into 4 levels directly; the intermediate precision improves the final quantization.
- **No subpixel rendering.** There are no subpixels.
- **Gamma correction on the alpha channel.** E-ink's response curve is not linear; text rendered with naive alpha blending looks thin and washed out. Apply a gamma of ~1.8 to the coverage value before blending. This one change is the difference between "looks like a cheap homebrew app" and "looks like the native reader."

**Fonts.** Mihon uses the Android system font (Roboto by default). The closest freely-licensable match with good hinting behavior is **Inter**, at Regular / Medium / SemiBold. Bundle a subset of **Noto Sans CJK** for Japanese/Chinese/Korean titles — full NotoSansCJK is ~16 MB, subset to JIS Level 1+2 + common Hangul and it's ~4 MB.

**Icons:** Material Symbols Rounded, subset to the ~40 codepoints actually used, rendered as a normal font face. This gets Mihon's exact iconography essentially for free.

### 5.3 The Mihon design system, translated to grayscale

Mihon uses Material 3. Material 3's whole surface-tint system is color-based, so it needs a deliberate translation, not a naive desaturation.

**Tone scale.** Define nine tones, mapped to the panel's 16-level output:

| Token | 8-bit value | Panel level | Material 3 role | Used for |
|---|---|---|---|---|
| `SURFACE` | 255 | 15 | surface | Page background |
| `SURFACE_1` | 238 | 14 | surface-container-low | Cards, bottom sheets |
| `SURFACE_2` | 221 | 13 | surface-container | App bar when scrolled, chips |
| `SURFACE_3` | 204 | 12 | surface-container-high | Pressed states, selected rows |
| `OUTLINE_VARIANT` | 187 | 11 | outline-variant | Dividers, card borders |
| `OUTLINE` | 136 | 8 | outline | Unselected icon strokes |
| `ON_SURFACE_VARIANT` | 102 | 6 | on-surface-variant | Secondary text, inactive nav labels |
| `ON_SURFACE` | 34 | 2 | on-surface | Primary text |
| `PRIMARY` | 0 | 0 | primary | Active nav item, selection, FAB fill |

**Key substitution:** Mihon's primary color (blue/purple accent) becomes **pure black plus weight**. An active bottom-nav item is a filled icon in `PRIMARY` with a `SURFACE_3` pill behind it; an inactive one is an outlined icon in `ON_SURFACE_VARIANT`. This preserves the visual hierarchy Material 3 achieves with hue, using tone and fill instead.

**Dark mode** inverts the tone scale. It is worth shipping because e-ink dark mode substantially changes refresh ghosting characteristics — some users strongly prefer one or the other.

**Spacing and type scale — kept identical to Mihon's Material 3 values, scaled by device dpi:**

| Role | Size | Weight | Line height |
|---|---|---|---|
| Top app bar title | 22 sp | Medium | 28 |
| Manga title (detail header) | 20 sp | SemiBold | 26 |
| List item primary | 16 sp | Regular | 22 |
| List item secondary | 14 sp | Regular | 20 |
| Library grid caption | 12 sp | Medium | 16 |
| Nav bar label | 12 sp | Medium | 16 |
| Chip / badge | 11 sp | Medium | 14 |

At 300 ppi with the Kindle's `sp`≈`px`×(300/160) relationship, 16 sp ≈ 30 px. Layout in sp, convert once at startup.

### 5.4 Replacing animation

Mihon's UI is full of motion: ripples, shared-element transitions, slide-in screens, crossfading covers. None of it survives contact with e-ink. Each needs a replacement that communicates the same thing in one refresh.

| Mihon behavior | Sumiyomi replacement | Waveform |
|---|---|---|
| Touch ripple | Invert the pressed element's rect immediately on `TOUCH_DOWN`, restore on `UP`/commit | `A2`, ~120 ms |
| Screen push transition | Direct repaint of the new screen, no intermediate | `GC16` full, once |
| Bottom sheet slide-up | Sheet appears in final position in one refresh; a 1 px `OUTLINE` top border + a `SURFACE_1` fill sells the layering | `GL16` on sheet rect only |
| Cover crossfade on load | Placeholder (title initials on `SURFACE_2`) → final cover in one refresh when ready | `GL16` on that cell only |
| Snackbar slide-in | Appears in place at bottom, auto-dismisses after 3 s, dismiss triggers one refresh of its rect | `DU` |
| Pull-to-refresh spinner | A `SURFACE_2` bar across the top of the content area with text "Updating…"; no spinner | `DU` on bar only |
| Scroll | See §5.5 | `DU` |

**The invert-on-press feedback is the single most important perceived-responsiveness feature in the app.** A2 inversion of a list row within ~120 ms of finger-down makes the device feel alive even when the actual navigation takes 400 ms more. Without it, every tap feels like a gamble.

### 5.5 Scrolling

Free-flowing kinetic scroll is wrong on e-ink — it produces continuous full-area refreshes and looks like a smear. Two modes:

**Paged scroll (default).** Lists scroll in whole-viewport pages. Swipe up/down, or tap the top/bottom edge, advances by (viewport height − one row of overlap). One `DU` refresh per page. This is fast, crisp, and has no ghosting accumulation. Overlap of one item preserves the reader's place.

**Row-snap drag (optional setting).** During finger-drag, track the offset and repaint in `A2` at ~8 fps, snapping to row boundaries on release with a final `DU`. Feels more conventional, looks worse, costs more battery. Off by default.

Position indicator: a thin `OUTLINE_VARIANT` scrollbar track on the right edge with an `ON_SURFACE_VARIANT` thumb, repainted with the content.

---

## 6. Source extension system

This is the layer with the most direct lineage from Mihon, and the layer that determines whether the project is maintainable.

### 6.1 Why Lua, and why lexbor

Mihon extensions are Kotlin classes extending `HttpSource`/`ParsedHttpSource`. Their logic is: build a `Request`, parse the response with Jsoup CSS selectors, map to model objects. Very little of it is Kotlin-specific.

If Sumiyomi provides (a) a scripting language, (b) an HTTP client bound to it, and (c) **a Jsoup-shaped CSS selector API**, then porting an extension becomes near-mechanical transcription. That third item is why lexbor is a hard dependency — it gives real CSS selector matching over a real HTML5-conformant DOM, in C, fast, with a small footprint.

Lua over QuickJS because: smaller (≈250 KB vs ≈1 MB), lower memory ceiling per VM, faster startup, and trivially sandboxable. The cost is that extensions can't reuse JS from sites; that turns out to matter rarely.

### 6.2 Extension structure

```
sources/mangadex/
├── manifest.json
├── source.lua
└── icon.png          # 96×96, will be dithered to 4-bit at install
```

```json
{
  "id": "mangadex",
  "name": "MangaDex",
  "lang": "en",
  "version": "1.4.0",
  "api_level": 1,
  "base_url": "https://mangadex.org",
  "nsfw": false,
  "rate_limit": { "requests": 5, "per_seconds": 1 },
  "auth": "none",
  "capabilities": ["popular", "latest", "search", "filters"]
}
```

### 6.3 The extension API

Each extension returns a table implementing this interface. Names deliberately echo Mihon's.

```lua
local Source = {}

-- Browse: popular
-- Returns: { mangas = { {url=, title=, thumbnail_url=}, ... },
--            has_next_page = bool }
function Source.popular_manga(page) end

-- Browse: latest updates
function Source.latest_updates(page) end

-- Search. filters is a table matching get_filter_list()'s shape.
function Source.search_manga(page, query, filters) end

-- Detail page for one manga. Returns the full manga table.
function Source.manga_details(manga) end

-- Chapter list. Returns array ordered source-natural (usually newest first).
function Source.chapter_list(manga) end

-- Page URL list for one chapter.
-- Returns: { { index=1, url="https://..." }, ... }
function Source.page_list(chapter) end

-- Optional: some sources need a second request per page to get the image URL.
function Source.image_url(page) end

-- Optional: extra headers for image requests (Referer is the common case)
function Source.image_request_headers(page) end

-- Optional: declarative filter UI, rendered natively by the app
function Source.get_filter_list() end

return Source
```

**Host API available to extensions:**

```lua
-- HTTP. Synchronous from the extension's point of view; the host runs it
-- on the net thread and yields the Lua coroutine. Rate limiting, cookies,
-- retries and timeouts are enforced by the host, not the extension.
local res = http.get(url, { headers = {...} })
local res = http.post(url, { headers = {...}, body = "...", form = {...} })
-- res = { status = 200, body = "...", headers = {...}, url = "final-url" }

-- HTML parsing. Jsoup-shaped on purpose.
local doc  = html.parse(res.body)
local els  = doc:select("div.manga-item")      -- returns element list
local el   = doc:select_first("h1.title")
el:text()            -- normalized text content
el:own_text()
el:attr("href")
el:html()
el:select(".child")  -- nested select
#els                 -- length; els[i] for indexing

-- URL helpers
url.resolve(base, relative)
url.encode(s)

-- JSON
json.decode(s)
json.encode(t)

-- Misc
log.d(fmt, ...)
str.trim(s)
time.parse(format, s)   -- returns unix millis
```

### 6.4 A worked port

Mihon/Tachiyomi Kotlin, typical `ParsedHttpSource`:

```kotlin
override fun popularMangaRequest(page: Int) =
    GET("$baseUrl/browse?page=$page", headers)

override fun popularMangaSelector() = "div.series-card"

override fun popularMangaFromElement(element: Element) = SManga.create().apply {
    setUrlWithoutDomain(element.select("a.cover").attr("href"))
    title = element.select("h3.name").text()
    thumbnail_url = element.select("img").attr("abs:data-src")
}

override fun popularMangaNextPageSelector() = "a.pagination-next"
```

Sumiyomi Lua:

```lua
function Source.popular_manga(page)
    local res  = http.get(BASE .. "/browse?page=" .. page)
    local doc  = html.parse(res.body)
    local out  = {}

    for _, el in ipairs(doc:select("div.series-card")) do
        table.insert(out, {
            url           = url.resolve(BASE, el:select_first("a.cover"):attr("href")),
            title         = el:select_first("h3.name"):text(),
            thumbnail_url = url.resolve(BASE, el:select_first("img"):attr("data-src")),
        })
    end

    return {
        mangas        = out,
        has_next_page = doc:select_first("a.pagination-next") ~= nil,
    }
end
```

Same selectors, same shape, same mental model. That is the whole point of the design.

### 6.5 Sandboxing and safety

Each worker thread owns one Lua VM. Extensions run with:

- No `io`, `os.execute`, `os.remove`, `package`, `require`, `load`, `loadstring`, `dofile`. Stripped from the environment at VM creation.
- A **memory ceiling of 8 MB per VM**, enforced via a custom `lua_Alloc` that returns NULL past the cap. Exceeding it fails the call cleanly rather than OOMing the device.
- An **instruction-count hook** that aborts a call after ~50 M VM instructions. Prevents a runaway `while true` in a bad extension from hanging the app.
- A **wall-clock budget** of 20 s per extension call including network. On timeout, the coroutine is abandoned and the VM recycled.
- Network access only through the host `http` table, which enforces per-source rate limits, a global connection cap (4 concurrent), TLS verification, and a 15 s per-request timeout.

### 6.6 Extension distribution

v1: install by copying a folder to `sources/`, plus an in-app "Install from URL" that fetches a `.zip` from a repo index JSON. Repo format:

```json
{
  "repo_version": 1,
  "sources": [
    { "id": "mangadex", "name": "MangaDex", "lang": "en", "version": "1.4.0",
      "api_level": 1, "url": "https://.../mangadex-1.4.0.zip",
      "sha256": "..." }
  ]
}
```

Hash verification is mandatory. Signature verification is v2.

---

## 7. Image pipeline

The most performance-critical path in the app, and the one with no Mihon equivalent — Android hands you a decoded, scaled, color-managed bitmap. Here, every stage is yours.

### 7.1 Stages

```
 Network bytes (JPEG/PNG/WebP, typically 1600×2400, 200–600 KB)
        │
        ▼
 [1] Decode with DCT scaling
        │  libjpeg-turbo scale_num/scale_denom picks the nearest
        │  power-of-two-ish downscale DURING decode. Decoding a
        │  1600×2400 JPEG directly to ~800×1200 is roughly 3× faster
        │  and uses a quarter the memory of full decode + resize.
        │  This is the single biggest win in the pipeline.
        ▼
 [2] Grayscale conversion (NEON)
        │  Rec.709 luma: Y = 0.2126R + 0.7152G + 0.0722B
        │  Fused into the decode output loop where possible.
        ▼
 [3] Precise resize to target (NEON)
        │  Remaining fractional scale after DCT step.
        │  Box filter downscale ≥2×, Catmull-Rom otherwise.
        ▼
 [4] Auto-crop borders (optional, on by default)
        │  Scan inward from each edge; trim rows/cols whose variance
        │  is below threshold and whose mean is near-white/near-black.
        │  Recovers 5–12% of usable screen on most scans.
        ▼
 [5] Tone curve
        │  Per-user brightness/contrast, then e-ink gamma (~1.8),
        │  applied via a 256-entry LUT. One pass, cheap.
        ▼
 [6] Dither to 16 levels
        │  Default: Floyd–Steinberg, serpentine. ~15 ms at 1072×1448.
        │  Alternative: Bayer 8×8 ordered (~4 ms) for the "fast" preset.
        │  Line art dithers badly — see §7.3.
        ▼
 [7] Pack to 4-bit and cache to disk
        │  1072×1448 at 4bpp = 776 KB raw. Store raw, not PNG:
        │  decode-from-cache must be a memcpy-speed operation.
        ▼
 [8] Blit to backbuffer, FBInk GL16 refresh
```

### 7.2 Budget

Target for a cold page (not in cache), excluding network:

| Stage | PW4 budget |
|---|---|
| Decode w/ DCT scale | 90 ms |
| Grayscale + resize (NEON) | 35 ms |
| Auto-crop | 12 ms |
| Tone LUT | 6 ms |
| Dither (FS) | 15 ms |
| Pack + write cache | 20 ms |
| **Total processing** | **~180 ms** |
| Blit + GL16 refresh | 450 ms |

A **cached** page is: read 776 KB from eMMC (~25 ms) → blit → refresh. **Under 500 ms total, dominated by the panel itself.** That's the floor, and it's good.

This is why prefetch (§7.4) is not optional — it moves the 180 ms off the interaction path entirely.

### 7.3 Dithering, honestly

Floyd–Steinberg is right for screentones and gradients, which is most of manga. It is wrong for crisp line art and text-heavy panels, where it introduces visible noise around strokes.

**Adaptive approach:** compute a cheap edge-density metric during the grayscale pass (sum of absolute horizontal gradient / pixel count). If density is high (line art, text), use a lighter error-diffusion coefficient set or plain quantization with a small ordered dither; if low (tones, gradients), full Floyd–Steinberg. The metric costs ~3 ms and meaningfully improves perceived sharpness.

Expose three user presets anyway, because taste varies: **Sharp** (quantize only), **Balanced** (adaptive, default), **Smooth** (full FS).

### 7.4 Prefetch and cache

**Prefetch policy:** on entering page *N*, the worker pool processes *N+1* and *N+2*, and keeps *N−1* in RAM. Network fetches run 3 pages ahead of processing. On direction reversal (reader detects two consecutive back-taps), flip the prefetch direction.

**RAM cache:** decoded 4-bit buffers for the current page ± 2 → 5 × 776 KB ≈ 3.8 MB. Trivial.

**Disk cache:** `page_cache` table + files in `cache/pages/`. LRU eviction, default cap **512 MB**, user-configurable. Keyed on variant so a settings change (different fit mode, different dither) doesn't serve stale output.

**Downloads** are separate from cache: they store the *original* downloaded bytes in `downloads/`, not the processed output, so that changing reader settings doesn't require re-downloading. Processed output of downloaded chapters still goes through `page_cache`.

---

## 8. Screen-by-screen UI specification

Layout is described for PW5 (1236×1648). PW4 values scale proportionally. All dimensions in px unless marked sp.

### 8.1 Global chrome

**Top app bar.** Height 112. `SURFACE` background, no elevation when at scroll top; `SURFACE_2` background with a 1 px `OUTLINE_VARIANT` bottom border when content is scrolled. Left: title (22 sp Medium) or back chevron + title. Right: up to 3 icon actions, 48×48 touch targets, 24 px glyphs, 16 px gap.

**Bottom navigation bar.** Height 128, `SURFACE_2`, 1 px `OUTLINE_VARIANT` top border. Five destinations, exactly Mihon's:

```
┌──────────────────────────────────────────────────────────┐
│    ▓▓▓▓                                                  │   ← active pill
│   ┌────┐     ┌────┐     ┌────┐     ┌────┐     ┌────┐    │     SURFACE_3
│   │ ▣  │     │ ↻  │     │ ⏱  │     │ ⌕  │     │ ⋯  │    │     radius 32
│   └────┘     └────┘     └────┘     └────┘     └────┘    │
│  Library    Updates   History    Browse     More        │
└──────────────────────────────────────────────────────────┘
```

Active: filled icon in `PRIMARY`, label in `PRIMARY` Medium, `SURFACE_3` pill (radius 32, height 64, width 128) behind the icon. Inactive: outlined icon and label in `ON_SURFACE_VARIANT`. Tapping a destination: A2-invert the target immediately, then paint the new screen with one `GC16`.

### 8.2 Library

Mihon's default is a cover grid with unread badges and a category tab strip. Reproduced directly.

```
┌──────────────────────────────────────────────────────────┐
│  Library                              ⌕    ⚙    ⋮        │  112
├──────────────────────────────────────────────────────────┤
│   Reading  │  On Hold  │  Plan to Read  │  Completed     │  88  ← tabs
│  ━━━━━━━━━                                               │      (scrollable)
├──────────────────────────────────────────────────────────┤
│  ┌───────────┐  ┌───────────┐  ┌───────────┐            │
│  │        ⑫ │  │         ③ │  │           │            │  cover
│  │           │  │           │  │           │            │  2:3
│  │  cover    │  │  cover    │  │  cover    │            │
│  │           │  │           │  │           │            │
│  │▔▔▔▔▔▔▔▔▔▔▔│  │▔▔▔▔▔▔▔▔▔▔▔│  │▔▔▔▔▔▔▔▔▔▔▔│            │
│  │ Title tex │  │ Title tex │  │ Title tex │            │  caption
│  └───────────┘  └───────────┘  └───────────┘            │
│                                                          │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐            │
│  ...                                                     │
├──────────────────────────────────────────────────────────┤
│              [ bottom navigation ]                       │  128
└──────────────────────────────────────────────────────────┘
```

- **Grid:** 3 columns default (user-settable 2–5). Column width = (1236 − 2×32 padding − 2×24 gutter) / 3 = 374. Cover aspect 2:3 → 374×561.
- **Caption:** overlaid on the cover bottom in Mihon (gradient scrim + white text). On e-ink a scrim dithers badly, so instead: caption sits **below** the cover in a 56 px `SURFACE` strip, 12 sp Medium, 2 lines max with ellipsis. Cleaner, and no scrim banding.
- **Unread badge:** top-right of cover, `PRIMARY` fill, radius 12, 11 sp `SURFACE` text, 8 px padding. Downloaded badge (if enabled) sits to its left in `ON_SURFACE_VARIANT`.
- **Long-press → selection mode:** app bar swaps to "*n* selected" with actions (add to category, mark read, download, delete). Selected cells get a 4 px `PRIMARY` border and a check badge. Selection toggles are single-cell `A2` refreshes — fast.
- **Covers** are pre-processed at add time to exactly 374×561 4-bit and stored in `covers/`. Never resize a cover at scroll time.

**Refresh strategy for this screen:** entering → one `GC16`. Tab switch → one `DU` on the content area only (tabs and chrome don't change). Scroll page → one `DU` on content area. Cover finishing loading → `GL16` on that one cell.

### 8.3 Manga detail

```
┌──────────────────────────────────────────────────────────┐
│  ←                                    ⌕   ↗   ⋮          │  112
├──────────────────────────────────────────────────────────┤
│  ┌─────────┐   Chainsaw Man                              │
│  │         │   Tatsuki Fujimoto                          │  header
│  │  cover  │   Ongoing · MangaDex · 24 chapters          │  272
│  │ 200×300 │                                             │
│  └─────────┘                                             │
│                                                          │
│   ┌──────────────────┐   ┌──────────────────┐           │
│   │  ♥  In library   │   │  ⊕  Tracking     │           │  action row 96
│   └──────────────────┘   └──────────────────┘           │
│                                                          │
│  Denji is a teenage boy living with a Chainsaw Devil     │  description
│  named Pochita. Due to the debt his father left…  ▼      │  collapsed 3 lines
│                                                          │
│  ⟨ Action ⟩ ⟨ Horror ⟩ ⟨ Supernatural ⟩                  │  genre chips
│ ─────────────────────────────────────────────────────────│
│  24 chapters                            ⚙ filter  ↓ sort │  section head 80
│ ─────────────────────────────────────────────────────────│
│  ● Chapter 24 — Curse                                    │
│    2 days ago · 18 pages                            ⤓   │  row 112
│ ─────────────────────────────────────────────────────────│
│  ● Chapter 23 — Gun Devil                                │
│    5 days ago · 20 pages                            ✓   │
│ ─────────────────────────────────────────────────────────│
│    Chapter 22 — Kon                                      │  read: text at
│    1 week ago · 19 pages                            ✓   │  ON_SURFACE_VARIANT
├──────────────────────────────────────────────────────────┤
│         ▶  Resume Chapter 22                             │  sticky CTA 120
└──────────────────────────────────────────────────────────┘
```

- **Unread dot:** 16 px `PRIMARY` circle at row left. Read chapters lose the dot and drop their text to `ON_SURFACE_VARIANT`. Exactly Mihon's signal, exactly readable in grayscale.
- **Download state icon** at row right: `⤓` queued/available, progress arc while downloading, `✓` downloaded, `⚠` error.
- **Description** collapsed to 3 lines with a chevron; expanding relayouts the list below it — one `DU` on everything from the description down.
- **Sticky bottom CTA** — "Start reading" / "Resume Chapter N" — is Mihon's FAB translated to a full-width bar, which is both more thumb-reachable on a 6.8" panel and doesn't require a floating shadow (shadows dither badly).
- **Long-press a chapter** → selection mode, same pattern as library.

### 8.4 Reader

The screen that matters most. Full-bleed, no chrome, until tapped.

**Tap zones** (Mihon's "L Shaped" default, adapted — the Kindle's page-turn convention is left/right, and fighting that is a bad idea):

```
┌─────────────────────────────────────┐
│                                     │
│  ┌────────┐             ┌────────┐  │
│  │        │             │        │  │
│  │  PREV  │    MENU     │  NEXT  │  │
│  │  30%   │    40%      │  30%   │  │
│  │        │             │        │  │
│  │        │             │        │  │
│  └────────┘             └────────┘  │
│                                     │
└─────────────────────────────────────┘
```

Zones are mirrored automatically for right-to-left reading direction. Physical page-turn buttons (Oasis) map to prev/next and respect direction. Swipe left/right also turns pages; swipe up/down in webtoon mode scrolls.

**Viewers**, matching Mihon's set:

| Viewer | Behavior |
|---|---|
| Left-to-right | One page per screen, advance right |
| Right-to-left | One page per screen, advance left. Default for Japanese sources |
| Vertical | One page per screen, advance down |
| Webtoon | Continuous vertical, paged in viewport-height steps (not free scroll — see §5.5) |
| Continuous vertical | As webtoon, with configurable inter-page gap |

**Fit modes:** Fit screen (default), Fit width, Fit height, Original size, Smart fit (fit width if the page is a spread, fit screen otherwise — detected by aspect ratio > 1.2).

**Double-page spread handling:** if a page's aspect ratio exceeds 1.2, either (a) show fit-width and allow vertical panning within it, or (b) split into two half-pages ordered by reading direction. Splitting is the better default on a 6" panel; make it a setting.

**Reader menu** (on center tap) — Mihon's layout, e-ink-adapted:

```
┌─────────────────────────────────────┐
│  ←   Chapter 24 — Curse         ⋮   │  top bar, GL16 on its rect
│      Chainsaw Man                   │
├─────────────────────────────────────┤
│                                     │
│          (page stays visible)       │
│                                     │
├─────────────────────────────────────┤
│  ⏮  ━━━━━━━●━━━━━━━━━━━━━  ⏭        │  seek bar, 7 / 18
│                                     │
│   ⊞        ⤢        ☀        ⚙      │  viewer / fit / tone / settings
└─────────────────────────────────────┘
```

The menu overlays only the top 160 and bottom 200 px; the page content in between is untouched, so opening and closing the menu is two small `GL16` refreshes, not a full repaint. The page itself never re-renders.

**Seek bar dragging** uses `A2` on the bar rect at ~8 fps with a page-number bubble; on release, the target page renders with `GL16`.

**Refresh cadence in the reader.** This is the setting power users will care about most:

- Page turn: `GL16` (no flash), full screen.
- Every *N* page turns: `GC16` (full flash) to clear accumulated ghosting. Default N = 6, user-settable 0 (never) to 30.
- On entering/leaving the reader: always `GC16`.
- If the outgoing and incoming page differ by less than ~8 % of pixels (rare, but happens with near-identical pages): skip the refresh entirely.

### 8.5 Updates, History, Browse, More

**Updates** — flat reverse-chronological list grouped by date header ("Today", "Yesterday", date). Each row: 64×96 cover thumb, manga title, chapter name, download icon. Pull-to-refresh triggers a library update.

**History** — same row shape, grouped by date, showing last-read chapter and a "resume" affordance. Swipe a row left to delete; long-press for "remove all for this manga".

**Browse** — tab strip: `Sources` | `Extensions` | `Migrate`.
- *Sources*: grouped by language, each row is icon + name + "Latest"/"Popular" quick links.
- *Extensions*: installed with update badges, plus available-from-repo list.
- *Migrate*: select a source, pick entries, search a target source, confirm mapping. Same flow as Mihon.

Source browse screen itself reuses the library grid renderer with a search field pinned under the app bar and a filter sheet, so there's one grid implementation, not two.

**More** — settings entry points in Mihon's grouping: Downloads, Categories, Statistics, Data and storage, Settings, About. Settings sub-screens use standard list rows with switches (a 88×48 pill, `PRIMARY` fill when on, `OUTLINE` stroke when off) and sliders.

---

## 9. Networking

### 9.1 Client configuration

libcurl multi interface on a dedicated thread, driven by an epoll loop.

- HTTP/2 enabled with multiplexing; a single connection to a busy CDN serving 20 pages is a large win.
- Connection pool cap: 4 concurrent, 2 per host. Higher numbers do not help on this hardware and do hurt battery.
- Per-request timeouts: 10 s connect, 15 s total for API calls, 45 s total for images.
- Retry: 3 attempts, exponential backoff with jitter (1 s, 2.5 s, 6 s), only on 5xx / connection errors / timeouts. Never retry 4xx.
- Cookie jar persisted per source in `sumiyomi/cookies/<source_id>.txt`.
- User-Agent: configurable per source, defaulting to a current desktop Chrome string. Some sources 403 unknown agents.

### 9.2 TLS

**Do not use the Kindle's system OpenSSL.** It is old enough to fail against modern TLS configurations. Bundle mbedTLS (or BearSSL) statically with a current CA bundle shipped in `assets/`, refreshed on app updates.

### 9.3 Rate limiting

Per-source token bucket, configured in the extension manifest. Enforced by the host — an extension cannot opt out. This protects both the user (from bans) and the sources.

### 9.4 The Cloudflare problem

State it plainly: without a browser engine, sources behind Cloudflare's interactive challenge will not work on-device. This is the single largest functional gap versus Mihon.

Mitigations, in order of preference:

1. **Cookie import.** A settings screen where the user pastes `cf_clearance` and `__cf_bm` cookies obtained from a desktop browser, scoped to a source. Works, lasts hours-to-days, requires periodic re-entry. Ship this in v1.
2. **Companion proxy (optional, v2).** A small self-hosted service (FlareSolverr-shaped) the user runs on a PC/NAS; Sumiyomi routes flagged sources through it. Fully optional, off by default, clearly documented as requiring separate infrastructure.
3. **Prefer sources with real APIs.** MangaDex and similar have proper JSON APIs with no challenge layer. Document which bundled sources are challenge-free and default the first-run source list to those.

Be honest about this in the README rather than letting users discover it as a mysterious failure.

---

## 10. Performance engineering

### 10.1 Budgets, restated as tests

Every one of these should be an automated on-device benchmark, not a vibe:

| Interaction | Budget | Measured as |
|---|---|---|
| Cold app start → library visible | 2500 ms | `main()` entry to first refresh completion |
| Tap feedback (A2 invert) | 150 ms | Input event timestamp to ioctl return |
| Library tab switch | 400 ms | Tap to refresh completion |
| Library scroll page | 400 ms | |
| Open manga detail (cached) | 600 ms | |
| Page turn, page in RAM cache | 350 ms | |
| Page turn, page in disk cache | 500 ms | |
| Page turn, cold (no prefetch hit) | 1200 ms | Excludes network |
| Reader menu open | 350 ms | |
| Search results first paint | 2500 ms | Includes network, obviously variable |

### 10.2 Memory budget

| Component | Steady state | Peak |
|---|---|---|
| Backbuffer (PW5 8-bit) | 2.0 MB | 2.0 MB |
| Shadow buffer (for dirty-diffing) | 2.0 MB | 2.0 MB |
| Glyph cache | 3 MB | 4 MB |
| Cover cache (visible + 1 screen) | 8 MB | 12 MB |
| Page cache (current ± 2, 4-bit) | 4 MB | 6 MB |
| Image decode scratch | 0 | 24 MB (one page mid-decode) |
| SQLite page cache | 4 MB | 4 MB |
| Lua VMs (2 × 8 MB ceiling) | 3 MB | 16 MB |
| libcurl buffers | 4 MB | 8 MB |
| Code + static + heap overhead | 25 MB | 30 MB |
| **Total** | **~55 MB** | **~108 MB** |

Comfortable headroom under the 180 MB ceiling. The decode scratch is the spiky part — cap concurrent decodes at 1 to keep the peak bounded.

### 10.3 Battery

The dominant consumers on this hardware, ranked: wifi radio, CPU, panel refresh (and panel refresh is the smallest of the three).

- **Wifi is off unless needed.** Connect for: library update, browse/search, chapter download, page fetch of an undownloaded chapter. Disconnect after 45 s idle. Reading downloaded content should never wake the radio.
- **The UI thread blocks on epoll/input when idle.** No polling loops, no animation timers, no periodic repaints. An idle Sumiyomi should be indistinguishable from a sleeping device in `top`.
- **Batch network work.** Library update fetches all sources in one wifi session, then drops the radio, rather than reconnecting per source.
- **Honor the system suspend.** Do not hold `preventScreenSaver` while idle — release it after 60 s of no input and re-take it on wake.

---

## 11. Build, test, and project structure

### 11.1 Repository layout

```
sumiyomi/
├── CMakeLists.txt
├── cmake/
│   └── kindle-toolchain.cmake      # koxtoolchain wiring
├── third_party/                    # pinned submodules
│   ├── fbink/  freetype/  harfbuzz/  curl/  mbedtls/
│   ├── sqlite/  lexbor/  lua/  libjpeg-turbo/  libpng/  libwebp/
├── src/
│   ├── main.cpp
│   ├── platform/        # fb, input, lipc, power, paths
│   ├── gfx/             # backbuffer, dirty rects, blit, refresh policy
│   ├── text/            # freetype+harfbuzz, glyph cache, shaping
│   ├── ui/
│   │   ├── core/        # Node, layout, gesture, focus
│   │   ├── widgets/     # AppBar, NavBar, ListRow, Grid, Chip, Sheet, Switch
│   │   └── screens/     # Library, Detail, Reader, Updates, History, Browse, More
│   ├── data/            # sqlite wrapper, repositories, migrations
│   ├── net/             # curl multi, rate limit, cookies
│   ├── image/           # decode, resize, crop, tone, dither, cache
│   ├── source/          # lua host, extension API bindings, sandbox
│   ├── track/           # MAL / AniList / MangaUpdates clients
│   └── util/
├── tools/
│   ├── build-toolchain.sh
│   ├── package-kual.sh
│   └── bench/           # on-device benchmark harness
├── tests/
│   ├── unit/            # host-side, no device needed
│   ├── golden/          # rendering golden images
│   └── fixtures/        # saved HTML responses for extension tests
└── sources/             # first-party extensions
```

### 11.2 Development loop

The build-deploy-test loop determines how fast this project moves, so invest in it early.

1. **Host-side simulator.** Build the same UI code against an SDL2 backend on desktop, with a fake FBInk that renders to a window and *simulates e-ink*: 16-level quantization, per-mode latency, and ghosting accumulation. This lets 90 % of UI work happen without a device and catches "looks fine on my laptop" mistakes. Highest-leverage single piece of tooling in the project.
2. **QEMU** (`qemu-arm` with the Kindle sysroot) for verifying the ARM build runs at all.
3. **Device deploy** over USBNet: `rsync` the extension folder, SSH in, run. Sub-10-second cycle.

### 11.3 Testing

- **Unit tests** (host): layout math, dirty-rect merging, dither correctness, SQLite migrations, URL resolution, Lua sandbox escapes.
- **Golden-image tests:** render each screen with fixed fixture data, compare against checked-in PNGs with a small tolerance. Catches layout regressions.
- **Extension tests:** each source ships fixture HTML; a host-side runner executes `popular_manga`/`search_manga`/`chapter_list`/`page_list` against fixtures and asserts on parsed output. No network in CI.
- **On-device benchmarks:** the table in §10.1, run as a single command, output as CSV. Run before every release; regressions are bugs.
- **Soak test:** automated script that turns 500 pages, checks RSS growth and refresh timing drift. Catches leaks and ghosting policy bugs.

---

## 12. Roadmap

**M1 — Skeleton (≈3 weeks).** Toolchain, KUAL packaging, framebuffer up, FBInk refresh modes verified on device, touch input, clean startup/shutdown, SDL simulator. Deliverable: a black rectangle you can tap to invert, on real hardware, that exits cleanly.

**M2 — Render engine (≈4 weeks).** Widget tree, layout, dirty rects, text with gamma-corrected FreeType/HarfBuzz, the tone scale, core widgets (AppBar, NavBar, ListRow, Grid, Sheet, Switch, Chip). Deliverable: a navigable Mihon-shaped shell with fake data. Golden tests in place.

**M3 — Data + one source (≈3 weeks).** SQLite schema and migrations, repositories, libcurl stack, Lua host, lexbor bindings, the MangaDex extension (API-based, no Cloudflare). Deliverable: browse, search, view detail, add to library — real data.

**M4 — Reader (≈4 weeks).** Image pipeline end to end, all viewers, fit modes, tap zones, reader menu, prefetch, page cache, refresh cadence. Deliverable: the app is usable for its actual purpose.

**M5 — Completeness (≈4 weeks).** Downloads and download manager, categories, library update scheduling, Updates/History screens, extension install-from-repo, settings. Deliverable: v0.1 release candidate.

**M6 — Polish (≈3 weeks).** Trackers (MAL, AniList), migration, statistics, Cloudflare cookie import, backup/restore in a native JSON format, performance pass against §10.1.

**Post-v1 candidates:**
- Mihon `.tachibk` backup import (protobuf; the schema is stable and public — genuinely useful for onboarding).
- Multi-source chapter merging.
- Local source (CBZ/CBR/folder) via libarchive.
- Companion Cloudflare proxy.
- Kobo port — the rendering layer is already abstracted behind FBInk, which supports Kobo natively, so this is mostly input and packaging work.

Total to v1: roughly **5 months of focused solo work**, realistically 8–10 calendar months part-time. Be suspicious of any estimate shorter than that; the reader and image pipeline in particular always take longer than they look.

---

## 13. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Cloudflare locks out most popular sources | High | High | Cookie import in v1; lead with API-based sources; document clearly |
| Firmware update breaks jailbreak / kills the app | Medium | High | Document airplane-mode + OTA blocking; nothing else in our control |
| Amazon changes framebuffer/ioctl behavior | Low | High | FBInk absorbs most of this; pin to FBInk releases and follow upstream |
| Page turns feel slower than native Kindle reader | Medium | High | Prefetch is mandatory; benchmark every release; the GL16 floor (~450 ms) is physics — set expectations |
| Extension ecosystem never materializes | Medium | Medium | Ship 6–8 good first-party sources; make the porting guide excellent; the Jsoup-shaped API is the whole bet here |
| Memory pressure causes OOM kills | Low | High | Hard budgets in §10.2; stop the native framework; cap concurrent decodes at 1 |
| Ghosting accumulates to unreadable | Low | Medium | Configurable GC16 cadence; sane default of 6 |
| Scope creep into "reimplement all of Mihon" | **High** | **High** | The milestone list is the scope. Trackers and migration are M6 for a reason |

The last one is the real risk. Mihon is ten years of accumulated work by many people. The version of this project that ships is the one that does library, browse, read, and download extremely well on e-ink, and treats everything else as optional.

---

## 14. Licensing note

Mihon is Apache-2.0. If any Mihon code is directly translated rather than merely referenced for behavior — extension logic, schema, algorithms — Apache-2.0 attribution obligations apply: retain the copyright notice, include the license, and state changes made. Licensing Sumiyomi itself under Apache-2.0 is the simplest path and keeps the door open to sharing extension logic in both directions.

Independently reimplementing a UI's layout and interaction design from observation is not a derivative work in the copyright sense, but the courteous and low-friction move is to credit Mihon prominently and be explicit that this is an independent project, not an official port.
