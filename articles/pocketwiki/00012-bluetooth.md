# Bluetooth

The device advertises a Bluetooth Low Energy service for as long as it is
running. It exists so the Android app can find and set up a device that has no
network yet.

## What it is used for

- Sending Wi-Fi credentials to the device during setup.
- Uploading a pack when Wi-Fi transfer is not possible.
- Asking the device what is installed.

## Always available

Bluetooth stays available while the reader is serving, on both boards, so the
app never has to wait for a special mode. It costs memory, and on the 4 MB
board that memory budget is the reason the device does one thing at a time:
while a pack is being downloaded over Wi-Fi, Bluetooth is paused and resumes
when the transfer finishes.

## Do I need it?

No. Everything Bluetooth does, the browser can do too: the manage page at
`192.168.4.1/manage` sets up Wi-Fi and
installs packs from any phone or computer joined to the device's network.
Bluetooth is the convenience path, not the only one.
