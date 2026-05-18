# ESP32 Airbnk Door Lock (BLE-to-MQTT Gateway)

ESP32-C3 native ESP-IDF firmware that bridges Airbnk smart locks (BLE) to MQTT,
designed to work with [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt)
Home Assistant integration.

Based on [formatBCE/Airbnk-MQTTOpenGateway](https://github.com/formatBCE/Airbnk-MQTTOpenGateway).

Built with **native ESP-IDF** — no Arduino.h, no NimBLE-Arduino, no ESPHome
framework dependencies. Uses NimBLE host stack directly via ESP-IDF APIs.

## Hardware

- ESP32-C3 (esp32-c3-devkitc-02)
- Airbnk lock (M300, M500, M510, M530, M531)

## Network

- Static IP: 192.168.7.50
- Gateway: 192.168.6.4
- MQTT broker: 192.168.6.11

## MQTT Topics

| Topic | Direction | Purpose |
|---|---|---|
| `tele/maindoor/adv` | ESP32 -> MQTT | Lock BLE advertisement data (MAC, RSSI, manufacturer data) |
| `tele/maindoor/command` | MQTT -> ESP32 | Lock/unlock commands from HA |
| `tele/maindoor/command_result` | ESP32 -> MQTT | Command execution result |
| `tele/maindoor/LWT` | ESP32 -> MQTT | Last Will and Testament (Online/Offline) |

## File Structure

```
├── CMakeLists.txt                 # Root ESP-IDF project
├── sdkconfig.defaults             # ESP-IDF config defaults
├── main/
│   ├── CMakeLists.txt             # Main component
│   ├── Kconfig.projbuild          # Airbnk Gateway Kconfig options
│   └── main.c                     # Entry point (WiFi init + gateway start)
├── components/
│   └── airbnk_gateway/            # BLE-to-MQTT gateway component (pure ESP-IDF)
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── airbnk_gateway.h   # Public API: start/stop + config struct
│       │   ├── airbnk_ble.h       # BLE subsystem (NimBLE-based scan/connect/command)
│       │   ├── airbnk_mqtt.h      # MQTT subsystem (publish adv/result, subscribe cmd)
│       │   └── airbnk_utils.h     # Hex conversion, MAC comparison helpers
│       └── src/
│           ├── airbnk_gateway.c   # Glue layer: BLE↔MQTT bridging
│           ├── airbnk_ble.c       # BLE implementation (raw NimBLE GAP/GATT)
│           ├── airbnk_mqtt.c      # MQTT implementation (esp_mqtt_client)
│           └── airbnk_utils.c     # Utility implementations
├── esp32-door-lock.yaml           # Alternative: ESPHome config (uses old airbnk-gateway.h)
├── components/
│   └── airbnk-gateway.h           # Old ESPHome component (Arduino/NimBLE-Arduino based)
├── secrets/
│   └── secrets.yaml.example       # Template for ESPHome secrets
├── .gitea/
│   └── workflows/
│       └── ci.yaml                # Gitea Actions CI
└── README.md
```

## Build (ESP-IDF)

```bash
# 1. Set up ESP-IDF environment
. ~/esp-idf/export.sh

# 2. Configure (set WiFi SSID/password, lock MAC, MQTT broker)
idf.py menuconfig
# → Airbnk Gateway Configuration
# → Component config → Wi-Fi

# 3. Build
idf.py build

# 4. Flash
idf.py -p /dev/ttyUSB0 flash monitor
```

### Default Configuration (sdkconfig.defaults)

- **Lock MAC:** `E4:E1:12:C7:5C:52`
- **MQTT broker:** `mqtt://192.168.6.11`
- **MQTT topic prefix:** `tele/maindoor`
- **ESP32 target:** `esp32c3`
- **BLE stack:** NimBLE (controller-only, BLE-only mode)

## Build (ESPHome — legacy)

The `esp32-door-lock.yaml` and `components/airbnk-gateway.h` provide an ESPHome-based
alternative that uses NimBLE-Arduino. This is the original approach that depends on
Arduino.h. The ESP-IDF native component in `components/airbnk_gateway/` is the
recommended path going forward.

## Integration

After flashing, configure [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt)
in Home Assistant:

1. Add the custom integration via HACS or manual copy
2. Enter your Airbnk/WeHere email
3. Enter verification code
4. For each lock, enter:
   - MQTT topic: `tele/maindoor`
   - Lock MAC address
