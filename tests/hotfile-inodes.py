#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Hot metadata ordering, image hardlink counts and heap ordering."""
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
mkfs = os.environ.get('MKFS', str(repo / 'mkfs/mkfs.erofs'))
dump = os.environ.get('DUMP', str(repo / 'dump/dump.erofs'))
fsck = os.environ.get('FSCK', str(repo / 'fsck/fsck.erofs'))
has_lz4 = b'lz4' in subprocess.check_output([mkfs, '-V'])


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, (args, p.stderr)
    return p.stdout


def extents(image, path):
    return [(int(a), int(b)) for a, b in re.findall(
        r'^\s*\d+:.*?\|\s*\d+\s*:\s*(\d+)\.\.\s*(\d+)\s*\|',
        run(dump, '-e', '--path=' + path, image), re.M)]


with tempfile.TemporaryDirectory(prefix='erofs-inodes-') as tmp:
    work = Path(tmp)
    payload = random.Random(42).randbytes(8192)

    def build(label, root, options, hot):
        image, manifest = work / (label + '.erofs'), work / (label + '.hot')
        manifest.write_text(hot or '')
        flags = [] if hot is None else ['--hot-file-list=' + str(manifest)]
        run(mkfs, '-T0', '-Uclear', '-b4096', *options, *flags, image, root)
        run(fsck, image)
        return image

    # A multi-block directory and one whose xattr prevents inline packing.
    for count, xattr in [(262, False), (129, True)]:
        root = work / f'root-{count}'
        root.mkdir()
        for i in range(count):
            (root / f'f{i:018}').touch()
        (root / 'hot').write_bytes(payload)
        if xattr:
            os.setxattr(root, b'user.hotfile-boundary', b'x' * 256)
        sources = [('dir', root, [])]
        for kind, source, options in sources:
            for compressed in ([False, True] if has_lz4 else [False]):
                label = f'tail-{count}-{kind}-{compressed}'
                image = build(label, source, options + (['-zlz4'] if compressed else []), 'hot\n')
                assert max(end for _, end in extents(image, '/')) <= min(
                    start for start, _ in extents(image, '/hot')), label
                out = work / (label + '-out')
                run(fsck, '--extract=' + str(out), image)
                assert (out / 'hot').read_bytes() == payload
                assert len(list(out.iterdir())) == count + 1

    root = work / 'links'
    root.mkdir()
    (root / 'a').write_bytes(payload)
    os.link(root / 'a', root / 'b')
    (root / 'sub').mkdir()
    os.link(root / 'a', root / 'sub/alias')
    os.link(root / 'a', work / 'outside-alias')
    (root / 'hot').write_bytes(payload[::-1])
    for index, hot in enumerate([None, 'a\n', 'hot\n']):
        for excluded in [False, True]:
            options = ['--exclude-path=b'] if excluded else []
            image = build(f'links-{index}-{excluded}', root, options, hot)
            entries = ['a', 'sub/alias'] + ([] if excluded else ['b'])
            metadata = [tuple(map(int, re.search(r'NID:\s*(\d+)\s+Links:\s*(\d+)',
                        run(dump, '--path=/' + entry, image)).groups())) for entry in entries]
            assert len({nid for nid, _ in metadata}) == 1, metadata
            assert all(links == len(entries) for _, links in metadata), metadata

    # Selecting a directory also prioritizes symlinks that resolve to it.
    root = work / 'directory-alias'
    (root / 'real').mkdir(parents=True)
    for name in ['real/hot', 'real/cold'] + [f'a-cold-{i:03}' for i in range(24)]:
        (root / name).write_bytes(payload)
    (root / 'z-alias').symlink_to('real')
    for kind, source, options in [('dir', root, [])]:
        for rank, hot in enumerate(['real\n', 'real/\n', 'real/hot\n']):
            image = build(f'alias-{kind}-{rank}', source, options, hot)
            nids = {name: int(re.search(r'NID:\s*(\d+)',
                    run(dump, '--path=/' + name, image))[1])
                    for name in ['real', 'z-alias', 'a-cold-000']}
            assert nids['real'] < nids['a-cold-000']
            assert nids['z-alias'] < nids['a-cold-000']
            offsets = {name: min(start for start, _ in extents(image, '/' + name))
                       for name in ['real/hot', 'real/cold', 'a-cold-000']}
            assert offsets['real/cold'] > offsets['a-cold-000']
            assert (offsets['real/hot'] < offsets['a-cold-000']) == (rank == 2)
            if rank == 0:
                bare = nids, offsets
            elif rank == 1:
                assert (nids, offsets) == bare

    # Interleaved descendants cross the heap's initial allocation size.
    root = work / 'ranks'
    root.mkdir()
    order = []
    for i in range(128):
        name = f'd{i % 4}/file{i:03}'
        target = root / name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(payload)
        order.append(name)
    random.Random(123).shuffle(order)
    layouts = [('plain', [])]
    if has_lz4:
        layouts.append(('grouped', ['-zlz4', '--zD']))
    for label, options in layouts:
        image = build('ranks-' + label, root, options, '\n'.join(order) + '\n')
        offsets = [min(start for start, _ in extents(image, '/' + name)) for name in order]
        assert all(a < b for a, b in zip(offsets, offsets[1:])), offsets

    # A later explicit directory must retain its earlier descendant rank.
    root = work / 'directory-rank'
    for name in ('d', 'e'):
        (root / name).mkdir(parents=True)
        (root / name / 'hot').write_bytes(payload)
    image = build('directory-rank', root, [], 'd/hot\ne/hot\nd\n')
    nids = [int(re.search(r'NID:\s*(\d+)', run(dump, '--path=/' + name, image))[1])
            for name in ('d', 'e')]
    assert nids[0] < nids[1], nids

    # Special files are not hot payloads and must not enable reservations.
    root = work / 'special'
    root.mkdir()
    (root / 'data').write_bytes(payload)
    os.mkfifo(root / 'fifo')
    baseline = build('special-nohot', root, [], None)
    ignored = build('special-ignored', root, [], 'fifo\n')
    assert ignored.read_bytes() == baseline.read_bytes()
