"""PlatformIO post-build: stage flash images into firmware/dist/<env>/.

PlatformIO keeps build products in the gitignored .pio/build/<env> tree.
This hook copies the exact files the flashers (tools/flash_all.py, flasher/)
discover into one stable, browsable folder per environment:

    firmware/dist/<env>/bootloader.bin
    firmware/dist/<env>/partitions.bin
    firmware/dist/<env>/firmware.bin
    firmware/dist/<env>/partitions.csv
    firmware/dist/<env>/content/content.bin
    firmware/dist/<env>/content/index.bin

Point the GUI's build-directory picker (or --fw-dir / -P) at
firmware/dist/<env> and every artifact is in one place.
"""

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

from pathlib import Path
import shutil

env_name = env["PIOENV"]  # type: ignore[name-defined]
build = Path(env.subst("$BUILD_DIR"))  # type: ignore[name-defined]
dist = Path(env.subst("$PROJECT_DIR")) / "dist" / env_name  # type: ignore[name-defined]


def stage_images(target, source, env):  # noqa: ARG001
    dist.mkdir(parents=True, exist_ok=True)
    for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        src = build / name
        if src.exists():
            shutil.copy2(src, dist / name)
        else:
            print(f"pio_post: skipping missing {src}")
    content_src = build / "content"
    if content_src.exists():
        (dist / "content").mkdir(parents=True, exist_ok=True)
        for name in ("content.bin", "index.bin"):
            src = content_src / name
            if src.exists():
                shutil.copy2(src, dist / "content" / name)
    # Partition CSV for this env, so the folder is self-contained.
    parts_csv = env.GetProjectOption("board_build.partitions", "partitions.csv")
    csv_src = Path(env.subst("$PROJECT_DIR")) / parts_csv  # type: ignore[name-defined]
    if csv_src.exists():
        shutil.copy2(csv_src, dist / "partitions.csv")
    print(f"pio_post: staged flash images in {dist}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", stage_images)  # type: ignore[name-defined]
