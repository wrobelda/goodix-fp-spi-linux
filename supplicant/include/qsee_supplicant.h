// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define QS_FS_SERVICE_ID 10u
#define QS_GPFS_SERVICE_ID 0x7000u
#define QS_FS_BUFFER_SIZE (20u * 1024u)
#define QS_GPFS_BUFFER_SIZE (504u * 1024u)

struct qs_store {
	int root_fd;
	uint32_t file_mode;
	uint32_t dir_mode;
};

struct qs_service {
	uint32_t id;
	size_t buffer_size;
	int (*dispatch)(struct qs_store *store, void *buffer, size_t size);
	void (*reset)(void);
};

int qs_store_open(struct qs_store *store, const char *path,
		  uint32_t file_mode, uint32_t dir_mode);
void qs_store_close(struct qs_store *store);

/* Convert a protocol path to a normalized path relative to store->root_fd. */
int qs_normalize_path(const char *input, size_t input_size,
		      char *output, size_t output_size);
int qs_open_parent(struct qs_store *store, const char *path, bool create,
		   int *parent_fd, char *leaf, size_t leaf_size);

int qs_fs_dispatch(struct qs_store *store, void *buffer, size_t size);
void qs_fs_reset(void);
int qs_gpfs_dispatch(struct qs_store *store, void *buffer, size_t size);

extern const struct qs_service qs_builtin_services[];
extern const size_t qs_builtin_service_count;

struct qs_transport {
	const char *name;
	int (*serve)(struct qs_store *store, const struct qs_service *services,
		     size_t service_count, volatile sig_atomic_t *stop);
};

extern const struct qs_transport qs_qseecom_transport;
