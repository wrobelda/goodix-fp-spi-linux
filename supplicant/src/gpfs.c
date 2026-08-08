// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define GP_NAME_OFF 4u
#define GP_NAME_SIZE 256u
#define GP_OFFSET_OFF 0x104u
#define GP_LENGTH_OFF 0x108u
#define GP_BACKUP_OFF 0x10cu
#define GP_DATA_OFF 0x110u
#define GP_REPLY_DATA_OFF 12u
#define GP_READ_MAX 512000u

static pthread_mutex_t gpfs_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t value;
	memcpy(&value, p, sizeof(value));
	return value;
}

static void put_u32(uint8_t *p, uint32_t value)
{
	memcpy(p, &value, sizeof(value));
}

static int open_beneath(int dirfd, const char *path, int flags, mode_t mode)
{
	struct open_how how = {
		.flags = (uint64_t)flags | O_CLOEXEC,
		.mode = mode,
		.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
			   RESOLVE_NO_SYMLINKS,
	};
	return syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}

static int parent_and_leaf(struct qs_store *store, const char *path,
			   bool create, int *parent, char *leaf, size_t leaf_size)
{
	char work[512], *save = NULL, *part, *next;
	int current = -1, child;

	if (strlen(path) >= sizeof(work)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(work, path);
	current = dup(store->root_fd);
	if (current < 0)
		return -1;
	part = strtok_r(work, "/", &save);
	if (!part)
		goto bad;
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
	*parent = current;
	return 0;
bad:
	if (current >= 0)
		close(current);
	return -1;
}

static int copy_file_at(int dirfd, const char *source, const char *dest,
			mode_t mode)
{
	char tmp[320];
	uint8_t buffer[16384];
	ssize_t got, done;
	int in = -1, out = -1, rc = -1;

	in = openat(dirfd, source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (in < 0)
		return errno == ENOENT ? 0 : -1;
	if (snprintf(tmp, sizeof(tmp), ".%s.bak.tmp.%ld", source,
		     (long)getpid()) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	out = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		     mode);
	if (out < 0)
		goto out;
	while ((got = read(in, buffer, sizeof(buffer))) > 0) {
		for (done = 0; done < got;) {
			ssize_t n = write(out, buffer + done, (size_t)(got - done));
			if (n < 0)
				goto out;
			done += n;
		}
	}
	if (got < 0 || fsync(out))
		goto out;
	if (renameat(dirfd, tmp, dirfd, dest))
		goto out;
	if (fsync(dirfd))
		goto out;
	rc = 0;
out:
	if (rc && out >= 0)
		unlinkat(dirfd, tmp, 0);
	if (out >= 0)
		close(out);
	if (in >= 0)
		close(in);
	return rc;
}

static int atomic_write(struct qs_store *store, const char *path,
			const void *data, size_t length, bool backup)
{
	char leaf[256], tmp[320] = "", bak[320];
	int parent = -1, fd = -1, rc = -1;
	size_t done = 0;

	if (parent_and_leaf(store, path, true, &parent, leaf, sizeof(leaf)))
		return -1;
	if (backup) {
		if (snprintf(bak, sizeof(bak), "%s.bak", leaf) >= (int)sizeof(bak)) {
			errno = ENAMETOOLONG;
			goto out;
		}
		if (copy_file_at(parent, leaf, bak, store->file_mode))
			goto out;
	}
	if (snprintf(tmp, sizeof(tmp), ".%s.tmp.%ld", leaf,
		     (long)getpid()) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	unlinkat(parent, tmp, 0);
	fd = openat(parent, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		    store->file_mode);
	if (fd < 0)
		goto out;
	while (done < length) {
		ssize_t n = write(fd, (const uint8_t *)data + done, length - done);
		if (n < 0)
			goto out;
		done += (size_t)n;
	}
	if (fsync(fd) || renameat(parent, tmp, parent, leaf) || fsync(parent))
		goto out;
	rc = 0;
out:
	if (rc && parent >= 0 && tmp[0])
		unlinkat(parent, tmp, 0);
	if (fd >= 0)
		close(fd);
	if (parent >= 0)
		close(parent);
	return rc;
}

static void gp_reply(uint8_t *buf, uint32_t op, int error, uint32_t count)
{
	put_u32(buf, op);
	put_u32(buf + 4, (uint32_t)error);
	put_u32(buf + 8, count);
}

int qs_gpfs_dispatch(struct qs_store *store, void *buffer, size_t size)
{
	uint8_t *buf = buffer;
	char path[512], path2[512];
	uint32_t op, length;
	int32_t offset;
	int fd = -1, parent = -1, rc = 0, saved = 0;
	ssize_t n;
	char leaf[256], leaf2[256];

	if (!store || !buf || size < GP_DATA_OFF)
		return -EINVAL;
	op = get_u32(buf);
	if (op > 12) {
		gp_reply(buf, op, EINVAL, 0);
		return 0;
	}
	if (op == 12) {
		gp_reply(buf, op, 2, 0);
		return 0;
	}
	if (qs_normalize_path((char *)buf + GP_NAME_OFF, GP_NAME_SIZE,
			      path, sizeof(path))) {
		gp_reply(buf, op, errno, 0);
		return 0;
	}
	offset = 0;
	length = 0;
	if (op % 4 == 0 || op % 4 == 1) {
		offset = (int32_t)get_u32(buf + GP_OFFSET_OFF);
		length = get_u32(buf + GP_LENGTH_OFF);
		if (offset < 0 || length > GP_READ_MAX ||
		    (op % 4 == 1 && (size_t)length > size - GP_DATA_OFF)) {
			gp_reply(buf, op, EINVAL, 0);
			return 0;
		}
	}
	pthread_mutex_lock(&gpfs_lock);
	switch (op % 4) {
	case 0:
		fd = open_beneath(store->root_fd, path, O_RDONLY, 0);
		if (fd < 0) {
			saved = errno;
			break;
		}
		n = pread(fd, buf + GP_REPLY_DATA_OFF, length, offset);
		if (n < 0)
			saved = errno;
		else
			length = (uint32_t)n;
		break;
	case 1:
		if (offset == 0) {
			rc = atomic_write(store, path, buf + GP_DATA_OFF, length,
					  get_u32(buf + GP_BACKUP_OFF) != 0);
		} else {
			fd = open_beneath(store->root_fd, path, O_WRONLY, 0);
			if (fd >= 0) {
				n = pwrite(fd, buf + GP_DATA_OFF, length, offset);
				rc = n == (ssize_t)length && !fsync(fd) ? 0 : -1;
			} else {
				rc = -1;
			}
		}
		if (rc)
			saved = errno ? errno : EIO;
		break;
	case 2:
		if (parent_and_leaf(store, path, false, &parent, leaf, sizeof(leaf))) {
			saved = errno;
			break;
		}
		if (unlinkat(parent, leaf, 0) && unlinkat(parent, leaf, AT_REMOVEDIR))
			saved = errno;
		else if (fsync(parent))
			saved = errno;
		break;
	case 3:
		if (qs_normalize_path((char *)buf + GP_OFFSET_OFF, GP_NAME_SIZE,
				      path2, sizeof(path2))) {
			saved = errno;
			break;
		}
		if (parent_and_leaf(store, path, false, &parent, leaf, sizeof(leaf)) ||
		    parent_and_leaf(store, path2, true, &fd, leaf2, sizeof(leaf2)) ||
		    renameat(parent, leaf, fd, leaf2))
			saved = errno;
		else if (fsync(parent) || fsync(fd))
			saved = errno;
		break;
	}
	if (fd >= 0)
		close(fd);
	if (parent >= 0)
		close(parent);
	pthread_mutex_unlock(&gpfs_lock);
	gp_reply(buf, op, saved, saved ? 0 : length);
	return 0;
}
