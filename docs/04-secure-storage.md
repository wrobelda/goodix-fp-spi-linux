# Layer 4: secure storage

The trusted application
([`gfenu`](02-ta-protocol.md#the-application)) persists everything through
[the listener services](03-listener-services.md), as sealed objects.

The payloads are encrypted and integrity-protected with a device-bound
TrustZone key. A client stores and returns ciphertext and cannot read it. The
objects are portable between operating systems on the *same* device, and they
are useless anywhere else.

## Object format

Every object satisfies:

    file_size == first_u32 + 0x34

This is a four-byte payload length inside a 52-byte wrapper. The application
checks the format on the way in:

- `fts_open()` reads exactly four bytes from offset 0.
- It refuses the object if it gets anything other than four bytes back.
- It rejects the object if `length + 0x34` exceeds `0x4b000`
  (`GF_FTS_TEMPLATE_MAX_SIZE`, 307200).

This has two consequences for a client:

- **An empty file is worse than an absent one.** An absent file takes the
  create path. A present-but-empty file fails the four-byte header read and
  yields `1033 GF_ERROR_OPEN_SECURE_OBJECT_FAILED`. Never pre-create
  placeholders.
- **A read must return the full requested length.** A short or zero return
  counts as failure.

Observed sizes, for scale:

| object | file size | payload length (`first_u32`) |
|---|---|---|
| `finger_0_0.so` | 120202 | 120150 |
| `gf_calibration.so` | 23220 | 23168 |
| `gf_nav_base.so` | 3256 | 3204 |
| `gf_otp_info.so` | 116 | 64 |
| `gf_fdt_base.so` | 80 | 28 |
| `auth_token_0.so` | 70 | 18 |

## The file set

The application names the objects under `/data/vendor/fpdump/`. Those are the
names it asks for, as absolute Android paths. The listener service passes them
through as they are; it does not resolve them against a root of its own.
Because of this, a Linux client can put the whole set under one directory of
its choosing — see
[listener 28672](03-listener-services.md#listener-28672--globalplatform-file-system):

| object | written | purpose |
|---|---|---|
| `gf_otp_info.so` | at init | sensor OTP data |
| `gf_calibration.so` | at init, then continuously | imaging baseline |
| `gf_nav_base.so` | at init | navigation baseline |
| `gf_fdt_base.so` | at init | finger-detect baseline |
| `gf_data_process_info.so` | — | requested, never written here |
| `auth_token_0.so` | at save | one per group |
| `finger_<group>_<n>.so` | at save | template, slots 0–4 |
| `finger_<group>_<n>_bak.so` | at save | backup copy. An object the application names and writes itself, not the `<name>.bak` produced by the listener protocol's backup flag |

There is also `/persist/data/fingerprint_version`. It is written through the
plain `open`/`write` path on listener 10 rather than as a sealed object. It is
the one file that works without `gpfs`, which makes it a useful smoke test.

It is also the only thing that ever gets written on listener 10: in a full
authentication run there were 3 writes there against 581 opens, and all of
them were this file ([[our device]](../README.md#how-we-know)). See
[why there are two file services](03-listener-services.md#why-two-file-services).

Note there is no `auth_token.so`; only `auth_token_0.so` exists. The
application probes both.

## Neighbouring paths in the stock operating system

The paths above are the complete file set observed for this Goodix TA. The
stock operating system also contains nearby paths which belong to other
trusted applications or to the Android HAL, so they must not be treated as
additional `gfenu` template objects:

| path | observed role |
|---|---|
| `/persist/data/mfido/` | three files in a directory named for the FIDO service ([[our device]](../README.md#how-we-know)) |
| `/persist/data/Mlipay/` | one file in a directory named for Mlipay ([[our device]](../README.md#how-we-know)) |
| `/persist/goodix` | an empty directory on the reference platform ([[our device]](../README.md#how-we-know)) |
| `/data/gf_data/` | an auxiliary path named by `libgf_hal.so`, not a sealed-object path used for the templates documented here ([[vendor client disassembly]](../README.md#how-we-know)) |

The stock `/persist/data` tree also contains obfuscated directory names with a
`vTable`, data files, and a `.bak` for each file. These are QSEE Secure File
System objects belonging to other trusted applications. A read-only inspection
of the reference platform's stock `persist` partition confirmed these paths
and backup pairs. They show that the same listener-backed store serves several
TAs, but they do not extend the Goodix file set or its namespace
([[our device]](../README.md#how-we-know)).

## Calibration is generated

We found no command that produces calibration, and none is needed. The
application maintains its baseline during ordinary operation and persists it
through this same path.

The four baseline objects are OTP, calibration, navigation and finger-detect.
They appear during `INIT` on a device that has never had any, and
`gf_calibration.so` is rewritten continuously during use
([[our device]](../README.md#how-we-know)).

So there is nothing to import, and importing is harmful: a served copy of
someone else's calibration hides whether generation works. Start with the
directory empty and let the application populate it.

## Lifecycle observed

- **enrol, empty store** → creates `finger_0_0.so` and its backup
- **enrol, populated store** → reads the existing template, takes the next free
  slot, rewrites the existing backup
- **remove** → reads the template back, deletes both the object and its backup
- **reboot** → objects are re-read and unsealed; nothing is regenerated

Slots are allocated lowest-free-first, not by overwriting.

## Application index versus storage

The storage filenames are not the application-level index. To get the index,
ask the application with `ENUMERATE` (1015) after setting group context.

The 184-byte response contains:

- the count at `+100`
- group ids at `+104`
- finger ids at `+144`

Each array has ten slots. On this device the application returned group `0`,
finger `0x1da3a0c6` ([[our device]](../README.md#how-we-know)). The full
184-byte payload is required, because a SAVE-sized buffer truncates the
finger-id array.
