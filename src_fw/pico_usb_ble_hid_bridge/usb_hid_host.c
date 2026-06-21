#include "bsp/board_api.h"
#include "debug_print.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------+
// Macro Definitions
//--------------------------------------------------------------------+
// Maximum length of the saved HID report descriptor (in bytes)
#define SAVED_DESC_REPORT_MAX_LEN 2048

// USB control request type: Host-to-device, Class, Interface (0x21)
#define USB_REQ_TYPE_CLASS_INTERFACE 0x21

//--------------------------------------------------------------------+
// Global & Static Variables
//--------------------------------------------------------------------+
// State tracking for the bridged instance to support composite devices
static bool device_bridged =
    false; // Flag indicating if a USB HID device is currently bridged to BLE
static uint8_t bridged_dev_addr =
    0; // USB device address of the active bridged device
static uint8_t bridged_instance =
    0; // USB interface instance of the active bridged device

static uint8_t
    saved_desc_report[SAVED_DESC_REPORT_MAX_LEN]; // Buffer holding the saved
                                                  // HID report descriptor of
                                                  // the bridged device
static uint16_t saved_desc_len = 0; // Length of the saved HID report descriptor

#ifndef CFG_TUH_HID
#define CFG_TUH_HID 4
#endif

// Workaround: Custom SET_IDLE Control Transfer state
// Since tuh_hid_set_idle() does not exist in the current TinyUSB version,
// we manually send the SET_IDLE command using a generic control transfer.
// We use arrays indexed by the HID interface instance to prevent race
// conditions on composite devices where multiple interfaces are mounted
// simultaneously.
static tusb_control_request_t
    set_idle_reqs[CFG_TUH_HID]; // USB control request structures for SET_IDLE
                                // command
static tuh_xfer_t set_idle_xfers[CFG_TUH_HID]; // TinyUSB transfer structures
                                               // for control transfer

//--------------------------------------------------------------------+
// Extern Declarations
//--------------------------------------------------------------------+
extern bool use_report_ids; // Flag indicating if the current report descriptor
                            // utilizes Report IDs
extern void ble_hid_periph_start(
    uint8_t const *desc_report,
    uint16_t desc_len); // Initialize and start the BLE HID peripheral
extern void ble_hid_periph_send_raw_report(
    uint8_t const *report,
    uint16_t len); // Forward a raw HID report to the BLE stack

//--------------------------------------------------------------------+
// Function Prototypes
//--------------------------------------------------------------------+
// Public callbacks
void tuh_mount_cb(uint8_t dev_addr);
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

// Static internal helper functions
static bool send_set_idle(uint8_t dev_addr, uint8_t instance);
static void set_idle_complete_cb(tuh_xfer_t *xfer);

//--------------------------------------------------------------------+
// TinyUSB Callbacks (Public API Implementations)
//--------------------------------------------------------------------+

/**
 * @brief Callback invoked by TinyUSB when a generic USB device is mounted.
 * Logs the device address, VID, and PID, and issues warnings for special
 * devices like Xbox.
 *
 * @param dev_addr USB address of the mounted device.
 */
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid, pid; // Vendor ID and Product ID of the mounted USB device
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    // Log when any USB device finishes enumeration
    DbgPrint("\n>>> USB Device attached, address = %d, VID = 0x%04X, PID = "
             "0x%04X <<<\r\n",
             dev_addr, vid, pid);
}

/**
 * @brief Callback invoked by TinyUSB when a USB HID device interface is
 * mounted. Logs the interface, detects special controllers, locks onto the
 * first eligible interface to bridge to BLE, triggers a SET_IDLE request, and
 * requests reports.
 *
 * @param dev_addr USB address of the mounted device.
 * @param instance Interface instance number on the USB device.
 * @param desc_report Pointer to the raw HID report descriptor buffer.
 * @param desc_len Length of the report descriptor buffer in bytes.
 */
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    // Log the mount event with the device address and interface instance
    DbgPrint("HID device address = %d, instance = %d is mounted\r\n", dev_addr,
             instance);

    // Prevent locking onto dummy interfaces or failed enumerations
    if (desc_report == NULL || desc_len == 0) {
        DbgPrint("Warning: Descriptor is missing or empty. Ignoring instance %d.\n",
                 instance);
        return;
    }

    // Lock onto the first mounted interface. Some D-Input gamepads are composite
    // devices where the HID interface is not instance 0.
    if (!device_bridged) {
        device_bridged = true;
        bridged_dev_addr = dev_addr;
        bridged_instance = instance;

        // Save descriptor for later starting BLE (after SET_IDLE completes)
        if (desc_report && desc_len > 0) {
            saved_desc_len = (desc_len > sizeof(saved_desc_report))
                             ? sizeof(saved_desc_report)
                             : desc_len;
            memcpy(saved_desc_report, desc_report, saved_desc_len);
        } else {
            saved_desc_len = 0;
        }
    } else if (dev_addr != bridged_dev_addr || instance != bridged_instance) {
        // Do not forward to BLE, but continue USB receive polling to observe logs
        DbgPrint("Info: Instance %d is mounted but not bridged to BLE.\n",
                 instance);
    }

    // D-Input gamepads and generic HID devices do not support Boot Protocol.
    // Forcing them into Report Protocol will cause a STALL and break enumeration.
    // Only apply this workaround for actual Keyboard/Mouse interfaces.
    uint8_t const itf_protocol = tuh_hid_interface_protocol(
        dev_addr, instance); // Protocol type of interface
    if (itf_protocol != HID_ITF_PROTOCOL_NONE) {
        // Force the USB device into Report Protocol mode.
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
    }

    // [WAKE UP COMMAND]
    // As a standard HID initialization sequence, send SET_IDLE(0) to all devices
    // (keyboards, mice, gamepads, etc.). This ensures reports are only sent when
    // the state changes and also serves as a "wakeup" command for some gamepads
    // to start sending data.
    if (!send_set_idle(dev_addr, instance)) {
        DbgPrint("Warning: Failed to queue SET_IDLE control transfer. Starting BLE "
                 "Bridge directly.\n");
        if (device_bridged && dev_addr == bridged_dev_addr &&
            instance == bridged_instance) {
            if (saved_desc_len > 0) {
                ble_hid_periph_start(saved_desc_report, saved_desc_len);
            }
        }
    }

    // Request to receive the first HID report from the device
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        DbgPrint("Error: cannot request to receive report\r\n");
        if (device_bridged && dev_addr == bridged_dev_addr &&
            instance == bridged_instance) {
            device_bridged = false;
        }
    }
}

/**
 * @brief Callback invoked by TinyUSB when a USB HID device interface is
 * unmounted. Allows re-bridging if the currently active bridged device
 * interface is disconnected.
 *
 * @param dev_addr USB address of the unmounted device.
 * @param instance Interface instance number.
 */
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    // Log the unmount event
    DbgPrint("HID device address = %d, instance = %d is unmounted\r\n", dev_addr,
             instance);

    // Allow re-mounting if the active bridged interface is disconnected
    if (device_bridged && dev_addr == bridged_dev_addr &&
        instance == bridged_instance) {
        device_bridged = false;
    }
}

/**
 * @brief Callback invoked by TinyUSB when a new HID report is received.
 * Forwards the report data to the BLE HID peripheral if it matches the bridged
 * instance, and requests the next report.
 *
 * @param dev_addr USB address of the device that sent the report.
 * @param instance Interface instance number.
 * @param report Pointer to the report buffer.
 * @param len Length of the report in bytes.
 */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    // Defensive guard: ignore reports from non-bridged interfaces
    if (!device_bridged || dev_addr != bridged_dev_addr ||
        instance != bridged_instance) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    if (len > 0) {
        // Forward the raw HID report directly to the BLE HID Bridge
        ble_hid_periph_send_raw_report(report, len);
    }

    // Continue to request the next report from the device
    // This is necessary to keep receiving data continuously
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        DbgPrint("Error: cannot request to receive report\r\n");
    }
}

//--------------------------------------------------------------------+
// Static Internal Helper Functions
//--------------------------------------------------------------------+

/**
 * @brief Manually sends a SET_IDLE control request to a mounted USB HID
 * interface. Helps initialize devices and acts as a wake-up command for silent
 * gamepads.
 *
 * @param dev_addr USB device address.
 * @param instance Interface instance number.
 */
static bool send_set_idle(uint8_t dev_addr, uint8_t instance) {
    if (instance >= CFG_TUH_HID) {
        DbgPrint("Error: HID instance %d exceeds CFG_TUH_HID (%d)\n", instance,
                 CFG_TUH_HID);
        return false;
    }
    DbgPrint("send_set_idle: Queueing SET_IDLE for dev %d, instance %d...\n",
             dev_addr, instance);
    set_idle_reqs[instance].bmRequestType =
        USB_REQ_TYPE_CLASS_INTERFACE; // Host-to-device, Class, Interface
    set_idle_reqs[instance].bRequest = HID_REQ_CONTROL_SET_IDLE; // 0x0A
    set_idle_reqs[instance].wValue =
        0; // idle_rate = 0 (send report only when state changes)
    set_idle_reqs[instance].wIndex = instance;
    set_idle_reqs[instance].wLength = 0;

    set_idle_xfers[instance].daddr = dev_addr;
    set_idle_xfers[instance].ep_addr = 0; // Control Endpoint 0
    set_idle_xfers[instance].setup = &set_idle_reqs[instance];
    set_idle_xfers[instance].buffer = NULL;
    set_idle_xfers[instance].complete_cb = set_idle_complete_cb;
    set_idle_xfers[instance].user_data = (uintptr_t)
        instance; // Pass instance to callback to handle multiple interfaces

    bool queued = tuh_control_xfer(&set_idle_xfers[instance]);
    DbgPrint("send_set_idle: tuh_control_xfer returned %d\n", queued);
    return queued;
}

/**
 * @brief Callback executed by TinyUSB when the custom SET_IDLE control transfer
 * finishes. Starts the BLE HID peripheral with the saved descriptor once
 * SET_IDLE completes.
 *
 * @param xfer Pointer to the TinyUSB transfer status structure.
 */
static void set_idle_complete_cb(tuh_xfer_t *xfer) {
    uint8_t dev_addr =
        xfer->daddr; // USB device address of the completed transfer
    uint8_t instance =
        (uint8_t)xfer->user_data; // Interface instance number from user data

    DbgPrint("set_idle_complete_cb: callback invoked. dev=%d, instance=%d, "
             "result=%d\n",
             dev_addr, instance, xfer->result);

    if (xfer->result != XFER_RESULT_SUCCESS) {
        DbgPrint("Warning: SET_IDLE for dev %d, instance %d failed. Result: %d\n",
                 dev_addr, instance, xfer->result);
    } else {
        DbgPrint("SET_IDLE command completed for dev %d, instance %d.\n", dev_addr,
                 instance);
    }

    // Start BLE Bridge after SET_IDLE finishes (even if it failed, we still
    // start)
    if (device_bridged && dev_addr == bridged_dev_addr &&
        instance == bridged_instance) {
        if (saved_desc_len > 0) {
            ble_hid_periph_start(saved_desc_report, saved_desc_len);
        }
    }
}