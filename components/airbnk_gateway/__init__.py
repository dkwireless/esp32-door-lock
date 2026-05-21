import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["mqtt"]
AUTO_LOAD = []

CONF_MAC_ADDRESS = "mac_address"
CONF_TOPIC_PREFIX = "topic_prefix"

airbnk_ns = cg.esphome_ns.namespace("airbnk_gateway")
AirbnkGateway = airbnk_ns.class_("AirbnkGateway", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AirbnkGateway),
        cv.Required(CONF_MAC_ADDRESS): cv.string_strict,
        cv.Required(CONF_TOPIC_PREFIX): cv.string_strict,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_mac_address(config[CONF_MAC_ADDRESS]))
    cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
