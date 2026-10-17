#!/usr/bin/env python3
"""Check byte-range ordering and content after moving shared data blocks."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile

root = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(root / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(root / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(root / 'fsck/fsck.erofs'))


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=30).stdout


with tempfile.TemporaryDirectory(prefix='erofs-ranges-') as tmp:
    base = Path(tmp)
    source = base / 'source'
    source.mkdir()
    # Distinct, compressible blocks, with a cold file sharing the same data.
    data = b''.join(hashlib.shake_128(bytes([i])).digest(1024) * 4 for i in range(32))
    for name in ['a-cold', 'z-hot']:
        (source / name).write_bytes(data)
    os.link(source / 'z-hot', source / 'alias')
    (source / 'symlink').symlink_to('/alias')
    hotlist = base / 'hotlist'
    hotlist.write_text('/symlink\t98304\t4096\n')
    for mode in ['--chunksize=4096', '-zlz4']:
        for workers in ([1, 2] if '--workers=' in run(mkfs, '--help') else [None]):
            image = base / 'image'
            run(mkfs, mode, *([f'--workers={workers}'] if workers else []), '--hot-file-list=' + str(hotlist),
                str(image), str(source))
            text = run(dump, '-e', '--path=/z-hot', str(image))
            extents = [tuple(map(int, m)) for m in re.findall(
                r'\d+:\s*(\d+)\.\.\s*(\d+)\s*\|\s*\d+\s*:\s*(\d+)\.\.', text)]
            assert extents, text
            hot = [p for start, end, p in extents if start <= 98304 < end]
            assert hot and hot[0] == min(p for _, _, p in extents), text
            out = base / ('out-' + mode.lstrip('-').replace('=', '-') + str(workers))
            run(fsck, '--xattrs', '--extract=' + str(out), str(image))
            extent_ends = [tuple(map(int, m)) for m in re.findall(
                r'\d+:\s*(\d+)\.\.\s*(\d+)\s*\|\s*\d+\s*:\s*\d+\.\.\s*(\d+)', text)]
            expected_end = max(p for start, end, p in extent_ends if start < 102400 and end > 98304)
            assert struct.unpack('<Q', os.getxattr(out, 'user.erofs.hot_end')) == (expected_end,), (mode, os.getxattr(out, 'user.erofs.hot_end').hex(), expected_end, text)
            for name in ['a-cold', 'z-hot', 'alias']:
                assert (out / name).read_bytes() == data, (mode, workers, name)
            assert os.stat(out / 'z-hot').st_ino == os.stat(out / 'alias').st_ino
            print('PASS range ordering/content', mode, workers)
    for entry in ['/z-hot\t\t4096\n', '/z-hot\t0\t-1\n',
                  '/z-hot\t18446744073709551615\t2\n']:
        hotlist.write_text(entry)
        result = subprocess.run([mkfs, '--chunksize=4096', '--hot-file-list=' + str(hotlist),
                                 str(base / 'invalid'), str(source)],
                                capture_output=True, timeout=30)
        assert result.returncode != 0, repr(entry)
    print('PASS invalid ranges rejected')
