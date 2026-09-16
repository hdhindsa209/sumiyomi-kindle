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
downloads and logs live in `/mnt/us/sumiyomi/`; deleting that folder resets the app.

## Which Kindles does it work on?

Built and tested only on a base Kindle (11th generation, firmware 5.17.1.0.3). It should run on other recent
touch Kindles at 300 ppi — Paperwhite 3 and 4, Basic 10, Oasis — but that is untested. The screen layout is sized
for 300 ppi, so 167 ppi models (Kindle 7/8) will look wrong, and very old models won't run the binary at all.
Newer devices (Paperwhite 5, Scribe, Colorsoft) are unknown. If you try one, please open an issue saying what
happened.

## Building it yourself

See [docs/DEVELOPING.md](docs/DEVELOPING.md).
