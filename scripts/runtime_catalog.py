"""Discover official Mac runtime channels without executing website JavaScript."""
import json
import re
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
