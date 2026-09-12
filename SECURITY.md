# Security policy

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting for this repository. Do
not open a public issue until a fix is available. Include the affected commit,
reproduction steps, impact, and any suggested mitigation.

## Security model

PocketWiki is a local, read-only HTTP service on a device-created Wi-Fi
network. It does not provide transport encryption, user accounts, WAN access,
or remote update routes. Anyone who can join the access point can read its
articles and diagnostic endpoints.

Set a WPA2 password before deploying the device in a shared or public area.
Do not treat the device as a boundary for confidential material.

Article input is untrusted. The packer removes scripts, remote resources,
embedded objects, and unsupported attributes, then rewrites accepted internal
links. Archive headers, offsets, lengths, ordering, and CRC data are checked at
pack time and again by the firmware. Reports that bypass these controls are in
scope.

Physical access, radio jamming, vulnerabilities in Espressif SDK components,
and attacks that require a modified firmware image are outside this project's
direct control, though clear reproductions are still useful for dependency or
documentation updates.
