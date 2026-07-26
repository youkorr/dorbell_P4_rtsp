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

# h264 headless must still be accepted.
h264 = {"id": "s", "video": {"codec": "h264", "bitrate": 1500000, "gop": 15}}
results.append(run("h264 without camera_id", h264, True))

# ...and h264 + camera_id must still be rejected (the check that misfired).
h264cam = {"id": "s", "video": {"codec": "h264", "camera_id": "p4_cam"}}
results.append(run("h264 + camera_id rejected", h264cam, False, "cannot share an"))

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
assert str(d["video"]["codec"]) == "mjpeg", d["video"]["codec"]
assert d["video"]["jpeg_quality"] == 25 and d["video"]["framerate"] == 15
assert d["port"] == 8554 and str(d["path"]) == "/doorbell"
print("PASS   defaults: codec=mjpeg jpeg_quality=25 framerate=15 port=8554")

# And the enum must still map to the right C++ symbol for codegen.
from esphome.cpp_generator import safe_exp
h = rtsp.CONFIG_SCHEMA({"id": "s", "video": {"codec": "h264"}})
a = rtsp.CONFIG_SCHEMA({"id": "s", "audio": {"microphone_id": "m", "speaker_id": "s",
                                             "codec": "pcma"}})
print("       codegen emits: video.codec  ->", safe_exp(h["video"]["codec"]))
print("       codegen emits: audio.codec  ->", safe_exp(a["audio"]["codec"]))
print("       codegen emits: mic.channel  ->", safe_exp(
    rtsp.CONFIG_SCHEMA({"id":"s","audio":{"microphone":{"bclk_pin":20,"lrclk_pin":21,
    "din_pin":22,"channel":"right"}}})["audio"]["microphone"]["channel"]))


# The struct initializers must be valid C++ designated initializers.
import esphome.codegen as _cg
lv = rtsp.CONFIG_SCHEMA(lvgl)
v = lv["video"]
vs = _cg.StructInitializer(
    rtsp.VideoConfig,
    ("codec", v["codec"]), ("device", v["device"]),
    ("framerate", v["framerate"]), ("jpeg_quality", v["jpeg_quality"]),
    ("drive_camera", v["drive_camera"]),
)
text = str(vs)
assert text.startswith("rtsp_server::VideoPipeline::Config{"), text
assert ".codec = rtsp_server::VideoCodec::MJPEG," in text
assert '.device = "/dev/video0",' in text
assert ".drive_camera = false," in text
print("\nGenerated C++ (VideoPipeline::Config):")
print("  " + text.replace("\n", "\n  "))

print()
print("all schema checks passed" if all(results) else "SOME CHECKS FAILED")
sys.exit(0 if all(results) else 1)
