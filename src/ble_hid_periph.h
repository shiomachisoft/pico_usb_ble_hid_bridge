#ifndef BLE_HID_PERIPH_H_
#define BLE_HID_PERIPH_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Flag indicating if the current report descriptor utilizes Report IDs
extern bool use_report_ids;

/**
 * @brief Initializes non-volatile memory (Flash TLV) for storing pairing keys.
 * Ensures the device remains bonded across reboots.
 */
void ble_hid_periph_init_flash(void);

/**
 * @brief Starts the BLE HID peripheral, parses the USB descriptor, and prepares
 * the services. Schedules HIDS setup to run on Core 1.
 *
 * @param desc_report Pointer to the USB device descriptor buffer.
 * @param desc_len Length of the report descriptor buffer.
 */
void ble_hid_periph_start(uint8_t const *desc_report, uint16_t desc_len);

/**
 * @brief Safely enqueues a raw HID report from USB to send it over BLE.
 * Dequeues old reports if the queue overflows. Schedules a send callback.
 *
 * @param report Pointer to the raw report data.
 * @param len Length of the report data.
 */
void ble_hid_periph_send_raw_report(uint8_t const *report, uint16_t len);

/**
 * @brief Checks if a BLE host is currently connected and the link is encrypted.
 *
 * @return true if connected and encrypted, false otherwise.
 */
bool ble_hid_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_HID_PERIPH_H_
