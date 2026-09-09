# Build Instructions

This document describes how to build the firmware for Raspberry Pi Pico W / Pico 2 W.

## Prerequisites

- **OS**: Windows
- **Editor**: Visual Studio Code (VS Code)
- **VS Code Extension**: [Raspberry Pi Pico](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)

> [!NOTE]
> Tested and confirmed to work with the following versions:
> - VS Code: `v1.136.1`
> - Raspberry Pi Pico extension: `v0.23.0`
> - Pico SDK: `v2.2.0`

## Build Steps

1. Open a new VS Code window.
2. Click "Import Project" via the Raspberry Pi Pico extension.
3. Select the "src" folder for "Location".
4. Select "v2.2.0" for "Select Pico SDK version".
5. Click "Import".
6. Select "pico_w" or "pico2_w" as needed for "Switch Board".
7. Select "Release" or "Debug" as needed for "Switch Build Type".
8. Click "Compile Project" to start the build.

> [!WARNING]
> The build will fail if the project path contains Japanese (full-width) characters.
