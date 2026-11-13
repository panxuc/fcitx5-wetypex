"""Discover official Mac runtime channels without executing website JavaScript."""
import http.client
import json
import re
import socket
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


def _archive_connection(address, timeout=None, source_address=None,
                        family=socket.AF_UNSPEC, **_options):
    # The standard helper gives *each* address a fresh timeout. Keep one
    # connection budget even when a CDN resolves to many unreachable hosts.
    deadline = time.monotonic() + (timeout if isinstance(timeout, (int, float)) else 45)
    last_error = None
    for domain, kind, protocol, _canonical, target in socket.getaddrinfo(
            address[0], address[1], family, socket.SOCK_STREAM):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout('original archive connection timed out')
        connection = socket.socket(domain, kind, protocol)
        try:
            connection.settimeout(min(10, remaining))
            if source_address:
                connection.bind(source_address)
            connection.connect(target)
            connection.settimeout(max(0.001, deadline - time.monotonic()))
            return connection
        except OSError as error:
            last_error = error
            connection.close()
    raise last_error or OSError('no address for the original archive server')


def _ipv4_connection(address, timeout=None, source_address=None, **options):
    return _archive_connection(address, timeout, source_address,
                               family=socket.AF_INET, **options)


class _ArchiveHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._create_connection = _archive_connection


class _IPv4HTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._create_connection = _ipv4_connection


class _ArchiveHTTPSHandler(urllib.request.HTTPSHandler):
    handler_order = 499
    ipv4 = False

    def do_open(self, http_class, request, **kwargs):
        if http_class is http.client.HTTPSConnection:
            http_class = _IPv4HTTPSConnection if self.ipv4 else _ArchiveHTTPSConnection
        return super().do_open(http_class, request, **kwargs)


def download_archive(opener, url, output):
    """Download bounded ranges; the caller verifies the complete archive SHA."""
    limit = 768 * 1024 * 1024
    segment = 16 * 1024 * 1024
    failures = 0
    full_size = None
    validator = None
    logged = output.tell()
    transport = _ArchiveHTTPSHandler()
    opener.add_handler(transport)
    for _request in range(1024):
        offset = output.tell()
        end = min(offset + segment, limit) - 1
        if offset < 0 or offset >= limit:
            raise ValueError('invalid original archive offset')
        request = urllib.request.Request(url, headers={
            'User-Agent': 'WeTypeX runtime download',
            'Accept-Encoding': 'identity',
            'Range': 'bytes=%d-%d' % (offset, end),
        })
        if offset and validator:
            request.add_header('If-Range', validator)
        opener.archive_last_host = urllib.parse.urlsplit(url).hostname
        try:
            with opener.open(request, timeout=45) as response:
                validate_url(response.geturl(), DOWNLOAD_HOSTS, allow_cdn=True)
                if response.headers.get('Content-Encoding', 'identity').lower() != 'identity':
                    raise ValueError('unexpected original archive content encoding')
                if response.status == 206:
                    interval = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',
                                            response.headers.get('Content-Range', ''))
                    if not interval or int(interval[1]) != offset or not (
                            offset <= int(interval[2]) <= end and
                            int(interval[2]) < int(interval[3]) <= limit):
                        raise ValueError('invalid original archive range response')
                    expected = int(interval[2]) + 1
                    size = int(interval[3])
                    length = response.headers.get('Content-Length')
                    if length is not None and int(length) != expected - offset:
                        raise ValueError('inconsistent original archive range length')
                    if full_size is not None and full_size != size:
                        raise ValueError('original archive changed during download')
                    full_size = size
                elif response.status == 200:
                    # Range is optional. A full response replaces the prefix;
                    # it must never be appended to previously downloaded data.
                    output.seek(0)
                    output.truncate()
                    offset = 0
                    validator = None
                    length = response.headers.get('Content-Length')
                    expected = int(length) if length is not None else None
                    full_size = expected
                else:
                    raise ValueError('unexpected original archive response')
                if expected is not None and not (0 < expected <= limit):
                    raise ValueError('original archive exceeds its size limit')
                tag = response.headers.get('ETag', '')
                if re.fullmatch(r'"[\x21\x23-\x7e]*"', tag):
                    if response.status == 206 and validator and validator != tag:
                        raise ValueError('original archive validator changed during download')
                    validator = tag
                total = offset
                progress_time, progress_bytes = time.monotonic(), total
                while True:
                    # read() can keep filling its buffer indefinitely while
                    # tiny packets arrive. read1() returns after one I/O.
                    block = response.read1(256 * 1024)
                    if not block:
                        break
                    total += len(block)
                    if total > limit or (expected is not None and total > expected):
                        raise ValueError('original archive exceeds its declared size')
                    output.write(block)
                    now = time.monotonic()
                    if now - progress_time >= 60:
                        if total - progress_bytes < 64 * 1024:
                            raise socket.timeout('original archive download made no useful progress')
                        progress_time, progress_bytes = now, total
                    if total - logged >= 32 * 1024 * 1024:
                        print('Original archive downloaded: %d MiB' % (total // (1024 * 1024)),
                              file=sys.stderr)
                        logged = total
                if expected is not None and total < expected:
                    raise http.client.IncompleteRead(b'', expected - total)
                if full_size is None or total == full_size:
                    return
                # A complete 206 segment is success, not an interrupted
                # download. Continue from its end without spending a retry.
        # Python 3.8's socket.timeout is distinct from TimeoutError.
        except (urllib.error.URLError, http.client.HTTPException,
                ConnectionError, TimeoutError, socket.timeout, ssl.SSLError) as error:
            if isinstance(error, ssl.SSLCertVerificationError) or (
                    isinstance(error, urllib.error.URLError) and
                    isinstance(error.reason, ssl.SSLCertVerificationError)):
                raise
            if isinstance(error, urllib.error.HTTPError):
                if error.code == 416 and offset:
                    output.seek(0)
                    output.truncate()
                    full_size, validator = None, None
                elif error.code not in (408, 429, 500, 502, 503, 504):
                    raise
            failures += 1
            if failures >= 6:
                raise OSError('original archive download failed after six interruptions: ' + str(error)) from error
            if not isinstance(error, urllib.error.HTTPError):
                transport.ipv4 = True
                if failures >= 2:
                    # Keep IPv4 and certificate/hostname verification while
                    # avoiding oversized hybrid key shares on old gateways.
                    context = ssl.create_default_context()
                    context.set_ecdh_curve('prime256v1')
                    if ssl.HAS_ALPN:
                        context.set_alpn_protocols(['http/1.1'])
                    transport._context = context
            print('Original archive download interrupted at %s (%s); resuming at %d MiB (%d/6)' %
                  (opener.archive_last_host, type(error).__name__,
                   output.tell() // (1024 * 1024), failures), file=sys.stderr)
            time.sleep(min(failures, 3))
    raise OSError('original archive server returned too many partial responses')
