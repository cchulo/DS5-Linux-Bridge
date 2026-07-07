//
// Created by awalol on 2026/5/4.
//

#include "config.h"

#include <cmath>
#include <cstddef> // offsetof
#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h" // flash_safe_execute(): park core1 during the flash op
#include "pico/btstack_flash_bank.h" // PICO_FLASH_BANK_STORAGE_OFFSET (collision guard)
#include "usb_net.h" // WEBCONFIG_SUBNET_COUNT (subnet-index bound, always defined)
#include "utils.h"

constexpr uint32_t CONFIG_MAGIC = 0x66ccff00;
// Layout version. Only bump on a genuinely incompatible layout change (see the
// append-only note in config.h); NOT a reset trigger. v5 added
// webconfig_custom_ip; v4 bond_names.
constexpr uint16_t CONFIG_VERSION = 5;
// Config lives just BELOW BTstack's link-key bank, NOT in the last flash sector.
// The RP2350 BOOTSEL/picotool UF2 loader erases the top of flash (the last
// sector) on download -- even though the UF2 image ends far below it -- so a
// config kept there is wiped on every reflash while it survives plain reboots.
// BTstack's 2-sector TLV bank is relocated for the same reason (the SDK
// default put its second sector in the bootrom-erased last sector, wiping
// bonds on every other reflash): CMakeLists defines
// PICO_FLASH_BANK_STORAGE_OFFSET = FLASH - 3*sector, so the bank occupies
// sectors -3,-2 and we sit one sector under it at FLASH - 4*sector, still
// clear of the image (which ends < 1 MB).
//   Layout (4 MB build): image .. | cfg(-4) | btstack(-3,-2) | bootrom-erased(-1)
constexpr uint32_t CONFIG_FLASH_OFFSET =
    PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE;
// Bytes of the reserved sector that config actually occupies. Must be a
// multiple of FLASH_PAGE_SIZE (the save programs it page-by-page) and fit in
// one erase sector. 512 leaves comfortable headroom for future append-only
// growth (the whole struct is well under this today) without ballooning the
// on-stack save buffer.
constexpr size_t CONFIG_STORE_SIZE = 512;
// flash_safe_execute() timeout per attempt, and how many times we retry when
// core1 (audio) fails to park in time. Total worst-case block ~= product of the
// two; kept modest so a wedged core1 can't stall the web request indefinitely.
constexpr uint32_t CONFIG_SAVE_TIMEOUT_MS = 1000;
constexpr int CONFIG_SAVE_RETRIES = 3;
static Config config{};
bool is_dse = false;

// The whole Config (header + body) must fit in the reserved store, which in
// turn must page-align and fit the erase sector.
static_assert(sizeof(Config) <= CONFIG_STORE_SIZE);
static_assert(CONFIG_STORE_SIZE % FLASH_PAGE_SIZE == 0);
static_assert(CONFIG_STORE_SIZE <= FLASH_SECTOR_SIZE);
// Config region must start on a flash sector boundary.
static_assert(CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);
// Must not collide with BTstack's link-key bank that sits just above us, and
// must not itself land in the last (bootrom-erased) sector.
static_assert(CONFIG_FLASH_OFFSET + FLASH_SECTOR_SIZE <= PICO_FLASH_BANK_STORAGE_OFFSET);
static_assert(CONFIG_FLASH_OFFSET < PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE);

// Append-only enforcement: pin the offset of every field that has ever shipped.
// Adding a field at the END leaves these untouched; inserting/reordering one in
// the middle shifts a later offset and fails the build (see config.h note).
static_assert(offsetof(Config_body, config_version) == 0);
static_assert(offsetof(Config_body, speaker_volume) == 1);
static_assert(offsetof(Config_body, inactive_time) == 5);
static_assert(offsetof(Config_body, disable_inactive_disconnect) == 6);
static_assert(offsetof(Config_body, disable_pico_led) == 7);
static_assert(offsetof(Config_body, polling_rate_mode) == 8);
static_assert(offsetof(Config_body, audio_buffer_length) == 9);
static_assert(offsetof(Config_body, controller_mode) == 10);
static_assert(offsetof(Config_body, webconfig_subnet) == 11);
static_assert(offsetof(Config_body, webconfig_custom_ip) == 12);
static_assert(offsetof(Config_body, bond_names) == 16);
static_assert(offsetof(Config_body, audio_slot) == 104);
static_assert(offsetof(Config_body, feature_snapshot_valid) == 105);
static_assert(offsetof(Config_body, slot_rgb) == 261);
static_assert(offsetof(Config_body, led_count) == 273);
static_assert(offsetof(Config_body, disable_player_led_lock) == 291);
static_assert(offsetof(Config_body, pairing_led_mask) == 292);
static_assert(sizeof(Config_body) <= 320); // keep well inside the 512 B store

// CRC over the first `len` bytes of the body. `len` is the stored size, so an
// older/shorter blob still validates against the bytes it actually wrote.
static uint32_t calc_config_crc(const Config &con, size_t len) {
  return crc32(reinterpret_cast<const uint8_t *>(&con.body), len);
}

const Config *flash_config() {
  return reinterpret_cast<const Config *>(XIP_BASE + CONFIG_FLASH_OFFSET);
}

void config_valid() {
  // Clamp/default every body field to a sane value. The header (magic/version/
  // size) is set by config_load()/config_save(), not here -- migration already
  // ran by the time we reach this point, so a stale header is not "invalid".
  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;
  config.size = sizeof(Config_body);
  auto body = &config.body;
  if (std::isnan(body->speaker_volume) || body->speaker_volume < -100 ||
      body->speaker_volume > 0) {
    body->speaker_volume = -100;
    printf("[Config] Speaker Volume is invalid\n");
  }
  if (body->inactive_time < 5 || body->inactive_time > 60) {
    body->inactive_time = 30;
    printf("[Config] Inactive time is invalid\n");
  }
  if (body->disable_inactive_disconnect > 1) {
    body->disable_inactive_disconnect = 0;
    printf("[Config] disable_auto_disconnect is invalid\n");
  }
  if (body->disable_pico_led > 1) {
    body->disable_pico_led = 0;
    printf("[Config] disable_pico_led is invalid\n");
  }
  if (body->polling_rate_mode > 2) {
    body->polling_rate_mode = 2;
    printf("[Config] polling_rate_mode is invalid\n");
  }
  if (body->audio_buffer_length < 16 || body->audio_buffer_length > 128) {
    body->audio_buffer_length = 64;
    printf("[Config] haptics_buffer_length is invalid\n");
  }
  if (body->controller_mode > 2) {
    body->controller_mode = 2;
    printf("[Config] controller_mode is invalid\n");
  }
  if (body->webconfig_subnet > WEBCONFIG_SUBNET_MAX) {
    body->webconfig_subnet = 0; // default: 10.55.55.x
    printf("[Config] webconfig_subnet is invalid\n");
  }
  // If "custom IP" is selected, the stored address must be a valid private host
  // address; otherwise fall back to the default preset so the page stays
  // reachable (this is the safety net behind the custom-IP YOLO option).
  if (body->webconfig_subnet == WEBCONFIG_SUBNET_CUSTOM &&
      !webconfig_ip_is_valid(body->webconfig_custom_ip)) {
    body->webconfig_subnet = 0;
    printf("[Config] webconfig_custom_ip invalid; using default preset\n");
  }
  // Feature snapshot: sanity-check the stored lengths; anything out of range
  // invalidates the snapshot (it re-captures from the next controller).
  if (body->feature_snapshot_valid) {
    if (body->feature_cal_len < 2 || body->feature_cal_len > sizeof(body->feature_cal) ||
        body->feature_fw_len < 2 || body->feature_fw_len > sizeof(body->feature_fw) ||
        body->feature_pair_len < 2 || body->feature_pair_len > sizeof(body->feature_pair)) {
      body->feature_snapshot_valid = 0;
      printf("[Config] feature snapshot invalid, dropped\n");
    }
  }
  // Slot colors: all-zero is "unset" (fresh defaults or a config migrated
  // from firmware without this field) -> default blue #0000FF. A deliberate
  // black lightbar isn't representable, which is fine: "off" isn't a slot
  // identity.
  for (auto &rgb : body->slot_rgb) {
    if (rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0) {
      rgb[2] = 0xff;
    }
  }
  // LED strip layout: 0 = unset (migrated config) -> 8-pixel default.
  if (body->led_count < 1 || body->led_count > LED_STRIP_MAX_PIXELS) {
    body->led_count = 8;
  }
  if (body->led_map_valid > 1) {
    body->led_map_valid = 0;
  }
  if (!body->led_map_valid) {
    // Classic alternating default: slot k lights pixel 2k+1, spacers dark;
    // pairing mode blinks the spacers (pixels 0,2,4,6).
    for (int s = 0; s < 4; s++) {
      body->slot_led_mask[s] = 1u << (s * 2 + 1);
    }
    body->pairing_led_mask = 0x55;
  }
  if (body->disable_player_led_lock > 1) {
    body->disable_player_led_lock = 0;
    printf("[Config] disable_player_led_lock is invalid\n");
  }
  // Legacy in-body version byte, kept in sync with the header for compatibility
  // with older firmware that read it. Not authoritative; the header version is.
  body->config_version = CONFIG_VERSION;
  // Defensive: guarantee every bond name is NUL-terminated so corrupt flash
  // can never yield an unbounded C string when the web UI reads it.
  for (auto &b : body->bond_names) {
    b.name[CONFIG_BOND_NAME_LEN - 1] = '\0';
  }
}

// Reset the in-RAM config to all defaults (does NOT touch flash). Most fields
// get their default from config_valid()'s range check on the zeroed body, but
// fields whose 0 value is itself valid must be defaulted explicitly here (a
// range check can't distinguish "unset" from "user picked 0"). Used for
// uninitialized/corrupt flash and by config_factory_reset().
void config_default() {
  memset(&config, 0, sizeof(config));
  // 0 is a valid selection for these, so config_valid() would leave the zeroed
  // body as DS5 / 250 Hz. Default to the preferred out-of-box behavior instead.
  config.body.controller_mode = 2;   // Auto (0: DS5, 1: DSE, 2: Auto)
  config.body.polling_rate_mode = 2; // Real-time / 1000 Hz (0: 250, 1: 500, 2: RT)
  config_valid();
}

void config_load() {
  // The header is at a fixed offset and always safe to read; the body may be
  // shorter (older firmware) or longer (newer) than ours. Read the raw stored
  // header first to decide how to migrate.
  const Config *stored = flash_config();

  // Uninitialized or foreign flash -> start from defaults. (Erased flash reads
  // 0xFF, so magic won't match; a genuinely different magic can't be trusted.)
  if (stored->magic != CONFIG_MAGIC) {
    printf("[Config] no valid config in flash (magic=0x%08lx) -> defaults\n",
           (unsigned long) stored->magic);
    config_default();
    return;
  }

  // `stored->size` is how many body bytes were valid when it was saved. A newer
  // firmware may have written MORE than we know about, an older one FEWER. Bound
  // it to the body region that actually fits the reserved store so a corrupt
  // size can't make us read past the sector.
  constexpr size_t MAX_STORED_BODY = CONFIG_STORE_SIZE - sizeof(Config) +
                                     sizeof(Config_body); // body bytes that fit
  size_t stored_body = stored->size;
  if (stored_body > MAX_STORED_BODY) {
    printf("[Config] stored size %u exceeds store; treating as corrupt -> defaults\n",
           (unsigned) stored_body);
    config_default();
    return;
  }

  // Validate the CRC over exactly the bytes the writer covered (which may be
  // more than sizeof(Config_body) if it came from newer firmware). A mismatch
  // means corruption (or a pre-CRC blob) -> fall back to defaults.
  const uint32_t stored_crc =
      crc32(reinterpret_cast<const uint8_t *>(&stored->body), stored_body);
  if (stored_crc != stored->crc32) {
    printf("[Config] config CRC mismatch (size=%u) -> defaults\n",
           (unsigned) stored_body);
    config_default();
    return;
  }

  // CRC good: adopt the overlapping body bytes; default-init the tail we have
  // that the writer didn't (older blob), or ignore the extra bytes it had that
  // we don't understand (newer blob).
  const size_t copy_body =
      stored_body < sizeof(Config_body) ? stored_body : sizeof(Config_body);
  memcpy(&config, stored, sizeof(uint32_t) + sizeof(uint16_t) +
                              sizeof(uint32_t) + sizeof(uint16_t)); // header
  memcpy(&config.body, &stored->body, copy_body);
  if (copy_body < sizeof(Config_body)) {
    memset(reinterpret_cast<uint8_t *>(&config.body) + copy_body, 0,
           sizeof(Config_body) - copy_body);
    printf("[Config] migrated older config (%u -> %u body bytes)\n",
           (unsigned) copy_body, (unsigned) sizeof(Config_body));
  } else if (stored_body > sizeof(Config_body)) {
    printf("[Config] config from newer firmware (%u body bytes); using first %u\n",
           (unsigned) stored_body, (unsigned) sizeof(Config_body));
  }
  config_valid();
}

// Runs with core1 parked (flash_safe_execute) and core0 interrupts disabled, so
// neither core touches XIP flash while the sector is erased/programmed. Without
// the core1 park this races the audio core and corrupts audio (buzzing) -- or
// faults -- when saving during playback (e.g. bond rename while connected).
static void config_save_flash_op(void *param) {
  const uint8_t *store = static_cast<const uint8_t *>(param);
  const uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  // Program the whole reserved store (multiple pages) in one call; the SDK
  // splits it into page-sized writes internally.
  flash_range_program(CONFIG_FLASH_OFFSET, store, CONFIG_STORE_SIZE);
  restore_interrupts(interrupts);
}

bool config_save() {
  // Stamp the header so a future load can migrate this blob by size and verify
  // it by CRC over exactly the bytes we write.
  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;
  config.size = sizeof(Config_body);
  config.crc32 = calc_config_crc(config, sizeof(Config_body));
  alignas(4) uint8_t page[CONFIG_STORE_SIZE];
  memset(page, 0xff, sizeof(page));
  memcpy(page, &config, sizeof(Config));

  // flash_safe_execute() parks core1 (the audio core) before touching flash. If
  // core1 is asleep in __wfe() (idle audio loop) it can miss the lockout request
  // and the call returns PICO_ERROR_TIMEOUT -- the erase/program never happens.
  // Historically the single failure was ignored by callers, so the web UI would
  // report "saved" (RAM was updated) while flash kept the old bytes; the change
  // then vanished on the next boot. Retry a few times, nudging core1 awake with
  // __sev() before each attempt so its flash-safe IRQ handler can run.
  int rc = PICO_ERROR_TIMEOUT;
  for (int attempt = 0; attempt < CONFIG_SAVE_RETRIES; attempt++) {
    __sev(); // wake core1 out of __wfe() so it can honour the flash-safe lockout
    rc = flash_safe_execute(config_save_flash_op, page, CONFIG_SAVE_TIMEOUT_MS);
    if (rc == PICO_OK) break;
    printf("[Config] config_save flash_safe_execute failed (attempt %d/%d): %d\n",
           attempt + 1, CONFIG_SAVE_RETRIES, rc);
  }
  if (rc != PICO_OK) {
    printf("[Config] config_save FAILED after %d attempts: %d (config NOT persisted)\n",
           CONFIG_SAVE_RETRIES, rc);
    return false;
  }

  Config verify{};
  memcpy(&verify, flash_config(), sizeof(verify));
  const auto verify_crc32 = calc_config_crc(verify, sizeof(Config_body));
  if (verify_crc32 == config.crc32) {
    printf("[Config] Config write flash verify success\n");
    return true;
  }
  printf("[Config] Config write flash verify FAILED (config NOT persisted)\n");
  return false;
}

bool config_factory_reset() {
  // Preserve the feature snapshot across the reset: it is captured
  // controller data (calibration / firmware-info / pairing blobs used to
  // answer bind-time probes for empty slots), not a user setting. Wiping it
  // forced a re-capture on the next pad connect, whose one-time
  // usb_request_rebind() bounced the whole USB device off the bus --
  // gamepads AND the config-page network link -- for no benefit: the host
  // already enumerated against real data.
  const uint8_t snap_valid = config.body.feature_snapshot_valid;
  uint8_t cal_len = config.body.feature_cal_len;
  uint8_t fw_len = config.body.feature_fw_len;
  uint8_t pair_len = config.body.feature_pair_len;
  uint8_t cal[sizeof(config.body.feature_cal)];
  uint8_t fw[sizeof(config.body.feature_fw)];
  uint8_t pair[sizeof(config.body.feature_pair)];
  memcpy(cal, config.body.feature_cal, sizeof(cal));
  memcpy(fw, config.body.feature_fw, sizeof(fw));
  memcpy(pair, config.body.feature_pair, sizeof(pair));

  config_default();

  config.body.feature_snapshot_valid = snap_valid;
  config.body.feature_cal_len = cal_len;
  config.body.feature_fw_len = fw_len;
  config.body.feature_pair_len = pair_len;
  memcpy(config.body.feature_cal, cal, sizeof(cal));
  memcpy(config.body.feature_fw, fw, sizeof(fw));
  memcpy(config.body.feature_pair, pair, sizeof(pair));
  config_valid(); // re-check the restored snapshot lengths

  return config_save();
}

const Config_body &get_config() { return config.body; }

void set_config(const uint8_t *new_config, const uint16_t len) {
  const auto copy_len = len < sizeof(Config_body) ? len : sizeof(Config_body);
  memcpy(&config.body, new_config, copy_len);
  config_valid();
  if (config.body.disable_pico_led) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
  } else {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
  }
}

//--------------------------------------------------------------------+
// Bond-name table helpers. An all-zero addr marks an empty slot.
//--------------------------------------------------------------------+

static bool addr_is_zero(const uint8_t *a) {
  for (int i = 0; i < CONFIG_BOND_ADDR_LEN; i++)
    if (a[i]) return false;
  return true;
}

static bool addr_eq(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, CONFIG_BOND_ADDR_LEN) == 0;
}

const char *config_bond_name(const uint8_t *addr) {
  if (!addr) return nullptr;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) return b.name;
  }
  return nullptr;
}

void config_clear_bond_name(const uint8_t *addr) {
  if (!addr) return;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) {
      memset(&b, 0, sizeof(b));
    }
  }
}

bool config_set_bond_name(const uint8_t *addr, const char *name) {
  if (!addr || addr_is_zero(addr)) return false;
  if (!name) name = "";

  // Blank/whitespace-only name clears the slot instead of storing it.
  bool blank = true;
  for (const char *p = name; *p; p++)
    if (*p != ' ' && *p != '\t') { blank = false; break; }
  if (blank) {
    config_clear_bond_name(addr);
    return true;
  }

  BondName *slot = nullptr;
  for (auto &b : config.body.bond_names) {
    if (!addr_is_zero(b.addr) && addr_eq(b.addr, addr)) { slot = &b; break; }
  }
  if (!slot) {
    for (auto &b : config.body.bond_names) {
      if (addr_is_zero(b.addr)) { slot = &b; break; }
    }
  }
  if (!slot) return false; // table full

  memcpy(slot->addr, addr, CONFIG_BOND_ADDR_LEN);
  strncpy(slot->name, name, CONFIG_BOND_NAME_LEN - 1);
  slot->name[CONFIG_BOND_NAME_LEN - 1] = '\0';
  return true;
}

void set_config(const Config_body &new_config) {
  config.body = new_config;
  config_valid();
  // Apply the live-effective LED state immediately (mirrors the uint8_t*
  // overload). Other live fields (inactive_time, audio_buffer_length, ...) are
  // re-read on their own cadence; descriptor-bound fields (controller_mode,
  // polling_rate_mode) only take effect on the next USB re-enumeration.
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !config.body.disable_pico_led);
}
