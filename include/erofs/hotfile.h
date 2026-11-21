/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_HOTFILE_H
#define __EROFS_HOTFILE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "defs.h"

int erofs_hotfile_load(const char *path);
void erofs_hotfile_exit(void);
bool erofs_hotfile_enabled(void);

unsigned int erofs_hotfile_rank(const char *path, bool isdir);

#ifdef __cplusplus
}
#endif

#endif
