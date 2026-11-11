#!/usr/bin/env python3
"""Render reproducible upstream-only updates and verify the bot's merge gate.

No original executable is loaded by this tool. Runtime checks run separately,
on read-only Actions workers. Discovery, verification and publication are opt-in.
"""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))
from runtime_catalog import discover, validate_url, DOWNLOAD_HOSTS, download_archive
from prepare_macos_runtime import select_slice

CATALOG = 'data/runtime-profiles.json'
PATHS = (CATALOG, 'CMakeLists.txt', 'packaging/PKGBUILD.in',
         'packaging/aur/PKGBUILD', 'README.md')
VERIFY_WORKFLOW = 'upstream-verify.yml'
COMPLETE_WORKFLOW = 'upstream-complete.yml'
MAX_AUTOMATIC_ATTEMPTS = 3
REQUIRED_JOBS = frozenset((
    'packages / arch-linux', 'packages / ubuntu-jammy-deb',
    'packages / ubuntu-focal-legacy', 'packages / arm64 / packages',
    'packages / nix (ubuntu-24.04)', 'packages / nix (ubuntu-24.04-arm)',
    'packages / deb-runtime (debian:bookworm-slim)',
    'packages / deb-runtime (debian:trixie-slim)',
    'packages / deb-runtime (ubuntu:22.04)',
    'packages / deb-runtime (ubuntu:24.04)',
    'packages / deb-runtime (ubuntu:26.04)',
    'runtime (ubuntu-24.04)', 'runtime (ubuntu-24.04-arm)',
))


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', value):
        raise ValueError('invalid original version')
    return tuple(map(int, value.split('.')))


def inspect_archive(path, metadata):
    validate_url(metadata['archive_url'], DOWNLOAD_HOSTS)
    version(metadata['version'])
    sha, md5 = hashlib.sha256(), hashlib.md5()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
            md5.update(block)
    if md5.hexdigest() != metadata['archive_md5']:
        raise ValueError('official archive MD5 mismatch')
    with zipfile.ZipFile(path) as source:
        member = 'WeType.app/Contents/MacOS/WeType'
        if sum(row.filename == member for row in source.infolist()) != 1:
            raise ValueError('original archive has a missing or duplicate core')
        info = source.getinfo(member)
        if info.file_size > 512 * 1024 * 1024:
            raise ValueError('original core exceeds its size limit')
        raw = source.read(member)
        prefix = 'WeType.app/Contents/Resources/imeData.bundle/'
        dictionaries = {}
        for info in source.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            name = info.filename[len(prefix):]
            if (Path(name).name != name or name in ('', '.', '..') or name in dictionaries or
                    info.file_size > 512 * 1024 * 1024):
                raise ValueError('invalid original dictionary member')
            value = hashlib.sha256()
            with source.open(info) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    value.update(block)
            dictionaries[name] = dict(sha256=value.hexdigest(), size=info.file_size)
        if 'index.json' not in dictionaries:
            raise ValueError('original dictionaries are missing')
    return dict(version=metadata['version'], url=metadata['archive_url'], sha256=sha.hexdigest(),
                dictionaries=dict(sorted(dictionaries.items())),
                metadata_url=metadata['metadata_url'], md5=md5.hexdigest()), dict(
        member=member, sha256=hashlib.sha256(raw).hexdigest(),
        slices={arch: hashlib.sha256(select_slice(raw, arch)[0]).hexdigest()
                for arch in ('x86_64', 'arm64')})


def download(metadata, directory):
    class OfficialRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, fp, code, message, headers, url):
            validate_url(url, DOWNLOAD_HOSTS, allow_cdn=True)
            self.parent.archive_last_host = urllib.parse.urlsplit(url).hostname
            return super().redirect_request(request, fp, code, message, headers, url)
    validate_url(metadata['archive_url'], DOWNLOAD_HOSTS)
    target = Path(directory) / 'official.zip'
    with target.open('wb') as output:
        download_archive(urllib.request.build_opener(OfficialRedirect()), metadata['archive_url'], output)
    return target


def render(files, metadata, archive):
    """Change only a core profile and its adapter release, keeping pinned history."""
    result = dict(files)
    catalog = json.loads(files[CATALOG])
    old, new = catalog['default'], metadata['version']
    if version(new) <= version(old):
        return result
    package, component = inspect_archive(archive, metadata)
    archive_id = 'official-' + new + '-' + package['sha256'][:12]
    engine_id = archive_id + '-engine'
    component['archive'] = archive_id
    for table, key, value in (('archives', archive_id, package), ('components', engine_id, component)):
        if key in catalog[table] and catalog[table][key] != value:
            raise ValueError('an immutable catalog entry would change')
        catalog[table][key] = value
    profile = copy.deepcopy(catalog['profiles'][old])
    profile.update(version=new, dictionaries_archive=archive_id)
    profile['components']['engine'] = engine_id
    if new in catalog['profiles']:
        # Promotion of an already validated beta still uses the stable archive.
        known = catalog['components'][catalog['profiles'][new]['components']['engine']]
        if known['sha256'] != component['sha256']:
            raise ValueError('same-version upstream replacement requires manual review')
    catalog['profiles'][new] = profile
    catalog['default'] = new
    result[CATALOG] = json.dumps(catalog, ensure_ascii=False, indent=2) + '\n'
    project = re.search(r'^project\(fcitx5-wetypex VERSION ([0-9.]+)', files['CMakeLists.txt'], re.M)
    if not project:
        raise ValueError('adapter project version is missing')
    previous = project.group(1)
    result['CMakeLists.txt'] = files['CMakeLists.txt'].replace(
        'project(fcitx5-wetypex VERSION ' + previous, 'project(fcitx5-wetypex VERSION ' + new, 1)
    for path in ('packaging/PKGBUILD.in', 'packaging/aur/PKGBUILD'):
        result[path] = re.sub(r'^pkgver=.*$', 'pkgver=' + new, files[path], flags=re.M)
        result[path] = re.sub(r'^pkgrel=\d+$', 'pkgrel=1', result[path], flags=re.M)
    old_release = re.search(r'^pkgrel=(\d+)$', files['packaging/PKGBUILD.in'], re.M)
    if not old_release:
        raise ValueError('adapter package revision is missing')
    result['README.md'] = files['README.md'].replace(previous + '-' + old_release.group(1), new + '-1').replace(previous, new)
    return result


def api(path, method='GET', data=None):
    repository = os.environ['GITHUB_REPOSITORY']
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('invalid GitHub repository')
    request = urllib.request.Request('https://api.github.com/repos/' + repository + path,
        method=method, data=None if data is None else json.dumps(data).encode(), headers={
            'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
            'Accept': 'application/vnd.github+json', 'User-Agent': 'WeTypeX upstream updater',
            'X-GitHub-Api-Version': '2022-11-28', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        content = response.read(4 * 1024 * 1024 + 1)
    if len(content) > 4 * 1024 * 1024:
        raise ValueError('GitHub response exceeds its size limit')
    return json.loads(content) if content else {}


def optional_api(path):
    try:
        return api(path)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        return None


def pages(path, key=None):
    result = []
    separator = '&' if '?' in path else '?'
    for page in range(1, 21):
        value = api(path + separator + 'per_page=100&page=' + str(page))
        batch = value[key] if key else value
        result.extend(batch)
        if len(batch) < 100:
            return result
    raise ValueError('GitHub result requires manual pagination beyond 2000 entries')


def git(*arguments, input=None, env=None):
    process_env = os.environ.copy()
    process_env.update(GIT_TERMINAL_PROMPT='0', GIT_TRACE='0', GIT_TRACE_CURL='0',
                       GIT_CURL_VERBOSE='0', GIT_TRACE_PACKET='0')
    if env:
        process_env.update(env)
    token = os.environ.get('GH_TOKEN')
    if token:
        count = int(process_env.get('GIT_CONFIG_COUNT', '0'))
        process_env['GIT_CONFIG_COUNT'] = str(count + 1)
        process_env['GIT_CONFIG_KEY_' + str(count)] = 'http.https://github.com/.extraheader'
        credential = base64.b64encode(('x-access-token:' + token).encode()).decode()
        process_env['GIT_CONFIG_VALUE_' + str(count)] = 'Authorization: Basic ' + credential
    result = subprocess.run(['git', *arguments], input=input, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=process_env)
    if result.returncode:
        raise RuntimeError('git operation failed: ' + result.stderr[-1200:])
    return result.stdout.strip()


def remote_url():
    return 'https://github.com/' + os.environ['GITHUB_REPOSITORY'] + '.git'


def generated_commit(base, files, changed, original_version):
    """Create a single commit without checking out or running candidate files."""
    git('fetch', '--no-tags', remote_url(), base)
    with tempfile.TemporaryDirectory(prefix='wetypex-upstream-index-') as directory:
        env = dict(GIT_INDEX_FILE=str(Path(directory) / 'index'),
                   GIT_AUTHOR_NAME='github-actions[bot]',
                   GIT_AUTHOR_EMAIL='41898282+github-actions[bot]@users.noreply.github.com',
                   GIT_COMMITTER_NAME='github-actions[bot]',
                   GIT_COMMITTER_EMAIL='41898282+github-actions[bot]@users.noreply.github.com')
        git('read-tree', base, env=env)
        for path in changed:
            blob = git('hash-object', '-w', '--stdin', input=files[path])
            git('update-index', '--add', '--cacheinfo', '100644', blob, path, env=env)
        tree = git('write-tree', env=env)
        return git('commit-tree', tree, '-p', base,
                   input='Update original core to ' + original_version + '\n', env=env)


def sealed_metadata(files):
    catalog = json.loads(files[CATALOG])
    profile = catalog['profiles'][catalog['default']]
    package = catalog['archives'][catalog['components'][profile['components']['engine']]['archive']]
    version(package['version'])
    validate_url(package['url'], DOWNLOAD_HOSTS)
    validate_url(package['metadata_url'], DOWNLOAD_HOSTS)
    if not re.fullmatch(r'[a-f0-9]{32}', package.get('md5', '')):
        raise ValueError('generated archive has no verified official digest')
    return dict(version=package['version'], archive_url=package['url'],
                archive_md5=package['md5'], metadata_url=package['metadata_url'])


def bot_pr(pr):
    repository = os.environ['GITHUB_REPOSITORY']
    return (pr['user']['login'] == 'github-actions[bot]' and
            pr['head'].get('repo') is not None and
            pr['head']['repo']['full_name'] == repository and
            pr['base']['ref'] == 'main' and not pr['draft'])


def canonical_candidate(head, archive, pr=None):
    commit = api('/git/commits/' + head)
    if len(commit['parents']) != 1:
        raise ValueError('the generated branch must contain one commit on its base')
    base = commit['parents'][0]['sha']
    changed = (pages('/pulls/' + str(pr['number']) + '/files') if pr else
               api('/compare/' + base + '...' + head)['files'])
    if (not changed or len(changed) > len(PATHS) or
            (pr and len(changed) != pr['changed_files']) or any(
                row['filename'] not in PATHS or row['status'] != 'modified' for row in changed)):
        raise ValueError('the branch contains changes outside the generated update')
    before, after = remote_files(base), remote_files(head)
    verify_generated_modes(head)
    metadata = sealed_metadata(after)
    expected = render(before, metadata, archive)
    if expected == before or expected != after:
        raise ValueError('the branch does not exactly match the canonical generated update')
    return base, after


def verification_runs(branch, head):
    query = urllib.parse.urlencode(dict(branch=branch, event='workflow_dispatch', head_sha=head))
    return pages('/actions/workflows/' + VERIFY_WORKFLOW + '/runs?' + query, 'workflow_runs')


def request_verification(branch, head, base, dry_run=False):
    runs = [row for row in verification_runs(branch, head)
            if row['display_title'] == 'upstream ' + head + ' base ' + base]
    if any(row['status'] != 'completed' or row['conclusion'] == 'success' for row in runs):
        return
    if len(runs) >= MAX_AUTOMATIC_ATTEMPTS:
        raise ValueError('verification repeatedly failed; inspect it before retrying manually')
    if not dry_run:
        api('/actions/workflows/' + VERIFY_WORKFLOW + '/dispatches', 'POST',
            dict(ref=branch, inputs=dict(expected_head=head, expected_base=base)))


def release_files(tag):
    match = re.fullmatch(r'v(\d+\.\d+\.\d+\.\d+)-(\d+)', tag)
    if not match:
        raise ValueError('invalid adapter release tag')
    core, revision = match.groups()
    release = core + '-' + revision
    result = {'SHA256SUMS', 'fcitx5-wetypex-' + core + '.tar.gz'}
    for native, deb in (('x86_64', 'amd64'), ('aarch64', 'arm64')):
        result.update((f'fcitx5-wetypex-{release}-{native}.pkg.tar.zst',
                       f'fcitx5-wetypex_{release}_{deb}.deb',
                       f'fcitx5-wetypex-legacy_{release}_{deb}.deb',
                       f'fcitx5-wetypex-{release}.{native}.rpm',
                       f'fcitx5-wetypex-{release}-linux-{native}.tar.zst'))
    return result


def release_complete(tag):
    release = optional_api('/releases/tags/' + tag)
    if not release or release['draft'] or release['prerelease']:
        return False
    from publish_release import remote_manifest
    try:
        return remote_manifest(release, tag) is not None
    except (ValueError, OSError):
        return False


def release_runs(tag):
    return [row for row in pages('/actions/workflows/release.yml/runs', 'workflow_runs')
            if row['display_title'] == 'WeTypeX release ' + tag]


def request_release(tag):
    runs = release_runs(tag)
    if any(row['status'] != 'completed' for row in runs):
        return
    if release_complete(tag) and any(row['conclusion'] == 'success' for row in runs):
        return
    if runs and runs[0]['conclusion'] != 'success':
        latest = runs[0]
        if latest['conclusion'] == 'cancelled':
            raise ValueError('publication was cancelled; resume that run manually')
        if latest.get('run_attempt', 1) >= MAX_AUTOMATIC_ATTEMPTS:
            raise ValueError('publication repeatedly failed; inspect the run before retrying')
        jobs = pages('/actions/runs/' + str(latest['id']) + '/jobs?filter=all', 'jobs')
        if any(job['name'] == 'packages' for job in jobs):
            api('/actions/runs/' + str(latest['id']) + '/rerun-failed-jobs', 'POST')
        else:
            # Old workflow revisions combined compilation and publication.
            # Recover their sealed artifacts with the fixed control workflow.
            built = any(job['name'] == 'release' and
                        {'为全部架构生成校验和', '保存构建产物'} <= {
                            step['name'] for step in job['steps'] if step['conclusion'] == 'success'}
                        for job in jobs)
            inputs = dict(tag=tag, only_aur=False)
            if built:
                inputs['recover_run'] = str(latest['id'])
            api('/actions/workflows/release.yml/dispatches', 'POST', dict(ref='main', inputs=inputs))
    else:
        api('/actions/workflows/release.yml/dispatches', 'POST',
            dict(ref='main', inputs=dict(tag=tag, only_aur=False)))


def reconcile(dry_run=False, archive_path=None, base_ref=None):
    """Discover, rebase only untouched bot proposals, and resume interrupted work."""
    metadata = discover('stable')
    base = base_ref or api('/git/ref/heads/main')['object']['sha']
    before = remote_files(base)
    current = json.loads(before[CATALOG])['default']
    branch = 'upstream/runtime-' + metadata['version']
    repository = os.environ['GITHUB_REPOSITORY']
    query = urllib.parse.urlencode(dict(state='all', base='main',
                                        head=repository.split('/')[0] + ':' + branch))
    candidates = pages('/pulls?' + query)
    if version(metadata['version']) <= version(current):
        if metadata['version'] == current:
            sealed = sealed_metadata(before)
            if any(metadata[key] != sealed[key] for key in ('archive_url', 'archive_md5')):
                raise ValueError('same-version upstream replacement requires manual review')
            if os.environ.get('WETYPEX_UPSTREAM_AUTOMATION') == 'release':
                for row in candidates:
                    pr = api('/pulls/' + str(row['number']))
                    if bot_pr(pr) and pr.get('merged'):
                        runs = verification_runs(branch, pr['head']['sha'])
                        parents = api('/git/commits/' + pr['head']['sha'])['parents']
                        title = ('upstream ' + pr['head']['sha'] + ' base ' + parents[0]['sha']) if len(parents) == 1 else ''
                        successful = next((run for run in runs if run['conclusion'] == 'success' and
                                           run['display_title'] == title), None)
                        tag = 'v' + current + '-1'
                        publications = release_runs(tag)
                        settled = release_complete(tag) and any(row['conclusion'] == 'success' for row in publications)
                        active = any(row['status'] != 'completed' for row in publications)
                        if successful and not settled and not active and not dry_run:
                            api('/actions/workflows/' + COMPLETE_WORKFLOW + '/dispatches', 'POST',
                                dict(ref='main', inputs=dict(verification_run=str(successful['id']), publish=True)))
                        break
        print(json.dumps(dict(version=metadata['version'], action='up_to_date', dry_run=dry_run)))
        return
    if candidates:
        pr = api('/pulls/' + str(candidates[0]['number']))
        if not bot_pr(pr) or pr['state'] != 'open':
            print(json.dumps(dict(version=metadata['version'], action='manual_pr_left_unchanged')))
            return
    else:
        pr = None
    reference = optional_api('/git/ref/heads/' + branch)
    previous = reference['object']['sha'] if reference else None
    if pr and previous != pr['head']['sha']:
        raise ValueError('the proposal changed during discovery')
    with tempfile.TemporaryDirectory(prefix='wetypex-upstream-discovery-') as directory:
        archive = archive_path or download(metadata, directory)
        if previous:
            old_base, after = canonical_candidate(previous, archive, pr)
            ancestry = api('/compare/' + old_base + '...' + base)['status']
            if ancestry not in ('ahead', 'identical'):
                raise ValueError('the proposal base is not in main history')
            sealed = sealed_metadata(after)
            if any(metadata[key] != sealed[key] for key in ('version', 'archive_url', 'archive_md5')):
                raise ValueError('the original package changed after the proposal was created')
        updated = render(before, metadata, archive)
    changed = [path for path in PATHS if before[path] != updated[path]]
    print(json.dumps(dict(version=metadata['version'], branch=branch, base=base,
                          action='rebase' if previous else 'create', changed=changed,
                          dry_run=dry_run)))
    if dry_run:
        return
    if previous and old_base == base:
        head = previous
    else:
        head = generated_commit(base, updated, changed, metadata['version'])
        ref = 'refs/heads/' + branch
        git('push', '--force-with-lease=' + ref + ':' + (previous or ''),
            remote_url(), head + ':' + ref)
    if not pr:
        pr = api('/pulls', 'POST', dict(title='Update original core to ' + metadata['version'],
            head=branch, base='main', body='更新原版输入核心和词典，保留已验证的传输模块与旧版回退档案。\n\n'
            '验证覆盖 x86-64、ARM64、旧系统软件包、原版核心、TLS、语音和用户数据迁移。'))
    request_verification(branch, head, base)


def remote_files(ref):
    result = {}
    for path in PATHS:
        item = api('/contents/' + path + '?ref=' + ref)
        if item.get('encoding') != 'base64' or item.get('size', 0) > 1024 * 1024:
            raise ValueError('unexpected candidate file encoding or size')
        result[path] = base64.b64decode(item['content'], validate=False).decode()
    return result


def verify_generated_modes(head):
    tree = api('/git/trees/' + head + '?recursive=1')
    entries = {row['path']: row for row in tree['tree']}
    if tree.get('truncated') or any(entries.get(path, {}).get('mode') != '100644' for path in PATHS):
        raise ValueError('generated files must remain ordinary non-executable files')


def complete(run_id, publish, archive_path=None):
    """Use trusted main code and API data only; never checkout a candidate."""
    run = api('/actions/runs/' + str(int(run_id)))
    repository = os.environ['GITHUB_REPOSITORY']
    if (run['event'] != 'workflow_dispatch' or run['status'] != 'completed' or
            run['conclusion'] != 'success' or
            run['head_repository']['full_name'] != repository or
            run['path'] != '.github/workflows/' + VERIFY_WORKFLOW or
            not re.fullmatch(r'upstream/runtime-\d+\.\d+\.\d+\.\d+', run['head_branch'])):
        raise ValueError('this is not a successful upstream verification run')
    candidates = api('/pulls?state=all&base=main&head=' + repository.split('/')[0] + ':' + run['head_branch'])
    candidates = [pr for pr in candidates if pr['head']['sha'] == run['head_sha']]
    if len(candidates) != 1:
        raise ValueError('the verification run has no unique open bot PR')
    pr = api('/pulls/' + str(candidates[0]['number']))
    head = pr['head']['sha']
    title = re.fullmatch(r'upstream ([0-9a-f]{40}) base ([0-9a-f]{40})', run['display_title'])
    if title is None or title.group(1) != head:
        raise ValueError('the verification run has no immutable head/base identity')
    base = title.group(2)
    main_head = api('/git/ref/heads/main')['object']['sha']
    already_merged = bool(pr.get('merged')) or main_head == head
    if [parent['sha'] for parent in api('/git/commits/' + head)['parents']] != [base]:
        raise ValueError('the candidate must be one generated commit on the verified base')
    if (pr['user']['login'] != 'github-actions[bot]' or pr['head']['repo']['full_name'] != repository or
            pr['draft'] or head != run['head_sha']):
        raise ValueError('the bot candidate changed or its base is no longer current')
    if not already_merged and (pr['state'] != 'open' or main_head != base):
        raise ValueError('the verified base is stale; rebase and rerun the complete matrix')
    changed = api('/pulls/' + str(pr['number']) + '/files?per_page=100')
    if len(changed) != pr['changed_files'] or any(
            item['filename'] not in PATHS or item['status'] != 'modified' for item in changed):
        raise ValueError('automatic updates may only contain the generated version/profile files')
    before, after = remote_files(base), remote_files(head)
    verify_generated_modes(head)
    metadata = discover('stable')
    target = sealed_metadata(after)
    if run['head_branch'] != 'upstream/runtime-' + target['version']:
        raise ValueError('verification branch does not match the generated version')
    if already_merged:
        # The sealed profile survives a moved/removed version-specific JSON.
        # An official rollback still requires maintainer review.
        if version(metadata['version']) < version(target['version']):
            raise ValueError('the official stable channel withdrew this version')
        if metadata['version'] == target['version'] and any(
                metadata[key] != target[key] for key in ('archive_url', 'archive_md5')):
            raise ValueError('the published original changed after verification')
        metadata = target
    elif any(metadata[key] != target[key] for key in target):
        raise ValueError('the proposal no longer matches current official stable metadata')
    with tempfile.TemporaryDirectory(prefix='wetypex-upstream-review-') as directory:
        expected = render(before, metadata, archive_path or download(metadata, directory))
    if expected == before or after != expected:
        raise ValueError('the PR does not exactly match the independently rendered official update')
    # Check every job in the dispatched verification, including all reusable
    # old-system/package checks. A partially successful matrix cannot publish.
    jobs = []
    page = 1
    while True:
        batch = api('/actions/runs/' + str(run['id']) + '/jobs?per_page=100&page=' + str(page))['jobs']
        jobs.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    names = {job['name'] for job in jobs}
    if not REQUIRED_JOBS <= names or any(job['conclusion'] != 'success' for job in jobs):
        raise ValueError('the complete verification/package matrix must succeed')
    release = metadata['version'] + '-1'
    print(json.dumps(dict(pr=pr['number'], head=head, release=release, publish=publish)))
    if not publish:
        return
    # Each generated proposal is already a single commit on the verified base.
    # A non-forced fast-forward atomically rejects a racing main update, unlike
    # the PR merge API (which accepts an expected head but no expected base).
    if not already_merged and api('/git/ref/heads/main')['object']['sha'] != base:
        raise ValueError('main advanced; dispatch verification against the new base')
    if not already_merged:
        latest_pr = api('/pulls/' + str(pr['number']))
        if latest_pr['head']['sha'] != head or latest_pr['state'] != 'open' or not bot_pr(latest_pr):
            raise ValueError('the proposal was edited during verification')
        git('fetch', '--no-tags', remote_url(), head)
        git('push', remote_url(), head + ':refs/heads/main')
    applied = pr['merge_commit_sha'] if pr.get('merged') else head
    commit = api('/git/commits/' + applied)
    if ([parent['sha'] for parent in commit['parents']] != [base] or
            commit['tree']['sha'] != api('/git/commits/' + head)['tree']['sha']):
        raise ValueError('the merge base changed during merge; rerun verification before publication')
    main_head = api('/git/ref/heads/main')['object']['sha']
    if api('/compare/' + applied + '...' + main_head)['status'] not in ('ahead', 'identical'):
        raise ValueError('the verified commit is no longer in main history')
    if api('/pulls/' + str(pr['number']))['head']['sha'] != head:
        raise ValueError('the proposal was edited while merging; publication stopped')
    tag = 'v' + release
    try:
        existing = api('/git/ref/tags/' + tag)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        existing = None
    if existing:
        target_ref = existing['object']
        for _ in range(5):
            if target_ref['type'] != 'tag':
                break
            target_ref = api('/git/tags/' + target_ref['sha'])['object']
        if target_ref['type'] != 'commit' or target_ref['sha'] != applied:
            raise ValueError('the release tag already identifies another commit')
    else:
        api('/git/refs', 'POST', dict(ref='refs/tags/' + tag, sha=applied))
    request_release(tag)


def release_version(tag):
    match = re.fullmatch(r'v?(\d+\.\d+\.\d+\.\d+)-(\d+)', tag or '')
    return (*version(match[1]), int(match[2])) if match else ()


def release_policy(tag):
    target = release_version(tag)
    if not target:
        raise ValueError('invalid adapter release tag')
    newer = any(not row['draft'] and not row['prerelease'] and
                release_version(row['tag_name']) > target for row in pages('/releases'))
    print(json.dumps(dict(make_latest='false' if newer else 'legacy', newer_release_exists=newer)))


def verify_release_directory(directory, tag):
    """Validate every distributed architecture, then write one checksum manifest."""
    root = Path(directory).resolve()
    names = release_files(tag) - {'SHA256SUMS'}
    files = {}
    for parent in (root, *(root / name for name in ('arch', 'debian', 'ubuntu20', 'fedora'))):
        for path in parent.glob('fcitx5-wetypex*'):
            if path.is_file() and path.suffix in ('.zst', '.gz', '.deb', '.rpm'):
                if (path.name not in names or path.name in files or path.is_symlink() or
                        root not in path.resolve().parents or not path.stat().st_size):
                    raise ValueError('unexpected, duplicate or empty release artifact: ' + str(path))
                files[path.name] = path
    if files.keys() != names:
        raise ValueError('missing release artifacts: ' + ', '.join(sorted(names - files.keys())))
    expected = tag.removeprefix('v') if sys.version_info >= (3, 9) else tag[1:]
    for name, path in files.items():
        if name.endswith('.deb'):
            arch = 'arm64' if name.endswith('_arm64.deb') else 'amd64'
            actual = [subprocess.check_output(['dpkg-deb', '-f', str(path), field], text=True).strip()
                      for field in ('Version', 'Architecture')]
            if actual != [expected, arch]:
                raise ValueError('Debian package metadata mismatch: ' + name)
        elif name.endswith('.rpm'):
            arch = 'aarch64' if name.endswith('.aarch64.rpm') else 'x86_64'
            command = (['rpm', '-qp', '--queryformat', '%{VERSION}-%{RELEASE}\n%{ARCH}', str(path)]
                       if shutil.which('rpm') else
                       ['docker', 'run', '--rm', '-v', str(root) + ':/out:ro',
                        'wetypex-build-fedora', 'rpm', '-qp', '--queryformat',
                        '%{VERSION}-%{RELEASE}\n%{ARCH}', '/out/' + str(path.relative_to(root))])
            actual = subprocess.check_output(command, text=True).strip().splitlines()
            if actual != [expected, arch]:
                raise ValueError('RPM package metadata mismatch: ' + name)
        elif name.endswith('.pkg.tar.zst'):
            content = subprocess.check_output(['tar', '--zstd', '-xOf', str(path), '.PKGINFO'], text=True)
            arch = 'aarch64' if name.endswith('-aarch64.pkg.tar.zst') else 'x86_64'
            if ('pkgver = ' + expected) not in content.splitlines() or ('arch = ' + arch) not in content.splitlines():
                raise ValueError('Arch package metadata mismatch: ' + name)
        elif name.endswith('.tar.zst'):
            binary = subprocess.check_output(['tar', '--zstd', '-xOf', str(path),
                                               './usr/lib/fcitx5-wetypex/wetypex-engine-host'])
            machine = 183 if name.endswith('-aarch64.tar.zst') else 62
            if binary[:6] != b'\x7fELF\x02\x01' or int.from_bytes(binary[18:20], 'little') != machine:
                raise ValueError('portable archive architecture mismatch: ' + name)
    manifest = ''.join(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + name + '\n'
                       for name, path in sorted(files.items()))
    temporary = root / '.SHA256SUMS.tmp'
    temporary.write_text(manifest)
    temporary.replace(root / 'SHA256SUMS')
    print(json.dumps(dict(tag=tag, verified_artifacts=sorted(files))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Reuse a downloaded official ZIP')
    parser.add_argument('--write', action='store_true', help='Write only the generated local files')
    parser.add_argument('--complete-run', type=int)
    parser.add_argument('--reconcile', action='store_true', help='Maintain canonical bot PRs and resume interrupted publication')
    parser.add_argument('--dry-run', action='store_true', help='Read-only discovery/reconciliation')
    parser.add_argument('--base-ref', help='Inspect an immutable base commit with --reconcile --dry-run')
    parser.add_argument('--release-complete', metavar='TAG', help='Exit zero only for a complete published release')
    parser.add_argument('--release-policy', metavar='TAG', help='Choose latest-release policy without downgrading a newer release')
    parser.add_argument('--verify-release-dir', type=Path, help='Validate all built packages and generate SHA256SUMS')
    parser.add_argument('--tag', help='Adapter release tag for --verify-release-dir')
    parser.add_argument('--publish', action='store_true', help='Merge/tag only after the complete gate succeeds')
    args = parser.parse_args()
    actions = (args.complete_run, args.reconcile, args.release_complete,
               args.release_policy, args.verify_release_dir)
    if sum(bool(value) for value in actions) > 1:
        parser.error('choose one automation operation')
    if args.publish and (not args.complete_run or args.archive):
        parser.error('--publish requires --complete-run and a fresh official download')
    if args.dry_run and not args.reconcile:
        parser.error('--dry-run requires --reconcile')
    if args.base_ref and (not args.reconcile or not args.dry_run or
                          not re.fullmatch(r'[0-9a-f]{40}', args.base_ref)):
        parser.error('--base-ref requires --reconcile --dry-run and a full commit SHA')
    if args.complete_run:
        complete(args.complete_run, args.publish, args.archive)
        return
    if args.reconcile:
        reconcile(args.dry_run, args.archive, args.base_ref)
        return
    if args.release_complete:
        raise SystemExit(0 if release_complete(args.release_complete) else 1)
    if args.release_policy:
        release_policy(args.release_policy)
        return
    if args.verify_release_dir:
        if not args.tag:
            parser.error('--verify-release-dir requires --tag')
        verify_release_directory(args.verify_release_dir, args.tag)
        return
    if args.publish:
        parser.error('--publish requires --complete-run')
    metadata = discover('stable')
    files = {path: (ROOT / path).read_text() for path in PATHS}
    catalog = json.loads(files[CATALOG])
    if version(metadata['version']) <= version(catalog['default']):
        profile = catalog['profiles'][catalog['default']]
        current = catalog['archives'][catalog['components'][profile['components']['engine']]['archive']]
        if metadata['version'] == catalog['default'] and (
                metadata['archive_url'] != current['url'] or
                (current.get('md5') and metadata['archive_md5'] != current['md5'])):
            raise ValueError('a same-version official replacement requires manual compatibility review')
        updated = files
    else:
        with tempfile.TemporaryDirectory(prefix='wetypex-upstream-') as directory:
            updated = render(files, metadata, args.archive or download(metadata, directory))
    changed = [path for path in PATHS if updated[path] != files[path]]
    if args.write:
        for path in changed:
            (ROOT / path).write_text(updated[path])
    print(json.dumps(dict(version=metadata['version'], branch='upstream/runtime-' + metadata['version'],
                          changed=changed), ensure_ascii=False))
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write('changed=' + ('true' if changed else 'false') + '\nversion=' + metadata['version'] + '\n')


if __name__ == '__main__':
    main()
