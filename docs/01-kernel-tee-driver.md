# Layer 1: the kernel TEE driver

The QSEECOM TEE driver exposes Qualcomm's QSEE trusted applications through the
standard [TEE subsystem](https://docs.kernel.org/tee/tee.html). Source: branch [`qcom-qseecom-tee`](https://github.com/wrobelda/linux/tree/qcom-qseecom-tee) at [wrobelda/linux](https://github.com/wrobelda/linux)
(`drivers/tee/qseecom/`, `CONFIG_TEE_QSEECOM`).

**The driver is application-agnostic; this document is not entirely.** Nothing
in the driver knows anything about fingerprints. The devices, the ioctls, the
parameter layouts, the load path and the listener mechanism described here are
the interface to *any* QSEE application, and none of it changes when the
application does.

What changes is the concrete values plugged into that interface: a patch offset,
a buffer size, a command number. Those belong to
[`gfenu`](02-ta-protocol.md#the-application), and appear here as worked examples
because an interface is easier to follow with real numbers in it. A different
application drives the same driver in the same way, with its own numbers.

`gfenu` is also the only application the driver has so far been exercised
against, which is a statement about test coverage rather than about what the
driver supports.

## Why this driver

Linux already has QSEECOM support in `drivers/firmware/qcom/`, but only for
in-kernel users such as `uefisecapp`. There is no way for a userspace process to
open a session to a trusted application and invoke commands in it, and no way to
answer the file-I/O requests such an application raises while it works.

Without both, a fingerprint sensor of this kind is inert. The sensor captures
images and raises interrupts; every decision — image processing, template
construction, matching — happens inside the trusted application. The secure
world has no storage of its own, so what the application builds is sealed and
handed back to the normal world to keep, which is why it cannot even finish its
first initialisation unless something is answering its file I/O.

**Why not the QTEE driver.** The kernel already carries a Qualcomm TEE driver:
[`qcomtee`](https://docs.kernel.org/tee/qtee.html), documented as QTEE. It
exposes an interface called smcinvoke, and smcinvoke is the wrong interface for
this application — which needs spelling out, because "Qualcomm already has a
TEE driver" is the first objection anyone raises.

Qualcomm's secure world can be reached through two different interfaces, and
which one a system needs is decided by the applications in its firmware rather
than by the age of its SoC. QSEECOM is command-based: load an application by
name, send it a numbered command, service the listener requests it raises.
smcinvoke, which `qcomtee` serves, is object-based.

They are not two routes to the same thing. A trusted application is built
against one interface or the other, and one that answers commands over QSEECOM
is not reachable as an smcinvoke object. Platforms offering both are common —
smcinvoke has been available for years while applications continued to be built
against QSEECOM — so QTEE being present says nothing about whether the
applications in that platform's firmware can be reached through it. The SM8250
this was developed against is such a platform: it exposes both, and
[`gfenu`](02-ta-protocol.md#the-application) answers only on QSEECOM.

## What the rest of the kernel had to grow

The TEE driver is new code, but it does not stand alone. Two files already in
the tree hold pieces it needs, and neither had the shape required.

### Image assembly — `drivers/soc/qcom/mdt_loader.c`

`qcom_mdt_load()` answers the remoteproc question: take an mdt and its segments
and scatter each one to its `p_paddr` inside a carveout. QSEE's application
loader wants the opposite — the `.mdt` followed by the segment payloads in
program-header order, as one contiguous buffer described by an mdt length and a
total length.

For some images the program headers cannot be treated as a file layout at all.
In the Qualcomm-signed application here, segment 0 declares `p_offset` 0, which
overlaps the ELF and program headers being parsed, and two later segments
declare the same `p_offset` with different sizes
([[our device]](../README.md#how-we-know)). Concatenation in program-header
order is what the vendor's own loader does and what the secure world accepts.

So two functions were added rather than a second parser grown elsewhere:
`qcom_mdt_get_image_size()` to size the buffer and `qcom_mdt_read_image()` to
fill it, both reusing the existing split-image handling. No existing caller
changes behaviour.

`QCOM_MDT_LOADER` also gained a Kconfig prompt. It had none because every
existing user selects it. This driver cannot: selecting `QCOM_SCM`, which it
already depends on, would close a dependency cycle, so a prompt is the only way
left to ask for the loader.

### Loading and listeners — `drivers/firmware/qcom/qcom_scm.c`

The SCM layer already knew how to look up a loaded application and send it a
command, which is all an in-kernel client such as `uefisecapp` needs. Two things
were missing before an application could be driven at all.

**Loading.** `qcom_scm_qseecom_app_load()` hands TZ an MDT-described image at a
physically contiguous, suitably aligned address and gets back the application
id. TZ keeps the name it was loaded under, which is what a later lookup matches.

**Listener services.** An application asks the normal world to do work on its
behalf — [file I/O](03-listener-services.md) above all — by raising a listener
request, and it stays blocked until one is answered. That meant registering a
listener with its shared buffer, servicing what TZ reports, and answering with
one of `enum qcom_scm_qseecom_listener_status`.

### Unloading, which did not exist

TZ refuses to load an application that is already resident, and nothing released
one, so re-initialising meant a reboot. That matters more than it sounds: an
application asks the normal world for its stored state during a first
initialisation and never again, so anything that changes what it is served
cannot be re-tested without a fresh load.

The app manager's shutdown command turned out to sit at 2, between start at 1
and lookup at 3. With `gfenu` resident and a load returning `-EINVAL`, owner 50
/ service 1 / command 2 with the application id returned success and echoed the
id back, and the next load succeeded ([[our device]](../README.md#how-we-know)).

### Four defects on paths that were already there

Reached for the first time by driving an application from user space, rather
than introduced by it:

- the listener id arrives from the secure world as a `u64` and selects which
  registered service handles a request; it was being narrowed;
- `qcom_scm_qseecom_app_load()` documented that the image must come from a TZ
  memory pool, then passed `qcom_tzmem_to_phys()`'s result to TZ unexamined — it
  returns 0 outside a pool, so a wrong caller sent 0 as the image address
  instead of getting an error;
- giving up on a listener that would not settle left the application parked in
  TZ awaiting an answer that never came, which by this file's own reasoning
  makes every later QSEECOM call return `-EBUSY` until the device is power
  cycled;
- two error paths abandoned a request without reporting it, including one whose
  return value was dropped entirely, so a failure to deliver the give-up answer
  left exactly the wedge that answer exists to prevent.

One further fix is deliberately not in this series: two `WARN_ON()`s in
`qcom_scm_qseecom_call()` are unreachable from in-kernel callers but become
reachable from an ordinary invoke, where `panic_on_warn` turns them into a
denial of service. It is a standalone patch, since it stands on its own merits.

## Devices

| node | use |
|---|---|
| `/dev/tee0` | client: open a session against an application, invoke commands |
| `/dev/teepriv0` | privileged: register a listener, or load an application. The two are told apart by the first parameter — a VALUE holding a listener id, or a MEMREF holding an application name |

`TEE_IOC_VERSION` reports `impl_id = 5` (`TEE_IMPL_ID_QSEECOM`). Note that a
device may also expose `qcomtee` as `tee1` with `impl_id = 4`; as
[Why this driver](#why-this-driver) explains, that is a different interface and
`gfenu` does not answer on it.

## Opening a session

A session is opened by application **name**, not by UUID, because that is what
QSEE matches on. Pass the NUL-terminated name (e.g. `"gfenu"`) in a memref:

    TEE_IOC_OPEN_SESSION
      params[0]  MEMREF_INPUT   application name

This is the one place the driver departs from its siblings, and it is worth
knowing why. [`amdtee`](https://docs.kernel.org/tee/amd-tee.html) also has no UUID of its own, but it *renders* the UUID as
a firmware filename, so the GlobalPlatform-shaped argument still works there.
QSEE has nothing to render: the name is the identity. Qualcomm's own newer
driver, `qcomtee`, hit the same wall and went a different way again, passing
object references rather than overloading the session arguments. The driver
therefore does not claim `TEE_GEN_CAP_GP`, which is what makes reusing param 0
legitimate rather than a violation.

## Invoking a command

Four parameters. The request header and the response share one buffer, with the
response following the 128-byte request; the command payload is a separate
allocation.

| param | attr | meaning |
|---|---|---|
| 0 | `MEMREF_INPUT` | 128-byte request header, at buffer offset 0 |
| 1 | `MEMREF_OUTPUT` | 64-byte response, at buffer offset 128 |
| 2 | `VALUE_INPUT` | address-patch descriptor, described under this table |
| 3 | `MEMREF_INOUT` | the payload buffer |

**The address patch.** The request header has to contain the *physical* address
of the payload buffer. User space neither knows nor should supply one, so
parameter 2 instructs the kernel: "write the address of parameter N into the
request at offset X, Y bytes wide". The descriptor is generic; which offset and
width to name is part of the application's own protocol. For `gfenu` it is
offset 0, four bytes wide.

Getting it wrong is not a quiet failure. `gfenu` dereferences whatever sits at
request offset 0 as a physical address, so leaving it zero — by omitting the
descriptor, or naming the wrong offset — makes the secure world dereference
zero, and the machine resets. A client for a different application will have its
own equivalent, and should assume the same class of consequence.

**Buffer safety.** Buffers the secure world reads are copied through kernel-only
memory rather than being mapped to user space, so a client cannot alter them
while TrustZone (TZ from here on) is looking at them.

## Loading an application

    TEE_IOC_OPEN_SESSION  (privileged device)  →  qcom_scm_qseecom_app_load()

The session carries one parameter, a MEMREF holding the application name. The
image is not passed in: the driver fetches `<name>.mdt` and its `.bNN` segments
with `request_firmware()`, so the files have to be under the kernel's firmware
search path (`/lib/firmware`). Userspace picks *which* application, never
*what is in it*.

That rule is taken from `amdtee`, which does the same thing in
`copy_ta_binary()`: userspace names a TA, the kernel fetches the image with
`request_firmware()`, and no image bytes ever cross from user space. Taking the
assembled image as a parameter instead would put the choice of what runs at a
higher privilege level than the kernel into the hands of any process allowed to
open the device.

### What the files are

An MDT image is one ELF-shaped image split across several files, which is how
Qualcomm's tooling ships firmware.

**Program headers, briefly.** Nothing about this image involves Linux: Qualcomm's
tooling produces it, the secure world consumes it, and Linux only relays the
bytes. What matters here is the program-header table, whose entries each cover
one **segment** and give where its bytes are in the file (`p_offset`), how many
there are (`p_filesz`), where they load (`p_paddr`, `p_vaddr`), how much room
they need once loaded (`p_memsz`, which can exceed `p_filesz` for zero-filled
data), and what kind of segment it is (`p_flags`). Qualcomm puts extra bits in
`p_flags` to mark its own segment types, which is how the authentication hash is
told apart from ordinary payload.

"Program-header order" in this document means the order those entries appear in the
table, index 0 upward — which is also what the `.bNN` numbering follows, so
segment 3 is always `.b03`.

- **`<name>.mdt`** is the metadata: a 32-bit ELF header and its program-header
  table, followed by the hash segment used to authenticate the image. It is
  small.
- **`<name>.bNN`** is one file per program header, `NN` being that header's
  index in decimal from `00` upward. Each holds exactly that segment's payload,
  `p_filesz` bytes of it.

Splitting it this way lets a loader fetch the metadata, authenticate the image
and size its allocation before pulling in megabytes of payload.

Taking `gfenu` as the worked example: the `.mdt` is 7260 bytes and there are
eight segments, `.b00` to `.b07`. The ELF header is 52 bytes and the eight
program headers 32 bytes each, which is 308 — exactly segment 0's `p_filesz`.
The `.mdt` is therefore segment 0 followed by segment 1 (the 6952-byte hash),
and `.b00` and `.b01` duplicate those two. Every `.bNN` matches its `p_filesz`
exactly. Assembled, the image is 4993345 bytes.

### How they are assembled

The driver assembles the pieces into one contiguous, 4 MiB-aligned buffer of TZ
memory: the `.mdt` first, then each segment payload in program-header order —
*every* segment that has one, including the hash, and including the two the
`.mdt` already contains. The duplication is not an accident to be optimised
away; it is the layout the secure world authenticates, and dropping a segment
shifts everything after it and gets the image rejected.

The kernel already parses this format, for remote processors: `qcom_mdt_load()`
in `drivers/soc/qcom/mdt_loader.c` reads an mdt and its segments and places each
one at its `p_paddr` inside a carveout. That is the wrong shape here, where QSEE
wants the image as a single contiguous buffer rather than scattered to physical
addresses, so this uses `qcom_mdt_read_image()` from the same file, which
assembles instead of placing. TZ remembers the name it was loaded under, which is what a later lookup
matches.

### The application manager

Loading, unloading and resolving all go through one QSEE service, which the
driver reaches with SCM calls. Such a call names its secure-world endpoint by an
owner and a service number, here owner 50 and service 1, and the service's own
command number. This one has three commands ([[our device]](../README.md#how-we-know)):

| command | takes | returns | notes |
|---|---|---|---|
| 1 start | the assembled image, its `.mdt` length and total length | the id QSEE assigned | refuses an application that is already resident |
| 2 shutdown | an application id | the same id | what makes a resident application loadable again |
| 3 lookup | a name | the id | resolves only applications the boot chain loaded |

Command 3's limitation is why the driver keeps a registry of its own. QSEE
remembers the name an application was loaded under, but will not resolve one
this driver loaded — a lookup for it returns `-ENOENT` even while it is running
and answering commands — so the driver has to remember the name-to-id mapping
itself.

### Application lifetime

**Nothing owns a loaded application.** The registry entry is
reference-counted: resolving an application by name takes a reference, every
session holds one, and the last one released unloads it. There is no explicit
unload.

This too is `amdtee`'s design (`get_ta_refcount()`/`put_ta_refcount()` in
`amdtee/call.c`, with `handle_unload_ta()` returning `-EBUSY` while the count is
non-zero), and the reason to copy it is failure behaviour rather than elegance.
QSEE refuses to load an application that is already loaded, and only a reboot
clears one. If a session *owned* its application, every crash between load and
close would strand it. A reference dropped by the teardown path cannot be
skipped, so a process that is killed gives its reference back like any other.

Applications the boot chain loaded are not owned by this driver and are never
unloaded by it.

A second load of an application that is already resident is refused with
`EINVAL` by QSEE, not by the driver, so a client that may be racing another
should open a session by name rather than loading blind.

Getting a *fresh* load matters more than it sounds: an application asks the
normal world for its stored files only during a first initialisation, so
anything that changes what the file service serves it cannot be re-tested
without one. The sequence is to drop every session naming the application, start
the supplicant — the normal-world process that answers its listener requests,
described below — and load again.

## Listener services

A trusted application asks the normal world to do work for it — file I/O, above
all — by raising a listener request, and it **blocks until one is answered**.

Register a listener by opening a session on `/dev/teepriv0`:

    TEE_IOC_OPEN_SESSION
      params[0]  VALUE_INPUT    listener id
      params[1]  MEMREF_INOUT   shared buffer (size is listener-specific)

Then loop:

    TEE_IOC_SUPPL_RECV   →  arg.func   = the listener this request belongs to
                            params[0]  VALUE_INPUT   request identifier
                            params[1]  MEMREF_INOUT  the shared buffer,
                                                     now holding the request
    ... serve it, writing the reply into the same buffer ...
    TEE_IOC_SUPPL_SEND   →  arg.ret    = 0 for success, non-zero for failure
                            params[0]  VALUE_OUTPUT  the identifier, echoed back

**Echo the identifier back or the answer is refused** with `-ESTALE`. It is what
lets the driver tell an answer to the outstanding request from one belonging to
a request that already timed out and was replaced; applying a stale answer hands
one application the reply meant for another.

This mechanism is [`optee`](https://docs.kernel.org/tee/op-tee.html)'s. `optee_supp_recv()` hands the request id out to
user space in a meta value parameter and `supp_pop_req()` looks it up with
`idr_find()` on the id the supplicant sends back. A counter kept only inside the
kernel cannot do this, because the answer carries nothing to compare against.

Mind the attribute flip: `VALUE_INPUT` going out, `VALUE_OUTPUT` coming back.
Directions are named from the supplicant's point of view, and the TEE core only
transfers a value marked the matching way in each direction
(`params_to_supp()` and `params_from_supp()` in `tee_core.c`).

### One supplicant queue per device

**The driver keeps a single supplicant queue for the whole device, not one per
listener.** Two processes each registering a different listener will race on
`TEE_IOC_SUPPL_RECV` and steal each other's requests — one will be handed the
other's traffic, decode it with the wrong layout, and write its reply over a
buffer the secure world is about to read.

Serve every listener from **one process** and dispatch on `arg.func`. This is
not a limitation to work around; it is why `arg.func` exists.

### Failure is not an answer

`arg.ret` distinguishes success from failure at the transport level, and the
distinction matters: claiming success without having filled the buffer
leaves the application acting on whatever was in it, which has been observed to
hang the secure world until the watchdog fires.

Note this is separate from the *protocol-level* result. Both listener protocols
report their own errors inside the reply buffer and still return transport
success — see the [listener protocols](03-listener-services.md).

### A wedged supplicant blocks more than itself

Servicing a listener request happens with the QSEECOM call lock held, and every
QSEECOM caller shares that lock — including in-kernel ones, which is what
`qcom_qseecom_uefisecapp` is. A supplicant that stops answering therefore stalls
an EFI variable read for as long as the request takes to time out, and an
application that will not settle can repeat that for several rounds before the
driver gives up on it.

That is inherent to serialising a single secure-world interface rather than a
defect, and it is why the supplicant timeout is short. It is worth knowing
before granting anyone access to the privileged device: the blast radius of a
wedged supplicant is every QSEECOM user on the system, not only the client that
registered the listener.

### Teardown

The supplicant is torn down when its device file closes, not when the context is
released. A listener holds a reference to its buffer, and `tee_shm` holds a
reference to the context that allocated it, so releasing only on `->release()`
cannot break the cycle: the context leaks, `tee_device_unregister()` waits for it
forever, and the module is stranded in "going" state that only a reboot clears.

## Security

This driver exists to let user space talk to trusted applications, so it
*is* an attack surface — the normal-world end of the path that trusted-application
vulnerabilities are reached through. Two halves to that: what the driver
guarantees, and what it cannot.

**What the driver enforces**

- Buffers TZ reads are copied through kernel-only memory, so user space cannot
  change them after they have been validated and while the secure world is
  reading them.
- The address-patch descriptor is bounds-checked against the request length,
  with the length checked *before* it is subtracted from — a request shorter
  than the address width would otherwise wrap the comparison and let any
  offset through, which is a kernel write at an attacker-chosen offset.
- Patched buffers are bounds-checked against the shared memory they refer to,
  the patch width is restricted to 4 or 8 bytes, and a 4-byte field refuses an
  address that does not fit rather than truncating it.
- Application images and the per-invoke staging buffer are size-capped.

**What it cannot enforce, and is therefore policy**

The driver does not know any application's payload format, so it cannot
validate what a command means. Anyone who can open the client device and start
a session can send arbitrary commands to any loaded trusted application.

That surface is well travelled on Android, which ships the same interface:

| | |
|---|---|
| `CVE-2022-48334` | integer overflow in `drm_verify_keys` and a resulting buffer overflow, in the Widevine trusted application. A bug inside a TA, reachable by whoever can send it commands |
| `CVE-2019-14041` | buffer overrun while processing a listener's modified response, because the size is not checked when writing physical address information into the message buffer. That is the path this driver implements |
| `CVE-2019-14040` | use-after-free inside QSEE itself |
| `CVE-2021-1961` | buffer overflow because the offset length is not checked while updating a buffer value |

Two things follow. A trusted application is trusted by the hardware in ways the
kernel is not, so compromising one is a route *back into* the kernel rather
than a secure-world-only problem. And access to the device nodes is part of the
security boundary, not a packaging detail.


Treat the device nodes accordingly:

| node | guard |
|---|---|
| `/dev/teepriv0` | **`CAP_SYS_ADMIN` enforced by the driver** — it loads code into the secure world and registers listeners |
| `/dev/tee0` | file permissions and LSM policy only — give it to the one service that needs it |

The capability check is in the driver, so it holds regardless of what created
the node. The client device deliberately has no such check: a fingerprint
daemon has no business holding `CAP_SYS_ADMIN`, so guarding it is a matter of
file permissions plus whatever LSM policy the system uses. A permissive udev
rule there turns a trusted-application bug into local privilege escalation.
