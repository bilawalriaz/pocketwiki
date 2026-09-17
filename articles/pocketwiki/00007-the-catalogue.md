# The pack catalogue

The catalogue is the list of packs that PocketWiki knows how to download. It
lives at `packs.educated.space` and lists each
pack's name, its article count, its size, and a checksum.

## How the device uses it

The manage page shows the catalogue from the device's own copy, which is
refreshed when the device has a Wi-Fi connection. When you install a pack, the
device checks the declared size and checksum before writing anything, then
validates the archive itself.

Because the catalogue is only a list, the device works perfectly well without
it: you can upload a pack file directly, or never install anything beyond this
guide.

## Versions and updates

Each pack has a version number. When a pack's content changes, its version
increases and a new file is published; older versions stay available at their
own addresses, so a pack you installed keeps working.

If a newer version of something you have is published, the manage page offers
it as an update. Installing the update replaces your copy; the older file is
removed, which frees its space at the same time.

## Where the articles come from

Article text comes from openly licensed sources, mainly Wikipedia and other
CC BY-SA material, distilled and packaged with attribution. Each pack lists its
licence on the manage page and in the catalogue. The firmware itself is MIT
licensed; see [About PocketWiki](00017-about-pocketwiki).
