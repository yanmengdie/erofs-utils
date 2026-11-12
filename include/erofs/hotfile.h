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

unsigned int erofs_hotfile_rank(const char *path, bool isdir);

#ifdef __cplusplus
}
#endif

#endif
