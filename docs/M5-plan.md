# M5 — Completeness: working plan

Scope from design doc §12: downloads + download manager, categories, library updates, Updates/History screens,
extensions from a repo, settings. **Deliverable:** v0.1 release candidate.

Carried rules: pure black/white UI, no swipes, one loading page then one refresh, host tests on the panel model,
a commit per stage, device checks when there's something to look at. Manga first (no webtoon work).

| Stage | What | Done when |
|---|---|---|
| **S1** | Downloads: user-initiated only. Original image bytes saved per chapter in `downloads/<source>/<manga>/<chapter>/` (design doc §7.4: originals, so reader settings never need a re-download), `downloads` table state (queued / downloading / done / error), one queue on the worker, resumable after restart, cancel / delete. The reader prefers downloaded files over the network | Tests: queue order, resume, cancel, delete, reader offline from a download |
| S2 | Download UI: on the manga page long-press is taken (read toggle), so a chapter selection mode: "Select" in the app bar → tap rows → bottom bar actions (Download, Delete download, Mark read, Mark unread). Row shows ↓ queued, "Downloading 12/33", ✓ downloaded, ⚠ error. "Download next 5 unread" / "Download all unread" in the manga's app bar menu. More → Download queue: list with progress, pause / resume / clear | Shell tests (panel model); **device check** |
| S3 | Categories: create / rename / delete / reorder (More → Categories), assign from the manga page, Library tabs per category | Repo + shell tests |
| S4 | Library updates: update library (all or one category) from the Library app bar, new chapters listed in Updates with day groups, optional auto-download of new chapters for chosen categories; History with resume and remove | Shell tests; **device check** |
| S5 | Settings: More → Settings (reader defaults, library update behavior, downloads: location usage + delete read chapters, page cache size + clear), Data and storage (cache/download sizes, clear) | Shell tests |
| S6 | Extensions from a repo: fetch an index (JSON) of Lua sources, install / update / uninstall into `sources/`, version + api_level checks, load without restart | Tests with a fixture repo |

Deferred to M6 (design doc): trackers, migration, statistics, backup/restore.
