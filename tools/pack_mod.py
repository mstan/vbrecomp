"""Build a reproducible .vbmod ZIP from a mod source directory (no ROM patching)."""
from pathlib import Path
import argparse
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = args.source.resolve(strict=True)
    output = args.output.resolve()
    if not (root / "manifest.toml").is_file():
        parser.error("source needs a root manifest.toml")
    if output.suffix != ".vbmod":
        parser.error("output must end in .vbmod")
    if output.is_relative_to(root):
        parser.error("output must be outside the source directory")
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            parser.error(f"symlinks are not package assets: {path}")
        if path.is_file():
            entries.append(path)
    if len(entries) > 4096 or sum(p.stat().st_size for p in entries) > 256 * 1024 * 1024:
        parser.error("package exceeds runtime limits")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in entries:
            info = zipfile.ZipInfo(path.relative_to(root).as_posix(), (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes())
    print(output)


if __name__ == "__main__":
    main()
