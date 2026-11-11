#!/usr/bin/env python3
"""Publish verified packages, or recover publication without rebuilding them."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from update_upstream import (api, optional_api, pages, release_files,
                             release_version, verify_release_directory)

SOURCE_ROOTS = frozenset(('CMakeLists.txt', 'README.md', 'LICENSE', 'NOTICE',
    'CONTRIBUTING.md', 'SECURITY.md', '.dockerignore', '.editorconfig',
    '.gitattributes', '.gitignore', '.markdownlint-cli2.jsonc', 'flake.nix',
    'flake.lock', 'nix', 'cmake', 'src', 'data', 'scripts', 'tools', 'packaging', 'docs'))
PROVENANCE = {'release-source-x86_64.json', 'release-source-aarch64.json'}


def retry(function):
    for attempt in range(3):
        try:
            return function()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            if isinstance(error, urllib.error.HTTPError) and error.code not in (408, 429, 500, 502, 503, 504):
                raise
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def gh(*arguments, output=None):
    return subprocess.run(['gh', *map(str, arguments)], check=True, stdout=output,
                          timeout=180)


def get(path):
    return retry(lambda: api(path))


def find_release(tag):
    # /releases/tags/{tag} returns published releases, not drafts.
    published = retry(lambda: optional_api('/releases/tags/' + tag))
    if published:
        return published
    matches = [row for row in retry(lambda: pages('/releases')) if row['tag_name'] == tag]
    if len(matches) > 1:
        raise ValueError('multiple releases identify this tag')
    return matches[0] if matches else None


def target_commit(tag):
    release_files(tag)
    target = get('/git/ref/tags/' + tag)['object']
    for _ in range(5):
        if target['type'] == 'commit':
            break
        if target['type'] != 'tag':
            raise ValueError('release tag does not identify a commit')
        target = get('/git/tags/' + target['sha'])['object']
    if target['type'] != 'commit':
        raise ValueError('release tag nesting exceeds the limit')
    sha = target['sha']
    main = get('/git/ref/heads/main')['object']['sha']
    if get('/compare/' + sha + '...' + main)['status'] not in ('identical', 'ahead'):
        raise ValueError('release tag is not in main history')
    values = {}
    for name in ('CMakeLists.txt', 'packaging/PKGBUILD.in'):
        row = get('/contents/' + name + '?ref=' + sha)
        values[name] = base64.b64decode(row['content']).decode()
    version = re.search(r'^project\(fcitx5-wetypex VERSION ([0-9.]+)', values['CMakeLists.txt'], re.M)
    revision = re.search(r'^pkgrel=(\d+)$', values['packaging/PKGBUILD.in'], re.M)
    if not version or not revision or tag != 'v' + version[1] + '-' + revision[1]:
        raise ValueError('tag does not match the source package version')
    return sha


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def download_api(path, output, limit, accept='application/octet-stream'):
    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, fp, code, message, headers, url):
            parsed = urllib.parse.urlsplit(url)
            if parsed.scheme != 'https' or parsed.username or parsed.password:
                raise ValueError('GitHub binary download redirected outside HTTPS')
            redirected = super().redirect_request(request, fp, code, message, headers, url)
            redirected.remove_header('Authorization')
            return redirected
    request = urllib.request.Request(
        'https://api.github.com/repos/' + os.environ['GITHUB_REPOSITORY'] + path,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                 'Accept': accept, 'User-Agent': 'WeTypeX release publisher',
                 'X-GitHub-Api-Version': '2022-11-28'})
    output.seek(0)
    output.truncate()
    started, total = time.monotonic(), 0
    with urllib.request.build_opener(Redirect()).open(request, timeout=30) as response:
        while block := response.read(1024 * 1024):
            total += len(block)
            if total > limit:
                raise ValueError('GitHub binary download exceeds its size limit')
            if time.monotonic() - started > 600:
                raise TimeoutError('GitHub binary download exceeded ten minutes')
            output.write(block)


def checksums(data, tag):
    if len(data) > 32768:
        raise ValueError('release checksum manifest exceeds its size limit')
    result = {}
    for line in data.decode('ascii').splitlines():
        match = re.fullmatch(r'([a-f0-9]{64})  ([^/\\\s]+)', line)
        if not match or match[2] in result:
            raise ValueError('invalid or duplicate release checksum entry')
        result[match[2]] = match[1]
    if result.keys() != release_files(tag) - {'SHA256SUMS'}:
        raise ValueError('release checksum manifest has the wrong artifact set')
    return result


def verify_source(archive, sha, tag):
    rows = get('/git/trees/' + sha + '?recursive=1')
    if rows.get('truncated'):
        raise ValueError('release source tree is truncated')
    expected = {row['path']: row['sha'] for row in rows['tree']
                if row['type'] == 'blob' and row['path'].split('/')[0] in SOURCE_ROOTS
                and '__pycache__' not in PurePosixPath(row['path']).parts}
    prefix = 'fcitx5-wetypex-' + tag[1:].rsplit('-', 1)[0] + '/'
    found = set()
    with tarfile.open(archive) as source:
        for row in source:
            if row.isdir():
                continue
            if not row.isfile() or not row.name.startswith(prefix) or row.size > 32 * 1024 * 1024:
                raise ValueError('invalid source archive member')
            name = row.name[len(prefix):]
            if name not in expected or name in found:
                raise ValueError('unexpected or duplicate source archive member: ' + name)
            blob = hashlib.sha1(b'blob ' + str(row.size).encode() + b'\0')
            with source.extractfile(row) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    blob.update(block)
            if blob.hexdigest() != expected[name]:
                raise ValueError('source archive differs from its tag: ' + name)
            found.add(name)
    if found != expected.keys():
        raise ValueError('source archive is incomplete')


def recover(run_id, tag, sha, output, cache):
    run = get('/actions/runs/' + str(run_id))
    repository = os.environ['GITHUB_REPOSITORY'].lower()
    if (run['path'] != '.github/workflows/release.yml' or
            run.get('head_repository', {}).get('full_name', '').lower() != repository or
            run['event'] not in ('push', 'workflow_dispatch') or
            run['display_title'] not in ('WeTypeX release ' + tag,
                                         'WeTypeX release ' + tag + ' (validation only)')):
        raise ValueError('artifact run is not the requested repository release')
    main = get('/git/ref/heads/main')['object']['sha']
    if get('/compare/' + run['head_sha'] + '...' + main)['status'] not in ('identical', 'ahead'):
        raise ValueError('artifact workflow source is not trusted main history')
    artifacts = retry(lambda: pages('/actions/runs/' + str(run_id) + '/artifacts', 'artifacts'))
    artifacts = [row for row in artifacts if not row['expired'] and row['size_in_bytes'] > 0]
    full_name = 'fcitx5-wetypex-' + tag
    names = ([full_name] if any(row['name'] == full_name for row in artifacts) else
             ['fcitx5-wetypex-x86_64-packages', 'fcitx5-wetypex-arm64-packages'])
    jobs = retry(lambda: pages('/actions/runs/' + str(run_id) + '/jobs?filter=all', 'jobs'))
    if names == [full_name]:
        producers = [job for job in jobs if job['name'] == 'release']
        required = {'为全部架构生成校验和', '保存构建产物'}
        if (run['head_sha'] != sha or not any(
                required <= {step['name'] for step in producer['steps']
                             if step['conclusion'] == 'success'} for producer in producers)):
            raise ValueError('legacy packages did not pass the complete release validation')
    else:
        if not {'packages', 'arm64 / packages'} <= {
                job['name'] for job in jobs if job['conclusion'] == 'success'}:
            raise ValueError('both package producer jobs must succeed')
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('artifact recovery needs an empty destination')
    found = set()
    proofs = set()
    total = 0
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    for name in names:
        matches = sorted((row for row in artifacts if row['name'] == name),
                         key=lambda row: (row['created_at'], row['id']), reverse=True)
        if not matches:
            raise ValueError('release artifact is missing: ' + name)
        artifact = matches[0]
        sealed = artifact.get('digest', '')
        if not re.fullmatch(r'sha256:[a-f0-9]{64}', sealed):
            raise ValueError('release artifact has no GitHub SHA-256 digest')
        print('Verifying Actions artifact ' + str(artifact['id']) + ': ' + name, flush=True)
        package = cache / (sealed[7:] + '.zip')
        if not package.is_file() or package.is_symlink() or digest(package) != sealed[7:]:
            if package.is_symlink():
                raise ValueError('artifact cache entry is a symbolic link')
            temporary = tempfile.NamedTemporaryFile(dir=cache, prefix='.download-', delete=False)
            temporary.close()
            pending = Path(temporary.name)
            def download():
                with pending.open('wb') as stream:
                    download_api('/actions/artifacts/' + str(artifact['id']) + '/zip',
                                 stream, 128 * 1024 * 1024, 'application/vnd.github+json')
                if digest(pending) != sealed[7:]:
                    raise ValueError('downloaded Actions artifact failed its digest check')
            try:
                retry(download)
                os.replace(pending, package)
            finally:
                pending.unlink(missing_ok=True)
        with zipfile.ZipFile(package) as source:
            for row in source.infolist():
                path = PurePosixPath(row.filename)
                mode = row.external_attr >> 16
                if path.is_absolute() or '..' in path.parts or '\\' in row.filename or stat.S_ISLNK(mode):
                    raise ValueError('unsafe release artifact path')
                if row.is_dir():
                    continue
                total += row.file_size
                if row.file_size > 128 * 1024 * 1024 or total > 512 * 1024 * 1024:
                    raise ValueError('release artifact exceeds its size limit')
                if path.name in PROVENANCE:
                    if row.file_size > 4096 or path.name in proofs:
                        raise ValueError('invalid source provenance')
                    proof = json.loads(source.read(row))
                    arch = 'aarch64' if path.name.endswith('aarch64.json') else 'x86_64'
                    if (proof.get('schema') != 1 or proof.get('tag') != tag or
                            proof.get('commit') != sha or proof.get('architecture') != arch or
                            proof.get('run_id') != run_id):
                        raise ValueError('package source provenance does not match the tag')
                    proofs.add(path.name)
                    continue
                if path.name not in release_files(tag) or path.name in found:
                    raise ValueError('unexpected or duplicate release artifact file: ' + row.filename)
                found.add(path.name)
                with source.open(row) as stream, (output / path.name).open('wb') as target:
                    while block := stream.read(1024 * 1024):
                        target.write(block)
    required = release_files(tag)
    if names != [full_name]:
        if proofs != PROVENANCE:
            raise ValueError('both architectures need matching source provenance')
        required -= {'SHA256SUMS'}
    if found != required:
        raise ValueError('release artifact set is incomplete')
    if 'SHA256SUMS' in found:
        for name, expected in checksums((output / 'SHA256SUMS').read_bytes(), tag).items():
            if digest(output / name) != expected:
                raise ValueError('package checksum mismatch: ' + name)
    print('Verifying source archive against the immutable tag', flush=True)
    verify_source(output / ('fcitx5-wetypex-' + tag[1:].rsplit('-', 1)[0] + '.tar.gz'), sha, tag)
    print('Verifying both architectures and all package formats', flush=True)
    verify_release_directory(output, tag)
    return {'run': run_id, 'source': sha, 'files': len(release_files(tag))}


def read_asset(asset):
    if asset['size'] > 128 * 1024 * 1024:
        raise ValueError('release asset exceeds its size limit')
    with tempfile.TemporaryFile() as output:
        retry(lambda: download_api('/releases/assets/' + str(asset['id']), output,
                                   min(asset['size'], 128 * 1024 * 1024)))
        output.seek(0)
        data = output.read(128 * 1024 * 1024 + 1)
    if len(data) != asset['size']:
        raise ValueError('release asset size mismatch')
    return data


def remote_manifest(release, tag):
    assets = {row['name']: row for row in release['assets']}
    if not release_files(tag) <= assets.keys():
        return None
    expected = checksums(read_asset(assets['SHA256SUMS']), tag)
    for name, value in expected.items():
        row = assets[name]
        if row['size'] <= 0:
            return None
        actual = row.get('digest')
        if actual is None:
            actual = 'sha256:' + hashlib.sha256(read_asset(row)).hexdigest()
        if actual != 'sha256:' + value:
            return None
    return expected


def publish(directory, tag, sha, dry_run):
    release = find_release(tag)
    local = checksums((directory / 'SHA256SUMS').read_bytes(), tag)
    if release and not release['draft']:
        if release['prerelease'] or remote_manifest(release, tag) is None:
            raise ValueError('an existing public release is incomplete; refusing to replace its assets')
        source_name = 'fcitx5-wetypex-' + tag[1:].rsplit('-', 1)[0] + '.tar.gz'
        source_asset = next(row for row in release['assets'] if row['name'] == source_name)
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / 'source.tar.gz'
            archive.write_bytes(read_asset(source_asset))
            verify_source(archive, sha, tag)
        print(json.dumps({'tag': tag, 'action': 'preserved_published_release', 'id': release['id']}))
        return
    if dry_run:
        try:
            staged_matches = release is not None and remote_manifest(release, tag) == local
        except ValueError:
            staged_matches = False
        print(json.dumps({'tag': tag, 'action': 'validated_draft',
                          'id': release['id'] if release else None, 'source': sha,
                          'uploaded_files_match': staged_matches,
                          'files': len(release_files(tag))}))
        return
    if not release:
        try:
            release = api('/releases', 'POST', dict(tag_name=tag, name='WeTypeX ' + tag[1:],
                                                  draft=True, generate_release_notes=True))
        except OSError:
            release = find_release(tag)
            if not release:
                raise
    release_id = release['id']
    # Upload the manifest last. A failed upload always leaves a private draft.
    for name in [*sorted(local), 'SHA256SUMS']:
        release = get('/releases/' + str(release_id))
        if not release['draft'] or release['tag_name'] != tag:
            raise ValueError('release visibility or tag changed during upload')
        existing = next((row for row in release['assets'] if row['name'] == name), None)
        expected = digest(directory / name)
        matches = existing and existing['size'] == (directory / name).stat().st_size
        if matches:
            actual = existing.get('digest') or 'sha256:' + hashlib.sha256(read_asset(existing)).hexdigest()
            matches = actual == 'sha256:' + expected
        if not matches:
            retry(lambda: gh('release', 'upload', tag, directory / name, '--clobber',
                             '--repo', os.environ['GITHUB_REPOSITORY']))
    staged = get('/releases/' + str(release_id))
    if remote_manifest(staged, tag) != local or target_commit(tag) != sha:
        raise ValueError('uploaded release differs from the verified packages or tag')
    newer = any(not row['draft'] and not row['prerelease'] and row['tag_name'] != tag and
                release_version(row['tag_name']) > release_version(tag)
                for row in retry(lambda: pages('/releases')))
    retry(lambda: api('/releases/' + str(release_id), 'PATCH',
                      dict(draft=False, prerelease=False, make_latest='false' if newer else 'legacy')))
    final = get('/releases/' + str(release_id))
    if final['draft'] or final['prerelease'] or remote_manifest(final, tag) != local:
        raise ValueError('published release failed its final verification')
    print(json.dumps({'tag': tag, 'action': 'published', 'id': release_id, 'files': len(release_files(tag))}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--run', required=True, type=int, help='Release workflow run containing validated packages')
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--artifact-cache', type=Path, help='Reuse only SHA-256 verified artifact ZIP files')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    sha = target_commit(args.tag)
    result = recover(args.run, args.tag, sha, args.directory,
                     args.artifact_cache or args.directory.parent / 'release-artifact-cache')
    print(json.dumps(result))
    publish(args.directory, args.tag, sha, args.dry_run)


if __name__ == '__main__':
    main()
