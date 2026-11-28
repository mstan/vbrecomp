"""Focused release and process-lifecycle contracts (no game/window automation)."""
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from debug_client import DebugClient
from release_common import archive_stage, copy_dll_closure, dependency_notices, staging, validate_cache, verify_members
from test_session import SessionTimeout, TestSession

SERVER = r'''
import json,socket,sys,time
port=int(sys.argv[1]);time.sleep(.08)
print('x'*300000,flush=True)
print('y'*300000,file=sys.stderr,flush=True)
with socket.socket() as listener:
 listener.bind(('127.0.0.1',port));listener.listen()
 with listener.accept()[0] as client:
  with client.makefile('rb') as stream:
   for raw in stream:
    request=json.loads(raw);reply={'ok':True,'id':request['id']}
    client.sendall((json.dumps(reply)+'\n').encode())
    if request['cmd']=='quit':break
'''

SLOW_SERVER = r'''
import json,socket,sys,time
port=int(sys.argv[1]);slow_ping=len(sys.argv)>2
with socket.socket() as listener:
 listener.bind(('127.0.0.1',port));listener.listen()
 with listener.accept()[0] as client:
  with client.makefile('rb') as stream:
   for raw in stream:
    request=json.loads(raw);print(request['cmd'],request['id'],flush=True)
    reply=(json.dumps({'ok':True,'id':request['id'],'effect':request['cmd']})+'\n').encode()
    if request['cmd']=='mutate' or slow_ping:
     slow_ping=False;client.sendall(reply[:7]);time.sleep(.12);client.sendall(reply[7:])
    else:client.sendall(reply)
    if request['cmd']=='quit':break
'''


class SharedToolsTests(unittest.TestCase):
    def test_cache_exact_flags_and_paths(self):
        root=Path(tempfile.gettempdir()).resolve()
        cache=f'// Build configuration\n\nCMAKE_BUILD_TYPE:STRING=Release\n\n// UI\nUI:BOOL=ON\nCMAKE_HOME_DIRECTORY:INTERNAL={root}\n'
        validate_cache(cache,[('UI','ON')],[('CMAKE_HOME_DIRECTORY',root)])
        for bad in (cache.replace('=ON','=ON-bad'),cache.replace('=Release','=Debug')):
            with self.assertRaises(SystemExit):validate_cache(bad,[('UI','ON')])
        with self.assertRaises(SystemExit):validate_cache(cache,paths=[('CMAKE_HOME_DIRECTORY',root/'other')])

    def test_package_duplicates_and_archive_inventory(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);source=root/'one';source.write_bytes(b'original')
            package=root/'test.vbmod'
            with zipfile.ZipFile(package,'w') as z:z.writestr('one',b'original')
            verify_members(package,{'one':source})
            with zipfile.ZipFile(package,'a') as z:z.writestr('extra',b'unknown')
            with self.assertRaises(SystemExit):verify_members(package,{'one':source})
            with staging(root,'stage-') as stage:
                staged=stage; (stage/'payload.txt').write_bytes(b'original')
                first=root/'first.zip';second=root/'second.zip'
                inventory=archive_stage(stage,first);archive_stage(stage,second)
                self.assertEqual(first.read_bytes(),second.read_bytes())
                self.assertEqual([r['path'] for r in inventory],['payload.txt'])
                (stage/'owner.sav').write_bytes(b'secret')
                with self.assertRaises(SystemExit):archive_stage(stage,root/'bad.zip')
            self.assertFalse(staged.exists())

    def test_recursive_dll_closure_and_missing_import(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);toolchain=root/'tools';toolchain.mkdir();stage=root/'stage';stage.mkdir()
            exe=root/'game.exe';exe.write_bytes(b'exe')
            (toolchain/'first.dll').write_bytes(b'first');(toolchain/'second.dll').write_bytes(b'second')
            def imports(command,**_):
                return {'game.exe':'DLL Name: first.dll\nDLL Name: api-ms-system.dll',
                        'first.dll':'DLL Name: second.dll','second.dll':'DLL Name: first.dll'}[Path(command[-1]).name]
            with patch('release_common.subprocess.check_output',side_effect=imports):
                self.assertEqual(set(copy_dll_closure(exe,stage,toolchain)),{'first.dll','second.dll'})
            with patch('release_common.subprocess.check_output',return_value='DLL Name: absent.dll'):
                with self.assertRaises(SystemExit):copy_dll_closure(exe,stage,toolchain)

    def test_session_logs_close_and_occupied_port(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with socket.socket() as port_socket:
                port_socket.bind(('127.0.0.1',0));port=port_socket.getsockname()[1]
                with self.assertRaises(OSError):TestSession([sys.executable,'-c','raise Exception()'],root/'occupied',port)
            with TestSession([sys.executable,'-u','-c',SERVER,str(port)],root/'session',port) as session:
                self.assertTrue(session.client.command('ping')['ok'])
                self.assertIsNone(session.process.poll())
            self.assertEqual(session.process.returncode,0)
            self.assertGreater((root/'session/stdout.log').stat().st_size,300000)
            self.assertGreater((root/'session/stderr.log').stat().st_size,300000)

    def test_frame_workload_ignores_legacy_deadline(self):
        client=object.__new__(DebugClient);frames=iter([1,2,3,4])
        client.command=lambda cmd,**_: {'target':4} if cmd=='run_frames' else {'frame':next(frames)}
        self.assertEqual(client.run_frames(4,timeout=0),4)

    def test_deferred_attachment_and_startup_exit(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
            with TestSession([sys.executable,'-u','-c',SERVER,str(port)],root/'deferred',port,wait=False) as session:
                self.assertIsNone(session.client)
                self.assertTrue(session.attach().command('ping')['ok'])
            with self.assertRaises(RuntimeError):
                TestSession([sys.executable,'-c','raise SystemExit(7)'],root/'failed',port)
            self.assertEqual(json.loads((root/'failed/crash.json').read_text())['exit'],7)

    def test_timeout_preserves_session_and_pending_nonidempotent_reply(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
            session=TestSession([sys.executable,'-u','-c',SLOW_SERVER,str(port)],root,port)
            try:
                with self.assertRaises(SessionTimeout) as caught:
                    with session:
                        session.client.socket.settimeout(.01)
                        session.client.command('mutate')
                self.assertIs(caught.exception.session,session)
                self.assertIsNone(session.process.poll())
                self.assertEqual(bytes(session.client.buffer),b'{"ok": ')
                with self.assertRaises(RuntimeError):session.client.command('mutate')
                self.assertEqual(session.recover()['effect'],'mutate')
                self.assertEqual(session.client.command('ping')['id'],3)
                self.assertEqual((root/'stdout.log').read_text().splitlines().count('mutate 2'),1)
                self.assertTrue(json.loads((root/'transport-timeout.json').read_text())['running'])
                session.close()
                self.assertEqual(session.process.returncode,0)
            finally:
                if session.process.poll() is None:session.stop()

    def test_constructor_timeout_retains_recoverable_owned_process(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
            session=None
            try:
                with patch('test_session.DebugClient',side_effect=lambda port:DebugClient(port,timeout=.01)):
                    with self.assertRaises(SessionTimeout) as caught:
                        TestSession([sys.executable,'-u','-c',SLOW_SERVER,str(port),'slow-ping'],root,port)
                session=caught.exception.session
                self.assertIsNone(session.process.poll())
                self.assertEqual(session.recover()['id'],1)
                self.assertEqual((root/'stdout.log').read_text().splitlines(),['ping 1'])
                session.close()
                self.assertEqual(session.process.returncode,0)
            finally:
                if session is not None and session.process.poll() is None:session.stop()

    def test_dependency_inventory_keeps_nested_notices(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);dependency=root/'dep';dependency.mkdir();(dependency/'.git').mkdir()
            (dependency/'LICENSE').write_text('aggregate notice')
            (dependency/'licenses').mkdir();(dependency/'licenses/donor-MIT.txt').write_text('donor notice')
            stage=root/'stage';stage.mkdir()
            def git(tree,*command,**_):
                if tree==root:return '160000 commit abc\tdep'
                if command==('rev-parse','HEAD'):return 'abc'
                if command==('ls-files','LICENSES','licenses'):return 'licenses/donor-MIT.txt'
                return ''
            with patch('release_common.git',side_effect=git):inventory=dependency_notices(root,stage)
            self.assertEqual((stage/'licenses/dependencies/dep/licenses/donor-MIT.txt').read_text(),'donor notice')
            self.assertIn('licenses/donor-MIT.txt',inventory[0]['notices'])


if __name__=='__main__':unittest.main()
