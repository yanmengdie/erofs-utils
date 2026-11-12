# SPDX-License-Identifier: MIT

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = man lib mkfs dump fsck
if ENABLE_FUSE
SUBDIRS += fuse
endif
SUBDIRS += mount contrib

TESTS = tests/xattr-empty.py
TESTS_ENVIRONMENT = MKFS='$(abs_builddir)/mkfs/mkfs.erofs' \
	DUMP='$(abs_builddir)/dump/dump.erofs' \
	FSCK='$(abs_builddir)/fsck/fsck.erofs'
LOG_COMPILER = python3
EXTRA_DIST = tests/xattr-empty.py
