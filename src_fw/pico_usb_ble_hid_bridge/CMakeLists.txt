# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico_usb_ble_hid_bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(pico_usb_ble_hid_bridge)

# Project source files including application logic and Pico-PIO-USB drivers
target_sources(pico_usb_ble_hid_bridge PRIVATE
        main.c
        usb_hid_host.c
        ble_hid_periph.c
        Pico-PIO-USB/src/pio_usb.c
        Pico-PIO-USB/src/pio_usb_host.c
        Pico-PIO-USB/src/pio_usb_device.c
        Pico-PIO-USB/src/usb_crc.c
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
        )

target_compile_options(pico_usb_ble_hid_bridge PRIVATE
        -Wall
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-maybe-uninitialized
        )

# Make sure TinyUSB can find tusb_config.h
target_include_directories(pico_usb_ble_hid_bridge PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${CMAKE_CURRENT_SOURCE_DIR}/Pico-PIO-USB/src)

target_compile_definitions(pico_usb_ble_hid_bridge PRIVATE 
        CYW43_LWIP=0
        PICO_DEFAULT_UART=1
        PICO_DEFAULT_UART_TX_PIN=4
        PICO_DEFAULT_UART_RX_PIN=5
        MAX_ATT_DB_SIZE=8192
        )

# In addition to pico_stdlib required for common PicoSDK functionality, add dependency on tinyusb_host
# for TinyUSB device support and tinyusb_board for the additional board support library used by the example
target_link_libraries(pico_usb_ble_hid_bridge PRIVATE
        pico_stdlib 
        tinyusb_host 
        tinyusb_board
        pico_btstack_ble
        pico_btstack_cyw43
        pico_btstack_flash_bank
        pico_cyw43_arch_threadsafe_background	
        pico_multicore
        pico_flash
        hardware_pio
        hardware_dma
        )

pico_add_extra_outputs(pico_usb_ble_hid_bridge)
