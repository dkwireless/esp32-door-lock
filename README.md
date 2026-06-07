# ESP32 Airbnk Door Lock — ESPHome BLE-MQTT Gateway

ESPHome external component for ESP32-C3 that bridges Airbnk smart locks
over BLE to MQTT. Works with the
[rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt)
Home Assistant integration.

> Based on [formatBCE/Airbnk-MQTTOpenGateway](https://github.com/formatBCE/Airbnk-MQTTOpenGateway)
> — ported from NimBLE-Arduino/CustomMQTTDevice to native ESPHome 2026.5+
> with standalone ESP-IDF NimBLE (no Arduino.h, no ble_client).

## Architecture

```
Airbnk Lock  ──BLE──>  ESP32-C3  ──MQTT──>  Home Assistant
                          │                    (rospogrigio/
                     ads: /adv              airbnk_mqtt)
                     cmds: /command
                     result: /command_result
```

The ESP32 is a **dumb passthrough** — no crypto, no key generation.
It forwards BLE advertisements to MQTT and relays lock/unlock commands
from HA to the lock via BLE GATT.

## Hardware

- ESP32-C3 board (esp32-c3-devkitm-1 or similar)
- Airbnk smart lock (M300, M500, M510, M530, M531)

## Prerequisites

- ESPHome 2026.5+ (`pip install esphome`)
- MQTT broker (Mosquitto or similar)
- HA custom integration:
  [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt)

## Quick Start

### 1. Clone & configure

```bash
git clone https://github.com/dkwireless/esp32-door-lock.git
cd esp32-door-lock
```

### 2. Set up secrets

```bash
cp secrets.yaml.example secrets.yaml
nano secrets.yaml
```

Add your WiFi credentials, MQTT credentials, and OTA password.

### 3. Configure the lock MAC

Edit `esp32-door-lock.yaml` and change `lock_mac` to your lock's
BLE MAC address.

```yaml
substitutions:
  lock_mac: "E4:E1:12:C7:5C:52"   # ← change this
```

### 4. Deploy

**Option A — standalone ESPHome (CLI):**

```bash
esphome compile esp32-door-lock.yaml
esphome upload esp32-door-lock.yaml
```

**Option B — ESPHome add-on in Home Assistant:**

Copy the files into the ESPHome config directory on your HA machine.

```bash
# From the repo root:
cp esp32-door-lock.yaml /path/to/ha/config/esphome/
cp -r components/airbnk_gateway/ /path/to/ha/config/esphome/components/
```

Then open the ESPHome add-on UI, you should see "esp32-door-lock"
in the list. Click Install.

### 5. Configure Home Assistant

1. Install the
   [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt)
   custom integration ([HACS](https://hacs.xyz) or
   [manual install](https://github.com/rospogrigio/airbnk_mqtt#installation)).
2. Add integration → Airbnk lock (MQTT-based)
3. Enter your Airbnk/WeHere email + verification code
4. For each lock, use **Custom MQTT** type with:
   - **MQTT Topic:** `airbnk/{device_id}` (e.g. `airbnk/aaa5a38`)
   - **MAC Address:** your lock's MAC

## MQTT Topics

| Topic | Direction | Payload |
|---|---|---|
| `{prefix}/adv` | ESP32 → MQTT | `{"mac":"...","rssi":-80,"data":"BABA..."}` |
| `{prefix}/command` | MQTT → ESP32 | `{"sign":N,"command1":"...","command2":"..."}` |
| `{prefix}/command_result` | ESP32 → MQTT | `{"success":true,"sign":N,"lockStatus":"..."}` |
| `{prefix}/availability` | ESP32 → MQTT | `online` / `offline` |

## Project Structure

```
├── esp32-door-lock.yaml              # ESPHome device config
├── components/
│   └── airbnk_gateway/              # ESPHome external component
│       ├── __init__.py              # Python codegen (config schema)
│       ├── airbnk_gateway.h         # C++ header
│       ├── airbnk_gateway.cpp       # C++ implementation (NimBLE + MQTT)
│       └── CMakeLists.txt           # ESP-IDF component registration
├── secrets.yaml.example              # Template for secrets
└── README.md
```

## Key Technical Details

- **BLE:** Standalone NimBLE host (not ble_client / esp32_ble_tracker)
  — connect-on-demand pattern for Airbnk lock protocol
- **MQTT:** Uses `mqtt::global_mqtt_client` from ESPHome's built-in MQTT
- **Framework:** ESP-IDF 5.5.4 on ESP32-C3
- **No Arduino.h dependency** — pure ESP-IDF + ESPHome Component API
- **No crypto on ESP32** — the HA integration generates commands

## Credits

- [formatBCE](https://github.com/formatBCE/Airbnk-MQTTOpenGateway) —
  original gateway firmware
- [rospogrigio](https://github.com/rospogrigio/airbnk_mqtt) —
  Home Assistant integration
