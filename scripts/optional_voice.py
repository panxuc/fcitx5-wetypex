"""Pinned, shared CPU ASR resources. No inference dependencies are installed by pip."""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import shutil
import ssl
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

import local_voice as original

_catalog = Path(__file__).parent / 'optional-voice-models.json'
if not _catalog.is_file():
    _catalog = Path(__file__).parent.parent / 'data/optional-voice-models.json'
CATALOG = json.loads(_catalog.read_text())
ROOT = original.DATA / 'voice/sherpa'
CACHE = original.CACHE / 'sherpa'
ARCH = {'amd64': 'x86_64', 'arm64': 'aarch64'}.get(platform.machine().lower(), platform.machine().lower())


def profile(model):
    try:
        return CATALOG['models'][model]
    except (KeyError, TypeError) as error:
        raise RuntimeError('未知离线模型，请在设置中重新选择') from error


def runtime_profile():
    if ARCH not in CATALOG['runtimes']:
        raise RuntimeError('可选语音模型目前支持 x86_64 和 ARM64 Linux')
    value = dict(CATALOG['runtimes'][ARCH])
    value['version'] = CATALOG['runtime_version'] + '-' + ARCH
    value['files'] = dict(value['files'], **{'silero_vad.onnx': CATALOG['vad']})
    return value


@contextlib.contextmanager
def lock(exclusive=False, operation=False):
    ROOT.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (ROOT / ('operation.lock' if operation else 'use.lock')).open('a+') as handle:
        try:
            fcntl.flock(handle, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('离线模型正在使用或更新，请结束语音输入后重试') from error
        yield handle


def verified(root, spec, full=False):
    try:
        target = (root / 'current').resolve(strict=True)
        if target.parent != (root / 'generations').resolve() or not target.is_dir():
            raise ValueError('invalid generation')
        record = json.loads((target / 'verified.json').read_text())
        if record['version'] != spec['version']:
            raise ValueError('model version')
        for name, expected in spec['files'].items():
            p = target / name
            st = p.stat()
            if p.is_symlink() or st.st_size != expected['size'] or record['mtime'][name] != st.st_mtime_ns:
                raise ValueError('file changed')
            if full and original.digest(p) != expected['sha256']:
                raise ValueError('file checksum')
        return target
    except (OSError, ValueError, KeyError) as error:
        raise RuntimeError('请在语音设置中下载或修复所选模型及运行组件') from error


def require_model(model, full=False):
    return (verified(ROOT / 'models' / model, profile(model), full),
            verified(ROOT / 'runtime', runtime_profile(), full))


def metadata(model):
    p = profile(model)
    runtime = runtime_profile() if ARCH in CATALOG['runtimes'] else {'size': 0, 'files': {}}
    return dict(id=model, name=p['name'], description=p['description'], license=p['license'], source=p['source'],
                size=p.get('size', sum(f['size'] for f in p['files'].values())),
                model_bytes=sum(f['size'] for f in p['files'].values()),
                runtime_size=runtime['size'], runtime_bytes=sum(f['size'] for f in runtime['files'].values()))


def status(model):
    p = profile(model)
    try:
        require_model(model)
        ready = True
    except RuntimeError:
        ready = False
    parts = list((CACHE / model).glob('*.part')) + list((CACHE / 'runtime').glob('*.part'))
    present = (ROOT / 'models' / model / 'current').is_symlink()
    return dict(metadata(model), ok=True, ready=ready, version=p['version'], present=present,
                supported=ARCH in CATALOG['runtimes'] and os.access(original.SUPPORT / 'wetypex-sherpa-voice', os.X_OK),
                installed_bytes=sum(f['size'] for f in p['files'].values()) if ready else 0,
                removable=present or bool(parts), downloaded_bytes=sum(f.stat().st_size for f in parts if f.is_file()))


def valid_url(url):
    value = urllib.parse.urlsplit(url)
    host = (value.hostname or '').lower()
    allowed = host in ('github.com', 'huggingface.co', 'release-assets.githubusercontent.com') or any(
        host.endswith(suffix) for suffix in ('.huggingface.co', '.xethub.hf.co', '.cdn.hf.co'))
    if value.scheme != 'https' or not allowed or value.username or value.password or value.port not in (None, 443):
        raise RuntimeError('离线模型下载地址不受信任')


class Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        valid_url(url)
        return super().redirect_request(request, fp, code, message, headers, url)


def download(spec, cache, name):
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    target = cache / (spec['sha256'] + '.part')
    if target.exists() and target.stat().st_size > spec['size']:
        target.unlink()
    context = ssl.create_default_context()
    # Compatible with the GitHub/HF TLS endpoints on older Python/OpenSSL too.
    context.set_ecdh_curve('prime256v1')
    opener = urllib.request.build_opener(Redirect(), urllib.request.HTTPSHandler(context=context))
    valid_url(spec['url'])
    deadline = time.monotonic() + 7200
    failures = 0
    while not target.exists() or target.stat().st_size < spec['size']:
        if time.monotonic() > deadline:
            raise RuntimeError('模型下载超时，已保留进度，可稍后继续')
        offset = target.stat().st_size if target.exists() else 0
        end = min(offset + 8 * 1024 * 1024, spec['size']) - 1
        request = urllib.request.Request(spec['url'], headers={'Range': 'bytes=%d-%d' % (offset, end),
                                        'Accept-Encoding': 'identity', 'User-Agent': 'WeTypeX offline voice'})
        try:
            with opener.open(request, timeout=30) as response:
                valid_url(response.geturl())
                if response.headers.get('Content-Encoding', 'identity') != 'identity':
                    raise RuntimeError('模型服务器返回了无效的编码')
                if response.status == 206:
                    match = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', response.headers.get('Content-Range', ''))
                    if not match or int(match[1]) != offset or int(match[2]) > end or int(match[3]) != spec['size']:
                        raise RuntimeError('模型服务器返回了无效的下载分段')
                    expected = int(match[2]) + 1
                    if expected <= offset:
                        raise RuntimeError('模型服务器返回了空下载分段')
                elif response.status == 200:
                    offset = 0
                    expected = spec['size']
                else:
                    raise RuntimeError('模型服务器返回了无效状态')
                with target.open('r+b' if target.exists() else 'w+b') as output:
                    output.seek(offset)
                    output.truncate()
                    last = 0
                    while True:
                        block = response.read(min(256 * 1024, expected - output.tell() + 1))
                        if not block:
                            break
                        if output.tell() + len(block) > expected:
                            raise RuntimeError('离线模型下载超过预期大小')
                        output.write(block)
                        if time.monotonic() - last >= .2:
                            output.flush()
                            print(json.dumps(dict(state='downloading', received=output.tell(), total=spec['size'], file=name)), flush=True)
                            last = time.monotonic()
                    if output.tell() != expected:
                        raise OSError('模型下载中断')
            failures = 0
        except InterruptedError:
            raise
        except (OSError, urllib.error.URLError) as error:
            failures += 1
            if failures >= 5 or time.monotonic() > deadline:
                raise RuntimeError('模型下载中断，已保留进度，可稍后继续') from error
            time.sleep(min(failures, 3))
    print(json.dumps(dict(state='verifying')), flush=True)
    if target.stat().st_size != spec['size'] or original.digest(target) != spec['sha256']:
        target.unlink()
        raise RuntimeError('离线资源校验失败，请重新下载')
    return target


def clean(root, keep=None):
    for p in (root / 'generations').glob('*'):
        if p != keep and p.is_dir() and not p.is_symlink() and (p.name.startswith('.stage-') or (p / 'verified.json').is_file()):
            shutil.rmtree(p)


def activate(root, spec, populate):
    try:
        return verified(root, spec, full=True)
    except RuntimeError:
        pass
    generations = root / 'generations'
    generations.mkdir(parents=True, exist_ok=True)
    clean(root, (root / 'current').resolve())
    with tempfile.TemporaryDirectory(prefix='.stage-', dir=str(generations)) as temp:
        stage = Path(temp)
        populate(stage)
        for name, expected in spec['files'].items():
            p = stage / name
            if p.is_symlink() or p.stat().st_size != expected['size'] or original.digest(p) != expected['sha256']:
                raise RuntimeError('解压后的离线资源校验失败')
        (stage / 'verified.json').write_text(json.dumps(dict(version=spec['version'],
            mtime={n: (stage / n).stat().st_mtime_ns for n in spec['files']})))
        target = generations / (spec['version'] + '-' + uuid.uuid4().hex[:8])
        stage.rename(target)
        link = root / ('.current-' + uuid.uuid4().hex)
        try:
            link.symlink_to(target.relative_to(root))
            os.replace(link, root / 'current')
        finally:
            if link.is_symlink():
                link.unlink()
        clean(root, target)
        return target


def unpack(archive, spec, stage):
    # Only regular files named in the pinned catalog are ever extracted.
    with tarfile.open(archive, 'r:bz2') as package:
        wanted = {spec['prefix'] + '/' + n: n for n in spec['files'] if n != 'silero_vad.onnx'}
        seen = set()
        for item in package:
            name = wanted.get(item.name)
            if name is None:
                continue
            if name in seen or not item.isfile() or item.size != spec['files'][name]['size']:
                raise RuntimeError('离线资源包格式无效')
            seen.add(name)
            output = stage / name
            output.parent.mkdir(parents=True, exist_ok=True)
            with package.extractfile(item) as source, output.open('wb') as destination:
                shutil.copyfileobj(source, destination)
        if seen != set(wanted.values()):
            raise RuntimeError('离线资源包不完整')


def install(model, accept=False, archive=None):
    if not accept:
        raise RuntimeError('请确认模型来源与许可后使用 --accept-upstream-license')
    if archive:
        raise RuntimeError('可选模型请使用校验下载；--archive 仅用于原版腾讯模型')
    p, r = profile(model), runtime_profile()
    helper = original.SUPPORT / 'wetypex-sherpa-voice'
    if not os.access(helper, os.X_OK):
        raise RuntimeError('当前安装缺少可选离线语音组件，请更新 WeTypeX')
    with lock(exclusive=True, operation=True), lock(exclusive=True):
        try:
            require_model(model, full=True)
            return dict(ok=True, ready=True, state='ready')
        except RuntimeError:
            pass
        CACHE.mkdir(mode=0o700, parents=True, exist_ok=True)
        required = sum(f['size'] for f in p['files'].values()) + p.get('size', sum(f['size'] for f in p['files'].values())) + r['size'] + sum(f['size'] for f in r['files'].values())
        if shutil.disk_usage(ROOT).free < required + 64 * 1024 * 1024:
            raise RuntimeError('离线模型下载和解压所需空间不足，请清理磁盘后重试')
        def populate_runtime(stage):
            packed = download(r, CACHE / 'runtime', '共享运行组件')
            unpack(packed, r, stage)
            shutil.copyfile(download(CATALOG['vad'], CACHE / 'runtime', '语音分段组件'), stage / 'silero_vad.onnx')
        runtime = activate(ROOT / 'runtime', r, populate_runtime)
        # Archives are disposable after atomic activation, even if model loading later fails.
        shutil.rmtree(CACHE / 'runtime', ignore_errors=True)
        def populate_model(stage):
            if 'url' in p:
                unpack(download(p, CACHE / model, p['name']), p, stage)
            else:
                for name, spec in p['files'].items():
                    shutil.copyfile(download(spec, CACHE / model, name), stage / name)
            with tempfile.TemporaryFile() as log:
                probe = subprocess.Popen([str(helper), '--check', str(stage), str(runtime), p['kind']],
                                         stdout=log, stderr=log)
                try:
                    if probe.wait(timeout=120):
                        raise RuntimeError('当前系统无法加载所选离线模型；请尝试 X-ASR 或原版腾讯模型')
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError('模型加载超时，请尝试较小的模型') from error
                finally:
                    if probe.poll() is None:
                        probe.kill()
                        probe.wait()
        activate(ROOT / 'models' / model, p, populate_model)
        shutil.rmtree(CACHE / model, ignore_errors=True)
        return dict(ok=True, ready=True, state='ready')


def remove(model):
    profile(model)
    with lock(exclusive=True, operation=True), lock(exclusive=True):
        root = ROOT / 'models' / model
        if (root / 'current').is_symlink():
            (root / 'current').unlink()
        clean(root)
        shutil.rmtree(CACHE / model, ignore_errors=True)
        if not any((ROOT / 'models').glob('*/current')):
            if (ROOT / 'runtime/current').is_symlink():
                (ROOT / 'runtime/current').unlink()
            clean(ROOT / 'runtime')
            shutil.rmtree(CACHE / 'runtime', ignore_errors=True)
    return dict(ok=True, ready=False)
