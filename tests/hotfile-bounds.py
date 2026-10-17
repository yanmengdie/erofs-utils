#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
"""The stored hot prefix covers physical extents, including aliased last paths."""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(root / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(root / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(root / 'fsck/fsck.erofs'))
key = 'user.erofs.hot_end'


def getxattr(path, name):
    """os.getxattr on Linux, xattr(1) on macOS."""
    if hasattr(os, 'getxattr'):
        return os.getxattr(path, name)
    out = subprocess.run(['xattr', '-px', name, str(path)], check=True,
                         capture_output=True, text=True).stdout
    return bytes.fromhex(''.join(out.split()))


def setxattr(path, name, value):
    """os.setxattr on Linux, xattr(1) on macOS."""
    if hasattr(os, 'setxattr'):
        return os.setxattr(path, name, value)
    subprocess.run(['xattr', '-w', name, value.decode(), str(path)], check=True)


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=30)
    assert p.returncode == 0, (args, p.stderr.decode())
    return p.stdout.decode()


with tempfile.TemporaryDirectory(prefix='erofs-bounds-') as tmp:
    base = Path(tmp)
    source = base / 'source'
    source.mkdir()
    (source / 'a').write_bytes(hashlib.shake_128(b'compressed').digest(1024) * 20480)
    (source / 'b').write_bytes(hashlib.shake_128(b'raw').digest(8192))
    os.link(source / 'a', source / 'c')
    (source / 'cold').write_bytes(b'cold' * 32768)
    hot = base / 'hotlist'
    hot.write_text('/b\n/a\n/c\n')
    images, blobs, ends = [], [], []
    for n, opts in enumerate([[], ['-zlz4'], ['--chunksize=4096']]):
        image, blob = base / f'{n}.erofs', base / f'{n}.blob'
        run(mkfs, '-b4096', *opts, '-Enoinline_data', '--blobdev=' + str(blob),
            '--hot-file-list=' + str(hot), image, source)
        out = base / f'out{n}'
        run(fsck, '--xattrs', '--extract=' + str(out), '--device=' + str(blob), image)
        bounds = struct.unpack('<QQ', getxattr(out, key))
        physical_ends = {
            name: [int(end) for end in re.findall(
                r'\.\.\s*(\d+)\s*\|\s*\d+\s*# device 1',
                run(dump, '-e', '--path=/' + name, image))]
            for name in ('a', 'b', 'c')}
        expected = max(end for values in physical_ends.values() for end in values)
        if opts == ['-zlz4']:
            assert len(physical_ends['a']) > 1
            assert expected == max(physical_ends['a'])
        images.append(image)
        blobs.append(blob)
        ends.append(expected)
        assert bounds == (0, expected), (opts, bounds, expected)
    print('PASS stored physical prefix for compression, raw fallback, chunks, hardlinks')

    # A large inode xattr forces an otherwise inline compressed tail out.
    tail_source = base / 'tail-source'
    tail_source.mkdir()
    payload = hashlib.shake_128(b'tail').digest(2000) * 30
    (tail_source / 'hot').write_bytes(payload)
    setxattr(tail_source / 'hot', 'user.padding', b'x' * 3000)
    hot.write_text('/hot\n')
    for external in (False, True):
        image = base / f'tail-{external}.erofs'
        blob = base / 'tail.blob'
        run(mkfs, '-b4096', '-zlz4', '-Eztailpacking', '--hot-file-list=' + str(hot),
            *(['--blobdev=' + str(blob)] if external else []), image, tail_source)
        extents = re.findall(r':\s*\d+\.\.\s*(\d+)\s*\|\s*4096(?:\s*# device (\d+))?',
                             run(dump, '-e', '--path=/hot', image))
        assert len(extents) == 1, extents
        end, device = extents[0]
        expected = [0] * (2 if external else 1)
        expected[int(device or 0)] = int(end)
        out = base / f'tail-out-{external}'
        run(fsck, '--xattrs', '--extract=' + str(out),
            *(['--device=' + str(blob)] if external else []), image)
        assert (out / 'hot').read_bytes() == payload
        assert struct.unpack('<' + 'Q' * len(expected), getxattr(out, key)) == tuple(expected)
    print('PASS hot bounds include compressed tails displaced by inode xattrs')
