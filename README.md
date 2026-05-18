# ESP32 Airbnk Door Lock (BLE-to-MQTT Gateway)

ESP32-C3 ESPHome firmware that bridges Airbnk smart locks (BLE) to MQTT, designed to work with [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt) Home Assistant integration.

Based on [formatBCE/Airbnk-MQTTOpenGateway](https://github.com/formatBCE/Airbnk-MQTTOpenGateway) ESPHome component.

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

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in your credentials
2. Update `lock_mac` substitution in `esp32-door-lock.yaml` with your Airbnk lock MAC
3. Flash to ESP32-C3 via ESPHome

## File Structure

```
├── esp32-door-lock.yaml          # Main ESPHome config
├── components/
│   └── airbnk-gateway.h          # C++ BLE-to-MQTT bridge component
├── secrets/
│   └── secrets.yaml.example      # Template for secrets
├── .gitea/
│   └── workflows/
│       └── ci.yaml               # Gitea Actions CI
└── README.md
```

## Integration

After flashing, configure [rospogrigio/airbnk_mqtt](https://github.com/rospogrigio/airbnk_mqtt) in Home Assistant:

1. Add the custom integration via HACS or manual copy
2. Enter your Airbnk/WeHere email
3. Enter verification code
4. For each lock, enter:
   - MQTT topic: `tele/maindoor`
   - Lock MAC address
