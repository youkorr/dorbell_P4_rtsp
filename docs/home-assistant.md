# Intégration Home Assistant

## 0. Comment Home Assistant trouve-t-il le flux ?

Il ne le trouve pas : **il n'y a aucune découverte automatique**, et Home
Assistant ne parle jamais au P4 en RTSP. La chaîne est en deux temps :

```
   P4  ──RTSP──►  go2rtc  ──WebRTC──►  Home Assistant
        ▲
        └── adresse écrite À LA MAIN dans go2rtc.yaml
```

1. Le P4 ouvre son serveur RTSP sur `rtsp://<ip-du-P4>:8554/doorbell`.
2. **Vous** inscrivez cette adresse dans `go2rtc.yaml`.
3. Home Assistant ne connaît que go2rtc. Le P4 n'apparaît nulle part côté flux.

Le P4 est donc découvert par Home Assistant **en tant qu'appareil ESPHome**
(via l'API native, mDNS) — bouton, carillon, capteurs — mais son flux vidéo,
lui, transite uniquement par go2rtc.

### Conséquence : le P4 doit avoir une adresse stable

Sinon go2rtc le perd au renouvellement du bail DHCP. Deux options :

```yaml
# Option A — par nom mDNS, publié automatiquement par ESPHome
streams:
  doorbell:
    - rtsp://user:pass@doorbell-p4-lvgl.local:8554/doorbell
```

Le nom vient de la clé `esphome: name:`. Cela suppose que la machine qui fait
tourner go2rtc résout le mDNS : c'est le cas sur Home Assistant OS, souvent pas
dans un conteneur Docker isolé.

```yaml
# Option B — par IP, avec une réservation DHCP sur votre box (recommandé)
streams:
  doorbell:
    - rtsp://user:pass@192.168.1.50:8554/doorbell
```

### Où lire l'adresse du P4

- Dans les logs ESPHome au démarrage :
  `[rtsp_server]: listening on rtsp://192.168.1.50:8554/doorbell`
- Dans Home Assistant, via le capteur **« Flux RTSP »** exposé par les deux
  exemples de configuration : il affiche l'URL complète, prête à coller dans
  `go2rtc.yaml`.
- `esphome logs doorbell-lvgl.yaml` depuis la ligne de commande.

Les `user:pass` de l'URL sont les `username:` / `password:` du bloc
`rtsp_server:`. S'ils sont absents de votre configuration, le flux est ouvert et
l'URL se réduit à `rtsp://192.168.1.50:8554/doorbell`.

## 0 bis. Quelle instance de go2rtc ? (à trancher en premier)

Il peut y en avoir **trois** sur une même machine, toutes sur le port 1984, et
c'est la source de confusion n°1 :

| Instance | Sa configuration | Éditable à la main ? |
|---|---|---|
| Add-on go2rtc | le fichier indiqué par ses logs | oui |
| go2rtc intégré à Home Assistant (≥ 2024.11) | générée automatiquement | **non**, réécrite à chaque démarrage |
| go2rtc intégré à Frigate | la section `go2rtc:` de `frigate.yaml` | oui |

**N'en gardez qu'une.** Si deux tournent, celle que vous voyez sur `:1984`
n'est pas forcément celle dont vous éditez le fichier : vos modifications
semblent alors « disparaître » alors qu'elles n'ont jamais été lues.

La seule source fiable est la première ligne des logs de go2rtc :

```
info  config path=/config/go2rtc_homekit.yml
```

C'est ce fichier-là qu'il faut éditer, quel que soit le nom attendu.

## 0 ter. Passer par Frigate (recommandé si vous l'avez déjà)

Frigate embarque go2rtc et lit sa configuration depuis `frigate.yaml`, qui n'est
jamais réécrit. C'est aussi la seule voie où **l'audio bidirectionnel de
l'Advanced Camera Card est officiellement supporté** : la carte réserve cette
fonction aux caméras de type Frigate.

Désactivez d'abord le démarrage automatique de l'add-on go2rtc, sinon les deux
se disputent les ports.

```yaml
# frigate.yaml
go2rtc:
  streams:
    doorbell:
      - rtsp://USERNAME:PASSWORD@192.168.1.50:8554/doorbell#backchannel=0
    doorbell_webrtc:
      - ffmpeg:doorbell#video=h264
      - rtsp://USERNAME:PASSWORD@192.168.1.50:8554/doorbell#backchannel=1

cameras:
  doorbell:
    ffmpeg:
      inputs:
        # On consomme le restream de go2rtc, pas le P4 directement : le P4 n'a
        # ainsi qu'un seul client à servir, quel que soit le nombre de vues.
        - path: rtsp://127.0.0.1:8554/doorbell_webrtc
          input_args: preset-rtsp-restream
          roles: [detect, record]
    detect:
      width: 800
      height: 640
      fps: 5
    live:
      # Frigate 0.14. En 0.15+ c'est un dictionnaire :
      #   streams: {Sonnette: doorbell_webrtc}
      stream_name: doorbell_webrtc
```

La carte pointe alors sur la caméra Frigate, sans bloc `go2rtc:` :

```yaml
type: custom:frigate-card
cameras:
  - camera_entity: camera.doorbell
    live_provider: go2rtc
menu:
  buttons:
    microphone:
      enabled: true
      type: momentary
```

Les règles de la section 1 restent valables : le transcodage H.264 est
nécessaire tant que le P4 est en `codec: mjpeg`, et la ligne `#backchannel=1`
séparée reste ce qui porte le push-to-talk.

### « Conversation bidirectionnelle non disponible pour ce flux »

Ce message de l'interface Frigate ne dit rien du flux : **Frigate désactive la
conversation bidirectionnelle hors contexte sécurisé**, quelle que soit la
qualité de la négociation ONVIF derrière. Ses deux ports n'ont pas les mêmes
droits :

| Port | Usage |
|---|---|
| `5000` | HTTP direct, sans authentification — **pas de conversation bidirectionnelle** |
| `8971` | HTTPS authentifié — celui qu'il faut |

Ouvrez `https://<frigate>:8971` et acceptez l'exception de certificat
(auto-signé). Publiez le port dans la configuration réseau de l'add-on s'il ne
l'est pas, en même temps que `8555/tcp` et `8555/udp`, sans lesquels le
navigateur ne peut établir la connexion WebRTC.

La même règle s'applique à la carte Lovelace : sur un Home Assistant en HTTP,
le bouton micro reste inerte. C'est le navigateur qui refuse `getUserMedia()`,
pas la carte.

## 1. MJPEG : quel flux go2rtc utiliser ?

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

> **Le mode d'affichage MJPEG ne transporte aucun son.** MJPEG est un format
> vidéo seul : une carte réglée sur `modes: [mjpeg]` donnera l'image sans jamais
> le micro, quel que soit l'état du backchannel côté RTSP. Pour entendre la
> sonnette il faut `modes: [webrtc]`, donc de la vidéo H.264 — d'où le
> transcodage ci-dessus. Et dans le lecteur, pensez à couper le mute : les
> navigateurs démarrent muets tant qu'on n'a pas cliqué.

### Vérifier que go2rtc reçoit bien le flux

| URL | Ce que ça teste |
|---|---|
| `http://<go2rtc>:1984/api/frame.jpeg?src=doorbell` | une **image fixe**, un instantané unique — normal qu'elle ne bouge pas dans la page ; rechargez pour en obtenir une nouvelle |
| `http://<go2rtc>:1984/api/stream.mjpeg?src=doorbell` | le flux **en direct** : c'est le vrai test que le P4 débite des trames |

Puis, dans l'interface go2rtc, le bouton **webrtc** sur `doorbell_webrtc` : tant
qu'il n'affiche pas d'image, aucune carte Lovelace ne fonctionnera. Ce test isole
go2rtc de Home Assistant et évite de chercher au mauvais endroit.

## 2. Prérequis

| Élément | Pourquoi |
|---|---|
| go2rtc (add-on ou binaire) | ingère le RTSP, republie en WebRTC |
| Accès à Home Assistant en **HTTPS** | le navigateur refuse `getUserMedia()` sur du HTTP non local — sans ça, **pas de micro**, quelle que soit la carte |
| Carte Lovelace : `custom:webrtc-camera` (AlexxIT) ou `custom:advanced-camera-card` | bouton push-to-talk |

`http://localhost` et `http://127.0.0.1` sont considérés comme des origines
sûres par les navigateurs ; **une IP de LAN en clair ne l'est pas**. C'est de
loin la cause n°1 des « le son descend mais je ne peux pas parler ».

## 3. Exposer le flux comme entité caméra

**La Generic Camera ne se configure plus en YAML.** Home Assistant l'a migrée
vers l'interface : un bloc `camera: - platform: generic` dans
`configuration.yaml` déclenche l'erreur « It's not possible to configure generic
camera by adding `platform: generic` » et aucune entité n'est créée.

**Paramètres → Appareils et services → + Ajouter une intégration →
« Caméra générique »** (le nom est traduit ; à défaut, ouvrez directement
`http://<ip-de-HA>:8123/config/integrations/dashboard/add?domain=generic`).

| Champ | Valeur |
|---|---|
| URL de l'image fixe | `http://192.168.1.10:1984/api/frame.jpeg?src=doorbell` |
| URL du flux | `rtsp://192.168.1.10:8554/doorbell_webrtc` |
| Nom d'utilisateur / mot de passe | vides (go2rtc ne les demande pas) |
| Vérifier le certificat SSL | décoché |
| Protocole de transport RTSP | TCP |

L'assistant affiche un aperçu avant de valider : s'il échoue là, le problème est
dans go2rtc, pas dans Home Assistant.

La vignette (`still_image_url`) peut venir du flux `doorbell` d'origine : en
MJPEG, go2rtc extrait une image sans rien décoder.

> **`192.168.1.10` est la machine qui fait tourner go2rtc, pas le P4.** C'est la
> confusion la plus fréquente : le P4 ne sert que du RTSP sur le port 8554, il
> n'a rien sur le 1984. Si vous ne connaissez pas l'adresse de go2rtc, lisez-la
> dans les logs du P4 : `client connected from 192.168.1.38` — ce client, c'est
> go2rtc.

Avec l'intégration [WebRTC Camera d'AlexxIT](https://github.com/AlexxIT/WebRTC)
installée via HACS, les flux déclarés dans `go2rtc.yaml` sont directement
utilisables par leur nom (`doorbell`), sans entité caméra.

## 4. Carte Lovelace — WebRTC Camera (AlexxIT)

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

## 5. Carte Lovelace — Advanced Camera Card

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

## 6. Automatisation : appui sur le bouton → notification

Le bouton est géré par ESPHome, donc côté Home Assistant il ne reste que la
notification. **Sans cette automatisation, rien n'est envoyé** : le capteur
change d'état et personne ne l'écoute. C'est la pièce qu'on oublie le plus
souvent, parce que tout le reste de la chaîne a l'air de fonctionner.

> À ne pas confondre avec le `notifications:` de `frigate.yaml` : celui-là
> notifie sur **détection d'objet** et demande un abonnement depuis l'interface
> de Frigate. Il ne connaît pas le bouton de la sonnette.

### Trouver les deux identifiants

Les deux lignes qui échouent silencieusement si elles sont fausses :

- **`entity_id` du capteur.** ESPHome le construit à partir du nom de
  l'appareil, pas du `friendly_name` que vous croyez : selon la configuration
  cela donne `binary_sensor.doorbell_lvgl_bouton` ou
  `binary_sensor.doorbell_p4_bouton`. Lisez-le dans **Outils de développement →
  États** en filtrant sur `bouton`, et copiez-le tel quel.
- **Le service de notification.** Il vaut `notify.mobile_app_<nom-du-mobile>`.
  La liste exacte est dans **Outils de développement → Actions**, en tapant
  `notify.`.

```yaml
automation:
  - alias: Sonnette - notification
    trigger:
      - platform: state
        entity_id: binary_sensor.doorbell_lvgl_bouton
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

## 7. Vérifier que l'audio bidirectionnel est bien négocié

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

## 8. Diagnostic

| Symptôme | Cause probable |
|---|---|
| go2rtc ne se connecte pas au P4 | l'IP a changé : réservation DHCP, ou passez au nom mDNS (étape 0) |
| WebRTC : écran noir, et les logs du P4 ne montrent qu'un `SETUP trackID=1` | go2rtc a jeté la vidéo JPEG, que WebRTC ne sait pas transporter : il faut le transcodage `ffmpeg:...#video=h264` (étape 1) |
| `probe` sur `doorbell_webrtc` affiche encore `JPEG` | le transcodage ffmpeg échoue — retirez `#hardware` si la machine n'a pas d'encodeur VA-API |
| Le port 1984 ne répond pas | vous visez le P4 au lieu de go2rtc : le P4 n'expose que le RTSP sur 8554 |
| Image OK puis clients refusés (`refusing ...: already serving 2 clients`) | la chaîne MJPEG prend 2 sessions RTSP : passez `max_clients: 3` dans le YAML ESPHome |
| Le push-to-talk marche une fois puis plus jamais | deux sources go2rtc demandent le backchannel : une seule doit l'avoir (voir `go2rtc/go2rtc.yaml`) |
| `Custom element not found: ...` | la carte Lovelace n'est pas installée — passez par HACS, puis Ctrl+Shift+R |
| « Échec de l'initialisation de la caméra » (Advanced Camera Card) | le `camera_entity` référencé n'existe pas : créez la Caméra générique par l'interface (étape 3) |
| Bouton micro absent ou inerte | Home Assistant en HTTP → passez en HTTPS |
| Image OK, aucun son montant | pas de `microphone` dans `media` (webrtc-camera) |
| Image OK, on ne peut pas parler | backchannel non négocié → étape 7 |
| Le son se coupe quand on parle | normal : `half_duplex: true` coupe le micro pendant l'émission |
| Larsen | l'ampli reste alimenté : câblez la broche `SD` (voir `docs/hardware.md`) |
| Image saccadée ou verte au démarrage (H.264) | le client attend la première trame clé — au plus `gop / framerate` secondes |
| Image fluide mais CPU élevé sur la machine HA | transcodage MJPEG → H.264 ; passez le P4 en `codec: h264` |
| Pas d'image dans la carte, mais `doorbell` visible dans go2rtc | la carte pointe sur `doorbell` alors que le P4 est en MJPEG : utilisez `doorbell_webrtc` |
| `RTSP: unsupported transport` | un client force l'UDP ; ce serveur est en TCP interleaved uniquement |
