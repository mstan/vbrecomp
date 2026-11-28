"""Owned, isolated test-process lifecycle shared by VB qualification scripts.

The caller chooses a finite workload and explicitly closes its own process.
Startup and shutdown deliberately have no elapsed-time kill deadlines.
This module does not deliver desktop/window input.
"""
from pathlib import Path
import json
import os
import socket
import subprocess
import time

from debug_client import DebugClient


class SessionTimeout(TimeoutError):
    """A reported transport timeout that retains the live owned session.

    Call session.recover() to receive its pending reply, or session.stop() for
    an explicit cancellation. Neither a command nor its effects are replayed.
    """
    def __init__(self, session, error):
        super().__init__(f'Transport timed out; process remains owned and running: {session.directory}: {error}')
        self.session = session


class TestSession:
    def __init__(self, command, directory, port, environment=None, hidden=True, wait=True):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.client = None
        self.process = None
        self.port = port
        # Never attach to another test or the owner's running game.
        if port is not None:
            with socket.socket() as probe:
                if os.name == 'nt':
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                probe.bind(('127.0.0.1', port))
        self.stdout_log = (self.directory / 'stdout.log').open('wb')
        self.stderr_log = (self.directory / 'stderr.log').open('wb')
        options = {}
        if os.name == 'nt' and hidden:
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            options.update(startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.process = subprocess.Popen(command, cwd=self.directory,
                env=environment, stdout=self.stdout_log, stderr=self.stderr_log, **options)
            if wait and port is not None:
                self.attach()
        except TimeoutError as error:
            if self.process is not None and self.process.poll() is None:
                raise self._timeout(error) from error
            self.close()
            raise
        except BaseException:
            self.close()
            raise

    def attach(self):
        """Attach after all peers are launched when session startup is coupled."""
        while self.client is None:
            if self.process.poll() is not None:
                raise RuntimeError(f'Test process exited during startup: {self.process.returncode}; see {self.directory}')
            try:
                self.client = DebugClient(self.port)
            except OSError:
                time.sleep(.02)
        self.client.command('ping')
        return self.client

    def _timeout(self, error):
        (self.directory / 'transport-timeout.json').write_text(json.dumps({
            'pid': self.process.pid, 'running': self.process.poll() is None,
            'pending_request': self.client.pending_request if self.client else None,
            'error': str(error), 'recovery': 'recover the pending response or explicitly stop this owned session'}, indent=2))
        return SessionTimeout(self, error)

    def recover(self):
        if self.client is None:
            raise RuntimeError('No pending debugger transport; the owned process is unchanged')
        return self.client.resume()

    def detach(self):
        """Explicitly transfer a prepared interactive process to its caller."""
        if self.client is not None:
            self.client.close()
            self.client = None
        process, self.process = self.process, None
        self.stdout_log.close()
        self.stderr_log.close()
        return process

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is not None and issubclass(kind, TimeoutError) and self.process.poll() is None:
            raise self._timeout(value) from value
        self.close()
        if kind is None and self.process.returncode:
            raise RuntimeError(f'Test process exit {self.process.returncode}; see {self.directory / "crash.json"}')

    def close(self):
        if self.client is not None:
            try:
                # Normal completed-workload cleanup is graceful and has no
                # transport timer. Drain a pending response, never resend it.
                self.client.socket.settimeout(None)
                if self.client.pending_request is not None:
                    self.client.resume()
                self.client.command('quit')
            except TimeoutError as error:
                raise self._timeout(error) from error
            except (OSError, RuntimeError):
                # Explicit cleanup with an unusable transport, not a timer.
                if self.process.poll() is None:
                    self.process.terminate()
            self.client.close()
            self.client = None
        elif self.process is not None and self.process.poll() is None:
            self.process.terminate()  # explicit cancellation during startup
        self._finish()

    def stop(self):
        """Explicit cancellation, including a caller choosing to stop a wait."""
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
        if self.client is not None:
            self.client.close()
            self.client = None
        self._finish()

    def _finish(self):
        if self.process is not None:
            self.process.wait()
        self.stdout_log.close()
        self.stderr_log.close()
        if self.process is not None and self.process.returncode:
            error = (self.directory / 'stderr.log').read_bytes().decode(errors='replace')
            (self.directory / 'crash.json').write_text(json.dumps({
                'exit': self.process.returncode, 'report': error[-4000:],
                'stdout': 'stdout.log', 'stderr': 'stderr.log'}, indent=2))
