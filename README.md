# Sumiyomi

A manga reader for jailbroken Kindles. Browse and search a source, keep a library, download chapters, and read
them on the e-ink screen. Modeled on [Mihon](https://mihon.app); not connected with Mihon or Amazon. Manga come
from the sources you install — nothing is hosted by this app.

## Install

1. Download `sumiyomi-<version>.zip` from [Releases](../../releases).
2. Unzip it and copy the `sumiyomi` folder into `/mnt/us/extensions/` on the Kindle.
3. On the Kindle, with Wi-Fi on: **KUAL → Sumiyomi → Start Sumiyomi**.
4. Leave the app with **More → Exit Sumiyomi**.

Needs a jailbroken Kindle with [KUAL](https://www.mobileread.com/forums/showthread.php?t=203326). Your library,
downloads and logs live in `/mnt/us/sumiyomi/`.

To remove it: **More → Uninstall Sumiyomi** in the app, which offers either removing the app and keeping your
library (reinstalling carries on where you left off) or removing everything. There is the same pair under
**KUAL → Sumiyomi → Uninstall**. Neither touches KUAL or the jailbreak.

## Sources

Manga come from sources you install in **Browse → Extensions**, which lists two repositories from the first run:

- **Sumiyomi's own** — small sandboxed Lua sources. WeebCentral is built in; MangaDex and Mangapill install from
  [sumiyomi-sources](https://github.com/hdhindsa209/sumiyomi-sources).
- **[Aidoku](https://aidoku.app)'s community sources** — WebAssembly modules covering hundreds of sites, kept
  working by that community rather than by this app.

You can add anyone else's repository, or remove either of those. A source needing a JavaScript engine or image
editing won't install and says so, and a site that blocks plain clients (Cloudflare) will list nothing.

## Which Kindles does it work on?

Built and tested only on a base Kindle (11th generation, firmware 5.17.1.0.3). It should run on other recent
touch Kindles at 300 ppi — Paperwhite 3 and 4, Basic 10, Oasis — but that is untested. The screen layout is sized
for 300 ppi, so 167 ppi models (Kindle 7/8) will look wrong, and very old models won't run the binary at all.
Newer devices (Paperwhite 5, Scribe, Colorsoft) are unknown. If you try one, please open an issue saying what
happened.

## Building it yourself

See [docs/DEVELOPING.md](docs/DEVELOPING.md).
