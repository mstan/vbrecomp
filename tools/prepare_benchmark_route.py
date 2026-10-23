"""Convert an existing JSON controller route to the finite runtime format.

This preserves guest frame counts and pad masks, ignores descriptive labels,
and does not synthesize input or write guest memory. Run the resulting route
with a cartridge-linked production runtime, not the generic interpreter host.
"""
import argparse
import json
from pathlib import Path


def convert_route(payload, expected_frames=None):
    if not isinstance(payload, list) or not payload:
        raise ValueError("route must be a nonempty list")
    rows, total = [], 0
    for segment in payload:
        if not isinstance(segment, dict):
            raise ValueError("each segment must contain frames and pad")
        frames, pad = segment.get("frames"), segment.get("pad")
        if type(frames) is not int or not 1 <= frames <= 1000000:
            raise ValueError("frames must be an integer in 1..1000000")
        if type(pad) is not int or not 0 <= pad <= 65535:
            raise ValueError("pad must be an integer in 0..65535")
        total += frames
        if total > 1000000:
            raise ValueError("total frames exceeds 1000000")
        rows.append(f"{frames} {pad}\n")
    if expected_frames is not None and total != expected_frames:
        raise ValueError(f"route has {total} frames, expected {expected_frames}")
    return "".join(rows), total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expected-frames", type=int)
    args = parser.parse_args()
    try:
        text, frames = convert_route(json.loads(args.source.read_text(encoding="utf-8-sig")),
                                     args.expected_frames)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_text(text, encoding="ascii")
    print(f"prepared {frames} guest frames from {args.source}")


if __name__ == "__main__":
    main()
