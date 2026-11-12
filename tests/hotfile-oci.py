#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Regression fixture: OCI hot paths use the same final tree semantics as TAR.
Run with MKFS, DUMP, FSCK pointing at the candidate build. No external registry.
"""
import gzip
import errno
import hashlib
import io
import json
import os
import re
import subprocess
import tarfile
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

root = Path(__file__).resolve().parents[1]
mkfs, dump, fsck = [os.path.abspath(os.environ.get(name, str(root / path)))
                   for name, path in (('MKFS', 'mkfs/mkfs.erofs'),
                                      ('DUMP', 'dump/dump.erofs'),
                                      ('FSCK', 'fsck/fsck.erofs'))]
env = dict(os.environ, NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
def run(*args, ok=True, cwd=None):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=60,
                       cwd=cwd, env=env)
    if (b'failed to initialize tar stream' in p.stderr and
            f'[Error {errno.EOPNOTSUPP}]'.encode() in p.stderr):
        print('SKIP OCI gzip fixture: this build has no gzip decoder', flush=True)
        raise SystemExit(77)
    if ok:
        assert p.returncode == 0, (args, p.returncode, p.stderr.decode())
    return p

def archive(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.PAX_FORMAT) as tf:
        for name, value in entries:
            t = tarfile.TarInfo(name)
            t.mode, t.mtime = 0o644, 0
            if value is None:
                t.type, t.mode = tarfile.DIRTYPE, 0o755
            elif isinstance(value, tuple):
                t.type = tarfile.LNKTYPE if value[0] == 'hard' else tarfile.SYMTYPE
                t.linkname = value[1]
            else:
                t.size = len(value)
            tf.addfile(t, io.BytesIO(value) if isinstance(value, bytes) else None)
    return gzip.compress(stream.getvalue(), mtime=0)

payload = hashlib.shake_128(b'file hot layout OCI fixture').digest(16384)
entries = [('.', None), ('a-cold', payload[::-1]), ('z', None), ('z/hot', payload),
           ('z/cold', payload[::-1]), ('relative', ('sym', 'z/hot')),
           ('absolute', ('sym', '/z/hot')), ('dir-alias', ('sym', '/z')),
           ('hard', ('hard', 'z/hot'))]
blobs, manifests = {}, {}
layered = [[('.', None), ('a-cold', payload[::-1]), ('z/hot', payload),
            ('vanished', ('hard', 'z/hot')), ('overwritten', payload)],
           [('vanished', payload[::-1]), ('overwritten', payload[::-1])]]
for label, layers in [('normal', [entries]),
                      ('empty-link', [entries + [('empty-link', ('sym', ''))]]),
                      ('layered', layered)]:
    descriptors = []
    for source in layers:
        data = archive(source)
        digest = 'sha256:' + hashlib.sha256(data).hexdigest()
        blobs[digest] = data
        descriptors.append(dict(mediaType='application/vnd.oci.image.layer.v1.tar+gzip',
                                digest=digest, size=len(data)))
    manifests[label] = json.dumps(dict(schemaVersion=2,
        mediaType='application/vnd.oci.image.manifest.v1+json',
        layers=descriptors)).encode()
class Registry(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self): self.respond(True)
    def do_HEAD(self): self.respond(False)
    def respond(self, body):
        key = self.path.rsplit('/', 1)[-1]
        if '/manifests/' in self.path and key in manifests:
            data, media = manifests[key], 'application/vnd.oci.image.manifest.v1+json'
        elif '/blobs/' in self.path and key in blobs:
            data, media = blobs[key], 'application/octet-stream'
        else:
            self.send_error(404); return
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Content-Type', media)
        self.end_headers()
        if body: self.wfile.write(data)
server = ThreadingHTTPServer(('127.0.0.1', 0), Registry)
thread = threading.Thread(target=server.serve_forever)
thread.start()
rows = []
try:
    with tempfile.TemporaryDirectory(prefix='oci-hotfile-review-') as temp:
        work = Path(temp)
        def metadata(image, name):
            text = run(dump, '-e', '--path=/' + name, image).stdout.decode()
            nid = int(re.search(r'NID:\s*(\d+)', text)[1])
            ext = [int(v) for v in re.findall(r'^\s*\d+:.*?\|\s*\d+\s*:\s*(\d+)\.\.', text, re.M)]
            return nid, ext
        compression_modes = [[], ['-zlz4']] if b'lz4' in run(mkfs, '-V').stdout else [[]]
        for compression in compression_modes:
            for hot in ['z/hot\n', './z/hot\n', '/z/hot\n', 'relative\n', 'absolute\n', 'dir-alias/hot\n', 'hard\n', 'z/\n', 'missing\n', 'empty-link\n', 'z/./hot\n']:
                label = 'empty-link' if hot == 'empty-link\n' else 'normal'
                manifest = work/'hot.txt'; manifest.write_text(hot)
                image = work/'out.erofs'
                ref = f'127.0.0.1:{server.server_port}/fixture:{label}'
                cwd = work
                if hot == 'z/./hot\n':
                    cwd = work / 'collision'
                    # An image reference can also name a local directory.
                    (cwd / ref / 'z').mkdir(parents=True, exist_ok=True)
                    (cwd / ref / 'z/hot').write_bytes(payload)
                run(mkfs, '-T0', '--oci=f,insecure,platform=linux/arm64',
                    *compression, '--hot-file-list=' + str(manifest), image,
                    ref, cwd=cwd)
                extracted = work / ('extract-' + str(len(rows)))
                run(fsck, '--extract=' + str(extracted), image) if label != 'empty-link' else run(fsck, '--extract', image)
                if label != 'empty-link': assert (extracted / 'z/hot').read_bytes() == payload
                if label != 'empty-link': assert (extracted / 'a-cold').read_bytes() == payload[::-1]
                if label != 'empty-link': assert os.readlink(extracted / 'relative') == 'z/hot'
                if label != 'empty-link': assert os.readlink(extracted / 'absolute') == '/z/hot'
                if label != 'empty-link': assert os.stat(extracted / 'z/hot').st_ino == os.stat(extracted / 'hard').st_ino
                cold_nid, cold_ext = metadata(image, 'a-cold')
                hot_nid, hot_ext = metadata(image, 'z/hot')
                if hot not in ('z/\n', 'missing\n', 'empty-link\n'):
                    assert hot_ext[0] < cold_ext[0], (compression, hot, hot_ext, cold_ext)
                elif hot == 'z/\n':
                    dir_nid, _ = metadata(image, 'z')
                    assert dir_nid < cold_nid, (compression, hot, dir_nid, cold_nid)
                    assert hot_ext[0] > cold_ext[0], 'directory must not recursively heat children'
                assert metadata(image, 'hard')[0] == hot_nid, 'hardlink identity lost'
                rows.append(dict(compression='lz4' if compression else 'plain', hot=hot.strip(), result='PASS'))
            ref = f'127.0.0.1:{server.server_port}/fixture:layered'
            common = [mkfs, '-T0', '-Uclear', '--oci=f,insecure', '--aufs',
                      '--ovlfs-strip=1', *compression]
            baseline = work / 'layered-baseline.erofs'
            run(*common, baseline, ref)
            for name in ('vanished', 'overwritten', 'missing'):
                manifest.write_text('/' + name + '\n')
                image = work / ('layered-' + name + '.erofs')
                run(*common, '--hot-file-list=' + str(manifest), image, ref)
                extracted = work / ('layered-out-' + str(len(rows)))
                run(fsck, '--extract=' + str(extracted), image)
                assert (extracted / 'vanished').read_bytes() == payload[::-1]
                assert (extracted / 'overwritten').read_bytes() == payload[::-1]
                assert (extracted / 'z/hot').read_bytes() == payload
                assert not (extracted / 'missing').exists()
                if name == 'missing':
                    assert image.read_bytes() == baseline.read_bytes(), 'absent path changed layout'
                else:
                    assert metadata(image, name)[1][0] < metadata(image, 'a-cold')[1][0]
                    assert metadata(image, 'a-cold')[1][0] < metadata(image, 'z/hot')[1][0], 'old alias kept its rank'
                rows.append(dict(compression='lz4' if compression else 'plain',
                                 hot='layered/' + name, result='PASS'))
finally:
    server.shutdown(); thread.join(); server.server_close()
print(json.dumps(rows, indent=2))
