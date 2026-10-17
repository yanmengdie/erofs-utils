/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_HOTFILE_H
#define __EROFS_HOTFILE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "defs.h"

struct erofs_inode;

int erofs_hotfile_load(const char *path, const char *root);
int erofs_hotfile_prepare_tree(struct erofs_inode *root);
void erofs_hotfile_exit(void);
bool erofs_hotfile_enabled(void);

unsigned int erofs_hotfile_rank(const char *path);

/* Byte-range hot layout: hot ranges of local files keyed by (st_dev, st_ino). */
bool erofs_hotchunk_enabled(void);
bool erofs_hotfile_has_chunk_ranges(u32 device, u64 inode);
unsigned int erofs_get_hot_chunk_rank(u32 device, u64 inode,
				      u64 offset, u64 length);

#ifdef __cplusplus
}
#endif

#endif
