"""Verified, opt-in original offline voice resources; no inference network I/O."""
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import urllib.request
import uuid
import zipfile

DATA = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'fcitx5-wetypex'
ROOT = DATA / 'voice/local'
CACHE = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'fcitx5-wetypex/local-voice'
_profile = Path(__file__).parent / 'local-voice-model.json'
if not _profile.is_file():
    _profile = Path(__file__).parent.parent / 'data/local-voice-model.json'
PROFILE = json.loads(_profile.read_text())
SUPPORT = Path(os.environ.get('WETYPE_SUPPORT_DIR', '/usr/lib/fcitx5-wetypex'))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


@contextlib.contextmanager
def lock(name, exclusive=True):
    ROOT.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (ROOT / name).open('a+') as handle:
        try:
            fcntl.flock(handle, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('离线模型正在使用或更新，请结束语音输入后重试')
        yield handle


def directory():
    current = ROOT / 'current'
    target = current.resolve(strict=True)
    if target.parent != (ROOT / 'generations').resolve() or not target.is_dir():
        raise RuntimeError('离线模型目录无效，请重新下载')
    return target


def require_model(full=False):
    try:
        target = directory()
        record = json.loads((target / 'verified.json').read_text())
        if record.get('version') != PROFILE['version']:
            raise ValueError('model version')
        for name, expected in PROFILE['files'].items():
            file = target / name
            st = file.stat()
            if file.is_symlink() or st.st_size != expected['size'] or record['mtime'][name] != st.st_mtime_ns:
                raise ValueError('model changed')
            if full and digest(file) != expected['sha256']:
                raise ValueError('model checksum')
        return target
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise RuntimeError('请在语音输入设置中下载或修复离线模型') from error


def status():
    try:
        require_model()
        ready = True
    except RuntimeError:
        ready = False
    part = CACHE / (PROFILE['version'] + '.part')
    present = (ROOT / 'current').is_symlink()
    return dict(ok=True, ready=ready, supported=os.access(SUPPORT / 'wetypex-local-voice', os.X_OK),
                present=present, removable=present or part.is_file(),
                version=PROFILE['version'], size=PROFILE['size'],
                installed_bytes=sum(f['size'] for f in PROFILE['files'].values()) if ready else 0,
                downloaded_bytes=part.stat().st_size if part.is_file() else 0)


class Download:
    def __init__(self, output):
        self.output, self.last = output, 0

    def __getattr__(self, name):
        return getattr(self.output, name)

    def write(self, block):
        if self.output.tell() + len(block) > PROFILE['size']:
            raise ValueError('离线模型下载超过预期大小')
        result = self.output.write(block)
        if time.monotonic() - self.last >= .2:
            self.output.flush()
            print(json.dumps(dict(state='downloading', received=self.output.tell(), total=PROFILE['size'])), flush=True)
            self.last = time.monotonic()
        return result


def clean_generations(keep=None):
    root = ROOT / 'generations'
    if root.is_dir():
        for target in root.iterdir():
            if target != keep and target.is_dir() and not target.is_symlink() and (target / 'verified.json').is_file():
                shutil.rmtree(target)


def install(archive=None, accept=False):
    if not accept:
        raise RuntimeError('下载原版模型需要 --accept-upstream-license；资源适用微信输入法相关协议')
    helper = SUPPORT / 'wetypex-local-voice'
    if not os.access(helper, os.X_OK):
        raise RuntimeError('当前安装缺少离线语音组件，请更新或重新安装 WeTypeX')
    with lock('operation.lock'), lock('use.lock'):
        try:
            ready = require_model(full=True)
            print(json.dumps(dict(ok=True, state='ready', ready=True)), flush=True)
            return ready
        except RuntimeError:
            pass
        CACHE.mkdir(mode=0o700, parents=True, exist_ok=True)
        download = CACHE / (PROFILE['version'] + '.part')
        source = Path(archive).resolve(strict=True) if archive else download
        if not archive:
            from runtime_catalog import download_archive, validate_url, DOWNLOAD_HOSTS
            class Redirect(urllib.request.HTTPRedirectHandler):
                def redirect_request(self, request, fp, code, message, headers, url):
                    validate_url(url, DOWNLOAD_HOSTS, allow_cdn=True)
                    return super().redirect_request(request, fp, code, message, headers, url)
            if download.exists() and download.stat().st_size > PROFILE['size']:
                download.unlink()
            if not download.exists() or download.stat().st_size < PROFILE['size']:
                with download.open('a+b') as output:
                    output.seek(0, 2)
                    download_archive(urllib.request.build_opener(Redirect()), PROFILE['url'], Download(output))
        print(json.dumps(dict(state='verifying')), flush=True)
        if source.stat().st_size != PROFILE['size'] or digest(source) != PROFILE['sha256']:
            if not archive:
                source.unlink()
            raise RuntimeError('离线模型校验失败，请重新下载')
        generations = ROOT / 'generations'
        generations.mkdir(parents=True, exist_ok=True)
        for abandoned in generations.glob('.stage-*'):
            if abandoned.is_dir() and not abandoned.is_symlink():
                shutil.rmtree(abandoned)
        with tempfile.TemporaryDirectory(prefix='.stage-', dir=str(generations)) as temporary:
            stage = Path(temporary)
            # ZipExtFile is seekable on Python 3.8+: stream the nested ZIP
            # instead of retaining a second 103 MB archive on disk.
            try:
                with zipfile.ZipFile(source) as outer:
                    if outer.getinfo(PROFILE['inner']).file_size > 160 * 1024 * 1024:
                        raise RuntimeError('离线模型资源包过大')
                    with outer.open(PROFILE['inner']) as packed, zipfile.ZipFile(packed) as package:
                        for name, expected in PROFILE['files'].items():
                            matches = [item for item in package.infolist() if Path(item.filename).name == name]
                            if len(matches) != 1 or matches[0].file_size != expected['size']:
                                raise RuntimeError('离线模型资源不完整')
                            target = stage / name
                            with package.open(matches[0]) as stream, target.open('wb') as output:
                                shutil.copyfileobj(stream, output)
                            if digest(target) != expected['sha256']:
                                raise RuntimeError('离线模型文件校验失败')
            except (zipfile.BadZipFile, KeyError) as error:
                raise RuntimeError('离线模型资源格式无效，请重新下载') from error
            probe = subprocess.run([str(helper), '--check', str(stage)], capture_output=True, timeout=20)
            if probe.returncode:
                raise RuntimeError('当前处理器无法加载离线模型，请更新 WeTypeX')
            record = dict(version=PROFILE['version'], mtime={name: (stage / name).stat().st_mtime_ns for name in PROFILE['files']})
            (stage / 'verified.json').write_text(json.dumps(record))
            target = generations / (PROFILE['version'] + '-' + uuid.uuid4().hex[:8])
            stage.rename(target)
            link = ROOT / ('.current-' + uuid.uuid4().hex)
            try:
                link.symlink_to(target.relative_to(ROOT))
                os.replace(link, ROOT / 'current')
            finally:
                if link.is_symlink():
                    link.unlink()
            clean_generations(target)
        if download.is_file():
            download.unlink()
        print(json.dumps(dict(ok=True, state='ready', ready=True)), flush=True)
        return target


def remove():
    with lock('operation.lock'), lock('use.lock'):
        current = ROOT / 'current'
        if current.is_symlink():
            current.unlink()
        clean_generations()
        for stage in (ROOT / 'generations').glob('.stage-*'):
            if stage.is_dir() and not stage.is_symlink():
                shutil.rmtree(stage)
        part = CACHE / (PROFILE['version'] + '.part')
        if part.is_file():
            part.unlink()
    return dict(ok=True, ready=False)


def selected_model():
    config = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'fcitx5/wetypex.json'
    try:
        value = json.loads(config.read_text())
        return value.get('local_voice_model', 'wetype') if isinstance(value, dict) else 'wetype'
    except (OSError, ValueError):
        return 'wetype'


def model_info():
    return dict(id='wetype', name='微信输入法官方模型', description='原版腾讯模型，边说边出字。',
                license='微信输入法相关协议', source='https://z.weixin.qq.com/',
                size=PROFILE['size'], model_bytes=sum(f['size'] for f in PROFILE['files'].values()),
                runtime_size=0, runtime_bytes=0)


def resolve_model(model, full=False):
    if model == 'wetype':
        return require_model(full), None
    import optional_voice
    return optional_voice.require_model(model, full)


def command(args, archive=None, accept=False, model=None):
    action = args[0] if args else 'status'
    import optional_voice
    if action == 'list':
        return dict(ok=True, models=[model_info()] + [optional_voice.metadata(k) for k in optional_voice.CATALOG['models']])
    model = model if model is not None else selected_model()
    if model != 'wetype':
        optional_voice.profile(model)
        if action == 'status':
            return optional_voice.status(model)
        if action == 'remove':
            return optional_voice.remove(model)
        if action == 'verify':
            with optional_voice.lock():
                optional_voice.require_model(model, full=True)
            return optional_voice.status(model)
        if action != 'install':
            raise ValueError('model: list, status, verify, install, remove')
        def cancelled(_signum, _frame):
            raise InterruptedError('已取消下载，可重新点击下载以继续')
        signal.signal(signal.SIGTERM, cancelled)
        return optional_voice.install(model, accept, archive)
    if action == 'status':
        return dict(model_info(), **status())
    if action == 'remove':
        return remove()
    if action == 'verify':
        with lock('use.lock', exclusive=False):
            require_model(full=True)
        return status()
    if action != 'install':
        raise ValueError('model: status, verify, install, remove')
    def cancelled(_signum, _frame):
        raise InterruptedError('已取消下载，可重新点击下载以继续')
    signal.signal(signal.SIGTERM, cancelled)
    return dict(ok=True, ready=bool(install(archive, accept)))
