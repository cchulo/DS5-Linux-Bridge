//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstring>
#include <utility>
#include "bt.h"
#include "usb.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include "btstack_event.h"
#include "btstack_tlv.h" // persistent blacklist storage (forget-bond enforcement)
#include "gap.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "state_mgr.h"
#include "tier.h"
#include "dse.h"
#include "wake.h"
#include "pico/util/queue.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

// Connection-attempt watchdog: if a connection commits to a device (inquiry
// found one / incoming request accepted) but doesn't reach USB-enumeration
// within this window, tear down and retry. Catches the silent stalls caused by
// USB 3.0 2.4 GHz RF interference on the CYW43 BT radio.
#define CONNECT_WATCHDOG_TIMEOUT_US (10 * 1000 * 1000)

using std::unordered_map;
using std::vector;
using std::queue;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

struct bt_slot;
static void bt_feature_snapshot_maybe_capture(bt_slot *s);
static void bt_send_full_state(uint8_t slot);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;

constexpr size_t BT_SEND_MAX_PACKET_SIZE = 400; // 0xA2 header + 398-byte audio report + slack

struct send_element {
    uint8_t data[BT_SEND_MAX_PACKET_SIZE];
    uint16_t len;
};

//--------------------------------------------------------------------+
// Slots. One per concurrent controller connection. A slot is a connection
// seat, assigned at connect time: the lowest free slot wins, so the first
// pad powered on in a session is always slot 0 / player 1 regardless of
// pairing order (matches PS5 behavior). Bonds carry no slot number.
//--------------------------------------------------------------------+

struct bt_slot {
    hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
    bd_addr_t addr{};
    uint16_t control_cid = 0;
    uint16_t interrupt_cid = 0;
    // Only new pairings (outgoing, inquiry-initiated) create the L2CAP
    // channels from our side; auto-reconnects are controller-initiated and go
    // through the registered services. 只有新匹配的设备才用创建channel，自动重连走的是service
    bool new_pair = false;
    bool check_dse = false;
    bool is_dse = false;
    // Per-slot setup watchdog timestamp. 0 == not armed; armed == ACL is up
    // but the controller hasn't identified yet.
    absolute_time_t connect_attempt_started = 0;
    absolute_time_t inactive_time = 0; // 手柄长时间静默
    int8_t rssi = 0;
    unordered_map<uint8_t, vector<uint8_t> > feature_data;
    // Outbound L2CAP FIFO + can-send-now chain state (see bt_write/bt_pump).
    queue_t send_fifo{};
    volatile bool send_chain_active = false;
    send_element retry_packet{};
    bool retry_pending = false;
};

static bt_slot slots[BT_MAX_SLOTS];

static bt_slot *slot_by_handle(hci_con_handle_t handle) {
    if (handle == HCI_CON_HANDLE_INVALID) return nullptr;
    for (auto &s : slots) {
        if (s.acl_handle == handle) return &s;
    }
    return nullptr;
}

static bt_slot *slot_by_addr(const bd_addr_t addr) {
    for (auto &s : slots) {
        if (s.acl_handle != HCI_CON_HANDLE_INVALID && bd_addr_cmp(s.addr, addr) == 0) return &s;
    }
    return nullptr;
}

// Find the slot owning an L2CAP CID (control or interrupt).
static bt_slot *slot_by_cid(uint16_t cid) {
    if (cid == 0) return nullptr;
    for (auto &s : slots) {
        if (s.control_cid == cid || s.interrupt_cid == cid) return &s;
    }
    return nullptr;
}

static bt_slot *slot_by_interrupt_cid(uint16_t cid) {
    if (cid == 0) return nullptr;
    for (auto &s : slots) {
        if (s.interrupt_cid == cid) return &s;
    }
    return nullptr;
}

static int slot_index(const bt_slot *s) {
    return (int) (s - slots);
}

static int bt_free_slot_count() {
    int n = 0;
    for (auto &s : slots) {
        if (s.acl_handle == HCI_CON_HANDLE_INVALID) n++;
    }
    return n;
}

int bt_connected_count() {
    return BT_MAX_SLOTS - bt_free_slot_count();
}

bool bt_slot_swap(uint8_t a, uint8_t b) {
    if (a >= BT_MAX_SLOTS || b >= BT_MAX_SLOTS || a == b) return false;
    bt_slot &x = slots[a];
    bt_slot &y = slots[b];
    // Refuse while either seat is mid-setup: HCI events between accept and
    // HID-open would land on a half-swapped slot.
    if (x.connect_attempt_started != 0 || y.connect_attempt_started != 0) return false;
    if (x.acl_handle == HCI_CON_HANDLE_INVALID &&
        y.acl_handle == HCI_CON_HANDLE_INVALID) {
        return true; // both empty: nothing to move
    }
    printf("[BT] Swap slots %u <-> %u\n", a, b);
    // Event routing is lookup-based (handle/CID -> slot scan), so swapping
    // the per-pad state is all it takes; in-flight retries, send chains and
    // inactivity stamps travel with their connection.
    std::swap(x.acl_handle, y.acl_handle);
    {
        bd_addr_t t;
        bd_addr_copy(t, x.addr);
        bd_addr_copy(x.addr, y.addr);
        bd_addr_copy(y.addr, t);
    }
    std::swap(x.control_cid, y.control_cid);
    std::swap(x.interrupt_cid, y.interrupt_cid);
    std::swap(x.new_pair, y.new_pair);
    std::swap(x.check_dse, y.check_dse);
    std::swap(x.is_dse, y.is_dse);
    std::swap(x.inactive_time, y.inactive_time);
    std::swap(x.rssi, y.rssi);
    x.feature_data.swap(y.feature_data);
    std::swap(x.send_fifo, y.send_fifo);
    {
        const bool t = x.send_chain_active;
        x.send_chain_active = y.send_chain_active;
        y.send_chain_active = t;
    }
    std::swap(x.retry_packet, y.retry_packet);
    std::swap(x.retry_pending, y.retry_pending);
    // Inputs follow the pad; output state stays with the seat (it belongs to
    // the host-facing interface).
    bridge_swap_slot_input(a, b);
    // The composite's DS/DSE identity follows the USB-exposed slot's model.
    if (slots[BT_USB_SLOT].acl_handle != HCI_CON_HANDLE_INVALID) {
        is_dse = slots[BT_USB_SLOT].is_dse;
    }
    // Each seat pushes its state to whichever pad now sits there, so
    // lightbars and player LEDs update immediately.
    bt_send_full_state(a);
    bt_send_full_state(b);
    return true;
}

int bt_lowest_connected_slot() {
    for (auto &s : slots) {
        if (s.acl_handle != HCI_CON_HANDLE_INVALID) return slot_index(&s);
    }
    return -1;
}

// Session-order assignment: lowest free slot. (A bond-sticky policy would
// hook in here.) Returns nullptr when full.
static bt_slot *slot_alloc(const bd_addr_t addr) {
    // Same address already seated (stale reconnect race): reuse its slot.
    if (bt_slot *existing = slot_by_addr(addr)) return existing;
    for (auto &s : slots) {
        if (s.acl_handle == HCI_CON_HANDLE_INVALID) return &s;
    }
    return nullptr;
}

// Push a slot's full cached output state (report 0x32: lightbar, player
// LEDs, rumble/FFB) to whichever pad sits there. Used at connect and after a
// slot swap.
static void bt_send_full_state(uint8_t slot) {
    if (slot >= BT_MAX_SLOTS) return;
    if (slots[slot].interrupt_cid == 0) return;
    uint8_t report32[142]{};
    report32[0] = 0x32;
    report32[1] = 0x10; // reportSeqCounter
    report32[2] = 0x10 | 0 << 6 | 1 << 7;
    report32[3] = 0x3f; // 63 bytes
    state_get(slot, report32 + 4, sizeof(SetStateData));
    bt_write(slot, report32, sizeof(report32));
}

void bt_slot_colors_refresh() {
    for (uint8_t i = 0; i < BT_MAX_SLOTS; i++) {
        if (slots[i].interrupt_cid == 0) continue;
        state_apply_slot_color(i);
        bt_send_full_state(i);
    }
}

// Reset a slot's connection state (does not touch the send FIFO's queue_t
// storage, which is drained instead of re-inited).
static void slot_clear(bt_slot *s) {
    s->acl_handle = HCI_CON_HANDLE_INVALID;
    s->control_cid = 0;
    s->interrupt_cid = 0;
    s->new_pair = false;
    s->check_dse = false;
    s->is_dse = false;
    s->connect_attempt_started = 0;
    s->rssi = 0;
    s->feature_data.clear();
    while (queue_try_remove(&s->send_fifo, NULL)) {}
    s->send_chain_active = false;
    s->retry_pending = false;
}

//--------------------------------------------------------------------+
// Pending pairing attempt (pre-ACL). Only one outgoing inquiry/pair runs at
// a time, so this stays global; connected state lives in the slots.
//--------------------------------------------------------------------+

static bd_addr_t current_device_addr; // inquiry target
static bool device_found = false;
static bool new_pair = false;

// Pairing window opened by the web UI (POST /api/bonds action=pair). While set:
//   - inquiry runs even with bonds stored (overriding the normal
//     "bonded -> page scan, no inquiry" gate);
//   - on a single-slot build the active controller (if any) is torn down,
//     keeping its bond, and inquiry opens from the disconnect-complete handler;
//   - incoming auto-reconnects are REJECTED when they would consume the last
//     free slot, so a just-disconnected controller can't page back in and win
//     the seat reserved for the new pairing.
// Cleared when a new controller opens HID, or when the inquiry window closes
// with nothing found.
static bool pairing_window = false;

// Pre-ACL connection-attempt watchdog timestamp (outgoing create / incoming
// accept until CONNECTION_COMPLETE hands off to the slot's own watchdog).
static absolute_time_t connect_attempt_started = 0;

// Persistent blacklist of controllers the user forgot via the web UI. Survives
// power-cycles via BTstack TLV flash. Without it, forgetting a controller that
// is connected (or actively paging us to auto-reconnect) is futile: BTstack
// re-accepts and re-bonds it instantly. We block it at CONNECTION_REQUEST /
// CONNECTION_COMPLETE so PS-only auto-reconnect fails; the INQUIRY path (an
// explicit PS+Share re-pair) is still allowed and removes the MAC from the
// blacklist on successful pair.
#define BT_BLACKLIST_TLV_TAG  ((uint32_t) 0x424C434B) // ASCII 'BLCK'
static bd_addr_t bt_cleared_addrs[NVM_NUM_LINK_KEYS];
static int bt_cleared_addrs_count = 0;
// Deferred-persist: bt_blacklist_remove() (on successful re-pair) sets dirty
// instead of writing flash inline, so the L2CAP HID-open hot path never blocks
// on flash. The main loop flushes after a settle window.
static bool bt_blacklist_dirty = false;
static uint32_t bt_blacklist_dirty_ms = 0;

static bt_data_callback_t bt_data_callback = nullptr;

void bt_register_data_callback(bt_data_callback_t callback) {
    bt_data_callback = callback;
}

static bool bt_disconnect_slot(bt_slot *s) {
    if (!s || s->acl_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }
    // 0x13 = remote user terminated connection
    hci_send_cmd(&hci_disconnect, s->acl_handle, 0x13);
    return true;
}

// Keep the radio connectable exactly while a free slot exists. Discoverable
// tracks connectable (pads page us directly; discoverable is only relevant
// during their own pairing scans, harmless to leave in step).
static void bt_update_scan_enable() {
    const bool open = bt_free_slot_count() > 0;
    gap_connectable_control(open ? 1 : 0);
    gap_discoverable_control(open ? 1 : 0);
}

// True if BTstack has at least one stored controller link key. Used to gate
// inquiry: once a controller is bonded the dongle stops looking for new ones
// (the bonded controller reconnects on its own via page scan) and only inquires
// again on an explicit bt_start_pairing() request.
static bool bt_has_stored_link_key() {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return false;
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    const bool has_key = gap_link_key_iterator_get_next(&it, addr, key, &type);
    gap_link_key_iterator_done(&it);
    return has_key;
}

static int bt_bond_count() {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return 0;
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    int n = 0;
    while (gap_link_key_iterator_get_next(&it, addr, key, &type)) n++;
    gap_link_key_iterator_done(&it);
    return n;
}

// Recovery/looping restart of inquiry. Gated on bond presence: with a controller
// bonded we keep the radio connectable for page-scan reconnect but do NOT
// re-open inquiry, so a transient failure can't silently reopen pairing.
static void bt_restart_inquiry() {
    device_found = false;
    new_pair = false;
    connect_attempt_started = 0;
    bt_update_scan_enable();
    if (bt_has_stored_link_key()) {
        printf("[BT] Stored controller -> page scan, skip inquiry restart\n");
        return;
    }
    gap_inquiry_stop();
    gap_inquiry_start(30);
}

// Open a fresh 30s inquiry to pair an additional controller, even when
// controllers are already bonded. The deliberate "add another controller"
// path, invoked from the web API (POST /api/bonds action=pair).
//
// Multi-slot: with a free slot available the inquiry simply runs alongside the
// connected controllers; nothing is torn down. With every bond seat occupied
// the request is rejected (forget one first). Single-slot builds keep the
// upstream swap UX: tear the active link down (KEEPING its bond, unlike
// forget) and open inquiry once the disconnect completes; the newly paired
// controller becomes the active connection, and the old one still reconnects
// later via its retained link key.
bool bt_start_pairing() {
    if (bt_bond_count() >= NVM_NUM_LINK_KEYS) {
        printf("[BT] Pair request rejected: all %d bond seats occupied\n", NVM_NUM_LINK_KEYS);
        return false;
    }
    if (bt_free_slot_count() == 0) {
#if BT_MAX_SLOTS == 1
        pairing_window = true;
        printf("[BT] Pair request -> disconnect current (keep bond), then inquire\n");
        bt_disconnect_slot(&slots[0]); // inquiry opens in the disconnection-complete handler
        return true;
#else
        printf("[BT] Pair request rejected: all %d slots connected\n", BT_MAX_SLOTS);
        return false;
#endif
    }
    pairing_window = true;
    printf("[BT] Pair request -> open inquiry\n");
    device_found = false;
    new_pair = false;
    connect_attempt_started = 0;
    gap_connectable_control(1);
    gap_discoverable_control(1);
    gap_inquiry_stop();
    gap_inquiry_start(30);
    return true;
}

void bt_connection_watchdog_tick() {
    const absolute_time_t now = get_absolute_time();
    // Pre-ACL attempt (outgoing create / incoming accept that never completed).
    if (connect_attempt_started != 0 &&
        absolute_time_diff_us(connect_attempt_started, now) >= CONNECT_WATCHDOG_TIMEOUT_US) {
        printf("[BT] Connection watchdog: pre-ACL attempt stalled, recovering\n");
        connect_attempt_started = 0; // disarm; the next attempt re-arms
        bt_restart_inquiry();
    }
    // Per-slot setup (ACL up but the controller never identified).
    for (auto &s : slots) {
        if (s.connect_attempt_started == 0) continue;
        if (absolute_time_diff_us(s.connect_attempt_started, now) < CONNECT_WATCHDOG_TIMEOUT_US) continue;
        printf("[BT] Connection watchdog: slot %d setup stalled, disconnecting\n", slot_index(&s));
        s.connect_attempt_started = 0; // disarm; teardown re-triggers recovery
        bt_disconnect_slot(&s);
    }
}

void bt_get_signal_strength(uint8_t slot, int8_t *rssi) {
    if (slot >= BT_MAX_SLOTS) return;
    bt_slot &s = slots[slot];
    // gap_read_rssi() completes asynchronously, so this function can only
    // return the last cached RSSI value. Trigger a refresh afterwards so a
    // subsequent call can observe the updated value once the RSSI event arrives.
    if (rssi != nullptr) {
        *rssi = s.rssi;
    }
    if (s.acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(s.acl_handle);
    }
}

//--------------------------------------------------------------------+
// Forget-bond blacklist (see bt_cleared_addrs declaration above).
//--------------------------------------------------------------------+

// Persist the current blacklist to BTstack TLV flash. Empty -> delete the tag.
static void bt_blacklist_persist() {
    const btstack_tlv_t *tlv = NULL;
    void *tlv_ctx = NULL;
    btstack_tlv_get_instance(&tlv, &tlv_ctx);
    if (!tlv) {
        printf("[BLACKLIST] No TLV instance available, not persisting\n");
        return;
    }
    if (bt_cleared_addrs_count == 0) {
        tlv->delete_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG);
        printf("[BLACKLIST] Empty, deleted from flash\n");
    } else {
        const uint32_t bytes = bt_cleared_addrs_count * (uint32_t) sizeof(bd_addr_t);
        int rc = tlv->store_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG,
                                (const uint8_t *) bt_cleared_addrs, bytes);
        printf("[BLACKLIST] Persisted %d entries (%lu bytes) to flash, rc=%d\n",
               bt_cleared_addrs_count, (unsigned long) bytes, rc);
    }
}

static void bt_blacklist_load() {
    const btstack_tlv_t *tlv = NULL;
    void *tlv_ctx = NULL;
    btstack_tlv_get_instance(&tlv, &tlv_ctx);
    if (!tlv) {
        bt_cleared_addrs_count = 0;
        return;
    }
    int len = tlv->get_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG,
                           (uint8_t *) bt_cleared_addrs, sizeof(bt_cleared_addrs));
    if (len > 0 && (len % (int) sizeof(bd_addr_t)) == 0) {
        bt_cleared_addrs_count = len / (int) sizeof(bd_addr_t);
        if (bt_cleared_addrs_count > NVM_NUM_LINK_KEYS) {
            bt_cleared_addrs_count = NVM_NUM_LINK_KEYS;
        }
        printf("[BLACKLIST] Loaded %d entries from flash\n", bt_cleared_addrs_count);
    } else {
        bt_cleared_addrs_count = 0;
    }
}

static bool bt_blacklist_contains(bd_addr_t addr) {
    for (int i = 0; i < bt_cleared_addrs_count; i++) {
        if (bd_addr_cmp(addr, bt_cleared_addrs[i]) == 0) return true;
    }
    return false;
}

// Add an address to the blacklist (idempotent). Persists immediately -- called
// only from the web-UI forget path, which is a deliberate user action where a
// brief flash blackout is acceptable (config_save runs right after anyway).
static void bt_blacklist_add(const uint8_t *addr) {
    bd_addr_t a;
    bd_addr_copy(a, addr);
    if (bt_blacklist_contains(a)) return;
    if (bt_cleared_addrs_count >= NVM_NUM_LINK_KEYS) return; // full
    bd_addr_copy(bt_cleared_addrs[bt_cleared_addrs_count++], a);
    bt_blacklist_persist();
}

// Remove an address from the blacklist on successful re-pair. Defers the flash
// persist to the main loop so the HID-open hot path stays fast.
static void bt_blacklist_remove(bd_addr_t addr) {
    for (int i = 0; i < bt_cleared_addrs_count; i++) {
        if (bd_addr_cmp(addr, bt_cleared_addrs[i]) == 0) {
            for (int j = i; j < bt_cleared_addrs_count - 1; j++) {
                bd_addr_copy(bt_cleared_addrs[j], bt_cleared_addrs[j + 1]);
            }
            bt_cleared_addrs_count--;
            printf("[BLACKLIST] Removed %s on re-pair, %d remaining (persist deferred)\n",
                   bd_addr_to_str(addr), bt_cleared_addrs_count);
            bt_blacklist_dirty = true;
            bt_blacklist_dirty_ms = to_ms_since_boot(get_absolute_time());
            return;
        }
    }
}

void bt_blacklist_persist_if_dirty() {
    if (!bt_blacklist_dirty) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - bt_blacklist_dirty_ms < 5000) return;
    bt_blacklist_dirty = false;
    bt_blacklist_persist();
}

//--------------------------------------------------------------------+
// Bond (paired-device) management for the web config UI.
// Thin wrappers over the BTstack gap_* link-key API. The iterator only works
// once the stack reaches HCI_STATE_WORKING (the local BT address must be known
// to select the per-controller TLV); since the config page is only reachable
// while a controller is connected, the stack is always working when these run.
//--------------------------------------------------------------------+

int bt_bond_list(uint8_t (*addrs)[BT_ADDR_LEN], int max) {
    if (!addrs || max <= 0) return 0;
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return 0;

    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    int n = 0;
    while (n < max && gap_link_key_iterator_get_next(&it, addr, key, &type)) {
        bd_addr_copy(addrs[n], addr);
        n++;
    }
    gap_link_key_iterator_done(&it);
    return n;
}

bool bt_bond_forget(const uint8_t *addr) {
    if (!addr) return false;
    bd_addr_t a;
    bd_addr_copy(a, addr);
    printf("[BT] Forget bond %s (web UI)\n", bd_addr_to_str(a));
    gap_drop_link_key_for_bd_addr(a);
    // Blacklist so it can't immediately auto-reconnect (PS-only) and re-bond.
    // An explicit PS+Share re-pair goes through the inquiry path and clears it.
    bt_blacklist_add(a);
    // If this is a live controller, drop the link too -- otherwise the active
    // ACL stays up and BTstack re-persists its key.
    if (bt_slot *s = slot_by_addr(a)) {
        printf("[BT] Forgetting the connected controller in slot %d -- disconnecting it\n", slot_index(s));
        bt_disconnect_slot(s);
    }
    return true;
}

void bt_bond_forget_all() {
    printf("[BT] Forget all bonds (web UI)\n");
    // Snapshot every stored bond address into the blacklist BEFORE deleting the
    // keys, plus the currently-connected ones (which may already be in the list).
    uint8_t list[NVM_NUM_LINK_KEYS][BT_ADDR_LEN];
    const int n = bt_bond_list(list, NVM_NUM_LINK_KEYS);
    for (int i = 0; i < n; i++) bt_blacklist_add(list[i]);
    for (auto &s : slots) {
        if (s.acl_handle != HCI_CON_HANDLE_INVALID) bt_blacklist_add(s.addr);
    }

    gap_delete_all_link_keys();
    for (auto &s : slots) {
        if (s.acl_handle != HCI_CON_HANDLE_INVALID) {
            printf("[BT] Disconnecting slot %d (forget all)\n", slot_index(&s));
            bt_disconnect_slot(&s);
        }
    }
}

bool bt_connected_addr(uint8_t *addr_out) {
    for (auto &s : slots) {
        if (s.acl_handle != HCI_CON_HANDLE_INVALID) {
            if (addr_out) bd_addr_copy(addr_out, s.addr);
            return true;
        }
    }
    return false;
}

// The latest gamepad input reports, one row per slot; byte 52 carries the
// DualSense battery state (same byte battery_led.cpp watches). Defined in
// main.cpp.
extern uint8_t interrupt_in_data[][63];

void bt_get_status(uint8_t slot, BtStatus *out) {
    if (!out) return;
    if (slot >= BT_MAX_SLOTS) {
        *out = BtStatus{};
        return;
    }
    const bt_slot &s = slots[slot];
    out->connected = (s.acl_handle != HCI_CON_HANDLE_INVALID);
    out->is_dse = s.is_dse;
    if (out->connected) {
        memcpy(out->addr, s.addr, sizeof(out->addr));
    } else {
        memset(out->addr, 0, sizeof(out->addr));
    }

    // DS5 battery byte: low nibble = level (0-10 -> 0-100% in 10% steps),
    // high nibble = power state (0 discharging, 1 charging, 2 full).
    const uint8_t b   = interrupt_in_data[slot][52];
    const uint8_t lvl = b & 0x0F;
    const uint8_t st  = (b >> 4) & 0x0F;
    // The byte is 0 before any report arrives; treat a connected controller with
    // a non-zero byte as valid. (A genuinely 0%/discharging pad reads 0x00 too,
    // but that is the critical-low case the LED already flags, so reporting it as
    // "unknown" briefly until the next report is harmless.)
    out->battery_valid = out->connected && (b != 0);
    uint16_t pct = (uint16_t) lvl * 10;
    if (pct > 100) pct = 100;
    out->battery_pct = (uint8_t) pct;
    out->charging = (st == 0x1 || st == 0x2); // charging or full
}

void bt_l2cap_init() {
    l2cap_event_callback_registration.callback = &l2cap_packet_handler;
    l2cap_add_event_handler(&l2cap_event_callback_registration);
    // 修复重连后自动断开的关键点
    sdp_init();
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL, MTU_CONTROL, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT, MTU_INTERRUPT, LEVEL_2);

    l2cap_init();
}

int bt_init() {
    for (auto &s : slots) {
        queue_init(&s.send_fifo, sizeof(send_element), 10);
    }

    bt_l2cap_init();

    // SSP (Secure Simple Pairing)
    gap_ssp_set_enable(true);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);

    // Faster reconnect: answer the controller's page on an interlaced page scan
    // with an 11.25ms interval instead of the BTstack default standard-mode scan
    // (~1.28s). The controller pages the dongle on PS-button reconnect, so a
    // tighter page-scan window cuts reconnect latency substantially.
    // (Ported from upstream awalol/DS5Dongle 1d4dbad.)
    gap_set_page_scan_activity(0x0012, 0x0012); // 11.25ms
    gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
    gap_connectable_control(1);
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
                bt_blacklist_load(); // local BD addr known -> TLV is selectable
                // Only open to pairing when no controller is bonded. A bonded
                // controller reconnects on its own via page scan, so we stay
                // connectable but don't inquire (avoids grabbing any nearby
                // DualSense in pairing mode). Use POST /api/bonds action=pair to
                // deliberately add another controller.
                if (bt_has_stored_link_key()) {
                    printf("[BT] Stack ready, stored controller -> page scan, no inquiry\n");
                } else {
                    printf("[BT] Stack ready, no stored controller -> start inquiry\n");
                    gap_inquiry_start(30);
                }
            }
            break;
        }
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE: {
            bd_addr_t addr;
            uint32_t cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                cod = hci_event_inquiry_result_get_class_of_device(packet);
                hci_event_inquiry_result_get_bd_addr(packet, addr);
            } else if (event_type == HCI_EVENT_INQUIRY_RESULT_WITH_RSSI) {
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
            } else {
                cod = hci_event_extended_inquiry_response_get_class_of_device(packet);
                hci_event_extended_inquiry_response_get_bd_addr(packet, addr);
            }

            // CoD 0x002508 = Gamepad (Major: Peripheral, Minor: Gamepad)
            if ((cod & 0x000F00) == 0x000500) {
                printf("[HCI] Gamepad found: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int) cod);
                // Already seated (e.g. inquiry raced a reconnect)? Ignore it.
                if (slot_by_addr(addr)) break;
                bd_addr_copy(current_device_addr, addr);
                device_found = true;
                gap_inquiry_stop();
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete.\n");
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                new_pair = true;
                connect_attempt_started = get_absolute_time(); // arm connection watchdog
                hci_send_cmd(&hci_create_connection, current_device_addr,
                             hci_usable_acl_packet_types(), 0, 0, 0, 1);
                break;
            }
            if (event_type == HCI_EVENT_INQUIRY_COMPLETE) {
                if (pairing_window) {
                    // Pairing window expired with no new controller found; close
                    // it so bonded controllers can auto-reconnect again.
                    pairing_window = false;
                    printf("[HCI] Pairing window closed (inquiry found nothing)\n");
                }
                // Keep looking only while nothing is bonded (first-run flash-
                // and-go); otherwise return to page-scan-only.
                printf("[HCI] Restart inquiry\n");
                bt_restart_inquiry();
            }
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[HCI] CmdStatus %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION && status != ERROR_CODE_SUCCESS) {
                printf("[HCI] Create connection rejected, restart inquiry\n");
                bt_restart_inquiry();
            }
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (opcode != HCI_OPCODE_HCI_READ_RSSI) {
                printf("[HCI] CmdComplete %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            }
            if (opcode == HCI_OPCODE_HCI_READ_RSSI) {
                if (status != ERROR_CODE_SUCCESS || packet[1] < 7) {
                    printf("[HCI] RSSI complete failed status=0x%02X param_len=%u\n", status, packet[1]);
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                bd_addr_t conn_addr;
                hci_event_connection_complete_get_bd_addr(packet, conn_addr);
                // Is this the pending outgoing pair attempt?
                const bool pending_pair = new_pair && bd_addr_cmp(conn_addr, current_device_addr) == 0;
                // Blacklist enforcement: an INCOMING (PS-only auto-reconnect)
                // connection from a forgotten MAC is disconnected here, before we
                // set up state or request auth. Outgoing connections we initiated
                // via inquiry (PS+Share re-pair) have pending_pair == true and are
                // allowed through so the blacklist entry clears at HID open.
                if (!pending_pair && bt_blacklist_contains(conn_addr)) {
                    printf("[HCI] Incoming connection from blacklisted %s - disconnecting\n",
                           bd_addr_to_str(conn_addr));
                    hci_send_cmd(&hci_disconnect, handle, 0x05);
                    break;
                }
                bt_slot *s = slot_alloc(conn_addr);
                if (!s) {
                    printf("[HCI] No free slot for %s - disconnecting\n", bd_addr_to_str(conn_addr));
                    hci_send_cmd(&hci_disconnect, handle, 0x05);
                    break;
                }
                s->acl_handle = handle;
                s->rssi = 0;
                bd_addr_copy(s->addr, conn_addr);
                s->new_pair = pending_pair;
                // Hand the watchdog off from the pre-ACL attempt to the slot.
                s->connect_attempt_started = get_absolute_time();
                connect_attempt_started = 0;
                if (pending_pair) {
                    new_pair = false;
                    device_found = false;
                }
                printf("[HCI] ACL connected handle=0x%04X -> slot %d\n", handle, slot_index(s));
                printf("[HCI] Request authentication on handle=0x%04X\n", handle);
                hci_send_cmd(&hci_authentication_requested, handle);
            } else {
                printf("[HCI] ACL connect failed status=0x%02X, restart inquiry\n", status);
                bt_restart_inquiry();
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_t link_key;
            link_key_type_t link_key_type;
            bool link = gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type);
            printf("[HCI] Link key: ");
            for (int i = 0; i < sizeof(link_key_t); i++) {
                printf("%02X", link_key[i]);
            }
            printf("\n");
            if (link) {
                printf("[HCI] Link key request from %s, reply stored key type=%u\n", bd_addr_to_str(addr),
                       (unsigned int) link_key_type);
                hci_send_cmd(&hci_link_key_request_reply, addr, link_key);
            } else {
                printf("[HCI] Link key request from %s, no key, force re-pair\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_link_key_request_negative_reply, addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("[HCI] User confirmation request from %s, accept\n", bd_addr_to_str(addr));
            hci_send_cmd(&hci_user_confirmation_request_reply, addr);
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("[HCI] Legacy pin request from %s, reply 0000\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            const uint8_t status = hci_event_authentication_complete_get_status(packet);
            const hci_con_handle_t handle = hci_event_authentication_complete_get_connection_handle(packet);
            printf("[HCI] Authentication complete handle=0x%04X status=0x%02X\n", handle, status);
            bt_slot *s = slot_by_handle(handle);
            if (status != ERROR_CODE_SUCCESS) {
                if (s) {
                    printf("[HCI] Authentication failed, drop stored key for %s\n", bd_addr_to_str(s->addr));
                    gap_drop_link_key_for_bd_addr(s->addr);
                    s->connect_attempt_started = 0; // disarm
                    bt_disconnect_slot(s);
                }
            } else {
                hci_send_cmd(&hci_set_connection_encryption, handle, 1);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            const uint8_t status = hci_event_encryption_change_get_status(packet);
            const hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            const uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            printf("[HCI] Encryption change handle=0x%04X status=0x%02X enabled=%u\n", handle, status, enabled);
            bt_slot *s = slot_by_handle(handle);
            if (s && status == ERROR_CODE_SUCCESS && enabled) {
                printf("[L2CAP] Open HID channels (slot %d)\n", slot_index(s));
                if (s->new_pair) {
                    if (s->control_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, s->addr, PSM_HID_CONTROL, MTU_CONTROL,
                                             &s->control_cid);
                    } else if (s->interrupt_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, s->addr, PSM_HID_INTERRUPT,
                                             MTU_INTERRUPT,
                                             &s->interrupt_cid);
                    }
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            if (bt_blacklist_contains(addr)) {
                printf("[HCI] Rejecting connection from %s (forgotten; re-pair via PS+Share)\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_reject_connection_request, addr, 0x0F);
                break;
            }
            if (bt_free_slot_count() == 0) {
                printf("[HCI] Rejecting connection from %s (all slots occupied)\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_reject_connection_request, addr, 0x0F);
                break;
            }
            if (pairing_window && bt_free_slot_count() <= 1) {
                // While the user is pairing a new controller, reject incoming
                // auto-reconnects that would consume the LAST free slot, so a
                // just-disconnected controller can't page back in and grab the
                // seat reserved for the new pairing. Bonds stay intact; they
                // reconnect once the pairing window closes.
                printf("[HCI] Rejecting connection from %s (pairing window open, last free slot reserved)\n",
                       bd_addr_to_str(addr));
                hci_send_cmd(&hci_reject_connection_request, addr, 0x0F);
                break;
            }
            if ((cod & 0x000F00) == 0x000500) {
                // Don't kill an active pairing inquiry for a routine reconnect;
                // outside the pairing window keep the upstream behavior of
                // stopping inquiry while the connection sets up.
                if (!pairing_window) gap_inquiry_stop();
                hci_send_cmd(&hci_accept_connection_request, addr, 0x01);
                connect_attempt_started = get_absolute_time(); // arm connection watchdog (incoming path)
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            const hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            const uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            bt_slot *s = slot_by_handle(handle);
            if (s) {
                printf("[HCI] Slot %d disconnected\n", slot_index(s));
                slot_clear(s);
                // Neutralize the slot's USB input buffer: with always-FULL
                // enumeration the interface stays visible, and a pad that
                // dropped mid-press (e.g. the PS+Triangle power-off shortcut)
                // must not leave its last buttons frozen "held" on the host.
                bridge_reset_slot_input((uint8_t) slot_index(s));
                if (slot_index(s) == tier_audio_slot()) {
                    state_reset_mute();
                }
#if BT_MAX_SLOTS > 1
                // A different controller may take this seat next; don't let it
                // inherit the previous pad's rumble/trigger/lightbar state.
                // (Single-slot builds keep the upstream behavior: state
                // persists across reconnects of the one controller.)
                state_slot_reset((uint8_t) slot_index(s));
#endif
            }
            if (bt_connected_count() == 0) {
#ifdef ENABLE_WAKE_HID
                // Stay enumerated with the FULL descriptor: the dongle now
                // presents all gamepad interfaces whenever it is plugged in
                // (user preference: connects/disconnects must be seamless, no
                // re-enumeration bounces). Remote wakeup keeps working -- it
                // only needs the device enumerated and suspended. The MINIMAL
                // ghost-hiding variant is retained in usb_descriptors.cpp for
                // a possible future config toggle, but is never requested.
#else
                // Without ENABLE_WAKE_HID we hide the USB device whenever no
                // controller is paired (upstream behavior).
                tud_disconnect();
#endif
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            }
            bt_update_scan_enable();
            device_found = false;
            new_pair = false;
            connect_attempt_started = 0; // disarm pre-ACL watchdog
#if ENABLE_BATT_LED
            if (s && slot_index(s) == BT_USB_SLOT) {
                battery_led_on_disconnect();
            }
#endif
            // An explicit web-UI pair request forces inquiry even with a bond
            // stored (single-slot builds just dropped the active controller to
            // make room); otherwise a bonded controller reconnects via page
            // scan, so we only inquire when nothing is bonded.
            if (pairing_window) {
                // Keep the window open across the disconnect; it closes when the
                // new controller opens HID or the inquiry finds nothing.
                printf("[HCI] Disconnected reason=0x%02X, pair request -> start inquiry\n", reason);
                gap_inquiry_start(30);
            } else if (bt_has_stored_link_key()) {
                printf("[HCI] Disconnected reason=0x%02X, stored controller -> page scan, no inquiry\n", reason);
            } else {
                printf("[HCI] Disconnected reason=0x%02X, no stored controller -> start inquiry\n", reason);
                gap_inquiry_start(30);
            }
            break;
        }

        case GAP_EVENT_RSSI_MEASUREMENT: {
            const hci_con_handle_t handle = gap_event_rssi_measurement_get_con_handle(packet);
            if (bt_slot *s = slot_by_handle(handle)) {
                s->rssi = static_cast<int8_t>(gap_event_rssi_measurement_get_rssi(packet));
            }
            break;
        }
    }
}

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type == L2CAP_DATA_PACKET) {
        bt_slot *s = slot_by_cid(channel);
        if (!s) {
            printf("[L2CAP] Data on unknown channel 0x%04X\n", channel);
            return;
        }
        const uint8_t slot = (uint8_t) slot_index(s);
        if (channel == s->interrupt_cid) {
            // printf("[L2CAP] HID Interrupt data len=%u\n", size);
            // printf_hexdump(packet, size);
            if (bt_data_callback) bt_data_callback(slot, INTERRUPT, packet, size);

            // 静默检测
            // Skip the inactivity watchdog while the controller mic is streaming
            // (packet[2] bit 0 set): mic-active input reports carry Opus frames
            // instead of the idle stick/button pattern this check expects, so the
            // watchdog mis-measures and could disconnect an actively-used headset.
            // (Ported from upstream awalol/DS5Dongle d7fb163.)
            if (!(packet[2] & 1) || get_config().disable_inactive_disconnect) {
                return;
            }
            if (packet[3] < 120 || packet[3] > 140 ||
                packet[4] < 120 || packet[4] > 140 ||
                packet[5] < 120 || packet[5] > 140 ||
                packet[6] < 120 || packet[6] > 140 ||
                packet[7] > 0 || packet[8] > 0 ||
                packet[10] != 0x08 || packet[11] != 0x00 ||
                packet[12] != 0x00) {
                s->inactive_time = get_absolute_time();
            } else if (absolute_time_diff_us(s->inactive_time, get_absolute_time()) >
                       static_cast<int64_t>(get_config().inactive_time) * 60 * 1000 * 1000) {
                printf("disconnect when inactive (slot %d)\n", slot);
                s->inactive_time = get_absolute_time();
                bt_disconnect_slot(s);
            }
        } else if (channel == s->control_cid) {
            if (s->check_dse) {
                if (packet[0] == 0xA3 && packet[1] == 0x70) {
                    printf("Connected DSE Controller (slot %d)\n", slot);
                    s->check_dse = false;
                    s->is_dse = true;
                    s->connect_attempt_started = 0; // fully up — disarm watchdog
                    if (slot == BT_USB_SLOT) {
                        // The USB identity (PID / report descriptor) follows the
                        // USB-exposed slot's model only.
                        is_dse = true;
                        // Unlock Edge profiles; USB connects immediately, profile
                        // reads are gated until the snapshot is prepared.
                        dse_on_connect();
                    }
                    // Wake the host if it's suspended (turn-on-to-wake). No-op
                    // when the host is awake; the variant swap stays deferred
                    // until the wake lands.
                    wake_on_bt_connect();
#ifdef ENABLE_WAKE_HID
                    usb_request_variant_full();
#else
                    tud_connect();
#endif
                } else if (packet[0] == 0x02) {
                    printf("Connected DS5 Controller (slot %d)\n", slot);
                    s->check_dse = false;
                    s->is_dse = false;
                    s->connect_attempt_started = 0; // fully up — disarm watchdog
                    if (slot == BT_USB_SLOT) {
                        is_dse = false;
                    }
                    // Wake the host if it's suspended (turn-on-to-wake). No-op
                    // when the host is awake; the variant swap stays deferred
                    // until the wake lands.
                    wake_on_bt_connect();
#ifdef ENABLE_WAKE_HID
                    usb_request_variant_full();
#else
                    tud_connect();
#endif
                }
            }
            if (packet[0] == 0xA3) {
                uint8_t report_id = packet[1];
                s->feature_data[report_id].assign(packet + 1, packet + size);
#if ENABLE_VERBOSE
                printf("[L2CAP] Stored Feature Report 0x%02X (slot %d), len=%u\n", report_id, slot, size - 1);
#endif
                bt_feature_snapshot_maybe_capture(s);
            }
            // The DSE profile module is bound to the USB-exposed slot; a DSE
            // seated elsewhere works as a gamepad but its profile snapshot
            // machinery isn't driven (profiles aren't reachable over USB
            // anyway until the per-slot interface fan-out).
            if (slot == BT_USB_SLOT) {
                dse_on_control_packet(packet, size);
            }
#if ENABLE_VERBOSE
            printf("[L2CAP] HID Control data len=%u\n", size);
            printf_hexdump(packet, size);
#endif
            if (bt_data_callback) bt_data_callback(slot, CONTROL, packet, size);
        }
        return;
    }

    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            const uint8_t status = l2cap_event_channel_opened_get_status(packet);
            const uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
            const hci_con_handle_t handle = l2cap_event_channel_opened_get_handle(packet);
            bt_slot *s = slot_by_handle(handle);
            if (!s) {
                printf("[L2CAP] Channel opened on unknown handle 0x%04X\n", handle);
                break;
            }
            if (status == 0) {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                if (psm == PSM_HID_CONTROL) {
                    printf("[L2CAP] HID Control opened cid=0x%04X (slot %d)\n", local_cid, slot_index(s));
                    s->control_cid = local_cid;

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(s->control_cid);
                    printf("[L2CAP] Remote Control MTU: %d\n", mtu);
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X (slot %d)\n", local_cid, slot_index(s));
                    s->interrupt_cid = local_cid;
                    // A controller is fully connected -- close any open pairing
                    // window so bonded controllers can auto-reconnect again.
                    pairing_window = false;
                    // Successful pair removes this MAC from the blacklist -- an
                    // explicit PS+Share re-pair is the user un-forgetting it.
                    bt_blacklist_remove(s->addr);

                    if (!get_config().disable_pico_led) {
                        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
                    }
                    s->inactive_time = get_absolute_time();

                    printf("Init DualSense (slot %d)\n", slot_index(s));

                    init_feature((uint8_t) slot_index(s));
                    // 初始化手柄状态 (per-slot state carries the slot's lightbar
                    // color and player indicators on multi-slot builds)
                    bt_send_full_state((uint8_t) slot_index(s));

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(s->interrupt_cid);
                    printf("[L2CAP] Remote Interrupt MTU: %d\n", mtu);

                    // Stay connectable while free slots remain; stop scanning
                    // only when the last seat fills.
                    bt_update_scan_enable();
                    // tud_connect();
                } else {
                    printf("[L2CAP] Unknown Channel psm: 0x%02X", psm);
                }
            } else {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                s->control_cid = 0;
                s->interrupt_cid = 0;
                device_found = false;
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X (slot %d)\n", psm, status, slot_index(s));
                bt_disconnect_slot(s);
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            const uint16_t local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
            const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X\n", psm, local_cid);
            l2cap_accept_connection(local_cid);
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            const uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
            bt_slot *s = slot_by_cid(local_cid);
            if (!s) {
                printf("[L2CAP] Channel closed cid=0x%04X\n", local_cid);
                break;
            }
            if (local_cid == s->control_cid) {
                s->control_cid = 0;
                printf("[L2CAP] HID Control closed cid=0x%04X (slot %d)\n", local_cid, slot_index(s));
            } else if (local_cid == s->interrupt_cid) {
                s->interrupt_cid = 0;
                printf("[L2CAP] HID Interrupt closed cid=0x%04X (slot %d)\n", local_cid, slot_index(s));
            }
            if (s->control_cid == 0 && s->interrupt_cid == 0) {
                bt_disconnect_slot(s);
            }
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            // printf("[L2CAP] L2CAP_EVENT_CAN_SEND_NOW\n");
            const uint16_t local_cid = l2cap_event_can_send_now_get_local_cid(packet);
            bt_slot *s = slot_by_interrupt_cid(local_cid);
            if (!s) {
                break;
            }

            send_element send_packet;
            if (s->retry_pending || queue_try_remove(&s->send_fifo, &send_packet)) {
                if (s->retry_pending) {
                    send_packet = s->retry_packet;
                }
                const uint8_t status = l2cap_send(s->interrupt_cid, send_packet.data, send_packet.len);
                if (status != 0) {
                    s->retry_packet = send_packet;
                    s->retry_pending = true;
                    s->send_chain_active = false;
                    break;
                }
                s->retry_pending = false;
            }
            if (s->retry_pending || !queue_is_empty(&s->send_fifo)) {
                // Self-chain: still have data, keep chain active.
                l2cap_request_can_send_now_event(s->interrupt_cid);
            } else {
                // Chain idle. bt_pump from main loop will kick it again
                // when new data arrives.
                s->send_chain_active = false;
            }
            break;
        }
    }
}

// Accessors used by the DSE profile module (dse.cpp). DSE support is bound to
// the USB-exposed slot (see the control-channel handler above).
uint16_t bt_control_cid() {
    return slots[BT_USB_SLOT].control_cid;
}

void bt_control_send(const uint8_t *data, uint16_t len) {
    if (slots[BT_USB_SLOT].control_cid != 0) {
        l2cap_send(slots[BT_USB_SLOT].control_cid, const_cast<uint8_t *>(data), len);
    }
}

bool bt_feature_cached(uint8_t reportId, vector<uint8_t> &out) {
    auto &cache = slots[BT_USB_SLOT].feature_data;
    auto it = cache.find(reportId);
    if (it == cache.end()) return false;
    out = it->second;
    return true;
}

bool bt_feature_cached_any(uint8_t reportId, vector<uint8_t> &out) {
    for (auto &s : slots) {
        if (s.acl_handle == HCI_CON_HANDLE_INVALID) continue;
        auto it = s.feature_data.find(reportId);
        if (it == s.feature_data.end()) continue;
        out = it->second;
        return true;
    }
    return false;
}

bool bt_feature_snapshot_get(uint8_t reportId, vector<uint8_t> &out) {
    const Config_body &c = get_config();
    if (!c.feature_snapshot_valid) return false;
    switch (reportId) {
        case 0x05: out.assign(c.feature_cal, c.feature_cal + c.feature_cal_len); return true;
        case 0x20: out.assign(c.feature_fw, c.feature_fw + c.feature_fw_len); return true;
        case 0x09: out.assign(c.feature_pair, c.feature_pair + c.feature_pair_len); return true;
        default: return false;
    }
}

// Deferred snapshot persist: set when the first controller's bind-time
// feature reports (0x05/0x20/0x09) have all been cached and no snapshot was
// stored yet. The main loop flushes it (one flash write, once per lifetime).
static bool feature_snapshot_save_pending = false;

static void bt_feature_snapshot_maybe_capture(bt_slot *s) {
    if (get_config().feature_snapshot_valid) return;
    if (feature_snapshot_save_pending) return; // captured, not yet flushed
    auto cal = s->feature_data.find(0x05);
    auto fw  = s->feature_data.find(0x20);
    auto pr  = s->feature_data.find(0x09);
    if (cal == s->feature_data.end() || fw == s->feature_data.end() ||
        pr == s->feature_data.end()) {
        return;
    }
    Config_body c = get_config();
    if (cal->second.size() < 2 || cal->second.size() > sizeof(c.feature_cal) ||
        fw->second.size()  < 2 || fw->second.size()  > sizeof(c.feature_fw) ||
        pr->second.size()  < 2 || pr->second.size()  > sizeof(c.feature_pair)) {
        return;
    }
    c.feature_cal_len = (uint8_t) cal->second.size();
    memcpy(c.feature_cal, cal->second.data(), cal->second.size());
    c.feature_fw_len = (uint8_t) fw->second.size();
    memcpy(c.feature_fw, fw->second.data(), fw->second.size());
    c.feature_pair_len = (uint8_t) pr->second.size();
    memcpy(c.feature_pair, pr->second.data(), pr->second.size());
    c.feature_snapshot_valid = 1;
    set_config(c);
    feature_snapshot_save_pending = true;
    printf("[BT] Feature snapshot captured from slot %d (cal %u, fw %u, pair %u bytes)\n",
           slot_index(s), c.feature_cal_len, c.feature_fw_len, c.feature_pair_len);
#ifdef ENABLE_WAKE_HID
    // The interfaces enumerated before this data existed answered their
    // bind-time probes with stalls; bounce the bus ONCE so the host rebinds
    // them against real data. Only ever happens right after the first
    // pairing on a fresh flash.
    usb_request_rebind();
#endif
}

void bt_feature_snapshot_persist_if_dirty() {
    if (!feature_snapshot_save_pending) return;
    feature_snapshot_save_pending = false;
    const bool ok = config_save();
    printf("[BT] Feature snapshot persist: %s\n", ok ? "OK" : "FAILED");
}

void bt_write(uint8_t slot, const uint8_t *data, const uint16_t len, bool kick) {
    if (slot >= BT_MAX_SLOTS) return;
    bt_slot &s = slots[slot];
    if (s.interrupt_cid == 0) return;
    if (static_cast<size_t>(len) + 1 > BT_SEND_MAX_PACKET_SIZE) {
        printf("[L2CAP bt_write] Error: packet too large: %u\n", len);
        return;
    }
    // Single scratch element: bt_write only runs on the core0 main-loop
    // context, so one staging buffer serves every slot.
    static send_element packet{};
    packet.len = len + 1;
    packet.data[0] = 0xA2;
    memcpy(packet.data + 1, data, len);
    fill_output_report_checksum(packet.data + 1, len);

    if (!queue_try_add(&s.send_fifo, &packet)) {
        if (!kick) {
            return;
        }
        printf("[L2CAP bt_write] Error: Failed to add packet to send FIFO (slot %d)\n", slot);
        return;
    }

    // Kick the can-send-now chain ONLY when called from low-frequency
    // paths (HID reports, command responses). The audio path at ~93 Hz
    // uses kick=false; bt_pump() in the main loop handles chain restart
    // for it. Calling l2cap_request_can_send_now_event from the audio
    // path costs ~535 us/call under PICO_CYW43_ARCH_POLL because it
    // dispatches event handlers (including the actual l2cap_send and
    // SPI transmit) inline.
    if (kick && !s.send_chain_active && queue_get_level(&s.send_fifo) == 1) {
        s.send_chain_active = true;
        l2cap_request_can_send_now_event(s.interrupt_cid);
    }
}

// Called from main loop after cyw43_arch_poll(). Kicks each slot's
// can-send-now chain if there's pending data AND it's not already in flight.
// This moves the ~535 us l2cap_request_can_send_now_event cost off the audio
// thread and onto the main loop, which is already paying that cost in
// cyw43_arch_poll().
//
// Each slot tracks a "chain active" flag so we don't request a second event
// while one is already in flight (would be wasted work). Cleared by the
// L2CAP_EVENT_CAN_SEND_NOW handler; set here (and in bt_write's kick path)
// when we issue a request.
void bt_pump() {
    for (auto &s : slots) {
        if (s.interrupt_cid == 0) continue;
        if (s.send_chain_active) continue;
        if (queue_is_empty(&s.send_fifo) && !s.retry_pending) continue;
        s.send_chain_active = true;
        l2cap_request_can_send_now_event(s.interrupt_cid);
    }
}

bool bt_send_pending() {
    for (auto &s : slots) {
        if (s.interrupt_cid == 0) continue;
        if (s.send_chain_active || !queue_is_empty(&s.send_fifo)) return true;
    }
    return false;
}

vector<uint8_t> get_feature_data(uint8_t slot, uint8_t reportId, uint16_t len) {
    (void) len;
    auto ret = vector<uint8_t>{};
    if (slot >= BT_MAX_SLOTS) return ret;
    bt_slot &s = slots[slot];
    // 若为0x81则会请求新内容，其他若有旧数据则不进行请求
    if (s.feature_data.contains(reportId)) {
        ret = s.feature_data[reportId];
    }
    if (!s.feature_data.contains(reportId) ||
        // Get Test Command Result
        reportId == 0x81 ||
        // DSE: Set Profile Save?
        reportId == 0x63 ||
        reportId == 0x65 ||
        reportId == 0x64 ||
        // DSE profile slots: return cache, but refetch in background so the
        // PS app's unlock(0x80) -> re-read flow sees fresh controller data.
        dse_is_profile_report(reportId)
    ) {
        if (s.control_cid != 0) {
            uint8_t get_feature[] = {0x43, reportId};
            l2cap_send(s.control_cid, get_feature, sizeof(get_feature));
#if ENABLE_VERBOSE
            printf("[L2CAP] Requesting Get Feature Report 0x%02X (slot %d)\n", reportId, slot);
#endif
        }
    }
    return ret;
}

void set_feature_data(uint8_t slot, uint8_t reportId, uint8_t *data, uint16_t len) {
    if (slot >= BT_MAX_SLOTS) return;
    bt_slot &s = slots[slot];
    if (s.control_cid == 0) return;
    // Largest legitimate DS/DSE feature report payload is ~63 bytes; +2 for
    // SET_REPORT header. Cap at 128 to keep this off the stack as a VLA and
    // prevent a too-large len (or attacker-influenced value) from smashing
    // the stack with no diagnostic.
    constexpr size_t SET_FEATURE_MAX = 128;
    if (static_cast<size_t>(len) + 2 > SET_FEATURE_MAX) {
        printf("[L2CAP] set_feature_data len=%u too large, dropped\n", len);
        return;
    }
    uint8_t get_feature[SET_FEATURE_MAX];
    get_feature[0] = 0x53;
    get_feature[1] = reportId;
    memcpy(get_feature + 2, data, len);
    fill_feature_report_checksum(get_feature + 1, len + 1);
    l2cap_send(s.control_cid, get_feature, len + 2);
#if ENABLE_VERBOSE
    printf("[L2CAP] Requesting Set Feature Report 0x%02X (slot %d)\n", reportId, slot);
    printf_hexdump(get_feature, len + 2);
#endif
    if (slot == BT_USB_SLOT) {
        dse_on_profile_write(reportId);
    }
}

void init_feature(uint8_t slot) {
    if (slot >= BT_MAX_SLOTS) return;
    get_feature_data(slot, 0x09, 20);
    get_feature_data(slot, 0x20, 64);
    get_feature_data(slot, 0x22, 64);
    get_feature_data(slot, 0x05, 41);
    // DSE
    // check DSE by request 0x70 feature report. DSE return DEFAULT
    // If len == 1, it's DS5
    slots[slot].check_dse = true;
    get_feature_data(slot, 0x70, 64);
}

void bt_slot_power_off(uint8_t slot) {
    // DualSense feature report 0x08 ("Set USB Settings 1") accepts a
    // sub-command at byte 0; sub-command 0x02 is power-off, equivalent to
    // a long PS-button hold. Remaining bytes are settings fields we leave
    // as zero (= "no change") because the controller is about to power down.
    // Report 0x08's payload is 47 bytes per the HID report descriptor; the
    // last 4 bytes get overwritten with the CRC32 inside set_feature_data().
    constexpr uint8_t REPORT_ID_SET_USB_SETTINGS_1 = 0x08;
    constexpr uint8_t SUBCMD_POWER_OFF             = 0x02;
    constexpr size_t  REPORT_08_PAYLOAD_LEN        = 47;

    if (slot >= BT_MAX_SLOTS) return;
    if (slots[slot].control_cid == 0) return; // empty slot, nothing to do
    uint8_t payload[REPORT_08_PAYLOAD_LEN] = {0};
    payload[0] = SUBCMD_POWER_OFF;
    set_feature_data(slot, REPORT_ID_SET_USB_SETTINGS_1, payload, sizeof(payload));
}

void bt_dualsense_power_off() {
    for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
        bt_slot_power_off(slot);
    }
}
