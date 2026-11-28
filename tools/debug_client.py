"""Line-framed Virtual Boy debugger client and generic command CLI."""
from __future__ import annotations
import argparse
import json
import socket
import time


class DebugClient:
    def __init__(self, port=4390, host="127.0.0.1", timeout=None):
        self.socket = socket.create_connection((host, port), timeout)
        if self.socket.getsockname()==self.socket.getpeername():
            self.socket.close()
            raise OSError("loopback self-connection before debugger listener started")
        # Keep partial replies ourselves: buffered socket files become unusable
        # after a read timeout and can lose the already received response prefix.
        self.buffer = bytearray()
        self.pending_request = None
        self.seq = 0

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def command(self, cmd, **args):
        if self.pending_request is not None:
            raise RuntimeError("A debugger response is pending; recover it before sending another command")
        self.seq += 1
        request = {"cmd": cmd, "id": self.seq, **args}
        self.pending_request = {"cmd": cmd, "id": self.seq}
        # The runtime path boundary consumes UTF-8, not literal \uXXXX text.
        self.socket.sendall((json.dumps(request, ensure_ascii=False) + "\n").encode())
        return self._receive()

    def resume(self):
        """Wait for the outstanding reply without ever resending its command."""
        if self.pending_request is None:
            raise RuntimeError("No debugger response is pending")
        self.socket.settimeout(None)
        return self._receive()

    def _receive(self):
        limit = 64 * 1024 * 1024
        end = self.buffer.find(b"\n")
        while end < 0:
            if len(self.buffer) >= limit:
                raise RuntimeError("Debugger response exceeds the framing limit")
            chunk = self.socket.recv(min(65536, limit - len(self.buffer)))
            if not chunk:
                raise RuntimeError("Debugger closed or returned an incomplete response")
            previous = len(self.buffer)
            self.buffer.extend(chunk)
            end = self.buffer.find(b"\n", previous)
        end += 1
        raw = bytes(self.buffer[:end])
        del self.buffer[:end]
        pending = self.pending_request
        self.pending_request = None  # the complete framed reply was consumed
        reply = json.loads(raw)
        if "id" in reply and reply["id"] != pending['id']:
            raise RuntimeError(f"Response ID mismatch: {reply.get('id')} != {pending['id']}")
        if not reply.get("ok"):
            raise RuntimeError(f"{pending['cmd']}: {reply}")
        return reply

    def run_frames(self, frames, timeout=None):
        # timeout is retained for old callers, but is not a workload deadline.
        target = self.command("run_frames", frames=frames)["target"]
        while True:
            frame = self.command("frame")["frame"]
            if frame >= target:
                if frame != target:
                    raise RuntimeError(f"Frame control overshot {target}: {frame}")
                return frame
            time.sleep(.005)


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
