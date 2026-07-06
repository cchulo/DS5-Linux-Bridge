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
#include <cstdio>
#include <malloc.h>

#include "config.h"
#include "dse.h"
#include "usb_net.h"
#include "tier.h"

#if ENABLE_BATT_LED
#include "battery_led.h"
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
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE,
                                 .speed = TUSB_SPEED_FULL};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  sleep_ms(150);
  tud_disconnect();
  board_init_after_tusb();

  if (cyw43_arch_init()) {
    printf("Failed to initialize CYW43\n");
    return 1;
  }

  // Load persisted config from flash BEFORE usb_net_init(): the web server
  // picks its subnet from get_config().webconfig_subnet, so the saved value
  // must be in place first (otherwise it always reads the default).
  config_load();

  // Bring up the onboard config web server (USB CDC-NCM + lwIP). No-op when
  // ENABLE_WEBCONFIG is off. lwIP is ours alone here (CYW43_LWIP=0).
  // Diagnostics print to UART0 (GP0 TX, 115200 8N1), not USB.
  usb_net_init();

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

  if (watchdog_caused_reboot()) {
    printf("Rebooted by Watchdog!\n");
    // 当崩溃重启以后，闪三下灯
    for (int i = 0; i < 6; i++) {
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

  bt_init();
  bt_register_data_callback(on_bt_data);

  audio_init();
  state_init();

#ifdef ENABLE_WAKE_HID
  // Enumerate immediately as the FULL variant: every gamepad interface (plus
  // audio, NCM, wake keyboard) is present whenever the dongle is plugged in,
  // so controllers join and leave with zero USB disruption. Bind-time feature
  // probes for not-yet-connected pads are answered from the persisted
  // snapshot (bt_feature_snapshot_get). Being enumerated before the host
  // suspends is also what makes remote-wakeup possible.
  tud_connect();
#endif

  watchdog_enable(1000, true);

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
    audio_loop();
    interrupt_loop();
    // DSE Edge profile snapshot prefetch/unlock state machine.
    dse_task();
#if ENABLE_BATT_LED
    battery_led_tick();
#endif
    // Yield only when the hot paths are idle; otherwise keep draining USB/BT.
    if (!tud_audio_available() && !bt_send_pending()) {
      sleep_us(250);
    } else {
      tight_loop_contents();
    }
  }
}
