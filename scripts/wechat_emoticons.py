#!/usr/bin/env python3
"""Build a local candidate-only colour font from verified original artwork."""
import argparse
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

vendor = pathlib.Path(__file__).resolve().parent / 'vendor'
if vendor.is_dir():
    sys.path.insert(0, str(vendor))

FAMILY = 'WeTypeX WeChat Emoticons v1'
ARCHIVE_PREFIX = ('WeType.app/Contents/MacOS/WeTypeSettings.app/Contents/'
                  'Frameworks/App.framework/Versions/A/Resources/flutter_assets/'
                  'assets/modules/candidate/icons/')


def metadata_path():
    here = pathlib.Path(__file__).resolve().parent
    installed = here / 'wechat-emoticons.json'
    return installed if installed.is_file() else here.parent / 'data/wechat-emoticons.json'


def install_font(archive, data, metadata=None):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables.sbixGlyph import Glyph
    from fontTools.ttLib.tables.sbixStrike import Strike
    from PIL import Image

    mapping = json.loads(pathlib.Path(metadata or metadata_path()).read_text())
    if mapping['version'] != 1 or mapping['font_family'] != FAMILY:
        raise ValueError('Unsupported WeChat emoticon metadata')
    entries = mapping['entries']
    if not 1 <= len(entries) <= 256:
        raise ValueError('Invalid WeChat emoticon count')
    glyphs = ['.notdef'] + [f'emoticon{i}' for i in range(len(entries))]
    builder = FontBuilder(1024, isTTF=True)
    builder.setupGlyphOrder(glyphs)
    builder.setupCharacterMap({row['codepoint']: glyphs[i + 1]
                               for i, row in enumerate(entries)})
    builder.setupGlyf({name: TTGlyphPen(None).glyph() for name in glyphs})
    builder.setupHorizontalMetrics({name: (1024, 0) for name in glyphs})
    builder.setupHorizontalHeader(ascent=896, descent=-128)
    builder.setupNameTable({'familyName': FAMILY, 'styleName': 'Regular',
                           'uniqueFontIdentifier': FAMILY,
                           'fullName': FAMILY, 'psName': 'WeTypeXWeChatEmoticons-v1',
                           'version': 'Version 1.0',
                           'copyright': 'Original artwork remains the property of its owner.'})
    builder.setupOS2(sTypoAscender=896, sTypoDescender=-128,
                     usWinAscent=896, usWinDescent=128, fsType=2)
    builder.setupPost()
    sbix = newTable('sbix')
    sbix.version, sbix.flags, sbix.strikes = 1, 1, {}
    with zipfile.ZipFile(archive) as source:
        pictures = []
        for row in entries:
            name = row['asset']
            if pathlib.PurePosixPath(name).name != name or not name.endswith('.png'):
                raise ValueError('Invalid emoticon resource name')
            member = source.getinfo(ARCHIVE_PREFIX + name)
            if member.file_size > 1048576:
                raise ValueError('Oversized emoticon resource')
            picture = Image.open(io.BytesIO(source.read(member)))
            if not (1 <= picture.width <= 1024 and 1 <= picture.height <= 1024):
                raise ValueError('Invalid emoticon dimensions')
            pictures.append(picture.convert('RGBA'))
        resample = getattr(Image, 'Resampling', Image).LANCZOS
        for size in (16, 20, 24, 32, 48, 64, 128):
            strike = Strike(ppem=size, resolution=72)
            for i, picture in enumerate(pictures):
                output = io.BytesIO()
                picture.resize((size, size), resample).save(output, format='PNG')
                strike.glyphs[glyphs[i + 1]] = Glyph(
                    glyphName=glyphs[i + 1], originOffsetY=-size // 8,
                    graphicType='png ', imageData=output.getvalue())
            sbix.strikes[size] = strike
    builder.font['sbix'] = sbix
    directory = pathlib.Path(data) / 'fonts'
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / 'fcitx5-wetypex-wechat.ttf'
    fd, temporary = tempfile.mkstemp(prefix='.wetypex-wechat-', dir=directory)
    os.close(fd)
    try:
        builder.save(temporary)
        os.chmod(temporary, 0o644)
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    subprocess.run(['fc-cache', '-f', str(directory)], check=True)
    return target


def main():
    from setup_runtime import ARCHIVE_SHA, digest
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=pathlib.Path, required=True)
    parser.add_argument('--data-dir', type=pathlib.Path,
                        default=pathlib.Path(os.environ.get('XDG_DATA_HOME',
                                             pathlib.Path.home() / '.local/share')))
    parser.add_argument('--metadata', type=pathlib.Path)
    args = parser.parse_args()
    if digest(args.archive) != ARCHIVE_SHA:
        raise ValueError('Archive SHA-256 mismatch')
    print(install_font(args.archive, args.data_dir, args.metadata))


if __name__ == '__main__':
    main()
