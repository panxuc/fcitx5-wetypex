#!/usr/bin/env python3
"""PipeWire/PulseAudio capture and session control for the original native voice engine."""
import argparse
import array
import fcntl
import hashlib
import json
import os
from pathlib import Path
import queue
import select
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import uuid

DATA = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share'))
STATE = Path(os.environ.get('WETYPE_STATE_DIR', DATA / 'fcitx5-wetypex/state'))
VOICE = STATE / 'voice'
ACCOUNT = Path(os.environ.get('WETYPE_ACCOUNT_STATE_DIR', DATA / 'fcitx5-wetypex/account'))
RUNTIME = Path(os.environ.get('WETYPE_RUNTIME_DIR', DATA / 'fcitx5-wetypex/runtime'))
SUPPORT = Path(os.environ.get('WETYPE_SUPPORT_DIR', '/usr/lib/fcitx5-wetypex'))
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'fcitx5/wetypex.json'
ADDRESS = '\0wetypex-voice-{}-{}'.format(os.getuid(), hashlib.sha256(str(STATE.resolve()).encode()).hexdigest()[:20])
LIMIT = 131072


def read_json(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def atomic_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def peer_is_self(connection):
    return struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1] == os.getuid()


def punctuation(text, mode):
    if mode in ('智能标点', '句末不加句号'):
        text = text.rstrip()
        if text.endswith(('。', '.')):
            text = text[:-1]
    elif mode == '空格替换标点':
        import re
        text = re.sub(r'[，。！？；：、,.!?;:]+', ' ', text)
    return text


def clipboard_helper():
    """Return a helper command that can hold a system clipboard selection."""
    helper = os.environ.get('WETYPE_CLIPBOARD_HELPER')
    if helper and os.access(helper, os.X_OK):
        return helper
    return shutil.which('fcitx5-wetypex-clipboard-set')


def copy_to_clipboard(text):
    """Publish a final result so clients that ignore commits can paste it."""
    if not text:
        return
    payload = text.encode('utf-8')
    if len(payload) > 65536:
        return
    environment = dict(os.environ, QT_IM_MODULE='none')
    if os.environ.get('WAYLAND_DISPLAY') and shutil.which('wl-copy'):
        # data-control clients own the selection without a surface or serial,
        # so nothing can steal focus from the input context while it ends.
        try:
            result = subprocess.run(
                ['wl-copy', '--type', 'text/plain;charset=utf-8'],
                input=payload, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=4, env=environment)
            if result.returncode == 0:
                return
        except (OSError, subprocess.TimeoutExpired):
            pass
    helper = clipboard_helper()
    if not helper:
        return
    if os.environ.get('WAYLAND_DISPLAY') and os.environ.get('DISPLAY'):
        # Qt cannot own a Wayland selection without a focused surface. The
        # XWayland connection keeps the copy silent and does not take focus.
        environment['QT_QPA_PLATFORM'] = 'xcb'
    try:
        owner = subprocess.Popen([helper, '--notify-ready'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, env=environment)
    except OSError:
        return
    try:
        owner.stdin.write(payload)
        owner.stdin.close()
        readable, _, _ = select.select([owner.stdout], [], [], 4)
        if readable and owner.stdout.readline() == b'ready\n':
            # The helper keeps the selection until another client replaces it.
            return
    except (OSError, ValueError):
        pass
    if owner.poll() is None:
        owner.kill()
    try:
        owner.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass


def recorder_options():
    try:
        result = subprocess.run(['pw-record', '--help'], capture_output=True,
                                timeout=3)
        if result.returncode == 0:
            options = result.stdout + result.stderr
            if b'--raw' in options and shutil.which('pw-dump'):
                # Installed tools do not prove an audio server is available.
                # Screen-casting PipeWire instances may have no audio source.
                probe = subprocess.run(['pw-dump'], capture_output=True,
                                       timeout=1)
                if probe.returncode != 0:
                    return b''
                try:
                    nodes = json.loads(probe.stdout)
                    def audio_source(node):
                        info = node.get('info') if isinstance(node, dict) else None
                        props = info.get('props') if isinstance(info, dict) else None
                        return isinstance(props, dict) and props.get('media.class') == 'Audio/Source'
                    if not isinstance(nodes, list) or not any(map(audio_source, nodes)):
                        return b''
                except ValueError:
                    return b''
            return options
    except (OSError, subprocess.TimeoutExpired):
        pass
    return b''


def microphone_command(config):
    microphone = config.get('voice_microphone', '自动检测')
    selected = microphone and microphone != '自动检测'
    options = recorder_options()
    if b'--raw' in options:
        command = ['pw-record', '--raw', '--rate', '16000', '--channels', '1',
                   '--format', 's16', '--latency', '20ms']
        if b'--properties' in options:
            command += ['--properties',
                        'application.name=WeTypeX node.name=wetypex-voice-input'
                        + (' node.dont-fallback=true' if selected else '')]
        if selected:
            command += ['--target', str(microphone)]
        return 'pipewire', command + ['-']
    # Old pw-record (including Ubuntu 22.04's 0.3.48) cannot produce this
    # raw PCM stream. PulseAudio clients also work with pipewire-pulse.
    if selected and str(microphone).isdigit():
        # PipeWire and PulseAudio numbers belong to different namespaces.
        # Reusing a saved PipeWire ID could record a different Pulse source.
        raise RuntimeError('音频后端已变化，请在设置中重新选择麦克风')
    if shutil.which('parec'):
        command = ['parec', '--raw', '--rate=16000', '--channels=1',
                   '--format=s16le', '--latency-msec=20',
                   '--client-name=WeTypeX', '--stream-name=voice']
        if selected:
            command += ['--device=' + str(microphone)]
        return 'pulse', command
    if shutil.which('ffmpeg'):
        return 'ffmpeg-pulse', [
            'ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
            '-f', 'pulse', '-sample_rate', '16000', '-channels', '1',
            '-fragment_size', '640', '-i', str(microphone) if selected else 'default',
            '-f', 's16le', '-ar', '16000', '-ac', '1', 'pipe:1']
    raise RuntimeError('需要支持原始音频的 pw-record、parec 或 FFmpeg')


class Backend:
    def __init__(self, owner):
        self.owner = owner
        self.ready = threading.Event()
        self.write_lock = threading.Lock()
        self.vad = False
        self.broken = False
        directory = VOICE / 'account'
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        directory.chmod(0o700)
        identity = read_json(ACCOUNT / 'identity.json')
        self.identity = (identity.get('uin'), identity.get('device_code'))
        previous = read_json(directory / 'identity.json')
        from_account = bool(identity.get('uin') and identity.get('device_code'))
        old_account = read_json(directory / 'source.json').get('account', False)
        switched = ((from_account and self.identity != (previous.get('uin'), previous.get('device_code')))
                    or (not from_account and old_account))
        if switched:
            shutil.rmtree(directory / 'business', ignore_errors=True)
            try:
                (directory / 'identity.json').unlink()
            except FileNotFoundError:
                pass
        if from_account:
            snapshot = {key: identity[key] for key in ('uin', 'device_code', 'unique_id', 'model') if key in identity}
            atomic_json(directory / 'identity.json', snapshot)
        atomic_json(directory / 'source.json', {'account': from_account})
        binary = SUPPORT / 'wetypex-engine-host'
        if not binary.is_file() or not (RUNTIME / 'manifest.txt').is_file():
            raise RuntimeError('请先安装并准备原版运行时')
        # This process outlives the recording thread that creates it.
        # --die-with-parent also triggers when that *thread* exits; stdin EOF
        # already shuts down the voice service when the controller exits.
        wrapper = shutil.which('bwrap')
        if not wrapper:
            raise RuntimeError('请先安装 Bubblewrap')
        command = [wrapper, '--unshare-pid',
                   '--ro-bind', '/usr', '/usr', '--symlink', 'usr/lib', '/lib',
                   '--ro-bind', '/lib64', '/lib64', '--symlink', 'usr/bin', '/bin',
                   '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--dir', '/etc']
        for name in ('resolv.conf', 'hosts', 'nsswitch.conf'):
            command += ['--ro-bind', '/etc/' + name, '/etc/' + name]
        for path in ('/etc/ssl', '/etc/pki', '/etc/ca-certificates'):
            command += ['--ro-bind-try', path, path]
        command += ['--ro-bind', str(RUNTIME), '/input', '--ro-bind', str(SUPPORT), '/support',
                    '--bind', str(directory), '/work', '--chdir', '/work',
                    '--setenv', 'PATH', '/usr/bin', '--setenv', 'HOME', '/work',
                    '--setenv', 'WETYPE_SUPPORT_DIR', '/support',
                    '--setenv', 'WETYPE_WORK_DIR', '/work/business',
                    '--setenv', 'WETYPE_IDENTITY_FILE', '/work/identity.json',
                    '--setenv', 'WETYPE_DEVICE_MODEL', 'LINUX',
                    '--setenv', 'WETYPE_NETWORK_LIVE', '1']
        vad = Path(os.environ.get('WETYPE_VAD_DIR', DATA / 'fcitx5-wetypex/voice/vad'))
        if vad.is_dir():
            command += ['--ro-bind', str(vad), '/voice-vad', '--setenv', 'WETYPE_VAD_DIR', '/voice-vad']
        command += ['/support/wetypex-engine-host', '/input', 'voice-service']
        self.log = (VOICE / 'backend.log').open('wb')
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, bufsize=65536, env={})
        os.set_blocking(self.process.stdin.fileno(), False)
        threading.Thread(target=self.receive, daemon=True).start()

    def receive(self):
        try:
            for raw in self.process.stdout:
                if len(raw) > LIMIT:
                    break
                try:
                    event = json.loads(raw)
                except ValueError:
                    continue
                if event.get('event') == 'ready':
                    self.vad = bool(event.get('vad'))
                    self.ready.set()
                else:
                    self.owner.receive(self, event)
        finally:
            self.broken = True
            self.ready.set()
            self.owner.backend_failed(self)

    def send(self, message):
        with self.write_lock:
            payload = memoryview((json.dumps(message) + '\n').encode())
            deadline = time.monotonic() + 1
            try:
                while payload:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([], [self.process.stdin], [], remaining)[1]:
                        raise RuntimeError('原版语音服务无响应')
                    try:
                        count = os.write(self.process.stdin.fileno(), payload)
                        payload = payload[count:]
                    except BlockingIOError:
                        continue
            except (OSError, RuntimeError):
                self.broken = True
                raise

    def close(self):
        try:
            if self.broken and self.process.poll() is None:
                self.process.kill()
            self.process.stdin.close()
            self.process.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()
        self.log.close()


class Job:
    def __init__(self, owner, request):
        self.owner = owner
        self.token = str(request['token'])
        self.session = int(request.get('session', 0))
        self.epoch = int(request.get('epoch', 0))
        self.pid = int(request.get('pid', 0))
        self.request = request
        self.config = read_json(CONFIG)
        self.capture = None
        self.capture_backend = ''
        self.reader = None
        self.backend = None
        self.cancelled = False
        self.started = False
        self.stopped = threading.Event()
        self.done = threading.Event()
        self.immediate = False
        self.stop_sent = False
        self.error = ''
        self.text = ''
        self.since = time.monotonic()
        self.requested = self.since
        stamp = self.token.split('-', 1)
        if len(stamp) == 2 and stamp[0] == str(self.pid) and stamp[1].isdigit():
            self.requested = int(stamp[1]) / 1000000
        self.stopped_at = None
        self.timing = {}
        self.last_level = 0
        self.chunks = queue.Queue(maxsize=500)

    def stop_capture(self):
        if self.stopped_at is None:
            self.stopped_at = time.monotonic()
        self.stopped.set()
        if self.capture and self.capture.poll() is None:
            try:
                self.capture.send_signal(signal.SIGINT)
            except ProcessLookupError:
                pass

    def stop(self, immediate=False):
        self.immediate |= immediate
        self.stop_capture()
        self.owner.publish(self, 'recognizing', self.text)
        if immediate and self.stop_sent:
            try:
                self.owner.send(self, {'op': 'finish'})
            except (OSError, RuntimeError) as error:
                self.owner.fail(self, str(error))

    def cancel(self):
        self.cancelled = True
        self.stop_capture()
        if self.started and self.backend and self.backend.process.poll() is None:
            try:
                self.backend.send({'op': 'cancel', 'token': self.token})
            except (OSError, RuntimeError):
                pass
        self.done.set()

    def capture_audio(self):
        pending = bytearray()
        last_audio = time.monotonic()
        try:
            while not self.cancelled and not self.done.is_set():
                current = time.monotonic()
                if current - self.since > 600:
                    self.stop_capture()
                if not select.select([self.capture.stdout], [], [], .1)[0]:
                    if self.stopped.is_set() and current - self.stopped_at > 1:
                        break
                    if current - last_audio > 5:
                        raise RuntimeError('音频采集无响应，请检查所选麦克风和音频服务')
                    continue
                chunk = os.read(self.capture.stdout.fileno(), 640 - len(pending))
                pending.extend(chunk)
                if not chunk and not pending:
                    break
                if chunk:
                    last_audio = time.monotonic()
                if chunk and len(pending) < 640:
                    continue
                frame = bytes(pending)
                pending.clear()
                if 'first_audio_ms' not in self.timing:
                    self.timing['first_audio_ms'] = round((time.monotonic() - self.requested) * 1000)
                if len(frame) < 640:
                    frame += bytes(640 - len(frame))
                if not self.stopped.is_set() and time.monotonic() - self.last_level > .1:
                    peak = max((abs(n) for n in array.array('h', frame)), default=0)
                    self.owner.publish(self, 'recording', self.text, level=min(100, peak * 100 // 16000))
                    self.last_level = time.monotonic()
                try:
                    self.chunks.put(frame, timeout=1)
                except queue.Full:
                    self.owner.fail(self, '语音服务未能及时启动')
                    break
                if not chunk:
                    break
        except (OSError, RuntimeError) as error:
            self.owner.fail(self, str(error))
        finally:
            while not self.cancelled and not self.done.is_set():
                try:
                    self.chunks.put(None, timeout=.1)
                    break
                except queue.Full:
                    pass

    def run(self):
        try:
            if self.cancelled or self.done.is_set():
                return
            recording_log = (VOICE / 'capture.log').open('wb')
            if self.request.get('file'):
                self.capture_backend = 'file'
                command = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error']
                if self.request.get('realtime'):
                    command += ['-re']
                command += ['-protocol_whitelist', 'file,pipe', '-i', self.request['file'],
                            '-f', 's16le', '-ar', '16000', '-ac', '1', 'pipe:1']
            else:
                self.capture_backend, command = microphone_command(self.config)
            self.capture = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=recording_log)
            self.timing['capture_started_ms'] = round((time.monotonic() - self.since) * 1000)
            self.timing['capture_from_key_ms'] = round((time.monotonic() - self.requested) * 1000)
            self.owner.publish(self, 'recording', self.text)
            if self.stopped.is_set():
                self.stop_capture()
            self.reader = threading.Thread(target=self.capture_audio, daemon=True)
            self.reader.start()
            self.backend = self.owner.get_backend(self)
            if not self.backend or not self.backend.ready.wait(20):
                raise RuntimeError('连接语音服务超时')
            self.timing['backend_ready_ms'] = round((time.monotonic() - self.since) * 1000)
            if self.backend.broken or self.backend.process.poll() is not None:
                raise RuntimeError('原版语音服务启动失败，请检查 voice/backend.log')
            if not self.owner.send(self, {'op': 'start',
                                         'polish': bool(self.config.get('voice_smart_polish', True)),
                                         'traditional': bool(self.request.get('traditional'))}):
                return
            while not self.cancelled and not self.done.is_set():
                try:
                    frame = self.chunks.get(timeout=.1)
                except queue.Empty:
                    continue
                if frame is None:
                    break
                if not self.owner.send(self, {'op': 'pcm', 'data': frame.hex()}):
                    return
            if self.cancelled or self.done.is_set():
                return
            status = self.capture.wait(timeout=2)
            if not self.stopped.is_set() and status != 0:
                raise RuntimeError('无法采集音频，请检查麦克风和 voice/capture.log')
            self.stop_capture()
            self.stop_sent = True
            self.owner.publish(self, 'recognizing', self.text)
            self.owner.send(self, {'op': 'finish' if self.immediate else 'stop'})
            if not self.done.wait(45):
                raise RuntimeError('语音识别超时')
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            self.owner.fail(self, str(error))
        finally:
            self.stop_capture()
            if self.capture:
                try:
                    self.capture.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.capture.kill()
                    self.capture.wait()
                if self.reader:
                    self.reader.join(timeout=1)
                self.capture.stdout.close()
            if 'recording_log' in locals():
                recording_log.close()


class Service:
    def __init__(self):
        self.condition = threading.Condition(threading.RLock())
        self.backend_lock = threading.Lock()
        self.backend = None
        self.job = None
        self.latest = {'state': 'idle', 'version': 0}
        self.last_activity = time.monotonic()
        self.last_native_activity = self.last_activity
        self.shutdown = False
        self.pending_controls = {}
        self.last_start = {}

    def publish(self, job, state, text='', **extra):
        with self.condition:
            if self.job is not job or job.cancelled:
                return
            if state == 'recording' and job.stopped.is_set():
                state = 'recognizing'
            if self.latest.get('token') == job.token and self.latest.get('state') == 'polishing' and state in ('recording', 'recognizing'):
                state = 'polishing'
            self.latest = dict(version=time.monotonic_ns(), daemon_pid=os.getpid(),
                               token=job.token, session=job.session,
                               epoch=job.epoch, state=state, text=text,
                               capture_backend=job.capture_backend,
                               timings=dict(job.timing), **extra)
            self.last_activity = time.monotonic()
            self.last_native_activity = self.last_activity
            if job.session:
                atomic_json(STATE / 'voice-inbox.json', self.latest)
            self.condition.notify_all()

    def get_backend(self, job):
        with self.backend_lock:
            if job.cancelled:
                return None
            identity = read_json(ACCOUNT / 'identity.json')
            signature = (identity.get('uin'), identity.get('device_code'))
            if self.backend and (self.backend.broken or self.backend.process.poll() is not None or
                                 signature != self.backend.identity):
                self.backend.close()
                self.backend = None
            if self.backend is None:
                self.backend = Backend(self)
            return self.backend

    def trim_backend(self):
        with self.backend_lock:
            with self.condition:
                if ((self.job and not self.job.done.is_set()) or
                        time.monotonic() - self.last_native_activity < 120):
                    return
                backend, self.backend = self.backend, None
            if backend:
                backend.close()

    def send(self, job, message):
        with self.condition:
            if self.job is not job or job.cancelled or job.done.is_set():
                return False
            message['token'] = job.token
            job.backend.send(message)
            if message['op'] == 'start':
                job.started = True
            return True

    def receive(self, backend, event):
        with self.condition:
            job = self.job
            if not job or job.backend is not backend or job.token != event.get('token') or job.cancelled or job.done.is_set():
                return
            kind = event.get('event')
            if kind == 'state' and event.get('state') in (2, 3):
                job.stop_capture()
                self.publish(job, 'recognizing', job.text)
            elif kind == 'partial':
                job.text = event.get('text', '')
                if job.text and 'first_partial_ms' not in job.timing:
                    job.timing['first_partial_ms'] = round((time.monotonic() - job.since) * 1000)
                    job.timing['partial_before_stop'] = not job.stopped.is_set()
                phase = 'recognizing' if job.stopped.is_set() else 'recording'
                self.publish(job, phase, job.text)
            elif kind == 'polishing':
                job.stop_capture()
                self.publish(job, 'polishing', job.text)
            elif kind == 'error':
                job.error = event.get('message') or '语音服务错误 {}'.format(event.get('code'))
                if job.error == 'Network request failed':
                    job.error = '语音服务连接失败'
            elif kind == 'final':
                job.stop_capture()
                text = punctuation(event.get('text', ''), job.config.get('voice_punctuation', '智能标点'))
                if job.stopped_at is not None:
                    job.timing['stop_to_final_ms'] = round((time.monotonic() - job.stopped_at) * 1000)
                self.publish(job, 'final' if text else 'error', text,
                             error='' if text else (job.error or '没有识别到语音'), reason=event.get('reason'))
                job.done.set()
                # The plugin already commits the text to the input method's
                # client. Key-only clients such as terminal emulators can drop
                # that commit, so keep a copy the user can paste instead.
                if (text and job.session and
                        job.config.get('voice_copy_to_clipboard', True)):
                    threading.Thread(target=copy_to_clipboard, args=(text,),
                                     daemon=True).start()

    def backend_failed(self, backend):
        with self.condition:
            if self.job and self.job.backend is backend and not self.job.done.is_set():
                self.fail(self.job, '原版语音服务已退出，请检查 voice/backend.log')

    def fail(self, job, message):
        with self.condition:
            if job.done.is_set() or job.cancelled:
                return
            self.publish(job, 'error', error=message)
            job.cancel()

    def request(self, request):
        with self.condition:
            self.last_activity = time.monotonic()
            op = request.get('op')
            if op == 'shutdown':
                if self.job and not self.job.done.is_set():
                    self.publish(self.job, 'cancelled')
                    self.job.cancel()
                self.shutdown = True
                return {'ok': True}
            if op in ('start', 'transcribe'):
                stamp = str(request.get('token', '')).split('-', 1)
                pid = int(request.get('pid', 0))
                if len(stamp) == 2 and stamp[0] == str(pid) and stamp[1].isdigit():
                    sequence = int(stamp[1])
                    if sequence <= self.last_start.get(pid, 0):
                        return {'ok': True, 'stale': True}
                    self.last_start[pid] = sequence
                    if len(self.last_start) > 64:
                        self.last_start.pop(next(iter(self.last_start)))
                if self.job and not self.job.done.is_set():
                    if op == 'transcribe':
                        return {'ok': False, 'error': '已有语音输入正在进行'}
                    self.job.cancel()
                if not request.get('token'):
                    raise ValueError('missing voice token')
                if request.get('file'):
                    path = Path(request['file']).resolve(strict=True)
                    if not path.is_file():
                        raise ValueError('音频文件不存在')
                    request['file'] = str(path)
                self.job = Job(self, request)
                pending = self.pending_controls.pop(self.job.token, None)
                if pending and time.monotonic() - pending[1] < 10:
                    self.publish(self.job, 'cancelled')
                    self.job.cancel()
                else:
                    self.publish(self.job, 'recording')
                    threading.Thread(target=self.job.run, daemon=True).start()
            elif op in ('stop', 'finish', 'cancel'):
                token = request.get('token')
                if token and (not self.job or token != self.job.token):
                    # Separate shortcut helpers can be scheduled out of order.
                    # Remember a release/Esc received before its start helper.
                    self.pending_controls[token] = (op, time.monotonic())
                    if len(self.pending_controls) > 64:
                        self.pending_controls.pop(next(iter(self.pending_controls)))
                    return {'ok': True, 'queued': True}
                if not self.job:
                    return {'ok': True, 'state': 'idle'}
                if not self.job.done.is_set():
                    if op == 'cancel':
                        self.publish(self.job, 'cancelled')
                        self.job.cancel()
                    else:
                        self.job.stop(op == 'finish')
            return dict(self.latest, ok=True, daemon_pid=os.getpid(),
                        native_pid=self.backend.process.pid if self.backend and self.backend.process.poll() is None else None,
                        capture_pid=self.job.capture.pid if self.job and self.job.capture and self.job.capture.poll() is None else None,
                        native_ready=bool(self.backend and not self.backend.broken and self.backend.ready.is_set() and self.backend.process.poll() is None),
                        vad=bool(self.backend and self.backend.process.poll() is None and self.backend.vad))


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        if not peer_is_self(self.connection):
            return
        job = None
        try:
            raw = self.rfile.readline(LIMIT + 1)
            if len(raw) > LIMIT:
                return
            request = json.loads(raw)
            if not isinstance(request, dict):
                raise ValueError('invalid voice request')
            response = self.server.service.request(request)
            self.wfile.write((json.dumps(response) + '\n').encode())
            self.wfile.flush()
            if request.get('op') != 'transcribe' or not response.get('ok'):
                return
            service = self.server.service
            version = response.get('version', 0)
            with service.condition:
                job = service.job
            while True:
                with service.condition:
                    service.condition.wait_for(lambda: service.latest.get('version', 0) > version, timeout=50)
                    status = dict(service.latest)
                if status.get('token') != request['token']:
                    break
                version = status.get('version', 0)
                self.wfile.write((json.dumps(status) + '\n').encode())
                self.wfile.flush()
                if status.get('state') in ('final', 'error', 'cancelled'):
                    break
        except (OSError, ValueError) as error:
            if job and not job.done.is_set():
                with self.server.service.condition:
                    self.server.service.publish(job, 'cancelled')
                    job.cancel()
            try:
                self.wfile.write((json.dumps({'ok': False, 'error': str(error)}) + '\n').encode())
            except OSError:
                pass


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def serve():
    def terminate(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    VOICE.mkdir(mode=0o700, parents=True, exist_ok=True)
    VOICE.chmod(0o700)
    with (VOICE / 'daemon.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        with Server(ADDRESS, Handler) as server:
            service = server.service = Service()
            server.timeout = .25
            atomic_json(VOICE / 'control.json', dict(version=1, address=ADDRESS[1:], pid=os.getpid()))
            try:
                while True:
                    server.handle_request()
                    service = server.service
                    with service.condition:
                        job = service.job
                        if job and not job.done.is_set() and job.pid:
                            try:
                                os.kill(job.pid, 0)
                            except ProcessLookupError:
                                service.publish(job, 'cancelled')
                                job.cancel()
                        active = bool(job and not job.done.is_set())
                    service.trim_backend()
                    if service.shutdown or (not active and time.monotonic() - service.last_activity > 600):
                        break
            finally:
                server.server_close()
                try:
                    (VOICE / 'control.json').unlink()
                except FileNotFoundError:
                    pass
                if service.job:
                    if not service.job.done.is_set():
                        service.publish(service.job, 'cancelled')
                    service.job.cancel()
                if service.backend:
                    service.backend.close()


def connect():
    for attempt in range(40):
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.connect(ADDRESS)
            if not peer_is_self(connection):
                raise RuntimeError('语音服务身份不匹配')
            connection.settimeout(55)
            return connection
        except (ConnectionRefusedError, FileNotFoundError):
            connection.close()
            if attempt == 0:
                VOICE.mkdir(mode=0o700, parents=True, exist_ok=True)
                with (VOICE / 'daemon.log').open('wb') as log:
                    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'serve'],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=log, start_new_session=True, close_fds=True)
            time.sleep(.05)
    raise RuntimeError('无法启动语音服务')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('start', 'stop', 'finish', 'cancel', 'status', 'serve', 'transcribe', 'shutdown'))
    parser.add_argument('args', nargs='*')
    parser.add_argument('--realtime', action='store_true')
    parser.add_argument('--verbose', action='store_true')
    options = parser.parse_args()
    if options.command == 'serve':
        serve()
        return 0
    request = dict(op=options.command)
    if options.command == 'start':
        args = options.args
        request.update(session=int(args[0]) if args else 0,
                       epoch=int(args[1]) if len(args) > 1 else 0,
                       token=args[2] if len(args) > 2 else uuid.uuid4().hex,
                       traditional=len(args) > 3 and args[3] == '1', pid=os.getppid())
        stamp = request['token'].split('-', 1)
        if request['session'] and len(stamp) == 2 and all(part.isdigit() for part in stamp):
            # A helper can be reparented before Python starts. The input
            # method's token still identifies its actual owner process.
            request['pid'] = int(stamp[0])
    elif options.command == 'transcribe':
        if not options.args:
            parser.error('transcribe requires an audio file')
        request.update(file=str(Path(options.args[0]).resolve()), token=uuid.uuid4().hex,
                       pid=os.getpid(), realtime=options.realtime)
    elif options.args:
        request['token'] = options.args[0]
    try:
        connection = connect()
    except (OSError, RuntimeError) as error:
        if options.command == 'start' and request['session']:
            atomic_json(STATE / 'voice-inbox.json', dict(request, version=time.monotonic_ns(),
                                                        state='error', text='', error=str(error)))
        raise
    with connection:
        connection.sendall((json.dumps(request) + '\n').encode())
        stream = connection.makefile('rb')
        for raw in stream:
            event = json.loads(raw)
            if options.command != 'transcribe' or not event.get('ok', True):
                print(json.dumps(event, ensure_ascii=False))
                return 0 if event.get('ok') else 1
            if options.verbose:
                print(json.dumps(event, ensure_ascii=False), file=sys.stderr)
            if event.get('state') in ('final', 'error', 'cancelled'):
                result = dict(event, ok=event['state'] == 'final')
                print(json.dumps(result, ensure_ascii=False))
                return 0 if result['ok'] else 1
    return 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except (OSError, RuntimeError, ValueError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}, ensure_ascii=False))
        raise SystemExit(1)
