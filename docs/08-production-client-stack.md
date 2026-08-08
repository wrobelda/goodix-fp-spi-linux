# Production Linux client stack

This document describes the production userspace implementation. The
[reference test client](07-reference-client.md) is a separate command-line
hardware test program and is not used by either production component.

The production sources are maintained separately:

- [`wrobelda/qsee-supplicant`](https://github.com/wrobelda/qsee-supplicant)
  provides the machine-wide QSEECOM listener daemon, TA loader, service units,
  and hardware-free tests.
- [`wrobelda/libfprint`, branch `goodix-qsee`](https://github.com/wrobelda/libfprint/tree/goodix-qsee)
  provides generic firmware-described misc-device discovery and the Goodix
  QSEE match-on-chip driver.

The [client design investigation](05-writing-a-client.md) documents the
protocol research and architecture decisions.

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

The `goodixqsee` driver is a TEE match-on-chip driver:
imaging, templating, and matching are delegated to the Goodix trusted
application.  A Goodix sensor for which normal world has direct SPI or a
complete kernel imaging interface belongs in a separate REE/image driver,
selected by its own device table and kernel ABI.  There is no runtime
TEE-versus-REE backend switch inside `goodixqsee`.

The bus is therefore not the backend.  As libfprint's Unsupported Devices wiki
notes for SPI sensors, many Android devices restrict direct SPI access to secure
world.  Such a device may still have a normal-world kernel driver for power,
reset, and IRQ delivery, but it must bind an appropriate TEE driver.  A
separate REE driver must bind only when the kernel ABI actually permits sensor
transactions (or provides a complete imaging interface).  ACPI or device-tree
compatibility with the same Goodix family is not sufficient evidence by itself.

The `goodixqsee` driver is a normal-world protocol client. It owns, for one
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

There is no enrollment-import, cross-environment migration, or
orphan-preservation feature in this design.

Group 0 is the initial device policy.  The group is present in every locator so
multi-user/group policy can later be added without changing the storage model.
Secure-object filenames are deliberately absent from the print record: slot
filenames are a private TA/storage implementation detail and are not finger
ids.

### Enrollment authorization

The driver asks an enrollment-token provider for exactly 69 bytes after
`PRE_ENROLL`.  A provider receives the fresh challenge and returns a packed
`hw_auth_token_t`; it does not receive sensor data. The reference-platform
provider constructs the device-specific challenge-only token. This fallback
has a weaker normal-world authorization boundary.

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
list/delete/clear action conventions. The production branch adds generic
firmware-described misc-device discovery so the Goodix driver binds the real
platform device rather than a fake spidev node.

A July 2026 GXFP5130 kernel series is direct-REE Goodix prior art: it exposes a
`/dev/gxfp` misc device for a companion libfprint plugin rather than pretending
the eSPI mailbox is a spidev device.  Its public kernel series validates a misc
device as a legitimate Goodix userspace boundary. Its direct-REE protocol is a
separate libfprint driver scope.

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

## Build and installation

Build and test the supplicant with:

```sh
make
make check
make DESTDIR="$pkgdir" install
```

Enable `qsee-supplicant.service` before the loader instance named by the
device's DT `firmware-name` property. The reference platform uses
`qsee-app-loader@gfenu.service`. The loader holds the TA load reference in a
process separate from the supplicant.

Build libfprint with the `goodixqsee` driver enabled and install it for fprintd.
The fprintd service needs `/dev/goodix_fp` and the non-privileged TEE character
devices. It does not need `/dev/teeprivN`. A systemd service drop-in can grant:

```ini
[Service]
DeviceAllow=/dev/goodix_fp rw
DeviceAllow=char-tee rw
```

On Alpine, install a separate `/etc/pam.d/gdm-fingerprint` service because the
`fprintd-pam` package installs the module without enabling it for GDM:

```pam
#%PAM-1.0
auth       required    pam_fprintd.so
account    include     base-account
password   include     base-password
session    include     base-session
```

Keep `gdm-password` separate. GDM uses independent password and fingerprint
workers, so the fingerprint module does not need to be added to shared password
or SSH authentication stacks.

## Tests on the reference platform

The standalone supplicant and loader replace the harness listener and loader.
The following behavior has been tested on the Xiaomi Pad 5 Pro 5G:

- restart the supplicant while `gfenu` stays loaded, re-register both
  listeners, and continue TA initialization and enumeration;
- libfprint probe, open, initialization, close, and cancellation;
- fprintd enumeration, enrollment, duplicate rejection, known-finger
  verification, matched finger-ID reporting, and deletion of one print;
- finger-present and finger-removed events;
- per-sample enrollment retry and quality feedback;
- GDM login and GNOME session unlock through `pam_fprintd`.

The TA overwrites control fields in every IRQ response. The driver writes the
mode, size, and arm fields before every IRQ drain invocation. Without this step,
later invocations return an empty mask and fprintd times out.

## Continuous integration

The supplicant GitHub Actions workflow runs on pushes and pull requests. It
builds with GCC and Clang, runs the hardware-free protocol and filesystem tests,
checks a staged installation, and repeats the tests with AddressSanitizer and
UndefinedBehaviorSanitizer. Hardware-dependent listener registration and TA
tests are not run in GitHub-hosted CI.

The libfprint Goodix protocol test and core `fpi-device` test pass with the
driver enabled. Live hardware tests require the reference platform and physical
finger input.

## Remaining limitation

Enrollment uses the reference platform's challenge-only 69-byte HAT provider.
The driver exposes a replaceable token-provider boundary that can accept a
complete Gatekeeper-signed HAT from a separate credential service. Signed HAT
support is not implemented. PIN handling, Keymaster requests, and
synthetic-password logic do not belong in the fingerprint driver or
supplicant; see the [Gatekeeper protocol](06-Gatekeeper-protocol.md).
