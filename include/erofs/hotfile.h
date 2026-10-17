/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
#ifndef __EROFS_HOTFILE_H
#define __EROFS_HOTFILE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <limits.h>
#include "defs.h"

#define EROFS_HOT_RANK_NONE	UINT_MAX

int erofs_hotfile_load(const char *path);
int erofs_hotfile_add_directory(const char *path);
int erofs_hotfile_write_list(const char *path);
struct erofs_inode;
struct erofs_sb_info;
#define EROFS_HOT_END_XATTR "user.erofs.hot_end"
int erofs_hotfile_reserve_bounds(struct erofs_inode *root);
int erofs_hotfile_write_bounds(struct erofs_inode *root);
int erofs_hotfile_rebuild_bounds(struct erofs_sb_info *dst,
				struct erofs_sb_info *src);
void erofs_hotfile_record_extent(struct erofs_sb_info *sbi,
				unsigned int device, u64 start, u64 length);
int erofs_hotfile_prepare_tree(struct erofs_inode *root);
unsigned int erofs_get_hot_symlink_rank(const char *path);
int erofs_hotfile_resolve_oci(struct erofs_inode *root);
bool erofs_hotfile_enabled(void);
bool erofs_hotchunk_enabled(void);
bool erofs_hotfile_has_chunk_ranges(u32 device, u64 inode);
unsigned int erofs_get_hot_chunk_rank(u32 device, u64 inode,
				      u64 offset, u64 length);
unsigned int erofs_get_hot_file_rank(const char *path);
unsigned int erofs_get_hot_traversal_rank(const char *path);
void erofs_hotfile_exit(void);

#ifdef __cplusplus
}
#endif

#endif
