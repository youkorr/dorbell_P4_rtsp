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
| Vidéo | **MJPEG**, encodeur JPEG matériel du P4, RTP selon RFC 2435 |
| Source vidéo | caméra `esp_cam_sensor` partagée avec LVGL, **ou** V4L2 direct (`esp_video`) |
| Audio montant | micro → G.711 (PCMU/PCMA) 8 kHz → RTP |
| Audio descendant | RTP backchannel ONVIF → G.711 → haut-parleur |
| Source audio | composants ESPHome `microphone`/`speaker` (fdaudio, i2s_audio…) **ou** broches I2S brutes |
| Transport | RTP entrelacé sur la connexion RTSP TCP (RFC 2326 §10.12) |
| Chemin des données | zéro copie : le tampon DMA de la caméra est encodé sur place |
| Diagnostic audio | vumètre micro, compteurs de paquets, boucle locale, bip de test |
| Authentification | Basic, optionnelle |

### Pourquoi MJPEG et seulement MJPEG

L'ESP32-P4 possède bien un encodeur H.264 matériel, et une version précédente de
ce composant l'exposait. Il a été retiré, pour deux raisons qui ne se contournent
pas :

- **le P4 ne sait pas décoder du H.264.** Ce qu'il encode, il ne peut pas le
  réafficher : l'aperçu LVGL local était donc impossible en H.264 (il faudrait
  câbler un décodeur logiciel comme `edge264`) ;
- **les deux formats sont incompatibles dans le même flux de caméra.**
  L'encodeur H.264 consomme du YUV420, LVGL affiche du RGB565. Une seule caméra
  ne pouvait pas alimenter les deux : il fallait choisir entre l'écran et le
  flux.

MJPEG garde une unique trame RGB565 qui nourrit l'écran *et* le réseau, sans
contrainte d'alignement gênante et sans attente de trame clé. Le prix à payer est
le débit (~6-10 Mbit/s en 800×640 @ 15 fps), et le fait que WebRTC ne sache pas
transporter du MJPEG : go2rtc lancera ffmpeg pour le convertir. Les modes MSE,
MJPEG et les vignettes, eux, passent en direct.

Côté audio, G.711 traverse en revanche toute la chaîne **sans transcodage** dans
les deux sens : les navigateurs encodent et décodent nativement le PCMU/PCMA.

## Installation

```yaml
external_components:
  # `rtsp_server` vit pour l'instant sur cette branche de travail ; remplacez
  # par `main` une fois la PR fusionnée.
  - source:
      type: git
      url: https://github.com/youkorr/dorbell_P4_rtsp
      ref: claude/doorbell-p4-rtsp-audio-gkdosc
    components: [rtsp_server]
  - source:
      type: git
      url: https://github.com/youkorr/esphome_esp-video
      ref: claude/doorbell-p4-rtsp-audio-gkdosc
    components: [esp_video]

esp_video:
  id: video
  i2c_id: cam_i2c
  enable_isp: true

rtsp_server:
  id: doorbell_stream
  port: 8554
  path: /doorbell
  video:
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
| `camera_id` | — | caméra `esp_cam_sensor` à partager ; absent ⇒ V4L2 direct |
| `drive_camera` | `true` | passez à `false` si `lvgl_camera_display` fait déjà le dequeue V4L2 |
| `device` | `/dev/video0` | périphérique de capture MIPI-CSI (source directe) |
| `framerate` | `15` | images par seconde |
| `jpeg_quality` | `25` | 1 (min) à 100 (max) |
| `buffer_count` | `2` | tampons de capture (2 = double buffering) |
| `vflip` / `hflip` | `false` | retournement, appliqué par le capteur |

`codec:` n'accepte plus que `mjpeg`, et peut être omis. `codec: h264` est refusé
à la validation, avec l'explication ci-dessus — plutôt que de servir
silencieusement du MJPEG à une configuration qui comptait sur le passe-plat
WebRTC.

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

### Actions

| Action | Ce qu'elle fait |
|---|---|
| `rtsp_server.set_loopback` | renvoie le micro dans le haut-parleur, localement |
| `rtsp_server.play_test_tone` | joue un bip (`frequency`, `duration`) |

### Depuis une lambda

```cpp
id(doorbell_stream).client_count();   // uint8_t
id(doorbell_stream).is_streaming();   // au moins un client en PLAY
id(doorbell_stream).is_talking();     // audio backchannel en cours
id(doorbell_stream).set_drive_camera(true);  // reprendre le dequeue V4L2
```

## « Je ne sais pas si le micro et l'audio fonctionnent »

C'est la panne la plus difficile à situer de toute la chaîne : entre la capsule
et le navigateur, chaque étage peut avaler le son sans rien dire. Le composant
mesure donc aux deux points qui comptent — juste après la capture, juste avant la
restitution — et l'appareil répond tout seul, sans go2rtc ni navigateur.

### Les deux tests, dans cet ordre

1. **Bip de test** (`rtsp_server.play_test_tone`) — prouve le haut-parleur seul.
   Pas de bip ⇒ inutile de chercher plus loin : ampli coupé, `speaker_id`
   absent, volume à zéro.
2. **Boucle locale** (`rtsp_server.set_loopback`) — parlez devant la sonnette,
   vous devez vous entendre. Si oui, la capture, le gain, la compression G.711 et
   la restitution sont tous bons, et ce qui reste est dans le réseau, go2rtc ou
   Home Assistant.

Les deux sont câblés dans `doorbell.yaml` et `doorbell-lvgl.yaml`, en entités
Home Assistant *et* en boutons sur l'écran du P4.

### Les mesures

| Depuis une lambda | Ce que ça dit |
|---|---|
| `mic_level_db()` | niveau crête du micro, en dBFS, gain appliqué |
| `mic_level()` | le même, en 0.0 – 1.0 |
| `mic_alive()` | la source micro délivre-t-elle encore des échantillons ? |
| `mic_samples()` | échantillons lus depuis le démarrage |
| `speaker_level_db()` / `speaker_level()` | niveau de ce qui part au haut-parleur |
| `audio_packets_sent()` | paquets RTP montants (micro → réseau) |
| `audio_packets_received()` | paquets backchannel reçus (Home Assistant → P4) |
| `audio_running()` / `has_speaker()` | l'état du pipeline |

Comment lire `mic_level_db()` :

| Lecture | Diagnostic |
|---|---|
| `mic_alive()` faux | la source ne délivre **rien** : mauvais `microphone_id`, codec non démarré, broches I2S fausses. Aucun réglage de gain n'y fera rien. |
| −100 dBFS en parlant | la source délivre du **silence numérique** : mauvais slot I2S (`channel:`), capsule morte, gain à zéro. |
| −60 à −40 dBFS | ça capte, mais faiblement — montez le gain (`gain:`, ou `mic_gain_db` côté fdaudio). |
| −30 à −6 dBFS | niveau de parole correct. |
| au-dessus de −3 dBFS | ça écrête : baissez le gain. |
| `audio_packets_received()` figé à 0 pendant que vous parlez depuis Home Assistant | la voix descendante n'atteint pas le P4 : le défaut est en amont (backchannel non négocié, go2rtc), pas dans le composant. |

Les mêmes chiffres apparaissent dans le résumé périodique des logs, avec un
vumètre en texte :

```
[rtsp_server]: --- status ---------------------------------------
[rtsp_server]:   clients=1 playing=1 | negotiated: video=yes audio=yes backchannel=yes
[rtsp_server]:   video: 812 encoded, 0 skipped | tx: 0 packets, 0 frames dropped
[rtsp_server]:   audio: mic 1620 packets sent | backchannel 0 received, 0 dropped
[rtsp_server]:   mic:   259200 samples read (flowing), peak -21.4 dBFS [######----]
[rtsp_server]:   spk:   peak -100.0 dBFS [----------]
[rtsp_server]: --------------------------------------------------
```

## « J'appuie sur Sonner et rien n'arrive dans Home Assistant »

La sonnerie n'a rien à voir avec le flux RTSP : c'est de l'ESPHome ordinaire, et
c'est justement là que ça coince. Un `binary_sensor` seul ne suffit pas —

- **aucune carte ne réagit à un `binary_sensor`.** La carte caméra affiche une
  caméra, un point. Faire apparaître la sonnerie demande une automatisation ;
- **sans `device_class`, Home Assistant ne devine pas** qu'il s'agit d'une
  sonnette ;
- **une impulsion courte se rate.** Si l'API était déconnectée à cet instant,
  l'information est perdue.

Les configurations d'exemple exposent donc trois chemins, du plus utile au plus
brut. Le premier suffit presque toujours :

| Chemin | Entité / évènement | À utiliser pour |
|---|---|---|
| **`event`** | `event.<appareil>_sonnette`, `device_class: doorbell` | c'est **l'entité sonnette native de Home Assistant**. Son état est l'horodatage du dernier appui, elle se choisit directement comme déclencheur dans l'éditeur d'automatisations, et rien n'est perdu. |
| `binary_sensor` | `binary_sensor.<appareil>_bouton`, `device_class: occupancy` | les conditions et les cartes qui veulent un on/off. Tenu 5 s pour être visible à l'œil dans Outils de développement → États. |
| évènement de bus | `esphome.doorbell_pressed` | les automatisations écrites à la main en YAML. |

L'automatisation qui affiche la caméra sur les téléphones est dans
[`docs/home-assistant.md`](docs/home-assistant.md) §6.

Si rien ne part du tout, le premier point à trancher est *l'appui lui-même* : le
script de sonnerie journalise `ring: sequence declenchee`. Cette ligne absente
de `esphome logs` quand vous touchez le bouton signifie que l'appui tactile
n'atteint pas le widget (calibration ou rotation du GT911), et non que la
sonnerie échoue.

## Résolution de l'image

Le contrôleur MIPI-CSI de l'ESP32-P4 **ne redimensionne pas** : la résolution
vient du format sélectionné dans le capteur au moment de l'initialisation. En
source V4L2 directe, le composant lit ce format avec `VIDIOC_G_FMT` et
configure l'encodeur en conséquence — il n'y a rien à déclarer côté
`rtsp_server`. Avec `camera_id`, c'est la clé `resolution:` du composant
`esp_cam_sensor` qui décide.

Formats OV5647 disponibles dans `esphome_esp-video`, sélectionnés par
`CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT` :

| Index | Résolution | Format | |
|---|---|---|---|
| 0 | 800×1280 @ 50 fps | RAW8 | portrait — plutôt adapté à une sonnette |
| 1 | 800×640 @ 50 fps | RAW8 | celui de `doorbell-lvgl.yaml` |
| 2 | 800×800 @ 50 fps | RAW8 | |
| 3 | 1920×1080 @ 30 fps | RAW10 | lourd pour le débit MJPEG |
| 4 | 1280×960 @ 45 fps | RAW10 | recommandé en paysage |

RFC 2435 impose des multiples de 8 et au plus 2040 px par côté. Le composant
vérifie ces conditions à l'ouverture et refuse de démarrer avec un message
explicite plutôt que de produire un flux illisible.

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
- **Pas de H.264.** Voir plus haut : le P4 ne le décode pas, et son encodeur ne
  peut pas partager la caméra avec l'aperçu LVGL.
- **Un seul consommateur de la caméra.** Une seule tâche a le droit de dépiler
  les tampons V4L2. Avec `drive_camera: false`, c'est `lvgl_camera_display` ;
  si vous éteignez l'aperçu, rendez la main au serveur avec
  `id(...).set_drive_camera(true)`, sinon le flux se fige sans erreur (c'est ce
  que fait l'interrupteur d'aperçu de `doorbell-lvgl.yaml`).

## Détails d'implémentation

- **Zéro copie.** Le tampon de la caméra (PSRAM compatible DMA) est donné tel
  quel à `jpeg_encoder_process` ; seul le flux compressé est relu.
- **Un seul thread réseau.** Un unique tâche FreeRTOS possède la socket
  d'écoute, toutes les sessions et l'envoi : aucun verrou sur le chemin critique.
  Les tâches vidéo et audio déposent leurs paquets RTP dans un tampon circulaire
  et ne bloquent jamais sur le réseau — en cas de saturation, les paquets sont
  abandonnés plutôt que de figer l'encodage.
- **MJPEG (RFC 2435).** Les en-têtes JFIF ne sont pas transmis : le récepteur
  les reconstruit depuis l'en-tête RTP de 8 octets, et les tables de
  quantification voyagent en intrabande dans le premier paquet de chaque trame
  (`Q = 255`). Le sous-échantillonnage est déduit du marqueur SOF.
- **Backchannel.** La piste `sendonly` n'est annoncée que si le client envoie
  `Require: www.onvif.org/ver20/backchannel` — un client ordinaire (VLC, ffmpeg)
  voit un flux à deux pistes parfaitement normal.

## Licence

Voir [LICENSE](LICENSE).
