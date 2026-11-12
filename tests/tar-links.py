#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""TAR replacements and symlink chains use the actual dentry parent."""
import io
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

repo = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(repo / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(repo / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(repo / 'fsck/fsck.erofs'))


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True,
                            text=True, timeout=30)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    return result.stdout


cases = [
    ([('origin/file', b'old'), ('other/alias', (tarfile.LNKTYPE, 'origin/file')),
      ('other/alias', None), ('other/alias/new', b'new')],
     {'origin/file': b'old', 'other/alias/new': b'new'}, ('/other/alias/..', '/other')),
    ([('gone/file', b'old'), ('alias', (tarfile.LNKTYPE, 'gone/file')),
      ('gone', b'replaced')], {'alias': b'old', 'gone': b'replaced'}, None),
    ([('target', None), ('sub/target', None), ('sub/original', (tarfile.SYMTYPE, 'target')),
      ('alias', (tarfile.LNKTYPE, 'sub/original')), ('first', (tarfile.SYMTYPE, 'alias')),
      ('first/new', b'new')], {'target/new': b'new'}, None),
]
with tempfile.TemporaryDirectory(prefix='erofs-tar-links-') as tmp:
    root = Path(tmp)
    for index, (entries, expected, parents) in enumerate(cases):
        archive, image = root / f'{index}.tar', root / f'{index}.erofs'
        with tarfile.open(archive, 'w') as stream:
            for name, value in entries:
                entry = tarfile.TarInfo(name)
                entry.mode = 0o755 if value is None else 0o644
                if value is None:
                    entry.type = tarfile.DIRTYPE
                elif isinstance(value, tuple):
                    entry.type, entry.linkname = value
                else:
                    entry.size = len(value)
                stream.addfile(entry, io.BytesIO(value) if isinstance(value, bytes) else None)
        run(mkfs, '-T0', '--tar=f', image, archive)
        out = root / f'out-{index}'
        run(fsck, '--extract=' + str(out), image)
        for name, data in expected.items():
            assert (out / name).read_bytes() == data, (index, name)
        if parents:
            nids = [int(re.search(r'NID:\s*(\d+)', run(dump, '--path=' + name, image))[1])
                    for name in parents]
            assert nids[0] == nids[1], nids
    print('PASS TAR hardlink replacement parents and symlink chains')
