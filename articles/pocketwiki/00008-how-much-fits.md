# How much fits

The number of articles a device can hold depends on the board and on the
articles themselves.

| Board | Pack storage | Roughly |
| --- | ---: | ---: |
| ESP32-C3, 4 MB | about 1.9 MB | 900 articles |
| ESP32-S3, 16 MB | about 14.4 MB | 6,000 articles |

The manage page reports the same numbers after every install, and it refuses
an install that would not fit rather than filling the filesystem.

## Why the estimate moves

An article costs about 2.4 KB once packed. Long articles with tables cost more;
short ones cost less. A pack also carries a shared dictionary of a few kilobytes
of common phrases, which pays for itself as soon as a pack holds more than a
handful of articles.

Because of that, a hundred 100-article packs fit slightly fewer articles in
total than one large pack would, and a pack of 20 short articles costs more per
article than a pack of 200. All of it is visible before you install: the
catalogue lists each pack's real size.

## Space that is always free

This guide lives in the device's built-in flash rather than in pack storage, so
it never competes with the packs you install, and it cannot be removed. Even
with every pack deleted, the reader still has something to open.

## Running out of room

When pack storage is full the device says so, with the numbers, and suggests
removing a pack. Removing one frees its space immediately, and there is no
reformatting step and no need to reinstall anything else.
