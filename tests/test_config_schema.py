"""Drive the component's real CONFIG_SCHEMA with ESPHome's own validators.

Catches the class of bug that only shows up when ESPHome loads the component:
schema mistakes, validators that never fire (or always fire), and codegen that
emits the wrong C++.

    pip install --no-deps esphome voluptuous
    python3 tests/test_config_schema.py
"""
import sys, types
from pathlib import Path

import esphome.config_validation as cv
import esphome.codegen as cg

# The component imports the microphone/speaker packages only for cv.use_id();
# stub them so we don't need the whole component tree.
for name, cls in (("microphone", "Microphone"), ("speaker", "Speaker")):
    m = types.ModuleType("esphome.components." + name)
    setattr(m, cls, cg.esphome_ns.namespace(name).class_(cls))
    sys.modules["esphome.components." + name] = m

# cv.only_on_esp32 asks CORE for the target platform.
from esphome.core import CORE, KEY_CORE, KEY_TARGET_PLATFORM
CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = "esp32"

# Registers the esp32 pin schema so pins.internal_gpio_*_pin_number resolve.
import esphome.components.esp32.gpio  # noqa: F401
from esphome.components.esp32.const import KEY_ESP32, KEY_BOARD, KEY_VARIANT, VARIANT_ESP32P4
CORE.data[KEY_ESP32] = {KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4}
from esphome.config import path_context
path_context.set([])

import importlib.util
spec = importlib.util.spec_from_file_location(
    "rtsp_server", str(Path(__file__).resolve().parent.parent / "components" / "rtsp_server" / "__init__.py"))
rtsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rtsp)

def run(name, cfg, expect_ok, expect_msg=None):
    try:
        out = rtsp.CONFIG_SCHEMA(cfg)
        ok, msg = True, None
    except cv.Invalid as e:
        ok, msg = False, str(e)
    status = "PASS" if ok == expect_ok else "**FAIL**"
    if ok == expect_ok and expect_msg and (msg is None or expect_msg not in msg):
        status = "**FAIL (wrong message)**"
    print(f"{status:6} {name}")
    if status != "PASS":
        print(f"       expected ok={expect_ok} got ok={ok} msg={msg}")
        return False
    if not ok:
        print(f"       -> {msg.splitlines()[0][:110]}")
    return True

results = []

# The exact shape from doorbell-lvgl.yaml that just failed on the user's machine.
lvgl = {
    "id": "doorbell_stream", "port": 8554, "path": "/doorbell",
    "username": "user", "password": "pw",
    "video": {"codec": "mjpeg", "camera_id": "p4_cam", "drive_camera": False,
              "framerate": 15, "jpeg_quality": 25},
    "audio": {"codec": "pcmu", "sample_rate": 16000,
              "microphone_id": "board_microphone", "speaker_id": "board_speaker",
              "half_duplex": True, "talk_timeout": "300ms"},
}
results.append(run("doorbell-lvgl.yaml (mjpeg + camera_id + component audio)", lvgl, True))

# The headless shape from doorbell.yaml.
headless = {
    "id": "s", "video": {"codec": "mjpeg", "framerate": 15, "jpeg_quality": 25},
    "audio": {"codec": "pcmu", "sample_rate": 16000,
              "microphone": {"mode": "std", "i2s_port": 0, "bclk_pin": 20,
                             "lrclk_pin": 21, "din_pin": 22,
                             "bits_per_sample": 32, "channel": "left", "gain": 6.0},
              "speaker": {"i2s_port": 1, "bclk_pin": 23, "lrclk_pin": 26,
                          "dout_pin": 27, "bits_per_sample": 16,
                          "channel": "left", "volume": 0.8}},
}
results.append(run("doorbell.yaml (mjpeg + raw I2S pins)", headless, True))

# H.264 is gone. 'codec: h264' must say so rather than quietly serve MJPEG:
# a config that asked for it was relying on go2rtc not transcoding, and that
# assumption has to break loudly.
h264 = {"id": "s", "video": {"codec": "h264"}}
results.append(run("codec: h264 rejected with an explanation", h264, False, "only video codec is 'mjpeg'"))

# ...while an explicit 'codec: mjpeg' keeps working, so existing configs load.
results.append(run("codec: mjpeg still accepted", {"id": "s", "video": {"codec": "mjpeg"}}, True))

# Audio source mix-ups.
mix = dict(headless); mix["audio"] = dict(headless["audio"]); mix["audio"]["microphone_id"] = "m"
results.append(run("microphone_id + I2S pins rejected", mix, False, "choose one audio source"))

nomic = {"id": "s", "audio": {"speaker_id": "sp"}}
results.append(run("audio without any microphone rejected", nomic, False, "needs either"))

nobclk = {"id": "s", "audio": {"microphone": {"lrclk_pin": 21, "din_pin": 22}}}
results.append(run("std microphone without bclk_pin rejected", nobclk, False, "bclk_pin"))

fd = {"id": "s", "audio": {"microphone": {"mode": "std", "i2s_port": 0, "bclk_pin": 20,
                                          "lrclk_pin": 21, "din_pin": 22, "channel": "left"},
                           "speaker": {"i2s_port": 0, "bclk_pin": 20, "lrclk_pin": 21,
                                       "dout_pin": 27, "bits_per_sample": 32,
                                       "channel": "right"}}}
results.append(run("full duplex, same port, different slots", fd, True))

fdbad = {"id": "s", "audio": {"microphone": {"mode": "std", "i2s_port": 0, "bclk_pin": 20,
                                             "lrclk_pin": 21, "din_pin": 22, "channel": "left"},
                              "speaker": {"i2s_port": 0, "bclk_pin": 20, "lrclk_pin": 21,
                                          "dout_pin": 27, "bits_per_sample": 32,
                                          "channel": "left"}}}
results.append(run("full duplex, same slot rejected", fdbad, False, "different 'channel' slots"))

badpath = {"id": "s", "path": "doorbell"}
results.append(run("path without leading slash rejected", badpath, False, "must start with"))

# The defaults the codegen will read must be the documented ones.
d = rtsp.CONFIG_SCHEMA({"id": "s"})
assert d["video"]["jpeg_quality"] == 25 and d["video"]["framerate"] == 15
assert d["port"] == 8554 and str(d["path"]) == "/doorbell"
# No H.264 leftovers may reach the codegen: VideoPipeline::Config no longer has
# these members, so a stale key here would be a C++ compile error, not a warning.
for gone in ("bitrate", "gop", "min_qp", "max_qp", "encoder_device"):
    assert gone not in d["video"], gone
print("PASS   defaults: jpeg_quality=25 framerate=15 port=8554, no H.264 keys left")

# And the enum must still map to the right C++ symbol for codegen.
from esphome.cpp_generator import safe_exp
a = rtsp.CONFIG_SCHEMA({"id": "s", "audio": {"microphone_id": "m", "speaker_id": "s",
                                             "codec": "pcma"}})
print("       codegen emits: audio.codec  ->", safe_exp(a["audio"]["codec"]))
print("       codegen emits: mic.channel  ->", safe_exp(
    rtsp.CONFIG_SCHEMA({"id":"s","audio":{"microphone":{"bclk_pin":20,"lrclk_pin":21,
    "din_pin":22,"channel":"right"}}})["audio"]["microphone"]["channel"]))


# 'gain'/'volume' must be reachable on BOTH audio sources. With 'microphone_id'
# there is no nested I2S block to carry them, and they used to be pinned to 1.0
# with no way out -- a microphone that arrived too quiet stayed too quiet.
comp = rtsp.CONFIG_SCHEMA({"id": "s", "audio": {"microphone_id": "m", "speaker_id": "sp",
                                                "gain": 8.0, "volume": 0.5}})["audio"]
assert rtsp._level(comp, rtsp.CONF_GAIN, None, 1.0) == 8.0
assert rtsp._level(comp, rtsp.CONF_VOLUME, None, 1.0) == 0.5

# Omitted on the component path -> the neutral 1.0, never a silent boost.
bare = rtsp.CONFIG_SCHEMA({"id": "s", "audio": {"microphone_id": "m"}})["audio"]
assert rtsp._level(bare, rtsp.CONF_GAIN, None, 1.0) == 1.0

# On the I2S path the nested block still applies when nothing is set above it.
pins_only = rtsp.CONFIG_SCHEMA(headless)["audio"]
assert rtsp._level(pins_only, rtsp.CONF_GAIN, pins_only["microphone"], 1.0) == 6.0
assert rtsp._level(pins_only, rtsp.CONF_VOLUME, pins_only["speaker"], 1.0) == 0.8

# ...and the audio-level value overrides it, so a block can be tuned in place.
over = dict(headless); over["audio"] = dict(headless["audio"], gain=2.5, volume=0.1)
oc = rtsp.CONFIG_SCHEMA(over)["audio"]
assert rtsp._level(oc, rtsp.CONF_GAIN, oc["microphone"], 1.0) == 2.5
assert rtsp._level(oc, rtsp.CONF_VOLUME, oc["speaker"], 1.0) == 0.1

# The ceiling is 256, not 64: a codec that delivers -58 dBFS needs about +38 dB
# (x80) to reach a usable speech level, and 64 was not enough to get there.
results.append(run("gain of 80 accepted (a quiet codec needs it)",
                   {"id": "s", "audio": {"microphone_id": "m", "gain": 80.0}}, True))
results.append(run("gain out of range rejected", {"id": "s", "audio": {"microphone_id": "m", "gain": 999.0}},
                   False, "value must be at most 256"))
print("PASS   gain/volume reachable on the component path, overriding the I2S blocks")


# The struct initializers must be valid C++ designated initializers.
import esphome.codegen as _cg
lv = rtsp.CONFIG_SCHEMA(lvgl)
v = lv["video"]
vs = _cg.StructInitializer(
    rtsp.VideoConfig,
    ("device", v["device"]),
    ("framerate", v["framerate"]), ("jpeg_quality", v["jpeg_quality"]),
    ("drive_camera", v["drive_camera"]),
)
text = str(vs)
assert text.startswith("rtsp_server::VideoPipeline::Config{"), text
assert '.device = "/dev/video0",' in text
assert ".drive_camera = false," in text
print("\nGenerated C++ (VideoPipeline::Config):")
print("  " + text.replace("\n", "\n  "))

print()
print("all schema checks passed" if all(results) else "SOME CHECKS FAILED")
sys.exit(0 if all(results) else 1)
