#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
"""Rebuild compressed layers and apply TAR changes without a namespace plan."""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(root / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(root / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(root / 'fsck/fsck.erofs'))


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=30)
    assert p.returncode == 0, (args, p.stderr.decode())
    return p.stdout.decode()


with tempfile.TemporaryDirectory(prefix='erofs-fsmerge-') as tmp:
    base = Path(tmp)

    def archive(name, entries):
        path = base / (name + '.tar')
        with tarfile.open(path, 'w') as tf:
            for name, data in entries:
                info = tarfile.TarInfo(name)
                info.mode, info.uid, info.gid = 0o640, 123, 456
                if data is None:
                    info.type, info.mode = tarfile.DIRTYPE, 0o750
                    info.pax_headers = {'SCHILY.xattr.user.keep': 'root metadata'}
                    tf.addfile(info)
                elif isinstance(data, tuple):
                    info.type, info.linkname = data
                    tf.addfile(info)
                else:
                    info.size = len(data)
                    tf.addfile(info, io.BytesIO(data))
        return path

    lower_data = b'lower' * 16384
    upper_data = b'upper' * 16384
    raw = hashlib.shake_128(b'raw').digest(8192)
    lower = archive('lower', [('.', None), ('data', lower_data), ('alias', (tarfile.LNKTYPE, 'data')),
                             ('gone', raw), ('dir/old', raw), ('raw', raw),
                             ('lower-link', (tarfile.SYMTYPE, 'alias'))])
    lower_image, lower_blob = base / 'lower.erofs', base / 'lower.blob'
    metadata = archive('metadata-only', [('.', None), ('empty-dir', None)])
    metadata_image = base / 'metadata-only.erofs'
    run(mkfs, '-b4096', '--tar=f', metadata_image, metadata)
    run(mkfs, '-zlz4', '-T0', '-Uclear', '--tar=f', '--aufs', '-Enoinline_data',
        '--blobdev=' + str(lower_blob), lower_image, lower, metadata_image)
    # Resolve the hardlink before replacing its target in the upper layer.
    upper = archive('upper', [('saved', (tarfile.LNKTYPE, 'data')), ('data', upper_data),
                             ('.wh.gone', b''), ('dir/.wh..wh..opq', b''),
                             ('dir/new', upper_data), ('link', (tarfile.SYMTYPE, 'saved'))])
    merged, upper_blob = base / 'merged.erofs', base / 'upper.blob'
    hot = base / 'hotlist'
    hot.write_text('/lower-link\n')
    before = lower_blob.read_bytes()
    run(mkfs, '-zlz4', '-T0', '-Uclear', '--tar=f', '--aufs', '--ovlfs-strip=1',
        '-Enoinline_data', '--hot-file-list=' + str(hot),
        '--blobdev=' + str(upper_blob), merged, upper, lower_image)
    assert lower_blob.read_bytes() == before
    rebuilt = base / 'rebuilt.erofs'
    run(mkfs, '-zlz4', '-T0', '-Uclear', rebuilt, merged)
    for image in (merged, rebuilt):
        assert 'Uid: 123   Gid: 456  Access: 0750/' in run(dump, '--path=/', image)
        out = base / (image.stem + '-out')
        run(fsck, '--xattrs', '--extract=' + str(out), '--device=' + str(lower_blob),
            '--device=' + str(upper_blob), image)
        for name, expected in {'data': upper_data, 'saved': lower_data,
                               'alias': lower_data, 'raw': raw, 'dir/new': upper_data}.items():
            assert (out / name).read_bytes() == expected, (image, name)
        assert os.getxattr(out, 'user.keep') == b'root metadata'
        assert not (out / 'gone').exists()
        assert not (out / 'dir/old').exists()
        assert os.readlink(out / 'link') == 'saved'
        assert os.readlink(out / 'lower-link') == 'alias'
        assert (out / 'saved').stat().st_ino == (out / 'alias').stat().st_ino
        assert (out / 'saved').stat().st_nlink == 2
    print('PASS compressed TAR overlay, cross-layer hardlink, overwrite, whiteouts and multi-device rebuild')

    # An opaque root in an upper EROFS layer hides all lower entries.
    opaque = archive('opaque', [('.', None), ('.wh..wh..opq', b''), ('only', upper_data)])
    opaque_image, opaque_blob = base / 'opaque.erofs', base / 'opaque.blob'
    run(mkfs, '-zlz4', '--tar=f', '--aufs', '-Enoinline_data',
        '--blobdev=' + str(opaque_blob), opaque_image, opaque)
    run(mkfs, '-zlz4', '--ovlfs-strip=1', rebuilt, lower_image, opaque_image)
    out = base / 'opaque-out'
    run(fsck, '--extract=' + str(out), '--device=' + str(lower_blob),
        '--device=' + str(opaque_blob), rebuilt)
    assert sorted(p.name for p in out.iterdir()) == ['only']

    sparse = base / 'sparse'
    sparse.mkdir()
    with (sparse / 'data').open('wb') as f:
        f.write(b'payload' * 512)
        f.seek(12288)
        f.write(b'tail' * 1024)
    image, blob = base / 'sparse.erofs', base / 'sparse.blob'
    run(mkfs, '--chunksize=4096', '--blobdev=' + str(blob), image, sparse)
    run(mkfs, rebuilt, image)
    out = base / 'sparse-out'
    run(fsck, '--extract=' + str(out), '--device=' + str(blob), rebuilt)
    assert (out / 'data').read_bytes() == (sparse / 'data').read_bytes()
    print('PASS sparse external data survives native rebuild')
