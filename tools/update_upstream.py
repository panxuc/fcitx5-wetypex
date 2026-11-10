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
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))
from runtime_catalog import discover, fetch, validate_url, DOWNLOAD_HOSTS, download_archive
from prepare_macos_runtime import select_slice

CATALOG = 'data/runtime-profiles.json'
PATHS = (CATALOG, 'CMakeLists.txt', 'packaging/PKGBUILD.in',
         'packaging/aur/PKGBUILD', 'README.md')
VERIFY_WORKFLOW = 'upstream-verify.yml'


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
            if Path(name).name != name or name in ('', '.', '..') or info.file_size > 512 * 1024 * 1024:
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
    project = re.search(r'project\(fcitx5-wetypex VERSION ([0-9.]+)', files['CMakeLists.txt'])
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
    request = urllib.request.Request('https://api.github.com/repos/' + repository + path,
        method=method, data=None if data is None else json.dumps(data).encode(), headers={
            'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
            'Accept': 'application/vnd.github+json', 'User-Agent': 'WeTypeX upstream updater',
            'X-GitHub-Api-Version': '2022-11-28', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        content = response.read(4 * 1024 * 1024)
    return json.loads(content) if content else {}


def remote_files(ref):
    result = {}
    for path in PATHS:
        item = api('/contents/' + path + '?ref=' + ref)
        if item.get('encoding') != 'base64' or item.get('size', 0) > 1024 * 1024:
            raise ValueError('unexpected candidate file encoding or size')
        result[path] = base64.b64decode(item['content'], validate=False).decode()
    return result


def complete(run_id, publish):
    """Use trusted main code and API data only; never checkout a candidate."""
    run = api('/actions/runs/' + str(int(run_id)))
    repository = os.environ['GITHUB_REPOSITORY']
    if (run['event'] != 'workflow_dispatch' or run['conclusion'] != 'success' or
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
    already_merged = bool(pr.get('merged'))
    if [parent['sha'] for parent in api('/git/commits/' + head)['parents']] != [base]:
        raise ValueError('the candidate must be one generated commit on the verified base')
    if (pr['user']['login'] != 'github-actions[bot]' or pr['head']['repo']['full_name'] != repository or
            pr['draft'] or head != run['head_sha']):
        raise ValueError('the bot candidate changed or its base is no longer current')
    if not already_merged and (pr['state'] != 'open' or
            api('/git/ref/heads/main')['object']['sha'] != base):
        raise ValueError('the verified base is stale; rebase and rerun the complete matrix')
    changed = api('/pulls/' + str(pr['number']) + '/files?per_page=100')
    if len(changed) != pr['changed_files'] or any(
            item['filename'] not in PATHS or item['status'] != 'modified' for item in changed):
        raise ValueError('automatic updates may only contain the generated version/profile files')
    before, after = remote_files(base), remote_files(head)
    metadata = discover('stable')
    if already_merged:
        # Resume a tag/dispatch interrupted after the squash, even if the
        # website has moved on to another release. Its immutable official
        # installation JSON remains the source of truth.
        catalog = json.loads(after[CATALOG])
        profile = catalog['profiles'][catalog['default']]
        package = catalog['archives'][catalog['components'][profile['components']['engine']]['archive']]
        data = json.loads(fetch(package['metadata_url'], DOWNLOAD_HOSTS, 1024 * 1024))
        metadata = dict(version=data['zip_version'], archive_url=data['zip_download_url'],
                        archive_md5=data['zip_download_md5'], metadata_url=package['metadata_url'])
    with tempfile.TemporaryDirectory(prefix='wetypex-upstream-review-') as directory:
        expected = render(before, metadata, download(metadata, directory))
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
    if len(jobs) < 8 or any(job['conclusion'] != 'success' for job in jobs):
        raise ValueError('the complete verification/package matrix must succeed')
    release = metadata['version'] + '-1'
    print(json.dumps(dict(pr=pr['number'], head=head, release=release, publish=publish)))
    if not publish:
        return
    # The merge API rejects a racing head; recheck main immediately before it.
    if not already_merged and api('/git/ref/heads/main')['object']['sha'] != base:
        raise ValueError('main advanced; dispatch verification against the new base')
    merged = dict(merged=True, sha=pr['merge_commit_sha']) if already_merged else api('/pulls/' + str(pr['number']) + '/merge', 'PUT',
                 dict(sha=head, merge_method='squash', commit_title='Update original core to ' + metadata['version']))
    if not merged.get('merged'):
        raise ValueError('GitHub did not merge the validated candidate')
    commit = api('/git/commits/' + merged['sha'])
    if ([parent['sha'] for parent in commit['parents']] != [base] or
            commit['tree']['sha'] != api('/git/commits/' + head)['tree']['sha']):
        raise ValueError('the merge base changed during merge; rerun verification before publication')
    tag = 'v' + release
    try:
        existing = api('/git/ref/tags/' + tag)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        existing = None
    if existing:
        if existing['object']['sha'] != merged['sha']:
            raise ValueError('the release tag already identifies another commit')
    else:
        api('/git/refs', 'POST', dict(ref='refs/tags/' + tag, sha=merged['sha']))
    runs = api('/actions/workflows/release.yml/runs?event=workflow_dispatch&per_page=100')['workflow_runs']
    if any(item['display_title'] == 'WeTypeX release ' + tag and
           item.get('conclusion') in (None, 'success') for item in runs):
        return
    api('/actions/workflows/release.yml/dispatches', 'POST',
        dict(ref='main', inputs=dict(tag=tag, only_aur=False)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Reuse a downloaded official ZIP')
    parser.add_argument('--write', action='store_true', help='Write only the generated local files')
    parser.add_argument('--complete-run', type=int)
    parser.add_argument('--publish', action='store_true', help='Merge/tag only after the complete gate succeeds')
    args = parser.parse_args()
    if args.complete_run:
        complete(args.complete_run, args.publish)
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
