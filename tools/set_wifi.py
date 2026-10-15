#!/usr/bin/env python3
"""Save a board's uplink Wi-Fi over USB, without joining its access point.

Normally the uplink is set from /manage on the PocketWiki network or over
BLE. This writes it into the board's NVS partition instead, which is handy on
a bench or when provisioning a stack of boards: the computer keeps its own
internet connection throughout.

    python3 tools/set_wifi.py --port /dev/cu.usbmodem2101 --ssid SchoolNet

The password is asked for at the prompt (or read from PW_WIFI_PASSWORD), so
it does not land in shell history. The whole NVS partition is rewritten: any
other saved settings (a queued flasher pack list, Wi-Fi driver calibration)
are cleared and rebuilt by the firmware. Installed packs are not touched.
"""

from __future__ import annotations

import argparse
import getpass
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import archive_format as af  # noqa: E402


def _nvs_generator():
    """Espressif's NVS image generator, from pip or an ESP-IDF Python env."""
    try:
        import esp_idf_nvs_partition_gen.nvs_partition_gen as gen
        return gen
    except ImportError:
        pass
    for site in sorted(glob.glob(os.path.expanduser(
            "~/.espressif/python_env/*/lib/python3*/site-packages"))):
        if os.path.isdir(os.path.join(site, "esp_idf_nvs_partition_gen")):
            sys.path.append(site)
            try:
                import esp_idf_nvs_partition_gen.nvs_partition_gen as gen
                return gen
            except ImportError:
                sys.path.remove(site)
    raise SystemExit("set_wifi: install the generator with "
                     "`python3 -m pip install esp-idf-nvs-partition-gen`")


def _csv_value(value: str) -> str:
    if any(c in value for c in ',"\n\r'):
        raise SystemExit("set_wifi: commas, quotes and newlines are not supported here; "
                         "set this network from /manage instead")
    return value


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--partitions", type=Path, default=REPO / "firmware" / "partitions.csv",
                    help="partition table on the board (NVS is at the same place on both)")
    args = ap.parse_args(argv)

    if not 1 <= len(args.ssid.encode()) <= 32:
        raise SystemExit("set_wifi: an SSID is 1 to 32 bytes")
    password = os.environ.get("PW_WIFI_PASSWORD")
    if password is None:
        password = getpass.getpass(f"Password for {args.ssid} (empty for an open network): ")
    if len(password.encode()) > 64:
        raise SystemExit("set_wifi: a Wi-Fi password is at most 64 bytes")

    nvs = af.parse_partitions_csv(str(args.partitions))["nvs"]
    gen = _nvs_generator()
    with tempfile.TemporaryDirectory(prefix="pw-nvs-") as tmp:
        csv_path = Path(tmp) / "nvs.csv"
        image = Path(tmp) / "nvs.bin"
        csv_path.write_text("key,type,encoding,value\n"
                            "pocketwiki,namespace,,\n"
                            f"wifi_ssid,data,string,{_csv_value(args.ssid)}\n"
                            f"wifi_pass,data,string,{_csv_value(password)}\n")
        old_argv = sys.argv
        try:
            sys.argv = ["nvs_partition_gen", "generate", str(csv_path), str(image),
                        hex(nvs["size"])]
            gen.main()
        finally:
            sys.argv = old_argv
            csv_path.unlink()
        subprocess.run([sys.executable, "-m", "esptool", "--port", args.port,
                        "write-flash", hex(nvs["offset"]), str(image)], check=True)
    print(f"set_wifi: saved {args.ssid}; the board joins it after this reset.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
