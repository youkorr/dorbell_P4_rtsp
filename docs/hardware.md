# Hardware

## Overview

```
        ┌──────────────┐
        │   OV5647     │
        │  (MIPI CSI)  │
        └──────┬───────┘
               │ 2 MIPI lanes (dedicated pins, outside the GPIO matrix)
               │ + I2C/SCCB to configure the sensor
        ┌──────▼─────────────────────────────────┐
        │             ESP32-P4                   │
        │                                        │
        │  ISP  ──► RGB565 ──► HW JPEG encoder   │
        │  I2S0 ◄── microphone (INMP441)         │
        │  I2S1 ──► amplifier  (MAX98357A)       │
        └──────┬─────────────────────────────────┘
               │ SDIO
        ┌──────▼───────┐
        │  ESP32-C6    │  Wi-Fi 6
        └──────┬───────┘
               │ RTSP/TCP 8554
        ┌──────▼───────┐        WebRTC
        │   go2rtc     │ ─────────────────► Home Assistant
        └──────────────┘
```

Two hardware variants are described in this repository, and their audio wiring
is **not** interchangeable:

| Config | Audio |
|---|---|
| `doorbell-lvgl.yaml`, `doorbell-p4-headless.yaml` | the P4 evboard's own codec: ES8311 out, ES7210 in, driven by `fdaudio` |
| `doorbell.yaml` | INMP441 microphone and MAX98357A amplifier on raw I2S pins, no codec |

This page documents the second, plus the constraints that apply to both.

## ESP32-P4 GPIO constraints

Check these **before** settling on a pinout:

| Pins | Use | Usable? |
|---|---|---|
| MIPI CSI D0/D1/CLK | dedicated camera interface | No — outside the GPIO matrix |
| GPIO24, GPIO25 | USB-Serial-JTAG by default | Avoid (you lose the debug port) |
| GPIO34 – GPIO38 | strapping pins | Avoid (GPIO36 is often the camera XCLK) |
| SDIO bus to the ESP32-C6 | Wi-Fi | No — board-dependent |
| Flash / PSRAM | memory | No, dedicated pins |

The SDIO pinout to the C6 **differs from board to board** (P4-Function-EV-Board,
Waveshare NANO, Waveshare WIFI6, M5Stack Tab5…). Your board's schematic is the
only reliable source. If a pin is already taken, ESPHome or the I2S driver fails
at startup with an explicit error in the logs.

## Suggested pinout

Two separate I2S ports — the simplest and most forgiving arrangement.

### I2S microphone — INMP441 / ICS-43434 (I2S0)

| Signal | ESP32-P4 | Module | Note |
|---|---|---|---|
| BCLK | GPIO20 | SCK | bit clock |
| LRCLK / WS | GPIO21 | WS | word clock |
| DIN | GPIO22 | SD | data microphone → P4 |
| — | GND | L/R | tied to ground ⇒ **left** slot (`channel: left`) |
| 3V3 | 3V3 | VDD | |
| GND | GND | GND | |

The INMP441 outputs 24 bits left-justified in a 32-bit slot, hence
`bits_per_sample: 32` in the configuration. The component keeps only the top
16 bits.

### Speaker — MAX98357A (I2S1)

| Signal | ESP32-P4 | Module | Note |
|---|---|---|---|
| BCLK | GPIO23 | BCLK | |
| LRCLK / WS | GPIO26 | LRC | |
| DOUT | GPIO27 | DIN | data P4 → amplifier |
| SD (shutdown) | GPIO28 | SD | **recommended** — see below |
| 5V | 5V | Vin | the amplifier draws current in bursts |
| GND | GND | GND | |

The MAX98357A's `GAIN` pin left floating gives 9 dB, which suits a 4 Ω / 3 W
doorbell speaker. Tie it to GND for 12 dB if the level is too low.

### Button, chime and indicator

| Signal | ESP32-P4 | Note |
|---|---|---|
| Button | GPIO32 | `INPUT_PULLUP`, contact to GND |
| Chime relay | GPIO33 | through a transistor or optocoupler, never directly |
| Status LED | GPIO45 | |

> On a board with a screen, GPIO32 and GPIO33 are often already the backlight
> and the display reset. Check before reusing them.

## A board without an ES7210: check the I2C scan first

Before anything else, read the bus scan ESPHome prints at boot. It settles which
audio path your board can use:

```
[i2c.idf]: Results from bus scan:
[i2c.idf]: Found device at address 0x18     <- ES8311 audio codec
[i2c.idf]: Found device at address 0x36     <- camera sensor (OV5647, SC202CS…)
[i2c.idf]: Found device at address 0x40     <- ES7210 microphone ADC
```

| Address | Chip | Meaning |
|---|---|---|
| `0x18` | ES8311 | audio codec — DAC out, and it has its own ADC |
| `0x36` | camera sensor | OV5647 and several others share this address |
| `0x40` | ES7210 | a **separate** microphone ADC |

**`0x40` missing means the board has no ES7210.** Several ESP32-P4 boards,
including the Waveshare ESP32-P4-NANO, ship only the ES8311 and take the
microphone through the codec's own ADC.

That matters because **`fdaudio` requires an ES7210**: `mic_address` defaults to
`0x40` and there is no option to fall back to the ES8311's ADC. On such a board
`fdaudio` cannot bring the microphone up, whatever you set.

### Use ESPHome's own components instead

ESPHome's stock `es8311` component can drive the codec's ADC — that is exactly
what `use_microphone: true` does. Replace the whole `fdaudio:` block with:

```yaml
i2s_audio:
  - id: i2s_bus
    # CHECK THESE AGAINST YOUR BOARD'S SCHEMATIC. They differ from board to
    # board, and they are the one thing this page cannot guess for you.
    i2s_mclk_pin: GPIOxx
    i2s_bclk_pin: GPIOxx
    i2s_lrclk_pin: GPIOxx

audio_dac:
  - platform: es8311
    id: board_codec
    i2c_id: bsp_bus
    address: 0x18
    sample_rate: 16000
    bits_per_sample: 16bit
    use_mclk: true
    # This is the key: it configures the ES8311's own ADC as the microphone,
    # so no ES7210 is needed.
    use_microphone: true
    # 0DB, 6DB, 12DB, 18DB, 24DB, 30DB, 36DB, 42DB. Start high on a doorbell:
    # the measurement on the reference board showed the microphone arriving far
    # too quiet, and gain is cheaper here than downstream.
    mic_gain: 42DB

microphone:
  - platform: i2s_audio
    id: board_microphone
    i2s_audio_id: i2s_bus
    i2s_din_pin: GPIOxx
    adc_type: external
    bits_per_sample: 16bit
    channel: left
    sample_rate: 16000

speaker:
  - platform: i2s_audio
    id: board_speaker
    i2s_audio_id: i2s_bus
    dac_type: external
    i2s_dout_pin: GPIOxx
    audio_dac: board_codec
    sample_rate: 16000
    bits_per_sample: 16bit
    channel: mono
```

The `rtsp_server:` block does not change: it already takes any ESPHome
`microphone` and `speaker` through `microphone_id` / `speaker_id`.

### Finding the pins

This page deliberately does not list GPIO numbers for boards it cannot verify.
Get them from, in order of reliability:

1. your board's **schematic** from the vendor;
2. the **ESP-IDF BSP** for that board, if one exists — the
   `bsp/<board>/include/bsp/<board>.h` header defines `BSP_I2S_MCLK`,
   `BSP_I2S_SCLK`, `BSP_I2S_LCLK`, `BSP_I2S_DOUT`, `BSP_I2S_DSIN`, and the
   SDIO pins to the ESP32-C6;
3. the vendor's own ESPHome or Arduino example, if they publish one.

The SDIO pinout to the C6 in `esp32_hosted:` needs the same treatment — it is
board-specific, and a wrong pin there means Wi-Fi never comes up at all.

## Pin-saving variant: one I2S port in full duplex

The microphone and amplifier share BCLK and LRCLK, on two different slots:

```yaml
audio:
  microphone:
    i2s_port: 0
    bclk_pin: GPIO20
    lrclk_pin: GPIO21
    din_pin: GPIO22
    bits_per_sample: 32
    channel: left      # microphone L/R to GND
  speaker:
    i2s_port: 0        # same port as the microphone -> full duplex
    bclk_pin: GPIO20   # ignored, the microphone's clocks are reused
    lrclk_pin: GPIO21
    dout_pin: GPIO27
    bits_per_sample: 32  # must match the microphone
    channel: right     # MAX98357A SD to VDD -> right slot
```

Three pins saved. In exchange both directions share one clock and one slot
width; the component rejects an inconsistent configuration at compile time.

## Acoustics: what makes or breaks two-way audio

This component runs **no acoustic echo cancellation**. Without care the
microphone hears the speaker, the far end hears itself, and it can howl. Three
measures, most effective first:

1. **Half duplex (on by default).** `half_duplex: true` mutes the microphone
   while backchannel audio is arriving, and restores it `talk_timeout` after the
   last packet. That is the natural behaviour of a push-to-talk button.
2. **Cut the amplifier in hardware.** Wire the MAX98357A's `SD` pin and drive it
   from `on_talk_start` / `on_talk_end` (see `doorbell.yaml`). The amplifier is
   physically silent at rest: no hiss, less current.
3. **Physical separation.** Microphone and speaker at opposite ends of the
   enclosure, foam gasket around the microphone capsule, and no rigid path
   between the two. This buys the most for the least effort.

If you need real full duplex you will have to add `esp-sr`'s AFE (hardware AEC)
— outside the scope of this component.

> The local loopback test (`rtsp_server.set_loopback`) closes this acoustic loop
> on purpose. Expect it to howl at any useful gain; that is not a fault, and it
> is why the monitor stops itself after two minutes.

## Power supply

The P4 with the MIPI camera, the ISP, the JPEG encoder and Wi-Fi through the C6
draws noticeably more than a classic ESP32, with peaks on large frames and Wi-Fi
bursts. Plan for:

- a 5 V / 2 A supply, minimum;
- a 470 µF to 1000 µF decoupling capacitor near the module;
- short wires to the amplifier, which draws current in pulses.

A marginal supply shows up as reboots mid-stream or a `brownout` in the logs —
not as a degraded picture.
