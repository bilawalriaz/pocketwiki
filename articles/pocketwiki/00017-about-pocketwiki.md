# About PocketWiki

PocketWiki is an offline library for two cheap ESP32 boards: the ESP32-C3 with
4 MB of flash and the ESP32-S3 with 16 MB. The board serves its own articles,
and the code that serves them is the project.

## What is in the box

- Firmware for both boards, building with ESP-IDF 6 and PlatformIO.
- This guide, built into the firmware so it survives everything.
- A packer that turns articles into packs, and a catalogue that publishes them.
- A reader and a manage page served by the device itself.
- An Android app for setup and pack installation.

## Licences

- **Firmware and tools**: MIT. Use them, change them, ship them.
- **Articles in packs**: CC BY-SA 4.0, with attribution to the source. Packs
  carry their licence and their sources; the catalogue lists them too.

## Contributing

The project lives at
`github.com/bilawalriaz/pocketwiki`.
Bug reports that describe the board, the flash size, and the smallest repeating
case are the most useful kind. Changes to how packs are read or how HTML is
sanitized are treated as trust boundaries and come with tests.

## Why it exists

Reading should not need a signal. A device that costs a few pounds, holds
thousands of articles, and works on a table in the middle of nowhere is the
whole idea, and it gets more useful every time someone adds a pack to the
catalogue.
