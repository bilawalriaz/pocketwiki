"""PlatformIO pre-build: refresh embedded CSS and the built-in biology pack.

The built-in article pack is read from the pocketwiki-content checkout; see
tools/content_paths.py for how that path is resolved.
"""

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

from pathlib import Path
import subprocess
import sys

firmware_dir = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
repo = firmware_dir.parent
out = Path(env.subst("$BUILD_DIR")) / "content"  # type: ignore[name-defined]

sys.path.insert(0, str(repo / "tools"))
from content_paths import ARTICLES, require  # noqa: E402

try:
    biology_articles = require(ARTICLES / "db-packs" / "biology-health")
except FileNotFoundError as exc:
    raise SystemExit(f"pocketwiki: {exc}")

subprocess.run([sys.executable, str(repo / "tools/embed_style.py")], check=True)
subprocess.run([sys.executable, str(repo / "tools/embed_catalog.py")], check=True)
subprocess.run([sys.executable, str(repo / "tools/embed_manager_js.py")], check=True)
subprocess.run([
    sys.executable, str(repo / "tools/pack_content.py"), "build",
    str(biology_articles), str(out), "--codec", "gzip",
], check=True)
# The active partition layout is per environment: partitions.csv for the
# 4 MB ESP32-C3, partitions_16mb.csv for the 16 MB ESP32-S3. The CMake side
# mirrors this choice from IDF_TARGET.
partitions_csv = env.GetProjectOption("board_build.partitions", "partitions.csv")
subprocess.run([
    sys.executable, str(repo / "tools/pack_content.py"), "partition-image",
    str(out), "--partitions", str(firmware_dir / partitions_csv),
], check=True)
