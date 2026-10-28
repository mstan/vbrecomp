"""Interoperate with the central Windows campaign lock and older VB drivers."""
import msvcrt
from pathlib import Path


class PerformanceWindowBusy(Exception):
    def __init__(self,path):
        self.path=Path(path)
        super().__init__('Shared campaign measurement window occupied: '+str(path))


class PerformanceWindow:
    def __init__(self,paths=None):
        self.paths=tuple(Path(p) for p in (paths if paths is not None else (
            'G:/Temp/Codex/performance-measurement.lock',
            'G:/Temp/Codex/benchmark-window.lock')))
        self.handles=[]

    def __enter__(self):
        return self

    def acquire(self):
        if self.handles:raise RuntimeError('Measurement window already acquired')
        try:
            for path in self.paths:
                try:handle=path.open('a+b')
                except PermissionError as error:raise PerformanceWindowBusy(path) from error
                try:
                    handle.seek(0)
                    try:msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
                    except OSError as error:raise PerformanceWindowBusy(path) from error
                except BaseException:
                    handle.close();raise
                self.handles.append(handle)
                # Windows permits locking a range beyond EOF. Initialize only
                # after ownership, without truncating another driver's file.
                handle.seek(0,2)
                if handle.tell()==0:handle.write(b'\0');handle.flush()
        except BaseException:
            self.release();raise

    def release(self):
        while self.handles:
            handle=self.handles.pop()
            try:
                handle.seek(0)
                try:msvcrt.locking(handle.fileno(),msvcrt.LK_UNLCK,1)
                except OSError:pass # Closing the owning handle releases it too.
            finally:handle.close()

    def __exit__(self,*exception):
        self.release()
