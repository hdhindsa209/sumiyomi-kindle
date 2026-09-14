# M2 — Render engine: working plan

Scope from design doc §12: widget tree, layout, dirty rects, gamma-corrected FreeType/HarfBuzz text, the grayscale
tone scale, and core widgets (AppBar, NavBar, ListRow, Grid, Sheet, Switch, Chip).
**Deliverable:** a navigable Mihon-shaped shell with fake data, golden tests in place.

Built against the SDL simulator. Device check only once the shell is app-shaped (S6).
Each stage ends with host tests passing, a clean Kindle build, and a commit.

| Stage | What | Done when |
|---|---|---|
| **S1** | **M1-F1 fix: frame-based, non-blocking refresh.** The app marks damage while handling input; one frame step per loop wake resolves DirtyTracker + RefreshPolicy and *submits* without waiting. The simulator panel becomes asynchronous too (updates land after their simulated latency without blocking the loop), so it keeps predicting device behavior | Unit test: a 30-tap burst produces coalesced frames, and no handler blocks. The simulator stays responsive during a GC16 |
| S2 | Third-party: FreeType + HarfBuzz as pinned submodules, built static for host and Kindle through our CMake toolchain. Fonts: Inter (OFL-1.1), Material Symbols Rounded (Apache-2.0) in `assets/fonts/` with licenses | Both targets link; a smoke test shapes and rasterizes a string |
| S3 | Text: font faces at device DPI (sp→px), HarfBuzz shaping, FreeType light hinting, **gamma 1.8 on coverage**, LRU glyph cache (4 MB cap), measure/ellipsize/max-lines | Unit tests: metrics, ellipsis, line breaking, gamma LUT, cache eviction |
| S4 | Tone scale tokens (design doc §5.3), retained node tree, layout (Row/Column/Stack, padding, gap, fixed/flex sizes), dirty propagation into DirtyTracker, per-node refresh hint → RefreshPolicy | Layout unit tests; golden-image harness (offscreen canvas → PGM, tolerance compare) |
| S5 | Widgets: AppBar, NavBar, ListRow, Grid (cover cells with initials placeholder), Sheet, Switch, Chip. A2 invert-on-press everywhere (§5.4) | Golden test per widget |
| S6 | Shell: Library (tabs + grid), Updates, History, Browse, More (settings rows with switches), bottom-nav navigation, a sheet; paged scroll (§5.5); fake data | Navigable in the simulator → **device check** |

Known constraints carried from M1: single-core Cortex-A9 (keep the UI thread cheap; no real parallelism),
~200 MB available, measured waveform latencies (design doc §10.1 revised).
