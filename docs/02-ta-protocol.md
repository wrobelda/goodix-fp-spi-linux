# Layer 2: the Goodix trusted application protocol

Everything in this document crosses the
[TEE invoke interface](01-kernel-tee-driver.md#invoking-a-command).

## The application

As established, a Goodix sensor can have its fingerprint implementation provided
as an SoC trusted application (TEE mode), as opposed to being driven directly by
the operating system (REE mode).

Of the [two known implementations](../README.md), this document describes
Qualcomm's.

`gfenu` is that application on the Xiaomi Pad 5 Pro 5G, which uses the Qualcomm
SM8250 SoC and which this project was built against. It provides the whole
biometric implementation: sensor configuration, image capture, template
construction, matching and the sealing of stored data all happen inside it. The
normal world never sees a fingerprint image or a template — it starts commands,
answers [file-I/O requests](03-listener-services.md) on the application's
behalf, and stores [sealed ciphertext](04-secure-storage.md) it cannot read.

Practical facts about it:

- **It is addressed by name.** QSEE matches on the string `"gfenu"`, which is
  also the identity a later lookup resolves. There is no UUID.
- **Its image is `gfenu.mdt` plus `gfenu.b00`–`gfenu.b07`**, extracted from the
  device's stock operating system and placed in `/lib/firmware` for the kernel
  to find. See
  [the image format and how it is loaded](01-kernel-tee-driver.md#what-the-files-are).
- **It identifies its hardware**, reporting chip id and firmware strings through
  `GET_DEV_INFO` (1089).
- **It is a production build.** It answers `GF_ERROR_UNKNOWN_CMD` (1070) to
  commands that exist in the protocol but are not compiled into this build.

The name is not derivable. `gfenu` resembles Goodix's `gf` prefix followed by
part of this device's codename, `enuma`, but that is a partial match and nothing
in the image states a rule. Treat the name as something to be read from the
device rather than constructed: the device tree carries it as `firmware-name`,
and the sensor driver reports it at
[`/sys/.../firmware_name`](00-sensor-driver.md#interfaces).

The name is the lesser half of the portability problem, though. The image used
here was extracted from the stock operating system of the device it runs on, so
it is a given for that model. Whether the same image would load on a different
model, or on another manufacturer's device using the same sensor and SoC, is
untested. The `.mdt` carries a hash segment and the secure world does reject an
image that is not assembled exactly as expected, but that establishes integrity
checking, not that an image is bound to one model or one vendor.

Nor do we know how it is signed, or by whom, or what that means for
redistribution — [an open question](../README.md#what-is-not-understood), and the
one that decides whether any of this can be packaged rather than extracted per
device. Treat provenance as unresolved per model rather than assuming either
way.

`gfenu` is not the only trusted application in play. Enrolment expects an
authentication token signed by the gatekeeper application, `miriskm`, which runs
in the same secure world and holds the signing key — see
[the authentication token](#the-authentication-token).

What this document describes is the protocol such an application speaks, not the
image itself.

## Request header (128 bytes)

| off | size | field |
|---|---|---|
| 0 | 8 | payload **physical** address — the kernel patches this in. `gfenu`'s patch descriptor names four bytes, so only the low half is written and the upper half stays zero |
| 8 | 4 | token, caller-incremented. Nothing observed carries it back ([[our device]](../README.md#how-we-know)) |
| 12 | 4 | command class |
| 16 | 8 | timestamp, milliseconds **since midnight** (not since epoch) |
| 24 | 4 | logdump level — always 0; setting it non-zero changed nothing we could see ([[our device]](../README.md#how-we-know)) |
| 28 | 4 | unused; never seen non-zero ([[our device]](../README.md#how-we-know)) |
| 32 | 4 | command id |
| 36 | 4 | payload length |

## Response

At buffer offset 128:

| off | size | field |
|---|---|---|
| +128 | 4 | **status** — zero is `GF_SUCCESS` |
| +132 | 4 | *unused in practice; never seen non-zero* ([[our device]](../README.md#how-we-know)) |

The Android client ends its invoke wrapper with `ldr w19,[x24,#128]`, which is
where the status reading comes from ([[libdrmfs.so disassembly]](../README.md#how-we-know)).
The request's token at +8 is not echoed into either word
([[our device]](../README.md#how-we-know)).

Per-command data lives in the payload buffer. **Command bodies start at payload
+100**; the first 100 bytes are an application header whose fields are mostly
undocumented here, though the interrupt verdict at +12 is one of them.

## Command classes

The class at request +12 selects a dispatch table:

| class | table |
|---|---|
| 1 | normal path — enrol, authenticate, interrupts |
| 2 | client-level / key provisioning |
| 3 | factory test |
| 4 | image and template dump |

Classes 1 and 3 share the command number space, so the same id means different
things by class. This is a production build: it answers `UNKNOWN_CMD` (1070) to
every class-3 command that computes or commits a calibration.

## Commands used

| id | name | notes |
|---|---|---|
| 1000 | `DETECT_SENSOR` | payload 520 |
| 1001 | `INIT` | payload 348; triggers the storage reads |
| 1005 | `INIT_FINISHED` | payload 104 |
| 1006 | `PRE_ENROLL` | payload 112; returns the enrolment nonce |
| 1007 | `ENROLL` | payload 180; carries the auth token |
| 1008 | `POST_ENROLL` | ends an enrolment session. Android sends it before `REMOVE`; not required there |
| 1009 | `CANCEL` | ends an armed operation |
| 1010 | `AUTHENTICATE` | arms a match; see [Authentication](#authentication) |
| 1011 | `GET_AUTH_ID` | |
| 1012 | `SAVE` | persists the enrolled template |
| 1013 | `REMOVE` | unenrols one finger |
| 1014 | `SET_ACTIVE_GROUP` | payload 104; establishes group context |
| 1015 | `ENUMERATE` | payload 184; needs group context; see [Enumeration](#enumeration) |
| 1016 | `IRQ` | payload 327624; the event pump |
| 1017 / 1018 | `SCREEN_ON` / `SCREEN_OFF` | carry panel state; what the application does with it is not decoded. The [sensor driver](00-sensor-driver.md) has no route to the application and cannot send them: on the Android side [panel state goes to user space](00-sensor-driver.md#panel-state), so a client is what sends them here. See the [status list](../README.md#status) |
| 1086 | `AUTHENTICATE_FINISH` | payload 124; completes a match |
| 1089 | `GET_DEV_INFO` | payload 5500; chip id and firmware strings |

The table above lists what we have seen sent, with
the numbers they were sent as. `GF_CMD_LOCKOUT` is named in the image
([[gfenu symbols]](../README.md#how-we-know)) but we never saw it sent, so no
number is recorded for it.

## Bring-up

With the [file service](03-listener-services.md) already running and the
[sensor's char device](00-sensor-driver.md) held open so the sensor is
powered:

    DETECT_SENSOR (1000, 520)
    INIT          (1001, 348)   → geometry 160 x 36 at payload +104 / +108
    INIT_FINISHED (1005, 104)
    GET_DEV_INFO  (1089, 5500)  → "GF_CHIP_3626ZS1", part "A005203"

`INIT` is where the trusted application (`gfenu`) reads — and, on a device that has none, writes —
its calibration objects.

## Interrupts

The [sensor driver](00-sensor-driver.md) signals over netlink; the client
then pumps `IRQ` (1016)
until the mask comes back zero. **The handler drains one interrupt across
several 1016 calls, reusing the buffer without clearing it — do not `memset`
between calls.**

Three control fields must be armed in the payload (absolute, unaligned):

| off | value |
|---|---|
| 327049 | u32 `0x02000000` |
| 327096 | u32 `512` |
| 327616 | u32 `1` |

Response fields, as byte offsets into the payload buffer:

| off | field | when |
|---|---|---|
| 12 | image result: sample feedback or match verdict | image interrupts |
| 100 | interrupt bitmask | always |
| 104 | operation code | always |
| 136 | navigation code | navigation events |
| 327396 | group | enrolment and successful authentication |
| 327400 | finger id | enrolment and successful authentication |
| 327404 | samples remaining | enrolment |

The interrupt bits: `0x2` FINGER_DOWN, `0x4` FINGER_UP, `0x80` IMAGE,
`0x400` ONE_FRAME_DONE.

On an image interrupt, the command return status is the per-sample result and
the application mirrors it at payload `+12`. Qualcomm's `libgf_hal.so` consumes
the return status and maps selected non-zero Goodix values to Android
acquired-info notifications, including partial (`1`), imager dirty (`3`), too
slow (`4`), and vendor-defined values. A client should retain a non-zero `IRQ`
status as sample feedback rather than treating every non-zero result as a fatal
failure.

The concrete mappings in this Qualcomm HAL are:

| `IRQ` status | Android acquired-info |
|---:|---:|
| 1011, 1060 | partial (`1`) |
| 1012, 1052, 1058, 1101, 1104 | imager dirty (`3`) |
| 1117 | too slow (`4`) |
| 1045 `GF_ERROR_DUPLICATE_AREA` | vendor `1025` (duplicate area) |
| 1013 `GF_ERROR_DUPLICATE_FINGER` | vendor `1026` (duplicate finger) |
| 1094 `GF_ERROR_TOO_FAST` | vendor `1029` (too fast) |

The names above are not guesses from their ordering: the status values resolve
to those strings through `libgf_hal.so`'s exported `err_table`, and the IRQ
handler maps them to the indicated acquired-info values. The same handler also
emits vendor `1028` directly on a finger-up flag rather than in response to a TA
status. This is Goodix's `INPUT_TOO_LONG` acquired event: an older public Goodix
interface assigns that name to the corresponding event immediately before
duplicate-area and duplicate-finger, and the control flow here has the same
finger-up semantics.

There is no production acquired-info mapping for vendor `1027` in this HAL.
The sole literal `1027` is passed to the test-command dispatcher, not
`gf_hal_notify_acquired_info`; leave it unnamed unless another paired HAL is
found to use it.

These are HAL policy, not a claim that every Goodix generation assigns the
same meaning to every numeric TA status. A native client can expose the raw
status and apply the mappings appropriate to its paired TA/HAL generation.

A live enrolment confirmed the path: a partial press returned `1011`, placed
the same `1011` at payload `+12`, and left samples remaining unchanged at 25.
The Qualcomm HAL maps `1011` to acquired-info partial (`1`). An accepted press
returned zero and decremented samples remaining.

## Enumeration

Call `SET_ACTIVE_GROUP` first, then pass a 184-byte payload to `ENUMERATE`.
The response is two parallel arrays with room for ten entries:

| off | field |
|---:|---|
| 100 | number of entries |
| 104 | `u32 group[count]` |
| 144 | `u32 finger_id[count]` |

This layout is used by both `gfenu` and Qualcomm's `libgf_hal.so`. A live call
on this device returned group `0`, finger `0x1da3a0c6`. A shorter payload can
expose the count while truncating one or both arrays, so the full 184 bytes are
part of the command ABI.

## Enrolment

    PRE_ENROLL (1006)                     → nonce
    ENROLL     (1007) + auth token
    IRQ        (1016) × N                  ← a frame per finger press
    SAVE       (1012)                      when remaining == 0 and finger != 0
    SET_ACTIVE_GROUP (1014)
    GET_AUTH_ID      (1011)

Each interrupt carrying a frame updates three fields in the payload: the group
the finger is being enrolled into, the id the application has assigned it, and
how many further samples it still wants. The id is zero until the application
has enough of a template to commit to one, so it appears partway through a run
rather than at the start.

Enrolment is complete when **samples remaining reaches zero and the finger id is
non-zero**. Both conditions are needed. Samples remaining also reaches zero when
the application abandons an enrolment — a run of frames it cannot use, for
instance — and in that case the finger id stays zero and there is nothing to
save. Treating `remaining == 0` alone as success means calling `SAVE` with no
template behind it.

`SAVE`'s payload carries the group at +100 and the finger id at +104 — the
same two slots `REMOVE` uses. **Do not
send `POST_ENROLL` before `SAVE`**: it ends the enrolment session, leaving
nothing to persist.

Enrolling a finger that is already enrolled succeeded every time we tried it
([[our device]](../README.md#how-we-know)). The application is not indifferent to
the case, though: `GF_ERROR_DUPLICATE_FINGER` and `GF_ERROR_DUPLICATE_AREA` are
both defined in the image ([[gfenu symbols]](../README.md#how-we-know)). We never
saw either raised, so do not rely on duplicates being rejected — but equally, do
not assume they never will be.

## Authentication

    SET_ACTIVE_GROUP (1014) + group at payload +100
    AUTHENTICATE     (1010)
    IRQ              (1016) × N            ← verdict at payload +12
    AUTHENTICATE_FINISH (1086), payload 124 only on a match

Both `SET_ACTIVE_GROUP` and `AUTHENTICATE` take the group id at payload +100.
`AUTHENTICATE` does not consume the first 100 bytes returned by
`SET_ACTIVE_GROUP`.

**For authentication, the image result at interrupt payload +12 is the
verdict:** zero is a match,
`1064 GF_ERROR_MATCH_FAIL_AND_RETRY` is not. It does not say *which* finger
matched.

`AUTHENTICATE_FINISH` is a completion step, not a query. Its 124-byte payload
is:

| Offset | Size | Meaning |
|---:|---:|---|
| +104 | 4 | byte-swapped IRQ mask from IRQ payload +100 |
| +108 | 4 | byte-swapped operation from IRQ payload +104 |
| +112 | 4 | matched group from IRQ payload +0x4fce4 |
| +116 | 4 | matched finger id from IRQ payload +0x4fce8 |
| +120 | 1 | feature-study output |
| +121 | 1 | feature-study output |

The secure handler passes `+116`, `+120`, and `+121` to
`gf_algo_finger_feature_study`. FINISH is valid only after a successful match,
with the matched group and finger copied from the IRQ payload
([[our device]](../README.md#how-we-know)). Vendor traces never call it after an
ordinary mismatch ([[vendor trace]](../README.md#how-we-know)).

**`AUTHENTICATE` is one-shot.** An armed attempt ends with
`AUTHENTICATE_FINISH` on a match or `CANCEL` when the system gives up, and a
fresh `AUTHENTICATE` is required for the next attempt. Further presses after a
verdict raise no frames at all. When to arm is the driver's policy — see
[arm on demand](05-writing-a-client.md#things-that-will-bite).

## Removal

    POST_ENROLL (1008)                     Android sends this first
    REMOVE      (1013)                     finger id as int32 at payload +104,
                                           group at +100

The application reads the template back, then deletes both the object and its
`_bak` copy.

`POST_ENROLL` ends an enrolment session, and the Android HAL sends it before
every `REMOVE` ([[vendor trace]](../README.md#how-we-know)). It is not required:
the reference client omits it and removal succeeds
([[our device]](../README.md#how-we-know)). Send it if a removal may follow an
enrolment in the same session.

## Error codes

| id | name |
|---|---|
| 1001 | `GF_ERROR_OUT_OF_MEMORY` |
| 1003 | `GF_ERROR_BAD_PARAMS` |
| 1009 | `GF_ERROR_PREPROCESS_FAILED` |
| 1011 | `GF_ERROR_ACQUIRED_PARTIAL` |
| 1033 | `GF_ERROR_OPEN_SECURE_OBJECT_FAILED` |
| 1035 | `GF_ERROR_WRITE_SECURE_OBJECT_FAILED` |
| 1047 | `GF_ERROR_FINGER_NOT_EXIST` |
| 1057 | `GF_ERROR_UNTRUSTED_ENROLL` |
| 1064 | `GF_ERROR_MATCH_FAIL_AND_RETRY` |
| 1070 | `GF_ERROR_UNKNOWN_CMD` |
| 1088 | `GF_ERROR_NO_GROUP_ID` |

`1035` is raised at exactly one place, when a storage write returns a byte count
that does not equal the length requested. `1033` is an open failure. The
distinction is useful: `1035` means the write was attempted.

Those are the codes we have actually seen. The image defines 116
([[gfenu symbols]](../README.md#how-we-know)), and one group of them is worth
knowing about even unobserved, because it is the vocabulary the application has
for rejecting a sample:

    GF_ERROR_ACQUIRED_PARTIAL          GF_ERROR_ACQUIRED_IMAGER_DIRTY
    GF_ERROR_ALGO_DIRTY_FINGER         GF_ERROR_ALGO_COVER_BROKEN
    GF_ERROR_ALGO_PALM_DETECT          GF_ERROR_ALGO_INVALID_DATA
    GF_ERROR_TOO_FAST                  GF_ERROR_TOO_SLOW
    GF_ERROR_DUPLICATE_AREA            GF_ERROR_DYNAMIC_ENROLL_INVALID_PRESS_TOO_MUCH
    GF_ERROR_DYNAMIC_ERNOLL_INCOMPLETE_TEMPLATE   (sic)

These names do not all have values assigned here, but the transport is known:
the return status of each `IRQ` invocation carries the sample result. Qualcomm's
`libgf_hal.so` dispatches that status through its acquired-info mapping before
continuing the IRQ drain. An enrolment frame that is rejected also does not
decrement the samples-remaining count — in one run, 138
finger-down/up events produced 10 accepted samples
([[our device]](../README.md#how-we-know)).

## The authentication token

`ENROLL` carries a packed 69-byte `hw_auth_token_t` at payload +110:

| off | size | field |
|---|---|---|
| +0 | 1 | version |
| +1 | 8 | challenge (LE) — the nonce `PRE_ENROLL` returned |
| +9 | 8 | user id (LE) |
| +17 | 8 | authenticator id (LE) |
| +25 | 4 | authenticator type (**big endian**) |
| +29 | 8 | timestamp (**big endian**) |
| +37 | 32 | HMAC |

Under the Android stack, clearing only the HMAC turns `GF_SUCCESS` into
`1057 GF_ERROR_UNTRUSTED_ENROLL`, so the token is genuinely verified there. The
signing key belongs to the gatekeeper application (`miriskm`), so a token has to
be obtained rather than constructed.

On this device, in a state where no gatekeeper credential had ever been
provisioned, an unsigned token was accepted ([[our device]](../README.md#how-we-know)) — which is what makes
the reference client in [`harness/`](../harness/) work without one. Whether that holds once a credential exists, or on
another model or firmware version, we have not tested. **Do not rely on this**
— see [what a client author should know](05-writing-a-client.md#things-that-will-bite).
