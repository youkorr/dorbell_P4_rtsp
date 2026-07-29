# Home Assistant integration

How the ESP32-P4 doorbell reaches Home Assistant: go2rtc, Frigate, the Lovelace
card, notifications, and what to check when something does not work.

Throughout, the running example uses these values. Yours will differ — replace
them everywhere.

| | |
|---|---|
| ESP32-P4 | `192.168.1.9`, RTSP on port 8554 |
| Home Assistant / Frigate host | `192.168.1.38` |
| ESPHome device name | `doorbell-p4-lvgl` |
| go2rtc streams | `doorbell` (source), `doorbell_webrtc` (what you watch) |
| Frigate camera | `doorbell` → `camera.doorbell` in Home Assistant |

---

## 1. How the pieces fit

The P4 serves **MJPEG video and G.711 audio over RTSP**. Home Assistant cannot
consume that directly for live viewing with sound, so go2rtc sits in between and
republishes it as WebRTC. There is **no auto-discovery**: you configure the URL
by hand.

```
ESP32-P4 ──RTSP──► go2rtc ──WebRTC──► Lovelace card
   ▲                  │
   └── backchannel ───┘
```

Three things follow, and each one bites somebody:

1. **WebRTC cannot carry MJPEG.** go2rtc has to run ffmpeg to transcode the
   video to H.264. That costs CPU on the Home Assistant machine, and it is the
   root of the microphone-button problem in section 8.
2. **The audio is never transcoded.** G.711 crosses the whole chain untouched in
   both directions — browsers speak PCMU/PCMA natively.
3. **The P4 must have a stable address.** go2rtc dials it by IP or hostname; if
   that changes, the stream dies silently.

### Give the P4 a fixed address

```yaml
# Option A — mDNS name, published automatically by ESPHome:
#   rtsp://user:pass@doorbell-p4-lvgl.local:8554/doorbell
# Reliable only if your network resolves .local names, which many do not
# across VLANs.
#
# Option B — a DHCP reservation on your router (recommended):
#   rtsp://user:pass@192.168.1.9:8554/doorbell
```

The device logs its own URL on every boot, and the example configs expose it as
a `text_sensor` named "Flux RTSP".

---

## 2. Prerequisites

- **go2rtc**, either the standalone add-on or the copy embedded in Frigate;
- **HTTPS access to Home Assistant** if you want to talk back — see section 8.
  Nabu Casa and Tailscale both provide this with no certificate to manage;
- one of these Lovelace cards, via HACS:
  - **Advanced Camera Card** (formerly Frigate Card), or
  - **WebRTC Camera** (AlexxIT).

---

## 3. Which go2rtc?

Decide this first, because everything downstream depends on it.

| | Use it when |
|---|---|
| **go2rtc inside Frigate** | you already run Frigate. It has its own go2rtc; adding a second means two instances fighting over the same camera |
| **Standalone go2rtc add-on** | you do not run Frigate |

Two go2rtc instances both pulling the doorbell will exceed `max_clients` on the
P4 and produce intermittent failures that look like network trouble. Pick one.

---

## 4. go2rtc configuration

A ready-to-use file is in [`../go2rtc/go2rtc.yaml`](../go2rtc/go2rtc.yaml).

```yaml
streams:
  # The raw source. It gives the backchannel UP with '#backchannel=0' because it
  # is only used as the transcode input -- see the rule below.
  doorbell:
    - rtsp://USER:PASS@192.168.1.9:8554/doorbell#backchannel=0

  # What you actually watch. Two sources:
  #   1. ffmpeg transcodes the video to H.264, because WebRTC cannot carry MJPEG
  #   2. a direct RTSP session carries the microphone AND the backchannel, in
  #      G.711, with no codec round trip
  doorbell_webrtc:
    - ffmpeg:doorbell#video=h264#raw=-r 15 -g 15 -keyint_min 15
    - rtsp://USER:PASS@192.168.1.9:8554/doorbell#backchannel=1

webrtc:
  listen: ":8555/tcp"
  candidates:
    - 192.168.1.38:8555   # the IP of the machine running go2rtc, not an example
    - stun:8555

api:
  listen: ":1984"
```

### The one rule

**Exactly one source may hold the ONVIF backchannel open.** go2rtc requests it
by *default* on a bare `rtsp://` URL, and adding any `#` fragment flips that
default off:

| URL | Backchannel |
|---|---|
| `rtsp://.../doorbell` | ON (default) |
| `rtsp://.../doorbell#media=video,audio` | OFF |
| `rtsp://.../doorbell#backchannel=1` | ON (explicit) |
| `rtsp://.../doorbell#backchannel=0` | OFF (explicit) |

Two sources holding it at once is the single most common cause of "two-way audio
worked once and then never again".

### Consequences for `max_clients`

This chain costs **two RTSP sessions** on the P4. The default `max_clients: 2`
leaves no room for a third client, so raise it:

```yaml
rtsp_server:
  max_clients: 4    # 2 for go2rtc, plus room for VLC while debugging
```

### Check that go2rtc is receiving the stream

Open `http://192.168.1.38:1984` and click **probe** on **`doorbell_webrtc`**.
You should see three tracks:

```
video, recvonly, JPEG
audio, recvonly, PCMU/8000
audio, sendonly, PCMU/8000     <-- the backchannel
```

> Probe `doorbell_webrtc`, **not** `doorbell`. The latter carries
> `#backchannel=0` — it gives the backchannel up on purpose and will never show
> the `sendonly` track, even on a perfectly healthy chain.

If the third line is missing, either an extra `#` in the URL, or the P4 is
announcing the track on request only — see section 8.

To see the negotiation itself, set `log: {rtsp: trace}` in go2rtc and restart:
the `DESCRIBE` must carry `Require: www.onvif.org/ver20/backchannel`, and the
SDP the P4 returns must contain `a=sendonly`.

---

## 5. Frigate configuration

Skip this section if you do not use Frigate. A reference config is in
[`../frigate/frigate.yaml`](../frigate/frigate.yaml).

```yaml
go2rtc:
  streams:
    doorbell:
      - rtsp://USER:PASS@192.168.1.9:8554/doorbell#backchannel=0
    doorbell_webrtc:
      - ffmpeg:doorbell#video=h264#raw=-r 15 -g 15 -keyint_min 15
      - rtsp://USER:PASS@192.168.1.9:8554/doorbell#backchannel=1
  webrtc:
    listen: :8555
    candidates:
      - 192.168.1.38:8555

cameras:
  doorbell:
    ffmpeg:
      inputs:
        # Consume go2rtc's restream, never the P4 directly: that way the
        # doorbell serves one RTSP client however many views are open.
        - path: rtsp://127.0.0.1:8554/doorbell_webrtc
          input_args: preset-rtsp-restream
          roles:
            - detect
    detect:
      enabled: true
      width: 640
      height: 480
      # 5 fps is plenty for a doorbell and spares the detector. Aiming higher is
      # pointless: the stream tops out around 16 fps.
      fps: 5
    live:
      # Frigate 0.15+ syntax: a dict of display name -> go2rtc stream.
      streams:
        Doorbell: doorbell_webrtc
    objects:
      track:
        - person
```

Frigate creates `camera.doorbell` in Home Assistant, and — this matters for the
Lovelace card — it makes it a **Frigate-type camera**, which is one of the
conditions for the microphone button.

> Every `{VARIABLE}` in a Frigate config must resolve. A single missing one
> invalidates the whole file, and the error message points at a line that is
> often not the culprit.

### "Two-way audio unavailable for this stream"

This Frigate message says nothing about your stream. **Frigate disables two-way
audio outside a secure context**, whatever the camera can do. Reach Home
Assistant over HTTPS and it goes away. See section 8.

---

## 6. Exposing the camera in Home Assistant

With Frigate, the entity already exists: `camera.doorbell`. Nothing to do.

Without Frigate, add a **Generic Camera** through the UI (Settings → Devices &
services → Add integration → Generic Camera):

| Field | Value |
|---|---|
| Still image URL | `http://192.168.1.38:1984/api/frame.jpeg?src=doorbell` |
| Stream source URL | `rtsp://127.0.0.1:8554/doorbell_webrtc` |
| RTSP transport | TCP |

The still-image URL is worth noting on its own: go2rtc serves a JPEG snapshot of
any stream at `/api/frame.jpeg?src=NAME`, which is handy in notifications.

---

## 7. The Lovelace card

### `frigate-card` or `advanced-camera-card`?

Both work, and that is deliberate. From the card's own source:

```ts
// Keep the old name around for backwards compatibility.
@customElement('frigate-card')
class FrigateCard extends AdvancedCameraCard {}
```

`custom:frigate-card` is a **pure alias** — same class, same schema, no
behavioural difference. A config written for one works verbatim with the other.
`advanced-camera-card` is simply the current name.

What does matter is the **installed version**, because the schema has moved:

| Version | Note |
|---|---|
| ≥ 7.27.0 | requires Home Assistant ≥ 2026.2 |
| 7.27.4 | latest stable at the time of writing |
| 8.0.0-rc | **restructures automations**: `triggers:` required, `conditions:` separated, `actions_not` removed, microphone state split. Read its release notes first |

The keys below are the 7.x ones. If the card rejects a key, its error names it —
that almost always means an older version.

### The card

```yaml
type: custom:advanced-camera-card
cameras:
  - camera_entity: camera.doorbell
    live_provider: go2rtc
    go2rtc:
      # The transcoded stream. Pointing at `doorbell` gives an empty view in
      # webrtc mode, because the P4 emits MJPEG.
      stream: doorbell_webrtc
      modes:
        - webrtc          # the only mode that carries audio
    # THIS is what connects the doorbell press to the camera. Without it,
    # pressing the button makes nothing appear: a camera card shows a camera,
    # it does not listen to anything else.
    triggers:
      entities:
        - binary_sensor.doorbell_p4_lvgl_bouton

live:
  # Have the view ready before the press, or you watch a spinner while the
  # visitor waits.
  preload: true
  # `microphone` matters: it unmutes when the mic connects, without which you
  # talk into a muted stream and conclude push-to-talk is broken.
  auto_unmute:
    - selected
    - visible
    - microphone
  microphone:
    always_connected: false
    disconnect_seconds: 90

view:
  default: live
  triggers:
    show_trigger_status: true
    filter_selected_camera: true
    actions:
      trigger: live          # the ring brings the view to live
      untrigger: default     # and back afterwards
    # How long the view stays live AFTER the ring clears. This is the one you
    # want -- `interaction_seconds` means something else entirely: how long a
    # user interaction suspends the triggers (300 s by default).
    untrigger_seconds: 30

menu:
  buttons:
    microphone:
      enabled: true          # defaults to false; this is what creates the button
      type: momentary        # hold to talk; 'toggle' for a latch
```

`binary_sensor.doorbell_p4_lvgl_bouton` assumes `name: doorbell-p4-lvgl` in your
ESPHome config. Check the real entity id in **Developer Tools → States** — it is
built from the device name, not the friendly name.

This is also where the 5-second hold on that sensor earns its keep: a 100 ms
pulse would not give the card time to react.

### Enum values, read from the card's schema

Not guesses — these come from `src/config/schema/` in the card's repository:

| Key | Accepted values |
|---|---|
| `live.auto_unmute` | `selected`, `visible`, `microphone` |
| `live.auto_mute` | `unselected`, `hidden`, `microphone` |
| `view.triggers.actions.trigger` | `default`, `live`, `media`, `none`, `update` |
| `view.triggers.actions.untrigger` | `default`, `none` |
| `menu.buttons.microphone.type` | `momentary`, `toggle` |

> There is no `capabilities: force:` key. It appears in some configs found
> online, but not in 7.27.4 nor on `main` — the schema has only `disable` and
> `disable_except`. Copying it will fail validation.

### A complete dashboard

```yaml
type: vertical-stack
cards:
  - type: custom:advanced-camera-card
    cameras:
      - camera_entity: camera.doorbell
        live_provider: go2rtc
        go2rtc:
          stream: doorbell_webrtc
          modes: [webrtc]
        triggers:
          entities:
            - binary_sensor.doorbell_p4_lvgl_bouton
    live:
      preload: true
      auto_unmute: [selected, visible, microphone]
    view:
      default: live
      triggers:
        actions:
          trigger: live
          untrigger: default

  - type: entities
    entities:
      - entity: event.doorbell_p4_lvgl_sonnette
        name: Last press
      - entity: button.doorbell_p4_lvgl_sonner
        name: Ring (test)

  - type: entities
    title: Audio
    entities:
      - entity: sensor.doorbell_p4_lvgl_niveau_micro
        name: Microphone level
      - entity: sensor.doorbell_p4_lvgl_paquets_audio_recus
        name: Downstream voice received
      - entity: button.doorbell_p4_lvgl_bip_de_test
        name: Test beep
```

### The WebRTC Camera card (AlexxIT)

A far simpler card, with no camera-type restriction. Install **both** parts from
HACS — the *WebRTC Camera* **integration**, not only the card. The integration
provides the WebSocket proxy the card talks to; with the card alone you get a
permanent "Custom element doesn't exist".

```yaml
type: custom:webrtc-camera

# Frigate's embedded go2rtc -- NOT the one this integration starts on its own.
# Left out, the integration downloads and runs its own go2rtc, which has never
# heard of `doorbell_webrtc` and shows an empty card. Point it at the machine
# running Frigate, port 1984.
server: http://192.168.1.38:1984/

streams:
  # 1 - watching. Deliberately WITHOUT `microphone`; see below.
  - url: doorbell_webrtc
    name: Watch
    mode: webrtc

  # 2 - talking. Select it to speak, go back to stream 1 to stop.
  - url: doorbell_webrtc
    name: Talk
    mode: webrtc
    media: video,audio,microphone

ui: true               # built-in controls, and the stream selector
muted: false           # you want to hear the visitor
background: false      # stop the stream when the card is off screen
intersection: 0.75
poster: doorbell_webrtc
```

#### This card has no push-to-talk button

Worth knowing before choosing it, because it is the one real difference from the
Advanced Camera Card. From `video-rtc.js`:

```js
if (this.media.includes('microphone')) {
    const media = await navigator.mediaDevices.getUserMedia({audio: true});
```

The microphone is opened **when the stream connects**, not when you press
anything. A stream carrying `microphone` therefore keeps your microphone live —
and audible at the doorbell — for as long as that stream is selected.

That is why the config above declares the stream twice. The stream selector *is*
the talk button: "Watch" by default, "Talk" while you are speaking. It costs one
click more than a momentary button, and in exchange nothing can leave your
microphone open by accident.

If you would rather have a real hold-to-talk button, that is the Advanced Camera
Card's `menu.buttons.microphone.type: momentary`, and the reason to keep it
despite its heavier configuration.

#### `mode: webrtc` is not optional here either

`mse`, `hls` and `mjpeg` carry no microphone — the code above only runs on the
WebRTC path. And point `url:` at `doorbell_webrtc`, never at `doorbell`: the
latter is MJPEG, which WebRTC cannot carry, and it gives the backchannel up with
`#backchannel=0`.

Everything in [§8](#8-two-way-audio) still applies unchanged: HTTPS is required
for the browser to hand over a microphone at all, and exactly one go2rtc source
may hold the backchannel.

---

## 8. Two-way audio

Talking *to* the visitor is the hardest part of this chain. Listening works with
no special effort — the stream carries audio continuously, which also means the
microphone is live whenever anything is watching.

### HTTPS is not optional

A browser grants microphone access (`getUserMedia`) **only in a secure
context**: HTTPS, or `localhost`. A plain LAN address such as
`http://192.168.1.38:8123` is not one. The button will appear and fail.

This is a browser rule, not a limitation of this project or of any card. Three
ways to satisfy it:

- **Nabu Casa** (`https://….ui.nabu.casa`) or **Tailscale**
  (`https://….ts.net`) — valid HTTPS, no certificate to manage;
- open the page **from the machine running go2rtc**, where
  `http://localhost:1984` *is* a secure context;
- for a one-off test, mark the origin trusted in Chrome:
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure`.

The trap: it is the **URL in the address bar** that decides. The same Home
Assistant reached over `http://192.168.1.x:8123` will not have a microphone,
however it is installed.

### `backchannel: always`

The most confusing failure in the whole chain, because everything works and
nothing shows.

ONVIF says to announce the `sendonly` track **only** to a client that sends
`Require: www.onvif.org/ver20/backchannel`. But go2rtc, Frigate and the cards
decide whether a camera can be talked to by **probing** the stream — an ordinary
DESCRIBE, without that header. The track is not announced to the probe, so
Frigate concludes "no two-way audio", the card hides the button, and nobody ever
requests the backchannel.

A capability that only exists on request is invisible to anything that does not
already know it exists. Hence:

```yaml
rtsp_server:
  backchannel: always   # `auto` = strict ONVIF, the default
```

The device's status block confirms which side you are on:

```
clients=2 playing=2 | negotiated across all sessions: video=yes audio=yes backchannel=yes
```

and, when it is not negotiated, says which of the two cases you are in — nobody
has asked to talk yet (normal), or somebody asked and gave up (worth looking at).

### The Advanced Camera Card's conditions

The card's documentation lists these as **all mandatory**. If one is missing the
microphone button is not greyed out — it does not appear at all, which looks
like a bug and is configuration.

| # | Condition |
|---|---|
| 1 | The camera has an audio output |
| 2 | go2rtc can do two-way audio with it |
| 3 | Home Assistant is reachable over **HTTPS** |
| 4 | A **Frigate-type camera** — not a Generic Camera |
| 5 | `live_provider: go2rtc` — never `ha` |
| 6 | `modes: [webrtc]` only |
| 7 | The microphone button is enabled in `menu.buttons` |

Condition 6 has a consequence people find late: **`mjpeg` mode excludes
push-to-talk by construction**. Falling back to MJPEG because WebRTC stutters
means giving up talking.

### The remaining obstacle: two sources

Even with all seven satisfied, the card only shows the microphone button when
there is **one consumer** on the stream
([advanced-camera-card #1888](https://github.com/dermotduffy/advanced-camera-card/issues/1888)).

The MJPEG chain forces two, by construction: ffmpeg only pulls, so it cannot
carry the backchannel, and a second source is needed for the voice. This is not
a setting to find — it follows from the P4 emitting MJPEG.

If the button does not appear, the fallback is go2rtc's own page, which has no
such constraint (but still needs the secure context above):

```
http://localhost:1984/stream.html?src=doorbell_webrtc&mode=webrtc&media=video+audio+microphone
```

It embeds in a dashboard with a plain `iframe` card.

---

## 9. The doorbell press

A doorbell has four stages: **press → something rings → I see who it is → I
answer.** Here is where each happens.

| Stage | Who does it | Status |
|---|---|---|
| 1. Press | the P4 publishes `event…sonnette`, `binary_sensor…_bouton` and the `esphome.doorbell_pressed` event | nothing to do, it is in the YAML |
| 2. **Something rings** | a Home Assistant **automation** | **you must write it — section 10** |
| 3. I see who it is | the card's `triggers:` (section 7) and/or the notification | section 7 |
| 4. I answer | two-way audio | listening: yes. Talking: needs HTTPS, section 8 |

Stage 2 is the one everyone forgets, and it is the one that makes the whole
thing feel broken. **Home Assistant does not chime on its own.** The P4 does its
job scrupulously — it announces the press three different ways — but until
something listens and makes a noise, the press only changes a value in a
database.

---

## 10. Automations

### Which trigger

| Trigger | Why |
|---|---|
| **`event` entity** | Home Assistant's **native** doorbell entity. It carries a timestamp, appears in the graphical editor, and a press cannot be missed |
| `binary_sensor` | for conditions and cards. Held 5 s, so it is visible in Developer Tools → States |
| `esphome.doorbell_pressed` | a raw bus event, if you would rather not depend on an entity id |

```yaml
# Recommended: the event entity. Any state change is a new press.
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
```

```yaml
# Alternative: the bus event, if you would rather not look up an entity id.
triggers:
  - trigger: event
    event_type: esphome.doorbell_pressed
```

Find the real entity id in **Developer Tools → States**, filtering on
`sonnette`. ESPHome builds it from the device name, not the friendly name. The
notification service is `notify.mobile_app_<phone>`; the exact list is under
Developer Tools → Actions.

> **Check the press arrives before writing anything.** Filter on `sonnette` in
> Developer Tools → States and press the button: the event entity's timestamp
> must change. If it does not, the automation is not the problem — look at
> `esphome logs`, where the line `ring: sequence declenchee` tells you whether
> the press even reached the script.

### Chime and notification

A notification is only seen if the phone is in your hand. To actually *ring*,
you need a speaker.

```yaml
alias: Doorbell - chime and notify
mode: single
max_exceeded: silent
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
actions:
  # Quieter at night.
  - action: media_player.volume_set
    target:
      entity_id:
        - media_player.kitchen
        - media_player.bedroom
    data:
      volume_level: >-
        {{ 0.6 if is_state('sun.sun', 'below_horizon') else 0.75 }}
    continue_on_error: true

  - parallel:
      - action: media_player.play_media
        continue_on_error: true
        target:
          entity_id: media_player.kitchen
        data:
          # A file in /config/www/, served under /local/.
          media_content_id: /local/sounds/doorbell.mp3
          media_content_type: music

      - action: notify.all_phones
        continue_on_error: true
        data:
          title: Doorbell
          message: Someone is at the door
          data:
            # Tapping the notification body opens the view (Android).
            clickAction: /lovelace/doorbell
            # `tag` makes a second press REPLACE the notification instead of
            # stacking another one.
            tag: doorbell-ringing
            channel: Doorbell
            importance: high
            ttl: 0
            image: /api/camera_proxy/camera.doorbell
            actions:
              - action: URI
                title: Answer
                uri: /lovelace/doorbell
              - action: IGNORE
                title: Ignore

  # Anti-spam: an impatient visitor does not launch three overlapping chimes.
  - delay:
      seconds: 15
```

Three details that separate a pleasant doorbell from an unbearable one:

- **`continue_on_error: true` everywhere.** A television that is switched off
  must not stop the kitchen chime from sounding.
- **`tag:` on the notification.** Without it, three presses give three stacked
  notifications; with it, the last replaces the previous.
- **`mode: single` + `max_exceeded: silent` + the trailing delay.** Without
  them, three presses start three overlapping sequences.

A spoken announcement instead of a chime:

```yaml
  - action: tts.speak
    target:
      entity_id: tts.piper
    data:
      cache: true
      media_player_entity_id: media_player.kitchen
      message: Someone is at the door
```

To test without going to the door: **Settings → Automations → ⋮ → Run**. Or the
device's own `button…sonner`, which runs exactly the same sequence as a press.

### Ringing until someone answers

The [dahua-vto-on-home-assistant](https://github.com/felipecrs/dahua-vto-on-home-assistant)
project makes a point worth stealing: **a doorbell is not an event, it is a
state.** "Someone is waiting at the door" lasts until you answer or give up.

Our `event` and 5-second `binary_sensor` say "a press happened", not "someone is
still waiting" — which is why the chime above rings once. To ring until
answered, add a helper (Settings → Devices & services → Helpers → Toggle) named
`input_boolean.doorbell_calling`, set it at the start of the automation, and:

```yaml
  - repeat:
      while:
        - condition: state
          entity_id: input_boolean.doorbell_calling
          state: "on"
        - condition: template
          value_template: "{{ repeat.index <= 6 }}"
      sequence:
        - action: media_player.play_media
          continue_on_error: true
          target:
            entity_id: media_player.kitchen
          data:
            media_content_id: /local/sounds/doorbell.mp3
            media_content_type: audio/mp3
        - delay:
            seconds: 5
  - action: input_boolean.turn_off
    target:
      entity_id: input_boolean.doorbell_calling
```

Clearing the helper — from the card, or from an "Answer" button — stops the
chime.

---

## 11. Opening the page automatically

**Home Assistant cannot make a browser change page.** There is no native service
for it. An automation acts on devices — speakers, lights, phones — not on the
tab in front of you.

What a notification's `uri:` really does is *arm an action*: the page opens when
you **tap** it, not the instant it rings. And the card's `triggers:` block
switches a card that is *already on screen*; it does not go and fetch it.

### Check the view exists first

`/lovelace/doorbell` only exists if a view has exactly the **URL path**
`doorbell`. That is set in the dashboard → ✏️ → the view's tab → ⚙️ → "URL". It
is not the view's title: a view titled "Doorbell" may well have the path
`view_2`. Type the address by hand; if you land on an empty page or on the
default dashboard, the path is wrong and no notification will lead there.

On a dashboard other than the default, the full path includes its name:
`/doorbell-dashboard/doorbell`. Navigate to the view and copy the address bar.

### The three ways

| | How | Automatic? |
|---|---|---|
| Notification with a `URI` action | you tap the notification | no — a gesture |
| A card already on screen, with `triggers:` | it switches to live on its own | yes, but only if the page is already open |
| **Browser Mod** or **Fully Kiosk** | opens a window, or navigates, on a named browser | **yes, genuinely** |

### Browser Mod

[Browser Mod](https://github.com/thomasloven/hass-browser_mod) (via HACS) gives
Home Assistant control of a specific browser — what you want for a wall tablet
or a desk PC.

**HACS only does half the installation**, and it does not say so. After the
restart Browser Mod appears nowhere, and it looks like the install failed. The
missing step:

1. HACS → Browser Mod → **Download**;
2. **restart** Home Assistant (a config reload is not enough);
3. **Settings → Devices & services → "+ Add integration" → "Browser Mod"** ←
   *this is the step people miss*;
4. hard-refresh the browser (**Ctrl+Shift+R**), or the frontend module is not
   loaded and nothing registers.

Then each browser you want to control must **register itself**: open Home
Assistant on it, go to the **Browser Mod** panel, and enable **Register**. Give
it a readable name — that is what the automation targets. An unregistered
browser is not a visible error: the automation runs, finds no target, and
nothing happens.

```yaml
alias: Doorbell - show the camera
mode: single
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
actions:
  # A window over whatever is on screen: nothing to dismiss, it closes itself.
  - action: browser_mod.popup
    data:
      title: Someone is at the door
      size: wide
      timeout: 60000
      dismissable: true
      browser_id:
        - hall-tablet
      content:
        type: custom:advanced-camera-card
        cameras:
          - camera_entity: camera.doorbell
            live_provider: go2rtc
            go2rtc:
              stream: doorbell_webrtc
              modes: [webrtc]
        live:
          preload: true
          auto_unmute: [selected, visible, microphone]
        menu:
          buttons:
            microphone:
              enabled: true
              type: momentary
```

A popup beats navigation: it does not abandon what you were doing, and it closes
on its own. `browser_mod.navigate` exists if you prefer to change page outright.

### Fully Kiosk

If your wall tablet runs Fully Kiosk Browser, it is simpler:

```yaml
  - action: fully_kiosk.load_url
    target:
      device_id: <your tablet>
    data:
      url: https://YOUR-HA-HOST/lovelace/doorbell
    continue_on_error: true
```

---

## 12. Diagnosing the audio

Do not start in Home Assistant. The device answers on its own, and that
separates a hardware fault from a chain fault in two minutes.

1. **Test beep** — the `Bip de test` button, or the on-screen one. No beep means
   the fault is the speaker, not the network.
2. **Local loopback** — the `Test audio` switch. Speak in front of the doorbell:
   you should hear yourself. If you do, **the device's whole audio chain is
   sound** and what remains is upstream. It stops itself after two minutes.
3. **Level** — the `Niveau micro` sensor, in dBFS.

Reading the microphone level:

| Reading | Diagnosis |
|---|---|
| `Micro actif` in fault | the source delivers **nothing**: wrong `microphone_id`, codec not started, wrong I2S pins |
| −100 dBFS while speaking | the source delivers **silence**: wrong I2S slot, dead capsule, gain at zero |
| −60 to −40 dBFS | it hears but faintly: raise `mic_gain_db` (codec) or `gain:` |
| −30 to −6 dBFS | correct |
| above −3 dBFS | clipping, lower the gain |

For the **downstream** voice, the `Paquets audio recus` counter settles it alone:

| While you press talk | Where the fault is |
|---|---|
| stays at 0 | nothing reaches the P4: backchannel not negotiated, or go2rtc — section 8 |
| it rises, but you hear nothing | the device receives fine: amplifier, volume, or speaker (run the test beep) |

And on the output side, `Octets audio proposes` versus `acceptes`:

| The two counters | Meaning |
|---|---|
| offered = 0 | nothing was ever sent to the speaker — press the test beep first |
| offered rises, **accepted stays at 0** | the speaker component refuses everything: stopped, or the format displeases it |
| both rise together, still no sound | the audio left the software: amplifier GPIO, codec volume, or wiring |
| accepted < offered | it cannot keep up; the sound will be chopped |

---

## 13. Troubleshooting

| Symptom | Likely cause |
|---|---|
| go2rtc cannot connect to the P4 | the IP changed: DHCP reservation, or use the mDNS name (section 1) |
| WebRTC shows a black screen, and the P4 logs only a `SETUP trackID=1` | go2rtc discarded the JPEG video, which WebRTC cannot carry: the `ffmpeg:…#video=h264` transcode is missing (section 4) |
| `probe` on `doorbell_webrtc` still shows `JPEG` | the ffmpeg transcode is failing — drop `#hardware` if the machine has no VA-API encoder |
| Port 1984 does not answer | you are aiming at the P4 instead of go2rtc: the P4 only serves RTSP on 8554 |
| Image fine, then clients refused (`refusing …: already serving 2 clients`) | the MJPEG chain takes 2 RTSP sessions: raise `max_clients` |
| Push-to-talk worked once and never again | two go2rtc sources are requesting the backchannel; only one may |
| No microphone button, even over HTTPS | the P4 is announcing the track on request only: set `backchannel: always` (section 8) |
| Microphone button present but silent, or permission denied | not a secure context — check the URL in the address bar is HTTPS |
| `Custom element not found: …` | the Lovelace card is not installed — HACS, then Ctrl+Shift+R |
| "Camera initialisation failed" (Advanced Camera Card) | the `camera_entity` does not exist (section 6) |
| Image fine, no upstream sound | no `microphone` in `media` (webrtc-camera) |
| The sound cuts out when you talk | expected: `half_duplex: true` mutes the microphone while the far end speaks |
| Howling | the loopback monitor is on, or the amplifier stays powered — see `hardware.md` |
| Smooth image but high CPU on the HA machine | the MJPEG → H.264 transcode, unavoidable for WebRTC. Lower `framerate` or `jpeg_quality`, or watch in `mjpeg`/`mse` mode |
| No image in the card, but `doorbell` visible in go2rtc | the card points at `doorbell`; use `doorbell_webrtc` |
| The image freezes while the stream is active | the LVGL preview was switched off without handing the V4L2 dequeue back — see `set_drive_camera()` |
| `RTSP: unsupported transport` | a client is forcing UDP; this server is TCP-interleaved only |
| `Unable to find action with the name 'rtsp_server.…'` | ESPHome is compiling an old copy: wrong `ref:`, or the 24 h cache. Set `refresh: 0s` and delete `.esphome/external_components` |
| `codec: h264` rejected at compile time | intentional — H.264 was removed, see the README |
| Audio never starts, and the I2C scan shows no device at `0x40` | the board has no ES7210. `fdaudio` requires one; use ESPHome's `es8311` with `use_microphone: true` instead — see `hardware.md` |
