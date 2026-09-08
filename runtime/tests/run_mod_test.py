"""Generate owner-free package fixtures and exercise the compiled runtime."""
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

root = Path(tempfile.mkdtemp(prefix="vb-mod-test-", dir=sys.argv[2]))
(root / "rom.vb").write_bytes(b"abc")
(root / "wrong.vb").write_bytes(b"wrong")
for name in ("color", "conflict", "wrong-target"):
    target = "different.game" if name == "wrong-target" else "test.game"
    plugin = "test.alternate" if name == "conflict" else "test.color"
    manifest = f'''format_version = 1
id = "{name}"
version = "1.0.0"
name = "Color fixture"
author = "Tests"
license = "MIT"
[[target]]
game_id = "{target}"
rom_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
[[feature]]
id = "color"
name = "Color"
default_enabled = false
[[plugin]]
feature = "color"
id = "{plugin}"
[[option]]
feature = "color"
id = "strength"
label = "Strength"
type = "integer"
default = 50
min = 0
max = 100
step = 1
'''
    with zipfile.ZipFile(root / f"{name}.vbmod", "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.toml", manifest)
        archive.writestr("palette.txt", "fixture asset")
with zipfile.ZipFile(root / "traversal.vbmod", "w") as archive:
    archive.writestr("../escaped.txt", "invalid")
    archive.writestr("manifest.toml", manifest)
for name, entry in [("device", "CON.txt"), ("ads", "palette.txt:stream"),
                    ("trailing-dot", "directory./file"), ("absolute", "C:/escape.txt")]:
    with zipfile.ZipFile(root / f"{name}.vbmod", "w") as archive:
        archive.writestr("manifest.toml", manifest)
        archive.writestr(entry, "invalid")
with zipfile.ZipFile(root / "duplicate.vbmod", "w") as archive:
    archive.writestr("manifest.toml", manifest)
    archive.writestr("Palette.txt", "one")
    archive.writestr("palette.txt", "two")
with zipfile.ZipFile(root / "version-escape.vbmod", "w") as archive:
    archive.writestr("manifest.toml", manifest.replace('version = "1.0.0"', 'version = "../escape"'))
bad_crc = bytearray((root / "color.vbmod").read_bytes())
central = bad_crc.index(b"PK\x01\x02")
bad_crc[central + 16] ^= 1
(root / "bad-crc.vbmod").write_bytes(bad_crc)
subprocess.run([sys.argv[1], str(root)], check=True)
print("Package lifecycle, persistence, conflicts, identity, options, ZIP and ROM preservation passed")
