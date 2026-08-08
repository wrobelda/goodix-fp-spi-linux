// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int qs_store_open(struct qs_store *store, const char *path,
		  uint32_t file_mode, uint32_t dir_mode)
{
	int fd;

	if (!store || !path || !*path) {
		errno = EINVAL;
		return -1;
	}
	if (mkdir(path, dir_mode) && errno != EEXIST)
		return -1;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fchmod(fd, dir_mode)) {
		close(fd);
		return -1;
	}
	store->root_fd = fd;
	store->file_mode = file_mode;
	store->dir_mode = dir_mode;
	return 0;
}

void qs_store_close(struct qs_store *store)
{
	if (store && store->root_fd >= 0) {
		close(store->root_fd);
		store->root_fd = -1;
	}
}

int qs_normalize_path(const char *input, size_t input_size,
		      char *output, size_t output_size)
{
	size_t in = 0, out = 0, component = 0;

	if (!input || !output || output_size < 2 ||
	    !memchr(input, '\0', input_size)) {
		errno = EINVAL;
		return -1;
	}
	while (in < input_size && input[in] == '/')
		in++;
	if (!input[in]) {
		errno = EINVAL;
		return -1;
	}
	while (input[in]) {
		unsigned char c = (unsigned char)input[in++];

		if (c == '/') {
			if (!component)
				continue;
			if ((component == 1 && output[out - 1] == '.') ||
			    (component == 2 && output[out - 2] == '.' &&
			     output[out - 1] == '.')) {
				errno = EPERM;
				return -1;
			}
			if (out + 1 >= output_size) {
				errno = ENAMETOOLONG;
				return -1;
			}
			output[out++] = '/';
			component = 0;
			continue;
		}
		if (c < 0x20 || c == 0x7f) {
			errno = EINVAL;
			return -1;
		}
		if (out + 1 >= output_size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		output[out++] = (char)c;
		component++;
	}
	if (!component || (component == 1 && output[out - 1] == '.') ||
	    (component == 2 && output[out - 2] == '.' && output[out - 1] == '.')) {
		errno = EPERM;
		return -1;
	}
	output[out] = '\0';
	return 0;
}

int qs_open_parent(struct qs_store *store, const char *path, bool create,
		   int *parent_fd, char *leaf, size_t leaf_size)
{
	char work[512], *save = NULL, *part, *next;
	int current, child;

	if (!store || !path || strlen(path) >= sizeof(work)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(work, path);
	current = dup(store->root_fd);
	if (current < 0)
		return -1;
	part = strtok_r(work, "/", &save);
	if (!part) {
		errno = EINVAL;
		goto bad;
	}
	for (;;) {
		next = strtok_r(NULL, "/", &save);
		if (!next)
			break;
		child = openat(current, part,
			       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (child < 0 && create && errno == ENOENT) {
			if (mkdirat(current, part, store->dir_mode) && errno != EEXIST)
				goto bad;
			child = openat(current, part,
				       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		}
		if (child < 0)
			goto bad;
		close(current);
		current = child;
		part = next;
	}
	if (strlen(part) + 1 > leaf_size) {
		errno = ENAMETOOLONG;
		goto bad;
	}
	strcpy(leaf, part);
	*parent_fd = current;
	return 0;
bad:
	close(current);
	return -1;
}
