#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""TAR overlay origin attributes can be written with no value bytes."""
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


with tempfile.TemporaryDirectory(prefix='erofs-empty-xattr-') as tmp:
    root = Path(tmp)
    archive, image = root / 'source.tar', root / 'image'
    with tarfile.open(archive, 'w') as stream:
        entry = tarfile.TarInfo('.wh.missing')
        stream.addfile(entry)
    run(mkfs, '-T0', '--tar=f', '--aufs', image, archive)
    run(fsck, image)
    info = run(dump, '--path=/', image)
    assert int(re.search(r'Xattr size:\s*(\d+)', info)[1]) > 0, info
    print('PASS empty overlay origin xattr')
