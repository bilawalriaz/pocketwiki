# Privacy

The device is a library, not a service. It collects nothing about you and sends
nothing about your reading anywhere.

## What stays on the device

- Your Wi-Fi credentials, stored in the board's own configuration flash and
  used only to join that network.
- The packs you installed and the guide it came with.
- Which articles you read. Nothing is logged beyond a small live count of
  connected readers shown on the device's status page, and it is gone on the
  next reboot.

## What uses the network

Two things, both of them things you asked for:

- Refreshing the pack catalogue, when the device is connected to your Wi-Fi.
- Downloading a pack you chose to install.

Both go to the project's catalogue host over HTTPS. Nothing else is contacted,
and there is no telemetry, no analytics, and no crash reporting.

## The access point

The device's own network is open by default, so anyone in range can read the
library and reach the manage page. That is fine for a kitchen table and not
fine for a school corridor. Set an access-point password when you build the
firmware (`POCKETWIKI_AP_PASSWORD`) if the device lives somewhere shared.

## Your reading stays yours

Articles are served to whichever browser asks. The device has no idea who you
are, keeps no history, and has no account to sign into.
