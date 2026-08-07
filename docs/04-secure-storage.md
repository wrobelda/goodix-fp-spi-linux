# Layer 4: secure storage

Everything the trusted application
([`gfenu`](02-ta-protocol.md#the-application)) persists goes through
[the listener services](03-listener-services.md), as sealed objects. The payloads are encrypted and integrity-protected
with a device-bound TrustZone key: a client stores and returns ciphertext and
cannot read it. They are portable between operating systems on the *same*
device, and useless anywhere else.

## Object format

Every object satisfies:

    file_size == first_u32 + 0x34

A four-byte payload length inside a 52-byte wrapper. The application checks this
on the way in — `fts_open()` reads exactly four bytes from offset 0, refuses the
object if it gets anything other than four bytes back, and rejects it if
`length + 0x34` exceeds `0x4b000` (`GF_FTS_TEMPLATE_MAX_SIZE`, 307200).

Two consequences for a client:

- **An empty file is worse than an absent one.** Absent takes the create path;
  present-but-empty fails the four-byte header read and yields
  `1033 GF_ERROR_OPEN_SECURE_OBJECT_FAILED`. Never pre-create placeholders.
- **A read must return the full requested length.** A short or zero return
  reads as failure.

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

Named by the application under `/data/vendor/fpdump/`. Those are the names it
asks for, as absolute Android paths; the listener service passes them through as
they are rather than resolving them against a root of its own, so a Linux client
can put the whole set under one directory of its choosing — see
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

Plus `/persist/data/fingerprint_version`, which is written through the plain
`open`/`write` path on listener 10 rather than as a sealed object — it is the
one file that works without `gpfs`, which makes it a useful smoke test. It is
also the only thing that ever gets written on listener 10: in a full
authentication run, 3 writes there against 581 opens, all of them this file
([[our device]](../README.md#how-we-know)). See [why there are two file services](03-listener-services.md#why-two-file-services).

Note there is no `auth_token.so`; only `auth_token_0.so`. The application probes
both.

## Calibration is generated

We found no command that produces calibration, and none is needed: the
application maintains its baseline during ordinary operation and persists it
through this same path. The four baseline objects — OTP, calibration,
navigation and finger-detect — appear during `INIT` on a device that
has never had any, and `gf_calibration.so` is rewritten continuously during use
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

## What is not discoverable from storage

One of the gaps in the [status list](../README.md#status) is here.

Nothing found so far reports which fingers the application believes are
enrolled. `ENUMERATE` (1015) succeeds once group context is set but returns a
descriptor with no ids in it, only a count: its `+100` reports how many fingers
are enrolled ([[our device]](../README.md#how-we-know)). Listing the directory tells you which slots
have objects, which is not the same thing as asking the application.

`DUMP_TEMPLATE` (1079, class 4, payload 409612) follows every match in the
vendor trace and is the obvious place to look next.
