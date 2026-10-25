#!/usr/bin/env python3
"""Render the raster app icons from resources/images/AnanasSlicer.svg.

The SVG files resources/images/AnanasSlicer*.svg are the source of the logo; the
app renders them itself (nanosvg). Only the raster icons the operating systems
need are generated here:

  resources/images/AnanasSlicer_<N>px.png   window and Linux icons (32..512 px)
  resources/images/AnanasSlicer-mac_256px.png  macOS dock icon (macOS icon grid)
  resources/images/AnanasSlicer.ico         Windows icon (16..256 px)
  resources/AnanasSlicer.icns               macOS bundle icon (macOS only, iconutil)

The macOS icons place the tile on Apple's icon grid: 824 of 1024 units with a
transparent margin, so the icon has the same size as other apps in the Dock.

To replace the logo, replace the AnanasSlicer*.svg files and run this script.

usage: scripts/brand/build_icons.py
requires: Python 3 with cairosvg and Pillow (pip install cairosvg pillow;
          cairosvg needs the cairo library, e.g. brew install cairo)
"""

import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

import cairosvg
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
IMAGES = os.path.join(REPO, "resources", "images")
SOURCE = os.path.join(IMAGES, "AnanasSlicer.svg")
PNG_SIZES = (32, 128, 192, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
ICNS_SIZES = (16, 32, 128, 256, 512)


def render(svg, size):
    png = cairosvg.svg2png(bytestring=svg, output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def mac_grid(svg):
    """The icon scaled onto Apple's 824/1024 grid, centred with a transparent margin."""
    text = svg.decode("utf-8")
    match = re.search(r"<svg[^>]*>(.*)</svg>", text, re.S)
    if match is None:
        sys.exit("cannot read the <svg> element of %s" % SOURCE)
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">'
            '<g transform="translate(100 100) scale(0.8046875)">%s</g></svg>' % match.group(1)).encode("utf-8")


def main():
    with open(SOURCE, "rb") as f:
        svg = f.read()
    mac = mac_grid(svg)

    for size in PNG_SIZES:
        render(svg, size).save(os.path.join(IMAGES, "AnanasSlicer_%dpx.png" % size), optimize=True)
    render(mac, 256).save(os.path.join(IMAGES, "AnanasSlicer-mac_256px.png"), optimize=True)

    largest = render(svg, max(ICO_SIZES))
    largest.save(os.path.join(IMAGES, "AnanasSlicer.ico"), sizes=[(s, s) for s in ICO_SIZES])

    if shutil.which("iconutil") is None:
        print("iconutil not found (macOS only): resources/AnanasSlicer.icns left unchanged")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            iconset = os.path.join(tmp, "AnanasSlicer.iconset")
            os.mkdir(iconset)
            for size in ICNS_SIZES:
                render(mac, size).save(os.path.join(iconset, "icon_%dx%d.png" % (size, size)))
                render(mac, size * 2).save(os.path.join(iconset, "icon_%dx%d@2x.png" % (size, size)))
            subprocess.run(["iconutil", "-c", "icns", iconset, "-o",
                            os.path.join(REPO, "resources", "AnanasSlicer.icns")], check=True)
    print("icons written from %s" % os.path.relpath(SOURCE, REPO))


if __name__ == "__main__":
    main()
