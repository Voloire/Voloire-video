"""Builds assets/icon.ico from the PNGs the app renders itself (voloire-video --export-icons)."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
exe = Path(sys.argv[1]) if len(sys.argv) > 1 else repo / "build" / "bin" / "voloire-video.exe"
with tempfile.TemporaryDirectory() as d:
    subprocess.run([str(exe), "--export-icons", d], check=True)
    pngs = [(s, (Path(d) / f"icon_{s}.png").read_bytes()) for s in (16, 24, 32, 48, 64, 128, 256)]
out = bytearray(struct.pack("<HHH", 0, 1, len(pngs)))
offset = 6 + 16 * len(pngs)
for size, data in pngs:
    out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
for _, data in pngs:
    out += data
(repo / "assets" / "icon.ico").write_bytes(out)
print(f"wrote assets/icon.ico ({len(out)} bytes)")
