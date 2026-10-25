#!/usr/bin/env bash
# End-to-end check that a command line slice embeds the G-code thumbnails a GUI export writes.
#
# Slices tests/data/20mm_cube.obj with Voron 2.4 350 0.4 nozzle / 0.20mm Standard @Voron / Generic PLA,
# whose "thumbnails" setting is the default 48x48/PNG,300x300/PNG, and checks every
# "; thumbnail begin WxH LEN" block the way Moonraker reads it: one block per configured size, LEN equal
# to the Base64 length, and the decoded data a PNG of WxH that is not fully transparent.
# The CLI renders off screen through a hidden OpenGL window; a Linux host without a display has none
# and the test is skipped there.
#
# usage: test_cli_gcode_thumbnails.sh <orca-slicer binary> <python3> <source dir>
set -u

BIN="${1:-}"
PY="${2:-python3}"
SRC="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$SRC/resources/profiles" ] || { echo "FAIL: source directory not found: $SRC"; exit 1; }
if [ "$(uname -s)" = Linux ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "SKIP: no display, the CLI has no OpenGL context to render thumbnails"
    exit 77
fi

PROFILES="$SRC/resources/profiles"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-gcode-thumbnails.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

TIMEOUT=()
command -v timeout > /dev/null && TIMEOUT=(timeout 300)

${TIMEOUT[@]+"${TIMEOUT[@]}"} "$BIN" --datadir "$WORK/datadir" \
    --load-settings "$PROFILES/Voron/machine/Voron 2.4 350 0.4 nozzle.json;$PROFILES/Voron/process/0.20mm Standard @Voron.json" \
    --load-filaments "$PROFILES/OrcaFilamentLibrary/filament/Generic PLA @System.json" \
    --slice 0 --outputdir "$WORK" "$SRC/tests/data/20mm_cube.obj" > "$WORK/log" 2>&1 \
    || { echo "FAIL: slicer exited $?"; tail -n 40 "$WORK/log"; exit 1; }
[ -s "$WORK/plate_1.gcode" ] || { echo "FAIL: no G-code"; exit 1; }

"$PY" - "$WORK/plate_1.gcode" <<'EOF'
import base64
import re
import struct
import sys
import zlib


def unfiltered_rgba(raw, w, h):
    # Reverses the per-row PNG filters (None, Sub, Up, Average, Paeth) of 8-bit RGBA data.
    stride = 4 * w
    out = bytearray()
    prev = bytearray(stride)
    for row in range(h):
        kind = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        out += line
        prev = line
    return out


gcode = open(sys.argv[1], encoding="utf-8").read()
configured = re.search(r"^; thumbnails = (.*)$", gcode, re.M)
if not configured:
    sys.exit("FAIL: the G-code does not record the thumbnails setting")
expected = [tuple(int(v) for v in re.match(r"\s*(\d+)x(\d+)", item).groups())
            for item in configured.group(1).split(",") if item.strip()]
if not expected:
    sys.exit("FAIL: the shipped preset configures no thumbnails")

blocks = re.findall(r"^; thumbnail begin (\d+)x(\d+) (\d+)\n(.*?)^; thumbnail end$", gcode, re.M | re.S)
found = []
fails = 0
for w, h, length, body in blocks:
    w, h, length = int(w), int(h), int(length)
    found.append((w, h))
    data = "".join(line[2:] for line in body.splitlines() if line.startswith("; "))
    if len(data) != length:
        print(f"FAIL: {w}x{h}: header LEN {length}, Base64 text {len(data)} characters")
        fails += 1
        continue
    png = base64.b64decode(data, validate=True)
    if png[:8] != b"\x89PNG\r\n\x1a\n" or png[12:16] != b"IHDR":
        print(f"FAIL: {w}x{h}: not a PNG")
        fails += 1
        continue
    pw, ph, depth, colour = struct.unpack(">IIBB", png[16:26])
    if (pw, ph) != (w, h):
        print(f"FAIL: {w}x{h}: the PNG is {pw}x{ph}")
        fails += 1
        continue
    # The renderer writes 8-bit RGBA; an all-transparent image means the model was not drawn.
    if (depth, colour) != (8, 6):
        print(f"FAIL: {w}x{h}: expected 8-bit RGBA, got depth {depth} colour type {colour}")
        fails += 1
        continue
    idat = b""
    pos = 8
    while pos < len(png):
        size, kind = struct.unpack(">I4s", png[pos:pos + 8])
        if kind == b"IDAT":
            idat += png[pos + 8:pos + 8 + size]
        pos += 12 + size
    raw = zlib.decompress(idat)
    opaque = sum(1 for alpha in unfiltered_rgba(raw, w, h)[3::4] if alpha != 0)
    if opaque == 0:
        print(f"FAIL: {w}x{h}: every pixel is transparent")
        fails += 1
        continue
    print(f"ok: {w}x{h} PNG, {length} Base64 characters, {opaque} of {w * h} pixels drawn")

if sorted(found) != sorted(expected):
    print(f"FAIL: configured {expected}, found blocks {found}")
    fails += 1
sys.exit(1 if fails else 0)
EOF
