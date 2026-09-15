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
