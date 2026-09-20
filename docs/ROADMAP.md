# Roadmap

## Generate offline packs from pasted URLs

**Status:** Planned

Add a parent-friendly way to turn links into a PocketWiki pack without requiring
parents to prepare Markdown, run the pack builder, or understand the archive
format.

### User outcome

A parent opens either the PocketWiki web GUI or the Android app, pastes one or
more URLs to pages or documents, names the collection, and sends it to the
requesting ESP32. The device stores the resulting compressed `.pwp` pack. The
parent can then disable the internet or remove the device from the home network
while children continue reading the downloaded material through PocketWiki's
local access point.

Adding more content should be an additive action in either client: parents add
URLs to an existing or new collection and request an updated pack. They should
not need to create, upload, or manually maintain pack files.

### Proposed flow

1. The parent enters page or document URLs in either the web GUI or Android app,
   with a pack name and optional description. Both clients submit the same
   URL-to-pack job rather than implementing separate content pipelines.
2. The service creates a job for the requesting device and fetches the sources
   on the backend by default. Backend processing is preferred because it can
   handle HTML/PDF extraction, sanitisation, retries, size limits, and pack
   generation without consuming ESP32 memory.
3. The service converts the fetched sources into the existing article input,
   removes active content and remote resources, rewrites accepted internal
   links, preserves source and attribution metadata, and builds a verified
   `.pwp` archive using the existing pack tooling. Pack v3 (deflate + a shared
   trained dictionary), which every build decodes: the dictionary is loaded
   into the inflate ring the article path already owns, so it costs the C3 no
   RAM. See the device limits in [ARCHIVE_FORMAT.md](ARCHIVE_FORMAT.md).
4. The service reports progress and pack size in the GUI, then pushes or makes
   the completed pack available to the requesting ESP32. Transfer must support
   the existing resumable Wi-Fi path and its BLE fallback where applicable.
5. The device validates the pack with the existing size, CRC, header, index, and
   archive checks, then atomically installs it. A failed job or transfer must
   leave previously installed packs readable.
6. Once installed, the collection is available from the local reader without
   internet access, an account, or the backend remaining reachable.

### Processing boundary

Implement backend processing first. Keep on-device processing as a later,
measured option only if it is useful for small, already-supported inputs and
fits the C3 memory, storage, timeout, and TLS constraints. The ESP32 should not
be expected to crawl arbitrary websites or execute page JavaScript.

The backend/device protocol should make the boundary explicit so a future local
processor can be added without changing the pack format or reader experience.

### Android app support for the same URL workflow

The existing Android app must expose the same URL-to-pack feature, not merely be
an installer for prebuilt packs. The app already treats `.pwp` files as opaque
bytes: it downloads a pack, checks its size and catalogue SHA-256 when
available, then uploads it over Wi-Fi and falls back to chunked BLE. It does not
need a decoder for this flow.

The Android link screen should let the parent paste URLs and a pack name, submit
the backend job, see processing progress and errors, download the completed pack,
and install it through the existing `uploadNow` transport logic. The phone should
download the pack while it still has internet access, then hand it to the ESP32
over the current same-network, PocketWiki-AP, or BLE path. This supports the
offline-parent scenario even when the device itself is not configured for station
Wi-Fi.

Generated packs should use a trusted job result or signed download URL rather
than being forced through the public catalogue validation. The existing device
size, checksum, archive, and atomic-install checks remain authoritative.

### Acceptance criteria

- A parent can paste one or more supported URLs in either the web GUI or Android
  app and see a queued, processing, completed, or failed state with an actionable
  error.
- A completed request produces a deterministic, compressed `.pwp` pack using
  the existing archive format, uses bounded Zstd v2 by default, and records
  source URLs, fetch time, and attribution where available.
- The requesting ESP32 receives the pack, resumes an interrupted transfer, and
  installs it only after the normal device validation succeeds.
- The Android app's URL workflow can submit a link job, download its completed
  `.pwp`, and install it through the existing Wi-Fi-first and BLE-fallback
  transfer path.
- The new pack and its articles appear in the existing local search and reader.
- Reading still works after station Wi-Fi and internet access are disabled.
- Content is bounded and safe for the device: no active scripts, remote assets,
  unbounded crawl, or unreviewed external links are carried into the pack.
- Existing built-in and optional packs remain available if fetching,
  conversion, transfer, or installation fails.

### Decisions still needed

- Whether the GUI is hosted by the device, a local companion service, or a
  hosted parent dashboard, and how a device is authorised to receive a job.
- Which source types are supported initially: normal HTML pages, linked pages,
  PDFs, EPUBs, or selected documentation sites.
- Whether a submitted URL means one page only, a bounded link set, or a
  user-selected crawl depth and domain allowlist.
- Backend storage, retention, privacy, and handling of authenticated or
  paywalled sources.
- Per-pack and per-job size limits appropriate for 4 MB C3 and larger S3
  layouts.
