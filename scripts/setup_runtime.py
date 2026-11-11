#!/usr/bin/env python3
"""Install a verified, user-supplied original WeType runtime in user data."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import select
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

WORKER_STATUS = None
WORKER_FD = None

def install_ui_resources(archive, data):
    """Install verified original UI assets in user data; WeTypeX keeps its own icon."""
    prefix='WeType.app/Contents/MacOS/WeTypeSettings.app/Contents/Frameworks/App.framework/Versions/A/Resources/flutter_assets/assets/modules/'
    with zipfile.ZipFile(archive) as z:
        assets={}
        system_theme_root=next((path for path in (
            pathlib.Path('/usr/share/fcitx5/themes'),
            pathlib.Path('/usr/local/share/fcitx5/themes'))
            if (path/'wetypex-light/theme.conf').is_file()),None)
        if system_theme_root:
            for theme in ('wetypex-light','wetypex-dark'):
                shutil.copytree(system_theme_root/theme,
                                data/'fcitx5/themes'/theme,dirs_exist_ok=True)
        for name, original in {'input':'icon_setup_sidebar_keyboard.svg',
                               'voice':'icon_setup_sidebar_microphone.svg',
                               'phrases':'icon_setup_sidebar_changyongyu.svg',
                               'appearance':'icon_setup_sidebar_style.svg',
                               'shortcuts':'icon_setup_sidebar_keys.svg',
                               'devices':'icon_setup_multi_device.svg',
                               'mobile':'icon_setup_sidebar_phone_entry.svg',
                               'about':'icon_setup_sidebar_about.svg'}.items():
            assets['fcitx5-wetypex/ui/icons/'+name+'.svg']=prefix+'setting/icons/'+original
        for target, original in {
            'ai-logo-light.svg':'icon_ai_logo_light.svg',
            'ai-polish.svg':'icon_toolbox_polish.svg',
            'ai-send.svg':'icon_ai_sending.svg',
            'ai-scroll-down.svg':'icon_ai_scroll_down.svg',
            'ai-copy-image.svg':'icon_ai_copy_image.svg',
        }.items():
            assets['fcitx5-wetypex/ui/ai-icons/'+target]=prefix+'common/icons/'+original
        chrome='WeType.app/Contents/MacOS/WeTypeSettings.app/Contents/Frameworks/App.framework/Versions/A/Resources/flutter_assets/packages/window_manager/images/'
        for target, original in {
            'chrome-minimize.png':'ic_chrome_minimize.png',
            'chrome-maximize.png':'ic_chrome_maximize.png',
            'chrome-unmaximize.png':'ic_chrome_unmaximize.png',
            'chrome-close.png':'ic_chrome_close.png',
        }.items():
            assets['fcitx5-wetypex/ui/ai-icons/'+target]=chrome+original
        for name in ('icon_menu_chinese','icon_menu_punctuation','icon_menu_half',
                     'icon_menu_speechvoice','icon_menu_transmission',
                     'icon_menu_translation','icon_menu_ai','icon_menu_clipboard',
                     'icon_save_to_hotword','icon_hotword_delete_confirm',
                     'icon_transfer_copy',
                     'icon_transfer_glossary','icon_transfer_common',
                     'icon_transfer_filetransfer','icon_entry_ios','icon_entry_android'):
            assets['fcitx5-wetypex/ui/icons/'+name+'.svg']=prefix+'setting/icons/'+name+'.svg'
        for name in ('icon_speech','icon_time','icon_many','icon_about_arrow',
                     'icon_delete_key','icon_windows_close_btn','icon_tips_backslash','icon_left','icon_right',
                     'icon_sync_device','icon_tips_%5B','icon_tips_%5D',
                     'icon_tips_%E3%80%82','icon_tips_%EF%BC%8C',
                     'icon_tips_;','icon_tips_shift+tab_windows',
                     'icon_tips_tab_windows'):
            assets['fcitx5-wetypex/ui/icons/'+name+'.svg']=prefix+'setting/icons/'+name+'.svg'
        for name in ('side_bar_background_light.png','entry_phone_light.png',
                     'icon_computer_default_light.png','icon_iphone_default_light.png',
                     'icon_Android_default_light.png','hotword_guide.gif',
                     'logo_windows_light.png','icon_switch_open.png','icon_switch_close.png',
                     'icon_singlechoice_sel_light.png','icon_multiplechoice.png',
                     'icon_candidate_emoji_choose.png','icon_candidate_emoji_close.png',
                     'icon_candidate_gif_choose.png','icon_candidate_gif_close.png',
                     'icon_candidate_kaomoji_choose.png','icon_candidate_kaomoji_close.png',
                     'phone_ios_download.png','phone_android_download.png'):
            assets['fcitx5-wetypex/ui/images/'+name]=prefix+'setting/images/'+name
        for name in ('icon_transfer_computer_mini.png','icon_transfer_ios_mini.png',
                     'icon_transfer_android_mini.png','icon_button_connect.png'):
            assets['fcitx5-wetypex/ui/images/'+name]=prefix+'setting/icons/'+name
        candidate=prefix+'candidate/icons/'
        for theme,variant in (('wetypex-light','light'),
                              ('wetypex-dark','dark')):
            assets['fcitx5/themes/'+theme+'/next.svg']=(
                candidate+'bubble_normal_arrow_lower_'+variant+'.svg')
            assets['fcitx5/themes/'+theme+'/prev.svg']=(
                candidate+'bubble_normal_arrow_upper_'+variant+'.svg')
        for name in ('icon_keybar_calculator','icon_keybar_clipboard',
                     'icon_keybar_changyongyu','icon_keybar_symbol','icon_keybar_time',
                     'icon_v_list_enter','icon_v_clipboard_complete',
                     'icon_recent_clipboard','icon_recent_close','icon_hotword_add',
                     'icon_hotword_edit','icon_symboltab_kuohao','icon_symboltab_xuhao',
                     'icon_symboltab_shuxue','icon_symboltab_teshu',
                     'icon_symboltab_xiwen','icon_symboltab_riwen',
                     'icon_symboltab_zhongwen','icon_symboltab_zhuyin'):
            assets['fcitx5-wetypex/ui/candidate-icons/'+name+'.svg']=candidate+name+'.svg'
        transfer=prefix+'file_transfer/'
        for name in ('logo_green','icon_transmission_send','icon_transmission_others',
                     'icon_transmission_arrow','icon_transmission_equipment',
                     'icon_transmission_android_big','icon_transmission_ios_big',
                     'icon_transmission_macos_big','icon_tips_arrow','icon_tips_close',
                     'icon_statusbar_setup','icon_filetype_general','icon_filetype_folder',
                     'icon_filetype_video','icon_filetype_music','icon_filetype_pdf',
                     'icon_filetype_doc','icon_filetype_excel','icon_filetype_ppt',
                     'icon_filetype_txt','icon_filetype_json','icon_filetype_zip'):
            assets['fcitx5-wetypex/ui/transfer-icons/'+name+'.svg']=transfer+'icons/'+name+'.svg'
        for name in ('img_connection_pc_to_phone.png','img_connection_pc_to_pc.png',
                     'img_connection_pc_to_phone_dark.png','img_connection_pc_to_pc_dark.png'):
            assets['fcitx5-wetypex/ui/transfer-images/'+name]=transfer+'images/'+name
        for relative, member in assets.items():
            path=data/relative;path.parent.mkdir(parents=True,exist_ok=True)
            temporary=path.with_suffix(path.suffix+'.tmp');temporary.write_bytes(z.read(member));temporary.replace(path)
        candidate_images=prefix+'candidate/icons/emoji_'
        for member in z.namelist():
            if member.startswith(candidate_images) and member.endswith('.png'):
                name=member[len(prefix+'candidate/icons/'):]
                if pathlib.PurePosixPath(name).name!=name:continue
                path=data/'fcitx5-wetypex/ui/wechat-emoticons'/name
                path.parent.mkdir(parents=True,exist_ok=True)
                temporary=path.with_suffix('.png.tmp')
                temporary.write_bytes(z.read(member));temporary.replace(path)
    try:
        from wechat_emoticons import install_font
        font=install_font(archive,data)
        print('Original WeChat emoticons installed: '+str(font))
        print('Restart Fcitx5 to load the original emoticon font.')
    except ImportError:
        print('Install fontTools and Pillow to render original WeChat emoticons; text labels remain available.',file=sys.stderr)

def install_voice_resources(archive, data):
    directory=data/'fcitx5-wetypex/voice/vad'
    directory.mkdir(parents=True,exist_ok=True)
    prefix='WeType.app/Contents/Resources/vad/'
    with zipfile.ZipFile(archive) as source:
        for name in ('configure.16k.20221116.offline',
                     'model_16k_20210121_iter07.bin','CMS_40dim_chi.bin'):
            target=directory/name
            temporary=target.with_suffix(target.suffix+'.tmp')
            temporary.write_bytes(source.read(prefix+name))
            temporary.replace(target)

def stream_digest(stream, algorithm):
    result=hashlib.new(algorithm)
    for block in iter(lambda: stream.read(1024*1024), b''):
        result.update(block)
    return result.hexdigest()

def digest(path):
    with path.open('rb') as stream:return stream_digest(stream,'sha256')

def probe_core(command, work, user_snapshot=None, report=None, seed_common_phrase=False, timeout=25):
    """Exercise the same original-engine protocol used by the input method."""
    if user_snapshot:
        for name, relative in (('user','user'),('user_dict','userDict'),
                               ('persistent_data','persistent_data')):
            source=pathlib.Path(user_snapshot)/name
            if source.is_dir():
                shutil.copytree(source,pathlib.Path(work)/relative,symlinks=True)
    command=list(command)
    command[-1]='serve'
    with tempfile.TemporaryFile() as errors:
        process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                 stderr=errors,env={})
        pending=b''
        deadline=time.monotonic()+timeout
        sequence=0
        def receive():
            nonlocal pending
            while b'\n' not in pending:
                remaining=deadline-time.monotonic()
                if remaining<=0 or not select.select([process.stdout],[],[],remaining)[0]:
                    raise RuntimeError('original core protocol timed out')
                block=os.read(process.stdout.fileno(),65536)
                if not block:
                    raise RuntimeError('original core closed its protocol')
                pending+=block
                if len(pending)>4194304:
                    raise RuntimeError('original core reply exceeded its limit')
            line,pending=pending.split(b'\n',1)
            return json.loads(line)
        def request(operation,**fields):
            nonlocal sequence
            sequence+=1
            value=dict(op=operation,session=1,epoch=1,seq=sequence,**fields)
            process.stdin.write((json.dumps(value)+'\n').encode())
            process.stdin.flush()
            while True:
                value=receive()
                if value.get('seq')==sequence and value.get('event')!='update':
                    if value.get('error'):
                        raise RuntimeError(value['error'])
                    return value
        failure=None
        try:
            if receive().get('event')!='ready':
                raise RuntimeError('original core did not become ready')
            words=request('hotword_list')['hotwords']
            if seed_common_phrase:
                words=request('hotword_set', key='compat', words='WeTypeX兼容性验证🧪')['hotwords']
                if not any(row.get('words') == 'WeTypeX兼容性验证🧪' for row in words):
                    raise RuntimeError('original core did not save its compatibility phrase')
            canonical=sorted((x.get('id',''),x.get('key',''),x.get('words',''),
                              x.get('kind',0)) for x in words)
            if report is not None:
                report.update(hotword_count=len(words),hotword_sha256=hashlib.sha256(
                    json.dumps(canonical,ensure_ascii=False).encode()).hexdigest())
            request('open')
            for key in 'nihao':
                response=request('key',key=key)
            if not response.get('candidates'):
                raise RuntimeError('original core generated no candidates')
            expected=response['candidates'][0]
            selected=request('select',index=0,revision=response['revision'])
            if selected.get('commits')!=[expected] or selected.get('preedit'):
                raise RuntimeError('original candidate selection did not commit')
            request('reset')
            if user_snapshot is None:
                def select_text(text,**configuration):
                    request('close')
                    request('open',**configuration)
                    for key in text:
                        answer=request('key',key=key)
                    if not answer.get('candidates'):
                        raise RuntimeError('original input mode generated no candidates')
                    selected=request('select',index=0,revision=answer['revision'])
                    if selected.get('commits')!=[answer['candidates'][0]]:
                        raise RuntimeError('original input mode failed to commit')
                for scheme in range(1,8):
                    select_text('riqi',double_scheme=scheme)
                for scheme,text in ((0,'jjad'),(1,'jjdw'),(2,'jjad')):
                    select_text(text,keyboard=5,wubi_solution=scheme)
                request('close');request('open')
                for expression,expected in (('1+1','2'),('0.1+0.2','0.3')):
                    request('reset');request('key',key='v');request('vmode')
                    for key in expression:answer=request('key',key=key)
                    if expected not in answer.get('candidates',[]):
                        raise RuntimeError('original calculator returned an incorrect value')
                    selected=request('select',index=answer['candidates'].index(expected),
                                     revision=answer['revision'])
                    if selected.get('commits')!=[expected]:
                        raise RuntimeError('original calculator failed to commit')
                request('reset')
        except (OSError,ValueError,KeyError,RuntimeError) as error:
            failure=str(error)
        finally:
            process.stdin.close()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill();process.wait()
            process.stdout.close()
        if failure or process.returncode:
            errors.seek(0)
            return subprocess.CompletedProcess(command,process.returncode or 1,
                        stderr=(failure or '').encode()+b'\n'+errors.read())
    return subprocess.CompletedProcess(command,0,stderr=b'')


def validate_runtime(target,user_snapshot=None,report=None,converted_snapshot=None,seed_common_phrase=False,transport_probe=False):
    def failure(label, result):
        data=pathlib.Path(os.environ.get('XDG_CACHE_HOME',str(pathlib.Path.home()/'.cache')))/'fcitx5-wetypex'
        data.mkdir(mode=0o700,parents=True,exist_ok=True)
        path=data/'last-runtime-check.log'
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
        with os.fdopen(fd,'wb') as stream:stream.write(result.stderr[-262144:])
        return False,f'{label} failed (exit {result.returncode}); see {path}'
    support=pathlib.Path(os.environ.get('WETYPE_SUPPORT_DIR','/usr/lib/fcitx5-wetypex'))
    host=support/'wetypex-engine-host'
    locale=support/'locale.so'
    if not locale.exists():locale=support/'wetypex-locale.so'
    required=[host,locale,support/'libkqueue.so',support/'wcwss_bridge.so']
    if any(not path.exists() for path in required):
        return False,'WeTypeX engine host is not installed'
    bwrap=shutil.which('bwrap')
    if not bwrap:
        return False,'bubblewrap is not installed'
    try:
        network_report=subprocess.run([str(host),str(support),'network-info'],
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
        if network_report.returncode:
            return failure('engine host compatibility',network_report)
        capabilities=json.loads(network_report.stdout)
        if not capabilities.get('execution_ready', True):
            return False,'此 ARM64 CPU 需要兼容执行组件，请通过发行版安装 qemu-user 后重试'
        if not capabilities.get('utf8_json',False):
            return False,'UTF-8 JSON compatibility check failed; upgrade the WeTypeX engine host'
        timeout=90 if capabilities.get('execution_provider')=='qemu' else 25
    except (subprocess.TimeoutExpired,ValueError,OSError):
        return False,'unable to check UTF-8 JSON compatibility'
    with tempfile.TemporaryDirectory(prefix='.wetypex-check-') as work:
        command=[bwrap,'--unshare-net','--unshare-pid','--die-with-parent',
                 '--ro-bind','/usr','/usr','--symlink','usr/lib','/lib',
                 '--ro-bind-try','/lib64','/lib64','--symlink','usr/bin','/bin',
                 '--proc','/proc','--dev','/dev','--tmpfs','/tmp',
                 '--ro-bind',str(target),'/input','--ro-bind',str(support),'/support',
                 '--bind',work,'/work','--chdir','/work',
                 '--setenv','PATH','/usr/bin','--setenv','HOME','/work',
                 '--setenv','WETYPE_SUPPORT_DIR','/support',
                 '--setenv', 'WETYPE_ENGINE_PROVIDER', os.environ.get('WETYPE_ENGINE_PROVIDER', 'auto'),
                 '/support/wetypex-engine-host','/input','validate']
        try:
            result=probe_core(command,work,user_snapshot,report,seed_common_phrase,timeout)
        except subprocess.TimeoutExpired:
            return False,'engine validation timed out'
        if result.returncode:
            return failure('engine validation',result)
        if converted_snapshot:
            destination=pathlib.Path(converted_snapshot)
            for name,relative in (('user','user'),('user_dict','userDict'),
                                  ('persistent_data','persistent_data')):
                source=pathlib.Path(work)/relative
                if source.is_dir():
                    shutil.copytree(source,destination/name,symlinks=True)
        transfer_command=command[:-3]+[
            '/support/wetypex-engine-host','/input/flurry','flurry-credentials']
        try:
            transfer=subprocess.run(transfer_command,stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE,timeout=timeout,env={})
        except subprocess.TimeoutExpired:
            return False,'file-transfer runtime validation timed out'
        if transfer.returncode:
            return failure('file-transfer validation',transfer)
        if transport_probe:
            loopback_command=command[:-3]+[
                '/support/wetypex-engine-host','/input/flurry','flurry-loopback']
            try:
                loopback=subprocess.run(loopback_command,stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=timeout,env={})
            except subprocess.TimeoutExpired:
                return False,'local TLS transport validation timed out'
            if loopback.returncode:
                return failure('local TLS transport validation',loopback)
        wxp2p_command=command[:-3]+[
            '/support/wetypex-engine-host','/input/wxp2p','module-run']
        try:
            wxp2p=subprocess.run(wxp2p_command,stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE,timeout=timeout,env={})
        except subprocess.TimeoutExpired:
            return False,'WXP2P runtime validation timed out'
        if wxp2p.returncode:
            return failure('WXP2P validation',wxp2p)
        combined_command=command[:-3]+[
            '--setenv','WETYPE_AUX_RUNTIME','/input/wxp2p',
            '--setenv','WETYPE_FLURRY_DOWNLOAD_DIR','/work/downloads',
            '--setenv','WETYPE_FLURRY_TEMP_DIR','/work/parts',
            '/support/wetypex-engine-host','/input/flurry','flurry-wxp2p-server']
        try:
            combined=subprocess.run(combined_command,input=b'{"action":"quit"}\n',
                                    stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                    timeout=timeout,env={})
        except subprocess.TimeoutExpired:
            return False,'combined WXP2P runtime validation timed out'
        try:
            ready=json.loads(combined.stdout.splitlines()[0])
        except (IndexError,json.JSONDecodeError):
            ready={}
        if combined.returncode or ready.get('event')!='ready' or ready.get('transport')!='wxp2p':
            return False,'combined WXP2P runtime validation failed'
    return True,''


def verify_upgrade(manager, previous_version, download):
    """Verify real persisted data in both directions in an independent sandbox."""
    from runtime_manager import RuntimeManager
    with tempfile.TemporaryDirectory(prefix='wetypex-upgrade-check-') as directory:
        root=pathlib.Path(directory)
        temporary=RuntimeManager(root/'data',root/'data/fcitx5-wetypex/runtime',
                                 validate_runtime,install_ui_resources,install_voice_resources,
                                 manager.ui_required)
        temporary.cache=manager.cache
        temporary.cache_lock=manager.cache_lock
        temporary.cache_reference=manager.cache / ('.retained-' + hashlib.sha256(
            (str(temporary.root) + '\n' + str(temporary.target)).encode()).hexdigest() + '.json')
        previous=temporary.stage(previous_version,download=download)['pending']['generation']
        candidate=temporary.stage(manager.catalog['default'],download=download)['pending']['generation']
        before,after,reverse={},{},{}
        saved,converted=root/'saved',root/'converted'
        saved.mkdir();converted.mkdir()
        ready,error=validate_runtime(temporary.store/previous,report=before,
                                    converted_snapshot=saved,seed_common_phrase=True)
        if not ready:
            raise ValueError(error)
        ready,error=validate_runtime(temporary.store/candidate,saved,after,converted)
        if not ready or before.get('hotword_sha256')!=after.get('hotword_sha256'):
            raise ValueError(error or 'persisted common phrases changed during upgrade')
        ready,error=validate_runtime(temporary.store/previous,converted,reverse)
        if not ready or before.get('hotword_sha256')!=reverse.get('hotword_sha256'):
            raise ValueError(error or 'previous runtime cannot read upgraded user data')
        return dict(ok=True,previous=previous_version,current=manager.catalog['default'],
                    architecture=manager.arch,hotword_count=before['hotword_count'],
                    previous_can_read_upgraded_data=True)

UI_REQUIRED=['icons/input.svg','icons/icon_windows_close_btn.svg',
                     'icons/voice.svg','icons/phrases.svg','icons/appearance.svg',
                     'icons/shortcuts.svg','icons/devices.svg','icons/mobile.svg',
                     'icons/about.svg','icons/icon_sync_device.svg',
                     'icons/icon_transfer_copy.svg','icons/icon_transfer_glossary.svg',
                     'icons/icon_transfer_common.svg','icons/icon_transfer_filetransfer.svg',
                     'icons/icon_entry_ios.svg','icons/icon_entry_android.svg',
                     'images/side_bar_background_light.png',
                     'images/entry_phone_light.png','images/hotword_guide.gif',
                     'images/icon_computer_default_light.png',
                     'images/icon_iphone_default_light.png',
                     'images/icon_Android_default_light.png',
                     'ai-icons/ai-logo-light.svg','ai-icons/ai-polish.svg',
                     'ai-icons/ai-send.svg','ai-icons/chrome-close.png',
                     'candidate-icons/icon_keybar_calculator.svg',
                     'candidate-icons/icon_keybar_clipboard.svg',
                     'candidate-icons/icon_keybar_changyongyu.svg',
                     'candidate-icons/icon_keybar_symbol.svg',
                     'transfer-icons/icon_transmission_send.svg',
                     'transfer-icons/icon_transmission_equipment.svg',
                     'transfer-icons/icon_transmission_others.svg',
                     'transfer-icons/icon_statusbar_setup.svg',
                     'transfer-icons/icon_tips_close.svg',
                     'transfer-icons/icon_filetype_general.svg',
                     'transfer-icons/icon_filetype_folder.svg',
                     'transfer-images/img_connection_pc_to_phone.png']

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive',type=pathlib.Path,help='Verified original archive; cached for future reuse')
    ap.add_argument('--download',action='store_true',help='Download missing verified original archives')
    ap.add_argument('--accept-upstream-license',action='store_true')
    modes=ap.add_mutually_exclusive_group()
    modes.add_argument('--check',action='store_true')
    modes.add_argument('--status',action='store_true')
    modes.add_argument('--update',action='store_true',help='Prepare the default supported runtime')
    modes.add_argument('--activate-pending',action='store_true',help='Apply a prepared update when idle')
    modes.add_argument('--rollback',action='store_true',help='Restore the previous runtime and its data snapshot')
    modes.add_argument('--cleanup',action='store_true',help='Remove unused runtime generations and original ZIP caches')
    modes.add_argument('--icons-only',action='store_true')
    modes.add_argument('--repair-voice-resources',action='store_true',
                       help='Restore original VAD models without switching the input core')
    modes.add_argument('--cache-archive',type=pathlib.Path,help='Import an official ZIP for offline setup')
    modes.add_argument('--query-upstream',action='store_true')
    modes.add_argument('--verify-upgrade-from',metavar='VERSION',
                       help='Check original persisted data upgrade and reverse compatibility in isolation')
    ap.add_argument('--defer',action='store_true',help='Prepare without requesting immediate activation')
    ap.add_argument('--dry-run',action='store_true',help='Preview --cleanup without removing files')
    ap.add_argument('--version',help='Select a version from the installed compatibility catalog')
    ap.add_argument('--upstream-channel',choices=('stable','beta'),default='stable')
    ap.add_argument('--runtime-dir',type=pathlib.Path)
    ap.add_argument('--transport',action='store_true',help='Include real local TLS transfer with --check')
    ap.add_argument('--background',action='store_true',help='Continue an update or cleanup after closing settings')
    ap.add_argument('--operation-fd',type=int,help=argparse.SUPPRESS)
    args=ap.parse_args()
    if args.dry_run and not args.cleanup:
        ap.error('--dry-run requires --cleanup')
    if args.cleanup and (args.archive or args.download or args.version or args.defer):
        ap.error('--cleanup only removes unused local runtime files')
    if args.transport and not args.check:
        ap.error('--transport requires --check')
    if args.query_upstream:
        if args.download or args.archive or args.runtime_dir or args.version or args.defer:
            ap.error('--query-upstream only reads the official release metadata')
        from runtime_catalog import discover
        print(json.dumps(discover(args.upstream_channel),ensure_ascii=False,indent=2))
        return 0
    if args.upstream_channel!='stable':
        ap.error('--upstream-channel applies only to --query-upstream')
    if args.download and not args.accept_upstream_license:
        ap.error('--download requires --accept-upstream-license')
    data=pathlib.Path(os.environ.get('XDG_DATA_HOME',str(pathlib.Path.home()/'.local/share')))
    target=args.runtime_dir or pathlib.Path(os.environ.get('WETYPE_RUNTIME_DIR',str(data/'fcitx5-wetypex/runtime')))
    from runtime_manager import RuntimeManager, digest as archive_digest, lock
    manager=RuntimeManager(data,target,validate_runtime,install_ui_resources,install_voice_resources,UI_REQUIRED)
    if args.background and not (args.update or args.rollback or args.cleanup):
        ap.error('--background requires --update, --rollback or --cleanup')
    global WORKER_STATUS, WORKER_FD
    if args.background:
        import fcntl
        manager.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(manager.root / 'runtime-operation.lock', os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            operation = 'cleanup' if args.cleanup else 'rollback' if args.rollback else 'update'
            from runtime_manager import atomic_json
            atomic_json(manager.root / 'runtime-operation.json',
                        dict(busy=True, operation=operation, started_at=int(time.time())))
            command = [sys.executable, str(pathlib.Path(__file__).resolve()),
                       *(value for value in sys.argv[1:] if value != '--background'),
                       '--operation-fd', str(fd)]
            log = os.open(manager.root / 'runtime-operation.log', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            try:
                child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                         pass_fds=(fd,), start_new_session=True)
            finally:
                os.close(log)
            print(json.dumps(dict(started=True, pid=child.pid)))
            return 0
        finally:
            os.close(fd)
    if args.operation_fd is not None:
        WORKER_FD = args.operation_fd
        os.fstat(WORKER_FD)
        WORKER_STATUS = manager.root / 'runtime-operation.json'
        from runtime_manager import atomic_json, read_json
        operation = read_json(WORKER_STATUS)
        start_ticks = pathlib.Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19]
        operation.update(busy=True, pid=os.getpid(), start_ticks=start_ticks)
        atomic_json(WORKER_STATUS, operation)
    if args.cache_archive:
        if args.download or args.archive or args.version or args.defer or args.background:
            ap.error('--cache-archive only imports a verified local ZIP')
        manager.cache_archive(args.cache_archive)
        print(json.dumps(dict(cached=True)))
        return 0
    if args.cleanup:
        result = manager.cleanup(args.dry_run)
        if WORKER_STATUS is not None:
            from runtime_manager import atomic_json, read_json
            operation = read_json(WORKER_STATUS)
            operation['result'] = result
            atomic_json(WORKER_STATUS, operation)
        print(json.dumps(result,ensure_ascii=False,indent=2))
        return 0
    if args.verify_upgrade_from:
        print(json.dumps(verify_upgrade(manager,args.verify_upgrade_from,args.download),ensure_ascii=False,indent=2))
        return 0
    if args.status:
        print(json.dumps(manager.status(),ensure_ascii=False,indent=2))
        return 0
    if args.check:
        required=('image.macho','manifest.txt','symbols.txt','dicts.txt','resources/index.json','runtime.json',
                  'flurry/image.macho','flurry/manifest.txt','wxp2p/image.macho','wxp2p/manifest.txt')
        missing=[name for name in required if not (target/name).is_file()]
        ui=data/'fcitx5-wetypex/ui'
        missing_ui=[name for name in UI_REQUIRED if not (ui/name).is_file()]
        ready,error=(False,'runtime files are incomplete') if missing else validate_runtime(target,transport_probe=args.transport)
        report=dict(manager.status(),supported_runtime_version=manager.catalog['default'],
                    runtime_version=manager.status()['current'],runtime_installed=not missing,
                    missing_runtime=missing,ui=str(ui),ui_installed=not missing_ui,
                    missing_ui=missing_ui,core_ready=ready,core_error=error,transport_checked=args.transport)
        print(json.dumps(report,ensure_ascii=False,indent=2))
        return 0 if ready and not missing_ui else 1
    if args.activate_pending:
        if args.archive or args.download or args.version:
            ap.error('--activate-pending only applies an already prepared runtime')
        print(json.dumps(manager.activate(),ensure_ascii=False,indent=2))
        return 0
    if args.rollback:
        manager.request_rollback()
        result=manager.status() if args.defer else manager.activate()
        print(json.dumps(result,ensure_ascii=False,indent=2))
        return 0
    if args.icons_only:
        with lock(manager.setup_lock), lock(manager.cache_lock,exclusive=False):
            if args.archive:
                manager.cache_archive(args.archive)
            profile=manager.profile(args.version)
            archive=manager.archive(profile['assets_archive'],args.download)
            install_ui_resources(archive,data)
        print('Original UI assets installed in '+str(data/'fcitx5-wetypex/ui'))
        return 0
    if args.repair_voice_resources:
        installed=manager.status()['current']
        if not installed or (args.version and args.version!=installed):
            ap.error('--repair-voice-resources requires an installed runtime of the selected version')
        try:
            with lock(manager.setup_lock), lock(manager.state/'voice/daemon.lock',blocking=False), \
                    lock(manager.cache_lock,exclusive=False):
                if args.archive:
                    manager.cache_archive(args.archive)
                profile=manager.profile(installed)
                archive=manager.archive(profile['assets_archive'],args.download)
                install_voice_resources(archive,data)
        except BlockingIOError as error:
            raise ValueError('Finish voice input and run fcitx5-wetypex-voice shutdown before repairing models') from error
        print('Original voice models restored in '+str(data/'fcitx5-wetypex/voice/vad'))
        return 0
    if not args.archive and not args.download and not args.update:
        ap.error('Supply --archive FILE or --download, or use --update with cached originals')
    version=args.version
    if args.archive and not version and not args.update:
        value=archive_digest(args.archive)
        match=next((a for a in manager.catalog['archives'].values() if a['sha256']==value),None)
        if match:
            version=match['version']
    manager.stage(version,args.archive,args.download)
    result=dict(manager.status(),cleanup=manager.cleanup(automatic=True)) if args.defer else manager.activate()
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return 0

if __name__=='__main__':
    error_message = ''
    try:code = main()
    except (ValueError,OSError,RuntimeError,subprocess.CalledProcessError) as error:
        error_message = str(error)[:1500]
        print('Setup failed: '+error_message,file=sys.stderr)
        code = 1
    finally:
        if WORKER_STATUS is not None:
            from runtime_manager import atomic_json, read_json
            operation = read_json(WORKER_STATUS)
            operation.update(busy=False, finished_at=int(time.time()), error=error_message)
            atomic_json(WORKER_STATUS, operation)
    raise SystemExit(code)
