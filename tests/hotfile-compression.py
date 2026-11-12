#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Hot files retain their order with compression, fragments and dedupe."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(repo / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(repo / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(repo / 'fsck/fsck.erofs'))


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True,
                            text=True, timeout=60)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    return result.stdout


def extents(image, name):
    mapped = [(int(start), int(end)) for start, end in re.findall(
        r'^\s*\d+:.*?\|\s*\d+\s*:\s*(\d+)\.\.\s*(\d+)\s*\|',
        run(dump, '-e', '--path=/' + name, image), re.M) if int(end) > int(start)]
    if mapped:
        return mapped
    # Fragment-only files are stored in the packed inode, not at offset zero.
    packed = re.search(r'Filesystem packed nid:\s*(\d+)', run(dump, '-s', image))
    assert packed is not None, (image, name)
    return [(int(start), int(end)) for start, end in re.findall(
        r'^\s*\d+:.*?\|\s*\d+\s*:\s*(\d+)\.\.\s*(\d+)\s*\|',
        run(dump, '-e', '--nid=' + packed[1], image), re.M)]


with tempfile.TemporaryDirectory(prefix='erofs-hot-compression-') as tmp:
    work = Path(tmp)
    source = work / 'source'
    source.mkdir()
    # Identical cold data exercises sharing while the hot copy stays separate.
    compressed = hashlib.shake_128(b'compressible').digest(1024) * 128 + b'tail' * 31
    raw = hashlib.shake_128(b'raw').digest(8193)
    expected = {'a-cold': compressed, 'b-cold': compressed,
                'y-hot': raw, 'z-hot': compressed}
    hotlist = work / 'hotlist'
    hotlist.write_text('/z-hot\n/y-hot\n')
    workers = [1, 2] if '--workers=' in run(mkfs, '--help') else [None]
    options = [[], ['-Efragments'], ['-Ededupe'], ['-Eall-fragments'],
               ['-Eztailpacking'], ['--zD']]
    for index, extra in enumerate(options):
        contents = dict(expected)
        if extra == ['-Eztailpacking']:
            # Cold inline tails may occupy earlier metadata; use plain cold
            # controls to isolate the ordering of allocated hot-file data.
            contents['a-cold'] = contents['b-cold'] = raw[:8192]
        for name, data in contents.items():
            (source / name).write_bytes(data)
        for count in workers:
            image = work / f'{index}-{count}.erofs'
            flags = [] if count is None else [f'--workers={count}']
            common = [mkfs, '-T0', '-Uclear', '-zlz4', *extra, *flags]
            run(*common, '--hot-file-list=' + str(hotlist), image, source)
            out = work / f'out-{index}-{count}'
            run(fsck, '--extract=' + str(out), image)
            for name, data in contents.items():
                assert (out / name).read_bytes() == data, (extra, count, name)
            positions = {name: extents(image, name) for name in expected}
            assert all(positions.values()), (extra, count, positions)
            assert min(start for start, _ in positions['z-hot']) < min(
                start for start, _ in positions['y-hot']), (extra, count, positions)
            assert min(start for start, _ in positions['y-hot']) < min(
                start for start, _ in positions['a-cold']), (extra, count, positions)
            assert not set(positions['z-hot']) & set(positions['a-cold'])
            baseline = work / f'baseline-{index}-{count}.erofs'
            empty = work / 'empty'
            empty.write_text('')
            run(*common, baseline, source)
            run(*common, '--hot-file-list=' + str(empty), image, source)
            assert image.read_bytes() == baseline.read_bytes(), (extra, count)
    print('PASS compressed hot-file order, fallback, sharing and empty lists')

    source = work / 'inline-order'
    source.mkdir()
    (source / 'raw').write_bytes(hashlib.shake_128(b'raw-first').digest(8193))
    (source / 'compressed').write_bytes(b'compressible' * 100)
    hotlist.write_text('raw\ncompressed\n')
    image = work / 'inline-order.erofs'
    run(mkfs, '-zlz4', '-Eztailpacking', '--hot-file-list=' + str(hotlist), image, source)
    assert min(start for start, _ in extents(image, 'raw')) < min(
        start for start, _ in extents(image, 'compressed'))
    print('PASS hot compressed tails do not backfill earlier metadata blocks')
