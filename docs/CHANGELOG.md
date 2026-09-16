# Changelog

## v1.0.2

- **Fixed: the Uninstall menu entries did nothing.** They passed the mode as a KUAL `params` field, which KUAL
  doesn't support, so the item never ran. Each mode is its own script now. The uninstaller also runs itself from
  /tmp before deleting anything — `/bin/sh` reads a script as it goes, so deleting its own file mid-run could
  have killed it partway — and writes what it did to `/mnt/us/sumiyomi-uninstall.log`.

## v1.0.1

- **Uninstall from KUAL**: **Sumiyomi → Uninstall** removes the app, either keeping your library or deleting it
  too. It refuses while the app is running, since the binary holds the screen and the native UI is stopped.

## v1.0.0

Everything in v0.1 through v0.3, with sleeping fixed. Developed and run against a jailbroken Kindle
(11th generation, 5.17.1.0.3).

- **Sleeping works, and it's the device's own sleep.** Sumiyomi no longer holds powerd's screensaver lock, so
  the power button, the idle timer and the screensaver behave exactly as they do everywhere else on the Kindle.
  The app listens to powerd rather than driving it: a sleep screen on the way down, and the screen you were on
  repainted when you come back, with the reader still on its page. (v0.3.0 tried to suspend the device itself
  and never actually suspended it; that code is gone.)
- Mangapill reports its covers (needs Mangapill 1.1.0 from the source repository).

## v0.3.0

- An attempt at sleeping the device by suspending it directly. It didn't work — the device never suspended —
  and v1.0.0 replaces it. Don't run this one.
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
