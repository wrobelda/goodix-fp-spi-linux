# Reference test client

`harness/gfharness.c` is a command-line implementation of the documented
sensor and trusted-application protocols. It is used for protocol testing,
hardware diagnostics, and comparison with vendor traces. Production services
do not link to or execute it. See [05-writing-a-client.md](05-writing-a-client.md)
for the libfprint driver and machine-wide supplicant design.

## Build

```sh
cc -O2 -Wall -Wextra -o gfharness harness/gfharness.c
```

The program uses Linux TEE, netlink, and Goodix sensor interfaces directly. It
requires the QSEECOM TEE driver, the Goodix sensor driver, and the reference
platform firmware.

## Commands

The machine-wide supplicant and application loader are the normal way to run
the stack. Stop those services before using the harness listener or loader
modes; the kernel provides one supplicant request queue per TEE device.

```sh
./gfharness --supp 0 serve &
./gfharness --load gfenu
./gfharness --enumerate
./gfharness --capture 120 --auth
./gfharness --capture 300 --enroll
./gfharness --remove <finger-id>
```

`gfenu` is the TA filename on the reference platform. Production clients read
the filename from the sensor device's `firmware_name` sysfs attribute, which is
populated from the DT `firmware-name` property.

The capture argument is a timeout in seconds. Enrollment requires repeated
finger placement and removal. Authentication reports the TA status and matched
group and finger identifiers. Enumeration uses the full 184-byte response so
the finger-ID array is available.

## Listener mode

`--supp 0 serve` registers filesystem listener 10 and GPFS listener 28672 in a
single process. This mode includes test-oriented control flow and filesystem
handling. Use the standalone
[`qsee-supplicant`](https://github.com/wrobelda/qsee-supplicant) daemon for a
system installation.

The listener must be registered before loading the TA because initialization
reads stored objects. Do not create empty placeholder objects; the TA treats an
empty existing object as corrupt rather than missing.

## Safety

Enrollment and removal modify the active secure-object store. Back up that
store before destructive tests and obtain approval from its owner. Enumeration,
initialization, authentication, and cancellation do not add or remove prints.

The challenge-only enrollment token used by this test client is supported by
the tested TA but has a weaker authorization boundary than a Gatekeeper-signed
HAT. See [06-Gatekeeper-protocol.md](06-Gatekeeper-protocol.md).

## Tracing

[`tracing/README.md`](../tracing/README.md) documents the Frida scripts used to
capture the Android HAL and `qseecomd` behavior. The command-channel hook can
truncate large payload dumps; use the documented payload hooks when exact
buffer contents are required.
