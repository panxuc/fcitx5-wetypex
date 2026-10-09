#!/usr/bin/env python3
"""Create a source-only release and a pinned Arch build recipe."""
import gzip,hashlib,pathlib,tarfile
root=pathlib.Path(__file__).resolve().parent.parent
output=root/'dist/arch';output.mkdir(parents=True,exist_ok=True)
version='2.2.3.657';archive=output/f'fcitx5-wetypex-{version}.tar.gz'
files=[]
for name in ['CMakeLists.txt','README.md','LICENSE','NOTICE','CONTRIBUTING.md','SECURITY.md','.dockerignore','.editorconfig','.gitattributes','.gitignore','cmake','src','data','scripts','tools','packaging','docs']:
    p=root/name
    files.extend([p] if p.is_file() else (x for x in p.rglob('*') if x.is_file() and '__pycache__' not in x.parts))
with archive.open('wb') as raw,gzip.GzipFile(fileobj=raw,mode='wb',mtime=0,filename='') as compressed,tarfile.open(fileobj=compressed,mode='w') as tar:
    for path in sorted(files):
        if path.suffix in ['.apk','.exe','.dll','.dylib','.deb','.zip','.pyc','.so']:raise ValueError('Unexpected binary in source release: '+str(path))
        info=tar.gettarinfo(str(path),f'fcitx5-wetypex-{version}/'+str(path.relative_to(root)));info.uid=info.gid=0;info.uname=info.gname='';info.mtime=0
        with path.open('rb') as source:tar.addfile(info,source)
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
(output/'PKGBUILD').write_text((root/'packaging/PKGBUILD.in').read_text().replace('@SOURCE_SHA256@',digest))
(output/'fcitx5-wetypex.install').write_text((root/'packaging/aur/fcitx5-wetypex.install').read_text())
print(archive);print('SHA256 '+digest)
