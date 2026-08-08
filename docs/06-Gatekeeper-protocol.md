# Qualcomm Gatekeeper through `keymaster64`

This document records the observed private protocol used by Xiaomi's Android 13
Gatekeeper HAL on this device. It is useful beyond fingerprint support: it is a
hardware-backed credential verifier that can issue signed, challenge-bound
authentication tokens without exposing its signing key.

This is not the Android Gatekeeper HAL specification. It is the wire protocol
of one Qualcomm Keymaster application and the normal-world libraries shipped on
this device. Treat every result as platform-specific until confirmed elsewhere.

## What runs where

The trusted application is the signed `km41.mbn` image stored byte-for-byte in
both `keymaster_a` and `keymaster_b`. The bootloader loads it as a resident QSEE
application; clients open it under the name `keymaster64`. It must not be passed
to the dynamic-application loader used for `gfenu`.

`miriskm` is unrelated. It is a Xiaomi risk-management application whose
strings and diagnostics concern device status, registration tokens,
certificates, and remote authentication.

The credential is split across trust boundaries. Android stores password
handles, KDF parameters, salt, and synthetic-password blobs under `/data`. The
password handles are authenticated opaque blobs, not plaintext credentials.
The non-exportable verification and HAT-signing secrets belong to Keymaster in
secure world. Gatekeeper is also the appropriate layer for secure throttling,
but this implementation's retry behaviour has not yet been characterised.
Switching boot slots does not erase either Android userdata or the resident
application's secrets.

## Transport and version negotiation

The shipped Qualcomm `KeymasterUtils` client opens `keymaster64` with a
`0xa000`-byte shared buffer. Calls use the QSEECOM-compatible TEE interface with
function 0 and two memory references: request input and response output.

Before sending Gatekeeper CBOR commands, the client performs this handshake:

| command | request | response |
|---|---|---|
| `0x00000200` | command word | status, TA API major/minor, TA major/minor |
| `0x00000207` | `{ command, 4, 5, 4, 5, 0 }` | status |

The values are little-endian 32-bit words. The live application reported API
4.1 and TA version 4.558, then accepted the `0x0207` request. Disassembly of
`libkeymasterdeviceutils.so` produced the same six-word request. The vendor
Gatekeeper object constructs this versioned client before issuing Gatekeeper
commands, so negotiation is part of the usable protocol rather than optional
discovery.

The Android QSEECom wrapper places the response after the request at the next
four-byte-aligned address in its shared buffer. The Linux TEE probe used
separate request and response memory references and received the same results.

## Gatekeeper CBOR commands

Each Gatekeeper request begins with a little-endian 32-bit command followed by
a CBOR map. Responses begin with a signed 32-bit status and an unsigned 32-bit
CBOR length, followed by the CBOR object.

| command | observed map |
|---|---|
| `0x00021001` enroll | request `{ 3: uid, 8: desired_password }`; success is expected to return key 6, the password handle |
| `0x00021002` verify | request `{ 3: uid, 4: challenge, 6: handle, 9: password }` |
| `0x00021003` unknown | request `{ 3: uid }`; returned a large response whose purpose is not decoded |

Optional fields accepted by the vendor enrol serializer are key 5 for the
current password handle and key 7 for the current password. Key 3 is encoded as
an unsigned 64-bit UID. Keys 5 through 9 above are CBOR byte strings where
applicable.

A successful verify response was observed as a seven-entry map:

| key | meaning |
|---|---|
| 11 | status |
| 4 | challenge |
| 12 | secure user id |
| 13 | authenticator id |
| 14 | authenticator type |
| 15 | timestamp |
| 16 | 32-byte HMAC |

The returned authenticator type and timestamp are already represented so that
copying their integer storage into `hw_auth_token_t` produces its required
big-endian fields. The challenge and IDs occupy the token's little-endian
fields. Do not apply an additional byte swap without checking the serialised
bytes.

The password handle observed on this build is 58 bytes. Its secure user id
begins at byte 1 in little-endian order. The blob must otherwise be treated as
opaque.

## Android credential flow

Android does not submit the literal lock-screen PIN to `0x21002`. Its synthetic
password manager owns two Gatekeeper credentials involved in the observed
flow:

1. It verifies fake UID 100000 using a 58-byte handle stored in the `.pwd`
   record and a 64-byte derived input. This unlocks the synthetic-password
   material.
2. It derives a separate stable 32-byte Gatekeeper password from the synthetic
   password and verifies UID 0 with another 58-byte handle.
3. For fingerprint enrolment, the UID-0 verification carries the challenge
   returned by Goodix `PRE_ENROLL`. Its response is converted to a packed
   69-byte `hw_auth_token_t` and passed to Goodix `ENROLL`.

The stock trace showed the same challenge in all three places: Goodix
`PRE_ENROLL`, CBOR key 4 of the UID-0 verify request, and the returned HAT.
Goodix accepted `ENROLL` before Android requested the first sensor touch.

The `.pwd` record contains the credential type, base-two logarithms of the
scrypt parameters, a salt, and the fake-UID password handle. On the tested
Android 13 installation the derivation was reproduced exactly as:

    password_token = scrypt(UTF8(PIN), salt,
                            N = 1 << logN,
                            r = 1 << logR,
                            p = 1 << logP,
                            output = 32 bytes)

    gatekeeper_input = SHA-512(
        "user-gk-authentication" padded with zeroes to 128 bytes
        || password_token)

The resulting 64 bytes matched the live Android request byte-for-byte. This
does not by itself recover the UID-0 input: that value is derived from the
synthetic password after its protected blob has been unlocked.

## Relationship to `gfenu`

The signed HAT is an authorisation proof: a trusted credential authority
verified a credential and signed a particular fresh challenge. It prevents an
ordinary caller that can reach the fingerprint protocol from authorising its
own enrolment merely by requesting a nonce.

This `gfenu` build also accepts a token in which only the fresh challenge is
populated. That was tested both before and after provisioning Android's
lock-screen credential; a bogus `SAVE` immediately afterwards returned
`GF_ERROR_FINGER_NOT_EXIST`, confirming the successful `ENROLL` status without
creating a template. Gatekeeper provisioning therefore does not toggle
`gfenu` into a stricter mode.

That permissive behaviour is not a reason to collapse credential handling into
the fingerprint client. Without a signed token, enrolment authorisation ends in
normal-world Linux policy. With a signed token, the boundary extends into
secure world. A production client should request a token from a separate
credential service when one exists, and should retain the challenge-only route
only as a documented platform-specific fallback.

## Linux results and remaining work

A temporary Linux client successfully opened `keymaster64`, completed
`0x0200`/`0x0207`, and exchanged Gatekeeper CBOR. Attempts to enrol ASCII
`"1234"` under UID 0 and an unused UID returned status `-30`,
`KM_ERROR_VERIFICATION_FAILED`; no handle was created. The Android trace later
proved that a literal PIN is not the normal input, so this negative result does
not show that native enrolment is unavailable.

The next protocol work is:

- enrol a Linux-owned UID with Android-shaped 32- and 64-byte derived values;
- decode successful enrol, reenrol using keys 5 and 7, and credential deletion
  or reset semantics;
- verify persistence across reboot and rejection of an incorrect credential;
- characterise retry, timeout, and secure throttling responses;
- convert a live verify response into a canonical HAT and have `gfenu` accept
  it end to end;
- determine which other trusted applications accept tokens from the same
  signing authority.

A reusable implementation belongs in a small Gatekeeper service or library,
not in the Goodix driver. PAM, polkit, or another login service could own
credential enrolment and verification; HAT consumers would supply a challenge
and receive only the completed token.

## Related implementations

Android specifies the Keymaster and Gatekeeper HAL interfaces, not this private
CBOR protocol. Qualcomm's
[Keymaster 0.3 QSEE client in AOSP](https://android.googlesource.com/platform/hardware/qcom/keymaster/+/refs/heads/main/)
documents an earlier four-command application rather than `km41`.
[Samsung Keybuster](https://github.com/shakevsky/keybuster) demonstrates direct
Keymaster access with a different vendor protocol. A standalone
[Qualcomm device-ID provisioner](https://gist.github.com/MhmRdd/6c4256f4853674c6e330793138f93eb1)
independently uses the same `0x0200`/`0x0207` negotiation observed here.
