# Arduino

Arduino is an open-source electronics platform: affordable hardware boards
plus an easy-to-use development environment. It was born in 2005 at the
Interaction Design Institute Ivrea, Italy, as a tool for students who were not
electronics engineers.

## The ecosystem

- **Hardware**: boards such as the Uno, Nano, and Mega, built around AVR and
  ARM microcontrollers, plus countless compatible clones.
- **Software**: the Arduino IDE and a C++ framework that hides most of the
  hardware details behind simple functions like `digitalWrite`.
- **Libraries**: thousands of community libraries for sensors, displays, and
  connectivity.

## ESP32 and Arduino

The ESP32 is supported by the Arduino environment, which makes it very popular
for hobby projects. PocketWiki deliberately uses **ESP-IDF** instead: the
official framework gives a real FreeRTOS, a production HTTP server with
streaming, precise flash partitioning, and watchdog support — the kind of
control a reliable network appliance needs.

See also: [ESP32](esp32.md), [Microcontroller](microcontroller.md).
