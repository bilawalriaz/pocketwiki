# Installing packs

A pack is a file containing a set of articles: a science library, a set of
curated exam topics, a collection you built yourself. Installing one copies it
onto the device, where it stays until you remove it.

## From the device

1. Give the device a Wi-Fi connection ([Connecting to Wi-Fi](00005-wi-fi)).
2. Open `192.168.4.1/manage`.
3. Look at the catalogue, choose the packs you want, and install them.

The device downloads each pack itself and reports progress on the page and, if
you have one, on the OLED. A download can take a while on a slow connection,
and Bluetooth is paused during the transfer so the radio has the bandwidth to
itself.

## From your phone

The Android app can upload packs directly, either over the device's Wi-Fi or
over Bluetooth as a fallback. It checks free space as you pick packs, so you
can queue several and let them install in order. See
[The Android app](00010-android-app).

You can also upload a `.pwp` file from the manage page in any browser.

## What happens to a pack before it is kept

Every pack is checked before it is installed: its size, its checksums, its
headers, its article offsets, and its index. If any of it does not add up, the
upload is discarded and the library you already had keeps working. A failed
install never leaves you with half a pack.

## Removing a pack

Remove a pack from the manage page; the space it used comes back immediately.
This guide cannot be removed, because it is built into the device's flash. See
[This guide](00009-this-guide).
