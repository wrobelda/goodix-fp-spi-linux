# Layer 0: the sensor driver

Everything else in these documents talks to the trusted application. This layer
is underneath all of it: the kernel driver that owns the sensor's power, reset
line and interrupt, and does nothing else.

Goodix ships these sensors in two modes, served by
[two different downstream sources](#one-driver-family-many-parts). In the
first, REE mode, the operating system drives the sensor over SPI and does the
imaging itself. In the one this project deals with, TEE mode, a trusted
application inside the secure world drives the sensor instead, and the
operating system never touches that bus. The normal world is left holding the parts of the
hardware the secure world does not manage for itself. That is what this driver
is: not a sensor driver in the usual sense, since it never sees an image or a
byte of sensor traffic.

Two implementations of that mode are known for these sensors, differing
in which secure world the application runs in. The Qualcomm one is what these
documents describe: QSEE, reached over QSEECOM, with the command set in
[the trusted application's protocol](02-ta-protocol.md). The MediaTek one runs
the application under a MicroTrust TEE, and its driver
([`goodix_cap/gf_spi_tee.c`](https://github.com/XagaForge/android_kernel_xiaomi_mt6895/blob/16.2/drivers/input/fingerprint/goodix_cap/gf_spi_tee.c))
opens the session from the kernel rather than leaving it to user space.

Source: `drivers/input/misc/goodix_fp_spi.c`, on the
[`goodix-fp-spi`](https://github.com/wrobelda/linux/tree/goodix-fp-spi) branch of
[wrobelda/linux](https://github.com/wrobelda/linux), together with its device
tree binding. That branch is this driver alone, off mainline; the TEE driver is
a separate series on [`qcom-qseecom-tee`](https://github.com/wrobelda/linux/tree/qcom-qseecom-tee).

## Provenance and licence

Unlike the rest of this project, this driver is **not** clean-room work. It is a
backport of Goodix's downstream driver, taken from the Xiaomi msm-5.4 /
lineage-22.2 kernel (`drivers/input/fingerprint/goodix/`, the "platform bus" TEE
variant; the same tree carries a `goodix_fod/` copy for the under-display
sibling handset), and adapted for mainline: gpiod instead of raw GPIO numbers, a regular
`vdd` regulator, `miscdevice` instead of hand-rolled chrdev boilerplate,
`usleep_range()` in place of `msleep()` for the two sub-20ms waits — the 3 ms
reset pulse and the 10 ms settle after power-up, both of which `msleep()` rounds
up to the timer tick — and the removal of several dead code paths. The Xiaomi DRM blank notifier was not
carried over either, but for a different reason: the copy it comes from already
compiles it out, and it builds on a vendor display API mainline does not have —
see [what the notifier actually does](#panel-state).

Goodix maintain more than one source for this part, and the choice of base
matters. A MediaTek-platform copy of the same sensor exists — `goodix_cap/`,
2631 lines against 1114 — and shares this driver's shape: eleven of its function
names, and all fifteen of its ioctls with three more besides. It would have been
the wrong thing to backport. It binds as an SPI device rather than a platform one, carries
the SPI transfer and normal-world imaging paths, and opens the trusted
application from the kernel itself, holding the application's UUID as a literal
and calling a TEE vendor's client API. The Qualcomm copy used here knows
nothing about the trusted application at all, which is the division of labour
this project depends on: the driver owns hardware, and reaching the secure
world is user space's business through
[the TEE driver](01-kernel-tee-driver.md).

It is `GPL-2.0-only` and carries its original copyrights, `Goodix (2016-2017)`
and `Xiaomi, Inc. (2022)`, with the original authors retained in `MODULE_AUTHOR`.
That is the correct footing for upstreaming derived code, but it does mean any
submission has to preserve that attribution and be presented as an adaptation
rather than as new work.

The distinction matters because the two halves of this project have different
provenance: the protocol documented in `docs/01`–`docs/05` was reverse
engineered from observation and disassembly, whereas this driver is inherited
source.

## One driver family, many parts

Goodix does not publish this driver; [github.com/goodix](https://github.com/goodix)
carries their touchscreen drivers only. It reaches devices by being copied into
OEM kernel trees — GitHub's code search indexes 1028 files named
[`gf_spi.c`](https://github.com/search?q=filename%3Agf_spi.c&type=code),
spanning more than six hundred repositories — and each tree carries the copy its
own device needed, local fixups included, so no copy is authoritative.

The copies are one family carrying almost all of the same code: the same supply,
reset line and interrupt, the same character device, netlink socket and ioctls.
Some serve the capacitive GF parts, as on the
[reference platform](../README.md#reference-platform) used for this
project; others the in-display GW parts, and the two differ mostly in board power
and pin configuration: in the reference platform's tree the under-display
[`goodix_fod/`](https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/lineage-22.2/drivers/input/fingerprint/goodix_fod/gf_spi.c)
copy sits beside the
[`goodix/`](https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/lineage-22.2/drivers/input/fingerprint/goodix/gf_spi.c)
copy this driver derives from, differing by an extra 3.2 V rail and the
display-notifier symbols it binds to. Three findings about the family matter to this driver:

**Both modes exist, but not in one file.** The copies Qualcomm devices carry —
around a thousand lines, named `gf_spi.c`, and what this driver is backported
from — contain no capture code whatsoever. The capture code lives in the much
larger MediaTek variant, `gf_spi_tee.c`, which also opens the trusted
application from the kernel where the Qualcomm copies leave the secure world
entirely to user space. See [normal-world imaging](#normal-world-imaging-ree).

**Bus attachment is independent of mode.** The same sources pick `spi_driver`
or `platform_driver` from `USE_SPI_BUS` / `USE_PLATFORM_BUS`, so a sensor
driven from the secure world may still be described as an SPI child on some
boards. The gf3626 on the reference platform is a platform node.

**Two ioctl generations exist.** The older uses magic `'G'` and thirteen calls;
the newer magic `'g'` and fifteen, listed under [interfaces](#interfaces). The
gf3626 uses the newer one.

## Panel state

The downstream family carries a display-blank notifier, and it exists to wake
the display sooner, not to serve the sensor. On screen-off the driver sets an
`fb_black` flag, arms `wait_finger_down`, and reports the blank to user space as
`GF_NET_EVENT_FB_BLACK` on the same netlink socket as finger events. The first
finger to land while the screen is off then calls
`dsi_bridge_interface_enable()`, starting the panel wake at the moment of touch
instead of after the match comes back. Nothing on this path reaches the trusted
application.

This driver does not carry it, and neither does the copy it derives from: both
copies in the reference platform's tree compile it out with
`GOODIX_DRM_INTERFACE_WA`, because `dsi_bridge_interface_enable()` is an OEM
symbol. Copies in other trees leave it enabled, registering with vendor or fbdev
notifiers mainline does not have; reinstating it would mean using
`drm_panel_follower`, with `i2c-hid` as the worked example.

## Normal-world imaging (REE)

Everything else here assumes the secure world does the imaging. Goodix's sources
call the other mode REE, the rich execution environment, as against the
TEE: the operating system drives the sensor over SPI and reads the frames
itself. Nothing in this project implements it and the driver backported here
cannot, but the downstream code shows exactly what it would take.

**Where the code is.** Not in the `gf_spi.c` copies Qualcomm handsets carry,
which hold no transfers at all. It is in the MediaTek-lineage
[`gf_spi_tee.c`](https://github.com/fukehan/kernel-4.9/blob/master/drivers/input/fingerprint/goodix/gf_spi_tee.c),
behind `#ifdef SUPPORT_REE_SPI` and, for the read path, a second sensor-family
guard `SUPPORT_REE_OSWEGO`. It is rare: eight of those 1028 copies mention the
symbol at all.

**What it does.** `gf_read()` on the character device reads a status register at
0x8140 and gives up unless the sensor reports a frame ready, logging "no image
data available". Otherwise it raises the SPI clock, bulk-reads `count + 10`
bytes from the same register, sums a 16-bit checksum over the payload and
compares it against the last two bytes, copies the frame to user space past an
eight-byte header, and drops the clock back. The same helpers —
`gf_spi_read_bytes_ree()`, `gf_spi_write_bytes_ree()`,
`gf_spi_setup_conf_ree()` — also serve chip detection and firmware flashing, and
the factory-test sibling `gf_spi_factory.c` drives the sensor through
`spi_sync()` directly.

**How far it is from this driver.** Not far in shape. The REE-capable source for
this same part shares eleven of this driver's function names and every one of
its fifteen ioctls. What it adds is transfer: `GF_IOC_TRANSFER_CMD`,
`GF_IOC_TRANSFER_RAW_CMD` and `GF_IOC_SPI_INIT_CFG_CMD`, and the read path
above.

**What it would change here.** The node stops being a bare platform node. The
REE-capable source registers an `spi_driver` where this one registers a
`platform_driver`, so the sensor would be described as an SPI child with a `reg`
and a maximum frequency, and [the binding](#the-binding) would have to grow that
form. `firmware-name` would have nothing to name on a board where no trusted
application is involved, though whether matching also moves to the normal world
is not something these sources settle — they end at handing a frame to user
space.

## What it owns

| resource | why the normal world holds it |
|---|---|
| `vdd` regulator | the sensor is unpowered until something enables it |
| reset line | the secure world expects the part already out of reset |
| interrupt | finger events have to reach a userspace client, which lives here |

It also carries one thing that is not a resource: the name of the trusted
application paired with this sensor. That pairing is board knowledge, so it
belongs in the device tree, and this driver is what reads it and passes it on to
user space.

## Device tree

All of that comes from the device tree, which is the only part of this project
described there. As the draft binding defines it:

    fingerprint {
            compatible = "goodix,gf3626";

            interrupt-parent = <&tlmm>;
            interrupts = <23 IRQ_TYPE_EDGE_RISING>;

            reset-gpios = <&tlmm 125 GPIO_ACTIVE_LOW>;
            vdd-supply = <&vreg_l10a_3p0>;

            firmware-name = "gfenu";

            pinctrl-names = "default";
            pinctrl-0 = <&fp_int_default>;
    };

`vdd-supply` is the sensor regulator, held on for as long as the char device is
open; `interrupts` is the finger-down/up line relayed over netlink; and
`firmware-name` is the trusted application this sensor is paired with.

`firmware-name` is optional. Without it the sensor works, but a client has to
learn the application's name some other way.

### The binding

The node above is defined by the draft binding at
[`Documentation/devicetree/bindings/input/goodix-fingerprint.yaml`](https://github.com/wrobelda/linux/blob/goodix-fp-spi/Documentation/devicetree/bindings/input/goodix-fingerprint.yaml),
on the same branch as the driver. There is no binding for this sensor upstream, and none for any Goodix
fingerprint sensor: this would be the first, and it gates upstreaming the
driver.

It covers the family, since
[the driver already does](#one-driver-family-many-parts), and further parts
belong in it as they are tested. Three of its choices:

- **`goodix,gf3626`, not `goodix,fingerprint`** — a compatible names a part,
  not a category, whatever the downstream driver matches.
- **`interrupts`, not `irq-gpios`** — the driver calls `gpiod_to_irq()` on that
  line, and `interrupts` is the standard spelling of it.
- **`firmware-name`** carries the trusted application's name. The pairing is
  board knowledge, and the property has precedent in other input bindings
  (`silead,gsl1680`, `apple,z2-multitouch`).

## Interfaces

**`/dev/goodix_fp`**, a miscdevice, carrying ioctls with magic `'g'`. The
downstream driver defines fifteen; the bring-up sequence a client actually needs
is four, in this order and **under a single open**:

| order | ioctl | nr | effect |
|---|---|---|---|
| 1 | `GF_IOC_ENABLE_POWER` | 7 | enable the regulator |
| 2 | `GF_IOC_RESET` | 2 | pulse the reset line |
| 3 | `GF_IOC_ENABLE_SPI_CLK` | 5 | permit the secure world's SPI traffic |
| 4 | `GF_IOC_ENABLE_IRQ` | 3 | start delivering finger events |

Closing the descriptor undoes it, so the client has to hold it open for as long
as it wants the sensor alive. The remaining ioctls — `GF_IOC_INIT`, `EXIT`,
`DISABLE_*`, `INPUT_KEY_EVENT`, `ENTER_SLEEP_MODE`, `GET_FW_INFO`, `REMOVE`,
`CHIP_INFO`, `NAV_EVENT` — exist for the Android HAL's benefit and are not
needed for any flow documented here.

**`/sys/.../firmware_name`**, which reports the `firmware-name` from the device
tree, so a client does not have to read the device tree itself.

**A netlink socket**, protocol 25, on which the driver sends a one-byte event
`GF_NET_EVENT_IRQ` (1) from its interrupt handler. That is the entire protocol:
the message says an interrupt happened and nothing more. A client learns *what*
happened by then asking the trusted application, with `IRQ` (1016) — see
[how interrupts are serviced](02-ta-protocol.md#interrupts).

## What a client does with this

Power the sensor, hold the descriptor open, listen on the netlink socket, and
pump [`IRQ` (1016)](02-ta-protocol.md#interrupts) into the trusted application on
every event. The driver is a doorbell; the answer is always in the secure
world.

## Upstreaming

This driver is backported downstream code and is not upstream. Two things in the
interface above will not survive review as they stand:

- **The netlink protocol number is squatted.** Assigned families in
  `include/uapi/linux/netlink.h` stop at `NETLINK_SMC` (22); this driver claims
  25, an unallocated number in a globally-shared uapi namespace. A finger-down
  event is an input event, and reporting it through the input subsystem — or
  failing that, a poll-able char device — avoids the question entirely.
- **The ioctl surface is a vendor ABI.** Fifteen ioctls, most of them unused,
  several of which (`ENABLE_SPI_CLK`, `INPUT_KEY_EVENT`, `NAV_EVENT`) encode
  assumptions from the Android stack rather than anything the hardware requires.

Neither affects the work documented elsewhere here, which goes through the TEE
subsystem and does not depend on this driver's ABI beyond the four ioctls above.
