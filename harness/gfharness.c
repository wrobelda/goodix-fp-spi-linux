// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reference client for a Goodix fingerprint trusted application on Qualcomm
 * QSEE, over the QSEECOM TEE driver.
 *
 * The protocol this speaks is documented in ../docs; this file is the working
 * implementation, not the specification. It is a single-file test client: it
 * blocks, prints to stdout and assumes one device.
 *
 *   gfharness --supp 0 serve            file service, every listener, one
 *                                       process -- start this first
 *   gfharness --load <app> <fwdir>      load the application (holds it open)
 *   gfharness --bringup [--invoke]      DETECT_SENSOR, INIT, INIT_FINISHED
 *   gfharness --capture <secs> --enroll  enrol a finger
 *   gfharness --capture <secs> --auth    match a finger
 *   gfharness --remove <fid>            unenrol
 *   gfharness --enumerate               ask what the application has
 *   gfharness --power <secs>            hold the sensor powered
 *
 * Build: cc -O2 -o gfharness gfharness.c
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <poll.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include <linux/tee.h>

#define SUPP_DEV	"/dev/teepriv0"

/* This driver's TEE_IMPL_ID, which is how its client device is recognised. */
#define TEE_IMPL_ID_QSEECOM	5

/* Shared buffer handed to the listener; the TA reads and writes it in place. */
/*
 * Android registers listener 10 with a 0x5000 buffer. A read is bounded by it,
 * and a template is bigger than a page, so 4096 silently truncates.
 */
#define SUPP_BUF_SIZE		0x5000

#define GF_CMD_GET_DEV_INFO	1089

/*
 * GF_CMD_IRQ is what an event loop polls after the driver reports an
 * interrupt. The payload is enormous -- 320 KiB -- because it doubles as the
 * application's working area; no image data comes back through it.
 *
 * The request has to arm a control block near the end of that payload, at the
 * absolute offsets below, or the application does not treat the interrupt as
 * one worth acting on. A zeroed buffer already satisfies the fields that want
 * zero, so only three have to be written.
 */
#define GF_CMD_IRQ		1016
#define GF_IRQ_PAYLOAD		327624
#define GF_IRQ_CTRL_MODE	327049		/* u32 0x02000000, unaligned */
#define GF_IRQ_CTRL_SIZE	327096		/* u32 512 */
#define GF_IRQ_CTRL_ARM		327616		/* u32 1 */

/* Response fields, from the start of the payload. */
#define GF_IRQ_RSP_MASK		100		/* u32 bitmask, see below */
#define GF_IRQ_RSP_OP		104		/* u32 operation_table entry */
#define GF_IRQ_RSP_NAV		136		/* u32 nav code */

/* Frames to collect before a capture run can stop. */
#define GF_WANT_FRAMES		40

/*
 * Enrolment progress, reported in the IRQ payload alongside each accepted
 * sample. samples_remaining counts down to zero; the ids are what SAVE wants.
 */
#define GF_ENROL_GROUP_OFF	0x4fce4
#define GF_ENROL_FINGER_OFF	0x4fce8
#define GF_ENROL_REMAIN_OFF	0x4fcec

/*
 * Committing the template. gf_hal_save() builds a 112-byte body with the
 * group id at +100 and the finger id the enrolment issued at +104; a zeroed
 * body asks the application to commit finger 0, which does not exist.
 */
#define GF_CMD_POST_ENROLL	1008
#define GF_CMD_SAVE		1012
#define GF_CMD_ENUMERATE	1015
#define GF_CMD_REMOVE		1013
#define GF_CMD_AUTHENTICATE	1010
#define GF_CMD_AUTHENTICATE_FINISH	1086
#define GF_CMD_GET_AUTH_ID	1011
#define GF_CMD_SET_ACTIVE_GROUP	1014
#define GF_SAVE_PAYLOAD		112
#define GF_SET_ACTIVE_GROUP_PAYLOAD	104

/* How many GF_CMD_IRQ commands one interrupt may take before we give up. */
#define GF_IRQ_MAX_DRAIN	12

/* Bits in the interrupt bitmask that matter here. */
#define GF_IRQ_FINGER_DOWN	(1u << 1)
#define GF_IRQ_FINGER_UP	(1u << 2)
#define GF_IRQ_IMAGE		(1u << 7)
#define GF_IRQ_RESET		(1u << 8)
#define GF_IRQ_FRAME_DONE	(1u << 10)
#define GF_IRQ_FDT_REVERSE	(1u << 14)

/*
 * The whole table, recovered from gf_strirq() in libgf_hal.so: it is a flat
 * tbnz chain, one branch per bit, each block loading its own name. Worth
 * having in full because the two bits that matter -- IMAGE and ONE_FRAME_DONE
 * -- are the ones a stalled capture never reports, and telling "nothing
 * happened" from "something unexpected happened" needs every name.
 */
static const char *const gf_irq_names[32] = {
	[1]  = "FINGER_DOWN",		[2]  = "FINGER_UP",
	[3]  = "MENUKEY_DOWN",		[4]  = "MENUKEY_UP",
	[5]  = "BACKKEY_DOWN",		[6]  = "BACKKEY_UP",
	[7]  = "IMAGE",			[8]  = "RESET",
	[9]  = "TMR_IRQ_MNT",		[10] = "ONE_FRAME_DONE",
	[11] = "ESD_IRQ",		[12] = "ADC_FIFO_FULL",
	[13] = "ADC_FIFO_HALF",		[14] = "FDT_REVERSE",
	[15] = "NAV",			[16] = "FARGO_ERR",
	[17] = "FARGO_ERR_NOT_RESOLVED",[18] = "RESET_FIRST",
	[19] = "RESET_FAILED",		[20] = "ESD_ERR",
	[21] = "NAV_LEFT",		[22] = "NAV_RIGHT",
	[23] = "NAV_UP",		[24] = "NAV_DOWN",
	[25] = "PRESS_LIGHT",		[26] = "PRESS_HEAVY",
	[27] = "UPDATE_BASE",		[28] = "TEMPERATURE_CHANGE",
	[31] = "ERR_CHECKSUM",
};

/* The driver's netlink channel towards the HAL. */
#define GF_NETLINK_PROTO	25
#define GF_NET_EVENT_IRQ	1

/*
 * The companion character device owns the sensor's power. It enables the
 * supply on open and drops it on release, and holds the part in reset until
 * GF_IOC_RESET -- so the whole bring-up has to happen under one open, or the
 * sensor is power-cycled between commands and nothing works.
 */
#define GF_DEV			"/dev/goodix_fp"
#define GF_IOC_MAGIC		'g'
#define GF_IOC_RESET		_IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ	_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_ENABLE_SPI_CLK	_IOW(GF_IOC_MAGIC, 5, uint32_t)
#define GF_IOC_ENABLE_POWER	_IO(GF_IOC_MAGIC, 7)

/* Power the sensor up and return the fd, which must stay open. */
static int sensor_power_up(void)
{
	uint32_t clk = 0;
	int fd;

	fd = open(GF_DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", GF_DEV, strerror(errno));
		return -1;
	}

	if (ioctl(fd, GF_IOC_ENABLE_POWER))
		fprintf(stderr, "ENABLE_POWER: %s\n", strerror(errno));

	if (ioctl(fd, GF_IOC_RESET))
		fprintf(stderr, "RESET: %s\n", strerror(errno));

	usleep(20000);

	if (ioctl(fd, GF_IOC_ENABLE_SPI_CLK, &clk))
		fprintf(stderr, "ENABLE_SPI_CLK: %s\n", strerror(errno));

	/*
	 * Give the part time to come out of reset. 20ms is what the HAL uses
	 * between reset and the SPI clock, but the first command after that
	 * came back BAD_PARAMS while the same command sent a few seconds later
	 * succeeded, so wait longer before driving it.
	 */
	usleep(500000);

	printf("sensor powered up (%s held open)\n", GF_DEV);

	return fd;
}

struct shm {
	int id;
	void *va;
	size_t size;
};

static int shm_alloc(int fd, size_t size, struct shm *out)
{
	struct tee_ioctl_shm_alloc_data data = { .size = size };
	int shm_fd;

	shm_fd = ioctl(fd, TEE_IOC_SHM_ALLOC, &data);
	if (shm_fd < 0) {
		fprintf(stderr, "SHM_ALLOC(%zu): %s\n", size, strerror(errno));
		return -1;
	}

	out->va = mmap(NULL, data.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       shm_fd, 0);
	close(shm_fd);
	if (out->va == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return -1;
	}

	out->id = data.id;
	out->size = data.size;
	memset(out->va, 0, data.size);

	return 0;
}

/*
 * Find the client device. Which /dev/teeN this driver gets depends on what
 * else registered first -- it swapped with qcomtee across a reboot -- so ask
 * each one what it is rather than hardcoding a number.
 */
static int open_client(void)
{
	struct tee_ioctl_version_data vers;
	char path[32];
	int i, fd;

	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/tee%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		if (!ioctl(fd, TEE_IOC_VERSION, &vers) &&
		    vers.impl_id == TEE_IMPL_ID_QSEECOM) {
			printf("using %s\n", path);
			return fd;
		}

		close(fd);
	}

	fprintf(stderr, "no QSEECOM TEE device found\n");

	return -1;
}

static int show_version(int fd)
{
	struct tee_ioctl_version_data vers;

	if (ioctl(fd, TEE_IOC_VERSION, &vers)) {
		fprintf(stderr, "VERSION: %s\n", strerror(errno));
		return -1;
	}

	printf("impl_id=%u impl_caps=0x%x gen_caps=0x%x\n",
	       vers.impl_id, vers.impl_caps, vers.gen_caps);

	if (vers.impl_id != TEE_IMPL_ID_QSEECOM)
		printf("  ! expected impl_id 5 (TEE_IMPL_ID_QSEECOM)\n");

	return 0;
}

static int open_session(int fd, const char *app)
{
	struct {
		struct tee_ioctl_open_session_arg arg;
		struct tee_ioctl_param params[1];
	} buf = {};
	struct tee_ioctl_buf_data bd;
	struct shm name;

	if (shm_alloc(fd, 64, &name))
		return -1;

	strncpy(name.va, app, 63);

	/*
	 * Read the name back through our own mapping immediately before the
	 * ioctl. If this prints the name and the driver still reports an empty
	 * one, the two views of the buffer are not coherent.
	 */
	{
		unsigned char *n = name.va;

		printf("name shm id=%d readback: %02x %02x %02x %02x %02x %02x '%s'\n",
		       name.id, n[0], n[1], n[2], n[3], n[4], n[5],
		       (char *)name.va);
	}

	buf.arg.num_params = 1;
	buf.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	buf.params[0].a = 0;
	buf.params[0].b = strlen(app) + 1;
	buf.params[0].c = name.id;

	bd.buf_ptr = (uintptr_t)&buf;
	bd.buf_len = sizeof(buf);

	if (ioctl(fd, TEE_IOC_OPEN_SESSION, &bd)) {
		fprintf(stderr, "OPEN_SESSION('%s'): %s\n", app,
			strerror(errno));
		return -1;
	}

	printf("session=%u (app '%s') ret=0x%x\n", buf.arg.session, app,
	       buf.arg.ret);

	return buf.arg.session;
}

/*
 * The codes the application actually hands back, from its own gf_strerror()
 * table (see ENUMS-GOODIX-TA.md). Only the ones a run here can produce are
 * listed; anything else prints as a bare number.
 */
static const char *gf_strerror(uint32_t err)
{
	static const struct { uint32_t code; const char *name; } tbl[] = {
		{ 0,    "GF_SUCCESS" },
		{ 1001, "GF_ERROR_BAD_PARAMS" },
		{ 1002, "GF_ERROR_NO_MEMORY" },
		{ 1023, "GF_ERROR_SPI_TRANSFER_ERROR" },
		{ 1035, "GF_ERROR_WRITE_SECURE_OBJECT_FAILED" },
		{ 1036, "GF_ERROR_READ_SECURE_OBJECT_FAILED" },
		{ 1047, "GF_ERROR_FINGER_NOT_EXIST" },
		{ 1057, "GF_ERROR_UNTRUSTED_ENROLL" },
	{ 1064, "GF_ERROR_MATCH_FAIL_AND_RETRY" },
	};
	size_t i;

	for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
		if (tbl[i].code == err)
			return tbl[i].name;

	return "";
}

/* Logdump level put in every request header, from GF_LOGDUMP. */
static uint32_t gf_logdump_level;

/* Arm AUTHENTICATE rather than ENROLL for the capture window. */
static int do_auth;

/* Unenrol one finger: --remove <fid>. */
static int do_enumerate;
static int do_remove;
static uint32_t remove_fid;

/*
 * The Goodix request header: token at +8, class at +12, command at +32,
 * payload length at +36. Offset 0 is left alone -- it takes the *physical*
 * address of the payload buffer, which only the kernel can fill in, and which
 * the application dereferences in the secure world without checking it.
 * Sending a request with zero there resets the machine.
 */
static void build_cmd(uint8_t *req, uint32_t cls, uint32_t cmd, uint32_t plen)
{
	static uint32_t token;
	struct timespec ts;
	uint64_t ms = 0;

	/*
	 * Milliseconds since local midnight, not since the epoch. Headers
	 * captured from the vendor CA under Android decode to the time of day of
	 * the command they belonged to; an epoch value is ~1.7e12 and does not
	 * fit the field's observed magnitude.
	 */
	if (!clock_gettime(CLOCK_REALTIME, &ts)) {
		time_t secs = ts.tv_sec;
		struct tm tm;

		if (localtime_r(&secs, &tm))
			ms = ((uint64_t)tm.tm_hour * 3600 + tm.tm_min * 60 +
			      tm.tm_sec) * 1000 + ts.tv_nsec / 1000000;
	}

	memset(req, 0, 128);

	/* +0 is the payload address, left for the kernel to patch in. */
	*(uint32_t *)(req + 8) = ++token;
	*(uint32_t *)(req + 12) = cls;
	*(uint64_t *)(req + 16) = ms;		/* milliseconds since epoch */
	/*
	 * Logdump level. Android sends 0 and the vendor client's log readout is
	 * stubbed -- gf_ca_get_logbuf_length is "mov w0, wzr; ret" -- but the
	 * application itself is full of log code, so it is worth asking whether
	 * a non-zero level makes it say something. GF_LOGDUMP overrides it.
	 */
	*(uint32_t *)(req + 24) = gf_logdump_level;
	*(uint32_t *)(req + 32) = cmd;
	*(uint32_t *)(req + 36) = plen;
}

/*
 * Send one command, with the payload buffer's physical address patched into
 * the request by the kernel: parameters 2 and 3 say "write the address of this
 * buffer, four bytes wide, at offset 0".
 */
/*
 * As invoke(), but the caller supplies the payload to send and gets the
 * payload back. Commands that carry arguments -- ENROLL and its challenge,
 * above all -- need both.
 */
static int invoke_cls(int fd, uint32_t session, uint32_t cls, uint32_t cmd,
		      size_t payload_len, const void *in, void *out);

/*
 * The command class selects which dispatch table inside the application handles
 * the opcode: 1 is the normal path, 2 CA-level operations, 3 factory test, 4
 * image and template dump. Classes 1 and 3 share the GF_CMD_* number space, so
 * the same opcode can mean different things -- GF_CMD_IRQ is reachable from
 * both, and the GF_CMD_TEST_* block answers UNKNOWN_CMD on class 1 because it
 * belongs to 3.
 */
static int invoke_pl(int fd, uint32_t session, uint32_t cmd, size_t payload_len,
		     const void *in, void *out)
{
	return invoke_cls(fd, session, 1, cmd, payload_len, in, out);
}

static int invoke(int fd, uint32_t session, uint32_t cmd, size_t payload_len)
{
	return invoke_pl(fd, session, cmd, payload_len, NULL, NULL);
}

static int invoke_cls(int fd, uint32_t session, uint32_t cls, uint32_t cmd,
		      size_t payload_len, const void *in, void *out)
{
	struct {
		struct tee_ioctl_invoke_arg arg;
		struct tee_ioctl_param params[4];
	} buf = {};
	struct tee_ioctl_buf_data bd;
	struct shm sb, payload;
	uint32_t status;

	/*
	 * Request and response go in *one* buffer, the response following the
	 * 128-byte request header, which is how libgf_ca lays it out. Passing
	 * two separate allocations is what the first attempt did, and the
	 * machine reset.
	 */
	if (shm_alloc(fd, 4096, &sb) || shm_alloc(fd, payload_len, &payload))
		return -1;

	build_cmd(sb.va, cls, cmd, payload_len);

	/*
	 * Poison the response area. The driver copies only the first req_size
	 * bytes of this buffer into TZ memory, so this never reaches the secure
	 * world; it only tells us whether the copy back over it happened. Zeros
	 * afterwards mean the driver did write, and TZ returned zeros. Poison
	 * still there means nothing was written back at all.
	 */
	memset((uint8_t *)sb.va + 128, 0xAA, 64);

	if (in)
		memcpy(payload.va, in, payload_len);

	/*
	 * Nothing is poisoned. It is tempting to fill the response area so an
	 * untouched buffer cannot be misread as "status 0", but the shared
	 * buffer belongs to the protocol, not to us: poisoning the payload fed
	 * DETECT_SENSOR garbage and reset the machine, and poisoning the
	 * response area correlates with the resets that followed. The path
	 * that works leaves the whole buffer zeroed, so do exactly that and
	 * treat an all-zero answer as ambiguous rather than as success.
	 */

	{
		unsigned char *b = sb.va;

		printf("sb id=%d readback: +0=%02x%02x%02x%02x +8=%02x%02x%02x%02x "
		       "+12=%02x%02x%02x%02x +32=%02x%02x%02x%02x +36=%02x%02x%02x%02x\n",
		       sb.id, b[0], b[1], b[2], b[3], b[8], b[9], b[10], b[11],
		       b[12], b[13], b[14], b[15], b[32], b[33], b[34], b[35],
		       b[36], b[37], b[38], b[39]);
	}

	buf.arg.func = 0;
	buf.arg.session = session;
	buf.arg.num_params = 4;

	buf.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	buf.params[0].a = 0;
	buf.params[0].b = 128;
	buf.params[0].c = sb.id;

	buf.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
	buf.params[1].a = 128;
	buf.params[1].b = 64;
	buf.params[1].c = sb.id;

	/* Patch the payload address in at offset 0, four bytes wide. */
	buf.params[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	buf.params[2].a = 0;
	buf.params[2].b = 4;

	buf.params[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	buf.params[3].b = payload_len;
	buf.params[3].c = payload.id;

	bd.buf_ptr = (uintptr_t)&buf;
	bd.buf_len = sizeof(buf);

	if (ioctl(fd, TEE_IOC_INVOKE, &bd)) {
		fprintf(stderr, "INVOKE(cmd %u): %s\n", cmd, strerror(errno));
		return -1;
	}

	status = *(uint32_t *)((uint8_t *)sb.va + 128);
	printf("invoke class %u cmd %u: status=%u %s aux=0x%x%s\n", cls, cmd,
	       status, gf_strerror(status),
	       *(uint32_t *)((uint8_t *)sb.va + 132),
	       (!status && !*(uint32_t *)((uint8_t *)sb.va + 128)) ?
	       "  <-- all zero, may be untouched" : "");

	/*
	 * The status was being read from +128+4 on the assumption that the reply
	 * lands there. A deliberately corrupted challenge, which the application
	 * answers 1056, still read 0 from that offset -- so dump both regions and
	 * find where the answer really goes.
	 */
	{
		unsigned char *r = (unsigned char *)sb.va + 128;
		unsigned char *q = payload.va;
		size_t k;

		printf("  params[1] back: a=%llu b=%llu c=%llu   params[3] back: b=%llu\n",
	       (unsigned long long)buf.params[1].a,
	       (unsigned long long)buf.params[1].b,
	       (unsigned long long)buf.params[1].c,
	       (unsigned long long)buf.params[3].b);
	printf("  arg.ret=0x%x arg.ret_origin=0x%x  buf+128=%u  buf+132=%u\n",
	       buf.arg.ret, buf.arg.ret_origin,
	       *(uint32_t *)((uint8_t *)sb.va + 128),
	       *(uint32_t *)((uint8_t *)sb.va + 132));
	printf("  RSP[0..63]:");
		for (k = 0; k < 64; k++)
			printf("%s%02x", (k % 16) ? " " : "\n    ", r[k]);
		printf("\n  PAYLOAD[0..%zu]:", payload_len > 256 ? 256 : payload_len);
		for (k = 0; k < 256 && k < payload_len; k++)
			printf("%s%02x", (k % 16) ? " " : "\n    ", q[k]);
		printf("\n");
	}

	/* Per-command bodies start at +100, after the opaque TA header. */
	printf("  payload +100=0x%08x +104=0x%08x +108=0x%08x\n",
	       *(uint32_t *)((uint8_t *)payload.va + 100),
	       *(uint32_t *)((uint8_t *)payload.va + 104),
	       *(uint32_t *)((uint8_t *)payload.va + 108));

	if (out)
		memcpy(out, payload.va, payload_len);

	/* Any printable strings the application left behind. */
	if (payload_len > 200) {
		unsigned char *p = payload.va;
		size_t i, run = 0;

		for (i = 100; i < payload_len && i < 800; i++) {
			if (p[i] >= 32 && p[i] < 127) {
				if (!run)
					printf("  +%zu: \"", i);
				run++;
				putchar(p[i]);
			} else if (run) {
				printf("\"\n");
				run = 0;
			}
		}
		if (run)
			printf("\"\n");
	}

	/*
	 * Negative means the ioctl failed and nothing reached the secure world;
	 * otherwise the application's own status comes back, 0 for success. This
	 * used to return 0 whatever the secure world said, so callers written as
	 * "if (invoke_pl(...))" only noticed a failed ioctl and walked straight
	 * past a refusal.
	 */
	return (int)status;
}

/*
 * Leave a marker on disk before each step, flushed all the way down.
 * A secure-world reset takes DRAM with it, so ramoops captures nothing and
 * dmesg is gone; a file on UFS is what survives to say how far we got.
 */
static void mark(const char *fmt, ...)
{
	va_list ap;
	FILE *f;

	f = fopen("/home/cromo/gf-progress.log", "a");
	if (!f)
		return;

	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);

	fputc('\n', f);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

/*
 * Dump a buffer, collapsing runs of identical lines to a single '*'.
 *
 * The request header carries a fixed-size path field, so anything the TA
 * puts after it -- offsets, lengths -- sits several hundred bytes in. Dumping
 * only the head hides exactly the fields worth having.
 */
static void dump_buf(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	int eliding = 0;
	size_t i;

	for (i = 0; i < len; i += 16) {
		unsigned int j;

		if (i && !memcmp(p + i, p + i - 16, 16)) {
			if (!eliding) {
				printf("  *\n");
				eliding = 1;
			}
			continue;
		}
		eliding = 0;

		printf("  %04zx:", i);
		for (j = 0; j < 16; j++)
			printf(" %02x", p[i + j]);
		printf("  |");
		for (j = 0; j < 16; j++)
			printf("%c", (p[i + j] >= 32 && p[i + j] < 127) ?
				     p[i + j] : '.');
		printf("|\n");
	}
}

/*
 * Paths to answer with success rather than failure, matched as a substring.
 *
 * The point is to find out what the application does next. Every request so
 * far has been failed, so the protocol has only ever been seen in its first
 * step; answering one of them should reveal the second. The buffer is left
 * exactly as it arrived, which is deliberate -- everything past the path is
 * zero, so whatever length field the application reads back reads as zero,
 * which is a truthful "empty" rather than a garbage size it might try to act
 * on.
 */
static const char *supp_ok_match;

/*
 * Listener 10 -- the file system service. See docs/03-listener-services.md.
 *
 * Descriptor-based: only path-taking ops carry a path, and everything after an
 * open refers to the descriptor returned in the reply. In practice this
 * listener only ever sees existence probes; the data crosses gpfs below.
 *
 * Request:
 *
 *   +0      u32   operation
 *   +4      char  path[256]       (path-taking ops: open, creat, lstat, ...)
 *   +260    u32   open flags      (open; bit 6 is O_CREAT)
 *   +260    u32   mode            (creat, mkdir)
 *
 *   +4      s32   fd              (fd-based ops: read, write, close, lseek)
 *   +8      u32   count           (read)
 *   +8            data            (write)
 *   +8      s32   offset          (lseek)
 *   +12     u32   whence          (lseek)
 *   +20008  u32   count           (write)
 *
 * Reply, written over the head of the same buffer:
 *
 *   +0      u32   the operation, echoed back
 *   +4      s32   result: the fd for an open, a byte count for read and
 *                 write, 0 for success, or -1 on failure
 *   +4            data            (read)
 *   +20004  s32   bytes read      (read)
 */
#define FS_OP		0
#define FS_PATH		4
#define FS_PATH_MAX	256
#define FS_OPEN_FLAGS	260
#define FS_MODE		260

#define FS_FD		4
#define FS_ARG		8
#define FS_WHENCE	12
#define FS_WRITE_COUNT	20008

#define FS_RSP_OP	0
#define FS_RSP_RET	4
#define FS_RSP_DATA	4
#define FS_RSP_NREAD	20004
#define FS_RSP_MAX	20000

#define FS_OP_OPEN	0x0202
#define FS_OP_OPENAT	0x0203
#define FS_OP_UNLINKAT	0x0204
#define FS_OP_FCNTL	0x0205
#define FS_OP_CREAT	0x0206
#define FS_OP_READ	0x0207
#define FS_OP_WRITE	0x0208
#define FS_OP_CLOSE	0x0209
#define FS_OP_LSEEK	0x020a
#define FS_OP_LINK	0x020b
#define FS_OP_UNLINK	0x020c
#define FS_OP_RMDIR	0x020d
#define FS_OP_FSTAT	0x020e
#define FS_OP_LSTAT	0x020f
#define FS_OP_MKDIR	0x0210
#define FS_OP_TESTDIR	0x0211
#define FS_OP_TELLDIR	0x0212
#define FS_OP_REMOVE	0x0213
#define FS_OP_CHOWN_CHMOD 0x0214
#define FS_OP_UNUSED	0x0215
#define FS_OP_FSYNC	0x0216
#define FS_OP_RENAME	0x0217
#define FS_OP_FREE_SPACE 0x0218
#define FS_OP_DIR_OPEN	0x0219
#define FS_OP_DIR_READ	0x021a
#define FS_OP_DIR_CLOSE	0x021b
#define FS_OP_GETERR	0x021c	/* not a readdir: hands back the last errno */
#define FS_OP_SHUTDOWN	0x021d

/*
 * Where the service is rooted. The application asks for Android paths that do
 * not exist here -- /data/vendor/fpdump, /persist/data -- so they are served
 * from below this directory rather than taken literally.
 */
#define FS_ROOT		"/var/lib/goodix-fp"

static void fs_path(char *out, size_t out_len, const char *req)
{
	snprintf(out, out_len, "%s%s%s", FS_ROOT, req[0] == '/' ? "" : "/", req);
}

/* Where the service is rooted; a pristine copy of every write lands here too. */
#define FS_CAP_DIR	FS_ROOT "/captured-writes"

/* Create every parent directory of path, ignoring those that exist. */
static void fs_mkparents(const char *path)
{
	char tmp[512];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(tmp, 0755);
		*p = '/';
	}
}

/*
 * Keep a numbered, untouched copy of the payload the TA handed us -- this is
 * the auto-generated base/calibration, the artefact worth having off the
 * device. The counter keeps successive base updates from clobbering each other.
 */
static void fs_capture_write(uint32_t op, const char *req_path, uint32_t off,
			     const void *data, uint32_t len)
{
	static int seq;
	char cap[600];
	FILE *f;

	mkdir(FS_CAP_DIR, 0755);
	snprintf(cap, sizeof(cap), "%s/%03d-op%04x.bin", FS_CAP_DIR, seq, op);
	f = fopen(cap, "wb");
	if (f) {
		fwrite(data, 1, len, f);
		fclose(f);
		printf("  captured -> %s (%u bytes, req path %s off %u)\n",
		       cap, len, req_path, off);
	}
	seq++;
}

/*
 * Which path each descriptor was opened for. The protocol itself does not need
 * this -- everything after an open is addressed by fd -- but a log that names
 * files is worth a great deal more than one full of bare numbers, and the write
 * capture wants somewhere to file its payload.
 */
#define FS_MAX_FD	64
static char *fs_fd_path[FS_MAX_FD];

static void fs_fd_remember(int fd, const char *path)
{
	if (fd < 0 || fd >= FS_MAX_FD)
		return;
	free(fs_fd_path[fd]);
	fs_fd_path[fd] = strdup(path);
}

static const char *fs_fd_name(int fd)
{
	if (fd < 0 || fd >= FS_MAX_FD || !fs_fd_path[fd])
		return "?";
	return fs_fd_path[fd];
}

/*
 * The errno of the last failed operation, returned by op 0x021c. The
 * application asks after every failure and the answer decides what it does
 * next: ENOENT means "create it", anything else means give up.
 */
static int fs_last_errno;

/*
 * Answer with {op, result}. The listener always succeeds at the transport
 * level, exactly as libdrmfs does; a failure is reported in the result word,
 * not by refusing the request. Refusing at the transport level -- what this did
 * before -- tells the application nothing about what went wrong.
 */
static int fs_reply(uint8_t *buf, uint32_t op, int32_t ret)
{
	*(uint32_t *)(buf + FS_RSP_OP) = op;
	*(int32_t *)(buf + FS_RSP_RET) = ret;
	return 0;
}

/*
 * Answer a failure. The result word carries the *raw libc return* -- -1 -- and
 * not a negative errno: libdrmfs stores what open() and friends returned, and
 * the errno travels separately via 0x021c. Getting this wrong would hand the
 * application -2 where it expects -1.
 */
static int fs_fail(uint8_t *buf, uint32_t op, int32_t ret)
{
	fs_last_errno = errno;
	return fs_reply(buf, op, ret);
}

/* As above, for the ops whose result is just "did it work". */
static int fs_reply_rc(uint8_t *buf, uint32_t op, int rc)
{
	if (rc < 0)
		return fs_fail(buf, op, rc);
	return fs_reply(buf, op, rc);
}


/*
 * Listener 28672 -- the GP file system, where every read, write and delete of a
 * sealed object happens. See docs/03-listener-services.md.
 *
 * The opcode encodes both the operation and which base directory to resolve
 * against:
 *
 *   op % 4 == 0  read      op / 4 == 0  resolve automatically
 *   op % 4 == 1  write     op / 4 == 1  force the data path
 *   op % 4 == 2  delete    op / 4 == 2  force the persist path
 *   op % 4 == 3  rename
 *   op == 12     GPFS version query; returns version 2
 *
 * We serve every base the same way -- rooted under FS_ROOT, exactly as listener
 * 10 does -- because the names the application sends are absolute Android paths
 * and gpfs special-cases those to be taken as-is.
 *
 * Request:
 *
 *   +0      u32   operation
 *   +4      char  name[256]     NUL-terminated; the old name for a rename
 *   +0x104  s32   offset        read/write seek position
 *   +0x104  char  name[256]     rename only: the new name
 *   +0x108  u32   length        bytes wanted (read) or supplied (write)
 *   +0x10c  u32   backup        write only: also keep a .bak copy
 *   +0x110        data          write payload
 *
 * Reply, over the head of the same buffer:
 *
 *   +0      u32   the operation, echoed
 *   +4      s32   0, or errno
 *   +8      u32   bytes read or written
 *   +12           data          read only, up to 512000 bytes
 *
 * There is no errno opcode here; unlike listener 10, failures are reported
 * inline. A write is expected to copy the existing object to <name>.bak first.
 */
#define GPFS_LISTENER		28672
#define GPFS_BUF_SIZE		0x7e000

#define GP_OP			0
#define GP_NAME			4
#define GP_NAME_MAX		256
#define GP_OFFSET		0x104
#define GP_NAME2		0x104
#define GP_LENGTH		0x108
#define GP_BACKUP		0x10c
#define GP_DATA			0x110

#define GP_RSP_OP		0
#define GP_RSP_RET		4
#define GP_RSP_COUNT		8
#define GP_RSP_DATA		12
#define GP_READ_MAX		512000

/* Copy path to path.bak, best effort -- the vendor does this before a write. */
static void gp_backup(const char *path)
{
	char bak[600];
	char buf[8192];
	int in, out;
	ssize_t n;

	in = open(path, O_RDONLY);
	if (in < 0)
		return;

	snprintf(bak, sizeof(bak), "%s.bak", path);
	out = open(bak, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return;
	}

	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n)
			break;

	close(in);
	close(out);
}

/* Serve one gpfs request in place. Always answers; failures go in the reply. */
static int gpfs_serve(uint8_t *buf)
{
	char path[512], path2[512];
	uint32_t op, len;
	int32_t off;
	ssize_t n;
	int fd, rc;

	op = *(uint32_t *)(buf + GP_OP);
	buf[GP_NAME + GP_NAME_MAX - 1] = '\0';
	fs_path(path, sizeof(path), (const char *)(buf + GP_NAME));

	if (op > 12) {
		printf("  gpfs op %u out of range\n", op);
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = -1;
		return 0;
	}

	if (op == 12) {
		printf("  gpfs version -> 2\n");
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = 2;
		*(uint32_t *)(buf + GP_RSP_COUNT) = 0;
		return 0;
	}

	switch (op % 4) {
	case 0:					/* read */
		off = *(int32_t *)(buf + GP_OFFSET);
		len = *(uint32_t *)(buf + GP_LENGTH);
		if (len > GP_READ_MAX)
			len = GP_READ_MAX;

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			printf("  gpfs[%u] read  %s -> %s\n", op, path, strerror(errno));
			*(uint32_t *)(buf + GP_RSP_OP) = op;
			*(int32_t *)(buf + GP_RSP_RET) = errno;
			*(uint32_t *)(buf + GP_RSP_COUNT) = 0;
			return 0;
		}

		n = pread(fd, buf + GP_RSP_DATA, len, off);
		close(fd);

		printf("  gpfs[%u] read  %s off=%d want=%u got=%zd\n",
		       op, path, off, len, n);
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = n < 0 ? errno : 0;
		*(uint32_t *)(buf + GP_RSP_COUNT) = n < 0 ? 0 : (uint32_t)n;
		return 0;

	case 1:					/* write */
		off = *(int32_t *)(buf + GP_OFFSET);
		len = *(uint32_t *)(buf + GP_LENGTH);
		if (len > GP_READ_MAX)
			len = GP_READ_MAX;

		fs_mkparents(path);
		if (*(uint32_t *)(buf + GP_BACKUP))
			gp_backup(path);

		fs_capture_write(op, (const char *)(buf + GP_NAME), off,
				 buf + GP_DATA, len);

		fd = open(path, O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			printf("  gpfs[%u] write %s -> %s\n", op, path, strerror(errno));
			*(uint32_t *)(buf + GP_RSP_OP) = op;
			*(int32_t *)(buf + GP_RSP_RET) = errno;
			*(uint32_t *)(buf + GP_RSP_COUNT) = 0;
			return 0;
		}

		n = pwrite(fd, buf + GP_DATA, len, off);
		if (off == 0 && n > 0)
			rc = ftruncate(fd, n);
		close(fd);

		printf("  gpfs[%u] write %s off=%d len=%u wrote=%zd%s\n", op, path,
		       off, len, n,
		       *(uint32_t *)(buf + GP_BACKUP) ? " (+bak)" : "");
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = n < 0 ? errno : 0;
		*(uint32_t *)(buf + GP_RSP_COUNT) = n < 0 ? 0 : (uint32_t)n;
		return 0;

	case 2:					/* delete */
		rc = unlink(path);
		if (rc && errno == EISDIR)
			rc = rmdir(path);
		printf("  gpfs[%u] del   %s -> %s\n", op, path,
		       rc ? strerror(errno) : "ok");
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = rc ? errno : 0;
		return 0;

	default:				/* rename */
		buf[GP_NAME2 + GP_NAME_MAX - 1] = '\0';
		fs_path(path2, sizeof(path2), (const char *)(buf + GP_NAME2));
		fs_mkparents(path2);
		rc = rename(path, path2);
		printf("  gpfs[%u] move  %s -> %s : %s\n", op, path, path2,
		       rc ? strerror(errno) : "ok");
		*(uint32_t *)(buf + GP_RSP_OP) = op;
		*(int32_t *)(buf + GP_RSP_RET) = rc ? errno : 0;
		return 0;
	}
}

/* Serve one request in place. Returns 0 once a reply has been written. */
static int fs_serve(uint8_t *buf)
{
	char path[512], path2[512];
	uint32_t op, count, flags;
	int fd, rc;
	ssize_t n;

	op = *(uint32_t *)(buf + FS_OP);
	buf[FS_PATH + FS_PATH_MAX - 1] = '\0';
	fs_path(path, sizeof(path), (const char *)(buf + FS_PATH));

	switch (op) {
	case FS_OP_OPEN:
		flags = *(uint32_t *)(buf + FS_OPEN_FLAGS);
		/*
		 * The TA says for itself whether it means to create -- bit 6,
		 * O_CREAT -- so there is nothing left to guess at here. SAVE
		 * creating its secure objects is the whole point; refusing that
		 * open is what used to come back as
		 * GF_ERROR_WRITE_SECURE_OBJECT_FAILED.
		 */
		if (flags & O_CREAT)
			fs_mkparents(path);

		fd = open(path, (int)flags, 0644);
		if (fd < 0) {
			printf("  open  %s flags=0x%x -> %s\n", path, flags,
			       strerror(errno));
			return fs_fail(buf, op, -1);
		}

		fs_fd_remember(fd, path);
		printf("  open  %s flags=0x%x -> fd %d\n", path, flags, fd);
		return fs_reply(buf, op, fd);

	case FS_OP_CREAT:
		fs_mkparents(path);
		flags = *(uint32_t *)(buf + FS_MODE);
		fd = creat(path, (mode_t)flags);
		if (fd < 0) {
			printf("  creat %s mode=0%o -> %s\n", path, flags,
			       strerror(errno));
			return fs_fail(buf, op, -1);
		}

		fs_fd_remember(fd, path);
		printf("  creat %s mode=0%o -> fd %d\n", path, flags, fd);
		return fs_reply(buf, op, fd);

	case FS_OP_READ:
		fd = *(int32_t *)(buf + FS_FD);
		count = *(uint32_t *)(buf + FS_ARG);
		if (count > FS_RSP_MAX)
			count = FS_RSP_MAX;

		/*
		 * A read answers with the data where the result word would
		 * otherwise sit, and the count right at the end of the reply.
		 * This is the one op whose reply is not eight bytes.
		 */
		n = read(fd, buf + FS_RSP_DATA, count);
		if (n < 0)
			fs_last_errno = errno;
		*(uint32_t *)(buf + FS_RSP_OP) = op;
		*(int32_t *)(buf + FS_RSP_NREAD) = (int32_t)n;

		printf("  read  fd %d (%s) want=%u got=%zd\n", fd,
		       fs_fd_name(fd), count, n);
		return 0;

	case FS_OP_WRITE:
		fd = *(int32_t *)(buf + FS_FD);
		count = *(uint32_t *)(buf + FS_WRITE_COUNT);
		if (count > FS_RSP_MAX)
			count = FS_RSP_MAX;

		/*
		 * Keep a pristine copy of everything the TA persists. These are
		 * its sealed containers, and having them off the device is what
		 * lets a later run be reasoned about rather than repeated.
		 */
		fs_capture_write(op, fs_fd_name(fd), 0, buf + FS_ARG, count);

		n = write(fd, buf + FS_ARG, count);
		printf("  write fd %d (%s) len=%u wrote=%zd\n", fd,
		       fs_fd_name(fd), count, n);
		if (n < 0)
			return fs_fail(buf, op, -1);
		return fs_reply(buf, op, (int32_t)n);

	case FS_OP_CLOSE:
		fd = *(int32_t *)(buf + FS_FD);
		printf("  close fd %d (%s)\n", fd, fs_fd_name(fd));
		rc = close(fd);
		if (fd >= 0 && fd < FS_MAX_FD) {
			free(fs_fd_path[fd]);
			fs_fd_path[fd] = NULL;
		}
		return fs_reply_rc(buf, op, rc);

	case FS_OP_LSEEK:
		fd = *(int32_t *)(buf + FS_FD);
		rc = (int)lseek(fd, *(int32_t *)(buf + FS_ARG),
				(int)*(uint32_t *)(buf + FS_WHENCE));
		printf("  lseek fd %d (%s) -> %d\n", fd, fs_fd_name(fd), rc);
		return fs_reply_rc(buf, op, rc);

	case FS_OP_FSYNC:
		fd = *(int32_t *)(buf + FS_FD);
		printf("  fsync fd %d (%s)\n", fd, fs_fd_name(fd));
		return fs_reply_rc(buf, op, fsync(fd));

	case FS_OP_UNLINK:
		printf("  unlink %s\n", path);
		return fs_reply_rc(buf, op, unlink(path));

	case FS_OP_MKDIR:
		fs_mkparents(path);
		flags = *(uint32_t *)(buf + FS_MODE);
		rc = mkdir(path, (mode_t)flags);
		if (rc && errno == EEXIST)
			rc = 0;
		printf("  mkdir %s mode=0%o -> %d\n", path, flags, rc);
		return fs_reply_rc(buf, op, rc);

	case FS_OP_RENAME:
		buf[260 + FS_PATH_MAX - 1] = '\0';
		fs_path(path2, sizeof(path2), (const char *)(buf + 260));
		printf("  rename %s -> %s\n", path, path2);
		return fs_reply_rc(buf, op, rename(path, path2));

	case FS_OP_GETERR:
		/*
		 * Asked after every failure, and the answer decides whether the
		 * application creates the file or gives up. Plain positive
		 * errno, straight out of the global libdrmfs keeps.
		 */
		printf("  errno? -> %d (%s)\n", fs_last_errno,
		       strerror(fs_last_errno));
		return fs_reply(buf, op, fs_last_errno);

	case FS_OP_SHUTDOWN:
		printf("  shutdown requested\n");
		return fs_reply(buf, op, 0);

	default:
		/*
		 * Dump anything not in the table. The opcode space is dense
		 * (0x202..0x21d) and the ops left unimplemented here are the
		 * fcntl, link, rmdir, stat, directory and free-space operations,
		 * none of which the application has ever asked for. In particular,
		 * fstat and lstat need Qualcomm's packed tz_stat reply rather than a
		 * native struct stat. If any arrives, keep the request on record
		 * rather than answering it with a guessed ABI.
		 */
		printf("  op 0x%04x UNIMPLEMENTED -- full request follows\n", op);
		dump_buf(buf, SUPP_BUF_SIZE);
		mark("supplicant: unimplemented op 0x%04x", op);
		errno = ENOSYS;
		return fs_fail(buf, op, -1);
	}
}

/* Register a listener and serve requests until killed. */
/*
 * Serve every listener the application uses, from one process.
 *
 * This has to be one process: the driver keeps a single supplicant queue for
 * the whole device, so two supplicants would steal each other's requests. The
 * listener a request belongs to comes back in arg.func.
 */
struct supp_listener {
	uint32_t id;
	uint32_t bufsz;
	struct shm sb;
	int (*serve)(uint8_t *buf);
};

static int run_supplicant(uint32_t listener_id)
{
	struct supp_listener ls[2] = {
		{ 10,		 SUPP_BUF_SIZE, {0}, fs_serve   },
		{ GPFS_LISTENER, GPFS_BUF_SIZE, {0}, gpfs_serve },
	};
	unsigned int n = sizeof(ls) / sizeof(ls[0]), i;
	struct {
		struct tee_iocl_supp_recv_arg arg;
		struct tee_ioctl_param params[2];
	} recv = {};
	struct {
		struct tee_iocl_supp_send_arg arg;
		struct tee_ioctl_param params[1];
	} send = {};
	struct tee_ioctl_buf_data bd;
	int fd;

	/* A single id still works, for probing one listener at a time. */
	if (listener_id) {
		for (i = 0; i < n; i++)
			if (ls[i].id == listener_id) {
				ls[0] = ls[i];
				n = 1;
				break;
			}
		if (n != 1) {
			ls[0].id = listener_id;
			ls[0].bufsz = SUPP_BUF_SIZE;
			ls[0].serve = fs_serve;
			n = 1;
		}
	}

	fd = open(SUPP_DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", SUPP_DEV, strerror(errno));
		return -1;
	}

	for (i = 0; i < n; i++) {
		struct {
			struct tee_ioctl_open_session_arg arg;
			struct tee_ioctl_param params[2];
		} sess = {};

		if (shm_alloc(fd, ls[i].bufsz, &ls[i].sb))
			return -1;

		sess.arg.num_params = 2;
		sess.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
		sess.params[0].a = ls[i].id;
		sess.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
		sess.params[1].a = 0;
		sess.params[1].b = ls[i].bufsz;
		sess.params[1].c = ls[i].sb.id;

		bd.buf_ptr = (uintptr_t)&sess;
		bd.buf_len = sizeof(sess);

		if (ioctl(fd, TEE_IOC_OPEN_SESSION, &bd)) {
			fprintf(stderr, "register listener %u: %s\n", ls[i].id,
				strerror(errno));
			return -1;
		}

		printf("listener %u registered (%u byte buffer)\n", ls[i].id,
		       ls[i].bufsz);
		mark("supplicant: listener %u registered", ls[i].id);
	}
	fflush(stdout);

	for (;;) {
		struct supp_listener *l = NULL;
		int ok;

		memset(&recv, 0, sizeof(recv));
		recv.arg.num_params = 2;
		bd.buf_ptr = (uintptr_t)&recv;
		bd.buf_len = sizeof(recv);

		if (ioctl(fd, TEE_IOC_SUPPL_RECV, &bd)) {
			fprintf(stderr, "SUPPL_RECV: %s\n", strerror(errno));
			return -1;
		}

		for (i = 0; i < n; i++)
			if (ls[i].id == recv.arg.func)
				l = &ls[i];

		printf("=== request for listener %u ===\n", recv.arg.func);

		if (!l) {
			/*
			 * Not ours. Answering would mean writing into a buffer
			 * we do not own, so refuse it and say so.
			 */
			printf("  unregistered listener -- refusing\n");
			mark("supplicant: unknown listener %u", recv.arg.func);
		} else {
			dump_buf(l->sb.va, l->bufsz > 0x5000 ? 0x120 : l->bufsz);
		}

		ok = 0;
		if (l) {
			if (supp_ok_match && !strcmp(supp_ok_match, "serve"))
				ok = !l->serve(l->sb.va);
			else
				ok = supp_ok_match &&
				     strstr((const char *)l->sb.va + FS_PATH,
					    supp_ok_match) != NULL;
		}

		/*
		 * params[0] of the receive carries which request this is. Echo
		 * it back, or the driver cannot tell this answer from one that
		 * overran the timeout and rejects it with ESTALE.
		 *
		 * It arrives as VALUE_INPUT and goes back as VALUE_OUTPUT:
		 * directions are named for this process, and the core only
		 * transfers a value marked the matching way in each direction.
		 */
		memset(&send, 0, sizeof(send));
		send.arg.ret = ok ? 0 : 1;
		send.arg.num_params = 1;
		send.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
		send.params[0].a = recv.params[0].a;

		bd.buf_ptr = (uintptr_t)&send;
		bd.buf_len = sizeof(send);

		if (ioctl(fd, TEE_IOC_SUPPL_SEND, &bd)) {
			fprintf(stderr, "SUPPL_SEND: %s\n", strerror(errno));
			return -1;
		}

		if (l)
			printf("answered op 0x%04x -> %d\n",
			       *(uint32_t *)((uint8_t *)l->sb.va + FS_RSP_OP),
			       *(int32_t *)((uint8_t *)l->sb.va + FS_RSP_RET));
		fflush(stdout);
	}

	return 0;
}


/*
 * The bring-up sequence, in the order the HAL does it. INIT is the one that
 * calls out to listener 10, so a supplicant has to be attached before this
 * runs or TZ is left waiting.
 */
static int bringup(int fd, uint32_t session)
{
	static const struct {
		uint32_t cmd;
		size_t payload;
		const char *name;
	} seq[] = {
		{ 1000, 520, "DETECT_SENSOR" },
		{ 1001, 348, "INIT" },
		{ 1005, 104, "INIT_FINISHED" },
		{ 1089, 5500, "GET_DEV_INFO" },
	};
	unsigned int i;

	for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		mark("bringup: sending %s (%u, payload %zu)", seq[i].name,
		     seq[i].cmd, seq[i].payload);
		printf("--- %s ---\n", seq[i].name);
		fflush(stdout);

		if (invoke(fd, session, seq[i].cmd, seq[i].payload) < 0) {
			mark("bringup: %s FAILED at ioctl", seq[i].name);
			return -1;
		}

		mark("bringup: %s returned", seq[i].name);
	}

	mark("bringup: complete");

	return 0;
}

/*
 * Join the driver's netlink channel. The driver learns where to send events
 * from the first message it receives, so one has to be sent before any
 * interrupt will be delivered.
 */
static int gf_netlink_open(void)
{
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_pid = getpid(),
	};
	struct {
		struct nlmsghdr hdr;
		char body[16];
	} msg = {};
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	int s;

	s = socket(AF_NETLINK, SOCK_RAW, GF_NETLINK_PROTO);
	if (s < 0) {
		fprintf(stderr, "netlink socket: %s\n", strerror(errno));
		return -1;
	}

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr))) {
		fprintf(stderr, "netlink bind: %s\n", strerror(errno));
		close(s);
		return -1;
	}

	msg.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(msg.body));
	msg.hdr.nlmsg_pid = getpid();
	msg.hdr.nlmsg_flags = 0;
	strcpy(msg.body, "hello");

	if (sendto(s, &msg, msg.hdr.nlmsg_len, 0, (struct sockaddr *)&kernel,
		   sizeof(kernel)) < 0) {
		fprintf(stderr, "netlink register: %s\n", strerror(errno));
		close(s);
		return -1;
	}

	printf("netlink %d registered (pid %d)\n", GF_NETLINK_PROTO, getpid());

	return s;
}

/* Wait for one interrupt event. Returns 1 on an IRQ, 0 on timeout, -1 error. */
static int gf_netlink_wait(int s, int timeout_ms)
{
	struct pollfd pfd = { .fd = s, .events = POLLIN };
	char buf[512];
	ssize_t n;
	int ret;

	/*
	 * A signal must not end the window. poll() returning EINTR was treated
	 * as a fatal error by the caller, which breaks out of the capture loop
	 * -- so a stray SIGCHLD from a supplicant started alongside, or an
	 * alarm, ended a capture after its first interrupt and made it look as
	 * though the sensor had stopped reporting.
	 */
	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0)
		return ret;

	n = recv(s, buf, sizeof(buf), 0);
	if (n < (ssize_t)sizeof(struct nlmsghdr))
		return -1;

	{
		struct nlmsghdr *h = (struct nlmsghdr *)buf;
		unsigned char *body = (unsigned char *)NLMSG_DATA(h);

		if (body[0] == GF_NET_EVENT_IRQ)
			return 1;

		printf("netlink event %u (not an IRQ)\n", body[0]);
	}

	return 0;
}

/*
 * Where the payload is non-zero, and the head of it. The application uses this
 * buffer as its working area, so what it leaves behind between commands is the
 * only visible record of what it did with a frame -- and the HAL reads almost
 * nothing from it, so the interesting fields have to be found by comparing one
 * interrupt against another rather than by reading the HAL.
 */
static void pl_summary(const uint8_t *pl, size_t len)
{
	size_t i, start = 0;
	int inrun = 0, runs = 0;

	printf("  payload head:");
	for (i = 0; i < 48; i++)
		printf("%s%02x", (i % 16) ? " " : "\n    ", pl[i]);
	printf("\n");

	for (i = 0; i < len; i++) {
		if (pl[i] && !inrun) {
			inrun = 1;
			start = i;
		} else if (!pl[i] && inrun) {
			/* Tolerate short gaps so a run is not split by a zero. */
			size_t j;

			for (j = i; j < len && j < i + 32; j++)
				if (pl[j])
					break;
			if (j < len && j < i + 32)
				continue;
			inrun = 0;
			if (++runs <= 12)
				printf("  nonzero %zu..%zu (%zu bytes)\n",
				       start, i - 1, i - start);
		}
	}
	if (inrun && ++runs <= 12)
		printf("  nonzero %zu..%zu (%zu bytes)\n", start, len - 1,
		       len - start);
	if (runs > 12)
		printf("  ... %d nonzero runs in total\n", runs);
}

/* Keep a frame for offline comparison; a stalled enrolment is a diff problem. */
static void pl_save(const uint8_t *pl, size_t len, int n)
{
	char path[64];
	FILE *f;

	snprintf(path, sizeof(path), "/home/cromo/gf-image-%d.bin", n);
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(pl, 1, len, f);
	fclose(f);
	printf("  saved %s\n", path);
}

/*
 * Ask the application what the interrupt meant. This is the command a real
 * daemon's event loop is built around.
 */
static int gf_service_irq(int fd, uint32_t session, uint8_t *pl, uint32_t *out)
{
	uint32_t mask, op;
	int b;

	/*
	 * Deliberately *not* cleared. The handler in libgf_hal.so keeps one
	 * buffer for the whole of an interrupt and re-invokes GF_CMD_IRQ
	 * through it several times without ever zeroing it, so whatever the
	 * application left behind is part of the protocol. Clearing it between
	 * calls -- which this did -- throws that state away every time.
	 */

	/* Unaligned by design, so write it a byte at a time. */
	{
		uint32_t mode = 0x02000000, size = 512, arm = 1;

		memcpy(pl + GF_IRQ_CTRL_MODE, &mode, sizeof(mode));
		memcpy(pl + GF_IRQ_CTRL_SIZE, &size, sizeof(size));
		memcpy(pl + GF_IRQ_CTRL_ARM, &arm, sizeof(arm));
	}

	if (invoke_pl(fd, session, GF_CMD_IRQ, GF_IRQ_PAYLOAD, pl, pl) < 0)
		return -1;

	memcpy(&mask, pl + GF_IRQ_RSP_MASK, sizeof(mask));
	memcpy(&op, pl + GF_IRQ_RSP_OP, sizeof(op));

	printf("  irq mask=0x%08x operation=%u", mask, op);
	for (b = 0; b < 32; b++)
		if ((mask & (1u << b)) && gf_irq_names[b])
			printf(" %s", gf_irq_names[b]);
	printf("\n");
	mark("irq: mask=0x%08x operation=%u", mask, op);

	if (out)
		*out = mask;

	return 0;
}

/*
 * Enrolment, which is where the file service starts to matter: a template has
 * to be written somewhere, and the application cannot write it itself.
 *
 * ENROLL is gated on echoing back the challenge PRE_ENROLL just issued --
 * answering with anything else gets GF_ERROR_INVALID_CHALLENGE (1056). The
 * gate is the challenge value, not a valid signature, so the rest of the
 * authentication token can be left zero.
 */
static int enroll(int fd, uint32_t session)
{
	/* hw_auth_token_t: version at +0, then the challenge. 69 bytes. */
	static const size_t TOKEN_OFF = 110;
	uint8_t pl[512];
	uint64_t challenge;

	printf("--- PRE_ENROLL ---\n");
	mark("enroll: sending PRE_ENROLL");
	memset(pl, 0, sizeof(pl));
	if (invoke_pl(fd, session, 1006, 112, NULL, pl))
		return -1;

	memcpy(&challenge, pl + 104, sizeof(challenge));
	printf("  challenge = 0x%016llx\n", (unsigned long long)challenge);
	mark("enroll: challenge 0x%016llx", (unsigned long long)challenge);

	if (!challenge)
		printf("  ! no challenge issued; ENROLL will be refused\n");

	printf("--- ENROLL ---\n");
	mark("enroll: sending ENROLL");
	memset(pl, 0, sizeof(pl));
	*(uint32_t *)(pl + 100) = 0;			/* group id */
	pl[108] = 0;					/* dumping off */
	pl[TOKEN_OFF] = 0;				/* token version */
	if (getenv("GF_BAD_CHALLENGE")) {
		challenge ^= 0xffffffffffffffffULL;
		printf("  !! deliberately corrupting the challenge\n");
	}
	memcpy(pl + TOKEN_OFF + 1, &challenge, sizeof(challenge));

	if (invoke_pl(fd, session, 1007, 180, pl, pl))
		return -1;

	mark("enroll: ENROLL returned");

	/*
	 * A known-answer test that needs no finger. Android answers a SAVE with
	 * no enrolment behind it 1047 GF_ERROR_FINGER_NOT_EXIST. If that comes
	 * back here too, the status word carries errors and the corrupted
	 * challenge really was accepted. If it comes back 0, the word is not
	 * carrying errors at all and nothing read from it means anything.
	 */
	if (getenv("GF_PROBE_SAVE")) {
		uint8_t body[112];
		int rc2;

		printf("--- SAVE with no enrolment (expect 1047) ---\n");
		memset(body, 0, sizeof(body));
		*(uint32_t *)(body + 100) = 0;			/* group */
		*(uint32_t *)(body + 104) = 0x11223344;		/* bogus fid */
		rc2 = invoke_pl(fd, session, 1012, sizeof(body), body, body);
		printf("  SAVE -> %d %s\n", rc2, gf_strerror(rc2));
	}

	return 0;
}

/*
 * Capture a frame with no finger and no enrolment, through the factory-test
 * dispatch table.
 *
 * gf_hal_test_snr_get_snr_result() does exactly two things: it starts the test
 * with class 3 command 1097, then polls class 3 command 1098 in a loop and
 * copies width * height * 2 bytes out of the response at +100. Both payloads
 * are simply zeroed first, so nothing has to be configured -- only the lengths
 * are load bearing, and they are 29680 and 29668, not the 409612 recorded
 * earlier in the notes for 1098.
 *
 * This matters because it removes the need for someone to be at the sensor at
 * the right moment: a missed window looks exactly like a capture that stopped
 * working, which has already cost two runs.
 */
#define GF_CMD_TEST_START	1097
#define GF_TEST_START_LEN	29680
#define GF_CMD_TEST_POLL_IMAGE	1098
#define GF_TEST_POLL_LEN	29668
#define GF_TEST_IMAGE_OFF	100
#define GF_TEST_CLASS		3

/*
 * Enrolment stops once it has its samples; the template only reaches the secure
 * file system when the host asks for it. Hand back the ids the enrolment
 * reported -- committing finger 0 fails with GF_ERROR_FINGER_NOT_EXIST.
 */
static void commit_enrol(int fd, uint32_t session, uint32_t group, uint32_t finger)
{
	uint8_t body[GF_SAVE_PAYLOAD];

	printf("--- enrolment complete: POST_ENROLL + SAVE "
	       "(group=%u finger=0x%08x) ---\n", group, finger);
	mark("enrol: committing group=%u finger=0x%08x", group, finger);

	/*
	 * No POST_ENROLL here -- it ends the enrolment session, leaving SAVE
	 * nothing to persist. It belongs to the removal flow instead.
	 */
	memset(body, 0, sizeof(body));
	*(uint32_t *)(body + 100) = group;
	*(uint32_t *)(body + 104) = finger;
	if (invoke_pl(fd, session, GF_CMD_SAVE, sizeof(body), body, NULL))
		printf("  SAVE: invoke failed\n");

	memset(body, 0, sizeof(body));
	*(uint32_t *)(body + 100) = group;
	if (invoke_pl(fd, session, GF_CMD_SET_ACTIVE_GROUP,
		      GF_SET_ACTIVE_GROUP_PAYLOAD, body, NULL))
		printf("  SET_ACTIVE_GROUP: invoke failed\n");

	memset(body, 0, sizeof(body));
	if (invoke_pl(fd, session, GF_CMD_GET_AUTH_ID, sizeof(body), body, NULL))
		printf("  GET_AUTH_ID: invoke failed\n");
}

/*
 * Enrol, then actually service interrupts for a while. ENROLL only arms the
 * operation; the capture itself is interrupt driven, so nothing is written to
 * the template store until a finger has been on the sensor.
 */
/*
 * Arm a match. The payload must echo back the descriptor SET_ACTIVE_GROUP
 * returns -- zeros give 1047 GF_ERROR_FINGER_NOT_EXIST whatever is on disk.
 * See docs/02-ta-protocol.md.
 */
static int authenticate(int fd, uint32_t session)
{
	uint8_t body[GF_SAVE_PAYLOAD];

	memset(body, 0, sizeof(body));
	*(uint32_t *)(body + 100) = 0;			/* group */
	if (invoke_pl(fd, session, GF_CMD_SET_ACTIVE_GROUP,
		      GF_SET_ACTIVE_GROUP_PAYLOAD, body, body)) {
		printf("  SET_ACTIVE_GROUP: invoke failed\n");
		return -1;
	}

	printf("--- arming AUTHENTICATE (templates=%u) ---\n",
	       *(uint32_t *)(body + 0x10));

	if (invoke_pl(fd, session, GF_CMD_AUTHENTICATE, sizeof(body), body,
		      NULL)) {
		printf("  AUTHENTICATE: invoke failed\n");
		return -1;
	}
	return 0;
}

/* Close out a successful match. Not a query: the verdict is already known. */
static void auth_finish(int fd, uint32_t session)
{
	uint8_t body[GF_SAVE_PAYLOAD];

	memset(body, 0, sizeof(body));
	if (invoke_pl(fd, session, GF_CMD_AUTHENTICATE_FINISH, sizeof(body),
		      body, body))
		printf("  AUTHENTICATE_FINISH: invoke failed\n");
}

/*
 * Unenrol a finger. A single command carrying the finger id as an int32 at
 * payload +0x68 -- the same slot SAVE uses for it. The vendor sends REMOVE
 * after a couple of POST_ENROLLs, which is the teardown half of that command
 * rather than anything to do with enrolling.
 */
static int remove_finger(int fd, uint32_t session, uint32_t fid)
{
	uint8_t body[GF_SAVE_PAYLOAD];

	memset(body, 0, sizeof(body));
	*(uint32_t *)(body + 100) = 0;			/* group */
	*(uint32_t *)(body + 104) = fid;

	printf("--- REMOVE finger 0x%08x ---\n", fid);
	if (invoke_pl(fd, session, GF_CMD_REMOVE, sizeof(body), body, body)) {
		printf("  REMOVE: invoke failed\n");
		return -1;
	}
	printf("  removed\n");
	return 0;
}

/*
 * Ask the application what it believes is enrolled, rather than inferring it
 * from what is on disk. The response carries the same list shape the other
 * commands return: a count, then one entry per finger.
 */
static int enumerate(int fd, uint32_t session)
{
	uint8_t body[GF_SAVE_PAYLOAD];
	uint32_t i, n;

	/*
	 * Establish the group first. On its own ENUMERATE answers
	 * 1088 GF_ERROR_NO_GROUP_ID -- the same shape as AUTHENTICATE needing
	 * the template list: these commands want the context SET_ACTIVE_GROUP
	 * sets up, not just a group number in their own payload.
	 */
	memset(body, 0, sizeof(body));
	*(uint32_t *)(body + 100) = 0;			/* group */
	if (invoke_pl(fd, session, GF_CMD_SET_ACTIVE_GROUP,
		      GF_SET_ACTIVE_GROUP_PAYLOAD, body, body))
		printf("  SET_ACTIVE_GROUP: invoke failed\n");

	printf("--- ENUMERATE ---\n");
	if (invoke_pl(fd, session, GF_CMD_ENUMERATE, sizeof(body), body,
		      body)) {
		printf("  ENUMERATE: invoke failed\n");
		return -1;
	}

	n = *(uint32_t *)(body + 0x08);
	printf("  +0x08 = %u\n", n);
	for (i = 0; i < GF_SAVE_PAYLOAD / 4; i++) {
		uint32_t v = *(uint32_t *)(body + i * 4);

		if (v)
			printf("    +0x%02x = 0x%08x\n", i * 4, v);
	}
	return 0;
}

static int capture(int fd, uint32_t session, int gf, int seconds)
{
	uint8_t *pl;
	time_t deadline;
	int nl, i = 0, drain, frames = 0, enough = 0;
	uint32_t group = 0, finger = 0, remain = 0;
	int committed = 0;

	pl = malloc(GF_IRQ_PAYLOAD);
	if (!pl) {
		fprintf(stderr, "cannot allocate the IRQ payload\n");
		return -1;
	}

	nl = gf_netlink_open();
	if (nl < 0) {
		free(pl);
		return -1;
	}

	if (gf >= 0 && ioctl(gf, GF_IOC_ENABLE_IRQ))
		fprintf(stderr, "ENABLE_IRQ: %s\n", strerror(errno));

	if ((do_auth ? authenticate(fd, session) : enroll(fd, session))) {
		close(nl);
		free(pl);
		return -1;
	}

	printf("--- waiting for a finger (%d s) ---\n", seconds);
	fflush(stdout);
	mark("capture: waiting for a finger");

	/*
	 * Bounded by elapsed time, not by iterations. Counting iterations ends
	 * the window early exactly when it is working -- a burst of interrupts
	 * spends the whole budget in a couple of seconds.
	 */
	deadline = time(NULL) + seconds;
	while (time(NULL) < deadline) {
		int ret = gf_netlink_wait(nl, 1000);

		if (ret < 0)
			break;
		if (!ret)
			continue;

		printf("--- interrupt %d ---\n", ++i);

		/*
		 * Drain it. The milan handler (vtable slot 112, the static at
		 * 0x43ac8 in libgf_hal.so) does not service an interrupt with
		 * one command: it issues GF_CMD_IRQ, looks at the mask, acts,
		 * and issues it again from several points in the same pass.
		 * Sending exactly one per netlink event -- which this did --
		 * asks the application what happened and then never gives it
		 * the chance to do the next step, which is the most likely
		 * reason a capture reports finger-down and then stalls.
		 */
		for (drain = 0; drain < GF_IRQ_MAX_DRAIN; drain++) {
			uint32_t mask = 0;

			if (gf_service_irq(fd, session, pl, &mask)) {
				drain = -1;
				break;
			}

			if (!mask)
				break;

			if (mask & (GF_IRQ_IMAGE | GF_IRQ_FRAME_DONE)) {
				printf("  *** a frame arrived ***\n");
				mark("irq: frame arrived, mask=0x%08x", mask);
				pl_summary(pl, GF_IRQ_PAYLOAD);
				if (++frames <= GF_WANT_FRAMES)
					pl_save(pl, GF_IRQ_PAYLOAD, frames);

				if (do_auth) {
					/*
					 * The verdict is here, in the interrupt
					 * payload: +0x0c is zero on a match and
					 * 1064 GF_ERROR_MATCH_FAIL_AND_RETRY
					 * otherwise. Established by diffing a
					 * matching payload against one from a
					 * finger that was never enrolled -- it
					 * is the only field that differs.
					 */
					uint32_t v = *(uint32_t *)(pl + 0x0c);

					printf("  match: %s (0x%08x)\n",
					       v ? gf_strerror(v)
						 : "*** RECOGNISED ***", v);
					mark("auth: verdict %u", v);
					if (!v)
						auth_finish(fd, session);

					/*
					 * AUTHENTICATE is a one-shot arm, like
					 * ENROLL: once it has produced a verdict
					 * the operation is over and further
					 * presses raise no frames at all. Android
					 * sends a fresh 1010 for every unlock
					 * attempt rather than one per session, so
					 * re-arm for the next finger.
					 */
					authenticate(fd, session);
					continue;
				}

				group  = *(uint32_t *)(pl + GF_ENROL_GROUP_OFF);
				finger = *(uint32_t *)(pl + GF_ENROL_FINGER_OFF);
				remain = *(uint32_t *)(pl + GF_ENROL_REMAIN_OFF);
				printf("  enrol: group=%u finger=0x%08x remaining=%u\n",
				       group, finger, remain);

				if (!remain && finger && !committed) {
					committed = 1;
					commit_enrol(fd, session, group, finger);
				}

				/*
				 * A raw-frame probe stops as soon as it has a few
				 * frames to compare, but an enrolment needs the
				 * whole window: base generation is multi-step and
				 * only persists (the SFS write we are here for)
				 * after many finger-down/up cycles. So do not stop
				 * early -- run out the timed window and keep
				 * servicing every interrupt.
				 */
			}
		}

		if (drain < 0)
			break;
		if (enough) {
			printf("--- %d frames captured, stopping early ---\n",
			       frames);
			break;
		}
		if (drain == GF_IRQ_MAX_DRAIN)
			printf("  (still reporting after %d commands)\n", drain);
	}

	printf("--- %d interrupts serviced ---\n", i);

	mark("capture: finished");
	close(nl);
	free(pl);

	return 0;
}

/*
 * Ask the kernel to load an application by name.
 *
 * The image itself is not passed: the driver fetches <name>.mdt and its .bNN
 * segments through request_firmware() and assembles them itself, so the files
 * have to be somewhere the kernel's firmware search path looks, normally
 * /lib/firmware. This process only supplies the name.
 */
static int load_app(const char *name)
{
	struct {
		struct tee_ioctl_open_session_arg arg;
		struct tee_ioctl_param params[1];
	} sess = {};
	struct tee_ioctl_buf_data bd;
	struct shm nm;
	int fd;

	fd = open(SUPP_DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", SUPP_DEV, strerror(errno));
		return -1;
	}

	if (strlen(name) >= 64) {
		fprintf(stderr, "name '%s' is too long\n", name);
		return -1;
	}

	if (shm_alloc(fd, 64, &nm))
		return -1;

	strcpy(nm.va, name);

	sess.arg.num_params = 1;
	sess.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	sess.params[0].b = strlen(name) + 1;
	sess.params[0].c = nm.id;

	bd.buf_ptr = (uintptr_t)&sess;
	bd.buf_len = sizeof(sess);

	if (ioctl(fd, TEE_IOC_OPEN_SESSION, &bd)) {
		fprintf(stderr, "load '%s': %s\n", name, strerror(errno));
		fprintf(stderr,
			"is %s.mdt (with its .bNN segments) in /lib/firmware?\n",
			name);
		return -1;
	}

	printf("loaded '%s', session=%u -- hold this fd open\n", name,
	       sess.arg.session);

	/* The session keeps the load alive for inspection; wait for a key. */
	printf("press enter to release\n");
	getchar();

	return 0;
}

/* Hold the sensor powered for N seconds so another process can drive it. */
static int hold_power(unsigned int secs)
{
	int fd = sensor_power_up();

	if (fd < 0)
		return -1;

	printf("holding power for %us\n", secs);
	fflush(stdout);
	sleep(secs);
	close(fd);

	return 0;
}

int main(int argc, char **argv)
{
	const char *app = "gfenu";
	int do_invoke = 0, do_bringup = 0, do_enroll = 0, do_capture = 0;
	int capture_secs = 30;
	int fd, session;

	/*
	 * Line-buffer the output. These runs are driven from scripts that
	 * redirect to a file and then wait for a particular line -- with the
	 * default block buffering that line sits in stdio until the process
	 * exits, so the waiter times out and the next step races the one it
	 * was supposed to be sequenced after.
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	while (argc > 1 && argv[1][0] == '-' && strcmp(argv[1], "--supp") &&
	       strcmp(argv[1], "--load") && strcmp(argv[1], "--power")) {
		if (!strcmp(argv[1], "--invoke"))
			do_invoke = 1;
		else if (!strcmp(argv[1], "--bringup"))
			do_bringup = 1;
		else if (!strcmp(argv[1], "--enroll"))
			do_enroll = 1;
		else if (!strcmp(argv[1], "--auth"))
			do_auth = 1;
		else if (!strcmp(argv[1], "--enumerate"))
			do_enumerate = 1;
		else if (!strcmp(argv[1], "--remove")) {
			do_remove = 1;
			if (argc > 2) {
				remove_fid = strtoul(argv[2], NULL, 0);
				argv++;
				argc--;
			}
		}
		else if (!strcmp(argv[1], "--capture")) {
			do_capture = 1;
			if (argc > 2 && argv[2][0] >= "0"[0] &&
			    argv[2][0] <= "9"[0]) {
				capture_secs = atoi(argv[2]);
				argv++;
				argc--;
			}
		}
		argv++;
		argc--;
	}

	if (argc > 2 && !strcmp(argv[1], "--power"))
		return hold_power(strtoul(argv[2], NULL, 0)) ? 1 : 0;

	if (getenv("GF_LOGDUMP")) {
		gf_logdump_level = strtoul(getenv("GF_LOGDUMP"), NULL, 0);
		printf("logdump level %u\n", gf_logdump_level);
	}

	if (argc > 2 && !strcmp(argv[1], "--supp")) {
		if (argc > 3)
			supp_ok_match = argv[3];
		return run_supplicant(strtoul(argv[2], NULL, 0)) ? 1 : 0;
	}

	if (argc > 2 && !strcmp(argv[1], "--load"))
		return load_app(argv[2]) ? 1 : 0;

	if (argc > 1)
		app = argv[1];

	fd = open_client();
	if (fd < 0)
		return 1;

	if (show_version(fd))
		return 1;

	session = open_session(fd, app);
	if (session < 0)
		return 1;

	/*
	 * Not by default. Sending a command to this application has reset the
	 * machine twice: once with a null payload address, and once with a
	 * valid one, so the request layout is not yet right. Pass --invoke
	 * when you are willing to spend a reboot on finding out.
	 */
	if (do_bringup || do_enroll || do_capture || do_remove ||
	    do_enumerate) {
		int gf = sensor_power_up();

		/*
		 * Enrolment only means anything after the sensor has been
		 * brought up, so it always follows the same sequence rather
		 * than being a mode of its own.
		 */
		if (!bringup(fd, session)) {
			if (do_capture)
				capture(fd, session, gf, capture_secs);
			else if (do_enumerate)
				enumerate(fd, session);
			else if (do_remove)
				remove_finger(fd, session, remove_fid);
			else if (do_enroll)
				enroll(fd, session);
		}

		if (gf >= 0)
			close(gf);
	}
	else if (do_invoke)
		invoke(fd, session, GF_CMD_GET_DEV_INFO, 5500);
	else
		printf("session opened; skipping invoke (pass --invoke)\n");

	return 0;
}
