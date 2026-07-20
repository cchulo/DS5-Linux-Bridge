//
// Created by awalol on 2026/3/4.
//

#include "audio.h"
#include "bsp/board_api.h"
#include "bt.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "state_mgr.h"
#include "usb.h"
#include "utils.h"
#include "wake.h"
#include "weblog.h"
#include <cstdio>
#include <malloc.h>

#include "config.h"
#include "dse.h"
#include "usb_net.h"
#include "wifi_net.h"
#include "tier.h"

#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

#ifdef ENABLE_LED_STRIP
#include "ledstrip.h"
#endif

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"
#include "pico/time.h"

// Per-slot sequence counter for outgoing BT 0x31 output reports (audio.cpp
// keeps its own for the audio-frame path).
int reportSeqCounter[BT_MAX_SLOTS] = {};
uint8_t packetCounter = 0;
bool spk_active = false;
bool mic_active = false;

// Neutral/idle DualSense input report: centered sticks, no buttons. Every
// slot's buffer starts from this so the host sees a quiet pad (not garbage)
// before the first BT report lands.
static const uint8_t idle_input_report[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7, 0x08, 0x00, 0x00, 0x00,
    0x52, 0x43, 0x30, 0x41, 0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03,
    0x03, 0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00, 0xfc, 0x80,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b};

// Latest gamepad input report per slot (bt.cpp and battery_led.cpp read
// these too). Only slot BT_USB_SLOT is bridged to USB until the per-slot
// interface fan-out lands; the other rows are kept fresh for status and the
// upcoming composite device.
uint8_t interrupt_in_data[BT_MAX_SLOTS][63];

critical_section_t report_cs;
volatile bool report_dirty[BT_MAX_SLOTS] = {};

void interrupt_loop() {
#ifdef ENABLE_WAKE_HID
  // Only the FULL variant exposes real gamepads (HID instances
  // 0..BT_MAX_SLOTS-1). In MINIMAL instance 0 is an inert dummy HID, so don't
  // emit gamepad reports there. (The keyboard instance is computed per
  // variant, so a gamepad report can never reach it regardless -- see
  // usb_descriptors.cpp. This guard just avoids pushing reports at the dummy
  // / before any controller is connected.)
  if (!usb_descriptor_variant_is_full())
    return;
#endif

  const bool realtime = get_config().polling_rate_mode == 2;
  for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
    const uint8_t inst = usb_slot_hid_instance(slot);
    if (!tud_hid_n_ready(inst))
      continue;

    if (!realtime) {
      // Fixed-cadence mode: re-send the latest buffer every iteration; empty
      // slots keep reporting their neutral idle state.
      if (!tud_hid_n_report(inst, 0x01, interrupt_in_data[slot], 63)) {
        printf("[USBHID] tud_hid_report error (slot %u)\n", slot);
      }
      continue;
    }

    // Real-time (1000 Hz) mode: send only when fresh BT data arrived.
    bool should_send = false;
    // Local buffer to hold the report data while we prepare it to send.
    uint8_t safe_report[63];

    critical_section_enter_blocking(&report_cs);
    if (report_dirty[slot]) {
      memcpy(safe_report, interrupt_in_data[slot], 63);
      report_dirty[slot] = false;
      should_send = true;
    }
    critical_section_exit(&report_cs);

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
      if (!tud_hid_n_report(inst, 0x01, safe_report, 63)) {
        printf("[USBHID] tud_hid_report error (slot %u)\n", slot);

        // If the report failed to queue, restore the dirty flag
        // so we try again on the next loop iteration.
        critical_section_enter_blocking(&report_cs);
        report_dirty[slot] = true;
        critical_section_exit(&report_cs);
      }
    }
  }
}

void bridge_reset_slot_input(uint8_t slot) {
  if (slot >= BT_MAX_SLOTS) return;
  critical_section_enter_blocking(&report_cs);
  memcpy(interrupt_in_data[slot], idle_input_report,
         sizeof(idle_input_report));
  report_dirty[slot] = true;
  critical_section_exit(&report_cs);
}

void bridge_swap_slot_input(uint8_t a, uint8_t b) {
  if (a >= BT_MAX_SLOTS || b >= BT_MAX_SLOTS || a == b) return;
  uint8_t tmp[63];
  critical_section_enter_blocking(&report_cs);
  memcpy(tmp, interrupt_in_data[a], 63);
  memcpy(interrupt_in_data[a], interrupt_in_data[b], 63);
  memcpy(interrupt_in_data[b], tmp, 63);
  report_dirty[a] = true;
  report_dirty[b] = true;
  critical_section_exit(&report_cs);
}

// Push one slot's cached output state to its controller as a BT 0x31 report.
static void state_push_slot_to_bt(uint8_t slot) {
  uint8_t outputData[78]{};
  outputData[0] = 0x31;
  outputData[1] = reportSeqCounter[slot] << 4;
  if (++reportSeqCounter[slot] == 256) {
    reportSeqCounter[slot] = 0;
  }
  outputData[2] = 0x10;
  state_get(slot, outputData + 3, sizeof(SetStateData));
  bt_write(slot, outputData, sizeof(outputData));
}

void state_push_to_bt() {
  if (spk_active) {
    return;
  }
  state_push_slot_to_bt(tier_audio_slot());
}

void on_bt_data(uint8_t slot, CHANNEL_TYPE channel, uint8_t *data,
                uint16_t len) {
  // printf("[Main] BT data callback: slot=%u channel=%u len=%u\n", slot,
  //        channel, len);
  if (channel == INTERRUPT && len > 2 && data[1] == 0x31) {
    if (data[2] >> 1 & 1) {
      // Controller mic audio rides in the input report. Only the designated
      // audio slot's path is live; other slots shouldn't be streaming (the
      // tier policy keeps their mic off), so drop any stray frames.
      if (slot == tier_audio_slot() && tier_audio_allowed()) {
        mic_add_queue(data + 4);
      }
      return;
    }

    // Mute button detection (data[12] corresponds to byte 9 of input data).
    // Mute/jack are audio-path concerns -> designated audio slot only.
    if (slot == tier_audio_slot() && !g_host_hid_manages_mute) {
      static bool prev_mute_pressed = false;
      bool mute_pressed = (data[12] & 0x04) != 0;
      if (mute_pressed && !prev_mute_pressed) {
        g_firmware_mic_muted = !g_firmware_mic_muted;
        state_set_local_mute(g_firmware_mic_muted);
        state_push_to_bt();
      }
      prev_mute_pressed = mute_pressed;
    }

    // Controller shortcut: hold PS + Triangle for ~1 s to power that pad off
    // (same as the controller's own long PS hold: bond kept, it reconnects on
    // the next PS press; the host side is untouched — the pad's slot just
    // goes neutral). Raw BT report offsets: input byte 7 bit 7 = Triangle
    // (data[10]), input byte 9 bit 0 = PS (data[12]).
    {
      static uint64_t combo_since_us[BT_MAX_SLOTS] = {};
      static bool combo_fired[BT_MAX_SLOTS] = {};
      const bool combo = (data[12] & 0x01) && (data[10] & 0x80);
      if (combo) {
        // Intercept: these chords belong to the pico, and SteamOS has its
        // own bindings for them. Mask the buttons out of the report before
        // it is copied to the USB input buffer below, so the host sees
        // neither PS nor Triangle while the combo is held.
        data[10] &= ~0x80;
        data[12] &= ~0x01;
      }
      if (!combo) {
        combo_since_us[slot] = 0;
        combo_fired[slot] = false;
      } else if (!combo_fired[slot]) {
        const uint64_t now = time_us_64();
        if (combo_since_us[slot] == 0) {
          combo_since_us[slot] = now;
        } else if (now - combo_since_us[slot] >= 1'000'000) {
          combo_fired[slot] = true;
          printf("[Main] PS+Triangle held on slot %u -> controller power off\n",
                 slot);
          bt_slot_power_off(slot);
        }
      }
    }

    // Controller shortcut: PS + Create on the SLOT 1 pad only. Hold ~3 s to
    // open a pairing window (same as the web UI's "Pair new controller");
    // if a window is already open, hold ~1 s to cancel it. The fired latch
    // requires releasing the combo between actions, so opening a window
    // while still holding can't immediately cancel itself. Raw BT report
    // offsets: input byte 8 bit 4 = Create (data[11]), byte 9 bit 0 = PS
    // (data[12]).
    if (slot == BT_USB_SLOT) {
      static uint64_t pair_combo_since_us = 0;
      static bool pair_combo_fired = false;
      const bool combo = (data[12] & 0x01) && (data[11] & 0x10);
      if (combo) {
        // Intercept, same as PS+Triangle above: the host sees neither PS
        // nor Create while the combo is held.
        data[11] &= ~0x10;
        data[12] &= ~0x01;
      }
      if (!combo) {
        pair_combo_since_us = 0;
        pair_combo_fired = false;
      } else if (!pair_combo_fired) {
        const uint64_t now = time_us_64();
        if (pair_combo_since_us == 0) {
          pair_combo_since_us = now;
        } else {
          const bool window = bt_pairing_window_open();
          const uint64_t hold_us = window ? 1'000'000 : 3'000'000;
          if (now - pair_combo_since_us >= hold_us) {
            pair_combo_fired = true;
            if (window) {
              printf("[Main] PS+Create held on slot %u -> cancel pairing\n", slot);
              bt_cancel_pairing();
            } else {
              printf("[Main] PS+Create held on slot %u -> open pairing\n", slot);
              bt_start_pairing();
            }
          }
        }
      }
    }

    // Track actual DS5 jack state separately — interrupt_in_data[..][53]
    // has its HP_DETECT bit forced high for host UCM routing and cannot
    // be used as the previous-state comparison here.
    if (slot == tier_audio_slot()) {
      static uint8_t last_jack_state =
          0xFF; // sentinel: force set_headset on first report
      const uint8_t cur_jack_state = data[56] & 1;
      if (cur_jack_state != last_jack_state) {
        set_headset(cur_jack_state);
        last_jack_state = cur_jack_state;
      }
    }

    // Wake-on-PS must observe every BT input report regardless of polling
    // mode: the wake feature has its own state to maintain (button-byte
    // diff for edge detection) and short-circuiting it on non-2 polling
    // modes silently breaks wake while the host is suspended. Any slot's
    // controller may wake the host (idle pads report identical neutral
    // button bytes, so interleaved slots don't fake edges).
    wake_on_bt_input(data + 3, len - 3);

    // interrupt_in_data[..][53] = dualsense_input_report.status[1]:
    //   bit 0 = HP_DETECT  (headphones plugged into DS5 3.5mm jack)
    //   bit 1 = MIC_DETECT (headset mic plugged into DS5 3.5mm jack)
    // hid-playstation (≥6.18) reads these and emits SW_HEADPHONE_INSERT /
    // SW_MICROPHONE_INSERT input events. The USB audio mixer quirk (≥6.17)
    // wires those to "Headphone Jack" / "Headset Mic Jack" ALSA controls,
    // which alsa-ucm-conf uses to switch between mono Internal Speaker and
    // stereo Headphones profiles. We pass the DS5's real values through
    // unchanged — the DS5 hardware jack sensor is authoritative.
    if (get_config().polling_rate_mode != 2) {
      memcpy(interrupt_in_data[slot], data + 3, 63);
#if ENABLE_BATT_LED
      if (slot == BT_USB_SLOT) {
        battery_led_note_report();
      }
#endif
      return;
    }

    // We add the critical section here to avoid any race conditions when
    // writing to the interrupt_in_data buffer, which is shared between the main
    // loop and this callback. The critical section ensures that only one thread
    // can access the buffer at a time, preventing data corruption and ensuring
    // thread safety. We also set the report_dirty flag to true to indicate that
    // new data is available
    //  and needs to be sent in the next interrupt report.
    critical_section_enter_blocking(&report_cs);
    memcpy(interrupt_in_data[slot], data + 3, 63);
    report_dirty[slot] = true;
    critical_section_exit(&report_cs);
#if ENABLE_BATT_LED
    if (slot == BT_USB_SLOT) {
      battery_led_note_report();
    }
#endif
  }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
#ifdef ENABLE_WAKE_HID
  if (itf == usb_kbd_hid_instance()) {
    if (reqlen >= 8) {
      memset(buffer, 0, 8);
      return 8;
    }
    return 0;
  }
  // MINIMAL's non-keyboard instance is the inert dummy HID, NOT a gamepad.
  // Don't route its GET_REPORT into the BT feature path (which would query a
  // controller that isn't connected). Return 0 (STALL); the host never reads
  // it.
  if (!usb_descriptor_variant_is_full()) {
    return 0;
  }
#endif
  (void)report_type;

  const int mapped = usb_hid_instance_slot(itf);
  if (mapped < 0 || mapped >= BT_MAX_SLOTS) {
    return 0;
  }
  const uint8_t slot = (uint8_t) mapped;

  BtStatus st;
  bt_get_status(slot, &st);
  if (!st.connected) {
    // Empty slot: serve a plausible blob so hid-playstation's bind-time
    // probes (calibration 0x05, firmware 0x20, pairing 0x09) don't stall the
    // interface — a stalled probe fails the driver bind and the slot stays
    // dead until re-enumeration. Prefer a live pad's cache, else the
    // persisted snapshot from the first pad ever paired. Caveat: a pad that
    // connects AFTER enumeration inherits the placeholder IMU calibration
    // until the next re-enumeration.
    std::vector<uint8_t> ph;
    if (!bt_feature_cached_any(report_id, ph)) {
      bt_feature_snapshot_get(report_id, ph);
    }
    if (ph.size() <= 1) {
      return 0;
    }
    size_t n = ph.size() - 1;
    if (n > reqlen) n = reqlen;
    memcpy(buffer, ph.data() + 1, n);
    if (report_id == 0x09 && n >= 6) {
      // Pairing info carries the controller MAC, which hosts use as the
      // device's unique id — make each empty slot's MAC distinct.
      buffer[0] ^= (uint8_t)(slot + 1);
    }
    return (uint16_t) n;
  }

  // DSE profiles: while the unlock + prefetch is still in progress, return 0
  // (NAK) for profile reads so the PS app retries rather than caching an
  // empty snapshot. Still kick off the background BT fetch. (The DSE profile
  // machinery serves the USB-exposed slot only.)
  if (slot == BT_USB_SLOT && dse_is_profile_report(report_id) &&
      !dse_profiles_ready()) {
    get_feature_data(slot, report_id, reqlen);
    return 0;
  }

  std::vector<uint8_t> feature_data = get_feature_data(slot, report_id, reqlen);
  if (!feature_data.empty()) {
    memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
  }

  return feature_data.empty() ? 0 : feature_data.size() - 1;
}

bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *p_request) {
  (void)rhport;
  uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
  uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

  if (itf == 1) {
    printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
    spk_active = alt;
  } else if (itf == 2) {
    printf("[AUDIO] Set interface Mic to alternate setting %d\n", alt);
    mic_active = alt;
  }

  return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
#ifdef ENABLE_WAKE_HID
  if (itf == usb_kbd_hid_instance()) {
    // Drop keyboard SET_REPORT (host LED state).
    return;
  }
  // MINIMAL's non-keyboard instance is the inert dummy HID; ignore reports.
  if (!usb_descriptor_variant_is_full()) {
    return;
  }
#endif
  (void)report_type;

  const int mapped = usb_hid_instance_slot(itf);
  if (mapped < 0 || mapped >= BT_MAX_SLOTS) {
    return;
  }
  const uint8_t slot = (uint8_t) mapped;

  // INTERRUPT OUT
  if (report_id == 0) {
    switch (buffer[0]) {
    case 0x02: {
      state_update(slot, buffer + 1, bufsize - 1);
      // When the headset/speaker is active, output reports for the audio
      // slot normally piggyback on the audio frame path, so we defer (break)
      // here to avoid double-send. But a rumble-bearing SetStateData
      // (UseRumbleNotHaptics flags set) must go out NOW, or rumble
      // lags/drops a frame while audio is streaming. Non-audio slots have no
      // frame to piggyback on and always send immediately.
      // (Ported from upstream awalol/DS5Dongle 07ecbb3, issue #182.)
      bool send_now = ((buffer[1] >> 1) & 1) ||  // UseRumbleNotHaptics
                      ((buffer[39] >> 3) & 1);   // UseRumbleNotHaptics2
      if (!send_now && slot == tier_audio_slot() && spk_active) {
        break;
      }
      state_push_slot_to_bt(slot);
      break;
    }
    }
  }
  if (report_id == 0x80 ||
      // DSE: Write Profile Block
      report_id == 0x60 || report_id == 0x62 || report_id == 0x61) {
    set_feature_data(slot, report_id, const_cast<uint8_t *>(buffer), bufsize);
    return;
  }
}

int main() {
  // Arm the watchdog before anything that can hang. After a UF2 flash the
  // core warm-resets while the CYW43 radio keeps running un-power-cycled
  // (VBUS never dropped), and radio bring-up against a wedged chip can hang
  // or fail; without a watchdog that strands the dongle with USB
  // disconnected until someone replugs it. Generous period because early
  // init legitimately sleeps (vreg settle, POST blink); re-armed to the
  // tight 1 s period just before the main loop.
  watchdog_enable(8000, true);

#if SYS_CLOCK_KHZ != 150000
  // Overclock path: raise core voltage before bumping the system clock.
  // (1.20V is stable/safe for 320 MHz.) At the stock 150 MHz this is skipped —
  // RAM-relocated hot paths make the overclock unnecessary, and the SDK's
  // default clock init handles the stock case.
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(1000);
  set_sys_clock_khz(SYS_CLOCK_KHZ, true);
#endif

  board_init();
  // Mirror all printf diagnostics into a RAM ring served at /api/log, so
  // logs are readable from the browser without a UART adapter.
  weblog_init();
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE,
                                 .speed = TUSB_SPEED_FULL};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  sleep_ms(150);
  tud_disconnect();
  board_init_after_tusb();

  // The radio is the one piece that survives a warm reset with stale state,
  // so its init gets retries (cyw43_arch_init() deinits itself on failure)
  // and, if it never comes up, a watchdog reboot for a fresh start — never
  // a dead return with USB left disconnected.
  bool radio_up = false;
  for (int attempt = 1; attempt <= 3 && !radio_up; attempt++) {
    watchdog_update();
    radio_up = cyw43_arch_init() == 0;
    if (!radio_up) {
      printf("Failed to initialize CYW43 (attempt %d)\n", attempt);
      sleep_ms(100);
    }
  }
  if (!radio_up) {
    printf("CYW43 never came up -> rebooting\n");
#ifdef ENABLE_LED_STRIP
    // The strip holds this red frame across the reboot, so a boot-loop
    // shows as solid red until a boot succeeds (or the dongle is unplugged).
    ledstrip_panic_red();
#endif
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
  }

  // Load persisted config from flash BEFORE usb_net_init()/wifi_net_init():
  // the web server picks its subnet from get_config().webconfig_subnet and the
  // WiFi transport reads the stored credentials (STA vs AP onboarding) + mDNS
  // hostname, so the saved values must be in place first.
  config_load();

  // Bring up the WiFi transport (no-op with ENABLE_WIFI_WOL off). Decides STA
  // (provisioned: join the home WLAN, async) vs AP + captive portal
  // (unprovisioned onboarding). In WiFi builds the SDK's cyw43_arch_init()
  // above already brought lwIP up (CYW43_LWIP=1).
  wifi_net_init();

  // Bring up the onboard config web server (USB CDC-NCM + lwIP). No-op when
  // ENABLE_WEBCONFIG is off. Skipped during AP onboarding: that mode is a
  // dedicated setup network (portal + httpd started by wifi_net_init), and
  // the NCM netif must not compete for netif_default with the AP netif.
  // Diagnostics print to UART0 (GP0 TX, 115200 8N1), not USB.
  if (!wifi_net_in_ap_mode()) {
    usb_net_init();
  }

  // Power-On Self Test (POST) LED pattern: 3 rapid flashes to confirm
  // successful CPU overclocking and CYW43 Bluetooth module initialization.
  for (int i = 0; i < 6; i++) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2 == 0);
    sleep_ms(80);
  }
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
  battery_led_init();
#endif

#ifdef ENABLE_LED_STRIP
  // After cyw43_arch_init(): the radio's PIO SPI state machine is already
  // claimed, so the strip's dynamic claim can't collide with it.
  ledstrip_init();
#endif

  // watchdog_enable_caused_reboot(), not watchdog_caused_reboot(): the
  // bootrom also reboots via the watchdog hardware (e.g. after a UF2
  // flash), which is not a crash — only a timeout of OUR armed watchdog
  // earns the crash blink (and its 3 s boot delay).
  if (watchdog_enable_caused_reboot()) {
    printf("Rebooted by Watchdog!\n");
#ifdef ENABLE_LED_STRIP
    // Solid red on the strip too — the Pico's own LED can be hidden by the
    // mounting. Cleared by the first normal frame once the main loop runs.
    ledstrip_panic_red();
#endif
    // 当崩溃重启以后，闪三下灯
    for (int i = 0; i < 6; i++) {
      watchdog_update();
      if (i % 2 == 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
      } else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
      }
      sleep_ms(500);
    }
  } else {
    printf("Clean boot\n");
  }

  // Heap telemetry for bring-up: the audio path + web server + per-slot send
  // FIFOs all draw from one heap, and exhaustion here surfaces as a watchdog
  // reboot. Logged once at boot; compare across builds when chasing OOM.
  {
    extern char __StackLimit[], __bss_end__[];
    printf("[MEM] heap region %d bytes (bss_end %p..stacklimit %p), malloc used %d\n",
           (int) (__StackLimit - __bss_end__), (void *) __bss_end__,
           (void *) __StackLimit, mallinfo().uordblks);
  }

  // Seed every slot's input buffer with the neutral idle report so the host
  // sees centered sticks (not zeros) before the first BT report arrives.
  for (int slot = 0; slot < BT_MAX_SLOTS; slot++) {
    memcpy(interrupt_in_data[slot], idle_input_report,
           sizeof(idle_input_report));
  }

  // Initialize the critical section for the report buffers
  critical_section_init(&report_cs);
  wake_init();

  // WiFi onboarding (AP + captive portal) is a dedicated setup mode: no
  // controller, no audio. Crucially, BT classic page-scan/inquiry contends
  // with the SoftAP on the single shared CYW43 radio -- upstream observed the
  // AP beaconing but never admitting a station with BT up (stas=0, client
  // loops DHCP forever). So in AP mode we skip BT + audio entirely, handing
  // the radio to the AP (and freeing ~110 KB of heap; core1 is never
  // launched, which is why config_save() has the direct-write path). Normal
  // STA operation brings BT/audio up as usual.
  const bool ap_onboarding = wifi_net_in_ap_mode();
  if (!ap_onboarding) {
    watchdog_update();
    bt_init();
    bt_register_data_callback(on_bt_data);

    watchdog_update();
    audio_init();
    state_init();
  } else {
    printf("[BOOT] AP onboarding mode: skipping BT + audio (radio handed to SoftAP)\n");
  }

#ifdef ENABLE_WAKE_HID
  // Enumerate immediately as the FULL variant: every gamepad interface (plus
  // audio, NCM, wake keyboard) is present whenever the dongle is plugged in,
  // so controllers join and leave with zero USB disruption. Bind-time feature
  // probes for not-yet-connected pads are answered from the persisted
  // snapshot (bt_feature_snapshot_get). Being enumerated before the host
  // suspends is also what makes remote-wakeup possible.
  //
  // NEVER in AP onboarding mode. Unlike upstream (whose onboarding enumerates
  // an inert MINIMAL device), our FULL face exposes audio + NCM + gamepad
  // interfaces whose backing state (audio_init, state_init, usb_net_init) was
  // deliberately skipped above -- the host's first NCM frame hit the NULL
  // netif input fn: hard fault -> watchdog -> re-enumerate, a crash/replug
  // storm that bootlooped the dongle and took the host's USB stack with it
  // (HW-observed on SteamOS). During onboarding USB is power only; the
  // device is configured over the portal and reboots into STA when done.
  if (!ap_onboarding) {
    tud_connect();
  }
#endif

  watchdog_enable(1000, true);

  // Onboarding loop: a stripped main loop with BT/audio/HID skipped (they were
  // never initialised in AP mode). Pump only the radio/lwIP (cyw43_arch_poll +
  // wifi_net_task drive the SoftAP RX, DHCP/DNS servers, scan, captive portal)
  // and feed the watchdog. USB stays tud_disconnect()'d (see above); tud_task
  // is still pumped so the stack stays consistent if anything ever connects
  // it. The device leaves this loop by rebooting into STA mode once the user
  // provisions (wifi_net_task fires the deferred watchdog_reboot). The LED
  // strip and the NCM web server are deliberately not serviced here -- setup
  // mode only.
  if (ap_onboarding) {
    while (1) {
      watchdog_update();
      cyw43_arch_poll();
      tud_task();
      wifi_net_task();
#ifdef ENABLE_LED_STRIP
      // Setup-mode indicator: teal chase (reserved for onboarding). Not
      // ledstrip_tick() -- that reads BT state, which was never initialised
      // in this mode. Its first frame also clears a panic-red frame latched
      // by a prior crash blink, which the render-nothing AP loop would
      // otherwise leave lit forever.
      ledstrip_setup_chase_tick();
#endif
      sleep_us(250);
    }
  }

  while (1) {
    watchdog_update();
    cyw43_arch_poll();
    bt_connection_watchdog_tick();
    bt_blacklist_persist_if_dirty();
    bt_feature_snapshot_persist_if_dirty();
    bt_pump();
    tud_task();
    wake_task();
#ifdef ENABLE_WAKE_HID
    usb_variant_task();
#endif
    // Service lwIP timers for the onboard config web server (no-op when
    // ENABLE_WEBCONFIG is off). Cheap; not in the audio hot path.
    usb_net_task();
    // WiFi STA link supervision + mDNS registration + deferred reboots
    // (no-op with ENABLE_WIFI_WOL off). RX is pumped by cyw43_arch_poll().
    wifi_net_task();
    audio_loop();
    interrupt_loop();
    // DSE Edge profile snapshot prefetch/unlock state machine.
    dse_task();
#if ENABLE_BATT_LED
    battery_led_tick();
#endif
#ifdef ENABLE_LED_STRIP
    ledstrip_tick();
#endif
    // Yield only when the hot paths are idle; otherwise keep draining USB/BT.
    if (!tud_audio_available() && !bt_send_pending()) {
      sleep_us(250);
    } else {
      tight_loop_contents();
    }
  }
}
