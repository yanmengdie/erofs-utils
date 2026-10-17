/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_LIB_LIBEROFS_REBUILD_H
#define __EROFS_LIB_LIBEROFS_REBUILD_H

#include "erofs/internal.h"

enum erofs_rebuild_datamode {
	EROFS_REBUILD_DATA_BLOB_INDEX,
	EROFS_REBUILD_DATA_RESVSP,
	EROFS_REBUILD_DATA_FULL,
};

struct erofs_dentry *erofs_d_lookup(struct erofs_inode *dir, const char *name);

struct erofs_dentry *erofs_rebuild_get_dentry(struct erofs_inode *pwd,
		char *path, bool aufs, bool *whout, bool *opq, bool to_head);
struct erofs_dentry *erofs_rebuild_get_dentry_layer(struct erofs_inode *pwd,
		char *path, bool aufs, bool *whout, bool *opq, bool to_head,
		u32 tar_layer, struct erofs_inode **parent);

int erofs_rebuild_load_tree(struct erofs_inode *root, struct erofs_sb_info *sbi,
			    enum erofs_rebuild_datamode mode, bool inherit_root);
int erofs_rebuild_load_basedir(struct erofs_inode *dir, u64 *nr_subdirs,
			       unsigned int *i_nlink);
int erofs_rebuild_count_devices(struct erofs_sb_info *sbi);
#endif
