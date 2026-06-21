/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "debug_print.h"
#include <stdio.h>

#include "bsp/board_api.h"
#include "btstack_run_loop.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"

// System clock frequency required for Pico-PIO-USB stability (120MHz)
#define SYS_CLOCK_KHZ 120000

// LED blinking interval when Bluetooth is not connected (ms)
#define LED_BLINK_INTERVAL_MS 500

// LED check interval when Bluetooth is connected (ms)
#define LED_CONNECTED_CHECK_INTERVAL_MS 100

// Semaphore used for secure cross-core synchronization.
static semaphore_t bt_init_sem;

// Timer for LED blinking on Core 1
static btstack_timer_source_t led_timer;

// External functions provided by the BLE peripheral stack
extern bool ble_hid_is_connected(void);
extern void ble_hid_periph_init_flash(void);

// Static function prototypes
static void core1_entry(void);
static void led_timer_handler(btstack_timer_source_t *ts);

/**
 * @brief Main entry point of the application running on Core 0.
 * Sets the system clock, initializes standard I/O and board hardware,
 * boots Core 1 to handle Bluetooth/BLE, configures and starts TinyUSB
 * in USB host mode.
 *
 * @return int Returns 0 on completion (never reached in normal operation).
 */
int main(void) {
    // Set system clock to 120MHz for stable Pico-PIO-USB communication
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // Initialize standard board hardware (e.g., standard I/O for printf)
    board_init();

    // Explicitly initialize standard I/O (UART) for Pico SDK
    stdio_init_all();

    // Initialize the semaphore for cross-core synchronization
    sem_init(&bt_init_sem, 0, 1);

    DbgPrint("  Pico W USB to BLE HID Bridge Started\n");

    // Initialize safe execution on Core 0 during Core 1 flash writes
    // Prevents crashes if the BLE stack writes to flash (e.g., saving bonding
    // keys)
    flash_safe_execute_core_init();

    // Launch the Bluetooth stack on Core 1
    multicore_launch_core1(core1_entry);

    // Wait on Core 0 until Core 1 has finished initializing Bluetooth
    sem_acquire_blocking(&bt_init_sem);

    // Configure Pico-PIO-USB pins
    // Use default PIO USB pins (D+ = GPIO0, D- = GPIO1) since UART is remapped to
    // GPIO4/5 in CMake
    pio_usb_configuration_t pio_cfg =
        PIO_USB_DEFAULT_CONFIG; // Configuration options for Pico-PIO-USB hardware

    // Dynamically determine which PIO instance (0 or 1) has free state machines
    // using pio_claim_unused_sm. Since we do not need to consider PIO exhaustion,
    // we assume at least one instance has a free state machine.
    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio0, false);
    if (sm < 0) {
        pio = pio1;
        sm = pio_claim_unused_sm(pio1, true);
    }
    pio_sm_unclaim(pio, sm);

    uint8_t pio_num = (pio == pio0) ? 0 : 1;
    pio_cfg.pio_tx_num = pio_num;
    pio_cfg.pio_rx_num = pio_num;

    // Apply the PIO USB configuration to TinyUSB
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION,
                  &pio_cfg);

    // Initialize the TinyUSB host stack on the configured root hub port
    tuh_init(BOARD_TUH_RHPORT);

    DbgPrint(
        "TinyUSB Host initialized successfully. Waiting for USB devices...\n");

    // Optional post-TinyUSB initialization hook if provided by the BSP
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    // Main infinite loop running on Core 0 (handles USB tasks and LED blinking)
    while (1) {
        // Service the TinyUSB host background task
        tuh_task();
    }

    return 0;
}

//--------------------------------------------------------------------+
// Core 1 Tasks
//--------------------------------------------------------------------+
/**
 * @brief Entry point for Core 1, dedicated to running the Bluetooth stack
 * (cyw43 and BTstack). Initializes the Wi-Fi/Bluetooth chip, invokes flash
 * init, sets up the LED blinking timer, and enters the BTstack execution run
 * loop.
 */
static void core1_entry(void) {
    // Initialize the CYW43 architecture (handles Wi-Fi/Bluetooth on Pico W)
    if (cyw43_arch_init() != 0) {
        DbgPrint("failed to initialise cyw43_arch\n");
        return;
    }

    // Initialize the flash TLV bank safely at boot time (when USB host is
    // inactive)
    ble_hid_periph_init_flash();

    // Setup LED blinking timer on the BTstack thread (Core 1)
    btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
    btstack_run_loop_set_timer(&led_timer, LED_BLINK_INTERVAL_MS);
    btstack_run_loop_add_timer(&led_timer);

    // Mark Bluetooth as initialized so Core 0 can proceed
    sem_release(&bt_init_sem);
    // Enter the BTstack infinite run loop. This function never returns.
    btstack_run_loop_execute();
}

//--------------------------------------------------------------------+
// Blinking Task
//--------------------------------------------------------------------+
/**
 * @brief Timer handler executed on Core 1 to blink the onboard LED safely.
 * LED remains solid ON if a BLE connection is active, otherwise blinks at 500ms
 * intervals.
 *
 * @param ts Pointer to the BTstack timer source.
 */
static void led_timer_handler(btstack_timer_source_t *ts) {
    static bool led_state =
        false; // State of the LED (true for ON, false for OFF)
    static bool was_connected =
        false; // Previous connection state to detect state changes

    bool is_connected = ble_hid_is_connected(); // Current connection state

    // If a BLE host is connected, keep the LED solid ON
    if (is_connected) {
        if (!was_connected) {
            cyw43_arch_lwip_begin();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            cyw43_arch_lwip_end();
            was_connected = true;
        }
        // Re-check frequently to quickly detect disconnection
        btstack_run_loop_set_timer(ts, LED_CONNECTED_CHECK_INTERVAL_MS);
        btstack_run_loop_add_timer(ts);
        return;
    }

    if (was_connected) {
        // Just disconnected: reset state to start blinking from OFF
        was_connected = false;
        led_state = false;
    } else {
        // Toggle the LED state
        led_state = !led_state;
    }

    // Update the LED hardware state safely on Core 1
    cyw43_arch_lwip_begin();
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
    cyw43_arch_lwip_end();

    // Blink interval is LED_BLINK_INTERVAL_MS
    btstack_run_loop_set_timer(ts, LED_BLINK_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}
