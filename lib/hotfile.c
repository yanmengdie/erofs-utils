// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
#define _GNU_SOURCE

#include <ctype.h>
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
	struct erofs_hotfile_entry *directories;
	unsigned int nr_directories, cap_directories;
	struct erofs_hotfile_entry *dir_paths;
	unsigned int nr_dirs;
	unsigned int cap_dirs;
};

static struct erofs_hotfile_manifest hotfile_manifest;
static struct erofs_hotfile_entry *hotfile_input;
static unsigned int hotfile_nr_input, hotfile_cap_input;
static const char *hotfile_root;
static bool hotfile_prepared;
static bool hotfile_oci;
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


static int erofs_hotfile_add_file_path(const char *path, unsigned int rank)
{
	char *dup;
	int err;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = erofs_hotfile_add_file(dup, rank);
	if (err) {
		free(dup);
		return err;
	}
	return 0;
}

static int erofs_hotfile_add_dir_path(const char *path, unsigned int rank)
{
	char *dup;
	int err;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = erofs_hotfile_add_dir(dup, rank);
	if (err) {
		free(dup);
		return err;
	}
	return 0;
}

static int erofs_hotfile_add_plain_path(const char *path, bool dir_entry,
					unsigned int rank)
{
	if (!path[0])
		return 0;
	return dir_entry ? erofs_hotfile_add_dir_path(path, rank) :
		erofs_hotfile_add_file_path(path, rank);
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
		if (!hotfile_nr_dentries)
			return ERR_PTR(-ENOENT);
		key = (struct erofs_hotfile_lookup_key){ inode, p, end - p };
		found = bsearch(&key, hotfile_dentries, hotfile_nr_dentries,
				sizeof(*found), erofs_hotfile_dentry_key_cmp);
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

		cached = &hotfile_dir_cache[hash % EROFS_HOTFILE_DIR_CACHE_SIZE];
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

		/* Cache allocation failure need not fail a successful lookup. */
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
		if (len < 0 || len == size) {
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
				bool collect, char **resolved)
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
			if (part[1]) {
				char *slash = strrchr(prefix, '/');

				if (!*prefix && !hotfile_oci) {
					free(suffix);
					err = -EXDEV;
					goto out;
				}
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
			if (err == -ENOENT && hotfile_oci &&
			    asprintf(resolved, "%s%s%s", prefix,
				     *suffix ? "/" : "", suffix) < 0) {
				*resolved = NULL;
				err = -ENOMEM;
			}
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
			if (collect)
				err = erofs_hotfile_add_file_path(prefix, rank);
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
		if (collect && S_ISDIR(mode)) {
			err = erofs_hotfile_add_dir_path(prefix, rank);
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
	int err = erofs_hotfile_resolve(path, rank, true, &resolved);

	if (!err) {
		err = erofs_hotfile_add_plain_path(resolved,
					len && path[len - 1] == '/', rank);
		free(resolved);
	} else {
		while (hotfile_manifest.nr_files > nr_files)
			free(hotfile_manifest.file_paths[--hotfile_manifest.nr_files].path);
		while (hotfile_manifest.nr_dirs > nr_dirs)
			free(hotfile_manifest.dir_paths[--hotfile_manifest.nr_dirs].path);
		if (err != -ENOENT && err != -ENOTDIR)
			return err;
		erofs_warn("cannot resolve hot path %s: %s", path, strerror(-err));
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
		unsigned int ncap = *cap ? *cap * 2 : 64;

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

int erofs_hotfile_load(const char *path)
{
	FILE *fp;
	char *line = NULL;
	size_t linesz = 0;
	ssize_t nread;
	erofs_os_stat_t st;
	int err = 0;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;
	if (cfg.c_src_path && !erofs_os_stat(cfg.c_src_path, &st) && S_ISDIR(st.st_mode))
		hotfile_root = cfg.c_src_path;
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
		/* ./#name denotes a filename; only a literal leading # comments. */
		if (!nread || line[0] == '#')
			continue;
		input = strdup(line);
		if (!input) {
			err = -ENOMEM;
			break;
		}
		err = erofs_hotfile_add(&hotfile_input, &hotfile_nr_input,
				&hotfile_cap_input, input, hotfile_nr_input);
		if (err) {
			free(input);
			break;
		}
		if (hotfile_root)
			err = erofs_hotfile_add_resolved_path(input, hotfile_nr_input - 1);
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
	erofs_info("loaded %u hot paths from %s", hotfile_nr_input, path);
	return 0;
}

bool erofs_hotfile_enabled(void)
{
	return hotfile_prepared ?
		hotfile_manifest.nr_files || hotfile_manifest.nr_dirs :
		hotfile_nr_input != 0;
}





unsigned int erofs_get_hot_file_rank(const char *path)
{
	char *normalized;
	struct erofs_hotfile_entry *found;
	const char *fspath;
	unsigned int rank = EROFS_HOT_RANK_NONE;

	if (!erofs_hotfile_enabled() || !hotfile_manifest.nr_files)
		return EROFS_HOT_RANK_NONE;

	fspath = erofs_fspath(path);
	normalized = erofs_hotfile_normalize(fspath);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;

	found = bsearch(normalized, hotfile_manifest.file_paths,
			hotfile_manifest.nr_files,
			sizeof(hotfile_manifest.file_paths[0]),
			erofs_hotfile_path_cmp);
	if (found)
		rank = found->rank;

	free(normalized);
	return rank;
}

unsigned int erofs_get_hot_traversal_rank(const char *path)
{
	char *normalized;
	struct erofs_hotfile_entry *found;
	const char *fspath;

	if (!erofs_hotfile_enabled() || !hotfile_manifest.nr_dirs)
		return EROFS_HOT_RANK_NONE;

	fspath = erofs_fspath(path);
	normalized = erofs_hotfile_normalize(fspath);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;

	found = bsearch(normalized, hotfile_manifest.dir_paths,
			hotfile_manifest.nr_dirs,
			sizeof(hotfile_manifest.dir_paths[0]),
			erofs_hotfile_path_cmp);
	free(normalized);
	return found ? found->rank : EROFS_HOT_RANK_NONE;
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

unsigned int erofs_get_hot_symlink_rank(const char *path)
{
	char *resolved;
	unsigned int rank = erofs_get_hot_file_rank(path);
	int err;

	if (!erofs_hotfile_enabled() || (!hotfile_root && !hotfile_tree) || !rank)
		return rank;
	err = erofs_hotfile_resolve(erofs_fspath(path), 0, false, &resolved);
	if (!err) {
		struct erofs_hotfile_entry *found;

		found = hotfile_manifest.nr_files ?
			bsearch(resolved, hotfile_manifest.file_paths,
				hotfile_manifest.nr_files, sizeof(*found),
				erofs_hotfile_path_cmp) : NULL;
		if (found && found->rank < rank)
			rank = found->rank;
		found = hotfile_manifest.nr_dirs ?
			bsearch(resolved, hotfile_manifest.dir_paths,
				hotfile_manifest.nr_dirs, sizeof(*found),
				erofs_hotfile_path_cmp) : NULL;
		if (found && found->rank < rank)
			rank = found->rank;
		free(resolved);
	}
	return rank;
}

static int erofs_hotfile_walk_tree(struct erofs_inode *inode, const char *path,
				  bool clear)
{
	struct erofs_dentry *d;
	unsigned int rank;

	if (clear) {
		inode->hot_rank = EROFS_HOT_RANK_NONE;
		inode->hotfile = inode->hotdir = false;
	} else {
		rank = S_ISLNK(inode->i_mode) ? erofs_get_hot_symlink_rank(path) :
			erofs_get_hot_file_rank(path);
		if (rank < inode->hot_rank) {
			inode->hot_rank = rank;
			inode->hotfile = true;
		}
		if (S_ISDIR(inode->i_mode)) {
			rank = erofs_get_hot_traversal_rank(path);
			if (rank < inode->hot_rank)
				inode->hot_rank = rank;
			inode->hotdir = inode->hot_rank != EROFS_HOT_RANK_NONE;
			inode->hotfile = false;
		}
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
	hotfile_prepared = true;
	return erofs_hotfile_walk_tree(root, "", false);
}

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

static char *erofs_hotfile_python_source(const char *path)
{
	char *py, *base, *parent, *tag;
	size_t len = strlen(path);

	if (len < 4 || strcmp(path + len - 4, ".pyc"))
		return NULL;
	py = strdup(path);
	if (!py)
		return ERR_PTR(-ENOMEM);
	py[len - 4] = '\0';
	base = strrchr(py, '/');
	base = base ? base + 1 : py;
	parent = base == py ? NULL : base - 1;
	if (parent && parent - py >= 11 &&
	    !strncmp(parent - 11, "__pycache__", 11) &&
	    (parent - py == 11 || parent[-12] == '/')) {
		char *source;

		tag = strstr(base, ".opt-");
		if (tag)
			*tag = '\0';
		tag = strrchr(base, '.');
		if (tag)
			*tag = '\0';
		if (asprintf(&source, "%.*s/%s.py",
			     (int)(parent - py - 11), py, base) < 0)
			source = ERR_PTR(-ENOMEM);
		free(py);
		return source;
	}
	/* The original allocation also has room to replace .pyc with .py. */
	strcat(py, ".py");
	return py;
}

static int erofs_hotfile_rank_cmp(const void *a, const void *b)
{
	const struct erofs_hotfile_entry *lhs = a, *rhs = b;

	return lhs->rank < rhs->rank ? -1 : lhs->rank > rhs->rank;
}

int erofs_hotfile_add_directory(const char *path)
{
	char *copy = strdup(path);
	int err;

	if (!copy)
		return -ENOMEM;
	err = erofs_hotfile_add(&hotfile_manifest.directories,
		&hotfile_manifest.nr_directories,
		&hotfile_manifest.cap_directories, copy, 0);
	if (err)
		free(copy);
	return err;
}

static int erofs_hotfile_name_cmp(const void *a, const void *b)
{
	const struct erofs_dentry *const *lhs = a, *const *rhs = b;

	return strcmp((*lhs)->name, (*rhs)->name);
}

static int erofs_hotfile_expand_directory(struct erofs_inode *inode,
					const char *path, unsigned int *rank)
{
	struct erofs_dentry *d, **entries;
	unsigned int count = 0, i;
	int err = 0;

	if (!S_ISDIR(inode->i_mode)) {
		char *copy = strdup(path);

		if (!copy)
			return -ENOMEM;
		err = erofs_hotfile_add(&hotfile_input, &hotfile_nr_input,
			&hotfile_cap_input, copy, (*rank)++);
		if (err)
			free(copy);
		return err;
	}
	list_for_each_entry(d, &inode->i_subdirs, d_child)
		if (!is_dot_dotdot(d->name) && d->type != EROFS_FT_UNKNOWN)
			++count;
	if (!count)
		return 0;
	entries = malloc(count * sizeof(*entries));
	if (!entries)
		return -ENOMEM;
	i = 0;
	list_for_each_entry(d, &inode->i_subdirs, d_child)
		if (!is_dot_dotdot(d->name) && d->type != EROFS_FT_UNKNOWN)
			entries[i++] = d;
	qsort(entries, count, sizeof(*entries), erofs_hotfile_name_cmp);
	for (i = 0; i < count && !err; ++i) {
		char *child;

		if (asprintf(&child, "%s%s%s", path, *path ? "/" : "",
			     entries[i]->name) < 0) {
			err = -ENOMEM;
			break;
		}
		err = erofs_hotfile_expand_directory(entries[i]->inode, child, rank);
		free(child);
	}
	free(entries);
	return err;
}

int erofs_hotfile_write_list(const char *path)
{
	FILE *fp = fopen(path, "w");
	unsigned int i;
	int err = 0;

	if (!fp)
		return -errno;
	for (i = 0; i < hotfile_nr_input; ++i)
		if (fprintf(fp, "/%s\n", hotfile_input[i].path) < 0) {
			err = -EIO;
			break;
		}
	if (fclose(fp) && !err)
		err = -errno;
	return err;
}

static struct erofs_inode *erofs_hotfile_resolve_inode(const char *name,
						     char **path)
{
	int err = erofs_hotfile_resolve(name, 0, false, path);

	return err ? ERR_PTR(err) : erofs_hotfile_lookup_tree(*path);
}

int erofs_hotfile_resolve_oci(struct erofs_inode *root)
{
	struct erofs_hotfile_entry *raw;
	unsigned int i, nr_raw, rank = hotfile_nr_input;
	int err;

	hotfile_tree = root;
	hotfile_oci = true;
	err = erofs_hotfile_walk_tree(root, NULL, true);
	if (err)
		goto out;
	if (hotfile_nr_dentries)
		qsort(hotfile_dentries, hotfile_nr_dentries,
		      sizeof(*hotfile_dentries), erofs_hotfile_dentry_cmp);
	for (i = 0; i < hotfile_manifest.nr_directories; ++i) {
		char *path;
		struct erofs_inode *inode = erofs_hotfile_resolve_inode(
			hotfile_manifest.directories[i].path, &path);

		if (!IS_ERR(inode))
			err = erofs_hotfile_expand_directory(inode, path, &rank);
		else if (PTR_ERR(inode) == -ENOMEM)
			err = -ENOMEM;
		free(path);
		if (err)
			goto out;
	}
	raw = hotfile_input;
	nr_raw = hotfile_nr_input;
	hotfile_input = NULL;
	hotfile_nr_input = hotfile_cap_input = 0;
	for (i = 0; i < nr_raw && !err; ++i) {
		char *path, *py;
		struct erofs_inode *inode =
			erofs_hotfile_resolve_inode(raw[i].path, &path);

		if (IS_ERR(inode) && (PTR_ERR(inode) != -ENOENT || !path)) {
			if (PTR_ERR(inode) == -ENOMEM)
				err = -ENOMEM;
			else
				erofs_warn("skip hotlist path %s: %s", raw[i].path,
					   erofs_strerror(PTR_ERR(inode)));
			free(path);
			continue;
		}
		py = (!IS_ERR(inode) && !S_ISREG(inode->i_mode)) ? NULL :
			erofs_hotfile_python_source(path);
		if (!IS_ERR(inode) && S_ISREG(inode->i_mode)) {
			err = erofs_hotfile_add(&hotfile_input, &hotfile_nr_input,
				&hotfile_cap_input, path, hotfile_nr_input);
			if (!err)
				path = NULL;
		}
		free(path);
		if (IS_ERR(py)) {
			err = PTR_ERR(py);
		} else if (py && !err) {
			inode = erofs_hotfile_resolve_inode(py, &path);
			if (!IS_ERR(inode) && S_ISREG(inode->i_mode)) {
				err = erofs_hotfile_add(&hotfile_input, &hotfile_nr_input,
					&hotfile_cap_input, path, hotfile_nr_input);
				if (!err)
					path = NULL;
			} else if (IS_ERR(inode) && PTR_ERR(inode) == -ENOMEM) {
				err = -ENOMEM;
			}
			free(path);
		}
		if (!IS_ERR(py))
			free(py);
	}
	for (i = 0; i < nr_raw; ++i)
		free(raw[i].path);
	free(raw);
	hotfile_nr_input = erofs_hotfile_sort_dedupe(hotfile_input, hotfile_nr_input);
	if (hotfile_nr_input)
		qsort(hotfile_input, hotfile_nr_input, sizeof(*hotfile_input),
		      erofs_hotfile_rank_cmp);
	if (!err)
		err = erofs_hotfile_prepare_paths(root);
out:
	erofs_hotfile_drop_index();
	hotfile_tree = NULL;
	hotfile_oci = false;
	return err;
}

void erofs_hotfile_exit(void)
{
	unsigned int i;

	erofs_hotfile_clear_paths();
	for (i = 0; i < hotfile_manifest.nr_directories; ++i)
		free(hotfile_manifest.directories[i].path);
	free(hotfile_manifest.directories);
	hotfile_manifest = (struct erofs_hotfile_manifest){};
	for (i = 0; i < hotfile_nr_input; ++i)
		free(hotfile_input[i].path);
	free(hotfile_input);
	hotfile_input = NULL;
	hotfile_nr_input = hotfile_cap_input = 0;
	hotfile_root = NULL;
	hotfile_tree = NULL;
	hotfile_prepared = false;
}
