# piewifi

WiFi for PIE, in three kexts:

- `PIE80211Family.kext` is OpenBSD's net80211, the 802.11 stack, for a
  station. It does WPA2-PSK in the kernel.
- `PIEbwfm.kext` runs OpenBSD's bwfm driver for the CYW43455 of the
  Raspberry Pi 4 and 500, on SDIO. bwfm attaches its own 802.11 state to
  the family and drives net80211 as in OpenBSD.
- `PIErtw88.kext` drives the RTL8821CS in the RG353V internal SDIO slot.
  It uses the BSD-licensed rtw88 hardware implementation and tables with
  PIE's native SDIO and station interfaces.

## RG353V station support

The initial RTL8821CS backend uses 20 MHz channels and legacy rates on
2.4 and 5 GHz. It supports open networks and WPA2-PSK with CCMP.
Data uses a fixed conservative negotiated legacy rate. Automatic rate
control, HT, VHT, DFS channels, AP mode, Bluetooth and power saving are
deferred.

Country settings use pinned wireless-regdb data under its ISC licence.
The power calculation intersects country limits, chipset calibration and
board limits. The initial test profile caps conducted power at 10 dBm and
reserves 6 dB for antenna gain. These are conservative bring-up settings,
not measured antenna characteristics. Board RF validation remains pending.
An unknown profile is rejected before radio initialisation. Channels with
unsupported restrictions are excluded.
An unknown country or a country with no usable channels is rejected.

The driver needs the RK3566 internal-slot SDIO host in kernel pie10 or
later. Matching is limited to the RTL8821CS SDIO function. The initial
power profile is experimental. Keep USB Ethernet available during trials.
The `piewifi-rtw88` package holds `PIErtw88.kext`.

## Layout

- `src/openbsd-src` is a submodule at OpenBSD's release point. The build
  uses its `sys/net80211`, the AES, SHA, MD5, HMAC, CMAC and key wrap code
  of `sys/crypto`, the red-black trees of `sys/sys/tree.h` and
  `sys/kern/subr_tree.c`, and bwfm: `sys/dev/ic/bwfm*` and
  `sys/dev/sdmmc/if_bwfm_sdio*`. The tests also take its `sys/sys/queue.h`.
- `src/rtw88` holds Linux's rtw88 headers, the RTL8821C chip code and its
  tables, unmodified. They are dual-licensed, and the build uses them under
  BSD-3-Clause. `PIErtw88/upstream.json` pins them.
- `src/wireless-regdb` holds wireless-regdb's `db.txt` and its ISC licence,
  unmodified.
- `patches/` changes copies of upstream files. The submodule stays as it
  is.
- `PIE80211Family/private` gives net80211 the OpenBSD kernel's names:
  mbufs, the interface, timeouts and tasks of its own. net80211 never sees
  xnu's networking headers. `PIE80211Family/compat` holds the headers that
  net80211's quoted includes find, and `private` those that its angle
  includes find. WEP, TKIP and BIP are left out. Their entry points refuse
  keys and drop frames.
- `PIE80211Family/glue` holds the code behind those names, and the part
  that runs it on xnu: the family's thread, its lock and the gate.
- `PIE80211Family/include` is the family's interface for the driver kexts
  and the tool: its mbufs and its driver ioctls.
- `PIE80211Family/tests` runs net80211 and the family's code on the build
  machine.
- `piewifi/` holds the pie-wifi tool that stores networks and gives them
  to the kernel.
- `man/` holds pie-wifi's manual page.
- `PIErtw88/` holds the native Realtek adapter and its host tests.
- `regulatory/` generates and checks the country tables.
- `tests/` checks the pinned upstream sources and their licence notices.
- `PIEbwfm/` holds the driver kext's own code: the IOKit driver that
  matches the chip's SDIO function, the glue that runs bwfm on xnu, and the
  firmware list. `PIEbwfm/tests` runs bwfm on the build machine against a
  fake chip.

## Building

Initialise the submodule with `git submodule update --init --depth 1`.
Then, in a build root with pietoolchain and blueberry-pie-os-kernel-dev:

    make KERNEL_DEV=DIR

`KERNEL_DEV` is the root the kernel-dev package is unpacked in, by default
`/var/lib/pietoolchain/cross-root`. `PIE_SDK` names pietoolchain's SDK and
`O` the build directory, `build` by default. The kexts are
`O/PIE80211Family/PIE80211Family.kext`, `O/PIEbwfm/PIEbwfm.kext` and
`O/PIErtw88/PIErtw88.kext`. The family and Broadcom driver are checked
against every PIE kernel. The Realtek backend is checked against RK3566.
The tool is
`O/piewifi/pie-wifi`.

    make check KERNEL_DEV=DIR

builds the tests with the build machine's compiler and runs them. They
need a git checkout with the submodule. The source package leaves out the
tests and OpenBSD's queue.h, which only they read. The Realtek worker test
takes `<pie/sdio.h>` from `KERNEL_DEV`.

## Firmware

PIEbwfm reads the CYW43455's firmware, its CLM blob and the board's NVRAM
file in `/usr/share/firmware/brcm`, which the pie-firmware-brcm package
installs. It loads only files whose name, size and SHA-256 it knows.

When the SDIO host has to reset the card, PIEbwfm loads the firmware
again. The chip keeps its MAC address and gets the stored country again,
and an interface that was up comes back up. A load that fails, or a
country that does not go back, leaves the interface down until it is
brought up again. Scans are passive.

PIErtw88 loads `/usr/share/firmware/rtw88/rtw8821c_fw.bin` from
pie-firmware-rtw88. It checks the pinned firmware length and SHA-256.
The firmware remains unmodified.

## Boot-args

- `pie_wifi=0` keeps the driver from starting.

## Networks

`pie-wifi join SSID` asks for the passphrase and stores the key derived
from it in `/var/db/wifi/networks`, mode 0600. `pie-wifi import FILE` reads
`ssid=` and `passphrase=` lines, stores the networks, then deletes the
file. If the file stays, import says so and exits 3. `pie-wifi load`
gives the stored networks to the kernel.
No command prints or logs a key or a passphrase.

`pie-wifi country XX` stores the regulatory country and gives it to the
kernel. `pie-wifi load` and `pie-wifi watch` give it again before the
networks. With no country stored, none is given, and the chip keeps the
default of its firmware. PIErtw88 starts with the world country `00` and
also enforces any valid programmed hardware country. A setup file holds
networks only.
