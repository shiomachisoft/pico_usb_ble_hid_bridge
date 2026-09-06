#ifndef USB_HID_HOST_H_
#define USB_HID_HOST_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Periodically checks if the multi-device aggregation wait window has
 * elapsed, and merges all mounted HID report descriptors to start the BLE bridge.
 */
void usb_hid_host_task(void);

#ifdef __cplusplus
}
#endif

#endif // USB_HID_HOST_H_
