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
| MJPEG (le seul) | `doorbell_webrtc` | vidéo ré-encodée en H.264 par ffmpeg ; **audio copié tel quel** |

Le backchannel n'est jamais transcodé : dans les deux cas le G.711 traverse la
chaîne intact, donc le push-to-talk se comporte pareil. Seul le coût CPU sur la
machine Home Assistant change.

> **Le mode d'affichage MJPEG ne transporte aucun son.** MJPEG est un format
> vidéo seul : une carte réglée sur `modes: [mjpeg]` donnera l'image sans jamais
> le micro, quel que soit l'état du backchannel côté RTSP. Pour entendre la
> sonnette il faut `modes: [webrtc]`, donc de la vidéo H.264 — d'où le
> transcodage ci-dessus, que le P4 ne peut pas éviter puisqu'il n'émet que du
> MJPEG (voir le §4 ter). Et dans le lecteur, pensez à couper le mute : les
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
url: doorbell_webrtc
mode: webrtc             # seul mode qui gère l'audio bidirectionnel
media: video,audio,microphone
muted: false
ui: true
```

`microphone` dans `media` est ce qui fait apparaître le bouton micro et
déclenche la négociation du backchannel. Sans lui, le flux reste descendant.

## 4 ter. Pourquoi MJPEG et push-to-talk sont incompatibles en pratique

C'est le point le plus important de cette page, et il n'apparaît dans aucune
documentation : **la carte n'affiche le bouton micro que s'il y a UN SEUL
consommateur sur le flux** ([advanced-camera-card
#1888](https://github.com/dermotduffy/advanced-camera-card/issues/1888)).

Or la chaîne MJPEG en impose deux, par construction :

```
doorbell_webrtc:
  - ffmpeg:doorbell#video=h264   <- transcode la vidéo, ne remonte PAS le backchannel
  - rtsp://...#backchannel=1     <- source dédiée pour la voix montante
```

ffmpeg ne tire que dans un sens : il ne peut pas porter le backchannel. Il faut
donc une seconde source, et ces deux sources font deux consommateurs. Le bouton
micro ne s'affiche pas. Ce n'est pas un réglage à trouver, c'est une impasse.

Le bloc d'état du P4 la rend visible d'un coup d'œil :

```
clients=2 playing=2 | negotiated: video=NO audio=yes backchannel=NO
```

### La sortie qui n'existe plus : H.264

Les versions précédentes de cette page proposaient ici `codec: h264` : la vidéo
arrivait déjà dans le format attendu par WebRTC, plus de transcodage, donc une
seule source et le bouton micro apparaissait.

**Cette option a été retirée du composant**, et il faut être franc sur ce que
cela coûte : *si votre objectif est le push-to-talk depuis l'Advanced Camera
Card, c'était le seul chemin, et il n'y en a plus.* La raison du retrait :

- le P4 encode le H.264 mais ne le décode pas — l'aperçu local sur son écran
  était donc impossible dans ce mode ;
- son encodeur consomme du YUV420 quand LVGL affiche du RGB565 : une seule
  caméra ne peut pas alimenter les deux, il fallait choisir entre l'écran et le
  flux.

L'aperçu sur l'écran du P4 a été jugé plus important que le bouton micro d'une
carte particulière. `codec: h264` est maintenant refusé à la validation, avec ce
message, plutôt que de servir du MJPEG en silence.

### Ce qui marche quand même pour parler

Le backchannel du composant fonctionne : ce qui coince est la carte, pas
l'appareil. Trois façons de s'en servir, par ordre de simplicité :

1. **La page de go2rtc elle-même.** C'est le chemin le plus direct, et celui à
   essayer en premier pour vérifier que la voix descendante passe de bout en
   bout :

   ```
   http://ADRESSE_HA:1984/stream.html?src=doorbell&mode=webrtc&media=video+audio+microphone
   ```

   Un seul consommateur, le bouton micro y est, et il n'y a ni carte ni HTTPS à
   satisfaire. Elle s'intègre dans un tableau de bord avec une carte `iframe`.

2. **La carte WebRTC Camera (AlexxIT)** en `mode: webrtc` avec
   `media: video,audio,microphone` — voir le §4 plus haut.

3. **Les tests embarqués**, pour séparer une panne d'appareil d'une panne de
   chaîne : le bouton « Bip de test » prouve le haut-parleur, l'interrupteur
   « Test audio (boucle micro vers haut-parleur) » prouve le micro *et* le
   haut-parleur d'un coup. Détails dans le README.

Et pour savoir, sans quitter Home Assistant, si la voix descendante atteint
vraiment le P4 : le capteur **« Paquets audio recus »**. S'il reste à 0 pendant
que vous appuyez sur parler, rien n'arrive à l'appareil et c'est en amont qu'il
faut chercher. S'il monte et que vous n'entendez rien, le problème est l'ampli
ou le volume.

## 4 bis. Les sept conditions du push-to-talk (Advanced Camera Card)

La documentation de la carte les pose comme **toutes obligatoires**. Il n'y a
pas de dégradé : si une seule manque, le bouton micro n'est pas grisé, il
**n'apparaît pas du tout** — ce qui donne l'impression d'un bug alors que c'est
la configuration.

| # | Condition |
|---|---|
| 1 | La caméra a une sortie audio |
| 2 | go2rtc sait faire l'audio bidirectionnel avec elle |
| 3 | Home Assistant est accessible en **HTTPS** — voir l'encadré |
| 4 | **Caméra de type Frigate uniquement** — pas une Generic Camera |
| 5 | **`live_provider: go2rtc` uniquement** — jamais `ha` |
| 6 | **`modes: [webrtc]` uniquement** |
| 7 | Le bouton micro est activé dans `menu.buttons` |

> **La 3 se règle sans certificat à gérer.** Un accès distant Tailscale
> (`https://…​.ts.net`) ou Nabu Casa (`https://…​.ui.nabu.casa`) est du HTTPS
> valide : le contexte est sécurisé, et le micro devient accessible. Le piège est
> alors ailleurs — il faut **ouvrir Home Assistant par cette adresse-là**. La même
> interface atteinte par `http://192.168.1.x:8123`, sur le même réseau et le même
> écran, n'aura pas le micro. Ce n'est pas l'installation qui décide, c'est
> l'URL de la barre d'adresse.

La 6 a une conséquence qu'on découvre tard : **le mode `mjpeg` exclut le
push-to-talk par construction**. Se rabattre sur MJPEG parce que le WebRTC
saccade revient à renoncer à la parole. Corriger le WebRTC n'est donc pas un
confort, c'est un prérequis.

La carte minimale à faire fonctionner AVANT d'ajouter déclencheurs et éléments
personnalisés :

```yaml
type: custom:advanced-camera-card
cameras:
  - camera_entity: camera.doorbell
    live_provider: go2rtc
    go2rtc:
      modes:
        - webrtc
menu:
  style: outside
  buttons:
    microphone:
      enabled: true
      type: momentary
```

Si le bouton micro n'apparaît pas avec ça, c'est la condition 4 : vérifiez que
l'entité vient bien de l'intégration Frigate, et rechargez-la après tout ajout
de caméra dans `frigate.yaml`.

## 5. Carte Lovelace — Advanced Camera Card

### `frigate-card` ou `advanced-camera-card` ?

Les deux marchent, et ce n'est pas un hasard. Dans la source de la carte :

```ts
// Keep the old name around for backwards compatibility.
@customElement('frigate-card')
class FrigateCard extends AdvancedCameraCard {}
```

`custom:frigate-card` est donc un **alias pur** — la même classe, le même schéma
de configuration, aucune différence de comportement. Une configuration écrite
pour l'un fonctionne mot pour mot avec l'autre. Inutile de réécrire quoi que ce
soit ; `advanced-camera-card` est simplement le nom actuel.

Ce qui compte en revanche, c'est la **version installée**, car le schéma, lui, a
bougé. Elle se lit dans HACS, ou dans les outils de développement du navigateur.
Repères utiles :

| Version | À savoir |
|---|---|
| ≥ 7.27.0 | exige Home Assistant ≥ 2026.2 |
| 7.27.4 | dernière stable au moment où ceci est écrit |
| 8.0.0-rc | **restructure les automatisations** : `triggers:` obligatoires, `conditions:` séparées, `actions_not` supprimé, état du micro scindé. Ne pas y aller avant d'avoir relu ses notes de version |

Les clés utilisées ci-dessous sont celles des versions 7.x. Si la carte se
plaint d'une clé, son message la nomme : c'est presque toujours une version
antérieure.

### La carte, avec les valeurs de ce dépôt

Comme la sonnette est déclarée dans `frigate.yaml`, Home Assistant en fait une
**caméra de type Frigate** (`camera.doorbell`), et la carte découvre go2rtc
toute seule : pas besoin de renseigner `go2rtc.url`. C'est aussi ce qui satisfait
la condition n°4 du §4 bis.

```yaml
type: custom:advanced-camera-card
cameras:
  - camera_entity: camera.doorbell
    live_provider: go2rtc
    go2rtc:
      # Le flux transcodé : le P4 émet du MJPEG, que WebRTC ne transporte pas.
      # Pointer sur `doorbell` donnerait une vue vide en mode webrtc.
      stream: doorbell_webrtc
      modes:
        - webrtc          # seul mode qui porte l'audio
    # ---- LE point qui relie la sonnerie à la caméra --------------------------
    # Sans ça, appuyer sur « Sonner » ne fait rien apparaître : une carte caméra
    # affiche une caméra, elle n'écoute rien d'autre. Ces entités-là, elle les
    # écoute, et la vue bascule sur le direct quand la sonnette se déclenche.
    triggers:
      entities:
        - binary_sensor.doorbell_p4_lvgl_bouton

live:
  # La vue est prête avant l'appui : sans ça on regarde tourner un spinner
  # pendant que le visiteur attend.
  preload: true
  # Valeurs admises : selected, visible, microphone. La troisième compte —
  # elle démute automatiquement au moment où le micro se connecte, sans quoi on
  # parle dans le vide en croyant que le push-to-talk est cassé.
  auto_unmute:
    - selected
    - visible
    - microphone
  microphone:
    # false = le micro ne se connecte qu'au moment où on appuie. `true` évite la
    # coupure du flux au premier appui, au prix d'un micro ouvert en permanence.
    always_connected: false
    disconnect_seconds: 90

view:
  default: live
  triggers:
    show_trigger_status: true
    filter_selected_camera: true
    actions:
      # Valeurs admises pour `trigger` : default, live, media, none, update.
      # Pour `untrigger` : default, none. Rien d'autre ne validera.
      trigger: live          # la sonnerie amène la vue en direct
      untrigger: default     # et on revient à la vue normale ensuite
    # Combien de temps la vue reste sur le direct APRÈS que la sonnerie retombe.
    # C'est bien celui-ci qu'on veut régler, et non `interaction_seconds`, qui
    # dit tout autre chose : combien de temps une interaction de l'utilisateur
    # suspend les déclencheurs (300 s par défaut).
    untrigger_seconds: 30

menu:
  buttons:
    microphone:
      enabled: true
      type: momentary        # maintenir pour parler ; 'toggle' pour un verrou
```

Les valeurs d'énumération ci-dessus ne sont pas des suppositions, elles sont
lues dans le schéma de la carte (`src/config/schema/`) :

| Clé | Valeurs admises |
|---|---|
| `live.auto_unmute` | `selected`, `visible`, `microphone` |
| `live.auto_mute` | `unselected`, `hidden`, `microphone` |
| `view.triggers.actions.trigger` | `default`, `live`, `media`, `none`, `update` |
| `view.triggers.actions.untrigger` | `default`, `none` |
| `menu.buttons.microphone.type` | `momentary`, `toggle` |

`menu.buttons.microphone.enabled` vaut **`false` par défaut** : le mettre à
`true` n'est pas une redondance, c'est ce qui fait exister le bouton.

`binary_sensor.doorbell_p4_lvgl_bouton` suppose `name: doorbell-p4-lvgl` dans
votre YAML ESPHome. Vérifiez l'identifiant exact dans **Outils de développement →
États** en filtrant sur `bouton` — c'est le nom de l'appareil qui le construit,
pas le `friendly_name`. C'est ici que le capteur tenu 5 s prend tout son sens :
une impulsion de 100 ms ne laisserait pas à la carte le temps de réagir.

### Ce que cette carte donnera, et ce qu'elle ne donnera pas

| | |
|---|---|
| Image en direct | oui, en mode `webrtc` via `doorbell_webrtc` |
| Son descendant (entendre le visiteur) | oui — pensez à couper le mute, les navigateurs démarrent muets |
| La carte réagit à l'appui | oui, via `triggers` ci-dessus |
| Bouton micro (parler) | **probablement pas** — voir le §4 ter : la chaîne MJPEG impose deux sources à `doorbell_webrtc`, et la carte n'affiche le bouton que s'il y en a une seule |

Pour parler malgré tout, le plus simple reste la page de go2rtc elle-même, qui
n'a pas la contrainte du consommateur unique.

> **Elle a en revanche celle du HTTPS, et elle est incontournable.** Un
> navigateur ne donne accès au micro (`getUserMedia`) que dans un *contexte
> sécurisé* : HTTPS, ou `localhost`. Une adresse de LAN en clair comme
> `http://192.168.1.38:1984` n'en est pas un — le bouton micro sera là, et il
> échouera. Ce n'est pas propre à go2rtc ni à la carte : **aucun** push-to-talk
> dans un navigateur ne fonctionne sans HTTPS. C'est la condition n°3 du §4 bis,
> et elle s'applique partout.
>
> Trois façons de la satisfaire, de la plus propre à la plus rapide :
> mettre go2rtc et Home Assistant derrière HTTPS (Nabu Casa, ou un reverse proxy
> avec un certificat) ; ouvrir la page depuis la machine qui fait tourner go2rtc,
> où `http://localhost:1984` *est* un contexte sécurisé ; ou déclarer l'origine
> comme sûre dans le navigateur, pour un essai —
> `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, y ajouter
> `http://192.168.1.38:1984`, puis redémarrer Chrome.

L'adresse :

```
http://192.168.1.38:1984/stream.html?src=doorbell_webrtc&mode=webrtc&media=video+audio+microphone
```

Elle s'intègre au tableau de bord avec une carte `iframe` ordinaire :

```yaml
type: iframe
url: http://192.168.1.38:1984/stream.html?src=doorbell_webrtc&mode=webrtc&media=video+audio+microphone
aspect_ratio: 75%
```

### Une carte complète, sonnerie et diagnostic compris

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
      auto_unmute: [selected, visible]
    view:
      default: live
      triggers:
        actions:
          trigger: live
          untrigger: default

  # Le dernier appui, horodaté. C'est l'entité sonnette native de Home
  # Assistant : elle ne peut pas rater un appui, contrairement au capteur.
  - type: entities
    entities:
      - entity: event.doorbell_p4_lvgl_sonnette
        name: Dernier appui
      - entity: button.doorbell_p4_lvgl_sonner
        name: Sonner (test)

  # Le repli pour parler, et de quoi voir si l'audio va bien.
  - type: entities
    title: Audio
    entities:
      - entity: sensor.doorbell_p4_lvgl_niveau_micro
        name: Niveau micro
      - entity: sensor.doorbell_p4_lvgl_paquets_audio_recus
        name: Voix descendante reçue
      - entity: switch.doorbell_p4_lvgl_test_audio_boucle_micro_vers_haut_parleur
        name: Test audio (boucle)
      - entity: button.doorbell_p4_lvgl_bip_de_test
        name: Bip de test
```

Là encore, relevez les `entity_id` réels dans Outils de développement → États
plutôt que de recopier les miens : ESPHome les construit à partir du nom de
l'appareil.

## 5 bis. Le parcours complet d'un coup de sonnette

Le principe d'une sonnette, c'est quatre temps : **on appuie → ça sonne quelque
part → je vois qui c'est → je décroche et je réponds.** Voici où chacun se joue,
et ce qu'il faut avoir mis en place pour lui.

| Temps | Qui le fait | État |
|---|---|---|
| 1. On appuie | le P4 publie `event...sonnette`, `binary_sensor..._bouton` et l'évènement `esphome.doorbell_pressed` | rien à faire, c'est dans le YAML |
| 2. **Ça sonne** | une **automatisation** Home Assistant : carillon sur une enceinte, notification sur le téléphone | **à écrire — voir §6** |
| 3. Je vois qui c'est | la carte, avec `triggers:` (§5) et/ou la notification avec vignette | §5 |
| 4. Je réponds | l'audio bidirectionnel | descendant : oui. Montant : **exige HTTPS** (§4 ter) |

Le temps 2 est celui qu'on oublie, et c'est celui qui donne l'impression que
« rien ne marche ». **Home Assistant ne sonne pas tout seul.** Le P4 fait
scrupuleusement son travail — il annonce l'appui de trois façons — mais tant que
personne n'écoute cette annonce pour en faire du bruit, il ne se passe rien de
perceptible. Une sonnette sans automatisation, c'est un bouton qui change une
valeur dans une base de données.

Le temps 4 se heurte, lui, à une contrainte de navigateur et non de ce projet :
pas de micro sans HTTPS. Entendre le visiteur fonctionne sans rien (le flux
descend en permanence, cf. §7 bis) ; lui répondre demande d'avoir réglé le
HTTPS.

## 6. Automatisation : appui sur le bouton → notification

Le bouton est géré par ESPHome, donc côté Home Assistant il ne reste que la
notification. **Sans cette automatisation, rien n'est envoyé** : le capteur
change d'état et personne ne l'écoute. C'est la pièce qu'on oublie le plus
souvent, parce que tout le reste de la chaîne a l'air de fonctionner.

> À ne pas confondre avec le `notifications:` de `frigate.yaml` : celui-là
> notifie sur **détection d'objet** et demande un abonnement depuis l'interface
> de Frigate. Il ne connaît pas le bouton de la sonnette.

### Trois déclencheurs possibles — prenez le premier

La sonnette expose l'appui de trois façons (voir le README). Pour une
automatisation, le premier est de loin le meilleur :

| Déclencheur | Pourquoi |
|---|---|
| **entité `event`** | c'est l'entité sonnette **native** de Home Assistant. Elle porte un horodatage, se choisit dans l'éditeur graphique, et un appui ne peut pas être manqué. |
| entité `binary_sensor` | pour les conditions et les cartes. Tenue 5 s, donc visible à l'œil dans Outils de développement → États. |
| évènement `esphome.doorbell_pressed` | déclencheur brut, si vous préférez ne dépendre d'aucune entité. |

```yaml
# Recommandé : l'entité `event`. Tout changement d'état = un nouvel appui.
trigger:
  - platform: state
    entity_id: event.doorbell_p4_lvgl_sonnette
```

```yaml
# Variante : l'évènement de bus, si vous préférez ne dépendre d'aucun entity_id.
trigger:
  - platform: event
    event_type: esphome.doorbell_pressed
```

### Trouver les identifiants

Les lignes qui échouent silencieusement si elles sont fausses :

- **`entity_id` de l'entité.** ESPHome le construit à partir du nom de
  l'appareil, pas du `friendly_name` que vous croyez : selon la configuration
  cela donne `event.doorbell_p4_lvgl_sonnette` ou `event.doorbell_p4_sonnette`.
  Lisez-le dans **Outils de développement → États** en filtrant sur `sonnette`,
  et copiez-le tel quel. Idem pour `binary_sensor...._bouton`.
- **Le service de notification.** Il vaut `notify.mobile_app_<nom-du-mobile>`.
  La liste exacte est dans **Outils de développement → Actions**, en tapant
  `notify.`.

> **Vérifier que l'appui arrive, avant d'écrire quoi que ce soit.** Ouvrez
> **Outils de développement → États**, filtrez sur `sonnette`, et appuyez sur le
> bouton du P4 : l'horodatage de l'entité `event` doit changer. S'il ne bouge
> pas, l'automatisation n'est pas en cause — regardez `esphome logs`, la ligne
> `ring: sequence declenchee` vous dira si l'appui tactile a seulement atteint le
> script.

```yaml
automation:
  - alias: Sonnette - notification
    trigger:
      - platform: state
        entity_id: event.doorbell_p4_lvgl_sonnette
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

### Faire sonner la maison, pas seulement le téléphone

Une notification ne se voit que si on a le téléphone en main. Pour que ça
*sonne*, il faut une enceinte. C'est la même automatisation, avec une action de
plus :

```yaml
alias: Sonnette - carillon et notification
mode: single
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
actions:
  # 1. Le carillon, sur l'enceinte du salon. `media_player.play_media` avec un
  #    fichier depuis /config/www/ (donc servi sous /local/).
  - action: media_player.play_media
    target:
      entity_id: media_player.salon
    data:
      media_content_id: /local/sounds/doorbell.mp3
      media_content_type: music

  # Variante parlée, si vous preferez une annonce a un carillon :
  # - action: tts.speak
  #   target:
  #     entity_id: tts.piper
  #   data:
  #     cache: true
  #     media_player_entity_id: media_player.salon
  #     message: "Quelqu'un sonne a la porte"

  # 2. Le telephone, avec la vignette et le lien vers la vue.
  - action: notify.mobile_app_telephone
    data:
      title: Sonnette
      message: Quelqu'un est a la porte
      data:
        image: /api/camera_proxy/camera.doorbell
        actions:
          - action: URI
            title: Voir et parler
            uri: /lovelace/sonnette
        push:
          interruption-level: time-sensitive
        channel: doorbell
        importance: high
```

`mode: single` compte : sans lui, un visiteur qui appuie trois fois lance trois
carillons qui se chevauchent. Ajoutez un `- delay: 10s` en fin d'actions si vous
voulez en plus un temps mort avant qu'un nouvel appui puisse sonner.

Pour vérifier l'automatisation sans descendre à la porte : **Paramètres →
Automatisations → ⋮ → Exécuter**. Et pour vérifier toute la chaîne depuis le
début, le bouton `button...sonner` de l'appareil ESPHome déclenche exactement la
même séquence que l'appui tactile.

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

## 6 bis. « Ça ne m'ouvre pas la page de la sonnette »

Attente naturelle, et pourtant : **Home Assistant ne sait pas forcer un
navigateur à changer de page.** Il n'existe aucun service natif pour ça. Une
automatisation agit sur des appareils (enceintes, lumières, téléphones) ; elle
n'a pas la main sur l'onglet ouvert devant vous.

Ce que fait réellement le `uri:` d'une notification : il arme l'action de la
notification. La page s'ouvre **quand on tape dessus**, pas à la seconde où ça
sonne. C'est voulu — un téléphone qui change d'écran tout seul serait insupportable.

### Vérifier d'abord que la vue existe

Avant de chercher plus loin : `/lovelace/sonnette` n'existe que si un tableau de
bord contient une vue dont le **chemin d'URL** est exactement `sonnette`. Il se
règle dans **le tableau de bord → ✏️ → l'onglet de la vue → ⚙️ → « URL »**. Ce
n'est pas le titre de la vue : une vue titrée « Sonnette » peut très bien avoir
le chemin `view_2`. Tapez l'adresse à la main dans un navigateur : si vous
tombez sur une page vide ou sur le tableau de bord par défaut, le chemin est
faux, et aucune notification n'y mènera.

Sur un tableau de bord qui n'est pas celui par défaut, le chemin complet inclut
son nom : `/sonnette-dashboard/sonnette`. Le plus sûr est de naviguer jusqu'à la
vue et de recopier ce qui se trouve dans la barre d'adresse.

### Les trois façons d'arriver sur cette page

| | Comment | Automatique ? |
|---|---|---|
| Notification avec action `URI` | on tape sur la notification | non — un geste |
| La carte déjà à l'écran, avec `triggers:` (§5) | elle bascule sur le direct toute seule | oui, mais seulement si la page est déjà ouverte |
| **Browser Mod** | ouvre une fenêtre, ou navigue, sur un navigateur désigné | **oui, vraiment** |

### Browser Mod : la seule vraie réponse

[Browser Mod](https://github.com/thomasloven/hass-browser_mod) (via HACS) donne à
Home Assistant la main sur un navigateur précis. C'est ce qu'il faut pour une
tablette murale ou un PC de bureau qui doit afficher le visiteur sans qu'on
touche à rien.

#### L'installer vraiment — HACS ne fait que la moitié du travail

C'est le piège classique, et il ne dit pas son nom : **HACS télécharge des
fichiers, il n'ajoute pas l'intégration.** Après le redémarrage, Browser Mod
n'apparaît nulle part, et on croit que l'installation a échoué. Il manque une
étape :

1. HACS → Browser Mod → **Télécharger** ;
2. **redémarrer** Home Assistant ;
3. **Paramètres → Appareils et services → « + Ajouter une intégration » →
   taper « Browser Mod »**. ← *c'est cette étape qui manque* ;
4. rafraîchir le navigateur en vidant le cache (**Ctrl+Shift+R**), sans quoi le
   module frontend n'est pas chargé et rien ne s'enregistre.

Après quoi « Browser Mod » apparaît dans la barre latérale, et dans
Paramètres → Appareils et services.

Si Browser Mod ne sort pas dans la liste des intégrations à l'étape 3 : le
téléchargement HACS ne s'est pas terminé, ou le redémarrage n'en était pas un —
un simple « recharger la configuration » ne suffit pas, il faut un vrai
redémarrage (Paramètres → Système → ⏻ → **Redémarrer Home Assistant**).

#### Enregistrer les navigateurs

Un `browser_id` n'existe pas tant que le navigateur ne s'est pas déclaré. Sur
**chaque** appareil que vous voulez piloter — la tablette de l'entrée, le PC —
ouvrez Home Assistant, allez dans le panneau **Browser Mod**, et activez
**« Register »** pour ce navigateur. Donnez-lui au passage un nom lisible
(`tablette-entree` plutôt que l'identifiant aléatoire) : c'est ce nom que
l'automatisation ci-dessous désigne.

Un navigateur non enregistré n'est pas une erreur visible : l'automatisation
s'exécute, ne trouve pas la cible, et il ne se passe simplement rien.

Chaque navigateur enregistré reçoit un `browser_id`, lisible dans
**Paramètres → Browser Mod**.

```yaml
alias: Sonnette - afficher la camera
mode: single
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
actions:
  # Une fenetre par-dessus la vue courante : rien a fermer a la main, elle
  # disparait toute seule au bout de 60 s.
  - action: browser_mod.popup
    data:
      title: Quelqu'un sonne
      size: wide
      timeout: 60000
      dismissable: true
      browser_id:
        - tablette-entree
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
          auto_unmute: [selected, visible]
        menu:
          buttons:
            microphone:
              enabled: true
              type: momentary

  # Variante : changer carrement de page au lieu d'ouvrir une fenetre.
  # - action: browser_mod.navigate
  #   data:
  #     path: /lovelace/sonnette
  #     browser_id:
  #       - tablette-entree
```

La fenêtre est préférable à la navigation : elle n'abandonne pas ce que vous
étiez en train de faire, et elle se referme seule.

## 6 ter. Un modèle de sonnette éprouvé : l'état « ça sonne »

Le montage [dahua-vto-on-home-assistant](https://github.com/felipecrs/dahua-vto-on-home-assistant)
de Felipe Santos résout le même problème avec un interphone Dahua, et son
automatisation vaut d'être copiée. Le point central qu'il apporte :

> **une sonnette n'est pas un évènement, c'est un état.** « Quelqu'un attend à la
> porte » dure jusqu'à ce qu'on réponde ou qu'on renonce.

Notre `event` et notre `binary_sensor` de 5 s disent « on a appuyé ». Ils ne
disent pas « quelqu'un attend toujours ». C'est cette différence qui permet de
faire sonner *jusqu'à ce qu'on décroche* plutôt qu'une seule fois, et d'afficher
un bouton « Répondre » qui a un sens.

### L'aide-mémoire à créer

**Paramètres → Appareils et services → Aides → Créer un aide → Bascule**, nommé
`Sonnette en cours` (`input_boolean.doorbell_calling`). Un second, `Ne pas
déranger` (`input_boolean.do_not_disturb`), rend le carillon silencieux sans
toucher aux notifications.

### L'automatisation, adaptée au P4

```yaml
alias: Sonnette - quelqu'un attend
mode: single
max_exceeded: silent
triggers:
  - trigger: state
    entity_id: event.doorbell_p4_lvgl_sonnette
actions:
  - action: input_boolean.turn_on
    target:
      entity_id: input_boolean.doorbell_calling

  # Moins fort la nuit. Rien de plus penible qu'un carillon a plein volume a 23 h.
  - action: media_player.volume_set
    target:
      entity_id:
        - media_player.cuisine
        - media_player.chambre
    data:
      volume_level: >-
        {{ 0.6 if is_state('sun.sun', 'below_horizon') else 0.75 }}
    continue_on_error: true

  - parallel:
      - action: notify.all_phones
        continue_on_error: true
        data:
          title: Sonnette
          message: Quelqu'un sonne a la porte
          data:
            # Taper la notification elle-meme ouvre la vue (Android).
            clickAction: /lovelace/sonnette
            # `tag` fait qu'un second appui REMPLACE la notification au lieu
            # d'en empiler une deuxieme.
            tag: doorbell-ringing
            group: doorbell-ringing
            channel: Doorbell
            importance: high
            priority: high
            ttl: 0
            persistent: true
            timeout: 120
            vibrationPattern: 1000, 100, 1000, 100, 1000, 100
            image: /api/camera_proxy/camera.doorbell
            actions:
              - action: URI
                title: Repondre
                uri: /lovelace/sonnette
              - action: IGNORE
                title: Ignorer

      # Une image fixe directement depuis go2rtc, pratique pour un televiseur.
      - action: notify.all_tvs
        continue_on_error: true
        data:
          title: Sonnette
          message: Quelqu'un sonne a la porte
          data:
            image:
              url: http://192.168.1.38:1984/api/frame.jpeg?src=doorbell
            duration: 15
            fontsize: max

  # Le carillon, repete tant que personne n'a repondu -- six fois au plus.
  - if:
      - condition: state
        entity_id: input_boolean.do_not_disturb
        state: "off"
    then:
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
                entity_id: media_player.cuisine
              data:
                media_content_id: /local/sounds/doorbell.mp3
                media_content_type: audio/mp3
            - delay:
                seconds: 5

  - action: input_boolean.turn_off
    target:
      entity_id: input_boolean.doorbell_calling

  # Anti-matraquage : un visiteur impatient ne relance pas six carillons.
  - alias: Eviter le matraquage du bouton
    delay:
      seconds: 15
```

Trois détails qui font la différence entre une sonnette agréable et une sonnette
insupportable, et qu'on ne trouve qu'en s'y étant brûlé :

- **`continue_on_error: true` partout.** Un téléviseur éteint ne doit pas
  empêcher le carillon de la cuisine de sonner.
- **`tag:` sur la notification.** Sans lui, trois appuis donnent trois
  notifications empilées ; avec, la dernière remplace la précédente.
- **`mode: single` + `max_exceeded: silent` + le délai final.** Sans ça, un
  visiteur qui appuie trois fois lance trois séquences qui se chevauchent.

### Fully Kiosk, l'autre façon d'ouvrir la page

Ce montage n'utilise pas Browser Mod mais **Fully Kiosk Browser**, si votre
tablette murale tourne dessus :

```yaml
- action: fully_kiosk.load_url
  target:
    device_id: <l'identifiant de votre tablette>
  data:
    url: https://homeassistant.taild83edc.ts.net/lovelace/sonnette
  continue_on_error: true
```

Plus simple que Browser Mod quand la tablette est déjà en kiosque. Browser Mod
reste préférable pour un PC de bureau ou un navigateur ordinaire (§6 bis).

### Ce que ce montage confirme sur le micro

Sa documentation est catégorique, et rejoint mot pour mot le §4 ter :

> *« It is mandatory that you access your Home Assistant through HTTPS for
> microphone to work. This is a browser restriction for allowing websites to use
> your microphone. »*

Il montre aussi qu'avec go2rtc, l'audio bidirectionnel **fonctionne bel et bien**
dans l'Advanced Camera Card — ce n'est donc pas une impasse en soi. Chez lui, la
caméra fournit du H.264 : une seule source, un seul consommateur. C'est la
contrainte du §4 ter, et elle vient de notre transcodage MJPEG, pas de la carte.

## 7. Vérifier que l'audio bidirectionnel est bien négocié

1. Ouvrez l'interface de go2rtc : `http://192.168.1.38:1984`.
2. Cliquez **probe** sur le flux qui PORTE le backchannel. Vous devez voir
   **trois** pistes :

   ```
   video, recvonly, JPEG
   audio, recvonly, PCMU/8000
   audio, sendonly, PCMU/8000     <-- le backchannel
   ```

   > **Sondez `doorbell_webrtc`, pas `doorbell`.** Une version précédente de
   > cette page disait l'inverse, et c'était une perte de temps garantie :
   > `doorbell` porte `#backchannel=0`, il *renonce* explicitement au
   > backchannel, et n'affichera donc jamais la piste `sendonly`, même sur une
   > chaîne parfaitement saine. La piste vit sur `doorbell_webrtc`, dont la
   > seconde source porte `#backchannel=1`.

   Si la troisième ligne manque **sur `doorbell_webrtc`**, deux causes possibles :
   un `#` de trop dans l'URL (voir `go2rtc/go2rtc.yaml`), ou le P4 qui n'annonce
   la piste que sur demande — voir juste en dessous.
3. Passez `log: {rtsp: trace}` dans go2rtc et relancez : la requête `DESCRIBE`
   doit porter l'en-tête `Require: www.onvif.org/ver20/backchannel`, et le SDP
   renvoyé par le P4 doit contenir `a=sendonly`.

### Le bouton micro n'apparaît pas, même en HTTPS : `backchannel: always`

C'est la panne la plus déroutante de toute la chaîne, parce que **tout
fonctionne** et que rien ne s'affiche.

L'enchaînement :

1. ONVIF dit d'annoncer la piste `sendonly` **uniquement** au client qui envoie
   `Require: www.onvif.org/ver20/backchannel`. C'était le comportement du
   composant, et il est correct.
2. Mais go2rtc, Frigate et les cartes Lovelace décident si une caméra sait
   parler en **sondant** le flux — un DESCRIBE ordinaire, sans cet en-tête.
3. La piste n'est donc pas annoncée à ce sondage. Frigate conclut « pas d'audio
   bidirectionnel », la carte n'affiche pas le bouton micro, et personne ne
   demande jamais le backchannel.
4. Le bloc de statut du P4 le confirme : `backchannel=NO`, et *zéro* DESCRIBE
   portant l'en-tête ONVIF.

Une capacité qui n'existe que sur demande est invisible à qui ne sait pas
qu'elle existe. D'où l'option :

```yaml
rtsp_server:
  backchannel: always   # `auto` = comportement ONVIF strict, le défaut
```

`always` annonce la piste à tout le monde. Frigate et go2rtc voient enfin une
caméra capable de parler, et le bouton apparaît.

> Ne cherchez pas `capabilities: force:` du côté de la carte : cette clé
> n'existe ni en 7.27.4 ni sur `main` (le schéma n'a que `disable` et
> `disable_except`). Une configuration trouvée sur internet qui l'utilise est
> périmée, et la recopier ne fera que casser la validation.

Après le changement, resondez `doorbell_webrtc` : la ligne `audio, sendonly`
doit être là. Si elle y est et que le bouton manque toujours, c'est alors la
contrainte des deux sources du §4 ter.

## 7 bis. « Je ne sais pas si le micro et l'audio fonctionnent »

Ne cherchez pas dans Home Assistant en premier : l'appareil sait répondre tout
seul, et cela sépare en deux minutes une panne de matériel d'une panne de
chaîne. Dans l'ordre :

1. **Bip de test** — bouton `Bip de test` dans Home Assistant, ou le bouton
   « Bip » sur l'écran du P4. Pas de bip ⇒ le problème est le haut-parleur, pas
   le réseau. Et cette fois l'appareil dit *lequel* : regardez les deux compteurs
   `Octets audio proposes au HP` et `Octets audio acceptes par le HP` juste
   après l'appui.

   | Les deux compteurs | Ce que ça veut dire |
   |---|---|
   | proposés = 0 | le bip n'a même pas été synthétisé : pas de `speaker_id`, ou le pipeline audio n'a pas démarré |
   | proposés monte, **acceptés reste à 0** | le composant haut-parleur **refuse tout**. Il est arrêté, ou le format lui déplaît. Aucun réglage de volume n'y changera rien — l'écran affiche `HP refuse tout !` |
   | les deux montent ensemble, et pas de son | l'audio est bien sorti du logiciel : c'est l'ampli (GPIO53), le volume du codec (`output_volume` dans `fdaudio:`) ou le câblage |
   | acceptés < proposés | le haut-parleur n'arrive pas à suivre : le son sera haché |
2. **Boucle locale** — interrupteur `Test audio (boucle micro vers
   haut-parleur)`, ou le bouton « Test micro » sur l'écran. Parlez devant la
   sonnette : vous devez vous entendre. Si oui, **toute la chaîne audio de
   l'appareil est bonne** et ce qui reste est en amont.
3. **Vumètre** — le capteur `Niveau micro` (en dBFS) et la barre sur l'écran du
   P4. Il descend à −100 dBFS au silence et remonte quand on parle.

Comment lire le niveau :

| Lecture | Diagnostic |
|---|---|
| `Micro actif` en défaut | la source ne délivre **plus rien** : mauvais `microphone_id`, codec non démarré, broches I2S fausses |
| −100 dBFS alors que vous parlez | la source délivre du **silence** : mauvais slot I2S, capsule morte, gain à zéro |
| −60 à −40 dBFS | ça capte mais faiblement : montez `mic_gain_db` (fdaudio) ou `gain:` |
| −30 à −6 dBFS | niveau correct |
| au-dessus de −3 dBFS | ça écrête, baissez le gain |

Et pour la voix **descendante** (Home Assistant → sonnette), le capteur
`Paquets audio recus` tranche à lui seul :

| Compteur pendant que vous appuyez sur parler | Où est le défaut |
|---|---|
| reste à 0 | rien n'atteint le P4 : backchannel non négocié, ou go2rtc — voir le §7 |
| il monte, mais on n'entend rien | l'appareil reçoit bien : ampli coupé, volume, ou haut-parleur (faites le bip de test) |

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
| On ne sait pas si le micro capte | faites la boucle locale et lisez `Niveau micro` → étape 7 bis |
| `Paquets audio recus` reste à 0 quand on parle | la voix descendante n'atteint pas le P4 : le défaut est en amont, pas dans l'appareil |
| L'image se fige alors que le flux est actif | l'aperçu LVGL a été coupé sans rendre le dequeue V4L2 au serveur — l'interrupteur « Aperçu caméra locale » de `doorbell-lvgl.yaml` le fait, une config maison doit appeler `set_drive_camera(true)` |
| `codec: h264` refusé à la compilation | voulu : le H.264 a été retiré, voir le §4 ter |
| Le son se coupe quand on parle | normal : `half_duplex: true` coupe le micro pendant l'émission |
| Larsen | l'ampli reste alimenté : câblez la broche `SD` (voir `docs/hardware.md`) |
| Image fluide mais CPU élevé sur la machine HA | c'est le transcodage MJPEG → H.264 de ffmpeg, inévitable pour WebRTC. Baissez `framerate` ou `jpeg_quality` sur le P4, ou regardez la caméra en mode `mjpeg`/`mse` plutôt qu'en `webrtc` |
| Pas d'image dans la carte, mais `doorbell` visible dans go2rtc | la carte pointe sur `doorbell` alors que le P4 est en MJPEG : utilisez `doorbell_webrtc` |
| `RTSP: unsupported transport` | un client force l'UDP ; ce serveur est en TCP interleaved uniquement |
| `Unable to find action with the name 'rtsp_server.…'` | ESPHome compile une ancienne copie du composant : mauvais `ref:` dans votre YAML, ou cache de 24 h. Mettez `refresh: 0s` sur le bloc `source:` et supprimez `.esphome/external_components` — voir le README |
