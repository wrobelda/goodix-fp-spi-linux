# Layer 3: listener services

The application keeps its state in files that it cannot reach itself.
TrustZone has no storage of its own on this platform, so every byte a trusted
application persists ends up in the normal world's filesystem.

The data gets there through three layers. All three layers are inside the
secure world, and none of them can be called by a normal-world process. The
names come from symbols in the application image
([[gfenu symbols]](../README.md#how-we-know)):

- **`qsee_sfs_*`** — QSEE's secure file system, POSIX-shaped: `open`, `read`,
  `write`, `seek`, `close`, `getSize`, `mkdir`, `rm`, `rmdir`.
- **`qsee_fts_*`** — QSEE's sealing layer: `encrypt`, `decrypt`,
  `integrity_protect`, `integrity_verify`, and whole-file `read_file`,
  `write_file`, `remove_file`. Sealing turns a stored object into ciphertext
  that is bound to the device it was sealed on.
- **`fts_*`**, unprefixed — Goodix's own wrappers. These are what the trusted
  application ([`gfenu`](02-ta-protocol.md#the-application)) calls, and they
  in turn call the two QSEE families named here. The symbol names do not say
  that this layer is Goodix's; that is an inference. The image carries a log
  message prefixed with the literal string `"[gf_fts]"` and a
  `GF_FTS_TEMPLATE_MAX_SIZE` constant, and both of those prefixes are
  Goodix's ([[gfenu symbols]](../README.md#how-we-know)).

Both `qsee_` families are Qualcomm's, as the prefix says. Only the unprefixed
`fts_*` layer is Goodix's. The normal world only ever sees the downstream
effect of all this: a listener request asking for file I/O.

**Nothing the application persists works until these requests are served.**
The application blocks while it waits for answers.

## The listener map

`qseecomd` is the Android userspace daemon that Qualcomm ships to answer
listener requests; it is also the component this project replaces. It
registers ten listener services ([[vendor trace]](../README.md#how-we-know)).
Two of them are file services, and the Goodix application uses both:

| id | service | carries |
|---|---|---|
| **10** | file system | plain, unsealed files: probes, and one real file |
| **28672** | GlobalPlatform file system (`gpfs`) | **all sealed-object reads, writes and deletes** |

For completeness, these are the other listeners we recorded:

- 11 — time
- 4352 — GlobalPlatform request cancellation
- 8192 — RPMB
- 12288 — SSD
- 16384 — secure UI
- 36864 — interrupt
- 45056 — secure processor

That is seven, and `qseecomd` registers ten in total, so one id in the middle
went unrecorded.

Qualcomm now publishes QTEE implementations of four services from that same
map in [`qualcomm/minkipc`](https://github.com/qualcomm/minkipc/tree/main/listeners):

- filesystem 10
- time 11
- RPMB 8192
- GPFS 28672

The transports differ: those libraries register QTEE callback objects through
MinkIPC, while the Android binary observed here registers QSEECOM listeners.
The service ids and buffer protocols are Qualcomm's in both cases
([[Qualcomm minkipc]](../README.md#how-we-know)).

These are QSEE services, not Goodix ones. The names above are the service
names, not something we watched being used. A client for a different trusted
application will need whichever of these services that application raises.
That client will also have to work out what each service expects, because
nothing here documents them.

Not all of these services need to reach user space. OP-TEE splits its
equivalent requests: the kernel answers what it can itself — the clock, i2c
transfers, shared-memory allocation — and forwards only the rest to
`tee-supplicant` (`drivers/tee/optee/rpc.c`, and the note in
`optee_rpc_cmd.h` that it defines "only the commands handled by the kernel
driver"). By that division, listener 11 (time) is a kernel answer rather than
a supplicant one, while the file services are exactly the kind that has to go
out to user space.

We have never observed the Goodix application raising a request on any of
them, across load, enrolment, authentication and removal
([[our device]](../README.md#how-we-know)). That is not a guarantee that it
never will: a path we have not exercised, or another firmware version, could.
It is one of the [open questions](../README.md#what-is-not-understood).

> Both must be served **from one process**, dispatching on `arg.func`. The
> kernel driver has [a single supplicant queue per
> device](01-kernel-tee-driver.md#one-supplicant-queue-per-device).
>
> That limit is per *device*, not per application, so it also settles where a
> listener service belongs: in one process serving the whole machine. That is
> what `tee-supplicant` is for OP-TEE, and what `qseecomd` is for QSEE on
> Android. Two QSEE clients that each run their own listener service would
> consume each other's requests, whatever applications they drive.

## Why two file services

This is the most important point in this document. One service is not a
fallback for the other: they carry different things.

- **Listener 10 is a plain file service.** It offers POSIX-shaped `open`,
  `read`, `write`, `close`, `stat`, and an errno query. It operates on
  ordinary files, with no sealing anywhere in the path.
- **Listener 28672 is the sealed-object store.** Everything crossing it is
  encrypted and integrity-protected with the device-bound key described in
  [secure storage](04-secure-storage.md).

The application's `fts_open()` always probes with `open(path, O_RDONLY)`,
with flags literally zero, regardless of what it intends to do next. That
probe is an ordinary open, so it goes to **listener 10**. The sealed payload
then moves over **listener 28672**.

The consequence for a client is critical: **no sealed object ever crosses
listener 10.** A client that implements only listener 10 will see plausible
traffic and answer it correctly. Even so, every persistence operation will
fail, because the template and calibration data never crosses that listener
at all.

Listener 10 is not write-free, though: it carries one genuinely unsealed
file. In a full authentication run we counted this traffic on listener 10
([[our device]](../README.md#how-we-know)):

- 581 opens
- 403 errno queries
- 178 closes
- 3 writes

Every one of those writes was `/persist/data/fingerprint_version`, the one
[unsealed file](04-secure-storage.md#the-file-set).

A removal, for example, looks like this:

    listener 10     op 0x202  open  "finger_0_1.so" flags=0   → fd
    listener 10     op 0x209  close fd                        → 0
    listener 28672  op 0x2    delete                          → 0

## Listener 10 — file system

The shared buffer is 20480 bytes (`0x5000`). Opcodes run densely from `0x202`
through `0x21d`, with `0x215` deliberately unused.

The table below comes from the `dispatch()` jump table in the reference
device's stock `/vendor/lib64/libdrmfs.so` (build id
`9af65349d5b69a2a08633e83772e6b6f`, SHA-256
`f459fffb147cf37654ea6cebf732b8ddffb93bcfa76450d3cdec1dbfabe99bbc`). That
library's embedded mini debug information retains the names `dispatch`,
`file_open`, `file_remove` and `file_rename`
([[libdrmfs.so disassembly]](../README.md#how-we-know)).

Qualcomm's
[`minkipc/listeners/libfsservice/fs/fs_msg.h`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/fs/fs_msg.h)
defines the same `FILE_SERVICE_ID` (`0xa`), `FILE_SERVICE_BUF_LEN` (`20 *
1024`) and `TZ_FS_MSG_CMD_*` sequence. Its
[`fs_main.c`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/fs/fs_main.c)
dispatches those commands, and
[`fs.c`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/fs/fs.c)
implements the corresponding normal-world operations
([[Qualcomm minkipc]](../README.md#how-we-know)). Only the opcodes actually
exercised by the trusted application are additionally confirmed by live
traffic ([[our device]](../README.md#how-we-know)).

| op | operation | op | operation |
|---|---|---|---|
| `0x202` | open | `0x210` | mkdir |
| `0x203` | openat | `0x211` | test directory |
| `0x204` | unlinkat | `0x212` | tell directory |
| `0x205` | fcntl | `0x213` | remove |
| `0x206` | creat | `0x214` | chown/chmod helper |
| `0x207` | read | `0x215` | unused |
| `0x208` | write | `0x216` | fsync |
| `0x209` | close | `0x217` | rename |
| `0x20a` | lseek | `0x218` | partition free size |
| `0x20b` | link | `0x219` | opendir |
| `0x20c` | unlink | `0x21a` | readdir |
| `0x20d` | rmdir | `0x21b` | closedir |
| `0x20e` | fstat | `0x21c` | **get last errno** |
| `0x20f` | lstat | `0x21d` | shutdown |

**Request**

    +0      u32   operation
    +4      char  path[256]        path-taking ops
    +260    u32   open flags       open; bit 6 is O_CREAT
    +260    u32   mode             creat / mkdir
    +260    char  new_path[256]    rename; the old path is at +4

    +4      s32   fd               descriptor-based ops
    +8      u32   count            read
    +8            data             write
    +8      s32   offset           lseek
    +12     u32   whence           lseek
    +20008  u32   count            write

**Reply**, written over the head of the same buffer:

    +0      u32   the operation, echoed
    +4      s32   result — fd for open, byte count for read/write, 0, or -1
    +4            data             read
    +20004  s32   bytes read       read

Of the operations exercised here, replies are eight bytes, except for read,
which is 20008. The file-status and directory operations have their own
packed reply structures, defined concretely in Qualcomm's `fs_msg.h`. `gfenu`
has not needed them, and the reference client deliberately does not guess at
them.

Three things a client must get right:

- **Return a real descriptor from an open.** Everything after an open
  addresses it. If a client leaves the reply untouched, the application takes
  whatever was in the buffer as its fd; in practice that is the first four
  bytes of the previous request's path.
- **On failure the result word is the raw libc return, `-1`** — not a negative
  errno. The errno travels separately.
- **Implement `0x21c`.** This is not a corner case: it was the second-busiest
  operation on this listener in our runs, with 403 calls against 581 opens
  ([[our device]](../README.md#how-we-know)). The application asks it after
  *every* failed open, and the answer decides what the application does next.
  `ENOENT` means the file is not there yet, so the application creates it;
  anything else means something is broken, and the application gives up.
  Answering `EINVAL` turns a routine absent file into an aborted enrolment.

Failure is reported in the result word; the request itself always succeeds at
the transport level.

## Listener 28672 — GlobalPlatform file system

The shared buffer is 516096 bytes (`0x7e000`). There are thirteen opcodes;
each opcode encodes both the operation and which base directory to resolve
against:

    op % 4 == 0   read          op / 4 == 0   resolve automatically
    op % 4 == 1   write         op / 4 == 1   force the data path
    op % 4 == 2   delete        op / 4 == 2   force the persist path
    op % 4 == 3   rename
    op == 12      GPFS version query; returns version 2

In practice the Goodix application uses `0` (read), `1` (write) and `2`
(delete). Persisting an object is delete-then-write.

Qualcomm's
[`minkipc/listeners/libfsservice/gpfs/gpfs_msg.h`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/gpfs/gpfs_msg.h)
defines the same `GPFILE_SERVICE_ID` (`0x7000`) and `GPFILE_SERVICE_BUF_LEN`
(`504 * 1024`). It also names these operations:

- operations 4–7 are the data-directory variants
- operations 8–11 are the persist-directory variants
- operation 12 is `TZ_GPFS_MSG_CMD_GPFS_VERSION`

[`gpfs_main.c`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/gpfs/gpfs_main.c)
dispatches them, and
[`gpfs.c`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/gpfs/gpfs.c)
returns GPFS version 2 ([[Qualcomm minkipc]](../README.md#how-we-know)). This
confirms the operation grouping recovered from the stock `libdrmfs.so`;
operations 0–3 are its automatic-resolution group.

A fresh native-Linux `INIT` on the reference device successfully used
operation 0 to read calibration objects, then operations 2 and 1 to delete
and rewrite them. That confirms the automatic-resolution read, delete and
write meanings, and it confirms the field offsets below
([[our device]](../README.md#how-we-know)).

**Request**

    +0      u32   operation
    +4      char  name[256]      NUL-terminated; the *old* name for a rename
    +260    s32   offset         read / write
    +260    char  name[256]      rename only: the new name
    +264    u32   length         bytes wanted (read) or supplied (write)
    +268    u32   backup         write: also keep a `<name>.bak` copy
    +272          data           write payload

**Reply**

    +0      u32   the operation, echoed
    +4      s32   0, or errno
    +8      u32   bytes read or written
    +12           data           read only, up to 512000 bytes

Read replies are 512012 bytes: a 12-byte header plus 512000 bytes of data,
and the read length is clamped to 512000. Write replies are 12 bytes; delete
and rename replies are 8 bytes.

Notes:

- **Names are paths, not descriptors.** There is no handle concept; every
  operation re-resolves the name.
- **There is no errno opcode.** Unlike listener 10, failures are reported
  inline in the result word.
- When the backup flag is set, the Android implementation copies an existing
  object to `<name>.bak` before overwriting it.
- The vendor resolves names against `/data/vendor/tzstorage/` or
  `/mnt/vendor/persist/data/`, depending on the prefix. The application sends
  absolute Android paths, and those are special-cased through as-is, so a
  Linux client can simply root them all under one directory.
