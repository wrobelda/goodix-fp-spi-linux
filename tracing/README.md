# Tracing the vendor stack

Frida scripts used to capture the Android reference behaviour. Needed only to
verify something against the vendor implementation; the protocol they
established is written up in `../docs`.

The two channels live in different processes, so a single attach sees half the
conversation:

| script | target | channel |
|---|---|---|
| `android-gfhook.js` | fingerprint HAL | client → trusted application |
| `android-listener-hook.js` | `qseecomd` | trusted application → file service |
| `android-gkhook.js` | gatekeeper HAL | gatekeeper (CBOR) |

`android-capture-both.py <seconds> <outdir>` drives the first two at once and
timestamps both against a common start, which is what makes them interleavable.

    python3 android-capture-both.py 600 ./capture

Notes:

- Attach to `qseecomd`, not the fingerprint HAL. The Goodix libraries register
  no listener; their only `listener` symbol is `gf_hal_nav_listener`, which is
  swipe navigation.
- `qseecomd` serves every listener over one transport, so filter on the handle
  reported per request to tell them apart.
- **`android-gfhook.js` truncates dumps at 1024 bytes** against a 327624-byte
  interrupt payload. That covers the request header and the start of the
  payload, and nothing beyond. The fields `../docs` documents further out — the
  interrupt control words near 327049 and the enrolment fields near 327396 —
  were found with our own client, not with these hooks. Raise the cap before
  trusting the dumps further out, and note nothing in the output marks a dump as
  truncated.
- Frida 17 removed `Module.findExportByName`; use the module-instance method.
  The CLI exits when stdin closes under `nohup`, so use the Python driver.
