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
| **S1** ✅ | wasm3 vendored and building for host + Kindle; `.aix` unpacked; a module instantiated and its imports/exports read | Done 2026-09-16: `tools/aidoku/aix_probe` loads Aqua Manga and Asura Scans; zip + source.json reader unit-tested |
| S2 | Descriptor table, memory helpers, `std` + `env` (buffers, dates, print, sleep) | Unit tests against a fixture module |
| S3 | `net`: requests through our client, rate limits, response reading, `html()` | Tests with the recorded-fixture transport |
| S4 | `html`: the ~40 calls mapped onto lexbor | Tests comparing against the Lua html API on the same page |
| S5 | postcard decoder + mirrors of Aidoku's structs (Manga, Chapter, Page, MangaPageResult, …) | Round-trip tests against fixtures captured from a real source |
| S6 | `defaults`, source settings | Tests |
| S7 | App integration: install `.aix` from an Aidoku repository, list beside Lua sources, run through the existing Extension interface | Shell tests; a real source browses, searches, reads |
| S8 | Device: memory and speed of the interpreter on the Kindle's single core | Measured, written down here |

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
