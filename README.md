# 🔥 FireLine ESP32 Firmware (`v1.2.0-esp32`)

Autonomous edge safety, gas/smoke/thermal sensing, GSM SMS alerting, and cloud telemetry firmware for the **FireLine Gas & Fire Safety Monitor**.

Built for **ESP32 (NodeMCU / ESP32-WROOM-32)** using the Arduino framework and FreeRTOS multi-core task isolation so that life-safety sensor evaluation and solenoid valve shutoff **never block** on Wi-Fi, HTTPS, or cellular modem operations.

---

## 🏗️ Firmware Architecture & Dual-Core Task Isolation

The firmware separates real-time hardware safety from network and cellular I/O across the ESP32's two CPU cores:

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         ESP32 Dual-Core Runtime                         │
├───────────────────────────────────┬─────────────────────────────────────┤
│           CORE 1 (App)            │            CORE 0 (Pro)             │
│   loop() — Real-Time Safety Loop  │  Background Communication Tasks     │
│   • Hardware Watchdog (10s WDT)   │                                     │
│   • MQ-2 10x Oversampled ADC      │  1. networkTask (Stack: 16 KB)      │
│   • DHT11 Temp & Humidity Polling │     • Captive Wi-Fi Setup Portal    │
│   • Hysteresis Threshold Engine   │     • POST /api/readings (every 3s) │
│   • Immediate Relay Valve Shutoff │     • GET  /api/commands            │
│   • Serial CLI Command Handler    │     • GET  /api/config   (every 60s)│
│                                   │                                     │
│         │                         │  2. smsTask (Stack: 4 KB)           │
│         └───► FreeRTOS Queue ─────┼───► • SIM800L AT Command Driver     │
│               (smsQueue)          │     • Siren & Emergency SMS Retry   │
└───────────────────────────────────┴─────────────────────────────────────┘
```

### Key Safety Guarantees
1. **Relay-First Action:** On any gas, temperature, or persistent sensor fault condition, the LPG solenoid valve relay is driven to `RELAY_CLOSE_LEVEL` **first** before queuing telemetry or SMS dispatches.
2. **Fail-Closed Boot & Watchdog:** The relay pin is initialized to the closed state at boot, held closed throughout the 3-minute MQ-2 warm-up period, and guarded by a 10-second hardware Task Watchdog Timer (`esp_task_wdt`).
3. **No Automatic Reopen:** Once shut off, the valve **never** reopens automatically. A remote `open_valve` command is only accepted after all sensors are valid, no danger is active, warm-up is complete, and readings have remained continuously safe for `SAFE_CLEAR_MS` (30 seconds).

---

## 🔌 Hardware Pinout & Wiring

| Component | ESP32 Pin | Direction | Details / Notes |
| :--- | :--- | :--- | :--- |
| **MQ-2 Gas & Smoke Sensor** | `GPIO 34` | Input (ADC1) | Analog output (`AOUT`), 12-bit ADC (`0–4095`), `ADC_11db` attenuation |
| **DHT11 Temp & Humidity** | `GPIO 4` | Digital I/O | Digital data pin (pull-up required if using bare sensor) |
| **LPG Solenoid Valve Relay** | `GPIO 26` | Output | `HIGH` = Close Valve (`RELAY_CLOSE_LEVEL`), `LOW` = Open Valve (`RELAY_OPEN_LEVEL`) |
| **SIM800L GSM Modem (RX)** | `GPIO 25` | Input (UART2 RX) | Connects to **SIM800L TX** (`9600` baud, `8N1`) |
| **SIM800L GSM Modem (TX)** | `GPIO 33` | Output (UART2 TX) | Connects to **SIM800L RX** (use voltage divider if needed for 2.8V–3.3V logic) |
| **Wi-Fi Portal / Reset Button** | `GPIO 0` | Input (Pull-up) | Onboard `BOOT` button (Hold **3s** = Setup AP, Hold **7s** = Factory Wi-Fi Wipe) |

> [!IMPORTANT]
> - Verify your relay module's active polarity with the **gas supply disconnected** before installation. If using an active-low relay board where `LOW` cuts valve power, swap `RELAY_CLOSE_LEVEL` and `RELAY_OPEN_LEVEL` in [`src/main.cpp`](./src/main.cpp).
> - Power the **SIM800L** with a dedicated buck converter capable of delivering **2A peak current** at `3.7V–4.2V` with common `GND` shared with the ESP32.

---

## 🛡️ Safety Thresholds & Timing Constants

Configured in [`src/main.cpp`](./src/main.cpp) (dynamically overridable via cloud `GET /api/config`):

| Constant / Variable | Default Value | Description |
| :--- | :--- | :--- |
| `gasAdcDangerThreshold` | `50` (raw ADC) | Trips gas danger when oversampled 12-bit ADC reading `>= threshold` |
| `GAS_CLEAR_RATIO` | `0.90` (`90%`) | Hysteresis: gas danger clears only when ADC drops below `90%` of threshold |
| `tempDangerThreshold` | `50.0 °C` | Trips thermal danger when DHT11 temperature `>= threshold` |
| `TEMP_CLEAR_DELTA_C` | `2.0 °C` | Hysteresis: thermal danger clears at `threshold - 2.0 °C` |
| `WARMUP_MS` | `180,000 ms` (3 min) | Initial MQ-2 heater stabilization window; valve stays locked closed |
| `SAFE_CLEAR_MS` | `30,000 ms` (30 sec) | Continuous safe period required before remote valve reopening is allowed |
| `SMS_COOLDOWN_MS` | `300,000 ms` (5 min) | Minimum interval between primary emergency contact SMS alerts |
| `FAULT_SMS_DELAY_MS` | `60,000 ms` (1 min) | Duration a sensor fault must persist before dispatching a fault warning SMS |
| `WDT_TIMEOUT_S` | `10 sec` | Hardware watchdog timeout on Core 1 safety loop |

---

## 📡 Wi-Fi Provisioning & Captive Setup Portal (`WifiPortal.h`)

The firmware includes a self-contained captive portal and persistent credential manager ([`src/WifiPortal.h`](./src/WifiPortal.h)) using ESP32 NVS Flash (`Preferences` namespace `"wifi"`):

### How to Trigger Setup Mode
1. **Automatic Fallback:** If no Wi-Fi is saved, or if the configured network is unreachable for **2 minutes**, the ESP32 automatically launches the `FireMonitor-Setup` access point.
2. **Hardware BOOT Button (`GPIO 0`):**
   - **Hold for 3 seconds:** Opens the `FireMonitor-Setup` captive portal AP.
   - **Hold for 7+ seconds:** Erases saved Wi-Fi credentials from flash and launches the captive portal.
3. **Serial Monitor Command:** Send `wifi` (open portal) or `resetwifi` (erase & open portal).
4. **Remote Cloud Command:** Issue `start_portal` or `reset_wifi` from the FireLine PWA dashboard.

### Connecting to the Captive Portal
1. Connect your phone or laptop to the **`FireMonitor-Setup`** Wi-Fi network.
2. Your browser will automatically redirect to **`http://192.168.4.1`** (or open it manually).
3. Select a scanned 2.4 GHz network (or enter a hidden SSID), enter the password, and click **Save & Connect to Wi-Fi**.
4. The ESP32 closes the AP to eliminate radio channel conflicts, tests the connection for up to 20 seconds, and automatically reopens the portal if authentication fails.

---

## ☁️ Cloud API Protocol

The `networkTask` communicates with the FireLine Express backend over HTTPS (`API_BASE_URL` in [`src/FireApi.h`](./src/FireApi.h)):

### 1. Telemetry Ingestion (`POST /api/readings`)
Sent every **3 seconds** (`READING_INTERVAL_MS`) or immediately (`urgentSend = true`) when danger is triggered:
```json
{
  "gas": 42,
  "gas_adc": 42,
  "smoke": 4.2,
  "temperature": 27.5,
  "humidity": 61.0,
  "gas_sensor_valid": true,
  "temp_sensor_valid": true,
  "danger_detected": false,
  "valve_closed": true,
  "warming_up": false,
  "sms_last_status": "none",
  "rssi": -58,
  "uptime": 340,
  "firmware_version": "v1.2.0-esp32"
}
```

### 2. Cloud Threshold Sync (`GET /api/config`)
Polled on boot and every **60 seconds** (`CONFIG_INTERVAL_MS`). Updates `gasAdcDangerThreshold` (`gas_adc_danger_threshold` or `gas_danger_threshold`) and `tempDangerThreshold` (`temp_danger_threshold`).

### 3. Remote Actuator & Device Commands (`GET /api/commands` & `POST /api/commands/:id/result`)
Polled after every telemetry cycle. Supported commands:

| Command Name | Action | Safety Check / Notes |
| :--- | :--- | :--- |
| `close_valve` | Immediately shuts the LPG solenoid valve | Always executed |
| `open_valve` | Opens the LPG solenoid valve | **Rejected** if warming up, sensor fault, danger active, or safe `< 30s` |
| `send_sms` | Queues an SMS via SIM800L (`payload.phone`, `payload.message`) | Normalizes `09...` numbers to `+63...` |
| `start_portal` | Opens the `FireMonitor-Setup` Wi-Fi portal AP | Non-destructive |
| `reset_wifi` | Wipes stored Wi-Fi credentials from NVS flash and opens portal AP | Sets `cleared = true` so compile-time fallback is bypassed |
| `calibrate_sensor` | Acknowledges clean-air calibration request from dashboard | Returns `"executed"` |
| `reboot_device` | Reports `"executed"` to API and restarts the ESP32 (`ESP.restart()`) | Valve boots closed |

---

## 💻 Serial Monitor CLI (`115200` Baud)

Connect via USB Serial (`115200` baud, newline terminated) to inspect status or configure the device offline:

| Command | Description |
| :--- | :--- |
| `status` | Prints live sensor readings, valve state, Wi-Fi IP/RSSI, and setup portal state |
| `setwifi=SSID,PASS` | Saves new Wi-Fi credentials directly to flash and connects |
| `wifi` or `portal` | Launches the `FireMonitor-Setup` captive portal AP (`192.168.4.1`) |
| `resetwifi` | Erases saved Wi-Fi credentials from flash and launches the setup portal |
| `phone=+639XXXXXXXXX` | Saves the default emergency SMS recipient phone number to NVS flash |
| `testsms` | Queues a test SMS (`"TEST: Gas monitor SMS check."`) to the saved phone number |
| `testsiren` | Sends the activation SMS (`1234#ON#`) to the remote siren unit |
| `sirenoff` | Sends the deactivation SMS (`1234#OFF#`) to the remote siren unit |
| `reboot` | Restarts the ESP32 |
| `help` or `?` | Lists all available serial commands |

---

## 🚀 Building & Flashing

### Option A: PlatformIO (Recommended)

1. Install [PlatformIO IDE](https://platformio.org/) (VS Code extension) or PlatformIO Core CLI (`pio`).
2. Configure compile-time defaults in [`src/FireApi.h`](./src/FireApi.h):
   ```cpp
   #pragma once

   #define WIFI_SSID         "Your_2.4GHz_WiFi_SSID"
   #define WIFI_PASS         "Your_WiFi_Password"

   #define API_BASE_URL      "https://your-fireline-backend.onrender.com/api"

   #define DEFAULT_SMS_PHONE "+639XXXXXXXXX"
   ```
3. Build, upload, and open the serial monitor:
   ```bash
   # Build firmware
   pio run

   # Upload to connected ESP32
   pio run --target upload

   # Open Serial Monitor (115200 baud)
   pio device monitor
   ```

### Option B: Arduino IDE

1. Install the **esp32 by Espressif Systems** board package in Arduino Boards Manager and select **ESP32 Dev Module**.
2. Install the required libraries from Arduino Library Manager (matching [`platformio.ini`](./platformio.ini)):
   - `ArduinoJson` by Benoit Blanchon (`^7.0.4`)
   - `DHT sensor library` by Adafruit (`^1.4.6`)
   - `Adafruit Unified Sensor` by Adafruit (`^1.1.14`)
3. Copy the files in `src/` (`main.cpp` renamed to `firmware.ino`, `FireApi.h`, and `WifiPortal.h`) into a sketch folder and upload at `921600` upload speed / `115200` serial monitor baud.

---

## 📁 Project Structure

```text
firmware/
├── platformio.ini       # PlatformIO board, baud rate, and library dependencies
├── README.md            # Firmware documentation
└── src/
    ├── main.cpp         # Core 1 safety loop, Core 0 HTTP/SMS tasks, Serial CLI
    ├── WifiPortal.h     # Captive Wi-Fi setup portal & NVS credential manager
    └── FireApi.h        # Default Wi-Fi SSID/password, API URL, and SMS phone
```
