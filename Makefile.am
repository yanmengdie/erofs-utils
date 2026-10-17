# SPDX-License-Identifier: MIT

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = man lib mkfs dump fsck
if ENABLE_FUSE
SUBDIRS += fuse
endif
SUBDIRS += mount contrib

TESTS =
TESTS_ENVIRONMENT = MKFS='$(abs_builddir)/mkfs/mkfs.erofs' \
	DUMP='$(abs_builddir)/dump/dump.erofs' \
	FSCK='$(abs_builddir)/fsck/fsck.erofs'
LOG_COMPILER = python3
if ENABLE_LZ4
TESTS += tests/hotfile-links.py tests/hotfile-inodes.py tests/tar-direct.py
endif
EXTRA_DIST = tests/hotfile-links.py tests/hotfile-inodes.py tests/tar-direct.py
