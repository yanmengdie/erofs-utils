#!/usr/bin/env python3
"""OCI import preserves layer semantics, hot paths and input integrity."""
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import threading
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def layer(entries):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as archive:
        for name, value in entries:
            header = tarfile.TarInfo(name)
            header.mode = 0o755 if value is None else 0o644
            header.mtime = 0
            if value is None:
                header.type = tarfile.DIRTYPE
            elif isinstance(value, tuple) and value[0] == 'rootmeta':
                header.type = tarfile.DIRTYPE
                header.mode, header.uid, header.gid, header.mtime = value[1:]
            elif isinstance(value, tuple):
                header.type = tarfile.LNKTYPE if value[0] == 'hardlink' else tarfile.SYMTYPE
                header.linkname = value[1]
            else:
                header.size = len(value)
            archive.addfile(header, io.BytesIO(value) if isinstance(value, bytes) else None)
    return gzip.compress(raw.getvalue(), mtime=0)


def main(base):
    root = Path(__file__).resolve().parents[1]
    mkfs = os.environ.get('MKFS', str(root / 'mkfs/mkfs.erofs'))
    fsck = os.environ.get('FSCK', str(root / 'fsck/fsck.erofs'))
    cases = [
        ('opaque-links', [
            [('d/a', b'old'), ('d/sub/b', b'gone'), ('alias', ('hardlink', 'd/a')),
             ('removed', b'gone'), ('keep', b'keep')],
            [('d/pre', b'pre'), ('d/.wh..wh..opq', b''), ('d/post', b'post'),
             ('.wh.removed', b''), ('.wh.removed', b''), ('renamed', ('hardlink', 'd/a'))],
        ], {'alias': b'old', 'renamed': b'old', 'keep': b'keep',
            'd/pre': b'pre', 'd/post': b'post'}, {'d'}, [('alias', 'renamed')]),
        ('no-manifest-media-type', [[('file', b'valid manifest')]],
         {'file': b'valid manifest'}, set(), []),
        ('root-opaque', [
            [('old/a', b'gone'), ('removed', b'gone')],
            [('new/a', b'new'), ('.wh..wh..opq', b''), ('empty/.wh..wh..opq', b'')],
        ], {'new/a': b'new'}, {'new', 'empty'}, []),
        ('same-layer', [
            [('item', b'old'), ('parent/old', b'gone')],
            [('item', b'new'), ('.wh.item', b''), ('.wh.parent', b''),
             ('parent/new', b'new'), ('.wh.missing', b''),
             ('missing/.wh..wh..opq', b''), ('after', b'new'), ('.wh.after', b'')],
        ], {'item': b'new', 'parent/new': b'new', 'after': b'new'}, {'parent', 'missing'}, []),
        ('three-layers', [
            [('d/old', b'gone')], [('d/.wh..wh..opq', b''), ('d/new', b'new')],
            [('d/later', b'later'), ('d/.wh.new', b'')],
        ], {'d/later': b'later'}, {'d'}, []),
        ('replacements', [
            [('d/old', b'gone'), ('file', b'old'), ('link', ('hardlink', 'd/old'))],
            [('d/.wh..wh..opq', b''), ('d', b'file'), ('d/.wh.old', b''),
             ('file/new', b'new'), ('symlink', ('symlink', 'file/new'))],
        ], {'d': b'file', 'file/new': b'new', 'link': b'gone',
            'symlink': ('symlink', 'file/new')}, {'file'}, []),
        ('alias-replacement', [
            [('origin/file', b'original'), ('other/alias', ('hardlink', 'origin/file'))],
            [('other/alias', None), ('other/alias/new', b'new')],
        ], {'origin/file': b'original', 'other/alias/new': b'new'},
         {'origin', 'other', 'other/alias'}, []),
        ('detached-alias-replacement', [
            [('gone/file', b'original'), ('alias', ('hardlink', 'gone/file'))],
            [('gone', b'replaced'), ('alias', None), ('alias/new', b'new')],
        ], {'gone': b'replaced', 'alias/new': b'new'}, {'alias'}, []),
        ('hardlinked-symlink-chain', [[
            ('target', None), ('sub/target', None), ('sub/original', ('symlink', 'target')),
            ('alias', ('hardlink', 'sub/original')), ('first', ('symlink', 'alias')),
            ('first/new', b'new')],
        ], {'sub/original': ('symlink', 'target'), 'alias': ('symlink', 'target'),
            'first': ('symlink', 'alias'), 'target/new': b'new'},
         {'target', 'sub', 'sub/target'}, []),
        ('symlink-copyup-opaque', [
            [('real/old', b'old')],
            [('alias', ('symlink', 'real')), ('alias/new', b'new'), ('.wh..wh..opq', b'')],
        ], {'alias': ('symlink', 'real'), 'real/new': b'new'}, {'real'}, []),
    ]
    # Lexically late hot files make silently ignored hotlists observable.
    payload = hashlib.shake_128(b'oci-hotlist').digest(8192)
    cases.append(('empty-link-hotlist', [[('empty', ('symlink', '')), ('healthy', payload)]],
                  None, set(), []))
    host_only = base.resolve() / 'host-only-payload'
    host_only.write_bytes(payload)
    cases.append(('hotlist-root', [
        [('.', ('rootmeta', 0o711, 1234, 5678, 42)), ('real/old', b'gone'),
         ('shortcut', ('symlink', 'real')), ('pkg-alias', ('symlink', 'real/pkg')), ('absolute', ('symlink', '/real')),
         ('parent/relative', ('symlink', '../../real')),
         ('cycle-a', ('symlink', 'cycle-b')), ('cycle-b', ('symlink', 'cycle-a')),
         ('host-only', ('symlink', str(host_only)))],
        [('real/.wh..wh..opq', b''), ('A_cold', payload), ('real/z_hot', payload),
         ('real/pkg/p.py', payload), ('real/pkg/__pycache__/p.cpython-310.opt-1.pyc', payload),
         ('real/pkg/missing.py', payload), ('healthy', payload)],
    ], {'A_cold': payload, 'real/z_hot': payload, 'real/pkg/p.py': payload,
         'real/pkg/__pycache__/p.cpython-310.opt-1.pyc': payload,
         'real/pkg/missing.py': payload, 'healthy': payload,
         'shortcut': ('symlink', 'real'), 'pkg-alias': ('symlink', 'real/pkg'), 'absolute': ('symlink', '/real'),
         'parent/relative': ('symlink', '../../real'),
         'cycle-a': ('symlink', 'cycle-b'), 'cycle-b': ('symlink', 'cycle-a'),
         'host-only': ('symlink', str(host_only))},
         {'real', 'real/pkg', 'real/pkg/__pycache__', 'parent'}, []))
    cases.append(('mixed-layers', [
        [('plain/file', payload)], [('concat/file', payload), ('link', ('hardlink', 'plain/file'))],
        [('final/file', payload)],
    ], {'plain/file': payload, 'concat/file': payload, 'final/file': payload, 'link': payload},
         {'plain', 'concat', 'final'}, [('plain/file', 'link')]))
    cases.append(('invalid-layer', [[('old', b'old')], [('.wh.old', b''), ('new', b'new')]], None, set(), []))
    for label in ['bad-gzip-crc', 'truncated-gzip']:
        cases.append((label, [[('old', payload)]], None, set(), []))
    blobs, manifests = {}, {}
    digests = {}
    for name, layers, *_ in cases:
        desc, diff_ids = [], []
        for i, entries in enumerate(layers):
            data = layer(entries)
            if name == 'invalid-layer' and i == 1:
                corrupt = bytearray(gzip.decompress(data))
                corrupt[512] ^= 1
                data = gzip.compress(corrupt, mtime=0)
            decoded = gzip.decompress(data)
            diff_ids.append('sha256:' + hashlib.sha256(decoded).hexdigest())
            media = 'application/vnd.oci.image.layer.v1.tar+gzip'
            if name == 'mixed-layers':
                if i == 0:
                    data = decoded + b'nonstandard tail'
                    media = 'application/vnd.oci.image.layer.v1.tar'
                    diff_ids[-1] = 'sha256:' + hashlib.sha256(data).hexdigest()
                elif i == 1:
                    data = gzip.compress(decoded[:513], mtime=0) + gzip.compress(decoded[513:], mtime=0)
                else:
                    media = 'application/octet-stream'  # legacy decoder fallback
            elif name == 'bad-gzip-crc':
                damaged = bytearray(data)
                damaged[-8] ^= 1
                data = bytes(damaged)
            elif name == 'truncated-gzip':
                data = data[:-8]
            digest = 'sha256:' + hashlib.sha256(data).hexdigest()
            blobs[digest] = data
            desc.append(dict(mediaType=media,
                             digest=digest, size=len(data)))
        config = json.dumps(dict(architecture='amd64', os='linux', config={},
                                 rootfs=dict(type='layers', diff_ids=diff_ids))).encode()
        config_digest = 'sha256:' + hashlib.sha256(config).hexdigest()
        blobs[config_digest] = config
        manifest_body = dict(schemaVersion=2,
            config=dict(mediaType='application/vnd.oci.image.config.v1+json',
                        digest=config_digest, size=len(config)), layers=desc)
        if name != 'no-manifest-media-type':
            manifest_body['mediaType'] = 'application/vnd.oci.image.manifest.v1+json'
        manifest = json.dumps(manifest_body).encode()
        manifests[name] = manifest
        digests[name] = 'sha256:' + hashlib.sha256(manifest).hexdigest()
        manifests[digests[name]] = manifest

    class Registry(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_HEAD(self):
            self.respond(False)

        def do_GET(self):
            self.respond(True)

        def respond(self, body):
            name = self.path.rsplit('/', 1)[-1]
            if '/manifests/' in self.path and name in manifests:
                data, media = manifests[name], 'application/vnd.oci.image.manifest.v1+json'
            elif '/blobs/' in self.path and name in blobs:
                data, media = blobs[name], 'application/octet-stream'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Content-Type', media)
            self.end_headers()
            if body:
                self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Registry)
    env = os.environ.copy()
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        for name, _, files, directories, links in cases:
            for compression in (['plain', 'lz4', 'chunk'] if name == 'opaque-links' else ['plain', 'lz4']):
                output = base / (name + '-' + compression)
                output.mkdir()
                image = output / 'image.erofs'
                cmd = [mkfs, '--oci=f,insecure']
                cmd += ['-U', '11111111-2222-3333-4444-555555555555', '--MZ=id', '--aufs', '--ovlfs-strip=1', '-T', '0', '--quiet']
                if b'--workers=' in subprocess.check_output([mkfs, '--help']):
                    cmd += ['--workers=2']
                if compression == 'lz4':
                    cmd += ['-zlz4']
                elif compression == 'chunk':
                    cmd += ['--chunksize=4096']
                reference = 'test/app:' + name
                if name == 'root-opaque':
                    reference = 'test/app:ignored@' + digests[name]
                elif name == 'three-layers':
                    reference = 'test/app@' + digests[name]
                raw_hot = None
                if name == 'empty-link-hotlist':
                    hotlist = output / 'empty-link.hotlist'
                    hotlist.write_text('/empty\n/healthy\n')
                    emitted = output / 'emitted.hotlist'
                    cmd += ['--hot-file-list=' + str(hotlist),
                            '--hot-file-list-output=' + str(emitted)]
                if name == 'hotlist-root':
                    raw_hot = output / 'raw.hotlist'
                    raw_hot.write_text('/pkg-alias/../z_hot\n/absolute/pkg/__pycache__/p.cpython-310.opt-1.pyc\n'
                                       '/shortcut/pkg/__pycache__/missing.cpython-310.pyc\n'
                                       '/parent/relative/z_hot\n/../shortcut//./z_hot\n'
                                       '/cycle-a\n/host-only\n/real\n/healthy\n')
                    cmd += ['--hot-file-list=' + str(raw_hot)]
                cmd += [str(image), f'127.0.0.1:{server.server_port}/' + reference]
                result = subprocess.run(cmd, env=env, capture_output=True, timeout=30)
                if name == 'empty-link-hotlist':
                    assert result.returncode == 0, (compression, result.returncode, result.stderr.decode())
                    assert emitted.read_text() == '/healthy\n'
                    print('PASS empty symlink target skipped in hotlist', compression, flush=True)
                    continue
                if files is None:
                    assert result.returncode != 0, 'Corrupted layer was accepted'
                    if name == 'invalid-layer':
                        assert b'chksum mismatch' in result.stderr, result.stderr
                    print('PASS corrupt layer rejected', compression, flush=True)
                    continue
                assert result.returncode == 0, (name, compression, result.stderr.decode())
                extracted = output / 'root'
                result = subprocess.run([fsck, '--extract=' + str(extracted), str(image)],
                                        capture_output=True, timeout=30)
                assert result.returncode == 0, (name, compression, result.stderr.decode())
                actual, dirs = {}, set()
                for path in extracted.rglob('*'):
                    relative = str(path.relative_to(extracted))
                    if path.is_symlink():
                        actual[relative] = ('symlink', os.readlink(path))
                    elif path.is_dir():
                        dirs.add(relative)
                    else:
                        actual[relative] = path.read_bytes()
                assert actual == files, (name, compression, actual, files)
                assert dirs == directories, (name, compression, dirs, directories)
                for a, b in links:
                    assert (extracted / a).stat().st_ino == (extracted / b).stat().st_ino
                dump = str(Path(fsck).parent.parent / 'dump/dump.erofs')
                if name == 'alias-replacement':
                    def nid(path):
                        info = subprocess.check_output([dump, '--path=' + path, str(image)],
                                                       env=env, text=True, timeout=30)
                        return info.split('NID:', 1)[1].split()[0]
                    assert nid('/other/alias/..') == nid('/other')
                rootinfo = subprocess.run([dump, '--path=/', str(image)],
                                          env=dict(env, TZ='UTC'), capture_output=True, text=True, check=True, timeout=30).stdout
                assert 'Uid: 0   Gid: 0  Access: 0755/' in rootinfo, rootinfo
                assert '1970-01-01 00:00:00.000000000' in rootinfo, rootinfo
                if raw_hot:
                    canonical = output / 'resolved.hotlist'
                    canonical.write_text('/real/z_hot\n/real/pkg/__pycache__/p.cpython-310.opt-1.pyc\n'
                                         '/real/pkg/p.py\n/real/pkg/missing.py\n/healthy\n')
                    canonical_cmd = [('--hot-file-list=' + str(canonical))
                                     if a.startswith('--hot-file-list=') else a for a in cmd]
                    canonical_cmd[-2] = str(output / 'resolved.erofs')
                    subprocess.run(canonical_cmd, env=env, check=True, capture_output=True, timeout=30)
                    emitted = output / 'emitted.hotlist'
                    emit_cmd = cmd[:-2] + ['--hot-file-list-output=' + str(emitted)] + cmd[-2:]
                    emit_cmd[-2] = str(output / 'emitted.erofs')
                    subprocess.run(emit_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert emitted.read_bytes() == canonical.read_bytes(), emitted.read_text()
                    expanded = output / 'expanded.hotlist'
                    expand_cmd = cmd[:-2] + ['--hot-file-list-output=' + str(expanded),
                                            '--hot-file-list-directory=/absolute',
                                            '--hot-file-list-directory=/'] + cmd[-2:]
                    expand_cmd[-2] = str(output / 'expanded.erofs')
                    subprocess.run(expand_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert expanded.read_text() == canonical.read_text() + '/A_cold\n', expanded.read_text()
                    explicit_cmd = [('--hot-file-list=' + str(expanded))
                                    if a.startswith('--hot-file-list=') else a for a in cmd]
                    explicit_cmd[-2] = str(output / 'expanded-explicit.erofs')
                    subprocess.run(explicit_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert hashlib.sha256((output / 'expanded.erofs').read_bytes()).hexdigest() == hashlib.sha256((output / 'expanded-explicit.erofs').read_bytes()).hexdigest()
                    empty = output / 'empty.hotlist'
                    empty.write_text('')
                    volume_only = output / 'volume-only.hotlist'
                    volume_cmd = [('--hot-file-list=' + str(empty))
                                  if a.startswith('--hot-file-list=') else a for a in cmd[:-2]]
                    volume_cmd += ['--hot-file-list-directory=/', '--hot-file-list-output=' + str(volume_only),
                                   str(output / 'volume-only.erofs'), cmd[-1]]
                    subprocess.run(volume_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert '/A_cold\n' in volume_only.read_text()
                    replay_cmd = [('--hot-file-list=' + str(volume_only))
                                  if a.startswith('--hot-file-list=') else a for a in cmd]
                    replay_cmd[-2] = str(output / 'volume-only-explicit.erofs')
                    subprocess.run(replay_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert hashlib.sha256((output / 'volume-only.erofs').read_bytes()).hexdigest() == hashlib.sha256((output / 'volume-only-explicit.erofs').read_bytes()).hexdigest()
                    print('PASS resolved hotlist output and directory selection', flush=True)
                    original = hashlib.sha256(image.read_bytes()).hexdigest()
                    assert original == hashlib.sha256((output / 'resolved.erofs').read_bytes()).hexdigest()
                    cold_cmd = [a for a in cmd if not a.startswith('--hot-file-list=')]
                    cold_cmd[-2] = str(output / 'cold.erofs')
                    subprocess.run(cold_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert original != hashlib.sha256((output / 'cold.erofs').read_bytes()).hexdigest()
                    invalid = output / 'invalid.hotlist'
                    invalid.write_text('/cycle-a\n/host-only\n/real\n/nonexistent.pyc\n')
                    invalid_cmd = [('--hot-file-list=' + str(invalid))
                                   if a.startswith('--hot-file-list=') else a for a in cmd]
                    invalid_cmd[-2] = str(output / 'invalid.erofs')
                    subprocess.run(invalid_cmd, env=env, check=True, capture_output=True, timeout=30)
                    assert hashlib.sha256((output / 'invalid.erofs').read_bytes()).hexdigest() == hashlib.sha256((output / 'cold.erofs').read_bytes()).hexdigest()
                    print('PASS raw/resolved hotlist artifact equality; hotlist affects layout', flush=True)
                print('PASS', name, compression, flush=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == '__main__':
    try:
        with tempfile.TemporaryDirectory(prefix='erofs-oci-') as output:
            main(Path(output))
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr.decode(), flush=True)
        raise
