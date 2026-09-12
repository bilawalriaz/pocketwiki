#!/bin/sh
set -eu

site_script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$site_script_dir/../.." && pwd)

rm -rf "$site_script_dir/public/pocketwiki" "$site_script_dir/public/pocketwiki2" "$site_script_dir/public/pocketwiki3"
mkdir -p "$site_script_dir/public/pocketwiki/assets/screenshots" "$site_script_dir/public/pocketwiki2/assets/screenshots" "$site_script_dir/public/pocketwiki3/assets/screenshots"
cp "$repo_root/pocketwiki3.html" "$site_script_dir/public/pocketwiki/index.html"
cp "$repo_root/index.html" "$site_script_dir/public/pocketwiki2/index.html"
cp "$repo_root/pocketwiki3.html" "$site_script_dir/public/pocketwiki3/index.html"
cp "$repo_root"/assets/screenshots/*.png "$site_script_dir/public/pocketwiki/assets/screenshots/"
cp "$repo_root"/assets/screenshots/*.png "$site_script_dir/public/pocketwiki2/assets/screenshots/"
cp "$repo_root"/assets/screenshots/*.png "$site_script_dir/public/pocketwiki3/assets/screenshots/"
python3 "$repo_root/tools/build_landing_catalog.py" \
  --output "$site_script_dir/public/pocketwiki/assets/pack-catalog.json"
python3 "$repo_root/tools/build_landing_catalog.py" \
  --output "$site_script_dir/public/pocketwiki2/assets/pack-catalog.json"
python3 "$repo_root/tools/build_landing_catalog.py" \
  --output "$site_script_dir/public/pocketwiki3/assets/pack-catalog.json"

# The page flashes boards itself, so it ships the images it writes. Build them
# first (pio run -d firmware -e esp32-c3 -e esp32-s3); the tool refuses to write
# a manifest when an image overflows its partition.
python3 "$repo_root/tools/stage_web_firmware.py" \
  --out "$site_script_dir/public/pocketwiki/fw"
cp -R "$site_script_dir/public/pocketwiki/fw" "$site_script_dir/public/pocketwiki2/fw"
cp -R "$site_script_dir/public/pocketwiki/fw" "$site_script_dir/public/pocketwiki3/fw"
