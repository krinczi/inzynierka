#ifndef INC_OTA_LAYOUT_H_
#define INC_OTA_LAYOUT_H_

#include <stdint.h>

/*
 * STM32H753 dual-bank OTA layout. Every erase sector is 128 KiB.
 *
 * Bank 1:
 *   sector 0       immutable bootloader
 *   sectors 1..4   primary application slot
 *   sector 5       persistent event journal
 *   sectors 6..7   credential store
 *
 * Bank 2:
 *   sector 0       append-only OTA control log
 *   sectors 1..4   secondary application slot
 *   sector 5       swap scratch sector
 *   sectors 6..7   device configuration
 */
#define OTA_FLASH_BASE                 0x08000000UL
#define OTA_FLASH_BANK_SIZE            (1024UL * 1024UL)
#define OTA_FLASH_SECTOR_SIZE          (128UL * 1024UL)
#define OTA_FLASHWORD_SIZE             32UL

#define OTA_BOOTLOADER_ADDRESS         0x08000000UL
#define OTA_BOOTLOADER_SIZE            OTA_FLASH_SECTOR_SIZE

#define OTA_PRIMARY_SLOT_ADDRESS       0x08020000UL
#define OTA_SECONDARY_SLOT_ADDRESS     0x08120000UL
#define OTA_SLOT_SIZE                  (512UL * 1024UL)
#define OTA_SLOT_SECTOR_COUNT          4UL
#define OTA_IMAGE_HEADER_SIZE          1024UL
#define OTA_PRIMARY_IMAGE_ADDRESS      \
    (OTA_PRIMARY_SLOT_ADDRESS + OTA_IMAGE_HEADER_SIZE)
#define OTA_SECONDARY_IMAGE_ADDRESS    \
    (OTA_SECONDARY_SLOT_ADDRESS + OTA_IMAGE_HEADER_SIZE)
#define OTA_IMAGE_MAX_PAYLOAD_SIZE     \
    (OTA_SLOT_SIZE - OTA_IMAGE_HEADER_SIZE)

#define OTA_CONTROL_LOG_ADDRESS        0x08100000UL
#define OTA_CONTROL_LOG_SIZE           OTA_FLASH_SECTOR_SIZE
#define OTA_SCRATCH_ADDRESS            0x081A0000UL
#define OTA_SCRATCH_SIZE               OTA_FLASH_SECTOR_SIZE

#define OTA_EVENT_JOURNAL_ADDRESS      0x080A0000UL
#define OTA_CREDENTIAL_A_ADDRESS       0x080C0000UL
#define OTA_CREDENTIAL_B_ADDRESS       0x080E0000UL
#define OTA_CONFIG_A_ADDRESS           0x081C0000UL
#define OTA_CONFIG_B_ADDRESS           0x081E0000UL

#define OTA_PRIMARY_FIRST_SECTOR       1UL
#define OTA_SECONDARY_FIRST_SECTOR     1UL
#define OTA_CONTROL_LOG_SECTOR         0UL
#define OTA_SCRATCH_SECTOR             5UL

#define OTA_TRIAL_MAX_BOOT_ATTEMPTS    3UL
#define OTA_HEALTH_TIMEOUT_MS          120000UL

#endif /* INC_OTA_LAYOUT_H_ */
