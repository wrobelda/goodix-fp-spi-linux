# Machine-wide TEE supplicant

`qsee-supplicant` is a machine service for trusted applications that use
Qualcomm's legacy numeric listener ABI.  It is not a fingerprint daemon.  The
initial QSEECOM transport registers FS service 10 and GPFS service 28672 on the
QSEECOM `/dev/teeprivN` device and dispatches both from the one kernel queue.

The dispatch/service interface is independent of the transport so QTEE/Mink or
OP-TEE transports and other service sets can be added without embedding them in
biometric clients.

## Build and test

```sh
make
make check
make DESTDIR="$pkgdir" install
```

The tests use a temporary directory and no TEE or sensor hardware.  They cover
Android-path confinement, traversal rejection, restrictive modes, atomic GPFS
replacement, and protocol backup creation.

## Configuration and state

```sh
qsee-supplicant --state-dir /var/lib/qsee-supplicant
```

The state directory is the sole filesystem namespace visible to listener
requests.  `/data/vendor/fpdump/gf_calibration.so`, for example, resolves to
`STATE/data/vendor/fpdump/gf_calibration.so`.  It never opens the host's
`/data`.  `..`, symlinked path components, control characters, and malformed
fixed-size path fields are rejected.

Directories and objects are created 0700 and 0600.  Do not pre-create empty
objects: `gfenu` treats a present empty object as corrupt rather than absent.
Back up the entire state directory only as sensitive, device-bound data.

The supplied systemd and OpenRC units run as root.  The current kernel both
requires `CAP_SYS_ADMIN` and normally creates `/dev/teeprivN` mode 0600 owned by
root, so merely assigning the capability to a dedicated account is not enough.
The systemd unit bounds capabilities to `CAP_SYS_ADMIN` and confines filesystem,
network, and kernel access.  A downstream may use a dedicated account only when
its udev and LSM policy also grants that account access to the TEE node and state
directory.

## Lifecycle

Start the daemon before any service that may load a dependent TA.  Listener
registration is logged as `event=listener_registered`.  Loss of the TEE device
closes every registration and every outstanding listener-side file handle,
then retries discovery with bounded exponential
backoff.  SIGTERM/SIGINT closes `/dev/teeprivN`, which unregisters the
listeners, and exits cleanly.  Service managers should restart a failed daemon.

Only one legacy-QSEECOM supplicant may serve a TEE device.  Do not run the
harness `--supp` mode at the same time.

Dynamic trusted applications need an independent load reference.  Enable the
generic loader instance named by the device's firmware description, for example
`qsee-app-loader@gfenu.service` on the reference platform.  The loader starts
after the supplicant, asks the kernel to load `%i` from firmware, and holds that
session until shutdown.  It deliberately runs as a separate process: a
supplicant crash and restart re-registers listeners without unloading an
already resident application.  Clients read the application name from their
device's `firmware_name` sysfs attribute; neither daemon assumes `gfenu`.
