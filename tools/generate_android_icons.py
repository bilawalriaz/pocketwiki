#!/usr/bin/env python3
"""Generate the Android app's ImageVector icon set from Material Symbols.

The Android UI owns its own icon vocabulary instead of depending on
``material-icons-extended``: this script downloads the exact Material Symbols
Rounded glyphs the app uses (Apache-2.0, the same set Google's icon font
serves), rewrites each glyph's path from Material Symbols' ``0 -960 960 960``
view box into Compose's 24 dp viewport, and writes the result to
``android/app/src/main/java/com/example/pocketwiki/ui/icons/PocketIcons.kt``.

Re-run only when the icon list below changes:

    python3 tools/generate_android_icons.py

The generated file is committed; firmware and pack builds never run this.
"""

from __future__ import annotations

import pathlib
import re
import sys
import urllib.request

SOURCE = "https://fonts.gstatic.com/s/i/short-term/release/materialsymbolsrounded/{name}/fill1/24px.svg"
VIEW_BOX = "0 -960 960 960"
VIEW_SPAN = 960.0
TARGET = 24.0
SCALE = TARGET / VIEW_SPAN

# Material Symbols name -> Kotlin property name. Order is the emission order.
ICONS: dict[str, str] = {
    "menu_book": "Library",
    "library_add": "AddPacks",
    "router": "Device",
    "bluetooth": "Bluetooth",
    "bluetooth_searching": "BluetoothSearching",
    "bluetooth_disabled": "BluetoothOff",
    "wifi": "Wifi",
    "sync": "Sync",
    "refresh": "Refresh",
    "cloud_download": "CloudDownload",
    "folder_open": "FolderOpen",
    "link": "Link",
    "open_in_new": "OpenInNew",
    "content_copy": "Copy",
    "delete": "Delete",
    "storage": "Storage",
    "phone_android": "Phone",
    "check_circle": "CheckCircle",
    "check": "Check",
    "error": "Error",
    "warning": "Warning",
    "lock": "Lock",
    "visibility": "Visible",
    "visibility_off": "Hidden",
    "arrow_back": "ArrowBack",
    "chevron_right": "ChevronRight",
    "expand_more": "ExpandMore",
    "expand_less": "ExpandLess",
    "settings": "Settings",
}

ARITY = {"M": 2, "L": 2, "T": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "A": 7, "Z": 0}
TOKEN = re.compile(r"([MmLlHhVvCcSsQqTtAaZz])|(-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)")


def fetch(name: str) -> str:
    with urllib.request.urlopen(SOURCE.format(name=name), timeout=30) as response:
        return response.read().decode("utf-8")


def extract(svg: str, name: str) -> tuple[str, bool]:
    """Return the glyph's concatenated path data and whether it needs even-odd fill."""
    paths = re.findall(r"<path\b([^>]*)/?>", svg)
    if not paths:
        raise SystemExit(f"{name}: no <path> in the served SVG")
    even_odd = any('fill-rule="evenodd"' in attributes for attributes in paths)
    data = [
        re.search(r'\bd="([^"]+)"', attributes).group(1)
        for attributes in paths
        if re.search(r'\bd="([^"]+)"', attributes)
    ]
    if not data:
        raise SystemExit(f"{name}: path element without a d attribute")
    return " ".join(data), even_odd


def rewrite(data: str, name: str) -> str:
    """Map Material Symbols' 0/-960 view box onto Compose's 24x24 viewport."""
    tokens = TOKEN.findall(data)
    out: list[str] = []
    index = 0
    command = ""
    leading: bool | None = None
    while index < len(tokens):
        letter, _ = tokens[index]
        if letter:
            command = letter
            index += 1
            if command.upper() == "Z":
                out.append(letter)
                continue
            if command in "mM" and not out:
                # A moveto at the path origin is relative to (0, 0), and this
                # rewrite moves that origin. Anchor the first pair absolutely so
                # the glyph keeps its position, and re-emit the remaining pairs
                # of the same command as explicit linetos.
                pair = _values(tokens, index, name, command, 2)
                index += 2
                out.append("M" + " ".join(_format(_map("M", k, v, False)) for k, v in enumerate(pair)))
                leading = command.islower()
                continue
            out.append(letter)
            leading = None
        elif not command:
            raise SystemExit(f"{name}: path data starts with a number")
        elif leading:
            out.append("l")

        upper = command.upper()
        relative = command.islower()
        values = _values(tokens, index, name, command, ARITY[upper])
        index += ARITY[upper]

        for offset, value in enumerate(values):
            out.append(_format(_map(upper, offset, value, relative)))
    return " ".join(out)


def _values(tokens: list[tuple[str, str]], index: int, name: str, command: str, arity: int) -> list[float]:
    if index + arity > len(tokens):
        raise SystemExit(f"{name}: truncated {command} command")
    return [float(tokens[index + offset][1]) for offset in range(arity)]


def _map(command: str, offset: int, value: float, relative: bool) -> float:
    """Scale one path value, applying the y offset only for absolute coordinates."""
    axis = _axis(command, offset)
    if axis is None:  # arc rotation and the two arc flags pass through untouched
        return value
    if relative:
        return value * SCALE if axis != "flag" else value
    if axis == "x":
        return value * SCALE
    if axis == "y":
        return (value + VIEW_SPAN) * SCALE
    return value


def _axis(command: str, offset: int) -> str | None:
    if command == "H":
        return "x"
    if command == "V":
        return "y"
    if command == "A":
        if offset in (0, 1):
            return "x"
        if offset in (3, 4):
            return "flag"
        if offset == 5:
            return "x"
        if offset == 6:
            return "y"
        return None
    return "x" if offset % 2 == 0 else "y"


def _format(value: float) -> str:
    rounded = round(value, 4)
    if rounded == int(rounded):
        return str(int(rounded))
    return f"{rounded:.4f}".rstrip("0").rstrip(".")


HEADER = '''package com.example.pocketwiki.ui.icons

/*
 * GENERATED by tools/generate_android_icons.py — do not edit by hand.
 *
 * Material Symbols Rounded, fill 1, weight 400, 24 dp grid (Apache-2.0,
 * https://fonts.google.com/icons). Each path is rewritten from the symbol's
 * native 0/-960 view box into Compose's 0..24 viewport.
 */

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.unit.dp

/** The Android app's complete icon vocabulary, drawn from one Material Symbols set. */
object PocketIcons {
'''

FOOTER = '''
  private val cache = HashMap<String, ImageVector>()

  private fun icon(name: String, block: ImageVector.Builder.() -> Unit): ImageVector =
    ImageVector.Builder(
      name = "PocketIcons.$name",
      defaultWidth = 24.dp,
      defaultHeight = 24.dp,
      viewportWidth = 24f,
      viewportHeight = 24f,
    ).apply(block).build()
}
'''


def kotlin(icons: dict[str, tuple[str, bool]]) -> str:
    blocks = [HEADER]
    for property_name, (data, even_odd) in icons.items():
        fill_type = "PathFillType.EvenOdd" if even_odd else "PathFillType.NonZero"
        blocks.append(
            f"""
  val {property_name}: ImageVector
    get() = cache.getOrPut("{property_name}") {{
      icon("{property_name}") {{
        addPath(
          pathData = addPathNodes("{data}"),
          pathFillType = {fill_type},
          fill = SolidColor(Color.Black),
        )
      }}
    }}
"""
        )
    blocks.append(FOOTER)
    return "".join(blocks)


def main() -> int:
    icons: dict[str, tuple[str, bool]] = {}
    for symbol, property_name in ICONS.items():
        data, even_odd = extract(fetch(symbol), symbol)
        icons[property_name] = (rewrite(data, symbol), even_odd)
        print(f"{symbol:24} -> {property_name}")

    target = pathlib.Path(__file__).resolve().parent.parent / (
        "android/app/src/main/java/com/example/pocketwiki/ui/icons/PocketIcons.kt"
    )
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(kotlin(icons), encoding="utf-8")
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
