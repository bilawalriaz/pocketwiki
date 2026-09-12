# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Stack

ESP-IDF firmware for ESP32-C3, an embedded HTML/CSS/JavaScript management and reading interface, Python pack tooling, and a native Android app built with Kotlin and Jetpack Compose. The Android stack is delegated from the explicit request for a simple Android app and the existing native provisioning requirements.

## Users

Primary users are people carrying a PocketWiki device who want reliable access to a small offline reference library and a straightforward way to configure or refresh it from an Android phone or web browser. This is inferred from the supplied brief.

## Product Purpose

PocketWiki turns a USB-C-powered ESP32-C3 into a pocket library. It always offers a local Wi-Fi access point for reading and management, can join a 2.4 GHz network for online management, and can receive portable knowledge packs from an Android phone. Success means a new user can get the device online and install multiple packs without embedded-development knowledge; every installed pack participates in the library at the same time.

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
- A read-only Biology, Health & Medicine pack is recoverable from raw flash.
- Optional packs share a bounded internal flash filesystem. Every installed pack is enabled concurrently; firmware opens their indexes serially so serving remains memory-bounded.
- Pack files use the PocketWiki v2 archive, sanitizer, title index, and zstd-per-article decode path (shared trained dictionary, on-device decompression).
- BLE carries credentials, management commands, and chunked pack payloads with offset and CRC validation.
- No OTA firmware update path is in the first release.
- The default 4 MB layout deliberately prioritizes simplicity over dual-app OTA slots.

## Brand Commitments

Keep the PocketWiki name, concise plain-English voice, calm green reading identity, and recognisable `P` library mark established by `pocketwiki-oss`. The requested experience is sleek, modern, and intentionally small rather than feature-dense.

## Evidence on Hand

- The upstream packer, archive reader, sanitizer, server, tests, and stylesheet were imported from `pocketwiki-oss`.
- Article text lives in the separate `pocketwiki-content` repository: Wikipedia-derived pack sources under CC BY-SA 4.0, whose attribution status is recorded in that repository's ATTRIBUTION.md.
- No user research, product photography, testimonials, or commercial claims were supplied; future work must not invent them.

## Product Principles

- Offline reading is the invariant; networking only improves setup and refresh.
- Prefer one obvious action per screen and expose technical detail only when it helps recovery.
- Keep transfer protocols inspectable, bounded, and recoverable after interruption.
- Preserve a working starter library even when an optional pack is corrupt.
- Design for a 4 MB device first; larger-flash variants are explicit upgrades.

## Accessibility & Inclusion

The web surface must retain keyboard focus, readable contrast, reduced-motion support, and responsive text. Android uses Material 3 semantics, 48 dp touch targets, system font scaling, dark theme, predictive Back, and edge-to-edge insets.
