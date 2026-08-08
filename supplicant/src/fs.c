// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FS_PATH 4u
#define FS_PATH_SIZE 256u
#define FS_ARG 8u
#define FS_WRITE_COUNT 20008u
#define FS_REPLY_NREAD 20004u
#define FS_REPLY_MAX 20000u

struct fs_handle {
	int protocol_fd;
	int host_fd;
	struct fs_handle *next;
};

static struct fs_handle *handles;
static int next_handle = 3;
static int last_error;

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int reply(uint8_t *b, uint32_t op, int32_t result)
{
	put32(b, op); put32(b + 4, (uint32_t)result); return 0;
}

static int fail(uint8_t *b, uint32_t op)
{
	last_error = errno; return reply(b, op, -1);
}

static struct fs_handle *lookup(int id)
{
	struct fs_handle *h;
	for (h = handles; h; h = h->next) if (h->protocol_fd == id) return h;
	errno = EBADF; return NULL;
}

static int add_handle(int fd)
{
	struct fs_handle *h = calloc(1, sizeof(*h));
	if (!h) return -1;
	h->protocol_fd = next_handle++; h->host_fd = fd; h->next = handles; handles = h;
	return h->protocol_fd;
}

static int close_handle(int id)
{
	struct fs_handle **p = &handles, *h;
	while ((h = *p)) {
		if (h->protocol_fd == id) { *p = h->next; close(h->host_fd); free(h); return 0; }
		p = &h->next;
	}
	errno = EBADF; return -1;
}

void qs_fs_reset(void)
{
	while (handles) {
		struct fs_handle *next = handles->next;
		close(handles->host_fd);
		free(handles);
		handles = next;
	}
	next_handle = 3;
	last_error = 0;
}

static int normalize(uint8_t *b, char *path, size_t size)
{
	return qs_normalize_path((char *)b + FS_PATH, FS_PATH_SIZE, path, size);
}

int qs_fs_dispatch(struct qs_store *s, void *buffer, size_t size)
{
	uint8_t *b = buffer; uint32_t op, count, flags; int id, fd, rc; ssize_t n;
	char path[512], path2[512], leaf[256], leaf2[256]; int parent = -1, parent2 = -1; struct fs_handle *h;
	if (!s || !b || size < QS_FS_BUFFER_SIZE) return -EINVAL;
	op = get32(b);
	switch (op) {
	case 0x202: /* open */
		if (normalize(b, path, sizeof(path))) return fail(b, op);
		flags = get32(b + 260);
		if (qs_open_parent(s, path, (flags & O_CREAT) != 0, &parent, leaf, sizeof(leaf))) return fail(b, op);
		fd = openat(parent, leaf, (int)flags | O_CLOEXEC | O_NOFOLLOW,
			    s->file_mode);
		close(parent); parent = -1;
		if (fd < 0) return fail(b, op);
		id = add_handle(fd); if (id < 0) { close(fd); return fail(b, op); }
		return reply(b, op, id);
	case 0x206: /* creat */
		if (normalize(b, path, sizeof(path)) || qs_open_parent(s, path, true, &parent, leaf, sizeof(leaf))) return fail(b, op);
		fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
			    get32(b + 260) & s->file_mode);
		close(parent); parent = -1;
		if (fd < 0) return fail(b, op);
		id = add_handle(fd); if (id < 0) { close(fd); return fail(b, op); }
		return reply(b, op, id);
	case 0x207: /* read */
		h = lookup((int)get32(b + 4)); if (!h) return fail(b, op);
		count = get32(b + FS_ARG); if (count > FS_REPLY_MAX) count = FS_REPLY_MAX;
		n = read(h->host_fd, b + 4, count); if (n < 0) last_error = errno;
		put32(b, op); put32(b + FS_REPLY_NREAD, (uint32_t)n); return 0;
	case 0x208: /* write */
		h = lookup((int)get32(b + 4)); if (!h) return fail(b, op);
		count = get32(b + FS_WRITE_COUNT); if (count > FS_REPLY_MAX) { errno = EINVAL; return fail(b, op); }
		n = write(h->host_fd, b + FS_ARG, count); if (n < 0) return fail(b, op);
		return reply(b, op, (int32_t)n);
	case 0x209: return close_handle((int)get32(b + 4)) ? fail(b, op) : reply(b, op, 0);
	case 0x20a:
		h = lookup((int)get32(b + 4)); if (!h) return fail(b, op);
		n = lseek(h->host_fd, (int32_t)get32(b + 8), (int)get32(b + 12));
		return n < 0 ? fail(b, op) : reply(b, op, (int32_t)n);
	case 0x20c: case 0x213:
		if (normalize(b, path, sizeof(path))) return fail(b, op);
		if (qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf))) return fail(b, op);
		rc = unlinkat(parent, leaf, 0); close(parent); return rc ? fail(b, op) : reply(b, op, 0);
	case 0x20d:
		if (normalize(b, path, sizeof(path))) return fail(b, op);
		if (qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf))) return fail(b, op);
		rc = unlinkat(parent, leaf, AT_REMOVEDIR); close(parent); return rc ? fail(b, op) : reply(b, op, 0);
	case 0x210:
		if (normalize(b, path, sizeof(path)) || qs_open_parent(s, path, true, &parent, leaf, sizeof(leaf))) return fail(b, op);
		rc = mkdirat(parent, leaf, get32(b + 260) & s->dir_mode); close(parent);
		if (rc && errno == EEXIST) rc = 0;
		return rc ? fail(b, op) : reply(b, op, 0);
	case 0x216:
		h = lookup((int)get32(b + 4)); if (!h) return fail(b, op);
		return fsync(h->host_fd) ? fail(b, op) : reply(b, op, 0);
	case 0x217:
		if (normalize(b, path, sizeof(path)) ||
		    qs_normalize_path((char *)b + 260, FS_PATH_SIZE, path2, sizeof(path2)) ||
		    qs_open_parent(s, path, false, &parent, leaf, sizeof(leaf)) ||
		    qs_open_parent(s, path2, true, &parent2, leaf2, sizeof(leaf2))) return fail(b, op);
		rc = renameat(parent, leaf, parent2, leaf2); close(parent); close(parent2);
		return rc ? fail(b, op) : reply(b, op, 0);
	case 0x21c: return reply(b, op, last_error);
	case 0x21d: return reply(b, op, 0);
	default: errno = ENOSYS; return fail(b, op);
	}
}
