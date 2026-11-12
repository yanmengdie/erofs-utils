// SPDX-License-Identifier: GPL-2.0+ OR MIT
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "erofs/config.h"
#include "erofs/hotfile.h"
#include "erofs/inode.h"
#include "erofs/print.h"

struct erofs_hotfile_manifest {
	struct erofs_hotfile_entry {
		char *path;
		unsigned int rank;
	} *file_paths;
	unsigned int nr_files;
	unsigned int cap_files;
	struct erofs_hotfile_entry *dir_paths;
	unsigned int nr_dirs;
	unsigned int cap_dirs;
};

static struct erofs_hotfile_manifest hotfile_manifest;
static struct erofs_hotfile_entry *hotfile_input;
static unsigned int hotfile_nr_input, hotfile_cap_input;
static const char *hotfile_root;
static bool hotfile_prepared;
static struct erofs_inode *hotfile_tree;

/*
 * Cache successful local directory lookups only while reading the hot list.
 * Regular files and symlinks are still checked normally.  A bounded cache
 * avoids building an extra tree for a potentially small hot list.
 */
#define EROFS_HOTFILE_DIR_CACHE_SIZE 256
static struct erofs_hotfile_cached_dir {
	char *path;
	mode_t mode;
} *hotfile_dir_cache;

static void erofs_hotfile_drop_dir_cache(void)
{
	unsigned int i;

	if (!hotfile_dir_cache)
		return;
	for (i = 0; i < EROFS_HOTFILE_DIR_CACHE_SIZE; ++i)
		free(hotfile_dir_cache[i].path);
	free(hotfile_dir_cache);
	hotfile_dir_cache = NULL;
}

/* Final-tree lookup only; do not change the imported dentry order. */
static struct erofs_hotfile_dentry {
	struct erofs_inode *parent;
	struct erofs_dentry *dentry;
} *hotfile_dentries;
static size_t hotfile_nr_dentries, hotfile_cap_dentries;

static int erofs_hotfile_dentry_cmp(const void *a, const void *b)
{
	const struct erofs_hotfile_dentry *lhs = a, *rhs = b;
	uintptr_t lp = (uintptr_t)lhs->parent, rp = (uintptr_t)rhs->parent;

	if (lp != rp)
		return lp < rp ? -1 : 1;
	return strcmp(lhs->dentry->name, rhs->dentry->name);
}

struct erofs_hotfile_lookup_key {
	struct erofs_inode *parent;
	const char *name;
	size_t len;
};

static int erofs_hotfile_dentry_key_cmp(const void *a, const void *b)
{
	const struct erofs_hotfile_lookup_key *key = a;
	const struct erofs_hotfile_dentry *entry = b;
	uintptr_t kp = (uintptr_t)key->parent, ep = (uintptr_t)entry->parent;
	size_t len = entry->dentry->namelen;
	int ret;

	if (kp != ep)
		return kp < ep ? -1 : 1;
	ret = memcmp(key->name, entry->dentry->name, min(key->len, len));
	if (ret || key->len == len)
		return ret;
	return key->len < len ? -1 : 1;
}

static int erofs_hotfile_index_dentry(struct erofs_inode *parent,
				    struct erofs_dentry *dentry)
{
	if (hotfile_nr_dentries == hotfile_cap_dentries) {
		struct erofs_hotfile_dentry *entries;
		size_t cap;

		if (hotfile_cap_dentries > SIZE_MAX / 2 / sizeof(*entries))
			return -ENOMEM;
		cap = hotfile_cap_dentries ? hotfile_cap_dentries * 2 : 64;
		entries = realloc(hotfile_dentries, cap * sizeof(*entries));
		if (!entries)
			return -ENOMEM;
		hotfile_dentries = entries;
		hotfile_cap_dentries = cap;
	}
	hotfile_dentries[hotfile_nr_dentries++] =
		(struct erofs_hotfile_dentry){ parent, dentry };
	return 0;
}

static void erofs_hotfile_drop_index(void)
{
	free(hotfile_dentries);
	hotfile_dentries = NULL;
	hotfile_nr_dentries = hotfile_cap_dentries = 0;
}

static int erofs_hotfile_cmp(const void *a, const void *b)
{
	const struct erofs_hotfile_entry *lhs = a;
	const struct erofs_hotfile_entry *rhs = b;

	return strcmp(lhs->path, rhs->path);
}

static int erofs_hotfile_path_cmp(const void *a, const void *b)
{
	const char *lhs = a;
	const struct erofs_hotfile_entry *rhs = b;

	return strcmp(lhs, rhs->path);
}

static char *erofs_hotfile_normalize(const char *path)
{
	const unsigned char *src = (const unsigned char *)path;
	const unsigned char *end;
	char *out;
	size_t len = 0;

	end = src + strlen((const char *)src);

	out = malloc((end - src) + 1);
	if (!out)
		return NULL;

	/* Use the same image-relative spelling for manifests and tar paths. */
	while (src < end) {
		const unsigned char *component;
		size_t n;

		while (src < end && *src == '/')
			++src;
		component = src;
		while (src < end && *src != '/')
			++src;
		n = src - component;
		if (!n || (n == 1 && component[0] == '.'))
			continue;
		if (len)
			out[len++] = '/';
		memcpy(out + len, component, n);
		len += n;
	}
	out[len] = '\0';
	return out;
}

static int erofs_hotfile_add_file(char *path, unsigned int rank);
static int erofs_hotfile_add_dir(char *path, unsigned int rank);

static int erofs_hotfile_add_plain_path(const char *path, bool dir_entry,
					unsigned int rank)
{
	char *dup;
	int err;

	if (!path[0])
		return 0;
	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = dir_entry ? erofs_hotfile_add_dir(dup, rank) :
		erofs_hotfile_add_file(dup, rank);
	if (err)
		free(dup);
	return err;
}

static struct erofs_inode *erofs_hotfile_lookup_tree(const char *path)
{
	struct erofs_inode *inode = hotfile_tree;
	const char *p = path;

	while (*p) {
		const char *end = strchr(p, '/');
		struct erofs_hotfile_lookup_key key;
		struct erofs_hotfile_dentry *found;

		if (!end)
			end = p + strlen(p);
		if (!S_ISDIR(inode->i_mode))
			return ERR_PTR(-ENOTDIR);
		key = (struct erofs_hotfile_lookup_key){ inode, p, end - p };
		found = hotfile_nr_dentries ?
			bsearch(&key, hotfile_dentries, hotfile_nr_dentries,
				sizeof(*found),
				erofs_hotfile_dentry_key_cmp) : NULL;
		if (!found)
			return ERR_PTR(-ENOENT);
		inode = found->dentry->inode;
		p = *end ? end + 1 : end;
	}
	return inode;
}

/* Prefixes passed here contain neither symlinks nor dot components. */
static int erofs_hotfile_lookup(const char *path, mode_t *mode, char **target)
{
	struct erofs_hotfile_cached_dir *cached = NULL;
	erofs_os_stat_t st;
	char *full;
	int err;

	*mode = 0;
	*target = NULL;
	if (hotfile_tree) {
		struct erofs_inode *inode = erofs_hotfile_lookup_tree(path);

		if (IS_ERR(inode))
			return PTR_ERR(inode);
		*mode = inode->i_mode;
		if (S_ISLNK(*mode)) {
			if (!inode->i_link)
				return -EOPNOTSUPP;
			*target = strdup(inode->i_link);
			if (!*target)
				return -ENOMEM;
		}
		return 0;
	}
	if (hotfile_dir_cache) {
		u32 hash = erofs_crc32c(~0U, (const u8 *)path, strlen(path));

		hash %= EROFS_HOTFILE_DIR_CACHE_SIZE;
		cached = &hotfile_dir_cache[hash];
		if (cached->path && !strcmp(cached->path, path)) {
			*mode = cached->mode;
			return 0;
		}
	}
	if (asprintf(&full, "%s/%s", hotfile_root, path) < 0)
		return -ENOMEM;
	if (erofs_os_lstat(full, &st)) {
		err = -errno;
		free(full);
		return err;
	}
	*mode = st.st_mode;
	if (cached && S_ISDIR(*mode)) {
		char *dup = strdup(path);

		/* failing to cache must not fail the lookup itself */
		if (dup) {
			free(cached->path);
			cached->path = dup;
			cached->mode = *mode;
		}
	}
	if (S_ISLNK(*mode)) {
		size_t size = st.st_size + 1;
		ssize_t len;

		*target = malloc(size + 1);
		if (!*target) {
			free(full);
			return -ENOMEM;
		}
		len = erofs_os_readlink(full, *target, size);
		if (len < 0 || (size_t)len == size) {
			err = len < 0 ? -errno : -EAGAIN;
			free(*target);
			*target = NULL;
			free(full);
			return err;
		}
		(*target)[len] = '\0';
	}
	free(full);
	return 0;
}

/* Apply .. only after preceding symlinks, anchored at the image root. */
static int erofs_hotfile_resolve(const char *path, unsigned int rank,
				 char **resolved)
{
	char *pending = strdup(path), *prefix = strdup("");
	unsigned int links = 0;
	int err = 0;

	*resolved = NULL;
	if (!pending || !prefix) {
		err = -ENOMEM;
		goto out;
	}
	while (*pending) {
		char *part = pending, *end, *suffix, *next, *target;
		mode_t mode;
		size_t parentlen = strlen(prefix);
		bool has_slash;

		while (*part == '/')
			++part;
		if (!*part)
			break;
		end = strchr(part, '/');
		if (!end)
			end = part + strlen(part);
		has_slash = *end != '\0';
		suffix = strdup(*end ? end + 1 : end);
		if (!suffix) {
			err = -ENOMEM;
			goto out;
		}
		*end = '\0';
		if (!strcmp(part, ".") || !strcmp(part, "..")) {
			/* ".." of the image root is the root itself */
			if (part[1]) {
				char *slash = strrchr(prefix, '/');

				prefix[slash ? slash - prefix : 0] = '\0';
			}
			free(pending);
			pending = suffix;
			continue;
		}
		if (asprintf(&next, "%s%s%s", prefix,
			     parentlen ? "/" : "", part) < 0) {
			free(suffix);
			err = -ENOMEM;
			goto out;
		}
		free(prefix);
		prefix = next;
		err = erofs_hotfile_lookup(prefix, &mode, &target);
		if (err) {
			free(suffix);
			goto out;
		}
		if (S_ISLNK(mode)) {
			if (!*target) {
				free(target);
				free(suffix);
				err = -ENOENT;
				goto out;
			}
			err = erofs_hotfile_add_plain_path(prefix, false, rank);
			if (!err && ++links > 40)
				err = -ELOOP;
			if (!err && asprintf(&next, "%s%s%s", target,
					     has_slash ? "/" : "", suffix) < 0)
				err = -ENOMEM;
			prefix[target[0] == '/' ? 0 : parentlen] = '\0';
			free(target);
			free(suffix);
			if (err)
				goto out;
			free(pending);
			pending = next;
			continue;
		}
		/* A trailing slash also requires a directory. */
		if (has_slash && !S_ISDIR(mode)) {
			free(suffix);
			err = -ENOTDIR;
			goto out;
		}
		if (!S_ISREG(mode) && !S_ISDIR(mode)) {
			free(suffix);
			err = -ENOENT;
			goto out;
		}
		if (S_ISDIR(mode)) {
			err = erofs_hotfile_add_plain_path(prefix, true, rank);
			if (err) {
				free(suffix);
				goto out;
			}
		}
		free(pending);
		pending = suffix;
	}
	*resolved = prefix;
	prefix = NULL;
out:
	free(prefix);
	free(pending);
	return err;
}

static int erofs_hotfile_add_resolved_path(const char *path, unsigned int rank)
{
	char *resolved;
	size_t len = strlen(path);
	unsigned int nr_files = hotfile_manifest.nr_files;
	unsigned int nr_dirs = hotfile_manifest.nr_dirs;
	int err = erofs_hotfile_resolve(path, rank, &resolved);

	if (!err) {
		err = erofs_hotfile_add_plain_path(resolved,
					len && path[len - 1] == '/', rank);
		free(resolved);
	} else {
		struct erofs_hotfile_manifest *m = &hotfile_manifest;

		while (m->nr_files > nr_files)
			free(m->file_paths[--m->nr_files].path);
		while (m->nr_dirs > nr_dirs)
			free(m->dir_paths[--m->nr_dirs].path);
		/* a stale or broken entry should not fail the whole build */
		if (err != -ENOENT && err != -ENOTDIR && err != -ELOOP)
			return err;
		erofs_warn("ignoring hot path %s: %s", path, strerror(-err));
		err = 0;
	}
	return err;
}

static int erofs_hotfile_add(struct erofs_hotfile_entry **paths,
			     unsigned int *nr, unsigned int *cap,
			     char *path, unsigned int rank)
{
	struct erofs_hotfile_entry *npaths;

	if (*nr >= *cap) {
		unsigned int ncap;

		if (*cap > UINT_MAX / 2 ||
		    *cap > SIZE_MAX / 2 / sizeof(*npaths))
			return -ENOMEM;
		ncap = *cap ? *cap * 2 : 64;
		npaths = realloc(*paths, ncap * sizeof(*npaths));
		if (!npaths)
			return -ENOMEM;
		*paths = npaths;
		*cap = ncap;
	}
	(*paths)[*nr].path = path;
	(*paths)[*nr].rank = rank;
	++(*nr);
	return 0;
}

static int erofs_hotfile_add_file(char *path, unsigned int rank)
{
	return erofs_hotfile_add(&hotfile_manifest.file_paths,
				 &hotfile_manifest.nr_files,
				 &hotfile_manifest.cap_files,
				 path, rank);
}

static int erofs_hotfile_add_dir(char *path, unsigned int rank)
{
	return erofs_hotfile_add(&hotfile_manifest.dir_paths,
				 &hotfile_manifest.nr_dirs,
				 &hotfile_manifest.cap_dirs,
				 path, rank);
}

static unsigned int erofs_hotfile_sort_dedupe(struct erofs_hotfile_entry *paths,
					      unsigned int nr)
{
	unsigned int i, out = 0;

	if (!nr)
		return 0;

	qsort(paths, nr, sizeof(paths[0]), erofs_hotfile_cmp);
	for (i = 0; i < nr; ++i) {
		if (out && !strcmp(paths[out - 1].path, paths[i].path)) {
			if (paths[i].rank < paths[out - 1].rank) {
				free(paths[out - 1].path);
				paths[out - 1] = paths[i];
			} else {
				free(paths[i].path);
			}
			continue;
		}
		paths[out++] = paths[i];
	}
	return out;
}

static void erofs_hotfile_sort(void)
{
	hotfile_manifest.nr_files = erofs_hotfile_sort_dedupe(
		hotfile_manifest.file_paths, hotfile_manifest.nr_files);
	hotfile_manifest.nr_dirs = erofs_hotfile_sort_dedupe(
		hotfile_manifest.dir_paths, hotfile_manifest.nr_dirs);
}

int erofs_hotfile_load(const char *path, const char *root)
{
	FILE *fp;
	char *line = NULL;
	size_t linesz = 0;
	unsigned int nr = 0;
	ssize_t nread;
	int err = 0;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;
	hotfile_root = root;
	if (hotfile_root)
		hotfile_dir_cache = calloc(EROFS_HOTFILE_DIR_CACHE_SIZE,
					   sizeof(*hotfile_dir_cache));
	while ((nread = getline(&line, &linesz, fp)) >= 0) {
		char *input;

		if (memchr(line, '\0', nread)) {
			err = -EINVAL;
			break;
		}
		if (nread && line[nread - 1] == '\n')
			line[--nread] = '\0';
		if (nread && line[nread - 1] == '\r')
			line[--nread] = '\0';
		/* a leading # starts a comment, use ./#name for such files */
		if (!nread || line[0] == '#')
			continue;
		if (hotfile_root) {
			err = erofs_hotfile_add_resolved_path(line, nr);
		} else {
			input = strdup(line);
			if (!input) {
				err = -ENOMEM;
				break;
			}
			err = erofs_hotfile_add(&hotfile_input,
						&hotfile_nr_input,
						&hotfile_cap_input, input, nr);
			if (err)
				free(input);
		}
		++nr;
		if (err)
			break;
	}
	if (!err && ferror(fp))
		err = -EIO;
	free(line);
	fclose(fp);
	erofs_hotfile_drop_dir_cache();
	if (err) {
		erofs_hotfile_exit();
		return err;
	}
	erofs_hotfile_sort();
	hotfile_prepared = hotfile_root != NULL;
	erofs_info("loaded %u hot paths from %s", nr, path);
	return 0;
}

bool erofs_hotfile_enabled(void)
{
	return hotfile_prepared ?
		hotfile_manifest.nr_files || hotfile_manifest.nr_dirs :
		hotfile_nr_input != 0;
}

static unsigned int erofs_hotfile_find_rank(const char *path,
				  const struct erofs_hotfile_entry *paths,
				  unsigned int nr)
{
	const struct erofs_hotfile_entry *found;

	found = nr ? bsearch(path, paths, nr, sizeof(*paths),
			     erofs_hotfile_path_cmp) : NULL;
	return found ? found->rank : EROFS_HOT_RANK_NONE;
}

static bool erofs_hotfile_path_normalized(const char *path)
{
	while (*path) {
		size_t len = strcspn(path, "/");

		if (!len || (len == 1 && path[0] == '.'))
			return false;
		path += len;
		if (*path && !*++path)
			return false;
	}
	return true;
}

static unsigned int erofs_hotfile_get_rank(const char *path,
				 const struct erofs_hotfile_entry *paths,
				 unsigned int nr)
{
	char *normalized;
	unsigned int rank;

	if (!nr || !erofs_hotfile_enabled())
		return EROFS_HOT_RANK_NONE;
	path = erofs_fspath(path);
	if (erofs_hotfile_path_normalized(path))
		return erofs_hotfile_find_rank(path, paths, nr);
	normalized = erofs_hotfile_normalize(path);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;
	rank = erofs_hotfile_find_rank(normalized, paths, nr);
	free(normalized);
	return rank;
}

/* Return the hot-file-list position of @path or EROFS_HOT_RANK_NONE */
unsigned int erofs_hotfile_rank(const char *path, bool isdir)
{
	struct erofs_hotfile_manifest *m = &hotfile_manifest;
	unsigned int rank;

	rank = erofs_hotfile_get_rank(path, m->file_paths, m->nr_files);
	if (isdir)
		rank = min(rank, erofs_hotfile_get_rank(path, m->dir_paths,
							m->nr_dirs));
	return rank;
}

static void erofs_hotfile_clear_paths(void)
{
	unsigned int i;

	for (i = 0; i < hotfile_manifest.nr_files; ++i)
		free(hotfile_manifest.file_paths[i].path);
	for (i = 0; i < hotfile_manifest.nr_dirs; ++i)
		free(hotfile_manifest.dir_paths[i].path);
	free(hotfile_manifest.file_paths);
	free(hotfile_manifest.dir_paths);
	hotfile_manifest.file_paths = NULL;
	hotfile_manifest.nr_files = 0;
	hotfile_manifest.cap_files = 0;
	hotfile_manifest.dir_paths = NULL;
	hotfile_manifest.nr_dirs = 0;
	hotfile_manifest.cap_dirs = 0;
}

static int erofs_hotfile_walk_tree(struct erofs_inode *inode, const char *path,
				  bool clear)
{
	struct erofs_dentry *d;
	unsigned int rank;

	if (clear) {
		inode->hot_rank = EROFS_HOT_RANK_NONE;
	} else {
		/* hardlinks take the earliest rank of all surviving names */
		rank = erofs_hotfile_rank(path, S_ISDIR(inode->i_mode));
		inode->hot_rank = min(inode->hot_rank, rank);
	}
	if (!S_ISDIR(inode->i_mode))
		return 0;
	list_for_each_entry(d, &inode->i_subdirs, d_child) {
		char *child = NULL;
		int err;

		if (is_dot_dotdot(d->name))
			continue;
		if (clear) {
			err = erofs_hotfile_index_dentry(inode, d);
			if (err)
				return err;
		}
		if (!clear && asprintf(&child, "%s%s%s", path,
				       *path ? "/" : "", d->name) < 0)
			return -ENOMEM;
		err = erofs_hotfile_walk_tree(d->inode, child, clear);
		free(child);
		if (err)
			return err;
	}
	return 0;
}

static void erofs_hotfile_clear_input(void)
{
	unsigned int i;

	for (i = 0; i < hotfile_nr_input; ++i)
		free(hotfile_input[i].path);
	free(hotfile_input);
	hotfile_input = NULL;
	hotfile_nr_input = hotfile_cap_input = 0;
}

static int erofs_hotfile_prepare_paths(struct erofs_inode *root)
{
	unsigned int i;
	int err;

	erofs_hotfile_clear_paths();
	for (i = 0; i < hotfile_nr_input; ++i) {
		err = erofs_hotfile_add_resolved_path(hotfile_input[i].path,
						    hotfile_input[i].rank);
		if (err)
			return err;
	}
	erofs_hotfile_sort();
	erofs_hotfile_clear_input();
	hotfile_prepared = true;
	return erofs_hotfile_walk_tree(root, "", false);
}

/*
 * For archive sources, hot paths can only be resolved once the final tree
 * has been imported (later entries may replace or remove earlier ones).
 */
int erofs_hotfile_prepare_tree(struct erofs_inode *root)
{
	int err;

	if (!erofs_hotfile_enabled())
		return 0;
	hotfile_tree = root;
	/* Clear shared inodes before aggregating only surviving aliases. */
	err = erofs_hotfile_walk_tree(root, NULL, true);
	if (err)
		goto out;
	if (hotfile_nr_dentries)
		qsort(hotfile_dentries, hotfile_nr_dentries,
		      sizeof(*hotfile_dentries), erofs_hotfile_dentry_cmp);
	err = erofs_hotfile_prepare_paths(root);
out:
	erofs_hotfile_drop_index();
	hotfile_tree = NULL;
	return err;
}

void erofs_hotfile_exit(void)
{
	erofs_hotfile_clear_paths();
	erofs_hotfile_clear_input();
	hotfile_root = NULL;
	hotfile_tree = NULL;
	hotfile_prepared = false;
}
