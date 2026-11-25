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

struct erofs_hotfile_entry {
	char *path;
	unsigned int rank;
};

static struct erofs_hotfile_entry *hotfile_paths;
static unsigned int hotfile_nr_paths, hotfile_cap_paths;
static struct erofs_hotfile_entry *hotfile_input;
static unsigned int hotfile_nr_input, hotfile_cap_input;
static const char *hotfile_root;
static bool hotfile_prepared, hotfile_has_entries;
static struct erofs_inode *hotfile_tree;
static struct erofs_inode **hotfile_staged;
static size_t hotfile_nr_staged, hotfile_cap_staged;

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
	free(hotfile_staged);
	hotfile_staged = NULL;
	hotfile_nr_staged = hotfile_cap_staged = 0;
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

static int erofs_hotfile_add(struct erofs_hotfile_entry **paths,
			     unsigned int *nr, unsigned int *cap,
			     char *path, unsigned int rank);

static int erofs_hotfile_record(const char *path, struct erofs_inode *inode,
				unsigned int rank)
{
	char *dup;
	int err;

	if (hotfile_tree) {
		if (hotfile_nr_staged == hotfile_cap_staged) {
			struct erofs_inode **inodes;
			size_t cap;

			if (hotfile_cap_staged > SIZE_MAX / 2 / sizeof(*inodes))
				return -ENOMEM;
			cap = hotfile_cap_staged ? hotfile_cap_staged * 2 : 64;
			inodes = realloc(hotfile_staged, cap * sizeof(*inodes));
			if (!inodes)
				return -ENOMEM;
			hotfile_staged = inodes;
			hotfile_cap_staged = cap;
		}
		hotfile_staged[hotfile_nr_staged++] = inode;
		return 0;
	}
	if (!path[0])
		return 0;
	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = erofs_hotfile_add(&hotfile_paths, &hotfile_nr_paths,
				&hotfile_cap_paths, dup, rank);
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
static int erofs_hotfile_lookup(const char *path, mode_t *mode, char **target,
				struct erofs_inode **inode_out)
{
	erofs_os_stat_t st;
	char *full;
	int err;

	*mode = 0;
	*target = NULL;
	*inode_out = NULL;
	if (hotfile_tree) {
		struct erofs_inode *inode = erofs_hotfile_lookup_tree(path);

		if (IS_ERR(inode))
			return PTR_ERR(inode);
		*inode_out = inode;
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
	if (asprintf(&full, "%s/%s", hotfile_root, path) < 0)
		return -ENOMEM;
	if (erofs_os_lstat(full, &st)) {
		err = -errno;
		free(full);
		return err;
	}
	*mode = st.st_mode;
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
static int erofs_hotfile_resolve(const char *path, unsigned int rank)
{
	char *pending = strdup(path), *prefix = strdup("");
	unsigned int links = 0;
	size_t i;
	int err = 0;

	hotfile_nr_staged = 0;
	if (!pending || !prefix) {
		err = -ENOMEM;
		goto out;
	}
	while (*pending) {
		char *part = pending, *end, *suffix, *next, *target;
		struct erofs_inode *inode;
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
		err = erofs_hotfile_lookup(prefix, &mode, &target, &inode);
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
			err = erofs_hotfile_record(prefix, inode, rank);
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
		err = erofs_hotfile_record(prefix, inode, rank);
		if (err) {
			free(suffix);
			goto out;
		}
		free(pending);
		pending = suffix;
	}
	for (i = 0; i < hotfile_nr_staged; ++i) {
		hotfile_staged[i]->hot_rank =
			min(hotfile_staged[i]->hot_rank, rank);
		hotfile_has_entries = true;
	}
out:
	free(prefix);
	free(pending);
	return err;
}

static int erofs_hotfile_add_resolved_path(const char *path, unsigned int rank)
{
	unsigned int nr_paths = hotfile_nr_paths;
	int err = erofs_hotfile_resolve(path, rank);

	if (err) {
		while (hotfile_nr_paths > nr_paths)
			free(hotfile_paths[--hotfile_nr_paths].path);
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
		    (size_t)*cap > SIZE_MAX / 2 / sizeof(*npaths))
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

static void erofs_hotfile_sort(void)
{
	unsigned int i, out = 0;

	if (!hotfile_nr_paths)
		return;

	qsort(hotfile_paths, hotfile_nr_paths, sizeof(*hotfile_paths),
	      erofs_hotfile_cmp);
	for (i = 0; i < hotfile_nr_paths; ++i) {
		if (out && !strcmp(hotfile_paths[out - 1].path,
				   hotfile_paths[i].path)) {
			if (hotfile_paths[i].rank < hotfile_paths[out - 1].rank) {
				free(hotfile_paths[out - 1].path);
				hotfile_paths[out - 1] = hotfile_paths[i];
			} else {
				free(hotfile_paths[i].path);
			}
			continue;
		}
		hotfile_paths[out++] = hotfile_paths[i];
	}
	hotfile_nr_paths = out;
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
	if (err) {
		erofs_hotfile_exit();
		return err;
	}
	erofs_hotfile_sort();
	hotfile_prepared = hotfile_root != NULL;
	hotfile_has_entries = hotfile_nr_paths != 0;
	erofs_info("loaded %u hot paths from %s", nr, path);
	return 0;
}

bool erofs_hotfile_enabled(void)
{
	return hotfile_prepared ? hotfile_has_entries : hotfile_nr_input != 0;
}

static unsigned int erofs_hotfile_find_rank(const char *path)
{
	const struct erofs_hotfile_entry *found;

	found = hotfile_nr_paths ?
		bsearch(path, hotfile_paths, hotfile_nr_paths,
			sizeof(*hotfile_paths), erofs_hotfile_path_cmp) : NULL;
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

/* Return the hot-file-list position of @path or EROFS_HOT_RANK_NONE */
unsigned int erofs_hotfile_rank(const char *path)
{
	char *normalized;
	unsigned int rank;

	if (!hotfile_nr_paths)
		return EROFS_HOT_RANK_NONE;
	path = erofs_fspath(path);
	if (erofs_hotfile_path_normalized(path))
		return erofs_hotfile_find_rank(path);
	normalized = erofs_hotfile_normalize(path);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;
	rank = erofs_hotfile_find_rank(normalized);
	free(normalized);
	return rank;
}

static void erofs_hotfile_clear_paths(void)
{
	unsigned int i;

	for (i = 0; i < hotfile_nr_paths; ++i)
		free(hotfile_paths[i].path);
	free(hotfile_paths);
	hotfile_paths = NULL;
	hotfile_nr_paths = hotfile_cap_paths = 0;
}

static int erofs_hotfile_index_tree(struct erofs_inode *inode)
{
	struct erofs_dentry *d;
	int err;

	inode->hot_rank = EROFS_HOT_RANK_NONE;
	if (!S_ISDIR(inode->i_mode))
		return 0;
	list_for_each_entry(d, &inode->i_subdirs, d_child) {
		if (is_dot_dotdot(d->name))
			continue;
		err = erofs_hotfile_index_dentry(inode, d);
		if (err)
			return err;
		err = erofs_hotfile_index_tree(d->inode);
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

/*
 * For archive sources, hot paths can only be resolved once the final tree
 * has been imported (later entries may replace or remove earlier ones).
 */
int erofs_hotfile_prepare_tree(struct erofs_inode *root)
{
	unsigned int i;
	int err;

	if (!erofs_hotfile_enabled())
		return 0;
	hotfile_tree = root;
	hotfile_has_entries = false;
	/* Clear shared inodes before aggregating only surviving aliases. */
	err = erofs_hotfile_index_tree(root);
	if (err)
		goto out;
	if (hotfile_nr_dentries)
		qsort(hotfile_dentries, hotfile_nr_dentries,
		      sizeof(*hotfile_dentries), erofs_hotfile_dentry_cmp);
	for (i = 0; i < hotfile_nr_input; ++i) {
		err = erofs_hotfile_add_resolved_path(hotfile_input[i].path,
						      hotfile_input[i].rank);
		if (err)
			goto out;
	}
	erofs_hotfile_clear_input();
	hotfile_prepared = true;
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
	hotfile_has_entries = false;
}
