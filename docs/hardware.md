# Architecture matérielle

## Vue d'ensemble

```
        ┌──────────────┐
        │   OV5647     │
        │  (MIPI CSI)  │
        └──────┬───────┘
               │ 2 lanes MIPI (broches dédiées, hors matrice GPIO)
               │ + I2C/SCCB pour la configuration du capteur
        ┌──────▼─────────────────────────────────┐
        │             ESP32-P4                   │
        │                                        │
        │  ISP  ──► YUV420 ──► encodeur H.264 HW │
        │  I2S0 ◄── micro (INMP441)              │
        │  I2S1 ──► ampli   (MAX98357A)          │
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

## Contraintes GPIO de l'ESP32-P4

À vérifier **avant** de figer un brochage :

| Broches | Usage | Peut-on s'en servir ? |
|---|---|---|
| MIPI CSI D0/D1/CLK | Interface caméra dédiée | Non, hors matrice GPIO |
| GPIO24, GPIO25 | USB-Serial-JTAG par défaut | À éviter (perte du port de debug) |
| GPIO34 – GPIO38 | Broches de strapping | À éviter (GPIO36 est souvent XCLK caméra) |
| Bus SDIO vers l'ESP32-C6 | Wi-Fi | Non — dépend de la carte |
| Flash / PSRAM | Mémoire | Non, broches dédiées |

Le brochage SDIO vers le C6 **change d'une carte à l'autre** (P4-Function-EV-Board,
Waveshare NANO, Waveshare WIFI6, M5Stack Tab5…). Reportez-vous au schéma de votre
carte : c'est la seule source fiable. Si une broche est déjà prise, ESPHome ou le
driver I2S échouera au démarrage avec une erreur explicite dans les logs.

## Brochage proposé

Deux ports I2S séparés — c'est le montage le plus simple et le plus tolérant.

### Microphone I2S — INMP441 / ICS-43434 (I2S0)

| Signal | ESP32-P4 | Module | Remarque |
|---|---|---|---|
| BCLK | GPIO20 | SCK | horloge bit |
| LRCLK / WS | GPIO21 | WS | horloge mot |
| DIN | GPIO22 | SD | données micro → P4 |
| — | GND | L/R | à la masse ⇒ slot **gauche** (`channel: left`) |
| 3V3 | 3V3 | VDD | |
| GND | GND | GND | |

L'INMP441 sort du 24 bits cadré à gauche dans un slot de 32 bits : d'où
`bits_per_sample: 32` dans la configuration. Le composant ne garde que les
16 bits de poids fort.

### Haut-parleur — MAX98357A (I2S1)

| Signal | ESP32-P4 | Module | Remarque |
|---|---|---|---|
| BCLK | GPIO23 | BCLK | |
| LRCLK / WS | GPIO26 | LRC | |
| DOUT | GPIO27 | DIN | données P4 → ampli |
| SD (shutdown) | GPIO28 | SD | **recommandé** — voir plus bas |
| 5V | 5V | Vin | l'ampli tire des pointes de courant |
| GND | GND | GND | |

La broche `GAIN` du MAX98357A laissée en l'air donne 9 dB, ce qui convient à un
haut-parleur 4 Ω / 3 W de sonnette. Reliez-la à GND pour 12 dB si le niveau est
trop faible.

### Sonnerie et carillon

| Signal | ESP32-P4 | Remarque |
|---|---|---|
| Bouton | GPIO32 | `INPUT_PULLUP`, contact vers GND |
| Relais carillon | GPIO33 | via transistor/optocoupleur, jamais en direct |
| LED d'état | GPIO45 | |

## Variante « économie de broches » : un seul port I2S en full duplex

Le micro et l'ampli partagent BCLK et LRCLK, sur deux slots différents :

```yaml
audio:
  microphone:
    i2s_port: 0
    bclk_pin: GPIO20
    lrclk_pin: GPIO21
    din_pin: GPIO22
    bits_per_sample: 32
    channel: left      # L/R du micro à GND
  speaker:
    i2s_port: 0        # même port que le micro -> full duplex
    bclk_pin: GPIO20   # ignoré, les horloges du micro sont réutilisées
    lrclk_pin: GPIO21
    dout_pin: GPIO27
    bits_per_sample: 32  # doit être identique à celui du micro
    channel: right     # SD du MAX98357A à VDD -> slot droit
```

Trois broches économisées. En contrepartie les deux sens partagent la même
horloge et la même largeur de slot ; le composant refuse la configuration à la
compilation si ce n'est pas cohérent.

## Acoustique : le point qui fait ou défait l'audio bidirectionnel

L'ESP32-P4 n'exécute **pas** d'annulation d'écho acoustique (AEC) dans ce
composant. Sans précaution, le micro réentend le haut-parleur et le correspondant
s'entend lui-même, voire déclenche un larsen. Trois mesures, par ordre
d'efficacité :

1. **Half duplex (activé par défaut).** `half_duplex: true` coupe le micro tant
   que de l'audio arrive du backchannel, et le rétablit `talk_timeout` après le
   dernier paquet. C'est le comportement naturel d'un bouton push-to-talk.
2. **Coupure matérielle de l'ampli.** Câblez la broche `SD` du MAX98357A et
   pilotez-la depuis `on_talk_start` / `on_talk_end` (voir `doorbell.yaml`).
   L'ampli est physiquement muet au repos : plus de souffle, moins de
   consommation.
3. **Séparation physique.** Micro et haut-parleur aux deux extrémités du
   boîtier, joint mousse autour de la capsule micro, et pas de chemin rigide
   entre les deux. C'est ce qui rapporte le plus pour le moins d'effort.

Si vous avez besoin de vrai full duplex, il faudra ajouter l'AFE d'`esp-sr`
(AEC matériel) — hors périmètre de ce composant.

## Alimentation

Le P4 avec la caméra MIPI, l'ISP, l'encodeur H.264 et le Wi-Fi via le C6
consomme sensiblement plus qu'un ESP32 classique, avec des pointes lors des
trames clés et des rafales Wi-Fi. Prévoyez :

- une alimentation 5 V / 2 A minimum ;
- un condensateur de découplage de 470 µF à 1000 µF près du module ;
- des fils courts vers l'ampli, qui appelle du courant en impulsions.

Une alimentation juste suffisante se manifeste par des redémarrages en plein
flux ou par un `brownout` dans les logs — pas par une image dégradée.
