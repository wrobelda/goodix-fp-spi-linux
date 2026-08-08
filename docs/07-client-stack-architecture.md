# Linux client stack architecture

This document describes the production userspace split.  The protocol oracle in
`harness/gfharness.c` remains a hardware test program and is not a library used
by either component.

## Components and trust boundaries

### Machine-wide TEE supplicant

One machine-wide process owns each configured TEE supplicant transport.  Its
transport vtable discovers a TEE, registers services, receives a request, and
sends a reply.  A separate service registry maps transport-neutral service
identities to protocol dispatch objects.  The first transport owns the legacy
QSEECOM queue on `/dev/teeprivN`; its first services are Qualcomm listener 10
(FS) and listener 28672 (GPFS).

This follows MinkIPC's useful split between listener registration, a
callback/dispatch object, and service implementations without making MinkIPC
the daemon's permanent outer architecture.  QTEE object registration in
MinkIPC cannot implement the initial transport: this kernel exposes legacy
QSEECOM `TEE_IOC_SUPPL_RECV`/`TEE_IOC_SUPPL_SEND`.  A newer Qualcomm QTEE/Mink
transport or an OP-TEE transport can later implement the same lifecycle vtable
and publish its own service set.  Services are capabilities selected by
configuration, not fingerprint-specific assumptions.

The daemon is application agnostic.  It does not load `gfenu`, open the sensor,
or implement fingerprint policy.  The generic, separately supervised
`qsee-app-loader@.service` may load a configured TA only after the supplicant is
ready and hold its load session independently.

All protocol paths are untrusted input.  They are resolved beneath a configured
state directory and Android absolute paths are treated as names within that
directory, not host paths.  Path traversal, empty components where forbidden,
embedded-NUL ambiguity, symlink traversal, and non-regular GPFS objects are
rejected.  The daemon keeps directory descriptors rather than changing its
working directory.

GPFS writes are serialized.  A complete offset-zero write is staged in the
destination directory, synced, optionally backed up, atomically renamed, and
followed by a directory sync.  Partial offset writes are serialized and synced
but cannot be made atomically without changing the listener ABI.  Objects and
directories default to modes 0600 and 0700.  The service never creates empty
placeholder objects.

### libfprint Goodix drivers

The initial `goodix-gfenu` driver is specifically a TEE match-on-chip driver:
imaging, templating, and matching are delegated to the Goodix trusted
application.  A Goodix sensor for which normal world has direct SPI or a
complete kernel imaging interface belongs in a separate REE/image driver,
selected by its own device table and kernel ABI.  There is no runtime
TEE-versus-REE backend switch inside `goodix-gfenu`.

The bus is therefore not the backend.  As libfprint's Unsupported Devices wiki
notes for SPI sensors, many Android devices restrict direct SPI access to secure
world.  Such a device may still have a normal-world kernel driver for power,
reset, and IRQ delivery, but it must bind an appropriate TEE driver.  A
separate REE driver must bind only when the kernel ABI actually permits sensor
transactions (or provides a complete imaging interface).  ACPI or device-tree
compatibility with the same Goodix family is not sufficient evidence by itself.

The `gfenu` driver is a normal-world protocol client.  It owns, for one
open device lifetime:

* `/dev/goodix_fp`, including power/reset/IRQ setup;
* a QSEECOM client session to the TA named by the kernel `firmware_name` sysfs
  attribute, which is populated from the DT `firmware-name` property;
* the netlink IRQ subscription and IRQ-drain state.

The TA name is mandatory runtime device data.  `gfenu` is the value on the
reference platform, not the libfprint driver name or a fallback assumed for other
devices.

Sensor power/reset/IRQ operations live behind a sensor-I/O interface rather
than in the `gfenu` command codec.  Likewise, TA session/invoke operations live
behind a TEE-client interface.  QSEECOM is the first implementation.  A Goodix
TA hosted by MediaTek's TEE or OP-TEE should be a separate libfprint driver when
its TA or sensor ABI differs; it may reuse operation mapping and codec code only
where compatibility is established.  Similar hardware branding is not treated
as protocol compatibility.

The driver does not register listeners and does not need `/dev/teeprivN`.  Startup
ordering makes the external supplicant a prerequisite.  Access to the sensor
and TA is serialized by libfprint's action model and an internal operation
state; cancellation sends `CANCEL` before completing the cancelled action.

The driver is match-on-chip.  A serialized `FpPrint` contains only a versioned
locator:

```
protocol profile = 1, group id, Goodix finger id
```

The runtime TA name is part of the device identity, not each print locator.  A
profile number identifies the positively matched command/layout ABI.  The
record contains no image or portable template.  The configured state directory is
a self-contained set of sealed fingerprint objects.  `gfenu` reconstructs its
enrollment list from those files and exposes it through `ENUMERATE`; the TA has
no separate persistent enrollment database.  fprintd metadata is authoritative
for the prints managed through this stack, and its normal reconciliation and
orphan-cleanup policy applies.  Deletion is performed through the `gfenu`
protocol, never by editing opaque objects directly.  A successful identify uses
the matched group and finger id from the drained IRQ payload to select the
gallery entry.

There is no enrollment-import, cross-environment migration, or orphan-
preservation feature in this design.  Avoiding enrollment/removal and other
destructive reconciliation during development is a test-safety constraint for
the current device, not production policy.

Group 0 is the initial device policy.  The group is present in every locator so
multi-user/group policy can later be added without changing the storage model.
Secure-object filenames are deliberately absent from the print record: slot
filenames are a private TA/storage implementation detail and are not finger
ids.

### Enrollment authorization

The driver asks an enrollment-token provider for exactly 69 bytes after
`PRE_ENROLL`.  A provider receives the fresh challenge and returns a packed
`hw_auth_token_t`; it does not receive sensor data.  The first provider may
construct the device-specific challenge-only token, but that fallback must be
enabled explicitly and is a weaker normal-world authorization boundary.

A future credential service can implement the same boundary and return a
Gatekeeper-signed HAT.  PIN handling, Keymaster CBOR, synthetic-password state,
and credential throttling do not belong in the driver or supplicant.  Signed
token support is not complete until such a provider exists and is tested end to
end.

## Reuse investigation

Qualcomm MinkIPC supplies the authoritative FS/GPFS ids, message layouts,
operation tables, GPFS version, and the service-manager pattern.  Its current
`qtee_supplicant` dynamically loads listener libraries, while each listener
creates a Mink callback object with `CListenerCBO_new()` and registers it using
`IRegisterListenerCBO_register()`.  Those object-transport calls remain QTEE
specific and are replaced by one Linux TEE supplicant receive loop.

Mainline `qcomtee` already maps QTEE callback objects onto the generic Linux TEE
supplicant receive/send ioctls.  That confirms a second transport can share the
daemon's lifecycle and service registry, although its object-shaped request ABI
is not interchangeable with legacy QSEECOM's numeric listeners.

The operation handlers are used as a protocol reference rather than copied as
architecture.  Their Android mount probing and absolute-path policy are not
suitable for a native Linux machine-wide daemon.  The local backend instead
uses an explicit state root and Linux atomic-filesystem primitives.

OP-TEE's `tee-supplicant` provides the lifecycle precedent: it is available
before clients, owns the privileged backend device, and serves the whole
machine.  Its RPC protocol and OP-TEE implementation-id check are not reusable.

libfprint's existing match-on-chip drivers provide the device-stored print and
list/delete/clear action conventions.  Its current udev discovery supports only
spidev and hidraw resources, so the Goodix platform/misc device needs a small,
generic misc-device discovery extension; the driver must not bind a fake
spidev node.

A July 2026 GXFP5130 kernel series is direct-REE Goodix prior art: it exposes a
`/dev/gxfp` misc device for a companion libfprint plugin rather than pretending
the eSPI mailbox is a spidev device.  Its public kernel series validates a misc
device as a legitimate Goodix userspace boundary.  The companion plugin was not
linked in the posting and must be obtained before designing a duplicate generic
libfprint discovery extension.

## Lifecycle

The service manager creates configured state directories, starts required
supplicant transports before application-loader instances and their clients,
and treats service registration as readiness.  The loader and supplicant are
separate failure domains: restarting the supplicant must re-register listeners
and resume service for a previously loaded TA without unloading it.  This was
verified on the reference device by retaining the loader PID, restarting only
the supplicant, and then completing `gfenu` initialization and enumeration.
A separate direct-REE Goodix driver has no supplicant dependency.  On a recoverable TEE
device loss the daemon closes every listener/session and retries discovery and
registration with bounded exponential backoff.  SIGTERM/SIGINT stop receiving,
close the privileged descriptor (unregistering listeners), sync outstanding
state, and exit.  A watchdog may restart the daemon; a client operation blocked
inside secure world must fail or be restarted with the TEE device rather than
leaving a second supplicant racing the first.

Privilege is needed to open `/dev/teeprivN` and register listeners.  After the
state root and device are open, deployments should drop to a dedicated account
where the kernel/device policy permits it.  Service-manager sandboxing should
deny networking, new privileges, arbitrary filesystem writes, and access to
home directories.  Device-node and LSM policy must separately restrict
`/dev/teeN` and `/dev/goodix_fp` to fprintd.
