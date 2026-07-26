"""ESPHome external component: RTSP server with ONVIF two-way audio for the ESP32-P4.

Video is captured either from a shared `esp_cam_sensor` camera (so a board with a
screen can preview and stream at the same time) or straight from the `esp_video`
V4L2 device, and encoded by the ESP32-P4's hardware JPEG engine (MJPEG, the
default) or its hardware H.264 encoder.

Audio is captured and played back either through ESPHome `microphone` / `speaker`
platforms (fdaudio, i2s_audio, ...) or through raw I2S pins, and companded to
G.711 — which both go2rtc and browsers speak natively.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation, pins
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]
# So audio_pipeline.cpp can include the microphone/speaker headers.
AUTO_LOAD = ["microphone", "speaker"]

rtsp_server_ns = cg.esphome_ns.namespace("rtsp_server")
RTSPServer = rtsp_server_ns.class_("RTSPServer", cg.Component)

VideoConfig = rtsp_server_ns.namespace("VideoPipeline").struct("Config")
AudioConfig = rtsp_server_ns.namespace("AudioPipeline").struct("Config")

VideoCodec = rtsp_server_ns.enum("VideoCodec", is_class=True)
AudioCodec = rtsp_server_ns.enum("AudioCodec", is_class=True)
MicMode = rtsp_server_ns.enum("MicMode", is_class=True)

# The camera comes from the esp_cam_sensor external component. It is referenced
# by C++ type name so this module does not have to import that package.
esp_cam_sensor_ns = cg.esphome_ns.namespace("esp_cam_sensor")
MipiDSICamComponent = esp_cam_sensor_ns.class_("MipiDSICamComponent", cg.Component)

ClientConnectedTrigger = rtsp_server_ns.class_("ClientConnectedTrigger", automation.Trigger.template())
ClientDisconnectedTrigger = rtsp_server_ns.class_("ClientDisconnectedTrigger", automation.Trigger.template())
TalkStartTrigger = rtsp_server_ns.class_("TalkStartTrigger", automation.Trigger.template())
TalkEndTrigger = rtsp_server_ns.class_("TalkEndTrigger", automation.Trigger.template())

# All configuration keys are declared here rather than imported from
# esphome.const, so that a rename upstream cannot break this component.
CONF_PORT = "port"
CONF_PATH = "path"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_MAX_CLIENTS = "max_clients"
CONF_PACKET_SIZE = "packet_size"

CONF_VIDEO = "video"
CONF_CODEC = "codec"
CONF_CAMERA_ID = "camera_id"
CONF_DRIVE_CAMERA = "drive_camera"
CONF_DEVICE = "device"
CONF_ENCODER_DEVICE = "encoder_device"
CONF_FRAMERATE = "framerate"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_BITRATE = "bitrate"
CONF_GOP = "gop"
CONF_MIN_QP = "min_qp"
CONF_MAX_QP = "max_qp"
CONF_BUFFER_COUNT = "buffer_count"
CONF_VFLIP = "vflip"
CONF_HFLIP = "hflip"

CONF_AUDIO = "audio"
CONF_SAMPLE_RATE = "sample_rate"
CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_MODE = "mode"
CONF_I2S_PORT = "i2s_port"
CONF_BCLK_PIN = "bclk_pin"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_DIN_PIN = "din_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_BITS_PER_SAMPLE = "bits_per_sample"
CONF_CHANNEL = "channel"
CONF_GAIN = "gain"
CONF_VOLUME = "volume"
CONF_HALF_DUPLEX = "half_duplex"
CONF_TALK_TIMEOUT = "talk_timeout"

CONF_ON_CLIENT_CONNECTED = "on_client_connected"
CONF_ON_CLIENT_DISCONNECTED = "on_client_disconnected"
CONF_ON_TALK_START = "on_talk_start"
CONF_ON_TALK_END = "on_talk_end"

VIDEO_CODECS = {
    "mjpeg": VideoCodec.MJPEG,
    "h264": VideoCodec.H264,
}

AUDIO_CODECS = {
    "pcmu": AudioCodec.PCMU,
    "pcma": AudioCodec.PCMA,
}

MIC_MODES = {
    "std": MicMode.STD,
    "pdm": MicMode.PDM,
}

# Mapped straight onto `*_right_slot` in the C++ config.
CHANNELS = {"left": False, "right": True}


def _selected(value):
    """Name of the option a ``cv.enum`` validator accepted.

    ``cv.enum`` returns the validated *key* (an ``EnumValue``, i.e. a ``str``
    subclass) and keeps the mapped C++ object on ``.enum_value``. Comparing it
    against that mapped object is a trap: ``MockObj.__eq__`` builds a C++
    expression rather than answering a question, and the resulting object is
    always truthy. So compare names, never mapped values.
    """
    return str(value)


def _validate_path(value):
    value = cv.string(value)
    if not value.startswith("/"):
        raise cv.Invalid("'path' must start with '/'")
    return value


VIDEO_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CODEC, default="mjpeg"): cv.enum(VIDEO_CODECS, lower=True),
        # Share an esp_cam_sensor camera (its RGB565 frames also feed LVGL).
        # Omit it to open the V4L2 device directly, for a headless build.
        cv.Optional(CONF_CAMERA_ID): cv.use_id(MipiDSICamComponent),
        # Set false when lvgl_camera_display already drives the capture, so only
        # one consumer dequeues V4L2 buffers.
        cv.Optional(CONF_DRIVE_CAMERA, default=True): cv.boolean,
        cv.Optional(CONF_DEVICE, default="/dev/video0"): cv.string,
        cv.Optional(CONF_ENCODER_DEVICE, default="/dev/video11"): cv.string,
        cv.Optional(CONF_FRAMERATE, default=15): cv.int_range(min=1, max=30),
        # MJPEG: 1 (worst) to 100 (best). ~25 keeps a 1280x960 frame near 60 kB.
        cv.Optional(CONF_JPEG_QUALITY, default=25): cv.int_range(min=1, max=100),
        # H.264 only. The ESP32-P4 hardware encoder tops out at 2.5 Mbps.
        cv.Optional(CONF_BITRATE, default=1500000): cv.int_range(min=25000, max=2500000),
        cv.Optional(CONF_GOP, default=15): cv.int_range(min=1, max=120),
        cv.Optional(CONF_MIN_QP, default=25): cv.int_range(min=1, max=51),
        cv.Optional(CONF_MAX_QP, default=40): cv.int_range(min=1, max=51),
        cv.Optional(CONF_BUFFER_COUNT, default=2): cv.int_range(min=2, max=4),
        cv.Optional(CONF_VFLIP, default=False): cv.boolean,
        cv.Optional(CONF_HFLIP, default=False): cv.boolean,
    }
)


def _validate_video(config):
    if _selected(config[CONF_CODEC]) == "h264" and CONF_CAMERA_ID in config:
        raise cv.Invalid(
            "'codec: h264' reads YUV420 straight from the V4L2 device and cannot share an "
            "esp_cam_sensor camera (which delivers RGB565). Remove 'camera_id', or use 'codec: mjpeg'."
        )
    return config


VIDEO_SCHEMA = cv.All(VIDEO_SCHEMA, _validate_video)

MICROPHONE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MODE, default="std"): cv.enum(MIC_MODES, lower=True),
        cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=2),
        cv.Optional(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_BITS_PER_SAMPLE, default=32): cv.one_of(16, 32, int=True),
        cv.Optional(CONF_CHANNEL, default="left"): cv.enum(CHANNELS, lower=True),
        cv.Optional(CONF_GAIN, default=4.0): cv.float_range(min=0.1, max=64.0),
    }
)

SPEAKER_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_I2S_PORT, default=1): cv.int_range(min=0, max=2),
        cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, 32, int=True),
        cv.Optional(CONF_CHANNEL, default="left"): cv.enum(CHANNELS, lower=True),
        cv.Optional(CONF_VOLUME, default=0.8): cv.float_range(min=0.0, max=1.0),
    }
)


def _validate_audio(config):
    uses_components = CONF_MICROPHONE_ID in config
    uses_pins = CONF_MICROPHONE in config

    if uses_components and uses_pins:
        raise cv.Invalid(
            "choose one audio source: either 'microphone_id'/'speaker_id' (an ESPHome audio "
            "component such as fdaudio or i2s_audio), or the raw I2S 'microphone'/'speaker' pin blocks"
        )
    if not uses_components and not uses_pins:
        raise cv.Invalid(
            "the audio block needs either 'microphone_id' (an ESPHome microphone component) "
            "or a 'microphone' block describing the I2S pins"
        )

    if uses_components:
        if CONF_SPEAKER in config:
            raise cv.Invalid("'speaker' (I2S pins) cannot be combined with 'microphone_id'; use 'speaker_id'")
        return config

    mic = config[CONF_MICROPHONE]
    if CONF_SPEAKER_ID in config:
        raise cv.Invalid("'speaker_id' cannot be combined with the I2S 'microphone' block; use 'microphone_id'")

    if _selected(mic[CONF_MODE]) == "std" and CONF_BCLK_PIN not in mic:
        raise cv.Invalid("'bclk_pin' is required for a standard I2S microphone", [CONF_MICROPHONE])

    spk = config.get(CONF_SPEAKER)
    if spk is None or spk[CONF_I2S_PORT] != mic[CONF_I2S_PORT]:
        return config

    # Microphone and speaker share one I2S port: they must share its clock and
    # slot layout, and sit in different slots.
    if _selected(mic[CONF_MODE]) != "std":
        raise cv.Invalid(
            "a PDM microphone cannot share an I2S port with the speaker; give the speaker its own 'i2s_port'"
        )
    if spk[CONF_BITS_PER_SAMPLE] != mic[CONF_BITS_PER_SAMPLE]:
        raise cv.Invalid(
            "when the microphone and the speaker share an I2S port they must use the same 'bits_per_sample'"
        )
    if spk[CONF_CHANNEL] == mic[CONF_CHANNEL]:
        raise cv.Invalid(
            "when the microphone and the speaker share an I2S port they must use different 'channel' slots "
            "(one 'left', one 'right')"
        )
    return config


AUDIO_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_CODEC, default="pcmu"): cv.enum(AUDIO_CODECS, lower=True),
            # G.711 always runs at 8 kHz on the wire; this is the PCM rate of the
            # I2S bus or of the ESPHome microphone/speaker components.
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.one_of(8000, 16000, int=True),
            # Source A: existing ESPHome audio components (fdaudio, i2s_audio...).
            cv.Optional(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            # Source B: raw I2S pins, for a minimal headless doorbell.
            cv.Optional(CONF_MICROPHONE): MICROPHONE_SCHEMA,
            cv.Optional(CONF_SPEAKER): SPEAKER_SCHEMA,
            cv.Optional(CONF_HALF_DUPLEX, default=True): cv.boolean,
            cv.Optional(CONF_TALK_TIMEOUT, default="300ms"): cv.positive_time_period_milliseconds,
        }
    ),
    _validate_audio,
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RTSPServer),
            cv.Optional(CONF_PORT, default=8554): cv.port,
            cv.Optional(CONF_PATH, default="/doorbell"): _validate_path,
            cv.Optional(CONF_USERNAME): cv.string,
            cv.Optional(CONF_PASSWORD): cv.string,
            cv.Optional(CONF_MAX_CLIENTS, default=2): cv.int_range(min=1, max=4),
            cv.Optional(CONF_PACKET_SIZE, default=1400): cv.int_range(min=512, max=1460),
            cv.Optional(CONF_VIDEO, default={}): VIDEO_SCHEMA,
            cv.Optional(CONF_AUDIO): AUDIO_SCHEMA,
            cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ClientConnectedTrigger)}
            ),
            cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ClientDisconnectedTrigger)}
            ),
            cv.Optional(CONF_ON_TALK_START): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TalkStartTrigger)}
            ),
            cv.Optional(CONF_ON_TALK_END): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TalkEndTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


def _final_validate(config):
    """The V4L2 devices this component opens are created by `esp_video`."""
    full_config = fv.full_config.get()

    esp_video = full_config.get("esp_video")
    if esp_video is None:
        raise cv.Invalid(
            "the 'rtsp_server' component needs the 'esp_video' component to bring up the MIPI-CSI camera; "
            "add an 'esp_video:' block"
        )

    if _selected(config[CONF_VIDEO][CONF_CODEC]) == "h264":
        # esp_video only builds /dev/video11 when it is asked to.
        entries = esp_video if isinstance(esp_video, list) else [esp_video]
        if not any(entry.get("enable_h264") for entry in entries if isinstance(entry, dict)):
            raise cv.Invalid(
                "'codec: h264' needs the hardware H.264 encoder device (/dev/video11); "
                "set 'enable_h264: true' on the 'esp_video' component"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_path(config[CONF_PATH]))
    cg.add(var.set_max_clients(config[CONF_MAX_CLIENTS]))
    cg.add(var.set_packet_size(config[CONF_PACKET_SIZE]))

    if CONF_USERNAME in config or CONF_PASSWORD in config:
        cg.add(var.set_credentials(config.get(CONF_USERNAME, ""), config.get(CONF_PASSWORD, "")))

    video = config[CONF_VIDEO]
    cg.add(
        var.set_video_config(
            cg.StructInitializer(
                VideoConfig,
                ("codec", video[CONF_CODEC]),
                ("device", video[CONF_DEVICE]),
                ("encoder_device", video[CONF_ENCODER_DEVICE]),
                ("framerate", video[CONF_FRAMERATE]),
                ("jpeg_quality", video[CONF_JPEG_QUALITY]),
                ("bitrate", video[CONF_BITRATE]),
                ("gop", video[CONF_GOP]),
                ("min_qp", video[CONF_MIN_QP]),
                ("max_qp", video[CONF_MAX_QP]),
                ("buffer_count", video[CONF_BUFFER_COUNT]),
                ("vflip", video[CONF_VFLIP]),
                ("hflip", video[CONF_HFLIP]),
                ("drive_camera", video[CONF_DRIVE_CAMERA]),
            )
        )
    )

    if CONF_CAMERA_ID in video:
        camera = await cg.get_variable(video[CONF_CAMERA_ID])
        cg.add(var.set_camera(camera))

    if CONF_AUDIO in config:
        audio = config[CONF_AUDIO]
        mic = audio.get(CONF_MICROPHONE)
        spk = audio.get(CONF_SPEAKER)
        uses_components = CONF_MICROPHONE_ID in audio

        cg.add(
            var.set_audio_config(
                cg.StructInitializer(
                    AudioConfig,
                    ("codec", audio[CONF_CODEC]),
                    ("sample_rate", audio[CONF_SAMPLE_RATE]),
                    ("mic_port", mic[CONF_I2S_PORT] if mic else 0),
                    ("mic_mode", mic[CONF_MODE] if mic else MIC_MODES["std"]),
                    ("mic_bclk", mic.get(CONF_BCLK_PIN, -1) if mic else -1),
                    ("mic_lrclk", mic[CONF_LRCLK_PIN] if mic else -1),
                    ("mic_din", mic[CONF_DIN_PIN] if mic else -1),
                    ("mic_bits", mic[CONF_BITS_PER_SAMPLE] if mic else 16),
                    ("mic_right_slot", mic[CONF_CHANNEL] if mic else False),
                    ("mic_gain", mic[CONF_GAIN] if mic else 1.0),
                    # With ESPHome audio components the speaker is enabled by
                    # 'speaker_id'; with raw I2S it is the 'speaker' pin block.
                    (
                        "speaker_enabled",
                        (CONF_SPEAKER_ID in audio) if uses_components else (spk is not None),
                    ),
                    ("speaker_port", spk[CONF_I2S_PORT] if spk else -1),
                    ("speaker_bclk", spk[CONF_BCLK_PIN] if spk else -1),
                    ("speaker_lrclk", spk[CONF_LRCLK_PIN] if spk else -1),
                    ("speaker_dout", spk[CONF_DOUT_PIN] if spk else -1),
                    ("speaker_bits", spk[CONF_BITS_PER_SAMPLE] if spk else 16),
                    ("speaker_right_slot", spk[CONF_CHANNEL] if spk else False),
                    ("speaker_volume", spk[CONF_VOLUME] if spk else 1.0),
                    ("half_duplex", audio[CONF_HALF_DUPLEX]),
                    ("talk_timeout_ms", audio[CONF_TALK_TIMEOUT].total_milliseconds),
                )
            )
        )

        if CONF_MICROPHONE_ID in audio:
            mic_component = await cg.get_variable(audio[CONF_MICROPHONE_ID])
            cg.add(var.set_microphone(mic_component))
        if CONF_SPEAKER_ID in audio:
            spk_component = await cg.get_variable(audio[CONF_SPEAKER_ID])
            cg.add(var.set_speaker(spk_component))

    for key in (
        CONF_ON_CLIENT_CONNECTED,
        CONF_ON_CLIENT_DISCONNECTED,
        CONF_ON_TALK_START,
        CONF_ON_TALK_END,
    ):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
