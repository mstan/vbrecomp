"""Reject malformed finite workloads before a ROM or device is initialized."""
import pathlib
import subprocess
import sys
import tempfile

exe = str(pathlib.Path(sys.argv[1]).resolve())

def rejected(args, message):
    result = subprocess.run([exe, *args], capture_output=True, text=True, timeout=10)
    assert result.returncode == 2, (args, result.returncode, result.stderr)
    assert message in result.stderr, (args, result.stderr)

for value in ("0", "-1", "1000001", "1garbage", ""):
    rejected(["--benchmark", value, "--rom", "missing.vb"], "requires 1..1000000")
rejected(["--benchmark", "1"], "requires --rom")
rejected(["--benchmark", "1", "--rom", "missing.vb", "--paused"], "cannot use --paused")
with tempfile.TemporaryDirectory() as directory:
    route = pathlib.Path(directory) / "route.txt"
    for content in ("", "0 0\n", "1 65536\n", "2 0\n", "1 0 extra\n", "1\n", "-1 0\n", "4294967297 0\n", "1 4294967296\n"):
        route.write_text(content)
        rejected(["--benchmark", "1", "--rom", "missing.vb", "--benchmark-route", str(route)],
                 "must contain positive counts")
    rejected(["--benchmark-route", str(route)], "requires --benchmark")
print("finite benchmark argument and route rejection passed")
