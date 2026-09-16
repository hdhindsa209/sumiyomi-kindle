# Sumiyomi

A manga reader for jailbroken Kindles. Browse a source, keep a library, download chapters, read them on e-ink.
Modeled on [Mihon](https://mihon.app). Manga come from the sources you install; nothing is hosted here.

## Install

1. Download `sumiyomi-<version>.zip` from [Releases](../../releases).
2. Unzip it and copy the `sumiyomi` folder into `/mnt/us/extensions/` on the Kindle.
3. On the Kindle, with Wi-Fi on: **KUAL → Sumiyomi → Start Sumiyomi**.
4. Leave the app with **More → Exit Sumiyomi**.

Needs a jailbroken Kindle with [KUAL](https://www.mobileread.com/forums/showthread.php?t=203326). Your library
lives in `/mnt/us/sumiyomi/`.

To remove it: **More → Uninstall Sumiyomi**, or **KUAL → Sumiyomi → Uninstall**. Either keeps your library or
deletes it, your choice; neither touches KUAL or the jailbreak.

## Sources

Install sources in **Browse → Extensions**. Two repositories are listed from the first run:

- **Sumiyomi's own** — sandboxed Lua sources. WeebCentral is built in; MangaDex and Mangapill install from
  [sumiyomi-sources](https://github.com/hdhindsa209/sumiyomi-sources).
- **[Aidoku](https://aidoku.app)'s** — WebAssembly sources covering hundreds of sites, maintained by that
  community.

You can add or remove any repository. Sources needing a JavaScript engine won't install, and Cloudflare-protected
sites list nothing.

## Which Kindles does it work on?

Built and tested only on a base Kindle (11th generation, firmware 5.17.1.0.3). Other 300 ppi touch Kindles
(Paperwhite 3/4, Basic 10, Oasis) should work but are untested; 167 ppi models will look wrong. If you try one,
open an issue saying what happened.

## Building it yourself

See [docs/DEVELOPING.md](docs/DEVELOPING.md).
