# Project: ESP32-S3 AMOLED Network Image Display

## 1. Project goal

Develop custom firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16 development board.

The device should function as a network-accessible image display. It should receive images over HTTP and render them on its 480×480 AMOLED display.

The firmware must be modular, maintainable and designed for future extensions (layers, animations, multiple image sources, WebSocket communication, etc.).

Do not implement future features prematurely. The initial goal is a reliable, minimal image display device.

## 2. Hardware

Target board: Waveshare ESP32-S3-Touch-AMOLED-2.16

Verified hardware information:

- MCU: ESP32-S3 (QFN56), revision 0.2
- CPU: Dual Core, 240 MHz
- Flash: 16 MB
- PSRAM: 8 MB (AP_3v3)
- Wi-Fi: 2.4 GHz
- Bluetooth: BLE 5
- Display: 2.16-inch AMOLED, 480×480, 16.7M colors
- USB: Native USB Serial/JTAG
- Power management: AXP2101

### Board research (2026-11-04)

Waveshare's product-specific documentation and first-party repository confirm:

- AMOLED controller: CO5300, 480 × 480, QSPI interface.
- Touch controller: CST9220 capacitive touch.
- Power-management IC: AXP2101.
- Board support component: `waveshare/esp32_s3_touch_amoled_2_16` (official examples declare version `^2.0.1`).
- Waveshare's ESP-IDF guide requires ESP-IDF 5.5 or newer. The first-party example repository reports its example projects tested on ESP-IDF 5.5.5 and 6.0.2; this supports using the project's planned 6.1 release.
- The documented display example calls `bsp_display_start()` from the Waveshare BSP and then runs LVGL. That is evidence for the vendor-supported bring-up path, but it does not establish a non-LVGL panel initialization sequence. This project will keep its no-LVGL requirement and inspect the BSP implementation before selecting the display driver integration.
- The published example uses a BOOT button for IMU recalibration. This does not establish PWR-button signaling. The AXP2101 short-press event path and safe PWR-button behavior remain unverified; do not implement AP toggling until they are confirmed from the schematic and PMU driver/example.

Primary sources:

- [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16)
- [Waveshare ESP-IDF guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/ESP-IDF)
- [Waveshare first-party examples and schematic](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
- [ESP-IDF 6.1 documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/)

The exact display GPIO mapping, QSPI panel initialization commands, touch GPIO/I2C mapping, PMU I2C/interrupt pins, PSRAM mode, and PWR-button event semantics have not yet been verified against the schematic and implementation sources. These remain prerequisites for hardware initialization and physical-input work; no pin assignments should be inferred from examples for other boards.

The board currently has factory firmware installed.

A complete 16 MB flash backup has already been created:
`backups/factory-backup.bin`

Its size was verified to be exactly 16,777,216 bytes.

The following security properties were verified through espefuse:

- Secure Boot disabled
- Flash Encryption disabled
- Download mode enabled
- USB Serial/JTAG enabled

**IMPORTANT:** Never erase or modify eFuses. Never enable Secure Boot or Flash Encryption. Do not perform any destructive operation without explicit approval.

## 3. Development environment

- ESP-IDF 6.1
- ESP-IDF tools installed through Espressif Installation Manager
- VS Code with ESP-IDF extension
- Windows development environment
- Firmware language: C/C++ using ESP-IDF
- Supporting development tools and future editor: Bun + TypeScript

Do not use Arduino framework.

Do not introduce Python-based application code. Python utilities bundled with ESP-IDF are acceptable when required by the official toolchain.

Use native ESP-IDF APIs wherever practical.

## 4. First task: hardware research

Before implementing anything, investigate the exact board.

Use the official Waveshare documentation and official example repositories:

https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16

https://github.com/waveshareteam

Identify and document:

- Exact AMOLED controller and initialization sequence
- Display interface and GPIO assignments
- Display driver and supported pixel formats
- Touch controller and GPIO assignments
- AXP2101 power management configuration
- PSRAM initialization and configuration
- Physical button assignments and behavior
- USB and boot configuration
- Any required board-specific initialization sequence

Prefer adapting proven Waveshare examples over implementing hardware initialization from scratch.

Do not guess GPIO assignments, display controller models or button behavior.

## 5. Display subsystem

Implement a display abstraction independent of the network and image decoding layers.

Responsibilities:

- Initialize the AMOLED display.
- Manage framebuffer memory.
- Support RGB565 as the internal rendering format.
- Display images at 480×480.
- Clear the display.
- Handle image updates without unnecessary flickering.

Use the available 8 MB PSRAM for large buffers when appropriate.

A full 480×480 RGB565 framebuffer requires 460,800 bytes.

Initial image fitting modes:

- `contain`: preserve aspect ratio, fit entire image, pad unused area.
- `cover`: preserve aspect ratio, crop excess content.
- `stretch`: resize to exactly 480×480.

Do not introduce LVGL unless there is a clear technical reason. Prefer a simple native display implementation initially.

## 6. Wi-Fi subsystem: AP + STA

The device must support simultaneous Wi-Fi Access Point and Station modes.

### Station mode (STA)

- Connect to an existing 2.4 GHz Wi-Fi network.
- Store credentials persistently in NVS.
- Automatically reconnect after connection loss.
- Obtain IP through DHCP.
- Expose the HTTP server through the STA interface.
- Support mDNS, preferably `esp32-display.local`.

### Access Point mode (AP)

- The device must be able to create its own Wi-Fi network.
- Default SSID: `ESP32-Display-XXXX`, where XXXX is derived from the device MAC address.
- Default IP: `192.168.4.1`.
- Use WPA2-PSK.
- Use a configurable, unique password.
- The HTTP server and web interface must be accessible through the AP.

### AP power-button control — IMPORTANT

The physical PWR button must control the AP state.

Required behavior:

- When AP is enabled, a short PWR button press disables the AP.
- When AP is disabled, a short PWR button press enables the AP.
- The STA connection must remain active when the AP is disabled.
- The display must remain operational.
- The HTTP server must remain accessible through STA when available.
- Enabling/disabling AP must not restart the entire device.
- Debounce button events.
- Avoid accidental repeated toggles from a single physical press.

**Hardware research is mandatory before implementing this.**

The PWR button may be connected to the AXP2101 power-management IC rather than directly to a normal GPIO. Investigate the board schematic and official examples to determine how short-press events are reported.

Do not accidentally trigger the AXP2101 hardware power-off behavior.

Do not assume that the BOOT or GPIO18 user button is the PWR button.

If the PWR button cannot safely be used for this purpose because of hardware limitations, explain the limitation and propose an alternative before implementing a workaround.

### Startup behavior

- Start the STA connection process at boot if credentials are configured.
- Start AP by default so that the device remains discoverable and configurable.
- A missing or unavailable STA network must not prevent AP operation.
- AP state may be controlled at runtime through the physical button.
- Avoid blocking the main application while connecting to Wi-Fi.

## 7. HTTP server and REST API

Implement one HTTP server accessible through both AP and STA interfaces.

Initially, REST is sufficient. Do not implement WebSocket yet, but keep the architecture extensible for future WebSocket communication.

Endpoints:

| Method | Endpoint | Description |
|---|---|---|
| GET | `/` | Serve the static web interface |
| GET | `/api/device` | Device information |
| GET | `/api/network` | STA/AP status and IP information |
| POST | `/api/wifi` | Configure STA credentials |
| DELETE | `/api/wifi` | Remove saved STA credentials |
| POST | `/api/image` | Upload and display an image |
| DELETE | `/api/image` | Clear the display |

### Image upload

Use binary HTTP request bodies.

Example:

`POST /api/image?fit=contain`

Supported content types:

- `image/jpeg`
- `image/png`
- `image/bmp`

Validate the content type and actual image signature.

Requirements:

- Enforce a configurable maximum upload size.
- Validate image dimensions before allocating large buffers whenever possible.
- Handle malformed and unsupported images gracefully.
- Return meaningful HTTP status codes and error messages.
- Avoid unnecessary copies of image data.
- Do not store uploaded images permanently unless explicitly requested.

Example successful response:

```json
{
  "success": true,
  "width": 480,
  "height": 480,
  "format": "jpeg",
  "fit": "contain"
}
```

### Network API

`GET /api/network` should return structured information about:

- STA connection state
- STA IP address
- STA RSSI
- AP enabled state
- AP IP address
- AP SSID

Never return Wi-Fi passwords through API responses.

## 8. Image decoding

Implement a separate image decoding subsystem.

Required formats:

1. JPEG
2. PNG
3. BMP

Convert decoded pixels into RGB565 for display.

Investigate suitable ESP-IDF-compatible libraries and decoder implementations before choosing dependencies.

Consider memory usage carefully, especially for PNG decoding.

Avoid allocating the entire compressed image and multiple full-size intermediate buffers unnecessarily.

Keep the decoder interface independent of the HTTP implementation.

## 9. Static web interface

Serve a small static web application directly from the device.

Initially, the interface should contain:

- Device status
- Network status
- Current IP addresses
- Image upload control
- Image preview
- Image fit selection (`contain`, `cover`, `stretch`)
- Upload button
- Clear display button
- Wi-Fi configuration form

Keep the frontend simple.

The eventual image editor will be a separate Bun + TypeScript application. It will upload its final rendered image through the REST API.

Do not implement the image editor now.

Do not couple the firmware to a particular frontend framework.

Store static assets in an ESP-IDF-compatible embedded filesystem or embedded resources, choosing the simplest reliable approach.

## 10. Architecture and project structure

Use clear separation of responsibilities.

Suggested structure:

```text
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.cpp
│   ├── system/
│   │   ├── config.cpp
│   │   └── storage.cpp
│   ├── network/
│   │   ├── wifi_manager.cpp
│   │   ├── http_server.cpp
│   │   └── mdns.cpp
│   ├── api/
│   │   ├── device_api.cpp
│   │   ├── network_api.cpp
│   │   ├── wifi_api.cpp
│   │   └── image_api.cpp
│   ├── display/
│   │   ├── display_driver.cpp
│   │   └── framebuffer.cpp
│   ├── image/
│   │   ├── image_decoder.cpp
│   │   ├── jpeg_decoder.cpp
│   │   ├── png_decoder.cpp
│   │   └── bmp_decoder.cpp
│   └── input/
│       └── power_button.cpp
└── README.md
```

This is a suggested structure, not a requirement to create empty files for every module immediately.

Use C++ where it improves encapsulation, but prefer straightforward ESP-IDF-compatible code over unnecessary abstraction.

## 11. Future extensions — do not implement yet

The architecture should allow future support for:

- Multiple image layers
- Layer ordering and positioning
- Alpha blending
- Animated images
- WebSocket communication
- Real-time display updates
- A separate web-based image editor
- Multiple image sources
- External automation and MES integration

Do not implement any of these in the initial version.

## 12. Development workflow

Work incrementally.

### Workspace bootstrap status (2026-11-04)

A minimal ESP-IDF project only emits a startup log and does not initialize hardware. The ESP-IDF 6.1 framework and tools are available after loading `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`. The scaffold builds successfully with the ESP32-S3 target using `idf.py build`. Its flash-size default and generated esptool arguments are set to the board's verified 16 MB; the generated default partition table is temporary and allocates a 1 MB factory app until an application-specific layout is designed. Generated `build/` and `sdkconfig` are ignored. The factory backup is present at `backups/factory-backup.bin` (16,777,216 bytes); it must remain untouched.

Before coding:

1. Inspect the existing workspace.
2. Inspect the ESP-IDF installation and VS Code configuration.
3. Research the exact Waveshare board.
4. Identify and document all hardware-specific dependencies.
5. Propose the initial project structure and partition layout.
6. Identify any potential hardware or memory constraints.

Then implement in small, independently testable stages:

### Stage 1 — Board initialization

- Create the ESP-IDF project.
- Initialize power management.
- Initialize PSRAM.
- Initialize the AMOLED display.
- Display a simple test pattern or text.

### Stage 2 — Physical input

- Identify the PWR button event.
- Implement safe AP toggle behavior.
- Verify that the display and STA connection remain operational.

### Stage 3 — Networking

- Implement AP + STA.
- Persist Wi-Fi credentials.
- Implement reconnect behavior.
- Implement mDNS.

### Stage 4 — HTTP server

- Implement the REST endpoints.
- Serve a minimal static page.
- Test access through both AP and STA.

### Stage 5 — Image display

- Implement JPEG decoding first.
- Implement image fitting modes.
- Add PNG and BMP.
- Test malformed images and memory limits.

### Stage 6 — Integration and documentation

- Verify the complete workflow.
- Document build, flash and recovery procedures.
- Document API endpoints and example curl commands.

## 13. Critical development rules

- Do not erase the existing flash before the initial project has been reviewed and the backup is safely stored.
- Never modify eFuses.
- Never enable Secure Boot or Flash Encryption.
- Do not overwrite the factory backup.
- Do not assume hardware pin assignments.
- Do not use blocking operations in the main application loop.
- Avoid unnecessary dynamic memory allocations.
- Handle network disconnects and image decoding errors gracefully.
- Keep all hardware-specific code isolated.
- Build and test after each stage.
- Explain architectural decisions and significant dependencies.
- Do not silently skip failed tests.

## 14. First response expected from the coding agent

Do not immediately implement the entire firmware.

Start by:

1. Inspecting the workspace and ESP-IDF environment.
2. Researching the official Waveshare board documentation and examples.
3. Identifying the display controller, touch controller, PMIC and PWR button implementation.
4. Confirming the exact hardware initialization approach.
5. Reporting the proposed architecture, dependencies and implementation stages.
6. Highlighting any uncertainties, especially around PWR button behavior.

Wait for approval before performing the first firmware flash.
