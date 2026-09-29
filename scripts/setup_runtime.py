#!/usr/bin/env python3
"""Install a verified, user-supplied original WeType runtime in user data."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

UPSTREAM_VERSION='2.2.3.657'
UPSTREAM_RELEASE,UPSTREAM_BUILD=UPSTREAM_VERSION.rsplit('.',1)
UPSTREAM_ARCHIVE=f'WeType_{UPSTREAM_RELEASE}_{UPSTREAM_BUILD}.zip'
URL=f'https://download.weread.qq.com/app/wxkb/mac/{UPSTREAM_RELEASE}/{UPSTREAM_ARCHIVE}'
ARCHIVE_SHA='8ef48bb21fe9d7b017b8a09fb9496b1b8f960ad960db27872226e08c09db264b'
ENGINE_SHA='a68a92300cc38e5941f59832c516a37eb098c10c6bbe47ab3dcbe812705cf5ee'
FLURRY_SHA='4c97f45b03c511bf0611c7ccc39537b370b4dfba6ba3ba1ba4d9a42e4b6bbd39'
WXP2P_SHA='f99240391cbbb45c8162c9661ec1c7c3932aac53e4c674260b2be1cfcea963d6'

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

def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def validate_runtime(target):
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
    with tempfile.TemporaryDirectory(prefix='.wetypex-check-') as work:
        command=[bwrap,'--unshare-net','--unshare-pid','--die-with-parent',
                 '--ro-bind','/usr','/usr','--symlink','usr/lib','/lib',
                 '--ro-bind','/lib64','/lib64','--symlink','usr/bin','/bin',
                 '--proc','/proc','--dev','/dev','--tmpfs','/tmp',
                 '--ro-bind',str(target),'/input','--ro-bind',str(support),'/support',
                 '--bind',work,'/work','--chdir','/work','--clearenv',
                 '--setenv','PATH','/usr/bin','--setenv','HOME','/work',
                 '--setenv','WETYPE_SUPPORT_DIR','/support',
                 '/support/wetypex-engine-host','/input','validate']
        try:
            result=subprocess.run(command,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE,timeout=20)
        except subprocess.TimeoutExpired:
            return False,'engine validation timed out'
        if result.returncode:
            return False,'engine validation failed'
        transfer_command=command[:-3]+[
            '/support/wetypex-engine-host','/input/flurry','flurry-credentials']
        try:
            transfer=subprocess.run(transfer_command,stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE,timeout=20)
        except subprocess.TimeoutExpired:
            return False,'file-transfer runtime validation timed out'
        if transfer.returncode:
            return False,'file-transfer runtime validation failed'
        wxp2p_command=command[:-3]+[
            '/support/wetypex-engine-host','/input/wxp2p','module-run']
        try:
            wxp2p=subprocess.run(wxp2p_command,stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE,timeout=20)
        except subprocess.TimeoutExpired:
            return False,'WXP2P runtime validation timed out'
        if wxp2p.returncode:
            return False,'WXP2P runtime validation failed'
        combined_command=command[:-3]+[
            '--setenv','WETYPE_AUX_RUNTIME','/input/wxp2p',
            '--setenv','WETYPE_FLURRY_DOWNLOAD_DIR','/work/downloads',
            '--setenv','WETYPE_FLURRY_TEMP_DIR','/work/parts',
            '/support/wetypex-engine-host','/input/flurry','flurry-wxp2p-server']
        try:
            combined=subprocess.run(combined_command,input=b'{"action":"quit"}\n',
                                    stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                    timeout=20)
        except subprocess.TimeoutExpired:
            return False,'combined WXP2P runtime validation timed out'
        try:
            ready=json.loads(combined.stdout.splitlines()[0])
        except (IndexError,json.JSONDecodeError):
            ready={}
        if combined.returncode or ready.get('event')!='ready' or ready.get('transport')!='wxp2p':
            return False,'combined WXP2P runtime validation failed'
    return True,''

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive',type=pathlib.Path,help=f'Original {UPSTREAM_ARCHIVE}')
    ap.add_argument('--download',action='store_true',help='Download the pinned original package from Tencent')
    ap.add_argument('--accept-upstream-license',action='store_true',help='Confirm acceptance of the upstream license before downloading')
    ap.add_argument('--check',action='store_true')
    ap.add_argument('--icons-only',action='store_true',help='Install original icons from the verified archive without replacing the engine')
    ap.add_argument('--runtime-dir',type=pathlib.Path)
    args=ap.parse_args()
    data=pathlib.Path(os.environ.get('XDG_DATA_HOME',str(pathlib.Path.home()/'.local/share')))
    target=args.runtime_dir or pathlib.Path(os.environ.get('WETYPE_RUNTIME_DIR',str(data/'fcitx5-wetypex/runtime')))
    if platform.system()!='Linux' or platform.machine()!='x86_64':ap.error('WeTypeX supports Linux x86-64 only')
    if args.check:
        required=['image.macho','manifest.txt','symbols.txt','dicts.txt','resources/index.json','runtime.json',
                  'flurry/image.macho','flurry/manifest.txt',
                  'wxp2p/image.macho','wxp2p/manifest.txt']
        missing=[name for name in required if not (target/name).is_file()]
        ui=data/'fcitx5-wetypex/ui'
        ui_required=['icons/input.svg','icons/icon_windows_close_btn.svg',
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
        missing_ui=[name for name in ui_required if not (ui/name).is_file()]
        for name in ('fcitx5/themes/wetypex-light/next.svg',
                     'fcitx5/themes/wetypex-light/prev.svg',
                     'fcitx5/themes/wetypex-dark/next.svg',
                     'fcitx5/themes/wetypex-dark/prev.svg'):
            if not (data/name).is_file():missing_ui.append(name)
        core_ready,core_error=(False,'runtime files are incomplete') if missing else validate_runtime(target)
        runtime_version=''
        try:runtime_version=json.loads((target/'runtime.json').read_text()).get('version','')
        except (OSError,json.JSONDecodeError,AttributeError):pass
        print(json.dumps({'supported_runtime_version':UPSTREAM_VERSION,
                          'runtime_version':runtime_version,
                          'runtime':str(target),'runtime_installed':not missing,
                          'missing_runtime':missing,'ui':str(ui),
                          'ui_installed':not missing_ui,'missing_ui':missing_ui,
                          'core_ready':core_ready,'core_error':core_error},
                         ensure_ascii=False,indent=2))
        return 1 if missing or missing_ui or not core_ready else 0
    if not args.archive and not args.download:ap.error('Supply --archive FILE or explicitly request --download')
    if args.archive and args.download:ap.error('Choose one source')
    if args.download and not args.accept_upstream_license:
        ap.error('--download requires --accept-upstream-license; alternatively supply an existing --archive file')
    target.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.wetype-setup-',dir=target.parent) as temporary:
        temp=pathlib.Path(temporary)
        archive=args.archive
        if args.download:
            archive=temp/'original.zip';print('Downloading the pinned original package…',flush=True)
            with urllib.request.urlopen(URL,timeout=60) as response,archive.open('wb') as out:shutil.copyfileobj(response,out)
        if digest(archive)!=ARCHIVE_SHA:raise ValueError(f'Archive SHA-256 mismatch; expected the verified {UPSTREAM_VERSION} package')
        install_ui_resources(archive,data)
        if args.icons_only:
            print('Original UI assets installed in '+str(data/'fcitx5-wetypex/ui'))
            return 0
        runtime=temp/'runtime';runtime.mkdir();resources=runtime/'resources';resources.mkdir()
        original=temp/'WeType'
        with zipfile.ZipFile(archive) as z:
            original.write_bytes(z.read('WeType.app/Contents/MacOS/WeType'))
            flurry=temp/'flurry'
            flurry.write_bytes(z.read(
                'WeType.app/Contents/Frameworks/flurry.framework/Versions/A/flurry'))
            wxp2p=temp/'WXP2PTransferDyn'
            wxp2p.write_bytes(z.read(
                'WeType.app/Contents/Frameworks/WXP2PTransferDyn.framework/Versions/A/WXP2PTransferDyn'))
            prefix='WeType.app/Contents/Resources/imeData.bundle/'
            for name in z.namelist():
                if not name.startswith(prefix) or name.endswith('/'):continue
                relative=name[len(prefix):]
                if '/' in relative or relative in ('.','..'):raise ValueError('Unexpected resource path')
                (resources/relative).write_bytes(z.read(name))
        if digest(original)!=ENGINE_SHA:raise ValueError('Engine SHA-256 mismatch')
        prepare=pathlib.Path(__file__).with_name('prepare_macos_runtime.py')
        if not prepare.exists():prepare=pathlib.Path(__file__).resolve().parent.parent/'tools/prepare_macos_runtime.py'
        subprocess.run([sys.executable,str(prepare),str(original),str(runtime)],check=True)
        subprocess.run([sys.executable,str(prepare),str(flurry),str(runtime/'flurry'),
                        '--expected-sha256',FLURRY_SHA,'--slide','0x103000000',
                        '--export-prefix','__ZN6flurry'],check=True)
        subprocess.run([sys.executable,str(prepare),str(wxp2p),str(runtime/'wxp2p'),
                        '--expected-sha256',WXP2P_SHA,'--slide','0x104000000',
                        '--export-prefix','__ZN13wxp2ptransfer'],check=True)
        rows=json.loads((resources/'index.json').read_text())
        for row in rows:
            name=row['name']
            if pathlib.Path(name).name!=name:raise ValueError('Invalid dictionary path')
            with (resources/name).open('rb') as f:
                if hashlib.file_digest(f,'md5').hexdigest()!=row['md5']:raise ValueError('Dictionary digest mismatch: '+name)
        (runtime/'dicts.txt').write_text('\n'.join(f"{r['type']} {r['version']} {r['name']}" for r in rows if not r['is_cell'])+'\n')
        (runtime/'runtime.json').write_text(json.dumps({'version':UPSTREAM_VERSION,'engine_sha256':ENGINE_SHA,'archive_sha256':ARCHIVE_SHA,'source':URL,'protocol':1},indent=2)+'\n')
        if target.exists():
            old=target.with_name(target.name+'.previous')
            if old.exists():shutil.rmtree(old)
            target.rename(old)
        runtime.rename(target)
        old=target.with_name(target.name+'.previous')
        if old.exists():shutil.rmtree(old)
    print('Original runtime prepared: '+str(target))
    print('Add “WeTypeX” in Fcitx5 settings.')
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,subprocess.CalledProcessError) as error:
        print('Setup failed: '+str(error),file=sys.stderr);raise SystemExit(1)
