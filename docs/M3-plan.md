# M3 — Data + one source: working plan

Scope from design doc §12: SQLite schema + migrations, repositories, libcurl stack, Lua host, lexbor bindings, and the
MangaDex extension (API-based, no Cloudflare). **Deliverable:** browse, search, view detail, add to library, with real data.

Same approach as M2: simulator first, host tests with **no network in CI** (fixtures, design doc §11.3), a commit per
stage, one device check when real data shows up in the shell.

| Stage | What | Done when |
|---|---|---|
| **S1** | SQLite (vendored amalgamation), schema from §4 with `user_version` migrations, WAL, repositories: sources, manga, chapters (merge by URL, keeping read state), categories, history | Unit tests on in-memory and file databases |
| S2 | Network: mbedTLS + libcurl static for host and Kindle, bundled CA certificates, an HTTP client (timeouts, retries with backoff on 5xx/connect errors only, per-source token-bucket rate limit, cookie jar) | Retry/rate-limit logic tested with a fake transport; a live smoke tool (manual) |
| S3 | Background worker thread and result posting to the UI thread through the event loop (eventfd / pipe). Single core, so one worker for network + Lua | Tests: jobs run off the UI thread, results are delivered in order, cancellation |
| S4 | Lua host + sandbox (§6.5: no io/os/load/require, 8 MB allocator cap, instruction limit, wall clock), host API (§6.3): `http`, `json`, `url`, `str`, `log`, `time`, and `html` via lexbor (`select`, `select_first`, `text`, `attr`) | Tests: sandbox escapes blocked, memory cap, runaway loop killed, API behavior against fake HTTP |
| S5 | MangaDex source (`sources/mangadex/{manifest.json,source.lua}`): popular, latest, search, details, chapters, pages. An extension test runner against saved fixture responses | Runner passes on fixtures |
| S6 | Shell on real data: Browse → source → paged grid, search (**on-screen keyboard widget**; the Kindle has none for us), detail from the source, "Add to library" persisted, Library read from SQLite, "Update library" fetching new chapters | Works in the simulator against the live API → **device check** (needs Wi-Fi) |

Out of scope (M4): cover/page image decoding. Covers stay as initials placeholders in M3.

**Open device questions (to record in DEVICE_FACTS at the S6 check):** Wi-Fi/DNS availability while Sumiyomi runs, and
whether the Kindle's clock is right (TLS certificate validation depends on it).

## As built (device-verified 2026-09-14)

M3 is done: browse, search, detail and a persistent library work on the device against live data.
What changed from the plan, and why:

- **Source: WeebCentral, not MangaDex.** MangaDex was too slow on the user's devices. WeebCentral is HTML
  scraping (lexbor selectors in `sources/weebcentral/source.lua`, 1 request / 2 s). Its page images are served
  with a `.png` name but are JPEG, so the M4 decoder sniffs magic bytes.
- **Text-only lists.** No covers or placeholders anywhere (user preference; covers are not planned for M4 either).
- **E-ink UI rules** (see `src/ui/screen.h`, `src/ui/tone.h`, `src/app/shell.h`):
  - Pure black/white palette; emphasis by weight, 2 px rules, 6 px bars and inversion. No grays.
  - No swipe navigation. Lists page by whole pages with a bottom pager bar (`[<] Page N of M [>]`).
  - Every whole-screen repaint carries a reason: NewScreen = GC16 flash; Loading = GL16; PageTurn = GL16 with a
    flash every 5th; Update = GL16. Local changes refresh only their node.
  - Screens that need data show one full-screen loading page, then the finished screen with a single flash
    (skipped when data arrives within 300 ms).
  - REAGL looked worse than GL16/GC16 on this panel. Not used.
- **Bugs the simulator hid:** async results never painted (no frame without input). Tests now use FakeDisplay's
  panel model (a golden requires panel == framebuffer) plus a shell test on the real EventLoop + Worker.

Device answers: Wi-Fi and DNS work while Sumiyomi runs (DNS failed only while USBNet was misrouting), and TLS
validation passes, so the clock is fine.
