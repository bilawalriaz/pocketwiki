# Troubleshooting

## I cannot see the PocketWiki network

Give it a few seconds after power-on; the access point starts while the board
boots. If the network still does not appear, check that the board is actually
powered (the USB port must supply enough current) and that you are within a few
metres. If you have set an access-point password, the network asks for it.

## The page will not load

Stay connected to the `PocketWiki` network. Phones often jump back to a
network with internet access as soon as it appears. Turn off mobile data if
your phone insists. Then open `192.168.4.1`
again, not a cached copy.

## A pack was refused

The device validates every pack before installing it and refuses anything that
does not add up, with a reason. The usual causes:

- The download was interrupted.
- The file is meant for a different format version. Rebuild it with the
  current packer.
- It is not a pack file at all, or it was renamed.

Nothing is changed when a pack is refused: your existing libraries keep
working.

## There is no room left

Remove a pack from the manage page; the space comes back immediately. The
manage page shows free space before you start an install, and the app checks
it while you choose packs.

## The reader is slow, or stops answering

A weak connection to your home Wi-Fi is the usual cause, because the access
point and the uplink share one radio. Move the device closer, or clear the
network in the manage page so it stops trying. Wireless readers also compete
with each other: one or two at a time is comfortable.

## The board does not respond to flashing

Hold BOOT, tap RESET, release BOOT, then start the flash again. Close any
serial monitor or other browser tab that may be holding the port. On a board
that has never run PocketWiki, that sequence is the way in.

## Start over

Erase the flash: in the browser flasher, leave "Erase the whole flash first"
ticked; over serial, run `esptool erase_flash` before flashing. Everything
including the saved Wi-Fi is cleared, and the built-in guide comes back with
the firmware.
