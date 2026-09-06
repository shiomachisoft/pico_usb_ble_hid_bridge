#include "bsp/board_api.h"
#include "debug_print.h"
#include "pico/time.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#include "ble_hid_periph.h"
#include "usb_hid_host.h"

//--------------------------------------------------------------------+
// Macro Definitions
//--------------------------------------------------------------------+
#define MAX_BRIDGED_INTERFACES 8
#define SAVED_DESC_REPORT_MAX_LEN 1024
#define COMBINED_DESC_MAX_LEN 2048

// Aggregation wait timeout (in milliseconds) after the first HID device is mounted
#define AGGREGATION_WAIT_MS 1500

// USB control request type: Host-to-device, Class, Interface (0x21)
#define USB_REQ_TYPE_CLASS_INTERFACE 0x21

//--------------------------------------------------------------------+
// Type Definitions & Structs
//--------------------------------------------------------------------+
typedef struct {
    bool active;
    bool set_idle_pending;
    uint8_t set_idle_retry;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t itf_num;                   // USB bInterfaceNumber
    uint8_t desc_report[SAVED_DESC_REPORT_MAX_LEN];
    uint16_t desc_len;
    bool has_report_id;
    uint8_t single_assigned_report_id; // If has_report_id == false
    uint8_t report_id_offset;          // If has_report_id == true
} hid_interface_info_t;

typedef enum {
    BRIDGE_STATE_WAITING_FIRST_DEVICE = 0,
    BRIDGE_STATE_AGGREGATING,
    BRIDGE_STATE_STARTED
} bridge_state_t;

//--------------------------------------------------------------------+
// Global & Static Variables
//--------------------------------------------------------------------+
static hid_interface_info_t mounted_itfs[MAX_BRIDGED_INTERFACES];
static uint8_t mounted_itfs_count = 0;

static bridge_state_t bridge_state = BRIDGE_STATE_WAITING_FIRST_DEVICE;
static uint32_t first_mount_ms = 0;
static uint32_t last_mount_ms = 0;

// Combined HID report descriptor buffer
static uint8_t combined_desc[COMBINED_DESC_MAX_LEN];
static uint16_t combined_desc_len = 0;

// Shared control transfer buffer for sequential SET_IDLE requests
static tusb_control_request_t set_idle_req;
static tuh_xfer_t set_idle_xfer;
static bool set_idle_busy = false;
static uint32_t last_set_idle_ms = 0;

//--------------------------------------------------------------------+
// Function Prototypes
//--------------------------------------------------------------------+
// Public API Functions & Main Task
void usb_hid_host_task(void);

// TinyUSB Callbacks
void tuh_mount_cb(uint8_t dev_addr);
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

// Static Internal Helper Functions
static void start_combined_ble_bridge(void);
static void service_set_idle_queue(void);
static bool send_set_idle(uint8_t dev_addr, uint8_t itf_num, uint8_t instance);
static void set_idle_complete_cb(tuh_xfer_t *xfer);

//--------------------------------------------------------------------+
// Public API Implementations & Main Tasks
//--------------------------------------------------------------------+

/**
 * @brief Periodically checks if the multi-device aggregation wait window has
 * elapsed, and merges all mounted HID report descriptors to start the BLE bridge.
 * Also services pending SET_IDLE requests sequentially.
 */
void usb_hid_host_task(void) {
    service_set_idle_queue();

    if (bridge_state == BRIDGE_STATE_AGGREGATING) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        // Start bridge once AGGREGATION_WAIT_MS has passed since the first device
        // and at least 300ms of quiet time has elapsed since the last interface mounted.
        if ((now - first_mount_ms >= AGGREGATION_WAIT_MS) &&
            (now - last_mount_ms >= 300)) {
            start_combined_ble_bridge();
        }
    }
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks (Public API Implementations)
//--------------------------------------------------------------------+

/**
 * @brief TinyUSB callback invoked when a generic USB device is mounted.
 *
 * @param dev_addr USB device address.
 */
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    DbgPrint("\n>>> USB Device attached, address = %d, VID = 0x%04X, PID = 0x%04X <<<\r\n",
             dev_addr, vid, pid);
}

/**
 * @brief TinyUSB callback invoked when a HID interface is mounted.
 * Stores the descriptor and begins the aggregation wait timer.
 *
 * @param dev_addr USB device address.
 * @param instance TinyUSB HID instance index.
 * @param desc_report Pointer to the raw HID report descriptor buffer.
 * @param desc_len Length of the report descriptor.
 */
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    DbgPrint("HID device address = %d, instance = %d is mounted (desc_len = %d)\r\n",
             dev_addr, instance, desc_len);

    if (desc_report == NULL || desc_len == 0) {
        DbgPrint("Warning: Descriptor is missing or empty. Ignoring instance %d.\n", instance);
        return;
    }

    if (bridge_state != BRIDGE_STATE_STARTED) {
        if (mounted_itfs_count < MAX_BRIDGED_INTERFACES) {
            hid_interface_info_t *itf = &mounted_itfs[mounted_itfs_count];
            itf->active = true;
            itf->set_idle_pending = true;
            itf->set_idle_retry = 0;
            itf->dev_addr = dev_addr;
            itf->instance = instance;

            tuh_itf_info_t itf_info = { 0 };
            if (tuh_hid_itf_get_info(dev_addr, instance, &itf_info)) {
                itf->itf_num = itf_info.desc.bInterfaceNumber;
            } else {
                itf->itf_num = instance; // Fallback
            }

            itf->desc_len = (desc_len > sizeof(itf->desc_report)) ? sizeof(itf->desc_report) : desc_len;
            memcpy(itf->desc_report, desc_report, itf->desc_len);
            mounted_itfs_count++;

            uint32_t now = to_ms_since_boot(get_absolute_time());
            last_mount_ms = now;

            if (bridge_state == BRIDGE_STATE_WAITING_FIRST_DEVICE) {
                bridge_state = BRIDGE_STATE_AGGREGATING;
                first_mount_ms = now;
                DbgPrint("Started multi-device aggregation timer (%d ms)...\n", AGGREGATION_WAIT_MS);
            }
        }
    } else {
        DbgPrint("Info: Device mounted after BLE bridge already started.\n");
    }

    // Request first report
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        DbgPrint("Error: cannot request to receive report for dev %d instance %d\r\n",
             dev_addr, instance);
    }
}

/**
 * @brief TinyUSB callback invoked when a HID interface is unmounted.
 *
 * @param dev_addr USB device address.
 * @param instance TinyUSB HID instance index.
 */
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    DbgPrint("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
}

/**
 * @brief TinyUSB callback invoked when an input report is received from a device.
 * Offsets the Report ID if necessary and forwards the report to the BLE peripheral.
 *
 * @param dev_addr USB device address.
 * @param instance TinyUSB HID instance index.
 * @param report Pointer to the received raw report data.
 * @param len Length of the received report in bytes.
 */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (bridge_state == BRIDGE_STATE_STARTED && len > 0) {
        // Find matching interface
        hid_interface_info_t *itf = NULL;
        for (int i = 0; i < mounted_itfs_count; i++) {
            if (mounted_itfs[i].active &&
                mounted_itfs[i].dev_addr == dev_addr &&
                mounted_itfs[i].instance == instance) {
                itf = &mounted_itfs[i];
                break;
            }
        }

        if (itf != NULL) {
            uint8_t send_buf[65];
            if (!itf->has_report_id) {
                // Prepend assigned Report ID (ID 1 byte + data up to 64 bytes: max 65 bytes)
                send_buf[0] = itf->single_assigned_report_id;
                uint16_t copy_len = (len > 64) ? 64 : len;
                memcpy(&send_buf[1], report, copy_len);
                DbgPrint("USB RX itf %d (no-id -> id %d, len %d): %02x %02x %02x %02x\n",
                         instance, send_buf[0], copy_len + 1,
                         send_buf[0], (copy_len > 0) ? send_buf[1] : 0,
                         (copy_len > 1) ? send_buf[2] : 0, (copy_len > 2) ? send_buf[3] : 0);
                ble_hid_periph_send_raw_report(send_buf, copy_len + 1);
            } else {
                if (report[0] == 0) {
                    DbgPrint("Warning: itf %d has Report ID but report[0] == 0 (len %d, boot report?)\n", instance, len);
                }
                // Offset existing Report ID if needed (max 64 bytes)
                uint16_t copy_len = (len > 64) ? 64 : len;
                memcpy(send_buf, report, copy_len);
                if (itf->report_id_offset > 0) {
                    send_buf[0] += itf->report_id_offset;
                }
                DbgPrint("USB RX itf %d (orig-id %d -> id %d, len %d): %02x %02x %02x %02x\n",
                         instance, report[0], send_buf[0], copy_len,
                         send_buf[0], (copy_len > 1) ? send_buf[1] : 0,
                         (copy_len > 2) ? send_buf[2] : 0, (copy_len > 3) ? send_buf[3] : 0);
                ble_hid_periph_send_raw_report(send_buf, copy_len);
            }
        } else {
            DbgPrint("USB RX itf %d: not found in mounted list\n", instance);
        }
    } else {
        if (bridge_state != BRIDGE_STATE_STARTED) {
            DbgPrint("USB RX before bridge started: itf %d len %d\n", instance, len);
        }
    }

    // Request next report continuously
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        DbgPrint("Error: cannot request next report for dev %d itf %d\r\n", dev_addr, instance);
    }
}

//--------------------------------------------------------------------+
// Static Internal Helper Functions
//--------------------------------------------------------------------+

/**
 * @brief Merges report descriptors from all mounted interfaces, assigns/offsets
 * Report IDs, and starts the BLE HID peripheral.
 */
static void start_combined_ble_bridge(void) {
    if (bridge_state == BRIDGE_STATE_STARTED || mounted_itfs_count == 0) {
        return;
    }

    bridge_state = BRIDGE_STATE_STARTED;
    DbgPrint("\n=== Aggregating %d HID Interface(s) for BLE Bridge ===\n", mounted_itfs_count);

    uint8_t next_report_id = 1;
    combined_desc_len = 0;

    for (int idx = 0; idx < mounted_itfs_count; idx++) {
        hid_interface_info_t *itf = &mounted_itfs[idx];
        const uint8_t *src = itf->desc_report;
        uint16_t src_len = itf->desc_len;
        uint16_t itf_start_offset = combined_desc_len;

        // Scan descriptor to check if it already uses Report ID (tag 0x84 / item 0x85)
        itf->has_report_id = false;
        uint8_t max_id_in_desc = 0;

        for (uint16_t i = 0; i < src_len;) {
            uint8_t item = src[i];
            uint16_t size = item & 0x03;
            if (size == 3) size = 4;
            if (item == 0xFE) { // Long item
                size = (i + 1 < src_len) ? src[i + 1] + 2 : 0;
            }

            uint8_t type = item & 0xFC;
            if (type == 0x84) { // Report ID item
                itf->has_report_id = true;
                if (size > 0 && (i + 1 < src_len)) {
                    if (src[i + 1] > max_id_in_desc) {
                        max_id_in_desc = src[i + 1];
                    }
                }
            }
            i += size + 1;
        }

        if (!itf->has_report_id) {
            // Assign a single new Report ID
            itf->single_assigned_report_id = next_report_id++;
            itf->report_id_offset = 0;
            DbgPrint("  Interface %d (dev %d, itf %d): No Report ID -> Assigned ID %d\n",
                     idx, itf->dev_addr, itf->instance, itf->single_assigned_report_id);

            // Insert "Report ID (itf->single_assigned_report_id)" right after the first Collection item,
            // or at the interface start offset if no Collection is found.
            bool inserted = false;
            for (uint16_t i = 0; i < src_len;) {
                uint8_t item = src[i];
                uint16_t size = item & 0x03;
                if (size == 3) size = 4;
                if (item == 0xFE) {
                    size = (i + 1 < src_len) ? src[i + 1] + 2 : 0;
                }

                uint16_t item_total_len = size + 1;
                // Append current item
                if (combined_desc_len + item_total_len <= sizeof(combined_desc)) {
                    memcpy(&combined_desc[combined_desc_len], &src[i], item_total_len);
                    combined_desc_len += item_total_len;
                }

                // If this is a Collection item (tag 0xA0 / 0xA1) and not yet inserted, insert Report ID item (0x85, ID)
                if (!inserted && (item & 0xFC) == 0xA0) {
                    if (combined_desc_len + 2 <= sizeof(combined_desc)) {
                        combined_desc[combined_desc_len++] = 0x85; // Report ID tag (size 1)
                        combined_desc[combined_desc_len++] = itf->single_assigned_report_id;
                        inserted = true;
                    }
                }
                i += item_total_len;
            }

            if (!inserted) {
                // Fallback: If no collection item was found, insert Report ID at this interface's start offset
                if (combined_desc_len + 2 <= sizeof(combined_desc)) {
                    uint16_t bytes_to_shift = combined_desc_len - itf_start_offset;
                    memmove(&combined_desc[itf_start_offset + 2], &combined_desc[itf_start_offset], bytes_to_shift);
                    combined_desc[itf_start_offset] = 0x85;
                    combined_desc[itf_start_offset + 1] = itf->single_assigned_report_id;
                    combined_desc_len += 2;
                }
            }
        } else {
            // Interface already has Report IDs: add offset to prevent collisions
            itf->report_id_offset = (next_report_id > 1) ? (next_report_id - 1) : 0;
            DbgPrint("  Interface %d (dev %d, itf %d): Has Report ID (max=%d) -> Offset +%d\n",
                     idx, itf->dev_addr, itf->instance, max_id_in_desc, itf->report_id_offset);

            for (uint16_t i = 0; i < src_len;) {
                uint8_t item = src[i];
                uint16_t size = item & 0x03;
                if (size == 3) size = 4;
                if (item == 0xFE) {
                    size = (i + 1 < src_len) ? src[i + 1] + 2 : 0;
                }

                uint16_t item_total_len = size + 1;
                uint8_t type = item & 0xFC;

                if (type == 0x84 && size > 0 && (i + 1 < src_len)) {
                    // Rewrite Report ID item with offset
                    if (combined_desc_len + 2 <= sizeof(combined_desc)) {
                        combined_desc[combined_desc_len++] = 0x85;
                        combined_desc[combined_desc_len++] = src[i + 1] + itf->report_id_offset;
                    }
                } else {
                    if (combined_desc_len + item_total_len <= sizeof(combined_desc)) {
                        memcpy(&combined_desc[combined_desc_len], &src[i], item_total_len);
                        combined_desc_len += item_total_len;
                    }
                }
                i += item_total_len;
            }

            next_report_id += (max_id_in_desc > 0) ? max_id_in_desc : 1;
        }
    }

    DbgPrint("Combined Descriptor created: %d bytes total\n", combined_desc_len);
    ble_hid_periph_start(combined_desc, combined_desc_len);
}

/**
 * @brief Periodically checks and dispatches queued SET_IDLE requests sequentially.
 */
static void service_set_idle_queue(void) {
    if (set_idle_busy) {
        return;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    // Only attempt to queue SET_IDLE every 50ms to allow preceding control transfers (like SET_PROTOCOL) to complete
    if (now - last_set_idle_ms < 50) {
        return;
    }

    for (int i = 0; i < mounted_itfs_count; i++) {
        if (mounted_itfs[i].active && mounted_itfs[i].set_idle_pending) {
            last_set_idle_ms = now;
            if (send_set_idle(mounted_itfs[i].dev_addr, mounted_itfs[i].itf_num, mounted_itfs[i].instance)) {
                mounted_itfs[i].set_idle_pending = false;
                set_idle_busy = true;
                break; // Send sequentially (one transfer at a time)
            } else {
                // Control endpoint is temporarily busy (e.g. SET_PROTOCOL transfer in progress).
                // Retry in the next task tick after 50ms up to 10 times (total ~500ms).
                mounted_itfs[i].set_idle_retry++;
                if (mounted_itfs[i].set_idle_retry >= 10) {
                    DbgPrint("Warning: Giving up SET_IDLE for dev %d itf %d (instance %d) after %d retries\n",
                             mounted_itfs[i].dev_addr, mounted_itfs[i].itf_num, mounted_itfs[i].instance, mounted_itfs[i].set_idle_retry);
                    mounted_itfs[i].set_idle_pending = false;
                }
                break;
            }
        }
    }
}

/**
 * @brief Sends a USB HID SET_IDLE control request to the specified interface.
 *
 * @param dev_addr USB device address.
 * @param itf_num Target USB interface number (bInterfaceNumber).
 * @param instance TinyUSB HID instance index.
 * @return true if the control transfer was successfully queued, false otherwise.
 */
static bool send_set_idle(uint8_t dev_addr, uint8_t itf_num, uint8_t instance) {
    DbgPrint("send_set_idle: Queueing SET_IDLE for dev %d, itf %d (instance %d)...\n",
             dev_addr, itf_num, instance);
    set_idle_req.bmRequestType = USB_REQ_TYPE_CLASS_INTERFACE; // 0x21
    set_idle_req.bRequest = HID_REQ_CONTROL_SET_IDLE;          // 0x0A
    set_idle_req.wValue = 0;                                  // idle_rate = 0
    set_idle_req.wIndex = itf_num;                            // USB interface number (bInterfaceNumber)
    set_idle_req.wLength = 0;

    set_idle_xfer.daddr = dev_addr;
    set_idle_xfer.ep_addr = 0;
    set_idle_xfer.setup = &set_idle_req;
    set_idle_xfer.buffer = NULL;
    set_idle_xfer.complete_cb = set_idle_complete_cb;
    set_idle_xfer.user_data = (uintptr_t)instance;

    return tuh_control_xfer(&set_idle_xfer);
}

/**
 * @brief Completion callback for the SET_IDLE control transfer.
 *
 * @param xfer Pointer to the completed USB transfer descriptor.
 */
static void set_idle_complete_cb(tuh_xfer_t *xfer) {
    uint8_t dev_addr = xfer->daddr;
    uint8_t instance = (uint8_t)xfer->user_data;
    DbgPrint("set_idle_complete_cb: dev=%d, instance=%d, result=%d\n",
             dev_addr, instance, xfer->result);
    set_idle_busy = false;
}