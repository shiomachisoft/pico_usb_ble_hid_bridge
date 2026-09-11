# Pico W / Pico 2 W - USB to BLE HID Bridge

## Overview

This firmware allows the Raspberry Pi Pico W / Pico 2 W to operate as a "USB to BLE HID Bridge".  
*(Note: In this document, both boards are collectively referred to as "**Pico W**".)*  
By simply connecting a USB HID device (keyboard, mouse, gamepad, etc.) to the Pico W, you can use it as a BLE (Bluetooth Low Energy) device from a PC, tablet, smartphone, or other devices.

## System Configuration

<img width="827" height="460" alt="image" src="https://github.com/user-attachments/assets/df3899b8-a336-4d79-b9e0-1e1ae35f3c02" />

- **Special Notes**
  - **The standard USB connector on the Pico W is used exclusively for power supply (it is not used for USB communication).**
  - **USB communication with the USB device is performed through a software-implemented USB port by controlling GP0 and GP1 using PIO (Programmable I/O).**

### Hardware Connection

Below is a connection example when using the [Akizuki Denshi: AE-USB-A-DIP](https://akizukidenshi.com/catalog/g/g107429/) for the USB connector board (Type-A Female).

<img width="1024" height="477" alt="image" src="https://github.com/user-attachments/assets/d6162ca1-4a36-4a6a-9232-454f3f1f9e93" />

**Note:** Connect 27 ohm resistors between GP0/GP1 of the Pico W and D+/D- of the USB connector, respectively.

| Pico W Pin Name | Pin Number (Physical Pin) | USB Connector Board Pin |
| :--- | :--- | :--- |
| **GP0** | 1 | **D+** |
| **GP1** | 2 | **D-** |
| **GND** | 38 | **GND** |
| **VBUS** | 40 | **VBUS** |

> **Connecting Multiple Devices via USB Hub & Power Supply:**  
> 
> - **Connecting via Hub**: To connect multiple USB devices simultaneously, plug a standard USB hub into the USB connector board (Type-A Female) and connect your USB devices to the hub ports. **Please note that the number of simultaneously connected USB devices is generally limited to 2 devices** (e.g., a typical combination of 1 keyboard and 1 mouse).  
> - **Power Considerations**: If connecting power-hungry devices, or if you encounter instability/resets due to voltage drops, using an externally powered (**self-powered**) USB hub is strongly recommended.

## Features & Limitations

### Features
- **Broad Compatibility**: Recognized as a standard BLE HID device by PCs, tablets, and smartphones.
- **Versatile HID Device Support**: Supports keyboards, mice, gamepads, and other standard USB HID devices.
- **USB Hub Support**: Supports connecting multiple USB HID devices simultaneously via a USB hub (generally up to 2 devices).
- **Composite Device Support**: Supports composite HID devices (e.g., wireless keyboard + mouse combo dongles).
- **Persistent Bonding**: Pairing information is safely stored in non-volatile flash memory (Flash TLV), enabling automatic reconnection on subsequent boots.
    
### Limitations
- **Connected Device Limit**: While connecting multiple USB devices via a USB hub is supported, the number of simultaneously connected USB devices is **generally limited to 2 devices** (e.g., a typical combination of 1 keyboard and 1 mouse). Connecting 3 or more devices is not officially supported, although connecting up to 3 devices may work depending on the configuration.
- **Gamepad Mode**: Please set your gamepad to "DirectInput" mode. "XInput" mode is not supported.
- **No Hot-Plugging**: Hot-plugging of USB devices or USB hubs (connecting or disconnecting while powered on) is not supported. Always connect all USB devices and hubs before powering on the Pico W.

## Source Code & Binaries

The full source code for this program and the ready-to-flash binary (.uf2 file) are available in this repository:

- **Pre-built binaries**: Available under [`bin/`](bin/)
- **Build from source**: See [docs/build.md](docs/build.md) for build instructions using VS Code.

> **Note:**  
> The source code is written in C using the Pico SDK.

## Usage

### Flashing the Firmware

> **Note on Firmware Updates:**  
> When updating (reflashing) the firmware, it is recommended to remove **"USB-BLE HID Brg"** from the Bluetooth settings on your BLE host (PC, tablet, smartphone) beforehand as a precaution to prevent connection issues or stale GATT/bonding cache conflicts.

1. Connect the Pico W to your PC via USB while holding down the BOOTSEL button (the white button) so it is recognized as a mass storage drive (RPI-RP2 or RP2350).
2. Drag and drop the firmware (`pico_usb_ble_hid_bridge.uf2`) into the drive.

### Pairing

1. Connect each device as shown in the System Configuration diagram.
2. With the Pico W powered OFF, connect your USB device (keyboard, mouse, gamepad, etc.) to the USB connector (Type-A Female).  
   **When connecting multiple devices, connect a USB hub to the USB connector and plug all target devices into the hub beforehand.**
   
   > **Note:**  
   > - The number of simultaneously connected USB devices is generally limited to 2 devices (e.g., 1 keyboard and 1 mouse).
   > - If using a gamepad, please set it to "DirectInput" mode beforehand.
   > - Ensure all devices and the USB hub are connected **before** supplying power (hot-plugging is not supported).
     
3. Supply power to the Pico W's USB connector to turn it ON.
   - *In the standby state before a BLE connection is established, the onboard LED on the Pico W will **blink** (500ms intervals).*
4. Open the Bluetooth settings screen on your BLE host (PC, tablet, smartphone), search for **"USB-BLE HID Brg"**, and pair it.
   
   > **Note:**  
   > For Windows 11, please select the item indicated by the red frame in the figure below.

 <img width="492" height="566" alt="image" src="https://github.com/user-attachments/assets/396cd811-862d-4726-93bd-74e3d0864090" />

5. Once pairing is complete, the LED will **turn solid**, and you can now use the USB device.

   > **Regarding Reconnection:**  
   > - Once pairing is completed, the pairing keys are saved in Flash memory and it will automatically reconnect from the next time onwards.
   > - You do not need to perform the pairing operation on the BLE host (PC, tablet, smartphone) side again when reconnecting.

   > **Steps to Change the Connected USB Device(s):**  
   > The firmware dynamically parses the connected USB device(s) HID descriptor and constructs the BLE GATT database accordingly. Therefore, if you change, add, or remove connected USB devices (including devices connected to a USB hub), you must re-pair:
   > 1. Turn OFF the Pico W.
   > 2. Remove **"USB-BLE HID Brg"** from the Bluetooth pairing list on your BLE host (PC, tablet, smartphone).
   > 3. Connect or swap the new USB device(s) or USB hub.
   > 4. Perform the steps in "Pairing" again.
   > 
   > *(Note: Likewise, when updating or reflashing the firmware, it is recommended to delete the existing pairing information from your BLE host beforehand.)*

## Tested Devices

### USB Devices
- Mouse: ELECOM M-HC01UR
- Keyboard: ELECOM TK-FDM109T
- Wireless Keyboard & Mouse Combo: ELECOM TK-FDM078MBK (2.4GHz wireless via USB receiver)
- Gamepad: ELECOM GP20S
- USB Hub: BUFFALO BSH4A08U3BK

### BLE Hosts
- Windows 11 PC
- iPad 9th Gen (iPadOS 26.5)
- Pixel 8a (Android 16)

> **Note:**  
> - The gamepad has currently only been tested on Windows 11 PC.
> - Not all combinations of listed USB devices and BLE hosts are re-tested with every firmware release.

## Known Issues

Please refer to the [GitHub Issues (labeled "bug")](https://github.com/shiomachisoft/pico_usb_ble_hid_bridge/issues?q=is%3Aissue+label%3Abug).

## License

For details regarding the license of this software, please refer to the `LICENSE` file in the repository.

## Implementation Details

- **Dual-Core Task Distribution**:
  - **Core 0**: Dedicated to USB host processing (**Pico-PIO-USB** and TinyUSB). Isolating the timing-sensitive software USB communication onto Core 0 helps maintain responsive input capture and prevents packet loss.
  - **Core 1**: Handles the Bluetooth stack (**BTstack**) and the status LED timer, offloading wireless communication tasks from Core 0.
- **Dynamic BLE GATT Construction**:
  - Parses the connected device's USB Report Descriptor at boot and dynamically builds the matching BLE GATT database (HID Service). This allows support for various devices (keyboards, mice, gamepads, etc.) without requiring hardcoded descriptors or firmware re-compilation.
- **Direct HID Report Forwarding**:
  - Received USB HID input reports are forwarded directly to the BLE host as GATT notifications without modifying the payload, minimizing processing delay and preserving standard device functionality.

## Disclaimer
The author assumes no responsibility for any damages or issues arising from the content of this document or the use of this software. Please use it at your own risk.


