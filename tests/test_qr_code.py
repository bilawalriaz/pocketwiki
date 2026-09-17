"""Host checks for the firmware's QR encoder.

The encoder feeds the OLED screens that let a phone join the PocketWiki
access point and open the reader, so a wrong module matrix is user-visible.
The vectors pin the exact symbols for the two payloads the device draws; the
sweep checks version selection, determinism, and pattern placement over every
length the encoder accepts.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_MAIN = ROOT / "firmware" / "main"

# (payload, expected module matrix as rows of '#' and '.')
VECTORS = [
    ("WIFI:T:nopass;S:PocketWiki;;",
     (
         "#######..#...#..#.#######",
         "#.....#.##.#####..#.....#",
         "#.###.#..###..##..#.###.#",
         "#.###.#.##.######.#.###.#",
         "#.###.#...#.#..##.#.###.#",
         "#.....#.#.#..#.#..#.....#",
         "#######.#.#.#.#.#.#######",
         "...........##.#..........",
         "#####.######.##.##.#.#.#.",
         "###..#...#.###...#.#....#",
         "....######...#.##.#######",
         ".##......####.#...###..##",
         "#.#.####.#.#.######.##...",
         "#....#...####..##..#.##.#",
         "#.....#####...###.#.#..##",
         "#.###..###..#.#..#####.#.",
         "#...#.#.#..####.#####.###",
         "........##.#..#.#...#####",
         "#######.#.#...#.#.#.#####",
         "#.....#..####..##...#...#",
         "#.###.#.#.###.#######..#.",
         "#.###.#.##.#..#.###.#..##",
         "#.###.#.##...###.#.#.#..#",
         "#.....#.###.#.#.#.#.##..#",
         "#######.#..####..#..#.###",
     )),
    ("http://192.168.4.1/",
     (
         "#######.#..###..#.#######",
         "#.....#..##..##...#.....#",
         "#.###.#...#.###...#.###.#",
         "#.###.#.....#####.#.###.#",
         "#.###.#..#.#.##...#.###.#",
         "#.....#...#.###.#.#.....#",
         "#######.#.#.#.#.#.#######",
         "........#.##.#..#........",
         "##.##.#..###.#.#..#.....#",
         ".##..#..##..######..####.",
         ".#.#..#.#.##.#.#.#####..#",
         "...##..#.#####.#.#..#####",
         "##.#..####.###..###.....#",
         "####....#....###...##..#.",
         "##.#.###.#.###.###...####",
         "#.#..#.#.###....#...#.#.#",
         "#...#.###.#...#.#####.##.",
         "........#..######...#..#.",
         "#######....#....#.#.##..#",
         "#.....#.....#.#.#...#..#.",
         "#.###.#.#.#.##########..#",
         "#.###.#.#.....#...##.#.##",
         "#.###.#..##.#.#.....#.###",
         "#.....#.###...##.####.###",
         "#######.##...####.#..#..#",
     )),
]

# Byte-mode capacity of each version at ECC level L; the version the encoder
# must pick is the first one whose capacity holds the payload.
CAPACITY = [17, 32, 53, 78, 106, 134]


@pytest.fixture(scope="session")
def qr_harness(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available")
    exe = tmp_path_factory.mktemp("bin") / "c_qr_harness"
    subprocess.run(
        [cc, "-O2", "-I", str(FIRMWARE_MAIN),
         str(ROOT / "tests" / "c_qr_harness.c"),
         str(FIRMWARE_MAIN / "qr_code.c"),
         "-o", str(exe)],
        check=True, capture_output=True)
    return str(exe)


def encode(qr_harness, payloads):
    """Return [(size, rows)] for each payload, in input order."""
    out = subprocess.run([qr_harness], input="\n".join(payloads).encode(),
                         capture_output=True, check=True).stdout.decode().splitlines()
    result = []
    idx = 0
    for _ in payloads:
        size = int(out[idx])
        idx += 1
        result.append((size, out[idx:idx + size]))
        idx += size
    return result


def test_symbol_vectors(qr_harness):
    encoded = encode(qr_harness, [payload for payload, _ in VECTORS])
    for (payload, expected), (size, rows) in zip(VECTORS, encoded):
        assert size == len(expected), f"{payload!r} symbol is {size} modules"
        assert rows == list(expected), f"{payload!r} module matrix changed"


def test_device_payloads_fit_a_scannable_symbol(qr_harness):
    """Both screens render at two pixels per module on a 64 px panel, which
    leaves room only for version 1..3 symbols (29 modules + margin)."""
    for (payload, _), (size, _) in zip(VECTORS, encode(qr_harness, [p for p, _ in VECTORS])):
        assert size <= 29, f"{payload!r} needs a symbol too large for the OLED"


def test_version_selection_and_limits(qr_harness):
    lengths = [1, 17, 18, 32, 33, 53, 54, 78, 79, 106, 107, 134]
    payloads = ["z" * n for n in lengths]
    payloads.append("z" * 135)
    encoded = encode(qr_harness, payloads)

    for n, (size, rows) in zip(lengths, encoded):
        expected = 21 + 4 * next(i for i, cap in enumerate(CAPACITY) if n <= cap)
        assert size == expected, f"{n} bytes encoded as {size} modules"
        assert rows[0][:7] == "#######" and rows[0][-7:] == "#######"

    rejected = encoded[-1]
    assert rejected == (0, []), "a payload beyond version 6 must be rejected"


def test_encoding_is_deterministic(qr_harness):
    payloads = ["PocketWiki", "WIFI:T:WPA;S:PocketWiki;P:s3cret;;", "z" * 134]
    first = encode(qr_harness, payloads)
    second = encode(qr_harness, payloads)
    assert first == second


def test_finder_patterns_are_placed(qr_harness):
    """All three finders keep their 7x7 ring and centered 3x3 block."""
    payloads = ["z" * n for n in (1, 33, 134)]
    for size, rows in encode(qr_harness, payloads):
        for x0, y0 in ((0, 0), (size - 7, 0), (0, size - 7)):
            for y in range(7):
                for x in range(7):
                    dist = max(abs(x - 3), abs(y - 3))
                    want = dist in (0, 1, 3)
                    got = rows[y0 + y][x0 + x] == "#"
                    assert got == want, f"finder at {x0},{y0} module {x},{y}"
