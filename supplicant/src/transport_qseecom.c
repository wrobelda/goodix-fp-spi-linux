// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/tee.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEE_IMPL_ID_QSEECOM 5u

struct shared_memory { int id; void *address; size_t size; };
struct registered_service { const struct qs_service *service; struct shared_memory memory; };

static int alloc_shm(int fd, size_t size, struct shared_memory *memory)
{
	struct tee_ioctl_shm_alloc_data data = { .size = size };
	int shmfd = ioctl(fd, TEE_IOC_SHM_ALLOC, &data);
	if (shmfd < 0) return -1;
	memory->address = mmap(NULL, data.size, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	close(shmfd);
	if (memory->address == MAP_FAILED) return -1;
	memory->id = data.id; memory->size = data.size; memset(memory->address, 0, data.size);
	return 0;
}

static int open_qseecom_priv(void)
{
	char path[32]; struct tee_ioctl_version_data version; int i, fd;
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/teepriv%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		if (!ioctl(fd, TEE_IOC_VERSION, &version) && version.impl_id == TEE_IMPL_ID_QSEECOM)
			return fd;
		close(fd);
	}
	errno = ENODEV; return -1;
}

static int register_service(int fd, struct registered_service *registered)
{
	struct { struct tee_ioctl_open_session_arg arg; struct tee_ioctl_param params[2]; } request = {};
	struct tee_ioctl_buf_data data;
	if (alloc_shm(fd, registered->service->buffer_size, &registered->memory)) return -1;
	request.arg.num_params = 2;
	request.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	request.params[0].a = registered->service->id;
	request.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	request.params[1].b = registered->memory.size;
	request.params[1].c = registered->memory.id;
	data.buf_ptr = (uintptr_t)&request; data.buf_len = sizeof(request);
	return ioctl(fd, TEE_IOC_OPEN_SESSION, &data);
}

static int serve_qseecom(struct qs_store *store, const struct qs_service *services,
			 size_t count, volatile sig_atomic_t *stop)
{
	struct registered_service registered[16] = {}; size_t i; int fd, rc = -1;
	if (count > 16) { errno = E2BIG; return -1; }
	fd = open_qseecom_priv(); if (fd < 0) return -1;
	for (i = 0; i < count; i++) {
		registered[i].service = &services[i];
		if (register_service(fd, &registered[i])) goto out;
		fprintf(stderr, "event=listener_registered transport=qseecom id=%u size=%zu\n",
			services[i].id, services[i].buffer_size);
	}
	while (!*stop) {
		struct { struct tee_iocl_supp_recv_arg arg; struct tee_ioctl_param params[2]; } recv = {};
		struct { struct tee_iocl_supp_send_arg arg; struct tee_ioctl_param params[1]; } send = {};
		struct tee_ioctl_buf_data data; struct registered_service *selected = NULL;
		recv.arg.num_params = 2; data.buf_ptr = (uintptr_t)&recv; data.buf_len = sizeof(recv);
		if (ioctl(fd, TEE_IOC_SUPPL_RECV, &data)) { if (errno == EINTR && *stop) { rc = 0; break; } goto out; }
		for (i = 0; i < count; i++) if (services[i].id == recv.arg.func) selected = &registered[i];
		send.arg.ret = 1; send.arg.num_params = 1;
		if (selected && !selected->service->dispatch(store, selected->memory.address,
							 selected->memory.size)) send.arg.ret = 0;
		send.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
		send.params[0].a = recv.params[0].a;
		data.buf_ptr = (uintptr_t)&send; data.buf_len = sizeof(send);
		if (ioctl(fd, TEE_IOC_SUPPL_SEND, &data)) goto out;
	}
out:
	for (i = 0; i < count; i++) if (services[i].reset)
		services[i].reset();
	for (i = 0; i < count; i++) if (registered[i].memory.address)
		munmap(registered[i].memory.address, registered[i].memory.size);
	close(fd); return rc;
}

const struct qs_transport qs_qseecom_transport = { .name = "qseecom", .serve = serve_qseecom };
