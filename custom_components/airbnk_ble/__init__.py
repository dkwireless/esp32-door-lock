"""
ESPHome custom component: Airbnk BLE-MQTT Bridge

ESP-IDF only (no Arduino, no Bluedroid). Pure MQTT bridge —
all state published as JSON to airbnk/{device_id}/state.
Commands via airbnk/{device_id}/command: {"action":"unlock"} or {"action":"lock"}.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAC_ADDRESS

AUTO_LOAD = []
DEPENDENCIES = []

ns = cg.esphome_ns.namespace("airbnk_ble")
AirbnkBleComponent = ns.class_("AirbnkBleComponent", cg.Component)

CONF_MANUFACTURER_KEY = "manufacturer_key"
CONF_BINDING_KEY = "binding_key"
CONF_DEVICE_ID = "device_id"
CONF_BATTERY_V1 = "battery_v1"
CONF_BATTERY_V2 = "battery_v2"
CONF_BATTERY_V3 = "battery_v3"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AirbnkBleComponent),
    cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
    cv.Required(CONF_MANUFACTURER_KEY): cv.string,
    cv.Required(CONF_BINDING_KEY): cv.string,
    cv.Required(CONF_DEVICE_ID): cv.string_strict,
    cv.Optional(CONF_BATTERY_V1, default=3.5): cv.float_,
    cv.Optional(CONF_BATTERY_V2, default=3.8): cv.float_,
    cv.Optional(CONF_BATTERY_V3, default=4.1): cv.float_,
})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_mac_address(str(config[CONF_MAC_ADDRESS])))
    cg.add(var.set_manufacturer_key(config[CONF_MANUFACTURER_KEY]))
    cg.add(var.set_binding_key(config[CONF_BINDING_KEY]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_battery_v1(config[CONF_BATTERY_V1]))
    cg.add(var.set_battery_v2(config[CONF_BATTERY_V2]))
    cg.add(var.set_battery_v3(config[CONF_BATTERY_V3]))
