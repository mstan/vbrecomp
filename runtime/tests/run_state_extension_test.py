"""Run the real core serializer with a tiny non-executed synthetic ROM."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='state-extension-',dir=sys.argv[2]) as folder:
    rom=Path(folder)/'fixture.vb'
    rom.write_bytes(bytes(65536))
    raise SystemExit(subprocess.run([sys.argv[1],str(rom),*sys.argv[3:]],cwd=folder).returncode)
