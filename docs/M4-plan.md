# M4 — Reader: working plan

Scope from design doc §7 + §8.4 + §12: image pipeline end to end, the reader screen, tap zones, reader menu,
prefetch, page cache, refresh cadence. **Deliverable:** open a chapter from a manga's detail page and read it.

Same approach as M3: host tests with no network (fixtures), e-ink panel model in tests (panel == framebuffer),
a commit per stage, device checks only when the result is something to look at.

## Decisions (user, 2026-09-15)

- **Reading direction is a user setting:** right-to-left or left-to-right (global default + per-manga override).
  Tap zones and page keys mirror with it.
- **Image clarity over speed:** default refresh is a **full GC16 flash on every page turn**. The "flash every N
  pages" setting exists (1 = every page, up to 10, or never) but defaults to 1.
- Carried from M3: pure black/white chrome, no swipe navigation, whole-screen loading page then one refresh,
  no REAGL.

## Device constraints

- Single-core Cortex-A9 (NEON), ~200 MB free with the framework running. Page images seen from WeebCentral:
  JPEG, 784×1145 to 1200×1600, 190–370 KB, served with a **`.png` name** → decode by magic bytes, never by extension.

## Stages

| Stage | What | Done when |
|---|---|---|
| **S1** | Image decode: libjpeg-turbo (DCT-scaled decode straight to 8-bit gray), libpng + zlib, format sniffing (JPEG/PNG; WebP reported as unsupported for now). Size limits against decompression bombs | Host tests on fixture images (real WeebCentral page, PNG gray/RGBA/palette, truncated/corrupt files); builds for the Kindle |
| S2 | Processing to a panel-ready page: area resize to fit screen/width, auto-crop of near-uniform borders, tone LUT (contrast + gamma), 16-level quantization with dither presets (Sharp / Balanced / Smooth), spread detection (aspect > 1.2 → split in reading order) | Goldens of processed pages; `page_bench` tool timing each stage on the device |
| S3 | Page cache: processed 4-bit pages on disk (`cache/pages/`, keyed by URL + processing variant), `page_cache` table, LRU cap 512 MB; RAM cache for current page ±2 | Tests: hit/miss, variant keying, eviction |
| S4 | Reader screen: chapter → page list from the source → fetch → process → show. Tap zones (prev 30% / menu 40% / next 30%, mirrored for RTL), page keys, full-screen loading page for the chapter and for a page not ready yet, end-of-chapter page (next chapter / back). Marks chapters read, records history, resumes at the last page | Shell tests on fixtures (panel model); **device check** |
| S5 | Prefetch: process N+1, N+2 and fetch 3 ahead on the worker; cancel on leaving; follow direction reversals | Tests with a counting fake transport; device: page turns don't show loading once warmed |
| S6 | Reader menu (black/white top + bottom bars, local refreshes): chapter/page, prev/next chapter, page −/+ buttons; settings sheet: reading direction, fit mode, flash every N pages, dither preset, crop borders. Per-manga direction in `mangas.viewer_flags` | Shell tests; **device check** |
| S7 | Long strips (webtoon pages taller than ~2× screen): split into screen-height slices with a small overlap, paged | Tests on a synthetic tall image |

Out of scope: WebP (added only if a source needs it), downloads (M5), covers.
