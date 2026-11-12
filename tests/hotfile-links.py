#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Hot paths resolve inside the image and preserve links and file contents."""
import hashlib
import io
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


def run(args):
    return subprocess.run([str(x) for x in args], capture_output=True, timeout=60)


def check(condition, detail):
    if not condition:
        raise AssertionError(detail)


def main(out):
    repo = Path(__file__).resolve().parents[1]
    mkfs = os.environ.get('MKFS', str(repo / 'mkfs/mkfs.erofs'))
    dump = os.environ.get('DUMP', str(repo / 'dump/dump.erofs'))
    fsck = os.environ.get('FSCK', str(repo / 'fsck/fsck.erofs'))
    base = [('a-cold', 'f', 'C'), ('z-target', 'f', 'Z')]
    cases = {
        'relative': (base + [('hot', 's', 'z-target')], 'hot\n', 'z-target'),
        'absolute': (base + [('hot', 's', '/z-target')], 'hot\n', 'z-target'),
        'chain': (base + [('hop', 's', '/z-target'), ('hot', 's', 'hop')], 'hot\n', 'z-target'),
        'directory': (base + [('real/file', 'f', 'R'), ('alias', 's', '/real')], 'alias/./file\n', 'real/file'),
        'parent-after-link': (base + [('a/file', 'f', 'W'), ('real/file', 'f', 'R'), ('real/sub/dummy', 'f', 'D'), ('a/link', 's', '../real/sub')], 'a/link/../file\n', 'real/file'),
        'leading-space': (base + [(' z-target', 'f', 'W'), ('  control', 'f', 'K')], 'z-target\n', 'z-target'),
        'trailing-space': (base + [('z-target ', 'f', 'W')], 'z-target \n', 'z-target '),
        'hash-name': (base + [('#hot', 'f', 'H')], '# comment\n./#hot\n', '#hot'),
        'hard-rank': (base + [('b-alias', 'h', 'z-target'), ('sub/hot', 'h', 'z-target')], 'sub/hot\na-cold\nb-alias\n', 'z-target'),
        'overwritten-alias': (base + [('hot', 'h', 'z-target'), ('hot', 'f', 'N')], 'hot\n', 'hot'),
        'loop': (base + [('hot', 's', 'hop'), ('hop', 's', 'hot')], 'hot\n', None),
        'escape': (base + [('hot', 's', '../../outside')], 'hot\n', None),
    }
    for name, (entries, hot, target) in cases.items():
        case = out / name
        case.mkdir(exist_ok=True)
        root = case / 'root'
        root.mkdir(exist_ok=True)
        archive = case / 'root.tar'
        expected = {}
        with tarfile.open(archive, 'w', format=tarfile.PAX_FORMAT) as tf:
            for path, kind, value in entries:
                dest = root / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                if dest.exists() or dest.is_symlink():
                    dest.unlink()
                ti = tarfile.TarInfo('./' + path)
                ti.mode = 0o644
                if kind == 'f':
                    payload = value.encode() * 65536
                    dest.write_bytes(payload)
                    expected[path] = ('f', hashlib.sha256(payload).hexdigest())
                    ti.size = len(payload)
                    tf.addfile(ti, io.BytesIO(payload))
                elif kind == 'h':
                    os.link(root / value, dest)
                    expected[path] = expected[value]
                    ti.type, ti.linkname = tarfile.LNKTYPE, './' + value
                    tf.addfile(ti)
                else:
                    dest.symlink_to(value)
                    expected[path] = ('s', value)
                    ti.type, ti.linkname = tarfile.SYMTYPE, value
                    tf.addfile(ti)
        manifest = case / 'hot.txt'
        manifest.write_text(hot)
        for mode in ('dir', 'tar'):
            image = case / (mode + '.erofs')
            cmd = [mkfs, '-T0', '-U00000000-0000-0000-0000-000000000000', '--hot-file-list=' + str(manifest)]
            if mode == 'tar':
                cmd += ['--tar=f']
            p = run(cmd + [image, root if mode == 'dir' else archive])
            row = {'case': name, 'mode': mode, 'rc': p.returncode}
            if target is None:
                check(p.returncode != 0, row)
                continue
            check(p.returncode == 0, (row, p.stderr))
            extract = case / (mode + '-extract')
            p = run([fsck, '--extract=' + str(extract), '--overwrite', image])
            check(p.returncode == 0, (row, p.stderr))
            for path, (kind, value) in expected.items():
                actual = extract / path
                check(actual.is_symlink() if kind == 's' else actual.is_file(), (row, path))
                check((os.readlink(actual) if kind == 's' else hashlib.sha256(actual.read_bytes()).hexdigest()) == value, (row, path, 'content'))
            offsets = {}
            for path, (kind, _) in expected.items():
                if kind != 'f':
                    continue
                p = run([dump, '-e', '--path=/' + path, image])
                m = re.search(rb'^\s*0:\s*\d+\.\.\s*\d+\s*\|\s*\d+\s*:\s*(\d+)\.\.', p.stdout, re.M)
                check(m is not None, (row, path, p.stdout))
                offsets[path] = int(m[1])
            check(offsets[target] < offsets['a-cold'], (row, offsets))
            if name == 'leading-space':
                check(offsets[' z-target'] > offsets['  control'], (row, offsets))
            if name == 'parent-after-link':
                check(offsets['a/file'] > offsets['a-cold'], (row, offsets))
            if name == 'overwritten-alias':
                check(offsets['z-target'] > offsets['a-cold'], (row, offsets))
            if name == 'hard-rank':
                check(len({offsets[x] for x in ('z-target', 'b-alias', 'sub/hot')}) == 1, (row, offsets))
    # Empty/invalid candidates must not enable hot metadata reservations.
    case = out / 'relative'
    for mode in ('dir', 'tar'):
        source = case / ('root' if mode == 'dir' else 'root.tar')
        cmd = [mkfs, '-T0', '-U00000000-0000-0000-0000-000000000000']
        if mode == 'tar':
            cmd += ['--tar=f']
        reference = case / (mode + '-nohot.erofs')
        check(run(cmd + [reference, source]).returncode == 0, 'no-hot baseline')
        for label, content in (('empty', b''), ('comments', b'# only comment\n'),
                               ('missing', b'not/present\n'),
                               ('nul', b'z-target\x00ignored\n')):
            manifest = case / (label + '.txt')
            manifest.write_bytes(content)
            image = case / (mode + '-' + label + '.erofs')
            p = run(cmd + ['--hot-file-list=' + str(manifest), image, source])
            if label == 'nul':
                check(p.returncode != 0, (mode, 'embedded NUL accepted'))
            else:
                check(p.returncode == 0, (mode, label, p.stderr))
                check(image.read_bytes() == reference.read_bytes(), (mode, label, 'no-op changed image'))
    # Explicit conflicting sort requests must fail independent of option order.
    case = out / 'relative'
    manifest = case / 'sort-hot.txt'
    manifest.write_text('z-target\n')
    common = [mkfs, '--tar=f', '-Enoinline_data']
    for compression in ([], ['-zlz4']):
        for reverse in (False, True):
            options = ['--sort=none', '--hot-file-list=' + str(manifest)]
            if reverse:
                options.reverse()
            p = run(common + compression + options + [case / 'conflict.erofs', case / 'root.tar'])
            check(p.returncode != 0 and b'conflicts with --sort=none' in p.stderr,
                  ('sort conflict', compression, reverse, p.stderr))
    p = run(common + ['--sort=none', case / 'unsorted.erofs', case / 'root.tar'])
    check(p.returncode == 0, ('no-hot unsorted', p.stderr))
    check(run([fsck, '--extract', case / 'unsorted.erofs']).returncode == 0,
          'no-hot unsorted fsck')
    print('PASS hot-path resolution and layout')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='erofs-hotfile-') as tmp:
        main(Path(tmp))
