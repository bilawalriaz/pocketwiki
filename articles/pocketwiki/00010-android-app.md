# The Android app

The app is the companion for setting the device up and filling it with packs.
The reader stays in the browser; the app is about everything around it.

## What it does

- **Finds the device** over Bluetooth, so you do not have to guess an address.
- **Configures Wi-Fi**: it scans for 2.4 GHz networks and sends the credentials
  to the device over Bluetooth.
- **Installs packs**: pick several from the catalogue and it installs them in
  order, checking the space each one leaves before starting the next.
- **Shows the state** of the board: how much space is left, how many articles
  are installed, and which packs are on it.

## Wi-Fi first, Bluetooth as the fallback

Pack bytes travel over the device's own Wi-Fi network when the phone can reach
it, because that is much faster. When it cannot, for example a phone that refuses to stay
on a network without internet, the app falls back to sending the
pack over Bluetooth in acknowledged chunks. It is slower, and it works.

Bluetooth is always available on the device while it is serving, on both
boards, so the app can find it again at any time.

## Installing the app

Build it from the repository (`android/`, with `./gradlew assembleDebug`) or
install a build from the project's page. It needs no account and sends nothing
anywhere except to the device in front of you.
