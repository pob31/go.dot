#!/usr/bin/env python3
# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Go.dot's icons, rendered from the SVG sources in packaging/icons/.

Produces: packaging/icons/Go.dot.icns   macOS app icon
          packaging/icons/Go.dot.ico    Windows app icon
          packaging/icons/png/go.dot-<N>.png          Linux (hicolor) sizes
          packaging/icons/png/GoDotMenuBarTemplate.png and @2x
         Exits 0 when every file was written, 1 with a sentence saying why not.
Usage:   python3 scripts/make-icons.py
Build requirements: python3, Pillow (`pip install pillow`) and `rsvg-convert`
         on PATH (librsvg; `apt install librsvg2-bin`, `brew install librsvg`).

The renders are committed so the build never needs either tool. Run this after
changing an SVG, and commit the SVG and its renders together.

TWO CUTS OF ONE ICON. The full-detail drawing (the grid, the stone, the dot)
is used from 48 or 64 px up. At 32 px and under its dot is a pixel wide and the
grid is mush, so those sizes come from the small cut: no grid, a bigger stone
and a bigger dot. Which sizes take which cut is SMALL_MAX below.

TWO SHAPES. macOS draws its tile on Apple's icon grid, with a margin the Dock
expects (app.svg). Windows and Linux fill the square (app-square.svg).

THE MENU BAR GLYPH is a macOS template image: black on transparent, which the
system tints for a light or a dark menu bar. The file name must end in
"Template" for AppKit to treat it as one when loaded by name.
"""

from __future__ import annotations

import io
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ICONS = REPO_ROOT / "packaging" / "icons"
PNG = ICONS / "png"

# The largest size drawn from the small cut; anything bigger uses full detail.
SMALL_MAX = 32

MACOS_SIZES = (16, 32, 64, 128, 256, 512, 1024)
WINDOWS_SIZES = (16, 24, 32, 48, 64, 128, 256)
LINUX_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)


def render(svg: Path, size: int) -> bytes:
    """One square PNG of `svg` at `size` pixels."""
    return subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size), str(svg)],
        check=True, capture_output=True).stdout


def app_png(shape: str, size: int) -> bytes:
    """The app icon at `size`, in the cut that suits it. shape: "" or "-square"."""
    cut = "-small" if size <= SMALL_MAX else ""
    return render(ICONS / f"app{shape}{cut}.svg", size)


def main() -> int:
    if shutil.which("rsvg-convert") is None:
        print("make-icons: rsvg-convert is not on PATH (it comes with librsvg).")
        return 1
    try:
        from PIL import Image
    except ImportError:
        print("make-icons: Pillow is not installed (pip install pillow).")
        return 1

    def image(data: bytes) -> "Image.Image":
        return Image.open(io.BytesIO(data)).convert("RGBA")

    PNG.mkdir(exist_ok=True)

    # macOS. Pillow picks each .icns slot's image by pixel size from these, so
    # the 32 px render serves both 32 and 16@2x, and 64 both 64 and 32@2x.
    mac = [image(app_png("", s)) for s in MACOS_SIZES]
    mac[-1].save(ICONS / "Go.dot.icns", append_images=mac[:-1])

    # Windows. Each size is its own render, not a resize of the largest.
    win = [image(app_png("-square", s)) for s in WINDOWS_SIZES]
    win[-1].save(ICONS / "Go.dot.ico", sizes=[(s, s) for s in WINDOWS_SIZES],
                 append_images=win[:-1])

    for s in LINUX_SIZES:
        (PNG / f"go.dot-{s}.png").write_bytes(app_png("-square", s))

    (PNG / "GoDotMenuBarTemplate.png").write_bytes(render(ICONS / "menubar.svg", 18))
    (PNG / "GoDotMenuBarTemplate@2x.png").write_bytes(render(ICONS / "menubar.svg", 36))

    print(f"make-icons: wrote {ICONS.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
