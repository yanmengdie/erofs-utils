#!/usr/bin/env python3
"""Direct and streamed TAR imports preserve file data and external devices."""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(root / 'mkfs/mkfs.erofs'))
fsck = os.environ.get('FSCK', str(root / 'fsck/fsck.erofs'))

def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), capture_output=True, timeout=30, **kwargs)


with tempfile.TemporaryDirectory(prefix='erofs-tar-') as tmp:
    base = Path(tmp)
    archive = base / 'source.tar'
    # Cross the 16 MiB compression segment boundary and leave a partial tail.
    payload = bytes(range(256)) * (65536 + 17)
