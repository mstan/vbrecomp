"""Release mechanics; game wrappers retain payload, defaults and license policy."""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(path, *command, executable='git'):
    return subprocess.check_output([str(executable), '-C', str(path), *command], text=True).strip()


def validate_cache(cache, flags=(), paths=()):
    entries = dict(re.findall(r'^([^#/\s:][^:\r\n=]*):[^=\r\n]+=(.*)$', cache, re.MULTILINE))
    for key, expected in [('CMAKE_BUILD_TYPE', 'Release'), *flags]:
        if entries.get(key) != expected:
            raise SystemExit(f'Expected release configuration {key}={expected}')
    for key, expected in paths:
        if key not in entries or Path(entries[key]).resolve() != Path(expected).resolve():
            raise SystemExit(f'Build cache does not match source directory: {key}')
    return entries


def validate_sources(root, dependencies, executable='git'):
    roots = [Path(root), *(Path(path) for _, path in dependencies)]
    for tree in roots:
        if git(tree, 'status', '--porcelain', executable=executable):
            raise SystemExit(f'Commit source changes before packaging: {tree}')
    for name, tree in dependencies:
        if git(root, 'rev-parse', f'HEAD:{name}', executable=executable) != git(tree, 'rev-parse', 'HEAD', executable=executable):
            raise SystemExit(f'Build dependency does not match committed pin: {name}')


def verify_members(package, expected):
    """Reject duplicates, unknown members and any bytes differing from source."""
    with zipfile.ZipFile(package) as archive:
        names = archive.namelist()
        if len(names) != len(expected) or set(names) != set(expected) or any(
            archive.read(name) != Path(source).read_bytes() for name, source in expected.items()
        ):
            raise SystemExit(f'Rebuild package: contents differ from source: {package}')


@contextmanager
def staging(output, prefix):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=prefix, dir=output) as temporary:
        stage = Path(temporary).resolve()
        if stage == output or not stage.is_relative_to(output):
            raise SystemExit('Unexpected release staging directory')
        yield stage


def copy_dll_closure(exe, stage, toolchain, extra_roots=()):
    exe, stage, toolchain = map(Path, (exe, stage, toolchain))
    queue, copied = [exe], set()
    while queue:
        binary = queue.pop()
        imports = subprocess.check_output([str(toolchain / 'objdump.exe'), '-p', str(binary)], text=True)
        for name in re.findall(r'DLL Name:\s*(\S+)', imports):
            key = name.lower()
            if key in copied:
                continue
            # An import is a basename, never a path supplied to the packager.
            if Path(name).name != name or '/' in name or '\\' in name:
                raise SystemExit(f'Invalid dependency name: {name}')
            candidate = next((Path(folder) / name for folder in (exe.parent, *extra_roots, toolchain)
                              if (Path(folder) / name).is_file()), None)
            if candidate:
                copied.add(key)
                shutil.copy2(candidate, stage / name)
                queue.append(candidate)
            elif not (Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32' / name).is_file() and not key.startswith(('api-ms-', 'ext-ms-')):
                raise SystemExit(f'Unresolved dependency: {name}')
    return {path.name: sha(path) for path in sorted(stage.glob('*.dll'))}


def dependency_notices(framework, stage, executable='git'):
    """Inventory exact registered dependencies and ship their license notices."""
    framework, stage = Path(framework), Path(stage)
    licenses = stage / 'licenses'
    licenses.mkdir(exist_ok=True)
    inventory = []
    def visit(root, prefix=''):
        rows = git(root, 'ls-tree', '-r', 'HEAD', executable=executable).splitlines()
        for row in rows:
            metadata, relative = row.split('\t', 1)
            mode, _, commit = metadata.split()
            if mode != '160000':
                continue
            tree = root / relative
            if not (tree / '.git').exists():
                raise SystemExit(f'Initialize release dependency: {tree}')
            if git(tree, 'rev-parse', 'HEAD', executable=executable) != commit or git(tree, 'status', '--porcelain', executable=executable):
                raise SystemExit(f'Release dependency is dirty or differs from pin: {tree}')
            name = prefix + relative
            notices = [p for p in sorted(tree.iterdir()) if p.is_file() and
                       p.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE'))]
            if not notices:
                raise SystemExit(f'Missing dependency license notice: {name}')
            destination = licenses / 'dependencies' / name
            destination.mkdir(parents=True, exist_ok=True)
            for notice in notices:
                shutil.copy2(notice, destination / notice.name)
            notice_hashes = {p.name: sha(p) for p in notices}
            # Aggregate grants retain donor/third-party notices in these
            # tracked directories; a top-level LICENSE alone is insufficient.
            for relative in git(tree, 'ls-files', 'LICENSES', 'licenses', executable=executable).splitlines():
                notice = tree / relative
                if not notice.is_file() or notice.is_symlink():
                    raise SystemExit(f'Invalid dependency notice: {notice}')
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(notice, target)
                notice_hashes[relative] = sha(notice)
            inventory.append({'path': name, 'commit': commit,
                              'notices': notice_hashes})
            visit(tree, name + '/')
    visit(framework)
    (stage / 'dependency-sources.json').write_text(json.dumps(inventory, indent=2) + '\n')
    return inventory


def archive_stage(stage, archive, forbidden=('.vb', '.vboy', '.cfg', '.sav', '.src')):
    stage, archive = Path(stage).resolve(), Path(archive).resolve()
    inventory = []
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for source in sorted(stage.rglob('*')):
            if source.is_symlink():
                raise SystemExit(f'Unexpected release symlink: {source}')
            if not source.is_file():
                continue
            if source.suffix.lower() in forbidden:
                raise SystemExit(f'Unexpected private/profile data in release: {source.name}')
            name = source.relative_to(stage).as_posix()
            entry = zipfile.ZipInfo(name, (2026, 9, 12, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(entry, source.read_bytes())
            inventory.append({'path': name, 'bytes': source.stat().st_size, 'sha256': sha(source)})
    # Reopen and verify the final archive, not only the staging tree.
    with zipfile.ZipFile(archive) as check:
        if check.namelist() != [row['path'] for row in inventory] or check.testzip():
            raise SystemExit('Release archive inventory/integrity mismatch')
        for row in inventory:
            if hashlib.sha256(check.read(row['path'])).hexdigest() != row['sha256']:
                raise SystemExit(f'Release archive hash mismatch: {row["path"]}')
    archive.with_suffix('.contents.json').write_text(json.dumps(inventory, indent=2) + '\n')
    return inventory
