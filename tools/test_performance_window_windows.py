"""Real cross-process exclusion; no game launches or shared campaign locks."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid


@unittest.skipUnless(os.name=='nt','Windows locking contract')
class WindowContract(unittest.TestCase):
    def setUp(self):
        from performance_window_windows import PerformanceWindow
        self.Window=PerformanceWindow
        self.base=Path(os.environ['TEMP']).resolve()
        self.owned=self.base/('performance-lock-fixture-'+uuid.uuid4().hex);self.owned.mkdir()
        self.paths=[self.owned/'central.lock',self.owned/'compat.lock']

    def tearDown(self):
        self.assertTrue(self.owned.resolve().is_relative_to(self.base))
        for p in self.paths:
            if p.exists():p.unlink()
        self.owned.rmdir() # Never recurse, clear attributes or remove unexpected data.

    def blocked_child(self):
        code="""import sys
sys.path.insert(0,sys.argv[1])
from performance_window_windows import PerformanceWindow,PerformanceWindowBusy
w=PerformanceWindow(sys.argv[2:])
try:w.acquire()
except PerformanceWindowBusy:print('blocked',flush=True)
else:w.release();print('unexpected acquisition',flush=True)
sys.stdin.readline()
"""
        return subprocess.Popen([sys.executable,'-c',code,str(Path(__file__).parent),*[str(p) for p in self.paths]],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
            creationflags=subprocess.CREATE_NO_WINDOW)

    def finish(self,child):
        try:
            if child.poll() is None:child.stdin.write('\n');child.stdin.flush()
        finally:
            _,error=child.communicate(timeout=10)
            self.assertEqual(child.returncode,0,error)

    def test_both_protocols_and_partial_cleanup(self):
        for held in (self.paths,self.paths[:1],self.paths[1:]):
            with self.Window(held) as owner:
                owner.acquire();child=self.blocked_child()
                try:
                    self.assertEqual(child.stdout.readline().strip(),'blocked')
                    if held==self.paths[1:]:
                        # Child is still alive: failure on the second file
                        # must already have released its first acquired file.
                        with self.Window(self.paths[:1]) as other:other.acquire()
                finally:self.finish(child)
            with self.Window(self.paths) as reacquired:reacquired.acquire()

    def test_exception_cleanup_preserves_contents(self):
        self.paths[0].write_bytes(b'sentinel')
        original=self.Window(self.paths)
        with self.assertRaises(ValueError):
            with original:original.acquire();raise ValueError('fixture')
        with self.Window(self.paths) as other:other.acquire()
        self.assertEqual(self.paths[0].read_bytes(),b'sentinel')

    def test_exclusive_file_share_owner(self):
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,
            ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p]
        kernel.CreateFileW.restype=ctypes.c_void_p;kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=kernel.CreateFileW(str(self.paths[0]),0xC0000000,0,None,4,0x80,None)
        self.assertNotEqual(handle,ctypes.c_void_p(-1).value)
        try:
            child=self.blocked_child()
            try:self.assertEqual(child.stdout.readline().strip(),'blocked')
            finally:self.finish(child)
        finally:kernel.CloseHandle(handle)
        with self.Window(self.paths) as other:other.acquire()


if __name__=='__main__':unittest.main()
