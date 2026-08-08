// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/tee.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEE_IMPL_ID_QSEECOM 5u
#define APP_NAME_MAX 64u

static volatile sig_atomic_t stopping;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static int open_qseecom_priv(void)
{
	struct tee_ioctl_version_data version;
	char path[32];
	int fd;

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/teepriv%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, TEE_IOC_VERSION, &version) &&
		    version.impl_id == TEE_IMPL_ID_QSEECOM)
			return fd;
		close(fd);
	}
	errno = ENODEV;
	return -1;
}

int main(int argc, char **argv)
{
	struct tee_ioctl_shm_alloc_data allocation = { .size = APP_NAME_MAX };
	struct {
		struct tee_ioctl_open_session_arg arg;
		struct tee_ioctl_param params[1];
	} session = {};
	struct tee_ioctl_buf_data data;
	struct sigaction action = { .sa_handler = stop_handler };
	void *name_memory;
	int shm_fd;
	int tee_fd;

	if (argc != 2 || !argv[1][0] || strlen(argv[1]) >= APP_NAME_MAX ||
	    strchr(argv[1], '/')) {
		fprintf(stderr, "usage: %s APPLICATION\n", argv[0]);
		return 2;
	}

	tee_fd = open_qseecom_priv();
	if (tee_fd < 0) {
		fprintf(stderr, "event=loader_error app=%s stage=open errno=%d message=\"%s\"\n",
			argv[1], errno, strerror(errno));
		return 1;
	}
	shm_fd = ioctl(tee_fd, TEE_IOC_SHM_ALLOC, &allocation);
	if (shm_fd < 0) {
		fprintf(stderr, "event=loader_error app=%s stage=allocate errno=%d message=\"%s\"\n",
			argv[1], errno, strerror(errno));
		close(tee_fd);
		return 1;
	}
	name_memory = mmap(NULL, allocation.size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (name_memory == MAP_FAILED) {
		fprintf(stderr, "event=loader_error app=%s stage=map errno=%d message=\"%s\"\n",
			argv[1], errno, strerror(errno));
		close(tee_fd);
		return 1;
	}
	memset(name_memory, 0, allocation.size);
	memcpy(name_memory, argv[1], strlen(argv[1]));

	session.arg.num_params = 1;
	session.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	session.params[0].b = strlen(argv[1]) + 1;
	session.params[0].c = allocation.id;
	data.buf_ptr = (uintptr_t)&session;
	data.buf_len = sizeof(session);
	if (ioctl(tee_fd, TEE_IOC_OPEN_SESSION, &data)) {
		fprintf(stderr, "event=loader_error app=%s stage=load errno=%d message=\"%s\"\n",
			argv[1], errno, strerror(errno));
		munmap(name_memory, allocation.size);
		close(tee_fd);
		return 1;
	}

	fprintf(stderr, "event=application_loaded app=%s session=%u\n",
		argv[1], session.arg.session);
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	while (!stopping)
		pause();
	fprintf(stderr, "event=application_unloading app=%s session=%u\n",
		argv[1], session.arg.session);
	munmap(name_memory, allocation.size);
	close(tee_fd);
	return 0;
}
