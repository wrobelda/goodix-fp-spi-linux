# Layer 5: writing a client

This document records the investigation and decisions used to implement the
Linux client stack. See the [production client stack](08-production-client-stack.md)
for components and deployment, and the
[reference test client](07-reference-client.md) for command-line hardware and
protocol testing.

## Shape

What plugs into `fprintd` is a libfprint device, which libfprint calls a driver.
Image capture, template construction and matching all happen inside the trusted
application, so what you are writing is a protocol client plus a file server.

Three concurrent concerns:

1. **A listener thread** serving both
   [listener services](03-listener-services.md) from one process. It must be
   running before the trusted application
   ([`gfenu`](02-ta-protocol.md#the-application)) is loaded and stay up
   for the lifetime of the device, because the application blocks on it.
2. **A netlink listener** for sensor interrupts, from the
   [sensor driver](00-sensor-driver.md).
3. **The [command path](02-ta-protocol.md)** — enrol, verify, delete.

The listener service is the part with no libfprint analogue, and it is not
really yours. The TEE subsystem already has a name and a shape for this role:
`tee-supplicant`, the process that answers a TEE's requests on behalf of every
trusted application on the machine. OP-TEE ships one in `optee_client`, and
`qseecomd` is what Android runs for QSEE.

The kernel permits
[one receiver per device](01-kernel-tee-driver.md#one-supplicant-queue-per-device),
so a second QSEE client, handling some other type of application, could not
register a handler of its own. Building a file service into the client works
only for as long as nothing else on the system talks to QSEE. The harness
listener mode is a hardware test facility, not a component of the fprintd
driver.

### Supplicant design decision

The implementation resolves the following design questions.

**Code and ownership.** Qualcomm publishes the closest implementation in
[`qualcomm/minkipc`](https://github.com/qualcomm/minkipc). Its
[`qtee_supplicant/src/listener_mngr.c`](https://github.com/qualcomm/minkipc/blob/main/qtee_supplicant/src/listener_mngr.c)
loads separate FS, GPFS, time and RPMB service libraries, and those libraries
speak the same Qualcomm listener protocols documented here. In the filesystem
library,
[`fs_main.c`](https://github.com/qualcomm/minkipc/blob/main/listeners/libfsservice/fs/fs_main.c)
is the useful dividing line: `init()` allocates the buffer and registers
listener 10 through `CListenerCBO_new()` and
`IRegisterListenerCBO_register()`, while `smci_dispatch()` accepts that buffer
and dispatches the filesystem commands ([[Qualcomm minkipc]](../README.md#how-we-know)).

It does not work unmodified with this driver. MinkIPC registration creates
QTEE callback objects; a legacy-QSEECOM supplicant instead opens
`/dev/teeprivN`, registers each numeric listener and receives all of their
requests through one `TEE_IOC_SUPPL_RECV` queue. The reusable part is therefore
the protocol definitions and operation handlers below each library's
Mink-specific `init()`, with one QSEECOM receive loop dispatching by listener id.
The production implementation is a standalone daemon with a legacy-QSEECOM
transport and transport-independent service dispatch. It uses MinkIPC as the
protocol and dispatch reference without depending on its QTEE registration
transport.

`optee_client`'s `tee-supplicant` remains the precedent for the process's role
and lifecycle, but is a less direct code match. It refuses a device whose
`impl_id` is not `TEE_IMPL_ID_OPTEE` and dispatches OP-TEE RPC commands rather
than Qualcomm listener protocols. Reusing it would first require a
non-OP-TEE-backend architecture of its own.

**Application loading.** Listener service and TA loading use separate
processes. `qsee-supplicant` owns the request queue. `qsee-app-loader` loads the
configured TA and holds its load reference. This allows the supplicant to
restart and re-register listeners without unloading a resident TA.

**Listener scope.** OP-TEE answers the
clock, i2c transfers and shared-memory allocation in the kernel and forwards
only the rest to `tee-supplicant`. By that division
[listener 11](03-listener-services.md#the-listener-map) is a kernel answer here
too. The production QSEECOM transport registers the filesystem and GPFS
listeners required by the tested TA.

## Ordering that is not optional

- The file service must be registered **before** the application is loaded; the
  application asks for its stored objects during a first initialisation and
  never again.
- The application is **resident while anything refers to it** — hold a session
  open for as long as you want it loaded, and open by name rather than loading
  blind if something else may already have loaded it. The details, including why
  a crash cannot strand it, are under
  [application lifetime](01-kernel-tee-driver.md#application-lifetime).
- The sensor's char device must be held open for the regulator and
  reset line to stay up, and the bring-up ioctls must run under that same open
  — see the [bring-up sequence](00-sensor-driver.md#interfaces).

## Mapping to libfprint operations

| libfprint | sequence |
|---|---|
| `open` | load if needed, `DETECT_SENSOR`, `INIT`, `INIT_FINISHED` |
| `enroll` | `PRE_ENROLL`, `ENROLL`, pump `IRQ`, then `SAVE`, `SET_ACTIVE_GROUP`, `GET_AUTH_ID` |
| `verify` / `identify` | `SET_ACTIVE_GROUP` with the group at payload +100, `AUTHENTICATE`, pump `IRQ`, then 124-byte `AUTHENTICATE_FINISH` carrying the matched group/finger on a match |
| `delete` | `REMOVE` with the finger id. Android precedes it with `POST_ENROLL`, which ends an enrolment session; only needed if one may be open |
| `list` | `SET_ACTIVE_GROUP`, then 184-byte `ENUMERATE`; reconcile returned group/finger IDs with fprintd records |

Enrolment progress for the UI comes from the interrupt payload: samples
remaining at +327404, alongside the group and finger id — see
[the enrolment fields](02-ta-protocol.md#interrupts). The application does not
report how many it wanted to begin with, so take the first value it reports as
the total and count down from there.

## Things that will bite

**Arm on demand, cancel on dismissal.** `AUTHENTICATE` is one-shot. Arm it
whenever the device expects a fingerprint — with the screen off if unlock on
touch is configured, or while an authentication dialog is up — and cancel when it does
not. The reference client re-arms after every verdict, which is right for a test
and wrong for a daemon — it leaves the sensor armed indefinitely.

**Implement lockout yourself.** This is one of the
[open questions](../README.md#what-is-not-understood). `GF_CMD_LOCKOUT` exists,
but we never saw the
Android HAL send it, including on a device driven to a real lockout
([[vendor trace]](../README.md#how-we-know)), and we saw nothing suggesting the application rate-limits
attempts on its own ([[our device]](../README.md#how-we-know)).

That policy lives above the driver on both systems: `system_server` on Android,
`fprintd` and the PAM stack on Linux. The point for a `fprintd` driver author is that
nothing *below* your code appears to be counting failures, so do not report a
mismatch in a way that invites unlimited retries, and do not assume the
application will stop them. "We did not observe an internal limit" is weaker
than "there is none".

**Use the full IRQ result for identify.** The compact verdict at payload +12 is
only zero or `1064`. The matched group and finger are at `+0x4fce4` and
`+0x4fce8`; copy them into `AUTHENTICATE_FINISH` and use the finger id as the
identify result. This is separate from discovering the enrolled set before a
match; see the [application index versus
storage](04-secure-storage.md#application-index-versus-storage).

**Give enumerate its full buffer.** After `SET_ACTIVE_GROUP`, `ENUMERATE`
(1015) takes 184 bytes. Its response contains the count at `+100`, group ids at
`+104`, and finger ids at `+144`, with ten entries available in each array
([[our device]](../README.md#how-we-know)). A SAVE-sized 112-byte buffer appears
to return only a count because it cuts off the finger-id array.

**Keep non-zero IRQ statuses.** On an image interrupt, the command status is
per-sample feedback and is mirrored at payload `+12`. Qualcomm's
`libgf_hal.so` maps selected Goodix errors to Android's partial, imager-dirty,
too-slow, and vendor acquired-info values and continues draining the interrupt.
A live partial press produced `1011`, left the enrolment count unchanged, and
was mapped by the HAL to acquired-info partial. Treat transport failure as
fatal, but report these TA statuses as retry/quality feedback rather than
collapsing them into a generic operation failure.

**Calibration is generated, not shipped.** The application writes its own
baseline during `INIT` on a device that has none and maintains it during
ordinary operation, so there is nothing to import — and serving a copied one
hides whether generation works. Start with the storage directory empty; see
[calibration is generated](04-secure-storage.md#calibration-is-generated).

**Mind the two device nodes.** `/dev/teepriv0` requires `CAP_SYS_ADMIN`, which
the kernel TEE driver enforces — so whatever loads the application and runs the file
service needs it, and that argues for keeping the loader separate from the
daemon that does the matching.

`/dev/tee0` has no such check by design, because a fingerprint daemon should not
hold `CAP_SYS_ADMIN`. But anyone who can open it can send arbitrary commands to
any loaded trusted application, and the kernel cannot validate those payloads —
it does not know their formats. Restrict it to the one service that needs it and
label it in LSM policy rather than inheriting a permissive default. See
[what the kernel driver can and cannot enforce](01-kernel-tee-driver.md#security).

**Templates are not exposed by any command we found.** The unexplored
`DUMP_TEMPLATE` could change this answer — see
[the sealed file set](04-secure-storage.md#the-file-set).
On everything we did exercise, libfprint's model of a print as data
the host holds does not apply: the host holds sealed ciphertext keyed to the
device. Enrolled prints cannot be migrated, backed up meaningfully, or matched
anywhere else.

## The authentication token

`ENROLL` carries an `hw_auth_token_t`; its layout, the evidence that it is
verified, and the challenge-only token the reference client relies on are all
documented in
[the protocol document](02-ta-protocol.md#the-authentication-token).

The tested TA accepts a token containing the fresh `PRE_ENROLL` challenge with
the other fields zero, both before and after provisioning an Android
lock-screen credential. This makes the reference lifecycle usable on this
device, with authorization to enrol enforced above the fingerprint protocol. A
production client still cannot assume another model or firmware offers the
same shortcut.

Android uses a signed token, which preserves an authorization boundary in
secure world rather than relying only on access control around the Linux
fingerprint service. A production design should use that stronger path where a
credential service is available; the challenge-only behaviour is a property of
this tested TA, not a general protocol guarantee.

The fingerprint client should not implement password storage or accept a PIN
only to send it directly to Keymaster. It should ask a separate credential
service to verify a credential for the `PRE_ENROLL` challenge and return the
completed 69-byte token. The observed `keymaster64` transport, Android's
synthetic-password derivation, and the remaining work for a native Linux
Gatekeeper service are documented in
[the Gatekeeper protocol](06-Gatekeeper-protocol.md).

## Reference test client

See the [reference test client](07-reference-client.md) for the harness build,
commands, listener test mode, safety requirements, and links to the tracing
tools.
