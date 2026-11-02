#!/usr/bin/env python3
"""Check installed/extracted Debian launcher icons through GTK's real resolver."""
import argparse
import configparser
from pathlib import Path
import shutil
import struct
import tempfile
import zlib

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path("/"), help="installed filesystem or dpkg-deb extraction root")
args = parser.parse_args()
share = args.root / "usr/share"
entry = configparser.ConfigParser(interpolation=None)
entry.read(share / "applications/pacificdb-workbench.desktop")
assert entry["Desktop Entry"]["Icon"] == "pacificdb-workbench"
assert entry["Desktop Entry"]["StartupWMClass"] == "pacificdb-workbench"

sizes = (16, 32, 48, 64, 128, 256, 512)
for size in sizes:
    filename = share / f"icons/hicolor/{size}x{size}/apps/pacificdb-workbench.png"
    raw = filename.read_bytes()
    assert raw[:8] == b"\x89PNG\r\n\x1a\n", filename
    assert struct.unpack(">II", raw[16:24]) == (size, size), filename
    position = 8
    while position < len(raw):
        length = struct.unpack(">I", raw[position:position + 4])[0]
        chunk = raw[position + 4:position + length + 8]
        crc = struct.unpack(">I", raw[position + length + 8:position + length + 12])[0]
        assert zlib.crc32(chunk) == crc, filename
        assert chunk[:4] not in (b"tEXt", b"zTXt", b"iTXt", b"eXIf"), filename
        position += length + 12
    assert position == len(raw), filename

# Resolve only this package's icons, with the standard system theme definition.
# This also catches non-standard size directories that a file-existence check
# misses. No installed icon files or user caches are modified.
with tempfile.TemporaryDirectory(prefix="pacificdb-icon-test-") as temporary:
    icons = Path(temporary)
    for size in sizes:
        relative = Path(f"hicolor/{size}x{size}/apps/pacificdb-workbench.png")
        (icons / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(share / "icons" / relative, icons / relative)
    shutil.copyfile("/usr/share/icons/hicolor/index.theme", icons / "hicolor/index.theme")
    theme = Gtk.IconTheme.new()
    theme.set_custom_theme("hicolor")
    theme.set_search_path([str(icons)])
    for size in (16, 24, 32, 48, 64, 128, 256, 512):
        match = theme.lookup_icon("pacificdb-workbench", size, Gtk.IconLookupFlags.FORCE_SIZE)
        assert match is not None, f"launcher cannot resolve {size}px icon"
        assert Path(match.get_filename()).resolve().is_relative_to(icons), match.get_filename()
        print(f"LAUNCHER_ICON_PASS {size}px")
