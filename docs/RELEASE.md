# Releasing Sumiyomi

One version number: `project(sumiyomi VERSION ...)` in `CMakeLists.txt`. `SUMI_VERSION` (user agent, More → About)
and the KUAL `config.xml` in the package both come from it; the zip is named after it.

## Steps

1. **Bump the version** in `CMakeLists.txt` and note what changed in `docs/CHANGELOG.md`.
2. **Host checks:** `cmake --build --preset host && ctest --test-dir build/host` — all test programs pass.
   Goldens are part of this: a screen that changed on purpose is re-recorded and *looked at* first.
3. **Kindle build:** `./tools/kbuild.sh` (warnings are errors there too).
4. **Package:** `./tools/package-kual.sh` → `build/package/sumiyomi/` and `build/package/sumiyomi-<version>.zip`.
5. **Device checks** (`./tools/deploy.sh` first):
   - `./tools/crash-tests.sh` passes (SIGTERM and abort leave the native UI restored).
   - Walk through: library list and covers, categories, browse and search, a manga page, reading a chapter
     (turning pages, the menu, settings), downloading a chapter and reading it offline, a library check,
     History, Data and storage, exit.
   - Battery and front light read correctly; page turns show no stray refreshes or ghosting.
6. **Tag** the commit and attach the zip.

## Checks worth repeating on a fresh device

- First run with no database: the library is empty, Browse works, nothing crashes.
- Upgrade over an existing install: the database migrates (schema version in `src/data/db.cpp`), the library,
  downloads and settings survive.
- Out of space: downloads fail with a message rather than a crash; the page cache evicts oldest-first.

## Known limits in v0.1

- One bundled source (WeebCentral). Others need a repository (`tools/ext/build_index.py`).
- No trackers, no migration between sources, no statistics, no backup or restore (M6).
- Webtoon-style vertical reading is only page-by-page slices; the focus is manga.
- Preparing a page takes about 255 ms on the device against a 180 ms target, so the first pages of a chapter
  that isn't downloaded take a moment (NEON work is the obvious next step).
