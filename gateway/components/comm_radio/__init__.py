"""The fleet's ESP-NOW radio as an ESPHome component.

espnow.c / fleet.c / applog.c in this directory are SYMLINKS into the handset
firmware (../../../firmware/main). They are deliberately not copies: one
implementation of the wire format, the per-link LMKs, the AES-GCM and the
pairing ratchet, or the two ends of the radio drift apart silently.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import voice_assistant
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_NAME

CONF_FLEET_CODE = "fleet_code"
CONF_SELF_TEST = "self_test"
CONF_VOICE_ASSISTANT_ID = "voice_assistant_id"

CODEOWNERS = ["@samperk1"]
DEPENDENCIES = ["wifi", "esp32"]

comm_radio_ns = cg.esphome_ns.namespace("comm_radio")
CommRadio = comm_radio_ns.class_("CommRadio", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CommRadio),
        # The fleet's channel. This is NOT a request: a station's channel is the
        # AP's, so this is what we expect to land on and complain about if we
        # do not.
        cv.Optional(CONF_CHANNEL, default=6): cv.int_range(min=1, max=13),
        cv.Optional(CONF_NAME, default="Gateway"): cv.string,
        # The fleet's shared secret. Belongs in secrets.yaml and NEVER in a
        # published config - it is the only thing standing between a stranger's
        # ESP32 and the kids' handsets.
        cv.Optional(CONF_FLEET_CODE, default=""): cv.string,
        # Gate 3.1 scaffolding: push one synthetic frame at startup to prove the
        # component -> microphone -> on_data path. Not for normal use.
        cv.Optional(CONF_SELF_TEST, default=False): cv.boolean,
        # Optional so the radio still builds without a voice pipeline — pairing,
        # beacons and telemetry do not need one, and a gateway that can only
        # relay text is a legitimate configuration.
        cv.Optional(CONF_VOICE_ASSISTANT_ID): cv.use_id(
            voice_assistant.VoiceAssistant
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_gw_name(config[CONF_NAME]))
    cg.add(var.set_fleet_code(config[CONF_FLEET_CODE]))
    cg.add(var.set_self_test(config[CONF_SELF_TEST]))

    if CONF_VOICE_ASSISTANT_ID in config:
        va = await cg.get_variable(config[CONF_VOICE_ASSISTANT_ID])
        cg.add(cg.RawExpression(f"esphome::comm_radio::global_ask_relay.set_voice_assistant({va})"))
