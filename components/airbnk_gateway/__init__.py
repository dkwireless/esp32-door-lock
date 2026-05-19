import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import ble_client

DEPENDENCIES = ["ble_client", "mqtt"]
AUTO_LOAD = []

CONF_MAC_ADDRESS = "mac_address"
CONF_TOPIC_PREFIX = "topic_prefix"
CONF_MANUFACTURER_KEY = "manufacturer_key"
CONF_BINDING_KEY = "binding_key"
CONF_BLE_CLIENT_ID = "ble_client_id"

airbnk_gateway_ns = cg.esphome_ns.namespace("airbnk_gateway")
AirbnkGateway = airbnk_gateway_ns.class_("AirbnkGateway",
                                          cg.Component,
                                          ble_client.BLEClientNode)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AirbnkGateway),
        cv.GenerateID(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
        cv.Required(CONF_MAC_ADDRESS): cv.string_strict,
        cv.Required(CONF_TOPIC_PREFIX): cv.string_strict,
        cv.Optional(CONF_MANUFACTURER_KEY, default=""): cv.string_strict,
        cv.Optional(CONF_BINDING_KEY, default=""): cv.string_strict,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_mac_address(config[CONF_MAC_ADDRESS]))
    cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
    cg.add(var.set_manufacturer_key(config[CONF_MANUFACTURER_KEY]))
    cg.add(var.set_binding_key(config[CONF_BINDING_KEY]))
