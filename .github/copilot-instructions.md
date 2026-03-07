---
description: "Instructions for developing the Generic Switch Monitor ESP8266 IoT project"
---

# Generic Switch Monitor Workspace Instructions

## Project Overview
This is a battery-operated IoT sensor device for monitoring up to 11 GPIO pins on an ESP8266 microcontroller and reporting their state via MQTT. Built with PlatformIO (Arduino framework) for ultra-low power consumption through aggressive deep sleep cycles.

## Build and Run Commands
- Build firmware: `pio run`
- Upload to device: `pio run --target upload`
- Upload web pages: `pio run --target uploadfs`
- Serial monitor: `pio device monitor --baud 115200`

Configuration via serial (115200 baud), web UI, or MQTT commands.

## Architecture
- Microcontroller: ESP8266-01S (1MB flash, LittleFS)
- Power: Deep sleep 99% of time; wakes every N seconds or on GPIO reset pulse
- Networking: ESPAsyncWebServer + Arduino WiFi stack, PubSubClient for MQTT
- Storage: EEPROM for settings, LittleFS for web UI
- GPIO: Pins 0–5 and 12–16 (11 total), individually configurable with pull-ups

## Code Conventions
- Single-file architecture in main.cpp (~2000 lines)
- C-style structs and global state, no OOP
- Fixed-size char arrays with null-termination guards
- Simple strtok() command parsing
- Custom port indexing for non-contiguous GPIOs

## Pitfalls and Gotchas
- **GPIO16 bodge required**: Wire GPIO16 to reset pin for deep sleep
- Boot-state requirements: GPIO0 & GPIO2 must be high at boot
- Power monitoring: ADC in VCC mode with calibration constants
- Command format: `name=value` no spaces
- Topic root must end with "/"

## Key Files
- [src/main.cpp](src/main.cpp): Core logic
- [include/switchMonitor.h](include/switchMonitor.h): Macros and constants
- [data/index.html](data/index.html): Web UI template
- [platformio.ini](platformio.ini): Build configuration

## Web UI Configuration
The device provides a web-based configuration interface accessible at `http://[mdnsname].local` (e.g., `http://mousetrap.local`) or `http://192.168.4.1` in AP mode. The page is generated from [data/index.html](data/index.html) with placeholders replaced by current settings.

### Sections:
- **WiFi and Network**: SSID, password, static IP, netmask
- **MQTT**: Broker address, port, topic root (must end with "/"), user, password
- **Controls**: Debug flag, report interval (seconds), mDNS name
- **Monitored Ports**: Table for each GPIO (0,1,2,3,4,5,12,13,14,15,16) with:
  - Checkbox to enable monitoring
  - GPIO number and pin name
  - Custom MQTT message for high/low states
  - Pull-up resistor option
  - Notes on boot requirements and alternative functions (e.g., GPIO0/2 must be high at boot, GPIO1/3 disable serial if used)

Changes are saved via POST to `/save`, and the page shows a success message. JavaScript enables the save button only when fields are modified.

## Development Tips
- Update VERSION macro manually after changes
- Test with serial monitor first
- Ensure hardware bodge before deep-sleep testing