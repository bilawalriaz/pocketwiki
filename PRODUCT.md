# Product

## Platform

adaptive

## Stack

ESP-IDF firmware for ESP32-C3 and ESP32-S3, an embedded HTML/CSS/JavaScript management and reading interface, Python pack tooling, a Swift flasher, and a native Android app built with Kotlin and Jetpack Compose.

## Users

Primary users are people carrying a PocketWiki device who want reliable access to a small offline reference library and a straightforward way to configure or refresh it from an Android phone or web browser.

## Product Purpose

PocketWiki turns a USB-C-powered ESP32-C3 or ESP32-S3 into a pocket library. It always offers a local Wi-Fi access point for reading and management, can join a 2.4 GHz network for online management, and can receive portable knowledge packs from an Android phone. Success means a new user can get the device online and install multiple packs without embedded-development knowledge; every installed pack participates in the library at the same time.

## Positioning

The library remains useful with no router, account, cloud service, or phone after setup. Its portable, deterministic pack format streams compressed articles directly from flash instead of requiring a database or full-text engine.

## Operating Context

- The device is powered and flashed over USB-C.
- Readers join the device's own access point and open `http://192.168.4.1/`.
- Web management is available over the access point and the joined LAN.
- Android uses BLE for discovery, Wi-Fi credentials, storage reporting, pack listing, install, and removal; the phone keeps its internet Wi-Fi connection.
- The verified baseline is a 4 MB ESP32-C3 with an SSD1306 128×64 OLED on GPIO1/GPIO0.

## Capabilities and Constraints

- The ESP32-C3 supports 2.4 GHz Wi-Fi only and runs AP+STA concurrently on one radio.
- A read-only PocketWiki Guide is recoverable from raw flash. It is built into the firmware, cannot be removed, and a newer copy from the catalogue updates it in place.
- Optional packs share a bounded internal flash filesystem. Every installed pack is enabled concurrently; firmware opens their indexes serially so serving remains memory-bounded.
- Pack files use the PocketWiki archive format (v3), sanitizer, and sorted title index. Articles are independent DEFLATE streams compressed against a shared trained dictionary, which the device loads into the 32 KiB inflate ring it already owns, so both boards decode the same pack and the dictionary costs the ESP32-C3 no additional RAM next to the resident Bluetooth the app relies on.
- BLE carries credentials, management commands, and chunked pack payloads with offset and CRC validation. It stays available whenever the reader is serving, on both boards, so the app is always a working fallback.
- No OTA firmware update path is in the first release.
- The default 4 MB layout deliberately prioritizes simplicity over dual-app OTA slots.

## Brand Commitments

Keep the PocketWiki name, its concise plain-English voice, the calm green reading identity, and the recognisable `P` library mark. The experience stays sleek and intentionally small rather than feature-dense.

## Evidence on Hand

- Article text for the optional packs is adapted from Wikipedia and lives in the companion pocketwiki-content repository under CC BY-SA 4.0.
- `articles/pocketwiki/` in this repository holds the device's own guide.
- The project has no user research, product photography, testimonials or commercial claims behind it, and the documentation must not invent any.

## Product Principles

- Offline reading is the invariant; networking only improves setup and refresh.
- Prefer one obvious action per screen and expose technical detail only when it helps recovery.
- Keep transfer protocols inspectable, bounded, and recoverable after interruption.
- Preserve a working starter library even when an optional pack is corrupt.
- Design for a 4 MB device first; larger-flash variants are explicit upgrades.

## Accessibility & Inclusion

The web surface must retain keyboard focus, readable contrast, reduced-motion support, and responsive text. Android uses Material 3 semantics, 48 dp touch targets, system font scaling, dark theme, predictive Back, and edge-to-edge insets.
