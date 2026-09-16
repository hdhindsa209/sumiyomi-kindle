# Changelog

## Unreleased

- **Fixed: waking from sleep took far longer than it should.** The app decided it had woken by watching the
  clock, which on this firmware never reports suspended time — so after the device came back it went on waiting
  instead of repainting. It now wakes on the first key or touch, which is what the user actually does.
- Only the buttons wake the device from sleep now; a touch doesn't.
- Mangapill reports its covers (needs Mangapill 1.1.0 from the source repository).

## v0.3.0

- **Fixed: the device couldn't be put to sleep while Sumiyomi was open.** The app holds powerd's screensaver
  wakelock for as long as it runs, so the framework can't paint over a page mid-read — which also meant powerd
  never slept the device, and the power button did nothing. Sumiyomi now handles the power button itself: it
  shows a sleep screen, suspends the device, and on waking repaints the screen you were on, with the reader
  still on its page.
- **WebP pages and covers.** Sources that serve WebP (Mangapill's covers, among others) now display instead of
  failing to decode. WebP is rescaled as it decodes, the way JPEG already was, so a page costs no more memory
  than before.
- Slow Aidoku source calls are logged with the network time separated from the interpreter time.
- The Browse screen drops its empty "Migrate" tab; migrating a library between sources isn't built yet.
- "Nothing else to install" no longer appears when a repository couldn't be read at all.
- Tests clean up the throwaway sources they write, and those files are no longer in the repository.

## v0.2.2

- **Fixed: Aidoku sources failed with "malformed Wasm binary" on the device.** The interpreter keeps a pointer to
  a module's bytes and compiles each function the first time it's called; the source was parsed from the caller's
  copy of the package, which was freed as soon as installing finished. Sources now keep their own copy.
  It only showed up on the device because the freed memory happened to survive long enough elsewhere.
- `aix_runner` (a diagnosis tool) ships with the app and finds the CA bundle on the device.

## v0.2.1

- **Fixed:** upgrading from an older version didn't add Aidoku's repository, because the repository list was only
  set up on a first run. The repositories the app ships with are now offered once each, including after an
  upgrade; one you remove stays removed.
- The Sources tab points at the Extensions tab, since it only lists sources that are already installed.

## v0.2.0

- **Aidoku sources.** Sumiyomi now runs [Aidoku](https://aidoku.app)'s WebAssembly sources as well as its own Lua
  ones. Aidoku's community repository is offered from the start, so hundreds of sources are installable from
  **Browse → Extensions**, and they are maintained by that community rather than by this app.
- A source's kind is shown in the Extensions list; each Aidoku source keeps its own settings.
- Sources that need a JavaScript engine or image editing say so plainly instead of failing oddly.

## v0.1.1

- **Several source repositories at once.** Browse → Extensions keeps a list: Sumiyomi's own is there from the
  start, any can be removed, and anyone's can be added. Sources from every repository are offered together, with
  the newest version of each winning.
- **Sources move out of the app** to [sumiyomi-sources](https://github.com/hdhindsa209/sumiyomi-sources), so a
  broken site can be fixed without an app update: **MangaDex** and **Mangapill** are there to install, and
  WeebCentral stays inside the app so there's a source on first run.
- Startup no longer warns when nothing is installed from a repository; `deploy.sh` copies the README.

## v0.1 — first release

The whole app: library, browse and search, manga pages, reader, downloads, updates, history, categories,
settings, storage, and installing sources from a repository.

- **Library**: list or cover grid, categories as tabs, hold to select several manga (categories, download unread,
  mark read or unread, remove with or without their downloads).
- **Browse**: popular, latest and search per source; Extensions tab to install, update and remove sources from a
  repository, checked by SHA-256 and api level, with no restart.
- **Manga page**: chapters with sort and filter, selection mode, per-manga download queue, "next 5 unread".
- **Reader**: tap zones and page keys, right-to-left or left-to-right (per manga), fit page or width, double-page
  splitting, cropping, margins, contrast, darkness, dithering, front light, optional progress bar. A chapter opens
  once its next pages are ready and loads the rest while reading; downloaded chapters open at once.
- **Downloads**: user-started, original images kept on the device, resumed after a restart, read instead of the
  network, optionally deleted once a chapter is finished.
- **Updates and History**: check the library or one category (with progress and cancel), optional auto-download of
  new chapters, day-grouped Updates, History with resume, remove and clear.
- **Settings and storage**: reader defaults, startup check, downloaded-only, incognito, page cache size and
  clearing, download size and deletion.
- **E-ink**: pure black and white, arrows instead of swipes, one loading screen then a single refresh, a full
  refresh on every screen or page change, battery level in the app bars.
