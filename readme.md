# ESP32-C3 Scrolling LED Matrix Display

Firmware for a **side-scrolling LED matrix ("subway sign")** driven by an ESP32-C3.  
Supports **Wi-Fi configuration, text updates, and OTA firmware/filesystem updates** via a built-in web UI.

The Scrolling LED Matrix Display is a modular 7-row × 420-column LED panel, made from chained 60×7 matrix assemblies (there was 8 originally, but one was removed). It works like the scrolling information signs used in subway stations, continuously moving text or messages across the display from right to left.

---

## Hardware

- **LED Matrix**: 7 rows × 420 cols (7 chained modules)
- **ESP32-C3** via PlatformIO / Arduino framework
- **Pinout (to LED matrix boards)**:
  | Pin | Function       |
  |-----|----------------|
  | 1   | 5V supply      |
  | 2   | `clk` (shift clock) |
  | 3   | `latch` (shift → output) |
  | 4   | `R0` (row select bit 0) |
  | 5   | `R1` (row select bit 1) |
  | 6   | `R2` (row select bit 2) |
  | 7   | `/OE` (output enable, 300 µs on per row) |
  | 9   | `data` (shift register input) |
  | 10  | GND |

The ESP32 drives the rows sequentially at **~60 FPS** using a timer interrupt.

---

## Features

- **Wi-Fi modes**:
  - Boots in **AP mode** (`ScrollingDisplay` / `12345678`) at `192.168.0.1`
  - Attempts to connect to saved STA Wi-Fi after boot
  - Disconnects AP after 5 minutes, if no connection
- **Web UI** (served from `/data/index.html` in LittleFS):
  - Set display text and scroll delay  
  - Configure Wi-Fi (SSID, password, AP credentials, mDNS hostname)  
  - Upload **firmware** or **filesystem** updates
- **Persistent settings** stored in LittleFS (`/settings.json`)
- **mDNS**: device available at `http://scrollingdisplay.local/` (if STA connected)
- **Fallback text if no saved config** - shows system info (RAM/Flash usage stats on boot)

---

## Build & Upload

### Build
1. Clone this repo and open in VS Code
2. Install PlatformIO plugin
3. Select Build and Build Filesystem image commands, respectively

### Upload
#### If you are uploading firmware to the device for the first time
Upload firmware and filesystem via USB connection, using the standard PlatformIO commands. 

#### If you have uploaded this firmware to the device before, and wish to do it over the air (OTA)
1. Connect to device via 
    * AP, within 5 minutes of bootup 
        * *default SSID: ScrollingDisplay*
        * *default password: 12345678*
        * Go to http://192.168.0.1
    * Via your LAN, if you configured it and it has connected
        * Go to either [scrollingdisplay.local]() (default), or whatever hostname you configured,
        * Or access via it's IP address, which you could find from your router
2. Once the configuration page has loaded, you can upload the firmware and/or the filesystem images.


## Refs
The previous controller used micropython on ESP8266, and can be seen here: https://github.com/pelrun/signmatrix. Driver timings (e.g. enable duty cycle and frame rate) were measured from hardware running that code, otherwise there is no commonality between that code and this code.