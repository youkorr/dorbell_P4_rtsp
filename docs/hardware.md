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
