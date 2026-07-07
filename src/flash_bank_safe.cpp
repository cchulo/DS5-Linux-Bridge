//
// BTstack HAL flash bank with a core1-safe, watchdog-safe write path.
//
// The pico-sdk's pico_flash_bank_instance() runs its erase/program through
// flash_safe_execute(..., UINT32_MAX): it blocks until core1 honors the
// flash lockout. With PICO_FLASH_ASSUME_CORE1_SAFE=0 (required here so flash
// ops can't race the audio core), core1 asleep in __wfe() can miss the
// lockout request -- so a link-key store at controller-connect time hung the
// main loop until the 1 s watchdog rebooted the dongle. Symptoms: a
// boot loop on pad connect, and bonds that never persisted (every reconnect
// silently re-paired, hiding the loss).
//
// This drop-in replacement applies the same treatment config_save() uses:
// feed the watchdog, nudge core1 awake with __sev(), bound each attempt,
// and retry. A persistent failure loses that one write (BTstack re-stores
// the key on the next pairing) instead of rebooting the dongle.
//
// Bank layout, sizes and the page-merge write logic mirror pico-sdk's
// btstack_flash_bank.c (BSD-3-Clause, Copyright (c) 2023 Raspberry Pi
// (Trading) Ltd).
//

#include "flash_bank_safe.h"

#include <cstdio>
#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/btstack_flash_bank.h" // PICO_FLASH_BANK_* layout macros
#include "pico/flash.h"

namespace {

constexpr uint32_t BANK_SIZE = PICO_FLASH_BANK_TOTAL_SIZE / 2;
// Bounded so worst case (retries * timeout) stays visible next to the 1 s
// watchdog; the watchdog is fed before every attempt.
constexpr uint32_t OP_TIMEOUT_MS = 250;
constexpr int OP_RETRIES = 3;

struct mutation_op {
    bool op_is_erase;
    uint32_t flash_offset;
    const uint8_t *data; // page buffer for program; unused for erase
};

void do_mutation(void *param) {
    const mutation_op *op = (const mutation_op *) param;
    if (op->op_is_erase) {
        flash_range_erase(op->flash_offset, BANK_SIZE);
    } else {
        flash_range_program(op->flash_offset, op->data, FLASH_PAGE_SIZE);
    }
}

// The core1-safe wrapper: watchdog-fed, bounded, retried.
void run_mutation(mutation_op *op) {
    for (int attempt = 0; attempt < OP_RETRIES; attempt++) {
        watchdog_update();
        __sev(); // wake core1 out of __wfe() so its flash-safe IRQ can run
        const int rc = flash_safe_execute(do_mutation, op, OP_TIMEOUT_MS);
        watchdog_update();
        if (rc == PICO_OK) return;
        if (rc == PICO_ERROR_NOT_PERMITTED) {
            // Core1 hasn't been launched (and registered as a lockout victim)
            // yet -- this is the TLV bank formatting itself during bt_init(),
            // before audio_init() starts core1. With only one core running a
            // direct flash op is inherently safe; flash_safe_execute just
            // can't know that. This was THE reason bonds never persisted:
            // the SDK's bank hit the same error at every boot and silently
            // ignored it, leaving the bank unformatted forever.
            const uint32_t ints = save_and_disable_interrupts();
            do_mutation(op);
            restore_interrupts(ints);
            watchdog_update();
            printf("[FLASHBANK] %s done directly (single-core boot phase)\n",
                   op->op_is_erase ? "erase" : "program");
            return;
        }
        printf("[FLASHBANK] %s failed (attempt %d/%d): %d\n",
               op->op_is_erase ? "erase" : "program", attempt + 1, OP_RETRIES, rc);
    }
    printf("[FLASHBANK] flash op FAILED; bond data not persisted this time\n");
}

uint32_t bank_start(int bank) {
    return PICO_FLASH_BANK_STORAGE_OFFSET + BANK_SIZE * (uint32_t) bank;
}

uint32_t bank_get_size(void *) { return BANK_SIZE; }
uint32_t bank_get_alignment(void *) { return 1; }

void bank_erase(void *, int bank) {
    if (bank > 1) return;
    mutation_op op{true, bank_start(bank), nullptr};
    run_mutation(&op);
}

void bank_read(void *, int bank, uint32_t offset, uint8_t *buffer, uint32_t size) {
    if (bank > 1) return;
    if (offset >= BANK_SIZE || (offset + size) > BANK_SIZE) return;
    memcpy(buffer, (const void *) (XIP_BASE + bank_start(bank) + offset), size);
}

void bank_write(void *, int bank, uint32_t offset, const uint8_t *data, uint32_t size) {
    if (bank > 1) return;
    if (offset >= BANK_SIZE || (offset + size) > BANK_SIZE || size == 0) return;

    const uint32_t start = bank_start(bank);
    const uint32_t first_page = offset / FLASH_PAGE_SIZE;
    const uint32_t last_page = (offset + size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    uint32_t page_off = offset % FLASH_PAGE_SIZE;
    uint32_t data_pos = 0;
    uint32_t size_left = size;

    // Program whole pages, merging the new bytes over what the page holds
    // (flash can only clear bits; TLV appends into erased 0xFF space).
    for (uint32_t page = first_page; page < last_page; page++) {
        uint8_t page_data[FLASH_PAGE_SIZE];

        if (page == first_page && page_off > 0) {
            memcpy(page_data,
                   (const void *) (XIP_BASE + start + page * FLASH_PAGE_SIZE),
                   page_off);
        }
        if (page == last_page - 1 && (page_off + size_left) < FLASH_PAGE_SIZE) {
            memcpy(page_data + page_off + size_left,
                   (const void *) (XIP_BASE + start + page * FLASH_PAGE_SIZE +
                                   page_off + size_left),
                   FLASH_PAGE_SIZE - page_off - size_left);
        }

        const uint32_t chunk =
            size_left < (FLASH_PAGE_SIZE - page_off) ? size_left
                                                     : (FLASH_PAGE_SIZE - page_off);
        memcpy(page_data + page_off, data + data_pos, chunk);
        data_pos += chunk;
        size_left -= chunk;
        page_off = 0;

        mutation_op op{false, start + page * FLASH_PAGE_SIZE, page_data};
        run_mutation(&op);
    }
}

const hal_flash_bank_t flash_bank_safe_obj = {
    &bank_get_size,
    &bank_get_alignment,
    &bank_erase,
    &bank_read,
    &bank_write,
};

} // namespace

const hal_flash_bank_t *flash_bank_safe_instance(void) {
    return &flash_bank_safe_obj;
}
