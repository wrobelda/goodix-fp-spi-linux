// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void p32(void *p, uint32_t v) { memcpy(p, &v, 4); }

int main(void)
{
	char root[] = "/tmp/qsee-supp-test.XXXXXX", out[512], file[600];
	struct qs_store store = { .root_fd = -1 };
	uint8_t *b = calloc(1, QS_GPFS_BUFFER_SIZE);
	struct stat st;
	int handle;
	assert(mkdtemp(root));
	assert(!qs_store_open(&store, root, 0600, 0700));
	assert(!qs_normalize_path("/data/vendor/fpdump/a.so", 256, out, sizeof(out)));
	assert(!strcmp(out, "data/vendor/fpdump/a.so"));
	errno = 0; assert(qs_normalize_path("../../etc/shadow", 256, out, sizeof(out)) < 0 && errno == EPERM);

	/* Listener 10's observed open/write/seek/read/close and errno protocol. */
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x202);
	strcpy((char *)b + 4, "/persist/data/fingerprint_version");
	p32(b + 260, O_RDWR | O_CREAT);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE));
	handle = (int)u32(b + 4); assert(handle >= 3);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x208); p32(b + 4, (uint32_t)handle);
	memcpy(b + 8, "v1", 2); p32(b + 20008, 2);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == 2);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x20a); p32(b + 4, (uint32_t)handle);
	p32(b + 8, 0); p32(b + 12, SEEK_SET);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == 0);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x207); p32(b + 4, (uint32_t)handle); p32(b + 8, 2);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE));
	assert(!memcmp(b + 4, "v1", 2) && u32(b + 20004) == 2);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x209); p32(b + 4, (uint32_t)handle);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == 0);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x202); strcpy((char *)b + 4, "/missing");
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == UINT32_MAX);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x21c);
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == ENOENT);
	memset(b, 0, QS_FS_BUFFER_SIZE); p32(b, 0x202); strcpy((char *)b + 4, "../escape");
	assert(!qs_fs_dispatch(&store, b, QS_FS_BUFFER_SIZE)); assert(u32(b + 4) == UINT32_MAX);

	p32(b, 1); strcpy((char *)b + 4, "/data/vendor/fpdump/a.so");
	p32(b + 0x104, 0); p32(b + 0x108, 5); memcpy(b + 0x110, "first", 5);
	assert(!qs_gpfs_dispatch(&store, b, QS_GPFS_BUFFER_SIZE)); assert(u32(b + 4) == 0 && u32(b + 8) == 5);
	memset(b, 0, QS_GPFS_BUFFER_SIZE); p32(b, 1); strcpy((char *)b + 4, "/data/vendor/fpdump/a.so");
	p32(b + 0x108, 6); p32(b + 0x10c, 1); memcpy(b + 0x110, "second", 6);
	assert(!qs_gpfs_dispatch(&store, b, QS_GPFS_BUFFER_SIZE)); assert(u32(b + 4) == 0);
	snprintf(file, sizeof(file), "%s/data/vendor/fpdump/a.so", root); assert(!stat(file, &st) && st.st_size == 6 && (st.st_mode & 0777) == 0600);
	snprintf(file, sizeof(file), "%s/data/vendor/fpdump/a.so.bak", root); assert(!stat(file, &st) && st.st_size == 5);

	memset(b, 0, QS_GPFS_BUFFER_SIZE); p32(b, 3);
	strcpy((char *)b + 4, "/data/vendor/fpdump/a.so");
	strcpy((char *)b + 0x104, "/data/vendor/fpdump/moved.so");
	assert(!qs_gpfs_dispatch(&store, b, QS_GPFS_BUFFER_SIZE)); assert(u32(b + 4) == 0);
	snprintf(file, sizeof(file), "%s/data/vendor/fpdump/moved.so", root);
	assert(!stat(file, &st) && st.st_size == 6);

	memset(b, 0, QS_GPFS_BUFFER_SIZE); p32(b, 1); strcpy((char *)b + 4, "../escape");
	p32(b + 0x108, 1); b[0x110] = 1; assert(!qs_gpfs_dispatch(&store, b, QS_GPFS_BUFFER_SIZE)); assert(u32(b + 4) == EPERM);
	snprintf(file, sizeof(file), "%s/evil", root); assert(!symlink("/tmp", file));
	memset(b, 0, QS_GPFS_BUFFER_SIZE); p32(b, 1); strcpy((char *)b + 4, "evil/escaped");
	p32(b + 0x108, 1); b[0x110] = 1; assert(!qs_gpfs_dispatch(&store, b, QS_GPFS_BUFFER_SIZE)); assert(u32(b + 4) != 0);
	qs_store_close(&store); free(b); puts("protocol tests: OK"); return 0;
}
