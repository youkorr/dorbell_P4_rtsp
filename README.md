# dorbell_P4_rtsp

ESPHome external component: an **RTSP server with two-way audio (ONVIF
backchannel) for the ESP32-P4**, built for a video doorbell.

```
OV5647 ──MIPI CSI──► ESP32-P4 ──MJPEG + G.711 / RTSP──► go2rtc ──WebRTC──► Home Assistant
                                ◄──── G.711 backchannel (talk) ────
```

The component initialises no display and pulls in no graphics library: it runs
headless. It can nevertheless share the camera with LVGL when the board has a
screen, so the same frame feeds a local preview and the network stream.

## What it does

| | |
|---|---|
| Video | **MJPEG**, the P4's hardware JPEG encoder, RTP per RFC 2435 |
| Video source | an `esp_cam_sensor` camera shared with LVGL, **or** the `esp_video` V4L2 device directly |
| Audio up | microphone → G.711 (PCMU/PCMA) 8 kHz → RTP |
| Audio down | ONVIF backchannel RTP → G.711 → speaker |
| Audio source | ESPHome `microphone`/`speaker` components (fdaudio, i2s_audio…) **or** raw I2S pins |
| Transport | RTP interleaved over the RTSP TCP connection (RFC 2326 §10.12) |
| Data path | zero copy: the camera's DMA buffer is encoded in place |
| Diagnostics | microphone and speaker meters, packet counters, local loopback, test beep |
| Authentication | Basic, optional |

## Requirements

- an **ESP32-P4** board with PSRAM and an OV5647 (or other supported) MIPI-CSI
  sensor;
- a Wi-Fi companion — the P4 has no radio of its own, so an `esp32_hosted:`
  block with an ESP32-C6 is mandatory;
- **go2rtc**, standalone or inside Frigate, to republish the stream as WebRTC;
- ESPHome 2025.5 or newer.

> **Check your board's I2C scan before copying an audio block.** By default
> `fdaudio` looks for an **ES7210** microphone ADC at `0x40`, and its absence
> takes the speaker down with the microphone — the board then makes no sound at
> all. Several ESP32-P4 boards — the Waveshare ESP32-P4-NANO among them — ship
> only the ES8311 at `0x18` and wire the microphone into that codec's own ADC.
> On those, set `mic_source: output_codec`; see
> [`docs/hardware.md`](docs/hardware.md) and
> [`doorbell-waveshare-p4-nano.yaml`](doorbell-waveshare-p4-nano.yaml).

## Quick start

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/dorbell_P4_rtsp
      ref: main
    components: [rtsp_server]
    refresh: 0s

esp_video:
  i2c_id: bsp_bus
  enable_isp: true

esp_cam_sensor:
  id: p4_cam
  i2c_id: bsp_bus
  sensor_type: ov5647
  resolution: "800x640"

rtsp_server:
  id: doorbell_stream
  port: 8554
  path: /doorbell
  backchannel: always
  video:
    camera_id: p4_cam
    framerate: 15
    jpeg_quality: 25
  audio:
    microphone_id: board_microphone
    speaker_id: board_speaker
    gain: 8.0
```

> **Always set `refresh: 0s`.** ESPHome caches a git external component for a
> whole day, keyed on the *branch name* rather than the commit. Without it, a
> branch that moves forward changes nothing about your next build, silently.

### Four complete example configurations

| File | Board | Screen |
|---|---|---|
| [`doorbell-lvgl.yaml`](doorbell-lvgl.yaml) | P4 evboard, ES8311 + ES7210 | 1024×600 MIPI-DSI, LVGL preview |
| [`doorbell-p4-headless.yaml`](doorbell-p4-headless.yaml) | the same board | none |
| [`doorbell-waveshare-p4-nano.yaml`](doorbell-waveshare-p4-nano.yaml) | Waveshare P4-NANO, **ES8311 only** | none |
| [`doorbell.yaml`](doorbell.yaml) | INMP441 + MAX98357A on raw I2S | none |

The first two describe the *same hardware*. The third is the same design on a
board with **no ES7210** — see
[hardware.md](docs/hardware.md#a-board-without-an-es7210-check-the-i2c-scan-first),
because on such a board the default `fdaudio` settings kill the speaker as well
as the microphone. The fourth is a different, minimal build with no audio codec.

**The audio blocks are not interchangeable** — the pin assignments belong to
different boards. Start from the file that matches yours, and confirm with the
I2C bus scan ESPHome prints at boot.

> The entity names in the example configs are in French (`Sonnette`, `Bouton`,
> `Micro actif`…), because that is what the author's own installation uses and
> renaming them would break existing automations. Home Assistant entity ids are
> built from them, so the documentation refers to ids like
> `event.<device>_sonnette`. Rename them freely in your own copy — just look the
> real ids up in **Developer Tools → States** afterwards.

## Documentation

- [`docs/home-assistant.md`](docs/home-assistant.md) — go2rtc, Frigate, the
  Lovelace card, notifications, and the full troubleshooting table
- [`docs/hardware.md`](docs/hardware.md) — GPIO map, I2S wiring, ESP32-P4
  constraints, acoustics
- [`go2rtc/go2rtc.yaml`](go2rtc/go2rtc.yaml) — a ready-to-use go2rtc config
- [`frigate/frigate.yaml`](frigate/frigate.yaml) — a reference Frigate config

## Configuration reference

### `rtsp_server`

| Option | Default | Description |
|---|---|---|
| `port` | `8554` | RTSP listening port |
| `path` | `/doorbell` | RTSP URL path |
| `username` / `password` | — | Basic authentication; omit both for an open stream |
| `max_clients` | `2` | simultaneous RTSP connections |
| `packet_size` | `1400` | maximum RTP packet size |
| `tx_buffer_size` | `192kB` | transmit ring. **The practical ceiling on resolution** — see below |
| `tx_buffer_psram` | `false` | put that ring in PSRAM instead of internal RAM |
| `backchannel` | `auto` | `auto` announces the `sendonly` track only to a client that sends the ONVIF `Require` header; `always` announces it to everyone |

**Set `backchannel: always` if you want a talk button in Home Assistant.**
go2rtc, Frigate and the Lovelace cards decide whether a camera supports two-way
audio by *probing* the stream — an ordinary DESCRIBE with no ONVIF header. Under
`auto` the track is not announced to that probe, so the capability exists but is
undiscoverable, and the button never appears on a device whose backchannel works
perfectly.

### `rtsp_server.video`

| Option | Default | Description |
|---|---|---|
| `camera_id` | — | `esp_cam_sensor` camera to share; omit for direct V4L2 |
| `drive_camera` | `true` | set `false` when `lvgl_camera_display` already dequeues V4L2 buffers |
| `device` | `/dev/video0` | MIPI-CSI capture device (direct source) |
| `framerate` | `15` | frames per second |
| `jpeg_quality` | `25` | 1 (worst) to 100 (best) |
| `buffer_count` | `2` | capture buffers (2 = double buffering) |
| `vflip` / `hflip` | `false` | flip, applied by the sensor |

`codec:` accepts only `mjpeg` and may be omitted. `codec: h264` is rejected at
validation — see [Why MJPEG only](#why-mjpeg-only).

**If you declare `esp_cam_sensor:`, you must pass `camera_id:`.** Otherwise both
components open `/dev/video0` independently and fight over it.

### `rtsp_server.audio`

| Option | Default | Description |
|---|---|---|
| `codec` | `pcmu` | `pcmu` (µ-law) or `pcma` (A-law) |
| `sample_rate` | `16000` | PCM rate (`8000` or `16000`); the wire stays at 8 kHz |
| `microphone_id` | — | an ESPHome `microphone` component (fdaudio, i2s_audio…) |
| `speaker_id` | — | an ESPHome `speaker` component |
| `gain` | `4.0` | digital gain applied after the codec, 0.1 – 256 |
| `volume` | `0.8` | digital attenuation on playback, 0.0 – 1.0 |
| `half_duplex` | `true` | mute the microphone while the far end is talking |
| `talk_timeout` | `500ms` | how long the microphone stays muted after the far end was last audible — effectively the room's reverberation time |
| `microphone.mode` | `std` | `std` (classic I2S) or `pdm` |
| `microphone.bits_per_sample` | `32` | 32 for an INMP441/ICS-43434 |
| `microphone.channel` | `left` | I2S slot; `left` = L/R pin tied to ground |
| `speaker.bits_per_sample` | `16` | 16 for a MAX98357A |

Two mutually exclusive audio sources:

- `microphone_id` + `speaker_id` — reuse existing ESPHome components, so the
  board's codec stays shared with other consumers;
- the `microphone` / `speaker` blocks — direct I2S pin control, for a minimal
  doorbell (INMP441 + MAX98357A).

With no speaker the backchannel is disabled: the stream stays one-way and the
SDP advertises no `sendonly` track.

### Triggers

`on_client_connected`, `on_client_disconnected`, `on_talk_start`, `on_talk_end`.

`on_talk_start` / `on_talk_end` bracket incoming audio from Home Assistant — the
place to switch an amplifier on and light an LED.

> These belong to `rtsp_server:`, not to the `speaker:` block.

### Actions

| Action | What it does |
|---|---|
| `rtsp_server.set_loopback` | routes the microphone to the speaker, locally |
| `rtsp_server.play_test_tone` | plays a beep (`frequency`, `duration`) |

### From a lambda

```cpp
id(doorbell_stream).client_count();          // uint8_t
id(doorbell_stream).is_streaming();          // at least one client in PLAY
id(doorbell_stream).is_talking();            // backchannel audio in progress
id(doorbell_stream).set_drive_camera(true);  // take back the V4L2 dequeue
```

## What a healthy chain looks like

The status block of a doorbell mid-conversation, worth keeping as a reference:

```
--- status ------------------------------------------------
  clients=2 playing=2 | negotiated across all sessions: video=yes audio=yes backchannel=yes
  video: 7585 encoded, 0 skipped | tx: 282 packets, 19 frames dropped
  audio: mic 31452 packets sent | backchannel 16255 received, 121 dropped  <-- TALKING NOW
  mic:   8051712 samples read (flowing), now -41.1 dBFS [###-------]  max/60s -11.0 dBFS
  spk:   now -18.1 dBFS [######----]  max/60s -4.7 dBFS
         10506240 bytes offered, 10506240 accepted, 0 short writes
-----------------------------------------------------------
```

| Reading | What it proves |
|---|---|
| `backchannel=yes` | the upstream track is negotiated — what `backchannel: always` makes possible |
| `backchannel N received`, N rising | Home Assistant's voice really reaches the device |
| `mic max/60s` between −30 and −6 dBFS | the microphone captures at a usable level |
| `spk` moving while someone talks | sound is actually coming out |
| **`offered == accepted`, `0 short writes`** | nothing is lost between decoding and the speaker |

The last one is the most important and the least obvious: a gap between
`offered` and `accepted` does not sound like reduced quality, it sounds like
*nothing at all*.

## Answering "is the microphone working?"

This is the hardest fault to place in the chain, because every stage between the
capsule and the browser can swallow the sound in silence. The component measures
at the two points that matter — just after capture and just before playback — so
the device answers on its own, with no go2rtc and no browser involved.

### The two tests, in this order

1. **Test beep** (`rtsp_server.play_test_tone`) — proves the speaker alone. No
   beep means the fault is the output: amplifier off, no `speaker_id`, volume at
   zero. Look no further.
2. **Local loopback** (`rtsp_server.set_loopback`) — speak in front of the
   doorbell and you should hear yourself. If you do, capture, gain, G.711
   companding and playback are all sound, and whatever remains is in the
   network, in go2rtc or in Home Assistant.

Both are wired into every example config, as Home Assistant entities *and* as
on-screen buttons where there is a screen.

> The loopback **times out after two minutes, by design**. Microphone and
> speaker share a board: at any useful gain the loop becomes acoustic and runs
> away into a howl, and while it runs it holds the speaker against the real
> backchannel. There is no reason to leave it on.

### Readings

| From a lambda | What it tells you |
|---|---|
| `mic_peak_hold_db()` | loudest microphone level of the last minute, in dBFS |
| `mic_level_db()` | instantaneous level; decays in a second |
| `mic_alive()` | is the microphone source still delivering samples? |
| `speaker_peak_hold_db()` | the same, for what the speaker accepted |
| `speaker_bytes_offered()` / `speaker_bytes_written()` | offered versus accepted |
| `speaker_healthy()` | false when the speaker refuses or truncates |
| `audio_packets_sent()` / `audio_packets_received()` | RTP up / backchannel down |

Use `mic_peak_hold_db()` for a Home Assistant sensor, never `mic_level_db()`: a
level that decays in a second is unreadable at any polling interval a sensor can
use, and reports room tone rather than speech.

How to read the microphone level:

| Reading | Diagnosis |
|---|---|
| `mic_alive()` false | the source delivers **nothing**: wrong `microphone_id`, codec not started, wrong I2S pins. No amount of gain will help |
| −100 dBFS while speaking | the source delivers **digital silence**: wrong I2S slot (`channel:`), dead capsule, gain at zero |
| −60 to −40 dBFS | it hears, but faintly — raise the gain (the codec's own first, then `gain:`) |
| −30 to −6 dBFS | a healthy speech level |
| above −3 dBFS | clipping: lower the gain |
| `audio_packets_received()` stuck at 0 while you press talk | the downstream voice never reaches the device; the fault is upstream, not here |

## Getting the doorbell press into Home Assistant

The ring has nothing to do with the RTSP stream — it is ordinary ESPHome, and
that is exactly where it goes wrong. A `binary_sensor` on its own is not enough:

- **no card reacts to a `binary_sensor`.** A camera card shows a camera, full
  stop. Surfacing the ring takes an automation;
- **without a `device_class`, Home Assistant cannot tell** it is a doorbell;
- **a short pulse gets missed.** If the API was disconnected at that instant,
  the information is gone.

The example configs therefore expose three paths. The first is enough in almost
every case:

| Path | Entity / event | Use it for |
|---|---|---|
| **`event`** | `event.<device>_sonnette`, `device_class: doorbell` | Home Assistant's **native doorbell entity**. Its state is the timestamp of the last press, it can be picked straight out of the automation editor, and nothing is lost |
| `binary_sensor` | `binary_sensor.<device>_bouton`, `device_class: occupancy` | conditions and cards that want an on/off. Held for 5 s so it is visible in Developer Tools → States |
| bus event | `esphome.doorbell_pressed` | automations written by hand in YAML |

**Publishing the press does not make anything ring.** Home Assistant does not
chime on its own: until an automation listens to these entities and makes a
noise, the press only changes a value in a database. The chime and notification
automation is in [`docs/home-assistant.md`](docs/home-assistant.md).

If nothing leaves the device at all, settle the press itself first: the ring
script logs `ring: sequence declenchee`. That line missing from `esphome logs`
when you press the button means the press never reached the script — on a touch
screen, a calibration or rotation problem — not that the ring failed.

## Why MJPEG only

The ESP32-P4 does have a hardware H.264 encoder, and an earlier version of this
component exposed it. It was removed, for two reasons that cannot be worked
around from inside this component:

- **the P4 cannot decode H.264.** What it encodes, it cannot display again: the
  local LVGL preview was impossible in that mode (it would take a software
  decoder such as [edge264](https://github.com/tvlabs/edge264));
- **the two formats are incompatible within one camera stream.** The H.264
  encoder consumes YUV420, LVGL displays RGB565. A single camera could not feed
  both — you had to choose between the screen and the stream.

MJPEG keeps one RGB565 frame feeding the screen *and* the network, with no
awkward alignment constraint and no waiting for a key frame. The price is
bitrate (~6–10 Mbit/s at 800×640 @ 15 fps), and the fact that WebRTC cannot
carry MJPEG: go2rtc will run ffmpeg to convert it. MSE, MJPEG and thumbnails go
through untouched.

Audio, by contrast, crosses the whole chain **without transcoding** in either
direction: browsers encode and decode PCMU/PCMA natively.

## Image resolution

The ESP32-P4's MIPI-CSI controller **does not rescale**: the resolution comes
from the format selected in the sensor at init time. With a direct V4L2 source
the component reads it back with `VIDIOC_G_FMT` and configures the encoder
accordingly — there is nothing to declare on `rtsp_server`. With `camera_id`,
the `resolution:` key of `esp_cam_sensor` decides.

OV5647 formats available in `esphome_esp-video`, selected by
`CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT`:

| Index | Resolution | Format | |
|---|---|---|---|
| 0 | 800×1280 @ 50 fps | RAW8 | portrait — a good fit for a doorbell |
| 1 | 800×640 @ 50 fps | RAW8 | what the example configs use |
| 2 | 800×800 @ 50 fps | RAW8 | |
| 3 | 1920×1080 @ 30 fps | RAW10 | heavy for the MJPEG bitrate |
| 4 | 1280×960 @ 45 fps | RAW10 | recommended in landscape |

RFC 2435 requires multiples of 8 and at most 2040 px per side. The component
checks this when opening the device and refuses to start with an explicit
message rather than producing an undecodable stream.

## Known limitations

- **TCP only.** UDP transport (`RTP/AVP;unicast`) is not implemented: a UDP
  `SETUP` gets `461 Unsupported Transport`. That is go2rtc's default anyway; VLC
  needs `--rtsp-tcp`.
- **No echo cancellation.** Half duplex replaces AEC; see the acoustics section
  of [`docs/hardware.md`](docs/hardware.md).
- **One active backchannel.** Do not request the backchannel from two go2rtc
  sources at once (see the comments in `go2rtc.yaml`).
- **No RTCP.** Incoming RTCP is ignored and none is emitted. Neither go2rtc nor
  ffmpeg needs it here.
- **MJPEG and WebRTC.** WebRTC cannot carry MJPEG, so go2rtc runs ffmpeg to
  transcode. This costs CPU on the Home Assistant machine and, because the
  transcode and the backchannel end up as two separate sources, it is what
  blocks the microphone button in the Advanced Camera Card — see
  [`docs/home-assistant.md`](docs/home-assistant.md).
- **No H.264.** See above.
- **One camera consumer.** Only one task may dequeue V4L2 buffers. With
  `drive_camera: false` that is `lvgl_camera_display`; if you turn the preview
  off, hand the dequeue back with `set_drive_camera(true)` or the stream freezes
  with no error anywhere. The preview switch in `doorbell-lvgl.yaml` does this.

## Implementation notes

- **Zero copy.** The camera buffer (DMA-capable PSRAM) is handed to
  `jpeg_encoder_process` as is; only the compressed stream is read back.
- **One network thread.** A single FreeRTOS task owns the listening socket, all
  sessions and transmission: no lock on the critical path. The video and audio
  tasks drop their RTP packets into a ring buffer and never block on the
  network — under saturation, packets are dropped rather than stalling the
  encoder.
- **Whole frames are dropped, never fragments.** A JPEG missing a fragment from
  its middle is not a slightly worse picture: the decoder resynchronises on
  whatever follows and paints parts of two frames at once. Truncating at the
  overflow point and resuming cleanly on the next frame costs frames, not
  coherence.
- **MJPEG (RFC 2435).** JFIF headers are not transmitted: the receiver rebuilds
  them from the 8-byte RTP header, and the quantization tables travel in band in
  the first packet of every frame (`Q = 255`). Subsampling is derived from the
  SOF marker.
- **One writer on the speaker.** The test beep, the backchannel and the loopback
  monitor share one real-time sink. Two tasks each pushing 20 ms every 20 ms
  offer twice what it can take, so it accepts about half of each and the result
  is silence. Priority is beep > far end > loopback.
- **Short writes are retried.** `Speaker::play()` returns how many bytes it
  accepted, which is not always all of them. The remainder is re-offered until
  it is in or a 60 ms deadline expires — dropping it instead punches a hole in
  every packet.

## Tests

Five harnesses, all runnable on a plain Linux host without ESP-IDF. See
[`tests/README.md`](tests/README.md).

```bash
./tests/lint_cpp.sh
python3 tests/check_esphome_api.py
python3 tests/test_config_schema.py
```

## Licence

See [LICENSE](LICENSE).
