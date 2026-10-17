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
    with tarfile.open(archive, 'w') as tf:
        for name, data in [('large', payload), ('small', b'small' * 8193)]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            if name == 'small':
                info.type = tarfile.AREGTYPE
            tf.addfile(info, io.BytesIO(data))
    original = archive.read_bytes()
    common = [mkfs, '--tar=f', '-T0',
              '-U11111111-1111-1111-1111-111111111111',
              '--xattr-inode-digest=user.sha256']
    if b'--workers=' in subprocess.check_output([mkfs, '--help']):
        common += ['--workers=2']
    for label, flags in [('plain', []), ('lz4', ['-zlz4'])]:
        images = []
        for source in ['direct', 'pipe']:
            image = base / (label + '-' + source)
            cmd = common + flags + [image]
            result = run(cmd + [archive]) if source == 'direct' else run(cmd, input=original)
            assert result.returncode == 0, result.stderr
            images.append(image.read_bytes())
        assert images[0] == images[1], label
        extract = base / (label + '-extract')
        result = run([fsck, '--xattrs', '--xattr-inode-digest', '--extract=' + str(extract), image])
        assert result.returncode == 0, result.stderr
        assert (extract / 'large').read_bytes() == payload
        assert (extract / 'small').read_bytes() == b'small' * 8193
        assert os.getxattr(extract / 'large', 'user.sha256') == b'sha256:' + hashlib.sha256(payload).digest()
        truncated = base / 'truncated.tar'
        truncated.write_bytes(original[:4096])
        assert run(common + flags + [base / 'truncated.erofs', truncated]).returncode > 0
        print('PASS direct tar content, digest and truncation:', label)

    # The default data device must also apply to unencoded direct tar payloads.
    blobs, images = [], []
    for source in ['direct', 'pipe']:
        image, blob = base / ('external-' + source), base / ('blob-' + source)
        cmd = common + ['--blobdev=' + str(blob), image]
        result = run(cmd + [archive]) if source == 'direct' else run(cmd, input=original)
        assert result.returncode == 0, result.stderr
        images.append(image.read_bytes())
        blobs.append(blob.read_bytes())
        result = run([fsck, '--device=' + str(blob), '--extract', image])
        assert result.returncode == 0, result.stderr
    assert images[0] == images[1] and blobs[0] == blobs[1]
    print('PASS direct tar external data device')

    with tarfile.open(archive, 'w') as tf:
        info = tarfile.TarInfo('unsupported')
        info.type = b'Z'
        tf.addfile(info)
    result = run(common + [base / 'unsupported.erofs', archive])
    assert result.returncode > 0, 'unknown TAR entry was silently omitted'
