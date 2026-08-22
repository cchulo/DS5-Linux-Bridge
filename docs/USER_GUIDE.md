# DS5-Linux-Bridge — User Guide

Operational manual for the firmware: flashing, pairing, the on-device config
page, and OS-specific (Linux / Windows) behavior. For building from source and
debugging, see the [README](../README.md).

- [Flashing the firmware](#flashing-the-firmware)
- [Pairing a controller](#pairing-a-controller)
- [Adding a second controller](#adding-a-second-controller)
- [The configuration page](#the-configuration-page)
  - [Live status](#live-status)
  - [Paired controllers (bond management)](#paired-controllers-bond-management)
- [Steam Deck plugin](#steam-deck-plugin)
- [Operating system & driver behavior](#operating-system--driver-behavior)

---

## Flashing the firmware

1. Hold the **BOOTSEL** button on your Raspberry Pi Pico 2 W.
2. Connect it to your PC via USB. It mounts as a drive named `RP2350`.
3. Drag and drop the compiled `.uf2` firmware onto that volume. The board
   reboots into the firmware automatically.

### Re-flashing without the BOOTSEL button

Once the bridge firmware is running you never need the button again (handy when
the board is mounted inside a case): the **Flash mode** button at the bottom of
the config page reboots the adapter into the same UF2 bootloader over the USB
cable it's already plugged into. The `RP2350` drive appears; drop the new
`.uf2` on it as usual. On builds with the LED strip, the strip holds **solid
orange** for the whole flash-mode session and returns to the normal display
once the new firmware boots (see
[LED strip status codes](#led-strip-status-codes)). If you enter flash mode by
accident, unplug and replug the adapter — it boots the current firmware.

The button is only a convenience for a working firmware; keep BOOTSEL-button
access in mind as the fallback if you ever flash a build too broken to serve
the config page.

---

## Pairing a controller

1. Put the DualSense into Bluetooth pairing mode: hold **Share + PS** until the
   lightbar double-blinks.
2. The adapter detects, pairs, and connects. The onboard LED goes solid on a
   successful connection.
3. Once connected, the adapter presents the controller (gamepad, audio, haptics)
   to the host.

The adapter remembers controllers it has paired with (up to four). **It only
scans for a new controller when none is remembered.** Once at least one
controller is paired, it stops scanning — a remembered controller reconnects on
its own when you turn it on — so a nearby DualSense in pairing mode won't get
grabbed by your adapter.

If you cold-plug the adapter with no controller around, it stays in pairing mode
until the first controller pairs; after that, turning that controller on
reconnects it automatically.

---

## Adding a second controller

Because the adapter connects **one controller at a time**, and it stops scanning
once a controller is remembered, adding a *second* controller is a deliberate
action rather than something that happens automatically.

On the [config page](#the-configuration-page), under **Paired controllers**,
click **Pair new controller**. This:

1. Disconnects the controller you're currently using — but **keeps its bond**, so
   it still reconnects later.
2. Opens a 30-second pairing window. Put the new controller into **Share + PS**
   pairing mode during that window.
3. The new controller connects and is remembered as an additional bond. The
   previous controller is held off during the window so it can't grab the slot
   back before the new one finishes pairing.

> The config page is reachable whether or not a controller is connected, so you
> can also reach it (and pair) when the adapter is idle. Note the page may blip
> briefly as the adapter re-enumerates when a controller joins a new seat.

---

## The configuration page

The adapter is configured from a single web page that talks to it **directly
over USB** (WebHID) — no network, no app, nothing to install.

1. Open `web/index.html` from the firmware repository in **Chrome, Edge, or
   another Chromium-based browser** (Firefox and Safari do not support WebHID).
2. Press **Connect to adapter** and pick the **DualSense Wireless Controller**
   entry in the browser's chooser (that is the adapter).
3. Adjust settings — controller mode, polling rate, audio buffer length,
   inactivity timeout, lights, paired controllers — and click **Save**.
   Settings are written to the adapter's flash.

The page works whether or not a controller is connected, and it remembers the
adapter: on later visits it reconnects by itself. If the adapter is unplugged
and re-plugged while the page is open, press **Connect** again.

**WiFi is only used for Wake-on-LAN.** Under **Network**, enter your WiFi name
and password and press **Save WiFi & restart**; the adapter joins that network
after restarting so the PS button can send magic packets. With no network
saved the adapter simply runs with WiFi off. **Forget WiFi** clears it again.

On Linux the adapter must be accessible to your user. If Steam is installed
that is already the case (its udev rules cover Sony gamepads); otherwise add a
udev rule granting access to USB vendor `054c`.

### Live status

The top of the page shows a live status card — whether a controller is
connected, its model (DualSense / DualSense Edge), and a battery gauge (percent
plus a charging indicator) — refreshed every few seconds. It's served from a
read-only `GET /api/status` endpoint, so any client (including the
[Steam Deck plugin](#steam-deck-plugin)) can poll it.

### Paired controllers (bond management)

The page lists the controllers the adapter remembers (the Bluetooth link keys it
stores, up to four). For each you can:

- **Rename** it with a short nickname (≤15 chars), stored in the adapter's flash.
- **Forget** it, or **Forget all** — clears the stored link key(s) so the slot is
  freed.
- **Pair new controller** — see [Adding a second controller](#adding-a-second-controller).

Forgetting a controller disconnects it if it's the one currently connected, and
blacklists its Bluetooth address so it can't silently auto-reconnect afterward.
To bring a forgotten controller back, re-pair it explicitly (**Share + PS**),
which clears the blacklist entry on a successful pair. The blacklist persists
across power cycles.

---

## LED strip status codes

Builds with the WS2812B strip enabled use it as the adapter's status display
(handy when the board is mounted with its onboard LED hidden). Some of the
colors are yours to configure; the important ones are fixed so they always
mean the same thing.

**Fixed codes — cannot be changed or overridden:**

| Strip shows | Meaning |
| --- | --- |
| **Solid orange** (whole strip) | **Flash mode.** The adapter is in the UF2 bootloader waiting for firmware — the `RP2350` drive is mounted on your PC. Shown from the moment you press **Flash mode** until the freshly flashed firmware starts (the strip then snaps to the normal display, confirming the flash took). |
| **Solid red** (whole strip) | **Firmware error.** The firmware crashed and was restarted by the watchdog, or is stuck in a reboot loop (e.g. the radio failed to start). Red that clears after a few seconds means it recovered on its own; red that stays means it's boot-looping — unplug and replug the adapter. |

The fixed codes deliberately win over everything else, including the LED
debug panel's overrides — if the strip goes solid red or orange, that is the
adapter itself talking.

**Configurable displays** (Lights section of the config page). Every state
below has a configurable color **and** animation (solid / blink / pulse /
off) under **Animations & colors**; the defaults are:

| Strip shows (default) | Meaning |
| --- | --- |
| **Off** (whole strip) | Powered on, no controller connected, not pairing. The **Waiting** state is off by default; give it a color + animation (e.g. pulse blue) for a "powered and waiting" display. |
| **Solid slot color** (slot's LEDs, default blue) | That slot's controller is connected and healthy. Color: **Slot colors** (matches the pad's lightbar). |
| **Off/black** (empty seat's LEDs) | No controller in that seat while others are connected. Give **Empty slot** a color to mark vacant seats. |
| **Solid yellow** (slot's LEDs) | That controller's battery is at or below **40%** (discharging). |
| **Blinking red** (slot's LEDs, fast) | At or below **20%** (discharging) — charge it now. The one state that still blinks out of the box. |
| **Solid white** (chosen pixels) | Pairing mode — the adapter is searching for a controller. Pixels and color: the **Pairing** row of the layout grid. |

---

## Steam Deck plugin

A [Decky Loader plugin](https://github.com/kungaa/DS5-Linux-Decky) surfaces the
adapter's controller status and settings directly in the Steam Deck Quick Access
Menu. It's a client of the same on-device HTTP API the web page uses, so it works
without any extra firmware. See that repository for installation and usage.

---

## Operating system & driver behavior

### Linux / SteamOS (Bazzite, CachyOS)

- **Native driver integration.** Compatible with the kernel `hid-playstation`
  driver. When the Linux driver is active, the firmware yields LED and button
  control to the OS driver to avoid conflicts.
- **Jack detection.** The DS5's real `HP_DETECT` / `MIC_DETECT` jack bits are
  passed through to the host so `hid-playstation` (kernel ≥6.18) emits
  `SW_HEADPHONE_INSERT` / `SW_MICROPHONE_INSERT`. The ≥6.17 USB-audio mixer quirk
  wires these to the ALSA "Headphone Jack" / "Headset Mic Jack" controls that
  `alsa-ucm-conf` uses to switch between the mono Internal Speaker and the stereo
  Headphones profiles. The firmware also forces the HP_DETECT bit high in the
  report it presents to the host to bias toward the stereo Headphones profile for
  headphone output.

> **Known issue — one-earphone / mono audio on some setups.** Audio routing is
> ultimately decided host-side by PipeWire/ALSA via `alsa-ucm-conf`, and on some
> distros/kernels it lands on a mono profile (audio in one earphone only). This is
> kernel- and UCM-version dependent rather than a firmware fault — stereo
> generally needs a recent kernel (≥6.18) with the jack-detect mixer quirk. Note
> that bleeding-edge / rolling distros (e.g. CachyOS, Arch) can also *regress*
> here: a newer kernel or updated `alsa-ucm-conf` can change the routing behavior
> and break a setup that previously worked. Investigation is ongoing.

### Windows 11

- **Audio & mute sync.** Runs driverless. The physical Mute button operates at the
  hardware level, muting the mic stream in firmware and lighting the controller's
  orange LED. Muting/unmuting via the Windows Sound panel also syncs the
  controller LED. (Because it is driverless, toggling the physical button won't
  move the Windows checkmark; the mic stream is muted directly on the adapter.)
- **HD haptics.** Work out of the box in titles that support them on a wired
  DualSense (e.g. Death Stranding Director's Cut).
