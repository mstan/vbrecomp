"""Exercise the production recorder and PNG writer without a game or display."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import wave
import zlib

binary = str(Path(sys.argv[1]).resolve())


def run(root, name, frames="frames", wav="audio.wav", script="-", expected=0):
    folder = root / name
    folder.mkdir(exist_ok=True)
    result = subprocess.run([binary, frames, wav, script], cwd=folder,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == expected, (name, result.returncode, result.stdout)
    if expected:
        assert b"capture: cannot" in result.stdout, (name, result.stdout)
    return folder


def pcm(path):
    with wave.open(str(path), "rb") as stream:
        assert (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) == (2, 2, 44100)
        return struct.unpack("<6h", stream.readframes(stream.getnframes()))


def pixels(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", data[16:24]) == (4, 2)
    offset, compressed = 8, bytearray()
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        if data[offset + 4:offset + 8] == b"IDAT":
            compressed.extend(data[offset + 8:offset + 8 + length])
        offset += 12 + length
    return zlib.decompress(compressed)


with tempfile.TemporaryDirectory(prefix="headless-capture-", dir=sys.argv[2]) as temp:
    root = Path(temp)
    both = run(root, "sparse")
    assert pcm(both / "audio.wav") == (2, 102, 3, 103, 4, 104)
    assert [p.name for p in sorted((both / "frames").glob("*.png"))] == ["frame_000002.png", "frame_000004.png"]
    expected_row = b"\0" + bytes.fromhex("102030ff102030ff405060ff405060ff")
    assert pixels(both / "frames/frame_000002.png") == expected_row * 2
    audio = run(root, "audio-only", frames="-")
    assert pcm(audio / "audio.wav") == pcm(both / "audio.wav")
    assert not (audio / "frames").exists()
    run(root, "missing-input", script="absent.input", expected=2)
    run(root, "bad-wav", wav="absent/audio.wav", expected=2)
    blocked = root / "bad-png/frames/frame_000002.png"
    blocked.mkdir(parents=True)
    run(root, "bad-png", expected=2)
    assert blocked.is_dir()
    if os.name != "nt":
        run(root, "full-wav", wav="/dev/full", expected=2)
        full_png = root / "full-png/frames"
        full_png.mkdir(parents=True)
        (full_png / "frame_000002.png").symlink_to("/dev/full")
        run(root, "full-png", expected=2)
print("PASS: sparse PNGs, continuous ranged PCM, audio-only, and input/output failures")
