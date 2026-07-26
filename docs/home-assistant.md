# Intégration Home Assistant

## 0. MJPEG : quel flux go2rtc utiliser ?

Le composant sort du **MJPEG** par défaut. WebRTC ne sait pas transporter du
MJPEG, donc pour la carte Lovelace (mode `webrtc`, indispensable au
push-to-talk) il faut pointer sur le flux **transcodé** déclaré dans
`go2rtc.yaml` :

| Codec sur le P4 | Flux go2rtc à utiliser dans la carte | Transcodage |
|---|---|---|
| `mjpeg` (défaut) | `doorbell_webrtc` | vidéo ré-encodée en H.264 par ffmpeg ; **audio copié tel quel** |
| `h264` (option) | `doorbell` | aucun |

Le backchannel n'est jamais transcodé : dans les deux cas le G.711 traverse la
chaîne intact, donc le push-to-talk se comporte pareil. Seul le coût CPU sur la
machine Home Assistant change.

Dans les exemples ci-dessous, remplacez `doorbell_webrtc` par `doorbell` si vous
êtes passé en `codec: h264`.

## 1. Prérequis

| Élément | Pourquoi |
|---|---|
| go2rtc (add-on ou binaire) | ingère le RTSP, republie en WebRTC |
| Accès à Home Assistant en **HTTPS** | le navigateur refuse `getUserMedia()` sur du HTTP non local — sans ça, **pas de micro**, quelle que soit la carte |
| Carte Lovelace : `custom:webrtc-camera` (AlexxIT) ou `custom:advanced-camera-card` | bouton push-to-talk |

`http://localhost` et `http://127.0.0.1` sont considérés comme des origines
sûres par les navigateurs ; **une IP de LAN en clair ne l'est pas**. C'est de
loin la cause n°1 des « le son descend mais je ne peux pas parler ».

## 2. Exposer le flux comme entité caméra

```yaml
# configuration.yaml
camera:
  - platform: generic
    name: Sonnette
    still_image_url: http://192.168.1.10:1984/api/frame.jpeg?src=doorbell
    stream_source: rtsp://192.168.1.10:8554/doorbell_webrtc
    verify_ssl: false
```

La vignette (`still_image_url`) peut venir du flux `doorbell` d'origine : en
MJPEG, go2rtc extrait une image sans rien décoder.

`192.168.1.10` est la machine qui fait tourner go2rtc, pas le P4 : on passe par
go2rtc pour que le P4 n'ait qu'un seul client RTSP à servir.

Avec l'intégration [WebRTC Camera d'AlexxIT](https://github.com/AlexxIT/WebRTC)
installée via HACS, les flux déclarés dans `go2rtc.yaml` sont directement
utilisables par leur nom (`doorbell`), sans entité caméra.

## 3. Carte Lovelace — WebRTC Camera (AlexxIT)

C'est la mise en œuvre la plus directe du push-to-talk :

```yaml
type: custom:webrtc-camera
url: doorbell_webrtc     # 'doorbell' si le P4 est en codec: h264
mode: webrtc             # seul mode qui gère l'audio bidirectionnel
media: video,audio,microphone
muted: false
ui: true
```

`microphone` dans `media` est ce qui fait apparaître le bouton micro et
déclenche la négociation du backchannel. Sans lui, le flux reste descendant.

## 4. Carte Lovelace — Advanced Camera Card

La carte que vous visez ([card.camera](https://card.camera/#/examples?id=doorbell)),
avec le bouton en mode **momentané**, c'est-à-dire un vrai push-to-talk :

```yaml
type: custom:advanced-camera-card
cameras:
  - camera_entity: camera.sonnette
    live_provider: go2rtc
    go2rtc:
      url: http://192.168.1.10:1984
      stream: doorbell_webrtc   # 'doorbell' si le P4 est en codec: h264
      modes:
        - webrtc          # seul mode compatible audio bidirectionnel
live:
  microphone:
    always_connected: false   # true = pas de coupure du flux au 1er appui
  auto_unmute:
    - microphone
menu:
  buttons:
    microphone:
      enabled: true
      type: momentary       # maintenir pour parler ; 'toggle' pour un verrou
```

`type: momentary` est le comportement PTT ; `toggle` transforme le bouton en
interrupteur marche/arrêt.

> La documentation de la carte indique que l'audio bidirectionnel n'est
> officiellement supporté que pour les caméras de type Frigate. Avec une caméra
> générique il faut renseigner explicitement `go2rtc.url` et `go2rtc.stream`
> comme ci-dessus. Si le bouton micro reste inactif, repliez-vous sur
> `custom:webrtc-camera`, qui n'a pas cette restriction.

## 5. Automatisation : appui sur le bouton → notification

Le bouton et le relais du carillon sont gérés par ESPHome (`doorbell.yaml`),
donc côté Home Assistant il ne reste que la notification :

```yaml
automation:
  - alias: Sonnette - notification
    trigger:
      - platform: state
        entity_id: binary_sensor.doorbell_p4_bouton
        to: "on"
    action:
      - action: notify.mobile_app_telephone
        data:
          title: "Sonnette"
          message: "Quelqu'un est à la porte"
          data:
            # Vignette prise via go2rtc au moment de l'appui
            image: /api/camera_proxy/camera.sonnette
            actions:
              - action: URI
                title: "Voir et parler"
                # Ouvre le tableau de bord contenant la carte caméra
                uri: /lovelace/sonnette
            push:
              interruption-level: time-sensitive
            channel: doorbell
            importance: high
```

L'action `URI` ouvre le tableau de bord directement dans l'application, avec la
carte caméra et son bouton PTT.

Tableau de bord minimal associé :

```yaml
views:
  - title: Sonnette
    path: sonnette
    cards:
      - type: custom:advanced-camera-card
        cameras:
          - camera_entity: camera.sonnette
            live_provider: go2rtc
            go2rtc:
              url: http://192.168.1.10:1984
              stream: doorbell_webrtc
              modes: [webrtc]
        menu:
          buttons:
            microphone:
              enabled: true
              type: momentary
```

## 6. Vérifier que l'audio bidirectionnel est bien négocié

1. Ouvrez l'interface de go2rtc : `http://192.168.1.10:1984`.
2. Sur le flux `doorbell`, cliquez **probe**. Vous devez voir **trois** pistes :

   ```
   video, recvonly, JPEG          (H264 si le P4 est en codec: h264)
   audio, recvonly, PCMU/8000
   audio, sendonly, PCMU/8000     <-- le backchannel
   ```

   Sondez le flux **`doorbell`** (la source RTSP), pas `doorbell_webrtc`.

   Si la troisième ligne manque, go2rtc n'a pas demandé le backchannel : c'est
   presque toujours un `#` de trop dans l'URL (voir `go2rtc/go2rtc.yaml`).
3. Passez `log: {rtsp: trace}` dans go2rtc et relancez : la requête `DESCRIBE`
   doit porter l'en-tête `Require: www.onvif.org/ver20/backchannel`, et le SDP
   renvoyé par le P4 doit contenir `a=sendonly`.

## 7. Diagnostic

| Symptôme | Cause probable |
|---|---|
| Bouton micro absent ou inerte | Home Assistant en HTTP → passez en HTTPS |
| Image OK, aucun son montant | pas de `microphone` dans `media` (webrtc-camera) |
| Image OK, on ne peut pas parler | backchannel non négocié → étape 6 |
| Le son se coupe quand on parle | normal : `half_duplex: true` coupe le micro pendant l'émission |
| Larsen | l'ampli reste alimenté : câblez la broche `SD` (voir `docs/hardware.md`) |
| Image saccadée ou verte au démarrage (H.264) | le client attend la première trame clé — au plus `gop / framerate` secondes |
| Image fluide mais CPU élevé sur la machine HA | transcodage MJPEG → H.264 ; passez le P4 en `codec: h264` |
| Pas d'image dans la carte, mais `doorbell` visible dans go2rtc | la carte pointe sur `doorbell` alors que le P4 est en MJPEG : utilisez `doorbell_webrtc` |
| `RTSP: unsupported transport` | un client force l'UDP ; ce serveur est en TCP interleaved uniquement |
