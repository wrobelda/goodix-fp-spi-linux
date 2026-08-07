# Layer 5: writing a client

What a real implementation has to do, and where the reference client in
`harness/` is deliberately not one.

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
only for as long as nothing else on the system talks to QSEE. `harness/--supp`
is a prototype of the supplicant, not a component to lift into an `fprintd`
driver.

### Where it should live

Three things about that supplicant are unsettled, and they belong to whoever
writes it rather than to this project.

**Its home.** `optee_client`'s `tee-supplicant` is the same role and already
backs GlobalPlatform storage, so extending it would avoid carrying a separate
project. It does not work unmodified: it refuses any device whose `impl_id` is
not `TEE_IMPL_ID_OPTEE`, it dispatches on OP-TEE's own RPC command numbers
rather than [listener ids](03-listener-services.md#the-listener-map), and it has
no equivalent of the per-listener registration handshake. Whether upstream wants
a non-OP-TEE backend is theirs to say.

**Whether it loads applications as well as serving them.** The reference client
separates the two — `--supp` serves, `--load` loads, both on `/dev/teepriv0`,
which works only because the kernel allows
[one receiver rather than one opener](01-kernel-tee-driver.md#one-supplicant-queue-per-device).
A daemon could own loading, and there is an argument for it, since the
supplicant has to be up before any application starts. The alternative is that
loading stays with whoever wants the application and the
[reference count](01-kernel-tee-driver.md#application-lifetime) handles
lifetime.

**Whether the time listener reaches user space at all.** OP-TEE answers the
clock, i2c transfers and shared-memory allocation in the kernel and forwards
only the rest to `tee-supplicant`. By that division
[listener 11](03-listener-services.md#the-listener-map) is a kernel answer here
too, and only the file services need a process behind them.

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
| `verify` / `identify` | `SET_ACTIVE_GROUP`, `AUTHENTICATE` with the returned descriptor, pump `IRQ`, `AUTHENTICATE_FINISH` on a match |
| `delete` | `REMOVE` with the finger id. Android precedes it with `POST_ENROLL`, which ends an enrolment session; only needed if one may be open |
| `list` | **no mechanism** — see [Things that will bite](#things-that-will-bite) |

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

**The verdict does not identify the finger.** Interrupt payload +12 is zero or
`1064`, and nothing in it says which template matched. For `verify` (one
expected finger) that is sufficient. For `identify` it is not, and no command
has been found that reports the matched id — see `DUMP_TEMPLATE` under
[what storage cannot tell you](04-secure-storage.md#what-is-not-discoverable-from-storage).

**Enumerate does not tell you what is enrolled.** `ENUMERATE` (1015) exists and
succeeds once group context is set, but the descriptor it returns carries no
template ids ([[our device]](../README.md#how-we-know)), so while its `+100` tells you how many fingers
are enrolled, it cannot answer "which fingers do you have". Keep
your own index, and treat the object files as the source of truth for slot
allocation.

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
[what storage cannot tell you](04-secure-storage.md#what-is-not-discoverable-from-storage).
On everything we did exercise, libfprint's model of a print as data
the host holds does not apply: the host holds sealed ciphertext keyed to the
device. Enrolled prints cannot be migrated, backed up meaningfully, or matched
anywhere else.

## The authentication token

`ENROLL` carries an `hw_auth_token_t`; its layout, the evidence that it is
verified, and the unprovisioned-device loophole the reference client relies on
are all documented in
[the protocol document](02-ta-protocol.md#the-authentication-token).

A production `fprintd` driver should obtain a real token. The gatekeeper application is
`miriskm`, driven with plain `QSEECom_send_cmd` and CBOR payloads (Concise Binary Object Representation, RFC 8949):

    0x00021001  enroll   { 3: uid, 8: desired password }
                      -> { 6: password handle }
    0x00021002  verify   { 3: uid, 4: challenge, 6: handle, 9: password }
                      -> { 12: user_id, 14: authenticator_type,
                           15: timestamp, 16: hmac }
    0x00021003  ?        { 3: uid }   -> large, unidentified

Key 3 is the user id, key 4 the challenge. Keys 14 and 15 come back already
byte-swapped to match the packed token's mixed endianness — copy them straight
through rather than re-encoding. The handle is 58 bytes with the secure user id
at +1, little endian.

This is untested from Linux. Treat it as a starting point, not a specification.

## Reference client

`harness/gfharness.c` implements all of this except gatekeeper and lockout.
It is a single-file test client, not a library: it blocks, prints to stdout, and
assumes one device. Read it for the sequences and the file service; do not
copy its main loop.

    ./gfharness --supp 0 serve &         # both listeners, one process
    ./gfharness --load gfenu             # kernel fetches gfenu.mdt + .bNN
    ./gfharness --capture 300 --enroll   # the number is seconds
    ./gfharness --capture 120 --auth
    ./gfharness --remove <fid>
    ./gfharness --enumerate

`tracing/` holds the Frida scripts used to capture the Android stack, for anyone
who needs to check behaviour against it — see [its README](../tracing/README.md),
including the note on the command-channel hook truncating its dumps.
