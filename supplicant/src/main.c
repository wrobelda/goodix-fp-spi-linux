// SPDX-License-Identifier: GPL-2.0-only
#include "qsee_supplicant.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_handler(int signal_number) { (void)signal_number; stopping = 1; }

int main(int argc, char **argv)
{
	const char *state = "/var/lib/qsee-supplicant"; struct qs_store store = { .root_fd = -1 };
	unsigned int delay = 1;
	if (argc == 3 && !strcmp(argv[1], "--state-dir")) state = argv[2];
	else if (argc != 1) { fprintf(stderr, "usage: %s [--state-dir PATH]\n", argv[0]); return 2; }
	umask(0077);
	if (qs_store_open(&store, state, 0600, 0700)) { perror("state directory"); return 1; }
	{
		struct sigaction action = { .sa_handler = stop_handler };
		sigemptyset(&action.sa_mask);
		/* No SA_RESTART: a signal must interrupt the blocking receive ioctl. */
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
	}
	while (!stopping) {
		fprintf(stderr, "event=transport_start transport=%s state=%s\n", qs_qseecom_transport.name, state);
		if (!qs_qseecom_transport.serve(&store, qs_builtin_services,
					 qs_builtin_service_count, &stopping)) break;
		fprintf(stderr, "event=transport_error transport=%s errno=%d message=\"%s\" retry=%u\n",
			qs_qseecom_transport.name, errno, strerror(errno), delay);
		for (unsigned int i = 0; i < delay && !stopping; i++) sleep(1);
		if (delay < 30) delay *= 2;
	}
	qs_store_close(&store);
	fprintf(stderr, "event=shutdown\n"); return 0;
}
