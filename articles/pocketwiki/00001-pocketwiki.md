# PocketWiki

PocketWiki is a small offline library that lives on an ESP32 board. It serves
its own articles over Wi-Fi, decodes them on the board itself, and keeps
working with no internet connection at all.

You are reading one of its articles now, which means the device is set up.
What is on it:

- **This guide**, built into the board's flash. It explains the device, and it
  cannot be deleted, so if you ever get lost it is still there.
- **Optional packs**: libraries of articles you install from the catalogue at
  `packs.educated.space` or upload from your
  phone. Around 900 articles fit on the 4 MB board and about 6,000 on the
  16 MB one.
- **A reader page** at `192.168.4.1` for browsing, searching, and reading, plus
  an **Android app** that sets the device up over Bluetooth.

## Read something

1. Join the Wi-Fi network the device creates, called `PocketWiki`.
2. Open `192.168.4.1` in any browser.
3. Pick a library, then an article.

## Add more

Open `192.168.4.1/manage` to connect the
device to your home Wi-Fi (so it can download packs by itself) and to install
or remove packs. Nothing on the device needs an account, and nothing you read
leaves it.
