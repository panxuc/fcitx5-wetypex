"""Verified runtime generations, legacy migration and reversible activation."""
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid
import zipfile

from runtime_catalog import DOWNLOAD_HOSTS, validate_url

ASSETS = ('fcitx5-wetypex/ui', 'fcitx5-wetypex/voice/vad',
          'fcitx5/themes/wetypex-light', 'fcitx5/themes/wetypex-dark',
          'fonts/fcitx5-wetypex-wechat.ttf')
MANAGED_GENERATION = r'(?:[0-9]+(?:\.[0-9]+)+-(?:x86_64|arm64)-[0-9a-f]{12}|legacy-[0-9a-f]{32})'


def generation_name(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,160}', value):
        raise ValueError('invalid runtime generation name')
    return value


def digest(path, algorithm='sha256'):
    value = hashlib.new(algorithm)
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def read_json(path):
    try:
        if Path(path).stat().st_size > 1024 * 1024:
            raise ValueError('runtime metadata is too large')
        value = json.loads(Path(path).read_text())
        return value if isinstance(value, dict) else {}
    except FileNotFoundError:
        return {}


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def sync_tree(path):
    for directory, _children, files in os.walk(path, followlinks=False):
        for name in files:
            file=Path(directory)/name
            if file.is_symlink() or not file.is_file():
                continue
            fd=os.open(file,os.O_RDONLY|os.O_CLOEXEC)
            try:os.fsync(fd)
            finally:os.close(fd)
    for directory, _children, _files in os.walk(path,topdown=False,followlinks=False):
        sync_directory(directory)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def copy_private(source, target):
    source, target = Path(source), Path(target)
    if source.is_dir():
        def skip(directory, names):
            return [n for n in names if n.endswith(('.lock', '.sock')) or
                    not (Path(directory, n).is_dir() or
                         Path(directory, n).is_file() or Path(directory, n).is_symlink())]
        shutil.copytree(source, target, symlinks=True, ignore=skip, copy_function=clone_private)
        target.chmod(0o700)
    elif source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        clone_private(source, target)


def clone_private(source, target):
    """Use copy-on-write where supported; mutable user files never share inodes."""
    try:
        with open(source, 'rb') as src, open(target, 'wb') as dst:
            fcntl.ioctl(dst.fileno(), 0x40049409, src.fileno())  # Linux FICLONE
        shutil.copystat(source, target)
    except OSError as error:
        if error.errno not in (errno.EXDEV, errno.EOPNOTSUPP, errno.ENOTTY, errno.EINVAL):
            raise
        shutil.copy2(source, target)
    return str(target)


def copy_missing(source, target):
    """Fill an owned generation without replacing user themes or following links."""
    source, target = Path(source), Path(target)
    if target.is_symlink():
        if target.exists():
            return
        target.unlink()
    if not target.exists():
        copy_private(source, target)
    elif source.is_dir() and target.is_dir():
        target.chmod(stat.S_IMODE(target.stat().st_mode) | 0o700)
        for child in source.iterdir():
            copy_missing(child, target / child.name)


@contextlib.contextmanager
def lock(path, exclusive=True, blocking=True):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
    try:
        operation = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
        fcntl.flock(fd, operation | (0 if blocking else fcntl.LOCK_NB))
        yield fd
    finally:
        os.close(fd)


def exchange(left, right):
    """Linux permits exchanging a nonempty legacy directory with a symlink."""
    libc = ctypes.CDLL(None, use_errno=True)
    rename = getattr(libc, 'renameat2', None)
    if rename is None:
        raise OSError(errno.ENOSYS, 'atomic directory exchange is unavailable')
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                       ctypes.c_char_p, ctypes.c_uint)
    if rename(-100, os.fsencode(left), -100, os.fsencode(right), 2):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    sync_directory(Path(left).parent)


def point(path, target, legacy_backup=None):
    path, target = Path(path), Path(target)
    temporary = path.with_name('.' + path.name + '-' + uuid.uuid4().hex)
    temporary.symlink_to(os.path.relpath(target, path.parent))
    try:
        if path.exists() and not path.is_symlink():
            if legacy_backup is None:
                raise ValueError('legacy directory needs a preserved destination')
            try:
                exchange(path, temporary)
                os.replace(temporary, legacy_backup)
            except OSError as error:
                if error.errno not in (errno.ENOSYS, errno.EINVAL, errno.EOPNOTSUPP):
                    raise
                # Consumers hold the shared runtime lease. Under the exclusive
                # lease, a journaled two-rename transition also supports older
                # kernels/filesystems without RENAME_EXCHANGE. The previous
                # generation and data snapshot already exist durably.
                os.rename(path, legacy_backup)
                sync_directory(path.parent)
                sync_directory(legacy_backup.parent)
                os.replace(temporary, path)
        else:
            os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if temporary.is_symlink():
            temporary.unlink()


class RuntimeManager:
    def __init__(self, data, target, validator, assets_installer, voice_installer, ui_required=()):
        self.data, self.target = Path(data).absolute(), Path(target).absolute()
        self.root = self.data / 'fcitx5-wetypex'
        self.store = self.target.parent / 'runtime-generations'
        self.state = Path(os.environ.get('WETYPE_STATE_DIR', self.root / 'state'))
        self.account = Path(os.environ.get('WETYPE_ACCOUNT_STATE_DIR', self.root / 'account'))
        self.cache = Path(os.environ.get('WETYPE_RUNTIME_CACHE',
                         Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) /
                         'fcitx5-wetypex/archives'))
        self.cache_lock = self.cache / '.use.lock'
        self.owner = hashlib.sha256((str(self.root) + '\n' + str(self.target)).encode()).hexdigest()
        self.cache_reference = self.cache / ('.retained-' + hashlib.sha256(
            (str(self.root) + '\n' + str(self.target)).encode()).hexdigest() + '.json')
        catalog = Path(__file__).with_name('runtime-profiles.json')
        if not catalog.exists():
            catalog = Path(__file__).resolve().parent.parent / 'data/runtime-profiles.json'
        self.catalog = read_json(catalog)
        if self.catalog.get('schema') != 1:
            raise ValueError('unsupported runtime compatibility catalog')
        self.arch = {'x86_64': 'x86_64', 'amd64': 'x86_64',
                     'aarch64': 'arm64', 'arm64': 'arm64'}.get(platform.machine())
        if platform.system() != 'Linux' or self.arch is None:
            raise ValueError('supported hosts are Linux x86-64 and ARM64')
        self.validator = validator
        self.assets_installer, self.voice_installer = assets_installer, voice_installer
        self.ui_required = tuple(ui_required)
        self.pending = self.root / 'runtime-pending.json'
        self.journal = self.root / 'runtime-transaction.json'
        self.history = self.root / 'runtime-history.json'
        self.use_lock = self.root / 'runtime-use.lock'
        self.setup_lock = self.root / 'runtime-setup.lock'
        self.roots = {'user': self.state / 'user', 'user_dict': self.state / 'userDict',
                      'persistent_data': self.state / 'persistent_data', 'account': self.account,
                      'voice': self.state / 'voice'}
        self.roots={name:path.resolve() for name,path in self.roots.items()}
        for name, path in self.roots.items():
            for other, parent in self.roots.items():
                if other != name and (path == parent or parent in path.parents):
                    raise ValueError('runtime data directories must not overlap')
            if path == self.target or path == self.store or self.target in path.parents or self.store in path.parents:
                raise ValueError('runtime images and user data need separate directories')

    def check_existing_target(self):
        if self.target.is_symlink() and not self.target.exists():
            raise ValueError('the current runtime link is broken; the previous files are retained')
        if not self.target.exists():
            return
        if self.target.is_mount() and not self.target.is_symlink():
            raise ValueError('use a runtime subdirectory inside the mounted filesystem')
        if not self.target.is_dir():
            raise ValueError('the runtime destination is not a directory')
        if not any(self.target.iterdir()):
            return
        image=self.target/'image.macho'
        if not image.is_file() or not (self.target/'manifest.txt').is_file():
            raise ValueError('the existing directory is not an original runtime and will be preserved')
        known={value for key,component in self.catalog['components'].items()
               if key.endswith('engine') or '/MacOS/WeType' in component['member']
               for value in component['slices'].values()}
        if digest(image) not in known:
            raise ValueError('the existing original runtime has no supported compatibility profile')

    def profile(self, version=None):
        key = version or self.catalog['default']
        try:
            return self.catalog['profiles'][key]
        except KeyError as error:
            raise ValueError('this original version has no validated compatibility profile') from error

    def installed_profile(self):
        image = self.target / 'image.macho'
        if not image.is_file():
            return {}
        value = digest(image)
        matches = [profile for profile in self.catalog['profiles'].values()
                   if self.catalog['components'][profile['components']['engine']]['slices'][self.arch] == value]
        if not matches:
            raise ValueError('the current runtime belongs to another architecture or an unsupported version')
        declared = read_json(self.target / 'runtime.json').get('version')
        return next((profile for profile in matches if profile['version'] == declared), matches[0])

    def dictionaries_ready(self, runtime):
        try:
            rows = json.loads((runtime / 'resources/index.json').read_text())
            return (runtime / 'dicts.txt').is_file() and bool(rows) and all(
                Path(row['name']).name == row['name'] and
                digest(runtime / 'resources' / row['name'], 'md5') == row['md5'] for row in rows)
        except (OSError, ValueError, TypeError, KeyError):
            return False

    def auxiliary_ready(self, runtime, profile):
        return all((runtime / name / 'manifest.txt').is_file() and
                   (runtime / name / 'image.macho').is_file() and
                   digest(runtime / name / 'image.macho') ==
                   self.catalog['components'][profile['components'][name]]['slices'][self.arch]
                   for name in ('flurry', 'wxp2p'))

    def status(self):
        current = read_json(self.target / 'runtime.json')
        if not current.get('version') and (self.target / 'image.macho').is_file():
            current = self.installed_profile()
        desired = self.catalog['default']
        installed = current.get('version', '')
        newer = bool(installed and tuple(map(int, installed.split('.'))) >
                     tuple(map(int, desired.split('.'))))
        return dict(runtime=str(self.target), architecture=self.arch,
                    current=installed, default=desired,
                    update_available=bool(installed and not newer and
                        (installed != desired or current.get('preparation') != self.preparation())),
                    pending=read_json(self.pending), history=read_json(self.history))

    def preparation(self):
        prepare = Path(__file__).with_name('prepare_macos_runtime.py')
        if not prepare.exists():
            prepare = Path(__file__).resolve().parent.parent / 'tools/prepare_macos_runtime.py'
        return hashlib.sha256((digest(prepare) + ':storage-v2').encode()).hexdigest()

    def immutable(self, path, expected):
        """Share only verified immutable payloads, including across installations."""
        path = Path(path)
        if path.is_symlink() or not path.is_file() or digest(path) != expected:
            raise ValueError('immutable runtime payload checksum mismatch')
        objects = self.cache / 'objects'
        objects.mkdir(parents=True, exist_ok=True, mode=0o700)
        if objects.stat().st_dev != path.stat().st_dev:
            path.chmod(0o444)
            return
        blob = objects / expected
        if blob.is_symlink():
            raise ValueError('invalid immutable runtime cache entry')
        if not blob.exists() or digest(blob) != expected:
            fd, temporary = tempfile.mkstemp(prefix='.object-', dir=objects)
            os.close(fd)
            try:
                clone_private(path, temporary)
                if digest(temporary) != expected:
                    raise ValueError('runtime payload changed while caching')
                os.chmod(temporary, 0o444)
                with open(temporary, 'rb') as stream:
                    os.fsync(stream.fileno())
                os.replace(temporary, blob)
                sync_directory(objects)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        temporary = path.with_name('.shared-' + uuid.uuid4().hex)
        try:
            try:
                os.link(blob, temporary)
            except OSError as error:
                # Bind mounts can report the same device number while still
                # rejecting links across mount boundaries. Restricted filesystems
                # may also disallow links; the verified private copy is complete.
                if error.errno not in (errno.EXDEV, errno.EOPNOTSUPP, errno.EPERM,
                                      errno.EACCES, errno.EMLINK):
                    raise
                path.chmod(0o444)
                return
            os.replace(temporary, path)
        finally:
            if temporary.exists():
                temporary.unlink()

    def request_rollback(self):
        with lock(self.setup_lock):
            self.check_existing_target()
            history = read_json(self.history)
            previous = history.get('previous')
            if not previous:
                raise ValueError('no previous runtime generation is available')
            generation_name(previous)
            metadata = read_json(self.store / previous / 'runtime.json')
            atomic_json(self.pending, dict(action='rollback', generation=previous,
                        version=metadata.get('version', ''), architecture=self.arch,
                        prepared_at=int(time.time())))
            atomic_json(self.root / 'runtime-policy.json',
                        dict(suppressed_default=self.catalog['default']))
            return self.status()

    def cleanup(self, dry_run=False, automatic=False):
        # A shared lease permits normal input. Setup serialization and the
        # retained current/previous/pending generations prevent removal of any
        # image an existing consumer can still use.
        try:
            with lock(self.setup_lock, blocking=False), \
                    lock(self.use_lock, exclusive=False, blocking=False):
                return self._cleanup_locked(dry_run)
        except BlockingIOError:
            return dict(deferred=True, reason='an update is in progress')
        except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
            if not automatic:
                raise
            return dict(error=str(error)[:500])

    def archive_hashes(self, profile):
        names = {profile['assets_archive'], profile['dictionaries_archive']}
        names.update(self.catalog['components'][key]['archive']
                     for key in profile['components'].values())
        return {self.catalog['archives'][name]['sha256'] for name in names}

    def _cleanup_locked(self, dry_run=False):
        if self.journal.exists():
            return dict(deferred=True, reason='runtime recovery is pending')
        self.check_existing_target()
        if self.store.is_symlink():
            raise ValueError('cleanup requires a real runtime-generations directory')
        cache, store = self.cache.resolve(), self.store.resolve()
        for path in (self.target.resolve(), store, self.state.resolve(), self.account.resolve(),
                     *self.roots.values(), *((self.data / path).resolve() for path in ASSETS)):
            if cache == path or cache in path.parents or path in cache.parents:
                raise ValueError('archive cache must be separate from runtime images and user data')
        keep, metadata = set(), []

        def retain(path):
            resolved = path.resolve()
            if store in resolved.parents:
                name = resolved.relative_to(store).parts[0]
                if name not in keep:
                    keep.add(name)
                    metadata.append(read_json(self.store / name / 'runtime.json'))

        retain(self.target)
        if self.target.exists() and not metadata:
            metadata.append(read_json(self.target / 'runtime.json') or self.installed_profile())
        history, pending = read_json(self.history), read_json(self.pending)
        for name in (history.get('current'), history.get('previous'), pending.get('generation')):
            if name:
                retain(self.store / generation_name(name))
        for relative in ASSETS:
            retain(self.data / relative)
        # Themes or other generation assets can link into another generation.
        scanned = set()
        while keep - scanned:
            name = next(iter(keep - scanned))
            scanned.add(name)
            for directory, children, files in os.walk(self.store / name, followlinks=False):
                for child in children + files:
                    path = Path(directory) / child
                    if path.is_symlink():
                        retain(path)

        archives = set()
        for row in metadata:
            profile = self.catalog['profiles'].get(row.get('version'))
            if not profile:
                # An older adapter may not understand this generation's
                # dictionaries or resources. Keep its archives until it can.
                return dict(deferred=True, reason='a retained runtime has unknown dependencies')
            # Complete generations contain everything rollback needs. Keep ZIPs
            # only for incomplete legacy generations and the pending request.
            generation = row.get('generation')
            complete = bool(generation and (self.store / generation / 'assets').is_dir() and
                            self.dictionaries_ready(self.store / generation) and
                            self.auxiliary_ready(self.store / generation, profile))
            if not complete or pending.get('generation') == generation:
                archives.update(self.archive_hashes(profile))

        # is_mount() alone cannot detect bind mounts on the same filesystem.
        mounts = {Path(re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), line.split()[4]))
                  for line in Path('/proc/self/mountinfo').read_text().splitlines()}

        def size(path):
            total = 0
            device = path.lstat().st_dev
            for directory, children, files in os.walk(path, followlinks=False):
                for name in children + files:
                    child = Path(directory) / name
                    info = child.lstat()
                    if not stat.S_ISLNK(info.st_mode) and (info.st_dev != device or
                            child.resolve() in mounts or child.is_mount()):
                        raise ValueError('cleanup will not cross a mounted filesystem')
                    total += info.st_size
            return total

        try:
            with lock(self.cache_lock, blocking=False):
                # Persist this installation's dependencies before considering
                # other installations that share the same archive directory.
                if not dry_run:
                    atomic_json(self.cache_reference, dict(root=str(self.root), runtime=str(self.target),
                                                          archives=sorted(archives)))
                stale_pins = []
                for pin in (*self.cache.glob('.pending-*.json'), *self.cache.glob('.retained-*.json')):
                    if pin == self.cache_reference:
                        continue
                    if pin.is_symlink() or not pin.is_file():
                        continue
                    row = read_json(pin)
                    root = row.get('root')
                    hashes = row.get('archives')
                    if not isinstance(root, str) or not Path(root).is_absolute() or \
                            not isinstance(hashes, list) or any(not isinstance(value, str) or
                            not re.fullmatch(r'[0-9a-f]{64}', value) for value in hashes):
                        return dict(deferred=True, reason='an archive reservation is incomplete')
                    root = Path(root)
                    request = read_json(root / 'runtime-pending.json')
                    if pin.name.startswith('.retained-'):
                        runtime = row.get('runtime')
                        if not isinstance(runtime, str) or not Path(runtime).is_absolute():
                            return dict(deferred=True, reason='an archive reservation is incomplete')
                        needed = (Path(runtime).is_dir() or bool(request) or
                                  (root / 'runtime-transaction.json').exists())
                    else:
                        needed = bool(row.get('generation') and request.get('generation') == row['generation'])
                    if needed:
                        archives.update(hashes)
                    else:
                        stale_pins.append(pin)
                candidates = []
                allowed = {'runtime.json', 'image.macho', 'manifest.txt', 'symbols.txt',
                           'dicts.txt', 'sha256.txt', 'macho-info.json', 'resources',
                           'flurry', 'wxp2p', 'assets'}
                quarantine = '.gc-' + self.owner[:12] + '-'
                for path in sorted(self.store.iterdir()) if self.store.exists() else ():
                    if path.name in keep or path.is_symlink() or not path.is_dir() or \
                            path.resolve() in mounts or path.is_mount():
                        continue
                    snapshot = re.fullmatch(r'(?:' + re.escape(quarantine) + r')?data-[0-9a-f]{32}', path.name)
                    if snapshot:
                        marker = path / 'snapshot.json'
                        row = read_json(marker) if not marker.is_symlink() else {}
                        if ((path.name[len(quarantine):] if path.name.startswith(quarantine) else path.name) == history.get('snapshot') or
                                row.get('owner') != self.owner or row.get('schema') != 1 or
                                any(child.name not in {*self.roots, 'snapshot.json'} for child in path.iterdir())):
                            continue
                        candidates.append((path, size(path), True))
                        continue
                    quarantined = bool(re.fullmatch(re.escape(quarantine) + MANAGED_GENERATION, path.name))
                    if not quarantined:
                        if not re.fullmatch(MANAGED_GENERATION, path.name):
                            continue
                        marker = path / 'runtime.json'
                        if marker.is_symlink():
                            continue
                        row = read_json(marker)
                        if (row.get('generation') != path.name or row.get('protocol') != 1 or
                                row.get('owner') != self.owner):
                            continue
                        if not path.name.startswith('legacy-') and not path.name.startswith(
                                str(row.get('version')) + '-' + str(row.get('architecture')) + '-'):
                            continue
                        if any(child.name not in allowed for child in path.iterdir()):
                            continue
                    candidates.append((path, size(path), True))
                for path in sorted(self.cache.iterdir()) if self.cache.exists() else ():
                    if path.is_symlink() or not path.is_file() or path.resolve() in mounts or path.is_mount():
                        continue
                    archive = re.fullmatch(r'([0-9a-f]{64})\.zip', path.name)
                    stale = (re.fullmatch(r'\.(?:download|archive)-[a-z0-9_]{8}', path.name) and
                             path.stat().st_mtime < time.time() - 86400)
                    if (archive and archive[1] not in archives) or stale:
                        candidates.append((path, path.stat().st_size, False))
                result = dict(dry_run=dry_run, removed_generations=[], removed_archives=[],
                              removed_bytes=0, retained_generations=sorted(keep))
                if not dry_run:
                    for pin in stale_pins:
                        pin.unlink()
                    if stale_pins:
                        sync_directory(self.cache)
                for path, count, directory in candidates:
                    if not dry_run:
                        if directory:
                            trash = path if path.name.startswith(quarantine) else path.with_name(quarantine + path.name)
                            if trash != path:
                                path.rename(trash)
                                sync_directory(self.store)
                            shutil.rmtree(trash)
                            sync_directory(self.store)
                        else:
                            path.unlink()
                            sync_directory(self.cache)
                    result['removed_generations' if directory else 'removed_archives'].append(path.name)
                    result['removed_bytes'] += count
                # Hardlinks held by retained or other installations keep their
                # blobs alive. Only unreferenced objects are reclaimed.
                objects = self.cache / 'objects'
                if objects.is_dir() and not objects.is_symlink():
                    for blob in objects.iterdir():
                        if (re.fullmatch(r'[0-9a-f]{64}', blob.name) and not blob.is_symlink()
                                and blob.is_file() and blob.stat().st_nlink == 1):
                            result['removed_bytes'] += blob.stat().st_size
                            if not dry_run:
                                blob.unlink()
                    if not dry_run:
                        sync_directory(objects)
                return result
        except BlockingIOError:
            return dict(deferred=True, reason='original archives are being prepared')

    def _auto_cleanup(self):
        try:
            return self._cleanup_locked()
        except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
            # A successful activation remains successful if disk cleanup fails.
            return dict(error=str(error)[:500])

    def archive(self, name, download):
        with lock(self.cache_lock, exclusive=False):
            return self._archive_locked(name, download)

    def _archive_locked(self, name, download):
        package = self.catalog['archives'][name]
        self.cache.mkdir(parents=True, exist_ok=True, mode=0o700)
        path = self.cache / (package['sha256'] + '.zip')
        if path.is_file() and digest(path) == package['sha256']:
            return path
        if not download:
            raise ValueError('a required original archive is not cached: ' + package['version'])
        validate_url(package['url'], DOWNLOAD_HOSTS)
        class Redirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, request, fp, code, message, headers, newurl):
                validate_url(newurl, DOWNLOAD_HOSTS, allow_cdn=True)
                return super().redirect_request(request, fp, code, message, headers, newurl)
        fd, temporary = tempfile.mkstemp(prefix='.download-', dir=self.cache)
        try:
            with os.fdopen(fd, 'wb') as output:
                with urllib.request.build_opener(Redirect()).open(package['url'], timeout=45) as response:
                    validate_url(response.geturl(), DOWNLOAD_HOSTS, allow_cdn=True)
                    total = 0
                    while True:
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        total += len(block)
                        if total > 768 * 1024 * 1024:
                            raise ValueError('original archive exceeds its size limit')
                        output.write(block)
                output.flush()
                os.fsync(output.fileno())
            if digest(temporary) != package['sha256']:
                raise ValueError('original archive SHA-256 mismatch')
            os.replace(temporary, path)
            sync_directory(self.cache)
            return path
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def cache_archive(self, path):
        with lock(self.cache_lock, exclusive=False):
            return self._cache_archive_locked(path)

    def _cache_archive_locked(self, path):
        value = digest(path)
        if value not in {row['sha256'] for row in self.catalog['archives'].values()}:
            raise ValueError('archive has no supported SHA-256 compatibility profile')
        self.cache.mkdir(parents=True, exist_ok=True, mode=0o700)
        output = self.cache / (value + '.zip')
        if not output.is_file() or digest(output) != value:
            fd, temporary = tempfile.mkstemp(prefix='.archive-', dir=self.cache)
            os.close(fd)
            try:
                shutil.copyfile(path, temporary)
                if digest(temporary) != value:
                    raise ValueError('original archive changed while copying')
                os.replace(temporary, output)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)

    def prepare_assets(self, generation, profile, download):
        assets = generation / 'assets'
        source = self.catalog['archives'][profile['assets_archive']]
        cached = self.cache / (source['sha256'] + '.zip')
        current_profile = self.installed_profile()
        reusable = (current_profile.get('assets_archive') == profile['assets_archive'] and
                    all((self.data / path).exists() for path in ASSETS) and
                    all((self.data / 'fcitx5-wetypex/ui' / path).is_file() for path in self.ui_required))
        if reusable:
            for path in ASSETS:
                copy_private(self.data / path, assets / path)
        else:
            archive = self.archive(profile['assets_archive'], download)
            with contextlib.redirect_stdout(sys.stderr):
                self.assets_installer(archive, assets)
                self.voice_installer(archive, assets)
            if current_profile.get('assets_archive') == profile['assets_archive']:
                for theme in ('wetypex-light', 'wetypex-dark'):
                    source = self.data / 'fcitx5/themes' / theme
                    if source.is_dir():
                        destination = assets / 'fcitx5/themes' / theme
                        with tempfile.TemporaryDirectory(prefix='.theme-', dir=generation) as directory:
                            default = Path(directory) / theme
                            if destination.exists() or destination.is_symlink():
                                destination.rename(default)
                            copy_private(source, destination)
                            if default.exists():
                                copy_missing(default, destination)

    def prepare_component(self, work, name, profile, download, previous):
        component = self.catalog['components'][profile['components'][name]]
        destination = work if name == 'engine' else work / name
        prepare = Path(__file__).with_name('prepare_macos_runtime.py')
        if not prepare.exists():
            prepare = Path(__file__).resolve().parent.parent / 'tools/prepare_macos_runtime.py'
        installed = self.target / ('' if name == 'engine' else name) / 'image.macho'
        expected = component['slices'][self.arch]
        reusable = next((path for path in (installed, self.cache / 'objects' / expected)
                         if path.is_file() and (path == installed or not path.is_symlink()) and digest(path) == expected), None)
        if reusable:
            binary = reusable
        else:
            archive = self.archive(component['archive'], download)
            binary = work / ('.source-' + name)
            with zipfile.ZipFile(archive) as source:
                with source.open(component['member']) as data, binary.open('wb') as out:
                    shutil.copyfileobj(data, out, 1024 * 1024)
            expected = component['sha256']
        command = [sys.executable, str(prepare), str(binary), str(destination),
                   '--architecture', self.arch, '--expected-sha256', expected]
        if name != 'engine':
            command += ['--export-prefix', '__ZN6flurry' if name == 'flurry' else '__ZN13wxp2ptransfer']
            for parent in previous:
                command += ['--after-module', str(parent)]
        image = destination / 'image.macho'
        if image.exists() and image != binary:
            image.unlink()
        subprocess.run(command, check=True, stdout=subprocess.PIPE)
        if binary.parent == work and binary.name.startswith('.source-'):
            binary.unlink()
        self.immutable(destination / 'image.macho', component['slices'][self.arch])
        return dict(version=self.catalog['archives'][component['archive']]['version'],
                    source=self.catalog['archives'][component['archive']]['url'],
                    sha256=component['sha256'], slice_sha256=component['slices'][self.arch])

    def prepare_dictionaries(self, work, profile, download):
        resources = work / 'resources'
        resources.mkdir()
        files = self.catalog['archives'][profile['dictionaries_archive']]['dictionaries']
        with contextlib.ExitStack() as stack:
            source = None
            for relative, metadata in files.items():
                if Path(relative).name != relative or relative in ('', '.', '..'):
                    raise ValueError('invalid original dictionary path')
                expected = metadata['sha256']
                destination = resources / relative
                reusable = next((path for path in (self.target / 'resources' / relative,
                                    self.cache / 'objects' / expected)
                                 if path.is_file() and not path.is_symlink() and digest(path) == expected), None)
                if reusable:
                    clone_private(reusable, destination)
                else:
                    if source is None:
                        source = stack.enter_context(zipfile.ZipFile(
                            self.archive(profile['dictionaries_archive'], download)))
                    member = 'WeType.app/Contents/Resources/imeData.bundle/' + relative
                    if source.getinfo(member).file_size != metadata['size']:
                        raise ValueError('original dictionary size mismatch')
                    with source.open(member) as data, destination.open('wb') as out:
                        shutil.copyfileobj(data, out, 1024 * 1024)
                self.immutable(destination, expected)
        rows = json.loads((resources / 'index.json').read_text())
        for row in rows:
            if Path(row['name']).name != row['name']:
                raise ValueError('invalid original dictionary name')
            value = digest(resources / row['name'], 'md5')
            if value != row['md5']:
                raise ValueError('original dictionary checksum mismatch')
        (work / 'dicts.txt').write_text(''.join(f"{r['type']} {r['version']} {r['name']}\n" for r in rows if not r['is_cell']))

    def stage(self, version=None, archive=None, download=False):
        with lock(self.setup_lock), lock(self.cache_lock, exclusive=False):
            if self.journal.exists():
                with lock(self.use_lock, blocking=False), \
                        lock(self.state / 'backend.lock', blocking=False), \
                        lock(self.account / 'account.lock', blocking=False), \
                        lock(self.state / 'voice/daemon.lock', blocking=False):
                    self.recover()
            self.check_existing_target()
            profile = self.profile(version)
            if archive:
                self.cache_archive(archive)
            previous = self.installed_profile()
            if previous:
                if not self.dictionaries_ready(self.target):
                    self.archive(previous['dictionaries_archive'], download)
                if not self.auxiliary_ready(self.target, previous):
                    for name in ('flurry', 'wxp2p'):
                        self.archive(self.catalog['components'][previous['components'][name]]['archive'], download)
            selected = {name: self.catalog['components'][key]
                        for name, key in profile['components'].items()}
            fingerprint = hashlib.sha256(json.dumps(dict(profile=profile, components=selected,
                                                         ui_required=self.ui_required,
                                                         preparation=self.preparation(), owner=self.owner),
                                         sort_keys=True).encode()).hexdigest()[:12]
            generation_id = profile['version'] + '-' + self.arch + '-' + fingerprint
            target = self.store / generation_id
            self.store.mkdir(parents=True, exist_ok=True, mode=0o700)
            created = False
            if not target.exists():
                with tempfile.TemporaryDirectory(prefix='.prepare-', dir=self.store) as directory:
                    work = Path(directory) / 'runtime'
                    work.mkdir(mode=0o700)
                    components = {}
                    for component in ('engine', 'flurry', 'wxp2p'):
                        parents = [work] + ([work / 'flurry'] if component == 'wxp2p' else [])
                        components[component] = self.prepare_component(work, component, profile, download, parents)
                    self.prepare_dictionaries(work, profile, download)
                    self.prepare_assets(work, profile, download)
                    atomic_json(work / 'runtime.json', dict(version=profile['version'],
                                architecture=self.arch, protocol=1, abi=profile['abi'],
                                data_compatibility=profile.get('data_compatibility'),
                                preparation=self.preparation(),
                                owner=self.owner,
                                generation=generation_id, components=components))
                    ready, error = self.validator(work)
                    if not ready:
                        raise ValueError(error)
                    sync_tree(work)
                    work.rename(target)
                    sync_directory(self.store)
                    created = True
            for name, component in selected.items():
                path = target / ('' if name == 'engine' else name) / 'image.macho'
                if digest(path) != component['slices'][self.arch]:
                    raise ValueError('prepared runtime component checksum mismatch')
            if not created:
                ready, error = self.validator(target)
                if not ready:
                    raise ValueError(error)
            if self.target.is_symlink() and self.target.resolve() == target.resolve():
                if self.pending.exists():
                    self.pending.unlink()
                    sync_directory(self.root)
                return dict(up_to_date=True, **self.status())
            hashes = self.archive_hashes(profile)
            if previous:
                hashes.update(self.archive_hashes(previous))
            pin = self.cache / ('.pending-' + hashlib.sha256(
                (str(self.root) + '\n' + str(self.target) + '\n' + generation_id).encode()).hexdigest() + '.json')
            atomic_json(pin, dict(root=str(self.root), generation=generation_id, archives=sorted(hashes)))
            atomic_json(self.pending, dict(version=profile['version'], generation=generation_id,
                        prepared_at=int(time.time()), architecture=self.arch))
            return self.status()

    def snapshot(self, destination):
        destination.mkdir(mode=0o700)
        for name, source in self.roots.items():
            copy_private(source, destination / name)
        atomic_json(destination / 'snapshot.json', dict(schema=1, owner=self.owner))
        sync_tree(destination)

    def recover(self):
        transaction = read_json(self.journal)
        if not transaction:
            return
        if transaction.get('id') and read_json(self.history).get('transaction') == transaction['id']:
            self.journal.unlink()
            sync_directory(self.root)
            return
        old = transaction.get('old')
        if old:
            generation_name(old)
        if old and (self.store / old).is_dir():
            backup = self.store / ('interrupted-' + uuid.uuid4().hex)
            point(self.target, self.store / old, backup)
        elif not old and self.target.is_symlink():
            # A first installation interrupted before history was committed
            # has no prior image. Retry it as a first installation.
            if self.target.resolve() == (self.store / generation_name(transaction['new'])).resolve():
                self.target.unlink()
                sync_directory(self.target.parent)
        for row in reversed(transaction.get('data_exchanges', [])):
            a, b = Path(row['active']), Path(row['temporary'])
            if a not in self.roots.values() or b.parent != a.parent or \
                    not b.name.startswith('.' + a.name + '-restore-'):
                raise ValueError('invalid recovery data path')
            if b.exists() and a.exists() and a.stat().st_ino == row['candidate_inode']:
                exchange(a, b)
        self.journal.unlink()
        sync_directory(self.root)

    def activate(self, rollback=False):
        if not rollback and not self.pending.exists() and not self.journal.exists():
            return dict(changed=False, cleanup=self.cleanup(automatic=True), **self.status())
        try:
            with lock(self.setup_lock, blocking=False), lock(self.use_lock, blocking=False), \
                    lock(self.state / 'backend.lock', blocking=False), \
                    lock(self.account / 'account.lock', blocking=False), \
                    lock(self.state / 'voice/daemon.lock', blocking=False):
                self.recover()
                try:
                    with lock(self.cache_lock, exclusive=False):
                        result = self._activate_locked(rollback)
                    result['cleanup'] = self._auto_cleanup()
                    return result
                except Exception as error:
                    self.recover()
                    pending=read_json(self.pending)
                    if pending:
                        pending['error']=str(error)[:500]
                        atomic_json(self.pending,pending)
                    raise
        except BlockingIOError:
            return dict(changed=False, deferred=True, **self.status())

    def _activate_locked(self,rollback):
        self.check_existing_target()
        history = read_json(self.history)
        pending = read_json(self.pending)
        rollback = rollback or pending.get('action') == 'rollback'
        desired = history.get('previous') if rollback else pending.get('generation')
        if not desired:
            return dict(changed=False, **self.status())
        generation_name(desired)
        target = self.store / desired
        ready, error = self.validator(target)
        if not ready:
            raise ValueError(error)
        old_metadata = read_json(self.target / 'runtime.json')
        old_profile = self.installed_profile()
        if old_profile:
            old_metadata['version'] = old_profile['version']
            old_metadata['data_compatibility'] = old_profile.get('data_compatibility')
        if self.target.is_dir() and not any(self.target.iterdir()):
            self.target.rmdir()
        old = old_metadata.get('generation')
        if old:
            generation_name(old)
        if self.target.exists() and (not old or self.target.resolve() != (self.store / old).resolve() or
                                     not self.auxiliary_ready(self.target, old_profile) or
                                     not self.dictionaries_ready(self.target)):
            old = 'legacy-' + uuid.uuid4().hex
            legacy = self.store / old
            shutil.copytree(self.target, legacy, symlinks=True, copy_function=clone_private)
            components = dict(old_metadata.get('components', {}))
            components['engine'] = self.prepare_component(legacy, 'engine', old_profile, False, [])
            for name in ('flurry', 'wxp2p'):
                copied = legacy / name
                if copied.is_symlink():
                    copied.unlink()
                elif copied.exists():
                    shutil.rmtree(copied)
                parents = [legacy] + ([legacy / 'flurry'] if name == 'wxp2p' else [])
                components[name] = self.prepare_component(legacy, name, old_profile, False, parents)
            if not self.dictionaries_ready(legacy):
                resources = legacy / 'resources'
                if resources.exists() or resources.is_symlink():
                    resources.rename(legacy / 'original-resources')
                self.prepare_dictionaries(legacy, old_profile, False)
            else:
                for name, metadata in self.catalog['archives'][old_profile['dictionaries_archive']]['dictionaries'].items():
                    path = legacy / 'resources' / name
                    if path.is_file() and not path.is_symlink() and digest(path) == metadata['sha256']:
                        self.immutable(path, metadata['sha256'])
            for path in ASSETS:
                destination = legacy / 'assets' / path
                if not destination.exists():
                    if self.data.joinpath(path).exists():
                        copy_private(self.data / path, destination)
                next_profile = self.catalog['profiles'].get(read_json(target / 'runtime.json').get('version'), {})
                if old_profile.get('assets_archive') == next_profile.get('assets_archive'):
                    copy_missing(target / 'assets' / path, destination)
            metadata = dict(old_metadata, generation=old, architecture=self.arch,
                            components=components, owner=self.owner)
            atomic_json(legacy / 'runtime.json', metadata)
            sync_tree(legacy)
            sync_directory(self.store)
        if old == desired:
            if self.pending.exists():
                self.pending.unlink()
            return dict(changed=False, **self.status())
        snapshot_name = 'data-' + uuid.uuid4().hex
        self.snapshot(self.store / snapshot_name)
        snapshot = self.store / snapshot_name
        next_profile = self.catalog['profiles'].get(read_json(target / 'runtime.json').get('version'), {})
        same_data = bool(old_profile.get('data_compatibility') and
                         old_profile['data_compatibility'] == next_profile.get('data_compatibility'))
        if not rollback and old:
            before, after = {}, {}
            previous_runtime = self.store / old
            ready, error = self.validator(previous_runtime,snapshot,before)
            if not ready:
                raise ValueError(error)
            with tempfile.TemporaryDirectory(prefix='.verify-data-',dir=self.store) as directory:
                ready, error = self.validator(target,snapshot,after,Path(directory))
                if not ready:
                    raise ValueError(error)
                if before.get('hotword_sha256') != after.get('hotword_sha256'):
                    raise ValueError('original common phrases did not survive the runtime upgrade')
                if same_data:
                    reverse = {}
                    ready,error = self.validator(previous_runtime,Path(directory),reverse)
                    if not ready or before.get('hotword_sha256') != reverse.get('hotword_sha256'):
                        raise ValueError(error or 'previous runtime cannot read upgraded common phrases')
        transaction = dict(id=uuid.uuid4().hex, old=old, new=desired,
                           snapshot=snapshot_name, data_exchanges=[])
        atomic_json(self.journal, transaction)
        if rollback and not same_data:
            previous_snapshot = history.get('snapshot')
            if not previous_snapshot:
                raise ValueError('rollback data snapshot is unavailable')
            generation_name(previous_snapshot)
            for name, path in self.roots.items():
                candidate = path.with_name('.' + path.name + '-restore-' + uuid.uuid4().hex)
                saved = self.store / previous_snapshot / name
                if saved.exists():
                    copy_private(saved, candidate)
                else:
                    candidate.mkdir(mode=0o700, parents=True)
                if name == 'account':
                    identity = path / 'identity.json'
                    if identity.is_file():
                        shutil.copy2(identity, candidate / 'identity.json')
                    elif (candidate / 'identity.json').exists():
                        (candidate / 'identity.json').unlink()
                if name in ('account', 'voice'):
                    lock_name = 'account.lock' if name == 'account' else 'daemon.lock'
                    if (path / lock_name).is_file():
                        os.link(path / lock_name, candidate / lock_name)
                sync_tree(candidate)
                path.mkdir(parents=True, exist_ok=True, mode=0o700)
                transaction['data_exchanges'].append(dict(active=str(path), temporary=str(candidate),
                                                         candidate_inode=candidate.stat().st_ino))
                atomic_json(self.journal, transaction)
                exchange(path, candidate)
        for relative in ASSETS:
            current = self.data / relative
            source = self.target / 'assets' / relative
            # Both generations contain assets before these links switch.
            if not (target / 'assets' / relative).exists():
                continue
            current.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            if current.is_symlink() and current.resolve() == source.resolve():
                continue
            backup = self.store / ('asset-' + uuid.uuid4().hex)
            point(current, source, backup)
        preserved = self.store / ('original-' + uuid.uuid4().hex)
        point(self.target, target, preserved)
        atomic_json(self.history, dict(transaction=transaction['id'], current=desired, previous=old,
                    snapshot=snapshot_name, changed_at=int(time.time())))
        self.journal.unlink()
        if self.pending.exists():
            self.pending.unlink()
        sync_directory(self.root)
        return dict(changed=True, **self.status())
