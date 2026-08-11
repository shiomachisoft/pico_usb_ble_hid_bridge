# Pico W / Pico 2 W - USB to BLE HID Bridge

## 1. Overview

This firmware allows the Raspberry Pi Pico W / Pico 2 W to operate as a "USB to BLE HID Bridge".  
*(Note: In this document, both boards are collectively referred to as "**Pico W**".)*  
By simply connecting a USB HID device (keyboard, mouse, gamepad, etc.) to the Pico W, you can use it as a BLE (Bluetooth Low Energy) device from a PC, tablet, smartphone, or other devices.

## 2. System Configuration

### 2.1. Configuration Overview

<img width="827" height="460" alt="image" src="https://github.com/user-attachments/assets/df3899b8-a336-4d79-b9e0-1e1ae35f3c02" />

- **Special Notes**
  - **The standard USB connector on the Pico W is used exclusively for power supply (it is not used for USB communication).**
  - **USB communication with the USB device is performed through a software-implemented USB port by controlling GP0 and GP1 using PIO (Programmable I/O).**

### 2.2. Connection between Pico W and USB Connector Board (Type-A Female)

Below is a connection example when using the [Akizuki Denshi: AE-USB-A-DIP](https://akizukidenshi.com/catalog/g/g107429/) for the USB connector board (Type-A Female).

<img width="1024" height="477" alt="image" src="https://github.com/user-attachments/assets/37b2661b-cd10-452d-9856-b4e2c9bc50d1" />

| Pico W Pin Name | Pin Number (Physical Pin) | USB Connector Board Pin |
| :--- | :--- | :--- |
| **GP0** | 1 | **D+** |
| **GP1** | 2 | **D-** |
| **GND** | 38 | **GND** |
| **VBUS** | 40 | **VBUS** |

## 3. Features and Limitations

### 3.1. Features
- Recognized as a standard BLE HID device by PCs, tablets, and smartphones.
- Generally, there are no restrictions on the types of connectable USB HID devices; it supports various devices such as keyboards, mice, and gamepads.
    
### 3.2. Limitations
- Please set your gamepad to "DirectInput" mode. "XInput" mode is not supported.
- For composite USB devices (e.g., a single USB device that functions as both a keyboard and a mouse), only some functions may work.
- Hot-plugging of USB devices (connecting or disconnecting while powered on) is not supported.
- Do not connect a USB hub between the Pico W and the USB device.

## 4. Source Code and Binaries

The full source code for this program and the ready-to-flash binary (.uf2 file) are available in this repository:

> **Note:**  
> The source code is written in C using the Pico SDK.

## 5. Usage

### 5.1. Flashing the Firmware
1. Connect the Pico W to your PC via USB while holding down the BOOTSEL button (the white button) so it is recognized as a mass storage drive (RPI-RP2 or RP2350).
2. Drag and drop the firmware (`pico_usb_ble_hid_bridge.uf2`) into the drive.

### 5.2. Pairing

1. Connect each device as shown in the System Configuration diagram.
2. With the Pico W powered OFF, connect a USB device (keyboard, mouse, gamepad, etc.) to the USB connector (Type-A Female).
   
   > **Note:**  
   > If using a gamepad, please set it to "DirectInput" mode beforehand.
     
3. Supply power to the Pico W's USB connector to turn it ON.
   - *In the standby state before a BLE connection is established, the onboard LED on the Pico W will **blink**.*
4. Open the Bluetooth settings screen on your BLE host (PC, tablet, smartphone), search for "USB BLE HID Brg", and pair it.
   
   > **Note:**  
   > For Windows 11, please select the item indicated by the red frame in the figure below.

 <img width="492" height="566" alt="image" src="https://github.com/user-attachments/assets/396cd811-862d-4726-93bd-74e3d0864090" />

5. Once pairing is complete, the LED will **turn solid**, and you can now use the USB device.

   > **Regarding Reconnection:**  
   > - Once pairing is completed, it will automatically reconnect from the next time onwards.
   > - You do not need to perform the pairing operation on the BLE host (PC, tablet, smartphone) side again when reconnecting.

   > **Steps to Change the Connected USB Device:**  
   > If you want to change the connected USB device, please follow these steps:
   > 1. Turn OFF the Pico W.
   > 2. Remove "USB BLE HID Brg" from the pairing information on your BLE host (PC, tablet, smartphone).
   > 3. Connect the new USB device to the USB connector (Type-A Female).
   > 4. Perform the steps in "5.2. Pairing" again.

## 6. Verified Devices and Environments

### 6.1. Verified USB Devices
- Mouse: ELECOM M-HC01UR
- Keyboard: ELECOM TK-FDM109T
- Gamepad: ELECOM GP20S

### 6.2. Verified BLE Hosts
- Windows 11 PC
- iPad 9th Gen (iPadOS 26.5)
- Pixel 8a (Android 16)

> **Note:**  
> - The gamepad has currently only been tested on Windows11 PC.

## 7. License

For details regarding the license of this software, please refer to the `LICENSE` file in the repository.

## 8. Implementation Details

> **Note:**  
> This section will be added in the future.

## 9. Disclaimer
The author assumes no responsibility for any damages or issues arising from the content of this article or the use of this software. Please use it at your own risk.


