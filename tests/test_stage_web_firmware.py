"""The web flasher's stage tool must describe the layout the CSVs describe.

The browser writes these offsets straight to flash, so a mismatch here is
silent: the board simply does not boot. Every expectation is derived from the
partition tables in the repository, and each image is checked against the
region it lands in.
"""

from pathlib import Path
import hashlib

import pytest

from tools import archive_format as af
from tools.stage_web_firmware import ArchiveError, stage

ROOT = Path(__file__).resolve().parents[1]

LAYOUTS = (
    ("esp32-c3", "partitions.csv", "4MB"),
    ("esp32-s3", "partitions_16mb.csv", "16MB"),
)

DEFAULT_SIZES = {
    "bootloader.bin": 0x4000,
    "partitions.bin": 0x800,
    "firmware.bin": 0x20000,
    "content/content.bin": 0x3000,
    "content/index.bin": 0x1000,
}


def make_dist(tmp_path, sizes=None):
    """Build a staged dist directory that looks like a PlatformIO post-build."""
    dist = tmp_path / "dist"
    for env, csv_name, _ in LAYOUTS:
        env_dir = dist / env
        (env_dir / "content").mkdir(parents=True)
        (env_dir / "partitions.csv").write_bytes((ROOT / "firmware" / csv_name).read_bytes())
        for rel, size in (sizes or DEFAULT_SIZES).items():
            (env_dir / rel).write_bytes(b"\x5a" * size)
    return dist


def test_manifest_offsets_come_from_the_partition_tables(tmp_path):
    out = tmp_path / "fw"
    manifest = stage(make_dist(tmp_path), out)
    assert manifest["schema"] == 1
    assert (out / "firmware.json").is_file()

    for (env, csv_name, flash_size), target in zip(LAYOUTS, manifest["targets"]):
        partitions = af.parse_partitions_csv(str(ROOT / "firmware" / csv_name))
        assert target["id"] == env.replace("-", "")
        assert target["flash_size"] == flash_size
        assert target["flash_bytes"] == partitions["packs"]["offset"] + partitions["packs"]["size"]
        assert target["nvs"] == {"offset": partitions["nvs"]["offset"], "size": partitions["nvs"]["size"]}

        offsets = {image["name"]: image["offset"] for image in target["images"]}
        assert offsets == {
            "Bootloader": 0x0,
            "Partition table": 0x8000,
            "Firmware": partitions["factory"]["offset"],
            "Starter library": partitions["content"]["offset"],
            "Library index": partitions["index"]["offset"],
        }
        # The manifest is what the browser fetches, so the staged bytes must match it.
        for image in target["images"]:
            data = (out / image["path"]).read_bytes()
            assert len(data) == image["bytes"]
            assert hashlib.sha256(data).hexdigest() == image["sha256"]


def test_check_only_validates_without_writing(tmp_path):
    out = tmp_path / "fw"
    manifest = stage(make_dist(tmp_path), out, check_only=True)
    assert [target["id"] for target in manifest["targets"]] == ["esp32c3", "esp32s3"]
    assert not out.exists()


@pytest.mark.parametrize("rel,size", (
    ("bootloader.bin", 0x8001),
    ("partitions.bin", 0x1001),
    ("firmware.bin", 0x180001),
    ("content/content.bin", 0x70001),
    ("content/index.bin", 0x10001),
))
def test_images_that_overflow_their_region_are_rejected(tmp_path, rel, size):
    sizes = dict(DEFAULT_SIZES, **{rel: size})
    with pytest.raises(ArchiveError, match=rel.rsplit("/", 1)[-1]):
        stage(make_dist(tmp_path, sizes), tmp_path / "fw", check_only=True)


def test_missing_images_name_the_build_command(tmp_path):
    dist = make_dist(tmp_path)
    (dist / "esp32-c3" / "firmware.bin").unlink()
    with pytest.raises(ArchiveError, match="pio run -d firmware -e esp32-c3"):
        stage(dist, tmp_path / "fw", check_only=True)
