# Goodix SPI fingerprint sensors

A Goodix SPI fingerprint sensor — not the unrelated USB kind common in laptops —
can be driven in two ways.

In the "traditional" one, the Rich Execution Environment (REE), the operating system's
kernel driver handles all the hardware over SPI bus protocol, allowing the user space to
talk to the sensor directly to do the imaging, to perform the
imaging, the templating and the matching logic.

However, this is rather rare nowadays as it involves substantial security risk,
since the images and the templates are handled by the operating system, where
anything that compromises the kernel or the userspace stack can read or
substitute them.

The alternative is running the fingerprint logic in a Trusted Execution
Environment (TEE), such that the operating system never touches the imaging or the
matching at all — which instead happen inside a trusted, proprietary, signed and
closed-source application running in the SoC's secure world. The kernel driver
is left with handling only the bare minimum of the hardware, without access to
the SPI bus the scanner is connected to.

When it comes to Goodix scanners, two known implementations of such TEE
applications exist, one for MediaTek's MicroTrust TEE, the other for Qualcomm's
QSEE, and they ship in the operating system the device comes with.

## Supporting the Goodix in TEE mode on Linux

TEE mode is much safer of the two for the user, but it also pretty much
guarantees that the OEM of the — typically an Android — device keeps the Hardware
Abstraction Layer (HAL) library closed, and it is that library that implements the
entirety of the communication between the Operating System's permissions subsystem
and the trusted application. The only open piece is the kernel sensor
driver, which does the bare minimum of bringing the scanner up.

This unfortunately means that the only way to add support for these scanners in Linux
is by reverse-engineering the Android implementation. A 2021 article,
[Fingerprint sensors on tuxified Android phones: Impossible?](https://emainline.gitlab.io/2021/12/12/fingerprint_P1.html)
([archived](https://web.archive.org/web/20240418063251/https://emainline.gitlab.io/2021/12/12/fingerprint_P1.html)),
describes well what a port would have to do and why nobody had done it.

## LLM disclaimer

The project was developed using Claude Opus 5 and Kimi K3 over a span of 10 days.
Claude Fable 5 would likely speed things up a bit, but anything security-adjacent
gets flagged by Anthropic and the model was getting routinely dropped to Opus 5,
hence the Kimi K3 usage here.

This is not unverified AI slop — most of the architecture decisions and code layout
were made by myself, as well as the disassembly/reverse-engineering approach and driving it.

The work here is tested on my hardware, with the hope of further refinement, testing on more
devices and readying for mainlining into the Linux kernel.

## Status

**The TEE support for these scanners on Qualcomm SoCs is now done.**

This project serves as a proof of concept rather than a complete userspace implementation: what this project
provides is the protocol, the two kernel drivers needed to reach it on the hardware side and on the TEE side, and a
reference client that demonstrates the sequence, as the basis for a real
[`fprintd`](https://fprint.freedesktop.org/) driver.

- [ ] **REE mode** — the operating system does the imaging itself over SPI. It
  can be added to the sensor driver by [porting](docs/00-sensor-driver.md#normal-world-imaging-ree)
  the SPI transfer and image scanning code from the MediaTek-lineage `gf_spi_tee.c`, where it sits behind
  `SUPPORT_REE_SPI`
- [x] **TEE mode on Qualcomm** — reaching the trusted application over
  QSEECOM; everything documented here was exercised in this mode:
  - [x] [load and talk to the trusted application](docs/01-kernel-tee-driver.md#loading-an-application)
  - [x] [unloading the application](docs/01-kernel-tee-driver.md#application-lifetime)
  - [x] [persistence across reboot](docs/04-secure-storage.md#the-file-set) —
    QSEE-sealed objects, stored through the [listener services](docs/03-listener-services.md)
- [ ] **TEE mode on MediaTek** — the protocol and the sensor driver carry
  over; the missing half is a MicroTrust backend in `drivers/tee/`, where
  MediaTek platforms that reach a TEE upstream do so through OP-TEE. Requires
  reverse-engineering
- [x] **The [trusted application's protocol](docs/02-ta-protocol.md)** —
  Goodix-generic rather than Qualcomm's, so these should carry over to a
  MediaTek port:
  - [x] [enrolment, first finger](docs/02-ta-protocol.md#enrolment)
  - [x] [enrolment, additional fingers](docs/04-secure-storage.md#lifecycle-observed)
  - [x] [authentication (match and reject)](docs/02-ta-protocol.md#authentication)
  - [x] [unenrolment](docs/02-ta-protocol.md#removal)
  - [x] [calibration](docs/04-secure-storage.md#calibration-is-generated) —
    generated on-the-fly by `gfenu` itself, nothing to handle
  - [ ] [lockout policy](docs/05-writing-a-client.md#things-that-will-bite) —
    not observed being enforced by the trusted application
  - [ ] [Gatekeeper-signed enrolment auth token](docs/06-Gatekeeper-protocol.md#relationship-to-gfenu) —
    Android's signing flow is decoded, but Linux does not yet obtain and submit
    a signed HAT; the harness currently uses this TA's challenge-only fallback
  - [x] [listing enrolled fingers](docs/04-secure-storage.md#application-index-versus-storage) —
    `ENUMERATE` returns count, group ids, and finger ids
  - [x] [per-sample quality feedback](docs/02-ta-protocol.md#error-codes) —
    `IRQ` status, mirrored at payload +12 and mapped to Android acquired-info
  - [x] [which template matched](docs/02-ta-protocol.md#authentication) —
    finger id at IRQ payload +0x4fce8, copied into `AUTHENTICATE_FINISH`
  - [ ] [display wake on touch](docs/00-sensor-driver.md#panel-state) — not
    decoded; `SCREEN_ON`/`SCREEN_OFF` (1017, 1018) are a client's to send, not
    the sensor driver's
- [ ] **[GW in-display family](docs/00-sensor-driver.md#one-driver-family-many-parts)** —
  untested; differs from the capacitive copies mostly in
  board power and pins
- [ ] **Upstreaming the kernel side** — both drivers build and run, but neither
  has been posted. These have to be settled with the maintainers first:
  - [ ] [`qcom-scm-blocked-listener-warn`](https://github.com/wrobelda/linux/tree/qcom-scm-blocked-listener-warn) —
    two `WARN_ON()`s in `qcom_scm_qseecom_call()` that no in-kernel caller can
    reach but an ordinary invoke can, so `panic_on_warn` turns them into a
    denial of service. A standalone patch the TEE series depends on
  - [ ] **`TEE_IMPL_ID_QSEECOM = 5` is new uapi** — a client tells this driver
    from [`qcomtee`](docs/01-kernel-tee-driver.md#why-this-driver) by reading it
    from `TEE_IOC_VERSION`, so the number is ABI and needs the TEE maintainer's
    ack in the same posting
  - [ ] **Sessions are opened by name, not by UUID** — QSEE matches on a string
    and there is nothing to render a UUID into, so the name arrives in a
    parameter and the UUID must be zero. `amdtee` renders its UUID into a
    firmware filename; `qcomtee` hit the same wall and used objref parameters.
    This driver does not claim `TEE_GEN_CAP_GP`, but the TEE subsystem may
    prefer a generic "session by name" of its own to each backend overloading a
    parameter
  - [ ] **The privileged device's `open_session` is polymorphic** — it tells
    "register a listener" from "load an application" by whether parameter 0 is
    a value or a memref. Two `func` values would document themselves
  - [ ] **[`CAP_SYS_ADMIN` on the privileged device](docs/01-kernel-tee-driver.md#security)** —
    no other TEE backend checks a capability; OP-TEE relies on the permissions
    of `/dev/teepriv0`. Worth offering to drop rather than defending
  - [ ] **A TZ memory pool per invoke** — the bounce buffer for each command
    gets its own pool. For the [`IRQ` command](docs/02-ta-protocol.md#interrupts)
    that is a large allocation at interrupt rates, and `-ENOMEM` mid-capture
    under fragmentation
  - [ ] **The `mdt_loader` extension** needs a soc/qcom ack — it adds two
    functions and a Kconfig prompt to a file this project does not own; see
    [what the rest of the kernel had to grow](docs/01-kernel-tee-driver.md#what-the-rest-of-the-kernel-had-to-grow)
  - [ ] **[A wedged supplicant blocks every QSEECOM user](docs/01-kernel-tee-driver.md#a-wedged-supplicant-blocks-more-than-itself)**,
    in-kernel ones included — which decides who may be granted the privileged
    device

- [ ] **A QSEECOM supplicant** — the listener service
  [belongs in one machine-wide process](docs/05-writing-a-client.md#where-it-should-live)
  rather than inside the `fprintd` driver, since the kernel allows one receiver
  per device (not per trusted app!) and a second QSEE client handling some other
  type of app could not register a handler of its own. Qualcomm's
  [`minkipc`](https://github.com/qualcomm/minkipc) is the closest source-level
  reference: it publishes the same FS, GPFS, time and RPMB services, but their
  registration is tied to QTEE's Mink object transport. Whether to give those
  handlers a QSEECOM transport in that project or carry a separate daemon,
  whether it also loads applications, and whether the time listener is answered
  in the kernel are all open.

## What is not understood

This is a proof of concept, and this list is deliberately part of it. None of
these block the working lifecycle; all of them would matter to a production
driver.

What these have in common is that reading or porting the downstream code does
not settle them. They need a device to test against, a vendor to answer, or
evidence from outside this project entirely.

- [ ] **Whether another TA or firmware generation requires a
  [Gatekeeper-signed enrolment token](docs/06-Gatekeeper-protocol.md#relationship-to-gfenu).**
  This one accepts a token carrying only the fresh challenge
  both before and after a lock-screen credential is provisioned
  ([[our device]](#how-we-know)). Android supplies a signed, challenge-bound
  token; its UI's backup-PIN requirement is framework policy, not an additional
  Goodix protocol step.
- [ ] **Whether the trusted application enforces any lockout of its own.** Not
  observed ([status](#status)); treat the absence as unverified and implement
  lockout regardless.
- [ ] **Whether the other listener services are ever needed.** We never
  observed `gfenu` raising a request on any of them, across every flow we
  exercised ([[our device]](#how-we-know)). Unexercised paths, and other firmware versions, are
  not covered by that.
- [ ] **How broadly the recovered trusted-application ABI applies.** `gfenu` is
  the filename supplied by this device's firmware description, not a known
  cross-device Goodix protocol name.  We do not know whether another OEM's
  Goodix application, another sensor generation, or a Goodix application on a
  MediaTek or newer Qualcomm TEE uses the same command numbers, payload layouts,
  interrupt state machine, or storage conventions.  A production client must
  treat the protocol documented here as a versioned compatibility profile,
  select it from positive platform evidence, and obtain the application name
  from the kernel's `firmware_name` sysfs attribute, populated from the device
  tree `firmware-name` property, rather than compiling `gfenu` into the driver
  identity.
- [ ] **Whether a trusted application image is bound to a model or a vendor, and
  who signs it.** The image used here came from the stock OS of the model it
  runs on, and we never tried it anywhere else — another unit of the same model
  is not the interesting case; a different model or manufacturer sharing the
  sensor and SoC is. The secure world rejects a malformed image, which
  shows it checks integrity, but says nothing about device binding or about the
  signing chain. In particular we do not know whether trusted applications can
  be redistributed the way other Qualcomm firmware is, or whether they are
  signed with keys held only by Qualcomm or the OEM. Nobody has checked whether
  `linux-firmware` already carries a trusted application, which would answer it
  by example and is the cheapest place to start. That question decides whether any of this can be
  packaged for users rather than extracted per device.
- [ ] **What the sensor's own image data means.** The reference client can save raw
  frames, but nothing here interprets them; all image processing happens inside
  the secure world.

## What is where

Six pieces, each independently useful; the first four had to exist for any of
it to work.

| layer | what it does | where |
|---|---|---|
| **Sensor driver** | owns the sensor's power, reset and interrupt; relays finger events — the secure world expects the hardware powered and out of reset | [`docs/00-sensor-driver.md`](docs/00-sensor-driver.md), branch [`goodix-fp-spi`](https://github.com/wrobelda/linux/tree/goodix-fp-spi) |
| **TEE driver** | exposes QSEE trusted applications through Linux's TEE subsystem, so user space can reach them | [`docs/01-kernel-tee-driver.md`](docs/01-kernel-tee-driver.md), branch [`qcom-qseecom-tee`](https://github.com/wrobelda/linux/tree/qcom-qseecom-tee) |
| **TA protocol** | the command set the Goodix application speaks — undocumented, recovered by tracing the Android stack and reading the vendor's binaries | [`docs/02-ta-protocol.md`](docs/02-ta-protocol.md) |
| **Listener services** | the file service the application needs the normal world to run: it has no storage of its own and asks for reads and writes of its encrypted data — also undocumented | [`docs/03-listener-services.md`](docs/03-listener-services.md), the stored objects in [`docs/04-secure-storage.md`](docs/04-secure-storage.md) |
| **Client** | a proof of concept, basis for the real `fprintd`-based client | [`docs/05-writing-a-client.md`](docs/05-writing-a-client.md), code in [`harness/`](harness/) |
| **Gatekeeper** | the separate hardware-backed credential verifier Android uses to authorize enrolment with a signed token | [`docs/06-Gatekeeper-protocol.md`](docs/06-Gatekeeper-protocol.md) |

Read them in order. The TA protocol is meaningless without a listener service
running, and the listener service cannot be registered without the kernel
driver.

## Quick start

Two kernel drivers, one branch each, both off mainline v7.1 at
[wrobelda/linux](https://github.com/wrobelda/linux):

| branch | provides | config symbol |
|---|---|---|
| [`qcom-qseecom-tee`](https://github.com/wrobelda/linux/tree/qcom-qseecom-tee) | the TEE driver, reaching trusted applications | `CONFIG_TEE_QSEECOM` |
| [`goodix-fp-spi`](https://github.com/wrobelda/linux/tree/goodix-fp-spi) | the sensor driver and its binding, powering the sensor | `CONFIG_INPUT_GOODIX_FP_SPI` |

They are independent series and do not depend on each other, but the sequence
below needs both, plus a device tree node for the sensor — the board support is
not on either branch.

With that in place:

    cc -O2 -o gfharness harness/gfharness.c

    # the file service must be running before the application is loaded,
    # because it asks for its files during initialisation
    ./gfharness --supp 0 serve &
    ./gfharness --load gfenu             # kernel fetches gfenu.mdt + .bNN

    ./gfharness --capture 300 --enroll     # 300 seconds; present a finger repeatedly
    ./gfharness --capture 120 --auth       # 120 seconds; present it again
    ./gfharness --remove 0x1234abcd

`--supp 0` serves every listener the application uses from one process, which is
required — the kernel driver keeps [one supplicant queue per
device](docs/01-kernel-tee-driver.md#one-supplicant-queue-per-device).

## Reference platform

Everything documented here was verified on:

- **Device** Xiaomi Pad 5 Pro 5G ("enuma"), postmarketOS
- **Sensor** Goodix GF3626 (`GF_CHIP_3626ZS1`, part `A005203`), behind the
  power button
- **SoC** Qualcomm SM8250, QSEE trusted execution environment
- **Application** `gfenu`, loaded from `/vendor/firmware_mnt/image/`
- **[Sensor driver](docs/00-sensor-driver.md)**
  `drivers/input/misc/goodix_fp_spi.c`, which owns the regulator, reset line
  and
  interrupt, and reports interrupts over netlink

Its [device tree node, and the state of its
binding](docs/00-sensor-driver.md#device-tree), are documented with the
driver.

On another model, expect the application and the sensor part to differ, and
check payload sizes; the transport and the listener protocols should not
change.

None of what the sensor does is visible to the operating system. It captures
images and runs firmware of its own, but it does that across an SPI bus the
secure world owns; what reaches the normal world is an interrupt line saying
that something happened. Every decision — image processing, template
construction, matching — is made inside the trusted application. That is why
this is a protocol problem rather than a hardware-driver problem — although a
[small hardware driver](docs/00-sensor-driver.md) naturally exists too,
owning the power, reset and interrupt the secure world leaves to the normal
world.

## Scope

Three things stack up here, and they generalise differently:

- **The QSEECOM transport and its listener services are Qualcomm-generic.** Any
  device whose secure world is QSEE speaks this, whatever application is running
  on top. The [kernel driver](docs/01-kernel-tee-driver.md) and
  [listener service](docs/03-listener-services.md) documents should apply
  unchanged.
- **The command set is Goodix-generic.** The command numbering and the
  enrol / authenticate / remove sequences come from Goodix's own tables and are
  shared across their trusted applications. The
  [command protocol](docs/02-ta-protocol.md) and
  [secure storage](docs/04-secure-storage.md) documents should largely apply,
  with per-application differences in which commands a given
  build answers.
- **What is per-model splits three ways, and each is configured somewhere
  different.**
  - *The sensor hardware* — regulator, reset line, interrupt — is described in
    the device tree and driven by the
    [companion char device](docs/00-sensor-driver.md). Board description
    belongs there, and this is the only one of the three that is in DT.
  - *The trusted application* ships with the device's stock operating system,
    and was extracted from it. How far one image travels between models, and
    whether it may be redistributed at all, is unresolved — see
    [What is not understood](#what-is-not-understood).
  - *The application name and its payload sizes* belong to the application
    rather than the board. The client passes the name when opening a session and
    has to know the payload layout; the kernel TEE driver is
    application-agnostic and reads neither from the device tree.

Everything here was established on one device, the
[reference platform](#reference-platform), by the means set out in
[How we know](#how-we-know). Treat the layer
boundaries as the guide to what should port and what needs re-checking.

## How we know

Claims in these documents come from five different places, and they are not
equally strong. Where it matters, a claim carries a tag:

| tag | source | what would overturn it |
|---|---|---|
| `([our device])` | observed on a Xiaomi Pad 5 Pro 5G running our own client | another model, another firmware version, or a code path we never exercised |
| `([vendor trace])` | observed by instrumenting the stock Android HAL and `qseecomd` | a vendor path we did not trigger, or a different HAL build |
| `([libdrmfs.so disassembly])` | read out of the vendor binary's code, not observed running | misreading the binary; a different build of it |
| `([gfenu symbols])` | symbol names in the trusted application image | names that do not mean what they suggest |
| `([Qualcomm minkipc])` | Qualcomm's QTEE listener protocol headers and service implementations in [`qualcomm/minkipc`](https://github.com/qualcomm/minkipc/tree/main/listeners) | a device carrying a modified Qualcomm listener ABI |

Only the five labels in that table are tags, and each one in the text links back
here. Square brackets elsewhere — `"[gf_fts]"`, for instance — are literal
strings quoted from a binary, not provenance.

Untagged statements are either definitions or things that follow directly from
tagged ones. Two conventions to keep when editing: an absence is almost always
"we did not observe X", not "X does not exist", and anything learned from one
device should say so.

## Glossary

**The secure world**

| term | meaning |
|---|---|
| **TrustZone / secure world** | the higher-privilege execution state the kernel cannot inspect; QSEE runs there |
| **QSEE** | Qualcomm Secure Execution Environment, the TrustZone OS running in the secure world |
| **normal world** | everything outside TrustZone: Linux, its drivers and user space. It cannot read secure-world memory, and reaches it only through SCM calls |
| **trusted application** | abbreviated TA. A signed program running under QSEE. Here, [`gfenu`](docs/02-ta-protocol.md#the-application), which owns all biometric logic |
| **SCM call** | Secure Channel Manager call, the interface from the kernel into the secure world |

**The two interfaces, and the drivers that expose them**

| term | meaning |
|---|---|
| **QSEECOM / smcinvoke** | QSEE's two normal-world interfaces. An application is built against one or the other, so they are not alternative routes to the same thing:<br>• **QSEECOM** — command-based: load an application by name, send it a command buffer, service the listener requests it raises. `gfenu` and its generation are built against this one.<br>• **smcinvoke** — object-based: services are objects with operations that return results and further objects. Newer, but not a way to reach QSEECOM-era applications. |
| **TEE subsystem** | Linux's [generic trusted-execution framework](https://docs.kernel.org/tee/tee.html) (`/dev/tee*`), which our kernel driver plugs into |
| **memref** | a TEE parameter that refers to shared memory rather than carrying a value; the request, response and payload buffers are all memrefs |
| **`qcomtee` / `qseecomtee` / `qcom_qseecom`** | the three kernel drivers in play. The first two are TEE-subsystem backends, each exposing one of QSEE's interfaces to user space; the third is a kernel-internal client with no user-space face:<br>• **`qcomtee`** — exposes smcinvoke (`drivers/tee/qcomtee/`), documented as [QTEE](https://docs.kernel.org/tee/qtee.html). Reports `TEE_IMPL_ID_QTEE`, advertises `TEE_GEN_CAP_OBJREF`. This project never talks to it, but it is loaded on the same hardware and takes a TEE device node of its own.<br>• **`qseecomtee`** — what this project adds (`drivers/tee/qseecom/`), exposing QSEECOM. Reports `TEE_IMPL_ID_QSEECOM`. Where this driver and `qcomtee` are both loaded there are two TEE devices, and which of them is `/dev/tee0` depends on probe order — so a client finds the one it wants by reading `impl_id` from `TEE_IOC_VERSION`, never by node name.<br>• **`qcom_qseecom`** — `drivers/firmware/qcom/qcom_qseecom*.c`, letting *kernel* code talk to a trusted application, `qcom_qseecom_uefisecapp` for EFI variables. Predates this work, offers user space nothing, gated on a machine allowlist. |

**Answering the secure world**

| term | meaning |
|---|---|
| **listener service** | a normal-world service a TA calls out to, identified by a small integer id. The TA blocks until one answers |
| **supplicant** | the normal-world process that answers listener requests. Called that by the TEE subsystem; `qseecomd` is Android's |
| **`qseecomd`** | the Android userspace daemon Qualcomm ships to answer listener requests — what this project replaces |
| **`gpfs`** | GlobalPlatform file system, listener 28672: the sealed-object store |

**Storage**

| term | meaning |
|---|---|
| **sealed object** | data encrypted and integrity-protected by QSEE with a device-bound key. The normal world stores ciphertext it cannot read |
| **`qsee_sfs_*` / `qsee_fts_*` / `fts_*`** | the three layers a stored object passes through, inside the secure world:<br>• **`qsee_sfs_*`** — QSEE's secure file system, POSIX-shaped open/read/write/seek/close.<br>• **`qsee_fts_*`** — QSEE's sealing API: encrypt, decrypt, integrity protect and verify. Qualcomm's, not Goodix's, despite what it stores.<br>• **`fts_*`** — unprefixed, and Goodix's own: wrappers over the two above, and what `gfenu` calls directly. Attributed to Goodix from the `GF_`-prefixed constants and a `"[gf_fts]"` log string, not from the symbol names. |
| **template** | the enrolled representation of a fingerprint, built inside the secure world and stored as a sealed object. Never an image, and never readable by the normal world |
| **OTP** | one-time-programmable data fused into the sensor at manufacture, read out at initialisation and persisted as `gf_otp_info.so` |

**The applications here**

| term | meaning |
|---|---|
| **`gfenu`** | Goodix's fingerprint trusted application as shipped on the Xiaomi Pad 5 Pro 5G. Other models carry Goodix's implementation as their own application, under a different name |
| **gatekeeper** | [Android's credential authority](docs/06-Gatekeeper-protocol.md): it verifies the PIN or password and issues signed tokens attesting that it happened. Biometric enrolment expects one of its tokens |
| **auth token** | the [`hw_auth_token_t` blob](docs/02-ta-protocol.md#the-authentication-token) Gatekeeper signs and `ENROLL` carries, attesting that the user authenticated recently |
| **Keymaster / `km41.mbn`** | the [trusted application that implements Gatekeeper](docs/06-Gatekeeper-protocol.md) on this platform and signs those tokens. It lives in the dedicated `keymaster_a`/`keymaster_b` partition rather than among the dynamically loaded applications in `NON-HLOS.bin` |
| **`miriskm`** | a separate Xiaomi risk-management trusted application. Its own diagnostics and strings concern device-status, registration-token, certificate and remote-auth operations; it is not Android Gatekeeper |

**The hardware, and the stack this is aimed at**

| term | meaning |
|---|---|
| **sensor driver** | the [kernel driver owning the sensor's supply, reset line and interrupt](docs/00-sensor-driver.md). In TEE mode that is all it does, the trusted application doing the biometrics |
| **MDT image** | Qualcomm firmware format: one ELF-shaped image split across files. `.mdt` holds the ELF header, program-header table and authentication hash; each `.bNN` holds the payload of program header `NN`. See [the image format](docs/01-kernel-tee-driver.md#what-the-files-are) |
| **HAL** | hardware abstraction layer, the Android userspace library a vendor ships to drive a device. The one this project replaces is Goodix's fingerprint HAL |
| **`fprintd` / `libfprint`** | the Linux userspace fingerprint stack this work is ultimately aimed at |

## Licence

GPL-2.0-only, as declared in the source files. The kernel work lives on its own
branches and carries the kernel's own licensing: the driver is `GPL-2.0-only`,
and its device tree binding keeps the dual `GPL-2.0-only OR BSD-2-Clause` that
bindings use.

## Provenance

The protocol documentation and the reference client are clean-room work for
interoperability, produced on hardware owned by the author with the bootloader
unlocked by him, using his own fingerprints. They document how to *talk to* the
trusted application; they do not extract biometric data, recover keys, or defeat
any check the application makes.

The sensor driver is **not** clean-room and is not claimed to be. It
is a backport of Goodix's downstream driver, GPL-2.0-only, carrying its original
copyrights — see its [provenance and
licence](docs/00-sensor-driver.md#provenance-and-licence).

Fingerprint templates are sealed with a device-bound key inside the secure world
and remain opaque — the storage layer described here stores and returns
ciphertext without being able to read it. Where the application enforces
something, the documented answer is to satisfy it, never to bypass it.
