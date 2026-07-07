//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

#include "slots.h"

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

// Data callback now carries the slot index the packet arrived on. Slots are
// assigned at connection time (lowest free slot); slot BT_USB_SLOT is the one
// bridged to USB HID/audio until the per-slot interface fan-out lands.
typedef void (*bt_data_callback_t)(uint8_t slot, CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_write(uint8_t slot, const uint8_t *data, uint16_t len, bool kick = true);
// Kick the BT send chains if pending. Called from main loop after
// cyw43_arch_poll() so the kick cost is paid outside audio_loop.
void bt_pump();
bool bt_send_pending();
void bt_get_signal_strength(uint8_t slot, int8_t *rssi);

// Number of slots with a live ACL connection.
int bt_connected_count();

// Lowest slot index with a live ACL connection, or -1 if none.
int bt_lowest_connected_slot();

// Live controller status for the web UI / Decky plugin (GET /api/status).
// All fields are cheap reads of data the firmware already tracks. battery_pct
// and charging are only meaningful when connected (and after the first 0x31
// report). (RSSI was intentionally omitted: BR/EDR HCI_Read_RSSI is relative to
// the Golden Receive Power Range and reads ~0 in normal use -- not a useful
// signal-strength number to surface.)
struct BtStatus {
    bool    connected;
    bool    is_dse;       // true = DualSense Edge, false = standard DualSense
    uint8_t battery_pct;  // 0-100 (DS5 reports in 10% steps); 0 if unknown
    bool    charging;     // true while the controller is charging or full
    bool    battery_valid;// false until a fresh input report has been seen
    uint8_t addr[6];      // controller BD_ADDR; zeros when not connected
};
void bt_get_status(uint8_t slot, BtStatus *out);

std::vector<uint8_t> get_feature_data(uint8_t slot, uint8_t reportId, uint16_t len);
void init_feature(uint8_t slot);
void set_feature_data(uint8_t slot, uint8_t reportId, uint8_t *data, uint16_t len);

// Slot-BT_USB_SLOT conveniences for the USB bridge and the DSE profile module,
// which both speak to the USB-exposed controller only.
inline std::vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len) {
    return get_feature_data(BT_USB_SLOT, reportId, len);
}
inline void set_feature_data(uint8_t reportId, uint8_t *data, uint16_t len) {
    set_feature_data(BT_USB_SLOT, reportId, data, len);
}

// DSE profile module accessors (bound to slot BT_USB_SLOT). bt_feature_cached
// copies the cached feature report (as received: leading report id byte
// included) into `out` and returns true if present.
uint16_t bt_control_cid();
void bt_control_send(const uint8_t *data, uint16_t len);
bool bt_feature_cached(uint8_t reportId, std::vector<uint8_t> &out);

// Like bt_feature_cached but searches every connected slot's cache (lowest
// slot wins). Used to synthesize plausible feature reports for EMPTY slots so
// hid-playstation's bind-time probes (calibration 0x05, firmware 0x20,
// pairing 0x09) don't stall an interface whose controller hasn't connected
// yet.
bool bt_feature_cached_any(uint8_t reportId, std::vector<uint8_t> &out);

// Persisted feature snapshot (config flash): copies the stored blob for
// reportId 0x05/0x20/0x09 into `out` (as-cached format, leading report id
// included). False when no snapshot has been captured yet. Used to answer
// bind-time probes at boot, before any controller has connected this session.
bool bt_feature_snapshot_get(uint8_t reportId, std::vector<uint8_t> &out);

// Flush a freshly captured feature snapshot to flash (deferred from the BT
// control-channel hot path). Call every main-loop iteration; writes at most
// once per capture (normally once in the dongle's lifetime).
void bt_feature_snapshot_persist_if_dirty();

// Tells every connected DualSense to power off (same as a long-press of the
// PS button). No-op for empty slots. Used on host-suspend so controllers
// don't sit awake until their idle timers fire.
void bt_dualsense_power_off();

// Power off a single slot's controller (bond kept; it reconnects on the next
// PS press). Used by the PS+Triangle controller shortcut.
void bt_slot_power_off(uint8_t slot);

// Swap two slots (web UI "move controller"). Everything that belongs to the
// PAD moves (BT link state, input stream, feature cache); output state stays
// with the SEAT (the host-facing interface), so the moved pad adopts its new
// seat's lightbar/player LEDs immediately. Returns false while either seat is
// mid-connection-setup or on bad indices.
bool bt_slot_swap(uint8_t a, uint8_t b);

// Tick connection watchdogs (pre-ACL attempt + per-slot setup). Call from main loop.
void bt_connection_watchdog_tick();

//--------------------------------------------------------------------+
// Paired-device (bond) management, exposed to the web config UI.
// Bonds are BR/EDR link keys persisted by BTstack in its flash TLV bank
// (capacity NVM_NUM_LINK_KEYS). These wrap the BTstack gap_* link-key API so
// usb_net.cpp doesn't pull in btstack headers. All run on the core0 main-loop
// context (same as the btstack run loop), so no extra locking is needed.
//--------------------------------------------------------------------+

// Number of bytes in a Bluetooth address (matches btstack bd_addr_t).
#define BT_ADDR_LEN 6

// Copy up to `max` stored bond addresses into addrs (each BT_ADDR_LEN bytes).
// Returns the number written.
int bt_bond_list(uint8_t (*addrs)[BT_ADDR_LEN], int max);

// Forget a single bond by address (6 bytes). Returns true if it was issued.
bool bt_bond_forget(const uint8_t *addr);

// Forget every stored bond.
void bt_bond_forget_all();

// Open a fresh 30s inquiry to pair an additional controller, even when
// controllers are already bonded. Returns false (rejected) when every bond
// seat is occupied, or when all slots are connected on a multi-slot build
// (single-slot builds keep the upstream swap behavior: disconnect the active
// controller, keep its bond, pair the new one). Invoked from the web API
// (POST /api/bonds action=pair).
bool bt_start_pairing();

// If at least one controller is connected, copy the lowest connected slot's
// address into addr_out (BT_ADDR_LEN bytes) and return true.
bool bt_connected_addr(uint8_t *addr_out);

// Flush the forgotten-controller blacklist to flash if it changed (deferred
// from the HID-open hot path). Call every main-loop iteration.
void bt_blacklist_persist_if_dirty();

#endif //DS5_BRIDGE_BT_H
