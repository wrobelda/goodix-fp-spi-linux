# Layer 3: listener services

The application keeps its state in files it cannot reach itself. TrustZone has
no storage of its own on this platform, so every byte a trusted application
persists ends up in the normal world's filesystem.

It gets there through three layers, all of them inside the secure world and none
of them callable by a normal-world process. The names come from symbols in the
application image ([[gfenu symbols]](../README.md#how-we-know)):

- **`qsee_sfs_*`** — QSEE's secure file system, POSIX-shaped: `open`, `read`,
  `write`, `seek`, `close`, `getSize`, `mkdir`, `rm`, `rmdir`.
- **`qsee_fts_*`** — QSEE's sealing layer: `encrypt`, `decrypt`,
  `integrity_protect`, `integrity_verify`, and whole-file `read_file`,
  `write_file`, `remove_file`. Sealing is what makes a stored object ciphertext
  bound to the device it was sealed on.
- **`fts_*`**, unprefixed — Goodix's own wrappers. These are what the trusted
  application ([`gfenu`](02-ta-protocol.md#the-application)) calls; they call the two QSEE families named here. Taking
  them for Goodix's is an inference rather than something the symbol names say:
  the image carries a log message prefixed with the literal string `"[gf_fts]"`
  and a `GF_FTS_TEMPLATE_MAX_SIZE` constant, and both of those prefixes are
  Goodix's ([[gfenu symbols]](../README.md#how-we-know)).

Both `qsee_` families are Qualcomm's, as the prefix says. Only the unprefixed
`fts_*` layer is Goodix's. What the normal world ever sees is the downstream
effect of all this: a listener request asking for file I/O.

**Nothing the application persists works until these are served**, and it blocks
waiting for answers.

## The listener map

`qseecomd` — the Android userspace daemon Qualcomm ships to answer listener
requests, and the component this project replaces — registers ten listener
services ([[vendor trace]](../README.md#how-we-know)). Two of them are file services, and the Goodix
application uses both:

| id | service | carries |
|---|---|---|
| **10** | file system | plain, unsealed files: probes, and one real file |
| **28672** | GlobalPlatform file system (`gpfs`) | **all sealed-object reads, writes and deletes** |

The others we recorded, for completeness: 11 time, 4352 GlobalPlatform request
cancellation, 8192 RPMB, 12288 SSD, 16384 secure UI, 36864 interrupt, 45056
secure processor. That is seven, and `qseecomd` registers ten in total, so one
id in the middle went unrecorded.

These are QSEE services, not Goodix ones, and the names above are the service
names rather than anything we watched being used. A client for a different
trusted application will need whichever of them that application raises, and
will have to work out what each expects — nothing here documents them.

Not all of them need reach user space. OP-TEE splits its equivalent requests:
the kernel answers what it can itself — the clock, i2c transfers, shared-memory
allocation — and forwards only the rest to `tee-supplicant`
(`drivers/tee/optee/rpc.c`, and the note in `optee_rpc_cmd.h` that it defines
"only the commands handled by the kernel driver"). By that division listener 11,
time, is a kernel answer rather than a supplicant one, while the file services
are exactly the kind that has to go out to user space.

We have never observed the Goodix application raising a request on any of
them, across load, enrolment, authentication and removal
([[our device]](../README.md#how-we-know)). That is not a guarantee that it never
will — a path we have not exercised, or another firmware version, could. It is
one of the [open questions](../README.md#what-is-not-understood).

> Both must be served **from one process**, dispatching on `arg.func`. The
> kernel driver has [a single supplicant queue per
> device](01-kernel-tee-driver.md#one-supplicant-queue-per-device).
>
> That limit is per *device*, not per application, so it also settles where a
> listener service belongs: in one process serving the whole machine, which is
> what `tee-supplicant` is for OP-TEE and `qseecomd` for QSEE on Android. Two
> QSEE clients each running their own would consume each other's requests,
> whatever applications they drive.

## Why two file services

This is the single most important thing in this document. One service is not a
fallback for the other: they carry different things.

- **Listener 10 is a plain file service.** POSIX-shaped `open`, `read`, `write`,
  `close`, `stat`, and an errno query. It operates on ordinary files, with no
  sealing anywhere in the path.
- **Listener 28672 is the sealed-object store.** Everything crossing it is
  encrypted and integrity-protected with the device-bound key described in
  [secure storage](04-secure-storage.md).

The application's `fts_open()` always probes with `open(path, O_RDONLY)` —
flags literally zero — regardless of what it intends to do next. That probe is an
ordinary open, so it goes to **listener 10**. The sealed payload
then moves over **listener 28672**.

The consequence for a client is the part that bites: **no sealed object ever
crosses listener 10.** A client that implements only listener 10 will see
plausible traffic, answer it correctly, and still have every persistence
operation fail, because the template and calibration data never crosses that
listener at all.

Listener 10 is not write-free, though. It carries one genuinely unsealed file.
In a full
authentication run we counted 581 opens, 403 errno queries, 178 closes and 3
writes on listener 10, and every one of those writes was
`/persist/data/fingerprint_version` ([[our device]](../README.md#how-we-know)), the one
[unsealed file](04-secure-storage.md#the-file-set).

A removal, for example, looks like this:

    listener 10     op 0x202  open  "finger_0_1.so" flags=0   → fd
    listener 10     op 0x209  close fd                        → 0
    listener 28672  op 0x2    delete                          → 0

## Listener 10 — file system

Shared buffer 20480 bytes (`0x5000`). Opcodes run from `0x202`, with gaps —
`0x20d`, `0x215` and `0x218` have no entry — and with some operations reachable
through more than one: `stat` appears at `0x210`, `statfs` at `0x212`, `0x213`
and `0x217`, and `fstat` at both `0x20e` and `0x219`. Those repetitions are what
the jump table contains; we exercised only a few of the opcodes, so whether the
duplicates behave identically is untested.

This table is read out of `libdrmfs.so`'s jump table
([[libdrmfs.so disassembly]](../README.md#how-we-know)), not inferred from
traffic. Inference is not reliable here: observed requests alone suggest `0x208`
is `stat`, when it is `write`. Only the opcodes actually exercised are confirmed
by traffic ([[our device]](../README.md#how-we-know)).

| op | operation | op | operation |
|---|---|---|---|
| `0x202` | open | `0x210` | stat |
| `0x203` | openat | `0x211` | mkdir |
| `0x204` | unlinkat | `0x212`–`0x213` | statfs |
| `0x205` | open + fcntl | `0x214` | rename |
| `0x206` | creat | `0x216` | fsync |
| `0x207` | read | `0x217` | statfs |
| `0x208` | write | `0x219` | fstat |
| `0x209` | close | `0x21a` | readdir |
| `0x20a` | lseek | `0x21b` | closedir |
| `0x20b` | link | `0x21c` | **get last errno** |
| `0x20c` | unlink | `0x21d` | shutdown |
| `0x20e` | fstat | | |
| `0x20f` | lstat | | |

**Request**

    +0      u32   operation
    +4      char  path[252]        path-taking ops
    +260    u32   open flags       open; bit 6 is O_CREAT

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

Eight bytes for every op except read, which is 20008.

Three things a client must get right:

- **Return a real descriptor from an open.** Everything after an open addresses
  it. Leaving the reply untouched hands the application whatever was in the
  buffer as its fd — in practice the first four bytes of the previous request's
  path.
- **On failure the result word is the raw libc return, `-1`** — not a negative
  errno. The errno travels separately.
- **Implement `0x21c`.** This is not a corner case: it was the second-busiest
  operation on this listener in our runs, 403 calls against 581 opens
  ([[our device]](../README.md#how-we-know)). The application asks it after *every* failed open, and
  the answer decides what it does next: `ENOENT` means "not there yet, create
  it", anything else means something is broken and it gives up. Answering
  `EINVAL` turns a routine absent file into an aborted enrolment.

Failure is reported in the result word; the request itself always succeeds at
the transport level.

## Listener 28672 — GlobalPlatform file system

Shared buffer 516096 bytes (`0x7e000`). Thirteen opcodes, where the opcode
encodes both the operation and which base directory to resolve against:

    op % 4 == 0   read          op / 4 == 0   resolve automatically
    op % 4 == 1   write         op / 4 == 1   force the data path
    op % 4 == 2   delete        op / 4 == 2   force the persist path
    op % 4 == 3   rename
    op == 12      returns a constant; never observed in use

In practice the Goodix application uses `0` (read), `1` (write) and `2`
(delete). Persisting an object is delete-then-write.

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

Read replies are 512012 bytes (12-byte header plus 512000 of data, and the read
length is clamped to 512000). Writes reply 12 bytes, deletes and renames 8.

Notes:

- **Names are paths, not descriptors.** There is no handle concept; every
  operation re-resolves the name.
- **There is no errno opcode.** Unlike listener 10, failures are reported inline
  in the result word.
- The Android implementation copies an existing object to `<name>.bak` before
  overwriting it when the backup flag is set.
- The vendor resolves names against `/data/vendor/tzstorage/` or
  `/mnt/vendor/persist/data/` depending on prefix; because the application sends
  absolute Android paths that are special-cased through as-is, a Linux client
  can simply root them all under one directory.
