"""The fleet's ESP-NOW audio, presented to ESPHome as a microphone.

    microphone:
      - platform: comm_radio
        id: fleet_mic

Audio arrives from a handset over ESP-NOW; this makes it look like any other
capture device so voice_assistant can carry it to Home Assistant over the
native API. See espnow_microphone.h for why that is the whole point.
"""

import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID

from .. import CommRadio, comm_radio_ns

CODEOWNERS = ["@samperk1"]
DEPENDENCIES = ["comm_radio"]

CONF_COMM_RADIO_ID = "comm_radio_id"

EspNowMicrophone = comm_radio_ns.class_(
    "EspNowMicrophone", microphone.Microphone, cg.Component
)

def _set_stream_limits(config):
    """Declare exactly what this device produces.

    🔑 NOT A RANGE, AND NOT CONFIGURABLE. The audio is whatever the handsets
    record, and firmware/main/mic.h fixes that at 16 kHz, 16-bit, mono.
    Exposing these as options would invite a config that disagrees with the
    firmware at the other end of the radio, and the symptom would be speech
    played at the wrong speed rather than an error. Anything consuming this
    microphone is told the one format it will ever get.
    """
    # set_stream_limits() mutates the config and returns None, so the config
    # has to be handed back here or validation silently loses it.
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(EspNowMicrophone),
            cv.GenerateID(CONF_COMM_RADIO_ID): cv.use_id(CommRadio),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)

    # The radio needs to know where to push incoming audio. Wired here rather
    # than looked up at runtime so a missing microphone is a CONFIG error
    # instead of a null pointer on the first question anyone asks.
    parent = await cg.get_variable(config[CONF_COMM_RADIO_ID])
    cg.add(parent.set_microphone(var))
