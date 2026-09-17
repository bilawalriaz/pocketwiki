# How it works offline

Nothing about reading requires the internet, and nothing requires a service
to stay up. The whole library is on the board.

## Articles live on the device

Pack files are copied onto the device's own flash. When you open an article,
the device reads the compressed text from flash, expands it on the chip, and
sends it to your browser as an ordinary web page. That is why the reader is
identical on a brand-new phone and on an old laptop, and why it keeps working
in a car, a classroom, or anywhere else without a signal.

## Why packs are small

Articles are compressed with DEFLATE against a dictionary trained on the
articles in that pack: a few kilobytes of the phrases and words that repeat
across the collection. The dictionary is stored with the pack and loaded into
the decoder's memory before an article is expanded, which is why compression
costs the device no extra memory and why a whole science library fits in
megabytes.

## What the device never does

- It never fetches an article while you read it. There is no cache to miss and
  no page that half-loads because the network dropped.
- It never transcodes or re-encodes what you read; text arrives as HTML, and a
  downloaded article is a complete file.
- It never needs an account, a subscription, or a server of ours to be running.
  The only thing it ever asks the internet for is a pack you asked it to
  install, or a refreshed list of packs.
