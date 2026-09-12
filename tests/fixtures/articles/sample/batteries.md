# Batteries

A battery converts stored chemical energy into electrical energy. The name
comes from Benjamin Franklin, who used the military term for a row of
cannons ("a battery of Leyden jars").

## Primary vs secondary

- **Primary** cells are used once and discarded: alkaline AA cells, button
  cells, lithium coin cells.
- **Secondary** cells are rechargeable: lithium-ion (Li-ion), lithium polymer
  (LiPo), nickel-metal hydride (NiMH), lead-acid.

## Lithium-ion

Li-ion cells dominate portable electronics for three reasons: high energy
density, low self-discharge, and no memory effect. They are also finicky:
charging above 4.2 V per cell or discharging below about 2.5 V damages them, so
every pack carries protection electronics.

## For this project

PocketWiki is designed to run from USB power — a wall adapter or power bank.
A small battery could keep it alive in the field: the ESP32-S3 idles at a few
tens of milliamps and only wakes to serve pages.

See also: [Solar power](solar-power.md), [ESP32](esp32.md).
