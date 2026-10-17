#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
"""A fixed input identity and build profile must ignore ambient host state."""
import hashlib
import io
import os
import subprocess
import tarfile
import tempfile
import uuid
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(repo/'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(repo/'dump/dump.erofs'))
temporary = tempfile.TemporaryDirectory(prefix='erofs-repro-')
base = Path(temporary.name)
archive = base / 'input.tar'
with tarfile.open(archive, 'w') as tf:
    for name, payload in [('hot', b'Hot data\n' * 2500000),
                          ('cold', hashlib.shake_128(b'cold').digest(131072))]:
        info = tarfile.TarInfo(name)
        info.uid, info.gid, info.mtime = 123, 456, 1234567890
        info.mode = 0o640
        info.size = len(payload)
        tf.addfile(info, io.BytesIO(payload))
hotlist = b'/hot\n'
fixed = '11111111-2222-3333-4444-555555555555'
digests = []
helptext = subprocess.run([mkfs, '--help'], capture_output=True, text=True, check=True, timeout=30).stdout
for index, workers in enumerate((1, 2)):
    work = base / ('first-host-path' if index == 0 else 'other-host-path')
    work.mkdir()
    src, hot, image = work/'source.tar', work/'hotlist', work/'image.erofs'
    src.write_bytes(archive.read_bytes())
    hot.write_bytes(hotlist)
    os.utime(src, (10 + index * 999999, 10 + index * 999999))
    args = [mkfs, '--tar=f', '-zlz4', '-b4096',
            '-T0', '--mkfs-time', '--preserve-mtime', '-U'+fixed,
            *(['--workers='+str(workers)] if '--workers=' in helptext else []), '--hot-file-list='+str(hot), str(image), str(src)]
    env = dict(os.environ, TZ=('UTC' if index == 0 else 'Asia/Shanghai'), LC_ALL='C')
    env.pop('SOURCE_DATE_EPOCH', None)
    p = subprocess.run(args, capture_output=True, text=True, timeout=60, env=env)
    assert p.returncode == 0, p.stderr
    data = image.read_bytes()
    assert data[1072:1088] == uuid.UUID(fixed).bytes
    info = subprocess.run([dump, '--path=/', str(image)], capture_output=True, text=True, check=True, timeout=30).stdout
    assert 'Uid: 0   Gid: 0  Access: 0755/' in info, info
    digests.append(hashlib.sha256(data).hexdigest())
assert digests[0] == digests[1], digests
print('PASS identical output across input paths, input TAR mtimes, timezone, workers=1/2')
temporary.cleanup()
