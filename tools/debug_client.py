"""Line-framed Virtual Boy debugger client and generic command CLI."""
from __future__ import annotations
import argparse
import json
import socket
import time


class DebugClient:
    def __init__(self, port=4390, host="127.0.0.1", timeout=15):
        self.socket = socket.create_connection((host, port), timeout)
        if self.socket.getsockname()==self.socket.getpeername():
            self.socket.close()
            raise OSError("loopback self-connection before debugger listener started")
        self.file = self.socket.makefile("rb")
        self.seq = 0

    def close(self):
        self.file.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def command(self, cmd, **args):
        self.seq += 1
        request = {"cmd": cmd, "id": self.seq, **args}
        self.socket.sendall((json.dumps(request) + "\n").encode())
        raw = self.file.readline(64 * 1024 * 1024)
        if not raw.endswith(b"\n"):
            raise RuntimeError("Debugger closed or returned an incomplete response")
        reply = json.loads(raw)
        if "id" in reply and reply["id"] != self.seq:
            raise RuntimeError(f"Response ID mismatch: {reply.get('id')} != {self.seq}")
        if not reply.get("ok"):
            raise RuntimeError(f"{cmd}: {reply}")
        return reply

    def run_frames(self, frames, timeout=30):
        target = self.command("run_frames", frames=frames)["target"]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.command("frame")["frame"]
            if frame >= target:
                if frame != target:
                    raise RuntimeError(f"Frame control overshot {target}: {frame}")
                return frame
            time.sleep(.005)
        raise TimeoutError(f"Did not reach frame {target}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4390)
    parser.add_argument("command")
    parser.add_argument("args", nargs="*", help="key=value arguments; numbers accept 0x prefixes")
    args = parser.parse_args()
    fields = {}
    for field in args.args:
        key, value = field.split("=", 1)
        try:
            value = int(value, 0)
        except ValueError:
            pass
        fields[key] = value
    with DebugClient(args.port, args.host) as client:
        print(json.dumps(client.command(args.command, **fields), indent=2))


if __name__ == "__main__":
    main()
