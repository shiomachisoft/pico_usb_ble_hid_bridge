/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#include "debug_print.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack_util.h"
#include "hardware/irq.h"
#include "pico/cyw43_arch.h"
#include "pico/util/queue.h"

#include "ble/att_db_util.h"
#include "ble/le_device_db_tlv.h"
#include "btstack.h"
#include "btstack_tlv_flash_bank.h"
#include "pico/btstack_flash_bank.h"

//--------------------------------------------------------------------+
// Macro Definitions
//--------------------------------------------------------------------+
// Size of the queue to buffer HID reports from USB before sending via BLE
#define REPORT_QUEUE_SIZE 64

// Advertisement interval (0x0030 * 0.625ms = 30ms)
#define APP_ADV_INTERVAL 0x0030

#define MAX_BLE_REPORTS                                                        \
    64 // Maximum number of HIDS reports parsed from the descriptor

// Maximum length of the dynamic HID report descriptor (in bytes)
#define DYNAMIC_HID_DESC_MAX_LEN 2048

// Maximum length of a raw HID report (in bytes)
#define HID_REPORT_MAX_LEN 64

// Initial battery level percentage (0 to 100)
#define INITIAL_BATTERY_LEVEL 100

// BLE Appearance value for a generic HID device
#define BLE_APPEARANCE_HID_GENERIC 0x03C0

//--------------------------------------------------------------------+
// Type Definitions & Structs
//--------------------------------------------------------------------+
// Information mapping HIDS reports to GATT attributes
typedef struct {
    uint8_t id;            // Report ID of the HID report
    uint8_t type;          // Report type: 1 = Input, 2 = Output, 3 = Feature
    uint16_t value_handle; // GATT attribute handle for the characteristic value
} hid_report_info_t;

// Structure representing a single HID report to be stored in the queue
typedef struct {
    uint8_t report[HID_REPORT_MAX_LEN]; // Raw report data
    uint16_t len;                       // Length of the report data
} hid_report_t;

//--------------------------------------------------------------------+
// Global & Static Variables
//--------------------------------------------------------------------+
// Global variables
bool use_report_ids = false; // Flag indicating if the current report descriptor
                             // utilizes Report IDs

// Registration object for HCI event callbacks
static btstack_packet_callback_registration_t
    hci_event_callback_registration; // Event registration token for HCI packets
// Registration object for Security Manager (SM) event callbacks
static btstack_packet_callback_registration_t
    sm_event_callback_registration; // Event registration token for SM packets
// Battery level percentage (0-100)
static uint8_t battery =
    INITIAL_BATTERY_LEVEL; // Static variable holding current battery capacity
// Connection handle for the current BLE connection. Invalid if not connected.
static volatile hci_con_handle_t con_handle =
    HCI_CON_HANDLE_INVALID; // Active connection handle
// Flag to indicate if the BLE connection is fully encrypted
static volatile bool link_encrypted = false; // Security link status flag

static hid_report_info_t
    ble_reports[MAX_BLE_REPORTS];   // Array of parsed report definitions
static uint8_t num_ble_reports = 0; // Current count of elements in ble_reports

// Callback registrations for executing code safely on the BTstack thread (Core
// 1)
static btstack_context_callback_registration_t
    setup_callback; // Thread-safe callback handle for BLE HIDS setup
static btstack_context_callback_registration_t
    send_request_callback; // Thread-safe callback handle for requesting
                           // CAN_SEND_NOW
// Flag to prevent multiple concurrent CAN_SEND_NOW requests
static atomic_bool send_request_pending =
    false; // Thread-safe state flag to guard against redundant write requests

// Context for the Flash-based TLV storage used to save pairing/bonding keys
static btstack_tlv_flash_bank_t
    btstack_tlv_flash_bank_context; // Storage context for pairing keys in Flash

// Mask list of interrupts for Pico-PIO-USB and DMA to prevent XIP conflicts
// during Flash writes
static const uint8_t mask_irqs[] = {PIO0_IRQ_0, PIO0_IRQ_1,
                                    PIO1_IRQ_0, PIO1_IRQ_1,
#ifdef PIO2_IRQ_0
                                    PIO2_IRQ_0, PIO2_IRQ_1,
#endif
                                    DMA_IRQ_0,  DMA_IRQ_1};
#define NUM_MASK_IRQS (sizeof(mask_irqs) / sizeof(mask_irqs[0]))

static uint32_t irq_mask_state = 0;

static void mask_pio_dma_irqs(void) {
    irq_mask_state = 0;
    for (int i = 0; i < NUM_MASK_IRQS; i++) {
        if (irq_is_enabled(mask_irqs[i])) {
            irq_mask_state |= (1 << i);
            irq_set_enabled(mask_irqs[i], false);
        }
    }
}

static void restore_pio_dma_irqs(void) {
    for (int i = 0; i < NUM_MASK_IRQS; i++) {
        if (irq_mask_state & (1 << i)) {
            irq_set_enabled(mask_irqs[i], true);
        }
    }
}

static const hal_flash_bank_t *original_hal_flash_bank = NULL;
static hal_flash_bank_t my_hal_flash_bank;

static void my_flash_erase(void *context, int block_nr) {
    if (!original_hal_flash_bank)
        return;
    mask_pio_dma_irqs();
    original_hal_flash_bank->erase(context, block_nr);
    restore_pio_dma_irqs();
}

static void my_flash_write(void *context, int block_nr, uint32_t offset,
                           const uint8_t *data, uint32_t size) {
    if (!original_hal_flash_bank)
        return;
    mask_pio_dma_irqs();
    original_hal_flash_bank->write(context, block_nr, offset, data, size);
    restore_pio_dma_irqs();
}

// BLE Advertisement data payload (announces device presence and capabilities)
static const uint8_t adv_data[] = {
    // Flags general discoverable, BR/EDR not supported
    0x02,
    BLUETOOTH_DATA_TYPE_FLAGS,
    0x06,
    // Name
    0x10,
    BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'U',
    'S',
    'B',
    ' ',
    'B',
    'L',
    'E',
    ' ',
    'H',
    'I',
    'D',
    ' ',
    'B',
    'r',
    'g',
    // 16-bit Service UUIDs
    0x03,
    BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS,
    ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE & 0xff,
    ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE >> 8,
    // Appearance HID - Generic (Category 15, Sub-Category 0)
    0x03,
    BLUETOOTH_DATA_TYPE_APPEARANCE,
    0xC0,
    0x03,
};
// Length of the BLE advertisement data payload
static const uint8_t adv_data_len =
    sizeof(adv_data); // Byte size of advertisement payload

// Buffer to store the dynamic HID report descriptor received from the USB
// device
static uint8_t dynamic_hid_desc[DYNAMIC_HID_DESC_MAX_LEN]; // Buffer holding
                                                           // report descriptor
// Actual length of the dynamic HID report descriptor
static uint16_t dynamic_hid_desc_len =
    0; // Length of the active report descriptor
// Flag to indicate if the BLE peripheral stack has been initialized
static bool ble_initialized = false; // Initialization guard flag

// Thread-safe queue used to pass HID reports from the USB core (Core 0) to the
// BLE core (Core 1)
static queue_t report_queue; // Thread-safe circular queue structure

//--------------------------------------------------------------------+
// Function Prototypes
//--------------------------------------------------------------------+
// Public API Functions
void ble_hid_periph_init_flash(void);
void ble_hid_periph_start(uint8_t const *desc_report, uint16_t desc_len);
void ble_hid_periph_send_raw_report(uint8_t const *report, uint16_t len);
bool ble_hid_is_connected(void);

// Static Internal Helper Functions
static void setup_on_btstack_thread(void *context);
static void request_can_send_now_on_btstack_thread(void *context);
static void build_dynamic_gatt(const uint8_t *desc, uint16_t desc_len);
static void ble_hid_periph_setup(const uint8_t *desc, uint16_t desc_len);
static void typing_can_send_now(void);
static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void att_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size);

//--------------------------------------------------------------------+
// Public API Function Implementations
//--------------------------------------------------------------------+

/**
 * @brief Initializes non-volatile memory (Flash TLV) for storing pairing keys.
 * Ensures the device remains bonded across reboots.
 */
void ble_hid_periph_init_flash(void) {
    // Setup non-volatile memory (Flash) for saving Bluetooth pairing/bonding keys
    // Without this, the device will lose pairings on reboot and fail to reconnect
    const hal_flash_bank_t *hal_flash_bank =
        pico_flash_bank_instance(); // Pointer to flash memory bank configuration

    // Wrap original hal_flash_bank to mask PIO-USB/DMA interrupts during
    // erase/write operations
    if (hal_flash_bank) {
        original_hal_flash_bank = hal_flash_bank;
        my_hal_flash_bank = *hal_flash_bank;      // Copy struct contents
        my_hal_flash_bank.erase = my_flash_erase; // Override erase pointer
        my_hal_flash_bank.write = my_flash_write; // Override write pointer
    }

    const btstack_tlv_t *tlv_impl = btstack_tlv_flash_bank_init_instance(
        &btstack_tlv_flash_bank_context,
        original_hal_flash_bank ? &my_hal_flash_bank : hal_flash_bank,
        NULL); // TLV API implementation pointer
    // Set the global TLV storage instance for BTstack
    btstack_tlv_set_instance(tlv_impl, &btstack_tlv_flash_bank_context);

    if (tlv_impl) {
        DbgPrint("TLV Flash Bank initialized successfully.\n");
    } else {
        DbgPrint("ERROR: TLV Flash Bank initialization failed!\n");
    }

    // Initialize the LE Device DB using the TLV instance to securely save bonding
    // keys
    le_device_db_tlv_configure(tlv_impl, &btstack_tlv_flash_bank_context);
    le_device_db_init();
}

/**
 * @brief Starts the BLE HID peripheral, parses the USB descriptor, and prepares
 * the services. Schedules HIDS setup to run on Core 1.
 *
 * @param desc_report Pointer to the USB device descriptor buffer.
 * @param desc_len Length of the report descriptor buffer.
 */
void ble_hid_periph_start(uint8_t const *desc_report, uint16_t desc_len) {
    // If already initialized, do nothing
    if (ble_initialized)
        return;
    ble_initialized = true;

    // Ensure the incoming descriptor doesn't exceed buffer size
    if (desc_len > sizeof(dynamic_hid_desc)) {
        desc_len = sizeof(dynamic_hid_desc);
    }

    // Pass-through: copy descriptor without modifications
    memcpy(dynamic_hid_desc, desc_report, desc_len);
    dynamic_hid_desc_len = desc_len;

    DbgPrint("\n--- USB HID Report Descriptor (%u bytes) ---\n", desc_len);
    printf_hexdump(desc_report, desc_len);
    DbgPrint("--------------------------------------------\n\n");

    // Parse descriptor to determine dynamically required Report Characteristics
    num_ble_reports = 0;
    use_report_ids = false;
    uint8_t current_id = 0; // Temporary storage for current report ID

    for (uint16_t i = 0;
         i < desc_len;) {            // Loop index to traverse descriptor buffer
        uint8_t item = desc_report[i]; // Short/long item header byte
        uint16_t size = item & 0x03;   // Payload size of the item
        if (size == 3)
            size = 4;
        if (item == 0xFE) {
            size = (i + 1 < desc_len) ? desc_report[i + 1] + 2 : 0;
        }

        uint8_t type = item & 0xFC; // Prefix code identifying the item
        if (type == 0x84) {         // Report ID
            if (size > 0 && (i + 1 < desc_len)) {
                current_id = desc_report[i + 1];
                use_report_ids = true;
            }
        } else if (type == 0x80 || type == 0x90 || type == 0xB0) {
            uint8_t rtype = (type == 0x80)   ? 1
                            : (type == 0x90) ? 2
                                             : 3; // Mapped BLE HIDS report type
            bool found = false; // Check flag if definition is already stored
            for (int r = 0; r < num_ble_reports; r++) {
                if (ble_reports[r].id == current_id && ble_reports[r].type == rtype) {
                    found = true;
                    break;
                }
            }
            if (!found && num_ble_reports < MAX_BLE_REPORTS) {
                ble_reports[num_ble_reports].id = current_id;
                ble_reports[num_ble_reports].type = rtype;
                num_ble_reports++;
            }
        }
        i += size + 1;
    }

    // Initialize the thread-safe queue for report buffering
    queue_init(&report_queue, sizeof(hid_report_t), REPORT_QUEUE_SIZE);

    // Safely schedule the setup process to run on Core 1 (the BTstack thread)
    setup_callback.callback = &setup_on_btstack_thread;
    btstack_run_loop_execute_on_main_thread(&setup_callback);
}

/**
 * @brief Safely enqueues a raw HID report from USB to send it over BLE.
 * Dequeues old reports if the queue overflows. Schedules a send callback.
 *
 * @param report Pointer to the raw report data.
 * @param len Length of the report data.
 */
void ble_hid_periph_send_raw_report(uint8_t const *report, uint16_t len) {
    // Protect against null pointers or empty reports
    if (!report || len == 0)
        return;

    if (con_handle == HCI_CON_HANDLE_INVALID || !link_encrypted) {
        return;
    }

    hid_report_t item; // Report structure to push to the queue
    // Zero-clear the entire structure to prevent uninitialized stack memory from
    // entering the queue
    memset(&item, 0, sizeof(item));
    // Prevent buffer overflow by limiting the length to our internal structure
    // size
    if (len > sizeof(item.report))
        len = sizeof(item.report);
    // Copy the report data into the local item structure
    memcpy(item.report, report, len);
    item.len = len;

    // Add the data to the queue. If the queue is full, remove the oldest report.
    // HID is state-based, so newer reports contain the current button states.
    // This prevents button presses from being dropped if IMU/analog noise floods
    // the queue!
    if (!queue_try_add(&report_queue, &item)) {
        hid_report_t dummy; // Temporary storage to discard old report
        queue_try_remove(&report_queue, &dummy);
        queue_try_add(&report_queue, &item);
    }

    // Schedule a thread-safe callback to request a CAN_SEND_NOW event on Core 1
    if (!atomic_exchange(&send_request_pending, true)) {
        send_request_callback.callback = &request_can_send_now_on_btstack_thread;
        btstack_run_loop_execute_on_main_thread(&send_request_callback);
    }
}

/**
 * @brief Checks if a BLE host is currently connected and the link is encrypted.
 *
 * @return true if connected and encrypted, false otherwise.
 */
bool ble_hid_is_connected(void) {
    // Return true if the connection handle is valid
    return (con_handle != HCI_CON_HANDLE_INVALID) && link_encrypted;
}

//--------------------------------------------------------------------+
// Static Internal Helper Functions
//--------------------------------------------------------------------+

/**
 * @brief Thread-safe callback executing on Core 1 to configure HIDS and power
 * on HCI.
 *
 * @param context User context parameter (unused).
 */
static void setup_on_btstack_thread(void *context) {
    (void)context;
    // Set up all BLE services with the copied descriptor
    ble_hid_periph_setup(dynamic_hid_desc, dynamic_hid_desc_len);
    // Turn on the HCI controller power to activate Bluetooth
    hci_power_control(HCI_POWER_ON);
    DbgPrint("BLE HID bridge started with dynamic descriptor (len=%u)\n",
             dynamic_hid_desc_len);
}

/**
 * @brief Core 1 callback requesting a CAN_SEND_NOW event to send pending queue
 * data.
 *
 * @param context User context parameter (unused).
 */
static void request_can_send_now_on_btstack_thread(void *context) {
    (void)context;
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        att_server_request_can_send_now_event(con_handle);
    }
    // Clear the flag at the "end" of the callback execution (prevents linked list
    // corruption)
    atomic_store(&send_request_pending, false);
}

/**
 * @brief Dynamically constructs the BLE GATT database based on the HID
 * descriptor. Registers Services like GAP, GATT, DIS, Battery, and HIDS with
 * appropriate permissions.
 *
 * @param desc Pointer to the raw descriptor.
 * @param desc_len Descriptor length in bytes.
 */
static void build_dynamic_gatt(const uint8_t *desc, uint16_t desc_len) {
    att_db_util_init();

    // GAP Service
    att_db_util_add_service_uuid16(0x1800);
    att_db_util_add_characteristic_uuid16(0x2A00, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          (uint8_t *)"USB BLE HID Brg", 15);
    static uint16_t appearance =
        BLE_APPEARANCE_HID_GENERIC; // HID Generic appearance value
    att_db_util_add_characteristic_uuid16(0x2A01, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          (uint8_t *)&appearance, 2);

    // GATT Service
    att_db_util_add_service_uuid16(0x1801);

    // Device Information Service
    att_db_util_add_service_uuid16(0x180A);
    att_db_util_add_characteristic_uuid16(0x2A29, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          (uint8_t *)"Pico W", 6);

    // PnP ID (0x2A50) is strictly required by Windows for HID devices.
    // Source: 0x02 (USB), VID: 0x2E8A (Raspberry Pi), PID: 0x0000, Version:
    // 0x0100
    static uint8_t pnp_id[] = {0x02, 0x8A, 0x2E, 0x00,
                               0x00, 0x00, 0x01}; // Standard plug and play ID
    // Remove ENCRYPTED security requirement since Windows tries to read the PnP
    // ID before pairing
    att_db_util_add_characteristic_uuid16(0x2A50, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          pnp_id, 7);

    // Battery Service
    att_db_util_add_service_uuid16(0x180F);
    att_db_util_add_characteristic_uuid16(
        0x2A19, ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY, ATT_SECURITY_NONE,
        ATT_SECURITY_NONE, &battery, 1);
    // CCCD (0x2902) is mandatory because it has the NOTIFY property
    static uint16_t bat_client_config = 0; // CCCD state memory
    att_db_util_add_descriptor_uuid16(
        0x2902, ATT_PROPERTY_READ | ATT_PROPERTY_WRITE, ATT_SECURITY_NONE,
        ATT_SECURITY_NONE, (uint8_t *)&bat_client_config, 2);

    // HID Service
    att_db_util_add_service_uuid16(0x1812);

    static uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x02}; // HIDS info details
    // Metadata needs to be readable in plaintext before pairing (HOGP
    // specification)
    att_db_util_add_characteristic_uuid16(0x2A4A, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          hid_info, 4);

    static uint8_t protocol_mode_val = 1; // Protocol mode (1 = Report Mode)
    att_db_util_add_characteristic_uuid16(
        0x2A4E, ATT_PROPERTY_READ | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE, &protocol_mode_val, 1);

    static uint8_t hid_control_point = 0; // HIDS control point state
    att_db_util_add_characteristic_uuid16(
        0x2A4C, ATT_PROPERTY_WRITE_WITHOUT_RESPONSE, ATT_SECURITY_NONE,
        ATT_SECURITY_NONE, &hid_control_point, 1);

    // Report Map
    // Keep Report Map in plaintext so Windows can correctly identify it as a
    // mouse
    att_db_util_add_characteristic_uuid16(0x2A4B, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          (uint8_t *)desc, desc_len);

    // Gamepad reports can be up to HID_REPORT_MAX_LEN bytes.
    // Setting this to 8 causes BTstack to truncate or reject gamepad inputs!
    static uint8_t empty_report[HID_REPORT_MAX_LEN] = {
        0}; // Static zeroed report buffer
    static uint16_t client_configs[MAX_BLE_REPORTS]; // CCCDs configuration array
    static uint8_t rep_refs[MAX_BLE_REPORTS]
                           [2]; // Report Reference mapping values (id, type)
    memset(client_configs, 0, sizeof(client_configs));

    // Report Characteristics
    for (int i = 0; i < num_ble_reports;
         i++) { // Loop variable to configure characteristic details
        uint16_t props = ATT_PROPERTY_READ; // GATT properties flags
        if (ble_reports[i].type == 1)
            props |= ATT_PROPERTY_NOTIFY;
        else if (ble_reports[i].type == 2 || ble_reports[i].type == 3)
            props |= ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE;

        // The report data itself requires encrypted communication
        ble_reports[i].value_handle = att_db_util_add_characteristic_uuid16(
            0x2A4D, props, ATT_SECURITY_ENCRYPTED, ATT_SECURITY_ENCRYPTED,
            empty_report, sizeof(empty_report));

        if (props & ATT_PROPERTY_NOTIFY) {
            // Requiring encryption when writing to CCCD triggers pairing at the
            // correct time
            att_db_util_add_descriptor_uuid16(
                0x2902, ATT_PROPERTY_READ | ATT_PROPERTY_WRITE, ATT_SECURITY_NONE,
                ATT_SECURITY_ENCRYPTED, (uint8_t *)&client_configs[i], 2);
        }

        rep_refs[i][0] = ble_reports[i].id;
        rep_refs[i][1] = ble_reports[i].type;
        // Keep Report Reference readable in plaintext
        att_db_util_add_descriptor_uuid16(0x2908, ATT_PROPERTY_READ,
                                          ATT_SECURITY_NONE, ATT_SECURITY_NONE,
                                          rep_refs[i], 2);
    }
}

/**
 * @brief Sets up Bluetooth protocol layers (L2CAP, SM, GAP, ATT, HIDS) on
 * Core 1.
 *
 * @param desc Pointer to the descriptor.
 * @param desc_len Length of the descriptor.
 */
static void ble_hid_periph_setup(const uint8_t *desc, uint16_t desc_len) {

    // Initialize the L2CAP layer (Logical Link Control and Adaptation Protocol)
    l2cap_init();

    // Initialize the Security Manager
    sm_init();
    // Set IO capabilities to no input/output, typical for simple peripheral
    // devices
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // Require secure connections and bonding (save pairing information)
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION |
                                       SM_AUTHREQ_BONDING);

    // Dynamically build the GATT DB based on the USB device descriptor
    build_dynamic_gatt(desc, desc_len);

    // Setup ATT server with the dynamic DB
    att_server_init(att_db_util_get_address(), NULL, NULL);

    // setup advertisements
    // Set advertisement intervals: min and max to 0x0030 (30ms * 0.625)
    uint16_t adv_int_min = APP_ADV_INTERVAL; // Minimum advertisement interval
    uint16_t adv_int_max = APP_ADV_INTERVAL; // Maximum advertisement interval
    // Advertisement type 0: Connectable undirected advertising
    uint8_t adv_type = 0;    // Advertising parameter
    bd_addr_t null_addr;     // Blank target peer address
    memset(null_addr, 0, 6); // Clear the direct address field
    // Configure advertisement parameters
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0,
                                  null_addr, 0x07, 0x00);
    // Set the advertisement payload data
    gap_advertisements_set_data(adv_data_len, (uint8_t *)adv_data);

    // register for HCI events
    // Set up the callback for Host Controller Interface events (e.g.,
    // connections)
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // register for SM events
    // Set up the callback for Security Manager events (e.g., pairing processes)
    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    // Register for ATT events (e.g., CAN_SEND_NOW)
    att_server_register_packet_handler(att_packet_handler);
}

/**
 * @brief Dequeues a pending report from queue and transmits it over BLE notify.
 * Triggers another notification request if there is more data waiting.
 */
static void typing_can_send_now(void) {
    hid_report_t item; // Buffer to hold popped queue report
    // Dequeue a report and send it over BLE
    if (queue_try_remove(&report_queue, &item)) {

        uint8_t id = use_report_ids ? item.report[0]
                                    : 0; // Mapped Report ID from report data
        uint16_t val_handle = 0;         // Target HIDS characteristic value handle
        for (int i = 0; i < num_ble_reports; i++) {
            if (ble_reports[i].id == id && ble_reports[i].type == 1) { // 1 = Input
                val_handle = ble_reports[i].value_handle;
                break;
            }
        }

        if (val_handle != 0) {
            uint8_t status; // Notification return status
            if (use_report_ids && item.len >= 1) {
                status = att_server_notify(con_handle, val_handle, item.report + 1,
                                           item.len - 1);
            } else {
                status =
                    att_server_notify(con_handle, val_handle, item.report, item.len);
            }
            (void)status; // Suppress unused variable warning
        } else {
            // Fallback: If Report ID wasn't found (e.g., due to parser limits or spec
            // violation), send the raw data to the first available Input report
            // characteristic.
            for (int i = 0; i < num_ble_reports; i++) {
                if (ble_reports[i].type == 1) { // 1 = Input
                    if (use_report_ids && item.len >= 1) {
                        att_server_notify(con_handle, ble_reports[i].value_handle,
                                          item.report + 1, item.len - 1);
                    } else {
                        att_server_notify(con_handle, ble_reports[i].value_handle,
                                          item.report, item.len);
                    }
                    break;
                }
            }
        }

        // If there are more reports waiting in the queue, request another send
        // event
        if (!queue_is_empty(&report_queue)) {
            att_server_request_can_send_now_event(con_handle);
        }
    }
}

/**
 * @brief Handles incoming HCI controller packet events.
 * Manages states like initialization, disconnection, encryption changes, and
 * connections.
 *
 * @param packet_type Class of the packet (e.g., HCI_EVENT_PACKET).
 * @param channel Connection channel number.
 * @param packet Pointer to the event packet buffer.
 * @param size Byte length of the packet.
 */
static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    hid_report_t dummy; // Temporary storage to empty report queue
    // Suppress unused parameter warnings
    (void)channel;
    (void)size;

    // We only care about HCI event packets in this handler
    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet); // Event class code

    // Filter out spammy events:
    // 0x13 = HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS
    // 0x0E = HCI_EVENT_COMMAND_COMPLETE
    // 0x6E = HCI_EVENT_TRANSPORT_PACKET_SENT (Spams heavily during CYW43 firmware
    // download)
    if (event_type != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS &&
        event_type != HCI_EVENT_COMMAND_COMPLETE && event_type != 0x6E) {
        DbgPrint("HCI_EVENT: 0x%02x\n", event_type);
    }

    // Parse the specific event type from the packet
    switch (event_type) {

    // Case: BTstack internal state has changed
    case BTSTACK_EVENT_STATE:
        // Start advertising only when the HCI controller is fully powered on and
        // working
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {

            DbgPrint("BLE Stack initialized and ready, advertising started\n");
            gap_advertisements_enable(1);
        }
        break;

    // Case: The Bluetooth connection was terminated
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        // Invalidate the connection handle
        con_handle = HCI_CON_HANDLE_INVALID;
        link_encrypted = false;
        // packet[5] contains the HCI disconnect reason (error code)
        uint8_t reason = packet[5]; // Disconnection error reason
        DbgPrint("Disconnected (reason: 0x%02x", reason);
        switch (reason) {
        case 0x13: // ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION
            DbgPrint(" - Remote User Terminated Connection");
            break;
        case 0x08: // ERROR_CODE_CONNECTION_TIMEOUT
            DbgPrint(" - Connection Timeout");
            break;
        case 0x3D: // ERROR_CODE_MIC_FAILURE
            DbgPrint(" - MIC Failure / Key Mismatch");
            break;
        case 0x06: // ERROR_CODE_PIN_OR_KEY_MISSING
            DbgPrint(" - PIN or Key Missing");
            break;
        }
        DbgPrint(")\n");

        // Empty the report queue to avoid sending stale inputs upon reconnection
        // Keep the loops short
        while (queue_try_remove(&report_queue, &dummy))
            ;

        // Restart advertisements so the host can automatically reconnect after
        // disconnection
        gap_advertisements_enable(1);
        break;

    // Case: Encryption state has changed
    case HCI_EVENT_ENCRYPTION_CHANGE:
        DbgPrint("  -> status=0x%02x, enc_enable=0x%02x\n", packet[2], packet[5]);
        if (packet[2] == ERROR_CODE_SUCCESS && packet[5] != 0) {
            DbgPrint("Connection encrypted\n");
            link_encrypted = true;
        } else {
            DbgPrint(
                "Encryption failed! (Link key might be missing, status: 0x%02x)\n",
                packet[2]);
        }
        break;

    // Case: BLE-specific meta events (e.g., connection established, parameters
    // updated)
    case HCI_EVENT_LE_META:
        DbgPrint("  -> subevent=0x%02x\n",
                 hci_event_le_meta_get_subevent_code(packet));
        switch (hci_event_le_meta_get_subevent_code(packet)) {

        // Sub-case: A new BLE connection has been successfully established
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            // Parse manually for consistency and to bypass potential BTstack macro
            // issues packet[3] is status, packet[4] and packet[5] form the connection
            // handle
            if (packet[3] != ERROR_CODE_SUCCESS) {
                DbgPrint("Connection failed\n");
                // Restart advertisements to allow a new connection attempt
                gap_advertisements_enable(1);
                break;
            }
            // Clear queue to remove any stale reports before setting connection
            // handle
            while (queue_try_remove(&report_queue, &dummy))
                ;
            // Store the connection handle for future communication (offset 4)
            con_handle = packet[4] | (packet[5] << 8);
            link_encrypted = false;
            bd_addr_t peer_addr; // Bluetooth address of target central peer
            reverse_bd_addr(&packet[8], peer_addr);
            uint8_t peer_addr_type = packet[7]; // Address format (random/public)
            DbgPrint("Connected to %s (type %d)\n", bd_addr_to_str(peer_addr),
                     peer_addr_type);

            // Actively request encryption (or pairing) start from Windows
            sm_request_pairing(con_handle);

            break;

        // Sub-case: A new BLE connection has been successfully established
        // (Enhanced Privacy)
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1:
            // Parse manually to bypass BTstack version-specific getter macro issues
            // packet[3] is status, packet[4] and packet[5] form the connection handle
            if (packet[3] != ERROR_CODE_SUCCESS) {
                DbgPrint("Connection failed (Enhanced)\n");
                // Restart advertisements to allow a new connection attempt
                gap_advertisements_enable(1);
                break;
            }
            // Clear queue to remove any stale reports before setting connection
            // handle
            while (queue_try_remove(&report_queue, &dummy))
                ;
            // Store the connection handle for future communication (offset 4)
            con_handle = packet[4] | (packet[5] << 8);
            link_encrypted = false;
            bd_addr_t peer_addr_enh; // Bluetooth address of target central peer
                                     // (enhanced details)
            reverse_bd_addr(&packet[8], peer_addr_enh);
            uint8_t peer_addr_type_enh = packet[7]; // Address format (enhanced)
            DbgPrint("Connected [Enhanced] to %s (type %d)\n",
                     bd_addr_to_str(peer_addr_enh), peer_addr_type_enh);

            // Actively request encryption (or pairing) start from Windows
            sm_request_pairing(con_handle);

            break;

        // Sub-case: The central device has updated the connection parameters
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            // Log the new connection interval to monitor performance
            DbgPrint(
                "Connection parameters updated: interval %u (x 1.25 ms)\n",
                hci_subevent_le_connection_update_complete_get_conn_interval(packet));
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

/**
 * @brief Handles incoming Security Manager events (pairing, confirmation
 * display, success/fail state).
 *
 * @param packet_type Class of packet.
 * @param channel Channel index.
 * @param packet Pointer to the event packet buffer.
 * @param size Byte size of the packet.
 */
static void sm_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet); // Event class code

    switch (event_type) {
    case SM_EVENT_JUST_WORKS_REQUEST:
        DbgPrint("Just Works requested\n");
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        DbgPrint("Confirming numeric comparison: %06" PRIu32 "\n",
                 sm_event_numeric_comparison_request_get_passkey(packet));
        sm_numeric_comparison_confirm(
            sm_event_numeric_comparison_request_get_handle(packet));
        break;

    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        DbgPrint("Display Passkey: %06" PRIu32 "\n",
                 sm_event_passkey_display_number_get_passkey(packet));
        break;

    case SM_EVENT_PAIRING_COMPLETE:
        {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            switch (status) {
            case ERROR_CODE_SUCCESS:
                DbgPrint("Pairing complete, success\n");
                break;
            case ERROR_CODE_CONNECTION_TIMEOUT:
                DbgPrint("Pairing failed, timeout\n");
                break;
            case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                DbgPrint("Pairing failed, disconnected\n");
                break;
            default:
                DbgPrint("Pairing failed, reason 0x%02x\n", status);
                break;
            }
            if (status != ERROR_CODE_SUCCESS) {
                bd_addr_t addr;
                sm_event_pairing_complete_get_address(packet, addr);
                uint8_t addr_type = sm_event_pairing_complete_get_addr_type(packet);
                gap_delete_bonding(addr_type, addr);
                DbgPrint("Deleted bonding for %s (type %d) due to pairing failure\n",
                         bd_addr_to_str(addr), addr_type);
            }
        }
        break;

    case SM_EVENT_AUTHORIZATION_REQUEST:
        DbgPrint("Authorization requested\n");
        sm_authorization_grant(sm_event_authorization_request_get_handle(packet));
        break;

    case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
        DbgPrint("Identity resolving succeeded\n");
        break;

    case SM_EVENT_IDENTITY_RESOLVING_FAILED:
        DbgPrint(
            "Identity resolving failed (Host used old pairing or IRK mismatch)\n");
        break;

    default:
        break;
    }
}

/**
 * @brief Handles incoming Attribute Protocol events (like CAN_SEND_NOW
 * notifications).
 *
 * @param packet_type Class of packet.
 * @param channel Channel index.
 * @param packet Pointer to the event packet buffer.
 * @param size Byte size of the packet.
 */
static void att_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET)
        return;

    if (hci_event_packet_get_type(packet) == ATT_EVENT_CAN_SEND_NOW) {
        typing_can_send_now();
    } else if (hci_event_packet_get_type(packet) ==
               ATT_EVENT_MTU_EXCHANGE_COMPLETE) {
        DbgPrint("ATT MTU Negotiated: %u bytes\n",
                 att_event_mtu_exchange_complete_get_MTU(packet));
    }
}
