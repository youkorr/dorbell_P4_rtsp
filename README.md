# dorbell_P4_rtsp

Composant externe ESPHome : **serveur RTSP avec audio bidirectionnel (backchannel
ONVIF) pour ESP32-P4**, pour une sonnette connectée.

```
OV5647 ──MIPI CSI──► ESP32-P4 ──MJPEG + G.711 / RTSP──► go2rtc ──WebRTC──► Home Assistant
                                ◄──── G.711 backchannel (parler) ────
```

Le composant n'initialise aucun écran et n'embarque aucune bibliothèque
graphique : il fonctionne en *headless*. Il sait toutefois partager la caméra
avec LVGL quand la carte a un écran, pour un banc de test avec aperçu local
(voir `doorbell-lvgl.yaml`).

## Ce que fait le composant

| | |
|---|---|
| Vidéo (défaut) | **MJPEG**, encodeur JPEG matériel du P4, RTP selon RFC 2435 |
| Vidéo (option) | **H.264** matériel, RTP selon RFC 6184 — pour test/comparaison |
| Source vidéo | caméra `esp_cam_sensor` partagée avec LVGL, **ou** V4L2 direct (`esp_video`) |
| Audio montant | micro → G.711 (PCMU/PCMA) 8 kHz → RTP |
| Audio descendant | RTP backchannel ONVIF → G.711 → haut-parleur |
| Source audio | composants ESPHome `microphone`/`speaker` (fdaudio, i2s_audio…) **ou** broches I2S brutes |
| Transport | RTP entrelacé sur la connexion RTSP TCP (RFC 2326 §10.12) |
| Chemin des données | zéro copie : le tampon DMA de la caméra est encodé sur place |
| Authentification | Basic, optionnelle |

### MJPEG ou H.264 ?

|  | MJPEG (défaut) | H.264 (option) |
|---|---|---|
| Encodeur | JPEG matériel | H.264 matériel |
| Débit typique (800×640 @ 15 fps) | ~6-10 Mbit/s | ~1,5 Mbit/s |
| Partage de la caméra avec LVGL | **oui** | non (exige la source V4L2 en YUV420) |
| Contrainte de résolution | multiple de 8, ≤ 2040 px | multiple de **16** |
| Vers WebRTC via go2rtc | **transcodage ffmpeg** côté Home Assistant | passe-plat, aucun réencodage |
| Vers MSE / MJPEG / vignettes | direct | direct |

MJPEG est le défaut : il fonctionne avec l'aperçu LVGL, il n'a aucune contrainte
d'alignement gênante, et chaque trame est autonome (pas d'attente de trame clé).
Le prix à payer est le débit, et le fait que WebRTC ne sait pas transporter du
MJPEG : go2rtc devra lancer ffmpeg pour le convertir. Si votre machine Home
Assistant est modeste, basculez sur `codec: h264` une fois le montage validé.

Côté audio, G.711 traverse en revanche toute la chaîne **sans transcodage** dans
les deux sens : les navigateurs encodent et décodent nativement le PCMU/PCMA.

## Installation

```yaml
external_components:
  # `rtsp_server` et l'option `enable_h264` d'esp_video vivent pour l'instant sur
  # cette branche de travail ; remplacez par `main` une fois la PR fusionnée.
  - source:
      type: git
      url: https://github.com/youkorr/dorbell_P4_rtsp
      ref: claude/esp32-p4-doorbell-rtsp-kodm6j
    components: [rtsp_server]
  - source:
      type: git
      url: https://github.com/youkorr/esphome_esp-video
      ref: claude/esp32-p4-doorbell-rtsp-kodm6j
    components: [esp_video]

esp_video:
  id: video
  i2c_id: cam_i2c
  enable_isp: true
  # enable_h264: true    # uniquement si vous passez rtsp_server en codec: h264

rtsp_server:
  id: doorbell_stream
  port: 8554
  path: /doorbell
  video:
    codec: mjpeg
    framerate: 15
    jpeg_quality: 25
  audio:
    microphone:
      bclk_pin: GPIO20
      lrclk_pin: GPIO21
      din_pin: GPIO22
    speaker:
      i2s_port: 1
      bclk_pin: GPIO23
      lrclk_pin: GPIO26
      dout_pin: GPIO27
```

Deux configurations complètes et commentées :

- [`doorbell.yaml`](doorbell.yaml) — sonnette *headless* (sans écran), micro
  INMP441 et ampli MAX98357A câblés en I2S ;
- [`doorbell-lvgl.yaml`](doorbell-lvgl.yaml) — banc de test avec écran
  MIPI-DSI 1024×600, aperçu caméra LVGL et audio via le codec de la carte
  (`fdaudio` / ES8311 + ES7210). La caméra est partagée entre l'aperçu et le
  flux RTSP.

## Documentation

- [`docs/hardware.md`](docs/hardware.md) — brochage GPIO, câblage I2S, contraintes de l'ESP32-P4, acoustique
- [`docs/home-assistant.md`](docs/home-assistant.md) — carte Lovelace, bouton push-to-talk, automatisation de notification
- [`go2rtc/go2rtc.yaml`](go2rtc/go2rtc.yaml) — configuration go2rtc prête à l'emploi

## Référence de configuration

### `rtsp_server`

| Option | Défaut | Description |
|---|---|---|
| `port` | `8554` | port d'écoute RTSP |
| `path` | `/doorbell` | chemin de l'URL RTSP |
| `username` / `password` | — | authentification Basic ; absentes ⇒ flux ouvert |
| `max_clients` | `2` | connexions RTSP simultanées |
| `packet_size` | `1400` | taille maximale d'un paquet RTP |

### `rtsp_server.video`

| Option | Défaut | Description |
|---|---|---|
| `codec` | `mjpeg` | `mjpeg` ou `h264` |
| `camera_id` | — | caméra `esp_cam_sensor` à partager (MJPEG uniquement) ; absent ⇒ V4L2 direct |
| `drive_camera` | `true` | passez à `false` si `lvgl_camera_display` fait déjà le dequeue V4L2 |
| `device` | `/dev/video0` | périphérique de capture MIPI-CSI (source directe) |
| `encoder_device` | `/dev/video11` | périphérique d'encodage H.264 |
| `framerate` | `15` | images par seconde |
| `jpeg_quality` | `25` | MJPEG : 1 (min) à 100 (max) |
| `bitrate` | `1500000` | H.264 : débit cible ; l'encodeur plafonne à 2 500 000 |
| `gop` | `15` | H.264 : période entre trames clés, en images |
| `min_qp` / `max_qp` | `25` / `40` | H.264 : bornes de quantification |
| `buffer_count` | `2` | tampons de capture (2 = double buffering) |
| `vflip` / `hflip` | `false` | retournement, appliqué par le capteur |

### `rtsp_server.audio`

| Option | Défaut | Description |
|---|---|---|
| `codec` | `pcmu` | `pcmu` (µ-law) ou `pcma` (A-law) |
| `sample_rate` | `16000` | fréquence PCM (`8000` ou `16000`) ; le réseau reste à 8 kHz |
| `microphone_id` | — | composant ESPHome `microphone` (fdaudio, i2s_audio…) |
| `speaker_id` | — | composant ESPHome `speaker` |
| `half_duplex` | `true` | coupe le micro pendant que le correspondant parle |
| `talk_timeout` | `300ms` | silence après lequel la communication est réputée terminée |
| `microphone.mode` | `std` | `std` (I2S classique) ou `pdm` |
| `microphone.bits_per_sample` | `32` | 32 pour un INMP441/ICS-43434 |
| `microphone.channel` | `left` | slot I2S ; `left` = broche L/R à la masse |
| `microphone.gain` | `4.0` | gain numérique appliqué avant compression |
| `speaker.bits_per_sample` | `16` | 16 pour un MAX98357A |
| `speaker.volume` | `0.8` | atténuation numérique (0.0 – 1.0) |

Deux sources audio possibles, exclusives l'une de l'autre :

- `microphone_id` + `speaker_id` — réutilise des composants ESPHome existants,
  donc le codec de la carte reste partagé avec les autres usages ;
- les blocs `microphone` / `speaker` — pilotage direct des broches I2S, pour une
  sonnette minimale (INMP441 + MAX98357A).

Sans haut-parleur (`speaker_id` ou bloc `speaker` absents), le backchannel est
désactivé : le flux reste descendant et le SDP n'annonce plus de piste
`sendonly`.

### Déclencheurs

`on_client_connected`, `on_client_disconnected`, `on_talk_start`, `on_talk_end`.

`on_talk_start` / `on_talk_end` encadrent la réception d'audio depuis Home
Assistant : c'est là qu'on allume l'ampli et une LED (voir `doorbell.yaml`).

### Depuis une lambda

```cpp
id(doorbell_stream).client_count();   // uint8_t
id(doorbell_stream).is_streaming();   // au moins un client en PLAY
id(doorbell_stream).is_talking();     // audio backchannel en cours
```

## Résolution de l'image

Le contrôleur MIPI-CSI de l'ESP32-P4 **ne redimensionne pas** : la résolution
vient du format sélectionné dans le capteur au moment de l'initialisation. En
source V4L2 directe, le composant lit ce format avec `VIDIOC_G_FMT` et
configure l'encodeur en conséquence — il n'y a rien à déclarer côté
`rtsp_server`. Avec `camera_id`, c'est la clé `resolution:` du composant
`esp_cam_sensor` qui décide.

Formats OV5647 disponibles dans `esphome_esp-video`, sélectionnés par
`CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT` :

| Index | Résolution | Format | Convient à l'encodeur H.264 ? |
|---|---|---|---|
| 0 | 800×1280 @ 50 fps | RAW8 | oui (portrait — plutôt adapté à une sonnette) |
| 1 | 800×640 @ 50 fps | RAW8 | oui |
| 2 | 800×800 @ 50 fps | RAW8 | oui |
| 3 | 1920×1080 @ 30 fps | RAW10 | **non** — 1080 n'est pas un multiple de 16 |
| 4 | 1280×960 @ 45 fps | RAW10 | oui — recommandé en paysage |

En H.264, l'encodeur matériel exige une largeur **et** une hauteur multiples de
16. En MJPEG, RFC 2435 impose des multiples de 8 et au plus 2040 px par côté. Le
composant vérifie ces conditions à l'ouverture et refuse de démarrer avec un
message explicite plutôt que de produire un flux illisible.

## Limites connues

- **TCP uniquement.** Le transport UDP (`RTP/AVP;unicast`) n'est pas implémenté :
  un `SETUP` en UDP reçoit un `461 Unsupported Transport`. C'est le mode par
  défaut de go2rtc ; pour VLC il faut forcer le TCP (`--rtsp-tcp`).
- **Pas d'annulation d'écho.** Le mode half duplex remplace l'AEC ; voir la
  section acoustique de [`docs/hardware.md`](docs/hardware.md).
- **Un seul backchannel actif.** Ne demandez pas le backchannel depuis deux
  sources go2rtc simultanément (voir les commentaires de `go2rtc.yaml`).
- **Pas de RTCP.** Les paquets RTCP reçus sont ignorés ; aucun rapport n'est
  émis. Ni go2rtc ni ffmpeg n'en ont besoin ici.
- **MJPEG et WebRTC.** WebRTC ne transporte pas de MJPEG : go2rtc lancera ffmpeg
  pour transcoder. Les modes MSE, MJPEG et les vignettes fonctionnent en direct.
  `codec: h264` supprime ce transcodage.
- **H.264 et LVGL sont exclusifs.** L'encodeur H.264 consomme du YUV420 lu
  directement sur `/dev/video0`, il ne peut donc pas partager la caméra avec
  l'aperçu LVGL (qui délivre du RGB565).

## Détails d'implémentation

- **Zéro copie.** En MJPEG, le tampon de la caméra (PSRAM compatible DMA) est
  donné tel quel à `jpeg_encoder_process` ; en H.264, il est passé à l'encodeur
  en `V4L2_MEMORY_USERPTR` sur sa file OUTPUT. Dans les deux cas, seul le flux
  compressé est relu.
- **Un seul thread réseau.** Un unique tâche FreeRTOS possède la socket
  d'écoute, toutes les sessions et l'envoi : aucun verrou sur le chemin critique.
  Les tâches vidéo et audio déposent leurs paquets RTP dans un tampon circulaire
  et ne bloquent jamais sur le réseau — en cas de saturation, les paquets sont
  abandonnés plutôt que de figer l'encodage.
- **MJPEG (RFC 2435).** Les en-têtes JFIF ne sont pas transmis : le récepteur
  les reconstruit depuis l'en-tête RTP de 8 octets, et les tables de
  quantification voyagent en intrabande dans le premier paquet de chaque trame
  (`Q = 255`). Le sous-échantillonnage est déduit du marqueur SOF.
- **SPS/PPS (H.264).** Extraits en continu du flux et republiés dans le SDP
  (`sprop-parameter-sets`, `profile-level-id`) ainsi qu'en intrabande, pour que
  les clients qui arrivent en cours de route puissent décoder.
- **Backchannel.** La piste `sendonly` n'est annoncée que si le client envoie
  `Require: www.onvif.org/ver20/backchannel` — un client ordinaire (VLC, ffmpeg)
  voit un flux à deux pistes parfaitement normal.

## Licence

Voir [LICENSE](LICENSE).
