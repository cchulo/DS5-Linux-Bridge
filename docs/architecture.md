# Architecture Survey — Multi-Controller (4×) Refactor Baseline

Snapshot of the single-controller firmware as of `4438f0c`, mapped for the
Phase 1 multi-slot work. Every claim carries a `file:line` reference against
that commit. Read this before touching `bt.cpp`, `usb_descriptors.cpp`, or
`audio.cpp`.

**Headline:** the firmware is single-connection end-to-end — one BT ACL, one
HID channel pair, one send FIFO, one 63-byte report global, one audio
pipeline, one wake FSM. BTstack is compile-time capped at 1 connection. The
only multi-ready pieces are bond storage (4 link keys) and the stateless HTTP
API.

---

## 1. Module map

| Module | Files | Role |
|---|---|---|
| Bluetooth | `src/bt.cpp`, `src/btstack_config.h` | BTstack classic HID host: inquiry/paging, L2CAP 0x11/0x13, bonds, send FIFO |
| DSE support | `src/dse.cpp` | DualSense Edge detection, profile-report unlock/prefetch |
| USB composite | `src/usb_descriptors.cpp`, `src/usb.cpp`, `src/tusb_config.h` | TinyUSB device: audio + gamepad HID + NCM + wake keyboard; FULL/MINIMAL variant swap |
| HID bridge | `src/main.cpp` | core0 superloop; BT 0x31 → USB report 0x01; feature-report proxy callbacks |
| Output state | `src/state_mgr.cpp` | Authoritative 63-byte SetStateData (rumble/trigger/lightbar/mute) |
| Audio | `src/audio.cpp` | UAC1 4ch in / 2ch out ↔ BT 0x36 frames; Opus on core1 |
| Config | `src/config.cpp`, `src/config.h` | Flash store (512 B, append-only, size-migrated), bond nicknames |
| Web config | `src/usb_net.cpp`, `src/web_page.h`, `src/lwipopts.h` | lwIP httpd over CDC-NCM; REST API (Decky plugin contract) |
| Wake | `src/wake.cpp` | S3 remote-wakeup + S5 F15-keystroke FSM |
| Battery LED | `src/battery_led.cpp` | Onboard LED low-battery blink |
| RAM relocation | `cmake/relocate_to_ram.cmake`, `cmake/opus_unrelocate_analysis.cmake`, `src/ram_mem.c` | `.time_critical` hot-path relocation; libopus in RAM |

### Core split
- **core0** runs everything except audio DSP: the superloop at `main.cpp:381-408`
  pumps `cyw43_arch_poll()` (POLL model — `pico_cyw43_arch_poll`,
  `CMakeLists.txt:292`), BTstack, `tud_task()`, wake/variant/net tasks,
  `audio_loop()`, `interrupt_loop()`. Hardware watchdog 1 s (`main.cpp:379`).
- **core1** is Opus only: launched `audio.cpp:224` with a dedicated 32 KB stack
  (`audio.cpp:37`), `__wfe()`-gated loop (`audio.cpp:274-324`). Registered as a
  flash-safe victim (`audio.cpp:278`) so `config_save()` can park it.
- Inter-core: `audio_fifo` (6×4 KB), `mic_fifo`, `mic_decode_fifo`
  (`audio.cpp:38-53,220-223`); `opus_buf[200]` under `opus_cs`.
- No inter-core FIFO for HID input — `interrupt_in_data` is core0-only, guarded
  by `report_cs` because the 1000 Hz path decouples receive from send.

---

## 2. Bluetooth layer

### Setup
- `bt_init()` `bt.cpp:408-434`: L2CAP services for PSM 0x11 (HID control) and
  0x13 (HID interrupt), MTU 672, LEVEL_2 (`bt.cpp:29-30,402-403`); SSP
  just-works-ish (`DISPLAY_YES_NO`, no MITM, `bt.cpp:413-417`); fast reconnect
  via 11.25 ms interlaced page scan (`bt.cpp:424-425`).
- No sniff-mode configuration exists; power management is an inactivity
  disconnect watchdog (`bt.cpp:742-758`) plus DS power-off feature report 0x08
  sub 0x02 (`bt.cpp:1072-1088`).

### Connection lifecycle (implicit FSM, no enum)
State lives across `acl_handle`, `device_found`, `new_pair`, `pairing_window`,
`connect_attempt_started`, `hid_control_cid`, `hid_interrupt_cid`, `check_dse`;
driver is the HCI handler `bt.cpp:436-725`.

- **Pair (outgoing):** inquiry (only when zero bonds — `bt.cpp:445-457,131-140`)
  → gamepad CoD match `(cod & 0x000F00)==0x000500` (`bt.cpp:479`) →
  `hci_create_connection` → auth → encryption → *we* create both L2CAP channels
  (`bt.cpp:626-634`).
- **Reconnect (incoming):** controller pages us → `CONNECTION_REQUEST` accept if
  gamepad CoD and not blacklisted (`bt.cpp:640-666`) → stored link key auth →
  *controller* creates the channels, we accept (`bt.cpp:879-885`).
- 10 s connection watchdog (`bt.cpp:36,186-202`).
- Forget-bond blacklist persisted in BTstack TLV tag `'BLCK'`
  (`bt.cpp:69-76,221-305`) so a forgotten pad can't silently re-bond.

### Bonds
- Bonds are BTstack link keys, capacity 4 (`btstack_config.h:32-33`), in the
  BTstack flash TLV bank near top of flash (`config.cpp:9-11`).
- Nicknames are a separate app-level store `bond_names[4]` in our config sector,
  keyed by BD_ADDR (`config.h:11-12,56`; `config.cpp:310-344`).
- Managed via `POST /api/bonds` (`usb_net.cpp:505-561`): `pair`, `forget`,
  `forgetall`, `rename`.

### Data flow
- **Input:** L2CAP interrupt data → `bt_data_callback` (`bt.cpp:731-734`) →
  `on_bt_data` (`main.cpp:108-177`): 0x31 check, mic-frame split
  (`data[2]>>1&1` → `mic_add_queue`), then `data+3` → `interrupt_in_data[63]`.
- **Output:** USB SET_REPORT/INT-OUT → `state_update()` mutates `state[63]`
  (`state_mgr.cpp:71-208`) → `state_push_to_bt()` builds 0x31 →
  `bt_write()` (`bt.cpp:953-983`) prepends 0xA2, salts CRC32, enqueues
  `send_fifo` (depth 10) → self-chaining `CAN_SEND_NOW` drain
  (`bt.cpp:904-938`) with one-slot retry.
- **CRC32 seeds** (`utils.h:112-140`): output reports (0xA2) seed
  `0xEADA2D49`; feature reports (0x53) seed `0x2060efc3`. Shared helpers — the
  emulator harness (§1.5) must reuse these, not duplicate.

### btstack_config.h — the hard caps to raise for N slots

| Macro | Today | For N=4 |
|---|---|---|
| `MAX_NR_HCI_CONNECTIONS` (`:17`) | **1** | 4 |
| `MAX_NR_L2CAP_CHANNELS` (`:18`) | **2** | 8 |
| `MAX_NR_L2CAP_SERVICES` (`:19`) | 3 | 3 (services are shared) |
| `MAX_NR_HCI_ACL_PACKETS` (`:15`) | 4 | scale with streams (comment: 1–2 overflows a DS 0x31) |
| `NVM_NUM_LINK_KEYS` / `NVM_NUM_DEVICE_DB_ENTRIES` (`:32-33`) | 4 | already sufficient |

---

## 3. USB composite device

### FULL variant interface map (`usb_descriptors.cpp:37-49`)

| ITF | Function | Endpoints |
|---|---|---|
| 0 | UAC1 audio control | — |
| 1 | Audio OUT (speaker+haptics, 4ch/16/48k) | iso OUT 0x01 (392 B) |
| 2 | Audio IN (mic, 2ch/16/48k) | iso IN 0x82 (196 B) |
| 3 | **DualSense HID gamepad** | INT IN 0x84, INT OUT 0x03 |
| 4–5 | CDC-NCM (control+data, IAD) | INT IN 0x85, bulk IN 0x86, bulk OUT 0x05 |
| 6 | Boot keyboard (wake) | INT IN 0x87 |

- VID 0x054C; PID picked at runtime 0x0CE6 (DS5) / 0x0DF2 (Edge) by
  `ds_mode()` (`usb_descriptors.cpp:221-224,30-35`).
- Report descriptors: DS5 273 B / DSE 389 B (`usb_descriptors.cpp:764-1112`),
  deliberately near-identical to real-hardware dumps (report IDs 11/12 and
  0xF6–0xF9 removed to match). `wDescriptorLength` patched at fetch time
  (`:752-756`).
- **The 0xF6/0xF7 vendor-HID config channel no longer exists** — removed in
  favor of the NCM web page (`usb_descriptors.cpp:903-904,1107-1108`). The only
  vendor class left routes the MS OS 2.0 descriptor (`:1443-1464`).
- Polling rate (250/500/1000 Hz) is applied by patching `bInterval` at
  descriptor-fetch time (`usb_descriptors.cpp:737-751`).

### FULL/MINIMAL variant swap ("no ghost devices")
Not per-interface hiding — two complete config descriptors swapped by
re-enumeration:
- MINIMAL (`usb_descriptors.cpp:572-592`) = inert vendor pad (3 zero-endpoint
  interfaces under one IAD) + dummy HID (holds instance 0) + **same NCM
  interfaces/endpoints as FULL** + keyboard. Keeps the host's network adapter
  identity stable and remote wakeup armed while showing no audio/joystick
  ghosts.
- Swap FSM `usb_variant_task()` (`:687-724`): disconnect → 500 ms → flip →
  connect → 1500 ms. Triggered by BT connect (`bt.cpp:774,788` → FULL) and
  disconnect (`bt.cpp:676` → MINIMAL). Suspend-gated so it never yanks the bus
  during S3/S5 (`:669,688-701`).
- **Instance-stability invariant:** `usb_kbd_hid_instance()` returns hardcoded 1
  (`:628`) — gamepad/dummy is always HID instance 0, keyboard always 1, in both
  variants. Any multi-gamepad layout must recompute this.

### Feature-report proxy
- Cache `feature_data` map populated from 0xA3 control packets
  (`bt.cpp:89,794-800`); prefetched on connect: 0x09, 0x20, 0x22, 0x05, 0x70
  (`bt.cpp:1060-1070`).
- GET: `tud_hid_get_report_cb` (`main.cpp:182-220`) → `get_feature_data`
  (`bt.cpp:1008-1034`) — cached copy + async refresh (0x43 GET_FEATURE) for
  volatile IDs.
- SET: `tud_hid_set_report_cb` (`main.cpp:241-295`) → `set_feature_data`
  (`bt.cpp:1036-1058`), 0x53-framed with feature CRC.

### tusb_config.h constraints
- `CFG_TUD_HID` = 2 (gamepad + keyboard; `:95-99`) → needs ≥5 for 4 slots.
- EP addresses in use: 0x01, 0x82, 0x84, 0x03, 0x85, 0x86, 0x05, 0x87.
  **Endpoint budget is the primary hardware wall:** RP2350 USB has 16 EP
  numbers per direction; 4 gamepads × (IN+OUT) adds 8 on top of
  audio(2)+NCM(3)+kbd(1). Feasible but tight — needs a deliberate EP map, and
  the MINIMAL variant must mirror it.

---

## 4. Audio path (why the tier policy exists)

- Speaker: UAC 4ch → gain → 512-frame staging → core1
  `opus_encode_float` 480 frames/10 ms, **160 kbps CBR, complexity 0, VBR off**
  (`audio.cpp:280-292`). Haptics: 16:1 boxcar 48 kHz→3 kHz int8
  (`audio.cpp:142-159`). Resampler: linear 512→480 (`audio.cpp:261-272`).
- Assembled into 398-byte BT 0x36 frames at ~93 Hz (`audio.cpp:169-214`),
  `bt_write(..., kick=false)` — audio defers the can-send kick to `bt_pump()`
  because an inline kick costs ~535 µs under POLL (`bt.cpp:973-982`).
- Mic: decode gated on `mic_active` (`audio.cpp:328-332`) because idle decode
  load on core1 degrades speaker quality.
- **Implication:** one controller's duplex audio (160 kbps CBR + 93 Hz × 398 B
  + 1000 Hz input reports) already approaches the practical BR/EDR airtime
  budget. Concurrent audio for 2+ controllers doesn't fit the radio, the heap,
  or the single-core1 codec model — hence the Phase 1 tier table (full audio
  at 1, HD-haptics-or-primary-audio at 2, classic rumble at 3–4). The tier
  manager throttles *senders*; the BT layer needs per-slot send FIFOs with an
  airtime-aware arbiter.

## 5. Config, web API, RAM budget

### Flash config store (`config.cpp`)
- 512 B store at `PICO_FLASH_SIZE_BYTES - 4*FLASH_SECTOR_SIZE`
  (`config.cpp:34-41`) — one sector below BTstack's TLV bank, three below the
  bootrom-erased last sector. Header magic/version/crc/size + append-only body;
  size-based migration (`config.cpp:160-219`); offsets pinned by static asserts
  (`config.cpp:65-75`).
- Save: `flash_safe_execute` with 3 retries, `__sev()` to wake core1 first,
  read-back verify (`config.cpp:235-276`).
- **Adding a setting:** append to `Config_body` end (`config.h:57`), add
  offset assert, clamp in `config_valid()`, expose in `json_config()` +
  `apply_post()` + `web_page.h`. Slot-policy and tier settings follow this
  path.

### Web API (Decky plugin contract — keep additive)
- `GET /api/config`, `/api/bonds`, `/api/status` (page polls status every 4 s,
  `web_page.h:302`); `POST /api/config`, `/api/bonds`
  (`usb_net.cpp:352-385,505-563`). One POST at a time; `Connection: close`.
- `docs/DECKY_PLUGIN_HANDOVER.md` declares `/api/status` additive-only and
  `web_page.h` the reference client. Multi-slot status must extend, not
  reshape, these payloads (e.g. add a `slots[]` array alongside the existing
  top-level fields).

### RAM relocation & budget
- Hot functions renamed `.text.<fn>` → `.time_critical.*` at PRE_LINK
  (`cmake/relocate_to_ram.cmake`, `CMakeLists.txt:311-378`); libopus
  blanket-relocated (~220 KB) minus the never-executed tonality analyzer
  (~21 KB kept in flash) (`CMakeLists.txt:153-209`).
- `src/ram_mem.c`: RAM-resident memcpy/memset/memmove (RP2350 bootrom dropped
  ROM mem-ops).
- Known big RAM consumers: relocated opus ~200 KB, core1 stack 32 KB,
  audio FIFOs ~28 KB. No .map checked in; guidance in comments is qualitative
  — "solve within the remaining heap, never evict Opus"
  (`CMakeLists.txt:165-181`). 4× input-report state is cheap (bytes); 4×
  *audio* state is not — another reason audio stays single-slot.

---

## 6. Single-controller globals — the refactor hit list

These become fields of `slot_t state[MULTI_SLOT_COUNT]` (or per-slot arrays):

### BT connection (`bt.cpp`)
| Global | Line | Type |
|---|---|---|
| `acl_handle` | 78 | `hci_con_handle_t` |
| `hid_control_cid` / `hid_interrupt_cid` | 79-80 | `uint16_t` |
| `current_device_addr` | 47 | `bd_addr_t` |
| `device_found` / `new_pair` | 48-49 | `bool` |
| `pairing_window` | 60 | `bool` (stays global) |
| `connect_attempt_started` | 86 | `absolute_time_t` |
| `check_dse` | 87 | `bool` |
| `bt_rssi` | 88 | `int8_t` |
| `feature_data` | 89 | `unordered_map<uint8_t,vector<uint8_t>>` — needs slot keying |
| `send_fifo` | 90 | `queue_t` depth 10 — per-slot + shared arbiter |
| `send_chain_active` / `retry_packet` / `retry_pending` | 994 / 911-912 | per-slot |
| `inactive_time` | 99 | `absolute_time_t` |
| `is_dse` | (set 764/781) | `bool` — per-slot; **also flips the global USB PID/report descriptor** (design decision needed) |

### HID bridge (`main.cpp`)
`interrupt_in_data[63]` (:35), `report_cs`/`report_dirty` (:43-44),
`reportSeqCounter`/`packetCounter` (:30-31), `spk_active`/`mic_active`
(:32-33), `on_bt_data` static locals `prev_mute_pressed`/`last_jack_state`
(:118,131).

### Output state (`state_mgr.cpp`)
`state[63]` (:35), `g_firmware_mic_muted` (:31), `g_host_hid_manages_mute`
(:32), `g_last_uac_mute` (:33).

### DSE (`dse.cpp`)
`unlock_phase` (:19), `unlock_started_ms` (:20), `profiles_ready` (:26),
`profile_written_ms` (:29), `post_save_round` (:30), `prefetch_*` (:36-38).

### Audio (`audio.cpp`) — stays single-slot by policy
`reportSeqCounter`/`packetCounter`/`plug_headset` (:34-36), all FIFOs and the
core1 codec pair — bind to the *audio-designated slot*, not replicated.

### USB (`usb_descriptors.cpp`, `wake.cpp`, `usb.cpp`)
`ITF_NUM_*` enum (:37-49), EP 0x84/0x03 assignment (:486,493),
`active_variant`/`desired_variant`/swap FSM (:619,653,662-663),
`usb_kbd_hid_instance()==1` (:628); wake FSM + `prev_b7/b8/b9` snapshot
(`wake.cpp:59-69`), `power_off_armed` (:152-153); `mute[2]`/`volume[2]`
(`usb.cpp:9-10`).

### Status consumers
`battery_led.cpp` reads `interrupt_in_data[52]` directly (:13,66-69);
`bt_get_status()` (`bt.cpp:374-379`) feeds `/api/status`; the one CYW43 LED is
contended by battery_led/config/bt/main.

---

## 7. Wake code paths (Phase 2 integration points)

- FSM `wake.cpp:50-57`; driven by `wake_task()` from `main.cpp:388`.
- **S3:** `tud_suspend_cb` arms; any change in DS button bytes 7/8/9
  (`wake_on_bt_input`, `wake.cpp:211-249` — shoulder buttons wake the radio
  from sniff more reliably than PS alone) → `request_host_wake()`
  (`wake.cpp:81-111`): `tud_remote_wakeup()` + Linux DCD fallback, then F15
  keydown/keyup on the boot keyboard with 2 attempts (`wake.cpp:324-414`).
- **S5:** same F15 keystroke reaches BIOS USB-keyboard wake;
  `wake_on_bt_connect()` (`wake.cpp:261-270`) covers power-on-controller.
- Deferred controller power-off 10 s after host sleep (`wake.cpp:152-154,
  302-311`), cancelled on resume/mount.
- **Phase 2 GPIO pulse hooks:** escalate inside `request_host_wake()`
  (beside the DCD fallback, `wake.cpp:88-92`) or on the `WAKE_REQUESTED`
  timeout branch (`wake.cpp:346-351`) when no SOF/resume follows.
- Dead code: `wake_on_bt_disconnect()` (`wake.cpp:272-278`) is never called.

---

## 8. Corrections to PLAN-rev2 assumptions

1. **No BOOTSEL runtime button handling exists.** BOOTSEL is flash-mode only
   (docs). "Restart search" / "clear bonds" live exclusively in the web UI
   (`POST /api/bonds`). The plan's BOOTSEL pairing-window/clear-bonds UX is a
   *new feature to build*, not behavior to generalize.
2. **No 0xF6/0xF7 vendor config HID interface.** Removed upstream; config went
   to the NCM web page. Plan §1.3's "keep the config/companion vendor HID
   interface" is moot — the stable-identity requirement transfers to the NCM
   interface pair (already handled by the variant scheme).
3. **No sniff-mode tuning in firmware** — controller-driven only. Real-DS5
   sniff timing remains a hardware-validation item (§1.5 known gaps).
4. **TinyUSB is not the SDK 2.2.0 submodule default** — CI pins commit
   `08f9855e3dc51291a8c30e2eeccab31f8f07d0a9` (post-0.20.0; the 0.20.0 tag
   lacks the usbd wLength fixes the MS OS 2.0 fetch needs, and the SDK ships
   0.18.0 which fails to compile this tree). `docker/Dockerfile` mirrors the
   pin; keep the two in lockstep.
5. Debug story confirmed: UART0 GP0 @ 115200 8N1, no USB-CDC
   (`CMakeLists.txt:61-63,269-272`).
