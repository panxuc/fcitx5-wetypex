"""Discover official Mac runtime channels without executing website JavaScript."""
import http.client
import json
import re
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

OFFICIAL_SITE = 'https://z.weixin.qq.com/'
DOWNLOAD_HOSTS = frozenset(('download.weread.qq.com', 'download.z.weixin.qq.com'))
CHANNEL_KEYS = {'stable': 'InstallInfo', 'beta': 'InstallInfoTest'}


def validate_url(url, hosts, allow_cdn=False):
    if not isinstance(url, str):
        raise ValueError('upstream URL is not a string')
    parsed = urllib.parse.urlsplit(url)
    cdn = (allow_cdn and (parsed.hostname or '').endswith('.cdntips.net') and
           any(parsed.path.startswith('/' + host + '/') for host in hosts))
    if (parsed.scheme != 'https' or (parsed.hostname not in hosts and not cdn) or
            parsed.port not in (None, 443) or parsed.username or parsed.password or
            parsed.fragment):
        raise ValueError('upstream URL is outside the official HTTPS sources')
    return url


def fetch(url, hosts, limit):
    validate_url(url, hosts)
    class OfficialRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, fp, code, message, headers, newurl):
            validate_url(newurl, hosts, allow_cdn=True)
            return super().redirect_request(request, fp, code, message, headers, newurl)
    request = urllib.request.Request(url, headers={'User-Agent': 'WeTypeX runtime discovery'})
    with urllib.request.build_opener(OfficialRedirect()).open(request, timeout=30) as response:
        validate_url(response.geturl(), hosts, allow_cdn=True)
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError('upstream metadata exceeds its size limit')
    return data.decode('utf-8')


def discover(channel='stable'):
    if channel not in CHANNEL_KEYS:
        raise ValueError('unknown upstream release channel')
    page = fetch(OFFICIAL_SITE, {'z.weixin.qq.com'}, 4 * 1024 * 1024)
    marker = re.search(r'\bwindow\.injectData\s*=\s*', page)
    if marker is None:
        raise ValueError('official website does not expose installation metadata')
    data, _ = json.JSONDecoder().raw_decode(page[marker.end():])
    try:
        metadata_url = data['appInfo']['mac'][CHANNEL_KEYS[channel]]
    except (KeyError, TypeError) as error:
        raise ValueError('official website has no requested Mac release channel') from error
    validate_url(metadata_url, DOWNLOAD_HOSTS)
    metadata = json.loads(fetch(metadata_url, DOWNLOAD_HOSTS, 1024 * 1024))
    if not isinstance(metadata, dict):
        raise ValueError('official installation metadata is not an object')
    version = metadata.get('zip_version')
    md5 = metadata.get('zip_download_md5')
    if not isinstance(version, str) or not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', version):
        raise ValueError('invalid original package version')
    if not isinstance(md5, str) or not re.fullmatch(r'[a-fA-F0-9]{32}', md5):
        raise ValueError('invalid original package digest')
    archive_url = validate_url(metadata.get('zip_download_url'), DOWNLOAD_HOSTS)
    if not urllib.parse.urlsplit(archive_url).path.endswith('.zip'):
        raise ValueError('original package is not a ZIP archive')
    return dict(channel=channel, version=version, metadata_url=metadata_url,
                archive_url=archive_url, archive_md5=md5.lower(),
                checksum_source='official HTTPS metadata',
                compatibility_validated=False)


def download_archive(opener, url, output):
    """Resume interrupted official downloads; the caller verifies the full SHA."""
    limit = 768 * 1024 * 1024
    for attempt in range(3):
        offset = output.tell()
        request = urllib.request.Request(url, headers={'User-Agent': 'WeTypeX runtime downloader'})
        if offset:
            request.add_header('Range', 'bytes=' + str(offset) + '-')
        try:
            with opener.open(request, timeout=45) as response:
                validate_url(response.geturl(), DOWNLOAD_HOSTS, allow_cdn=True)
                if response.status == 206:
                    interval = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',
                                            response.headers.get('Content-Range', ''))
                    if not interval or int(interval[1]) != offset or not (
                            offset <= int(interval[2]) < int(interval[3]) <= limit):
                        raise ValueError('invalid original archive range response')
                    expected = int(interval[2]) + 1
                    full_size = int(interval[3])
                elif response.status == 200:
                    output.seek(0)
                    output.truncate()
                    offset = 0
                    length = response.headers.get('Content-Length')
                    expected = int(length) if length else None
                    full_size = expected
                else:
                    raise ValueError('unexpected original archive response')
                if expected is not None and expected > limit:
                    raise ValueError('original archive exceeds its size limit')
                total = offset
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    total += len(block)
                    if total > limit or (expected is not None and total > expected):
                        raise ValueError('original archive exceeds its declared size')
                    output.write(block)
                if expected is not None and total < expected:
                    raise http.client.IncompleteRead(b'', expected - total)
                if full_size is not None and total < full_size:
                    raise http.client.IncompleteRead(b'', full_size - total)
                return
        except (urllib.error.URLError, http.client.HTTPException,
                ConnectionError, TimeoutError, ssl.SSLError) as error:
            if isinstance(error, ssl.SSLCertVerificationError):
                raise
            if isinstance(error, urllib.error.HTTPError):
                if error.code == 416 and offset:
                    output.seek(0)
                    output.truncate()
                elif error.code not in (408, 429, 500, 502, 503, 504):
                    raise
            if attempt == 2:
                raise OSError('original archive download failed after three attempts: ' + str(error)) from error
            print('Original archive download interrupted; retrying (%d/3)' % (attempt + 2),
                  file=sys.stderr)
            time.sleep(attempt + 1)
