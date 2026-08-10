# Proof-of-concept client

`harness/gfharness.c` implements the complete sensor and trusted-application
client flow in one command-line program. It is used for:

- protocol testing
- hardware diagnostics
- comparison with vendor traces

The production stack implements the same responsibilities in separate system
services and a libfprint driver. It does not link to or execute the
proof-of-concept client. See the
[client design investigation](05-writing-a-client.md) and the
[production client stack](08-production-client-stack.md) for the libfprint
driver and machine-wide supplicant.

## Build

```sh
cc -O2 -Wall -Wextra -o gfharness harness/gfharness.c
```

The program uses Linux TEE, netlink, and Goodix sensor interfaces directly. It
requires:

- the QSEECOM TEE driver
- the Goodix sensor driver
- the reference platform firmware

## Commands

The machine-wide supplicant and application loader are the normal way to run
the stack. Stop those services before using the harness listener or loader
modes, because the kernel provides one supplicant request queue per TEE
device.

```sh
./gfharness --supp 0 serve &
./gfharness --load gfenu
./gfharness --enumerate
./gfharness --capture 120 --auth
./gfharness --capture 300 --enroll
./gfharness --remove <finger-id>
```

`gfenu` is the TA filename on the reference platform. Production clients read
the filename from the sensor device's `firmware_name` sysfs attribute. That
attribute is populated from the DT `firmware-name` property.

The capture argument is a timeout in seconds.

- Enrolment requires repeated finger placement and removal.
- Authentication reports the TA status and the matched group and finger
  identifiers.
- Enumeration uses the full 184-byte response, so the finger-ID array is
  available.

## Listener mode

`--supp 0 serve` registers filesystem listener 10 and GPFS listener 28672 in a
single process. This mode includes test-oriented control flow and filesystem
handling. For a system installation, use the standalone
[`qsee-supplicant`](https://github.com/wrobelda/qsee-supplicant) daemon.

The listener must be registered before loading the TA, because initialisation
reads stored objects. Do not create empty placeholder objects; the TA treats
an empty existing object as corrupt rather than missing.

## Safety

Enrolment and removal modify the active secure-object store. Back up that
store before destructive tests, and obtain approval from its owner.

These operations do not add or remove prints:

- enumeration
- initialisation
- authentication
- cancellation

This test client uses a challenge-only enrolment token. The tested TA supports
that token, but the token has a weaker authorisation boundary than a
Gatekeeper-signed HAT. See the
[Gatekeeper protocol](06-Gatekeeper-protocol.md).

## Tracing

[`tracing/README.md`](../tracing/README.md) documents the Frida scripts used
to capture the Android HAL and `qseecomd` behaviour. The command-channel hook
can truncate large payload dumps, so use the documented payload hooks when
exact buffer contents are required.
