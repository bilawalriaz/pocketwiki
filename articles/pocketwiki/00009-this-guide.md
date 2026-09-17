# This guide

The articles you are reading are themselves a pack: the one pack every
PocketWiki has.

## Built in, not installed

This guide is stored in the board's own flash, alongside the firmware, in the
space reserved for the built-in library. That has three consequences:

- It is always there. Delete every pack, wipe the pack storage, and the guide
  still opens.
- It does not use pack storage, so it never reduces how many articles you can
  install.
- It cannot be deleted. There is no remove control for it on the manage page,
  and the device refuses the request if one is sent anyway.

If you do not want it in the way once you know the device, open
`192.168.4.1/manage` and choose **Hide from libraries**: the guide leaves the
reader's list on the home page, its articles stay readable by link, and the
same button brings it back.

## Updating it

The guide has a version, like any pack, and it can be replaced by a newer one
from the catalogue without reflashing the board. When an update exists, the
manage page offers it; installing it downloads the new guide into pack storage,
where it takes precedence over the built-in copy. Remove the added copy and the
built-in guide returns.

## What is in it

This guide covers the device as it ships: setup, reading, packs, storage,
Wi-Fi, Bluetooth, the app, firmware updates, privacy, and troubleshooting. It
is written to be read on the device, offline, which is why it does not link to
anything outside it except the project's own catalogue address.
