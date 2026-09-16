# M6 — Aidoku sources: working plan

Sumiyomi's own sources are Lua files (§6.3) and stay exactly as they are. This milestone adds a **second kind of
source**: [Aidoku](https://aidoku.app)'s, which are WebAssembly modules maintained by a community of hundreds.
The point is upkeep: when a site changes, someone else fixes it, and the fix arrives through their repository.

## What the evidence says (2026-09-16)

Sampling eight English sources from the Aidoku community repository (136 sources, 55 English):

| Module | Sources needing it | Maps onto |
|---|---|---|
| `net` | 8/8 | our HTTP client, rate limiter, cookie jar |
| `html` | 7/8 | lexbor + our CSS selector wrapper |
| `std` | 8/8 | buffers, dates, memory |
| `env` | 8/8 | logging, sleep |
| `defaults` | 4/8 | the preferences table, per source |
| `js` | 1/8 | a JavaScript engine — **out of scope**; those sources won't install |
| `canvas` | 1/8 | image compositing — **out of scope** for now |

Every sampled package is pure WASM (no JavaScript files inside), 13–31 imports each. A host that covers
std/env/net/html/defaults runs seven of the eight.

## Shape

- `.aix` is a zip: `Payload/<id>/main.wasm`, `source.json`, icons. Reading it needs a small zip reader over the
  zlib we already ship.
- Exports the host calls: `start`, `get_manga_list`, `get_search_manga_list`, `get_manga_update`, `get_page_list`,
  plus optional ones (`get_filters`, `get_settings`, `get_image_request`, …).
- Results come back as a pointer into the module's memory: `len` (i32), `capacity` (i32), then a
  [postcard](https://postcard.jamesmunns.com)-serialized struct; negative values are errors. The host frees them
  with `free_result`.
- Handles (`Rid`) refer to host-side objects (documents, elements, requests): the host owns a descriptor table.

## Stages

| Stage | What | Done when |
|---|---|---|
| **S1** ✅ | wasm3 vendored and building for host + Kindle; `.aix` unpacked; a module instantiated and its imports/exports read | Done 2026-09-16: `tools/aidoku/aix_runner --probe` loads Aqua Manga and Asura Scans; zip + source.json reader unit-tested |
| S2 ✅ | Descriptor table, memory helpers, `std` + `env` (buffers, dates, print, sleep) | Done: real sources read their arguments |
| S3 ✅ | `net`: requests through our client, rate limits, response reading, `html()` | Done: sources fetch live pages |
| S4 ✅ | `html`: the calls real sources use, mapped onto lexbor | Done: 21 of 25 sampled sources load |
| S5 ✅ | postcard decoder + mirrors of Aidoku's structs | Done: listings, details, chapters and pages decode from real sources |
| S6 ✅ | `defaults` (settings per source) | Done (settings UI is part of S7) |
| S7 ✅ | App integration: install `.aix` from an Aidoku repository, list beside Lua sources, run through one source interface | Done: `SourceRunner` is implemented by both kinds; installing a real package is tested end to end |
| S8 | Device: memory and speed of the interpreter on the Kindle's single core | Sources run on the device (2026-09-16); speed and memory not measured yet |

## Risks, plainly

- **Postcard and struct layout** must match Aidoku's exactly; a mismatch is silent nonsense rather than an error.
  Mitigation: fixtures captured from real sources, checked field by field.
- **Interpreter speed** on a 1 GHz Cortex-A9 is unknown. Sources do string and DOM work, most of which is host-side,
  so the risk is moderate. Measured in S8 before this is promised to anyone.
- **Sources needing `js` or `canvas`** can't run. They're listed as unsupported rather than failing oddly.
- Aidoku's SDK changes (0.7 → 0.9 are in the index). The host targets the current one and reports a clear
  "needs a newer Sumiyomi" for anything beyond it.

## S1 notes (2026-09-16)

Two real sources load in wasm3 (Aqua Manga 223 KB, Asura Scans 215 KB of WebAssembly). Their exports match the
SDK's macro: `start`, `get_manga_list`, `get_search_manga_list`, `get_manga_update`, `get_page_list`,
`get_image_request`, `free_result`, plus optional handlers. Imports seen across the two:

- `std`: destroy, buffer_len, read_buffer, print, abort, parse_date, current_date
- `env`: print, send_partial_result
- `net`: init, send, send_all, set_url, set_header, set_body, set_rate_limit, data_len, read_data, html
- `html`: parse_fragment, select, select_first, size, get, attr, text, own_text, html, base_uri, set_text
- `defaults`: get, set

That is the whole surface to implement for these two, and it lines up with what the app already has.

## S2–S6 notes (2026-09-16)

`tools/aidoku/aix_runner` drives a package end to end (`--probe` just unpacks and reports). Across the 25 English sources sampled from the community
repository, 21 load and the rest say why (2 need a JavaScript engine, 1 image editing, 1 `html.kind`). Working
end to end today, against their live sites: Guya (6 manga, 39 chapters, 20 pages), MangaBat (24/25/100),
Drake Scans (24/119/7), EzManga (20/13/10), Danke fürs Lesen (20/1/2), Chikari, Athrea, Hive, Magus, Manga District.
Some sites answer nothing (403 or moved) — their own doing, not the host's.

Two things the SDK does that had to be matched exactly, and would have been invisible otherwise:

- `read_buffer` must return **0** for success; returning the byte count makes every argument look unreadable.
- A search query is handed over as **plain text**, not a postcard-encoded `Option<String>`; a handle of −1 means
  "no query", which is how a source's default listing is asked for.

Still to do (S7): install `.aix` packages from an Aidoku repository, keep them beside the Lua sources, give each
its settings, and run them through the app's own Extension interface. Then S8 on the device.

## S7 notes (2026-09-16)

Both kinds of source now answer one interface (`source::SourceRunner`), so the library, reader, downloads and
library checks don't know or care which they're talking to. Aidoku packages install as
`<data>/sources/<id>.aix`, are checked by being loaded before they're kept, and are loaded again at startup.
Each gets its own settings store (`aidoku.<id>.<key>` in the preferences table). Aidoku's community repository is
offered alongside Sumiyomi's own from the first run, and the Extensions tab marks which sources are Aidoku ones.

Their index carries no checksums, so an Aidoku package is trusted the way Aidoku itself trusts it: the repository
is the authority. Sumiyomi's own repository still checks SHA-256 for every file.

## The bug that only showed on the device (2026-09-16)

Aidoku sources installed and loaded on the Kindle but every call failed with `malformed Wasm binary`, then
`source aborted`. wasm3 keeps a *pointer* to a module's bytes and compiles each function the first time it is
called; the module was parsed from the caller's copy of the package, which was freed as soon as installing
finished. Calls then compiled from freed memory. Sources now parse from their own copy, and
`test_aidoku_keeps_its_module` wipes the caller's copy after loading to keep it that way.

It never failed on a development machine because the tools keep the package alive for the whole run — a reminder
that "works here" says nothing about a different allocator. The Kindle build of `aix_runner` is statically linked
so it can be run under `qemu-arm` locally, which is how this was finally cornered.

## S8: what a source call costs on the device (2026-09-16)

wasm3 interprets on the Kindle (no JIT on this kernel), so an Aidoku source does its parsing and its HTML
walking an opcode at a time. `AidokuSource::call` now logs any call over 500 ms as
`perf aidoku <id>.<fn>: <total>ms (<network>ms network, <interpreting>ms interpreting)`, which separates a slow
site from a slow interpreter. Numbers from the hardware still have to be collected — run a listing and a chapter
on the device and read `/mnt/us/sumiyomi/logs/`.

## Sleeping (2026-09-16)

`PowerGuard::acquire` holds powerd's `preventScreenSaver` for the whole session so the framework can't paint
over a page, which also stops powerd sleeping the device at all: with the lock held, neither the idle timer nor
the power button does anything. `main` therefore handles `Key::Power` itself — `app/sleep_screen.cpp` paints
the sleep screen straight to the framebuffer (never through `ui::Screen`, so the node tree and the reader's
state survive untouched), then `PowerGuard::sleep` suspends and returns when the device wakes.

Two details worth keeping:

- **Knowing the user is back.** The first attempt used the `CLOCK_BOOTTIME` / `CLOCK_MONOTONIC` gap, which
  grows by the length of each suspend on kernels that account for it. On this Kindle it never grew, even though
  the device plainly slept — so the app sat in its poll loop for the full timeout after the user had already
  woken it, which is what "takes quite a while to wake" was. Input is the reliable signal: our process is frozen
  with the device, so the press that wakes it is the first thing we see on resuming. The clock check stays as a
  second signal, since where it works we notice without the user touching anything.
- **Some firmwares won't suspend while the wakelock is held.** If nothing happens within four seconds, the lock
  is dropped (letting powerd sleep the device the way it normally would) and taken back when the user returns.
