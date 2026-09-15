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

## Status (2026-09-15)

S1–S2 done and device-verified, plus manga-page sort/filter, per-manga queue, select all, front light, Library covers.
S3 built on host: More → Categories (create / rename with the on-screen keyboard, move up / down, delete — manga stay
in the library), manga page "Categories" checklist (also offered right after "Add to library"), Library tabs
(All + categories; at most 4 visible with arrows stepping to the neighbouring tab; the chosen tab is remembered).
S3 device-verified. S4 built on host: library check from the Library (current tab's category) or Updates, one entry
per worker job with an in-place "Checking N of M" loading page and Cancel, result shown on Updates; Updates rows tap →
reader, hold → manga; auto-download (off / all / chosen categories via `categories.flags`) for entries that already
had chapters; History shows page progress, tap resumes, hold → open manga / remove, clear all with a confirm.
S4 device-verified. User requests (2026-09-15), built on host: (1) no "Loading page" on turns — a chapter loads
completely behind one counting loading page (3-connection FetchPool + worker processing), cached pages read on a
separate page thread; (2) battery level in app bars and the reader menu (powerd lipc, sysfs fallback — to confirm on
device); (3) Library selection: hold a manga (list or cover) → select more / select all → Categories, Download
unread, Mark read/unread, Remove (optionally deleting downloads).

Deferred to M6 (design doc): trackers, migration, statistics, backup/restore.
