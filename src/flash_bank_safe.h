//
// BTstack HAL flash bank with a core1-safe, watchdog-safe write path.
// Replaces the SDK's pico_flash_bank_instance() (whose erase/program block
// on flash_safe_execute with an unbounded timeout -- a hang and watchdog
// reboot whenever core1 sleeps through the flash lockout). See
// flash_bank_safe.cpp for details; bt_init() re-runs the TLV setup with this
// instance so link keys (bonds) persist without risking the main loop.
//

#ifndef DS5_BRIDGE_FLASH_BANK_SAFE_H
#define DS5_BRIDGE_FLASH_BANK_SAFE_H

#include "hal_flash_bank.h"

#ifdef __cplusplus
extern "C" {
#endif

const hal_flash_bank_t *flash_bank_safe_instance(void);

#ifdef __cplusplus
}
#endif

#endif // DS5_BRIDGE_FLASH_BANK_SAFE_H
