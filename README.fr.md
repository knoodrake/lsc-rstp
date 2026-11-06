# LSC PTZ → RTSP local + PTZ ONVIF (sans cloud)

[![checks](https://github.com/knoodrake/lsc-rstp/actions/workflows/ci.yml/badge.svg)](https://github.com/knoodrake/lsc-rstp/actions/workflows/ci.yml) · [MIT](LICENSE)

Transformez une **caméra PTZ LSC / Tuya** bon marché (plateforme Anyka AK3918EV330 ·
AK3168E) en **caméra 100 % locale** : flux RTSP bruts et un service **ONVIF PTZ**
fonctionnel, sans cloud Tuya, sans appli vendeur, sans compte. Conçu pour que
**Frigate** (et Home Assistant) puisse piloter le PTZ, **autotracking inclus**.

*Lire en : [English](README.md) · **Français***

> **Comment ceci a été écrit — assisté par IA, 100 % local.** Le code, la documentation et le
> travail de rétro-ingénierie de ce dépôt ont été produits en binôme avec un **agent IA
> local** (un modèle DeepSeek tournant sur la machine de l'auteur : aucun service d'IA dans
> le cloud, aucun code envoyé à un tiers). Chaque affirmation du tableau d'état ci-dessous a
> été mesurée sur des caméras physiques ; ce qui n'est pas vérifié est explicitement signalé.
> Relisez le code avant de le faire tourner sur votre matériel.

> **Statut : preview v0.1.** Tout ce qui suit a été développé et vérifié sur **trois
> caméras physiques**. Le logiciel embarqué côté caméra est prouvé ; l'*installeur en
> une commande* est en cours de conditionnement. Voir la
> [matrice d'état](#état--ce-qui-est-prouvé).

---

## Pourquoi

Les PTZ Tuya ne passent que par le cloud et n'exposent aucun PTZ local utilisable. Sur
la plateforme Anyka le matériel sait déjà tout faire — RTSP, encodeur H.264, moteur PTZ
avec retour de position — mais le firmware vendeur ne l'expose que dans un **mode test
usine** caché. Ce projet rend ce mode permanent, pose un démon ONVIF au-dessus du
moteur, et vous donne une caméra qu'un NVR normal sait piloter.

## Ce que vous obtenez

| | |
|---|---|
| **RTSP** | `rtsp://<cam>:554/main_ch` — H.264 1920×1080 (+ audio AAC) |
| **RTSP sub** | `rtsp://<cam>:554/videoSub` — 640×360, *si votre build le supporte* (voir limites) |
| **ONVIF PTZ** | `http://<cam>:8080/onvif/ptz_service` — RelativeMove / ContinuousMove / Stop / presets / GetStatus |
| **Santé** | `GET http://<cam>:8080/healthz` (JSON, 200 seulement si le backend PTZ répond) |
| **Frigate** | fonctionne avec `onvif:` + `autotracking`, `return_preset` inclus |
| **Sans cloud** | la caméra n'a plus jamais besoin de joindre Tuya |

## Matériel supporté

Vérifié sur trois exemplaires de la même famille :

| Build dans `share/VERSION` | Remarques |
|---|---|
| `3.10.62.5` | tout, sous-flux disponible |
| `3.10.56.1146` | flux principal + PTZ, sous-flux absent |
| `3.287.75.0` | flux principal + PTZ, sous-flux absent |

D'autres caméras PTZ **Anyka AK3918 / AK3168** sont probablement compatibles :
l'installeur **dumpe votre propre firmware** et n'y injecte que notre couche, il
s'adapte donc à votre build au lieu de livrer une image figée.

## Prérequis

* la caméra, et une **carte microSD** (≤ 32 Go, FAT32) pour le premier contact
* **un PC Linux** sur le même LAN — uniquement pour le mode *card-free* (Python 3.9+,
  `mksquashfs`/`unsquashfs` de `squashfs-tools`, `telnet`)
* éventuellement **Frigate** et/ou **Home Assistant**

## Comment ça marche (deux modes d'installation)

Les deux commencent pareil : on écrit une **carte sonde**. Au boot le firmware vendeur
la lit, ouvre un shell root sur le port 24 et expose le RTSP — c'est votre point d'entrée.
Ensuite, deux choix :

```
     carte microSD sonde  ──▶  la caméra boote : RTSP :554 + shell root :24
                                      │
            ┌─────────────────────────┴──────────────────────────┐
            ▼                                                    ▼
  MODE A — SD seule (sans PC)                      MODE B — bake (card-free)
  la carte porte aussi nos démons                  l'installeur dumpe votre APP,
  et les lance à chaque boot                       y injecte notre couche, reflashe
  la carte reste dans la caméra                    mtd7 avec vérification md5
  ✔ aucun logiciel sur un PC                       ✔ carte retirée ensuite
  ⚠ expérimental en v0.1                           ✔ le mode vérifié sur 3 caméras
```

* **[Mode B — installation card-free (PC)](docs/en/install-pc.md)** — le chemin recommandé.
* **[Mode A — installation SD seule (sans PC)](docs/en/install-sd.md)** — expérimental.
* **[Mode C — provisioning par point d'accès (téléphone)](docs/en/install-ap.md)** —
  prévu ; `hostapd` et la page web de configuration tournent déjà sur la caméra.

## Démarrage rapide (Mode B)

```bash
git clone https://github.com/LocalActionTuyaCams/lsc-rstp.git
cd lsc-rstp
pip install -r installer/requirements.txt

# 1. construire la carte sonde (écrit sur une carte FAT32 montée)
sudo python3 installer/sd_card.py --ssid "VotreWiFi" --pass "VotreMotDePasse" \
    --ip 192.168.1.50 --netmask 255.255.255.0 --gateway 192.168.1.1 --out /media/carte

# 2. insérer, allumer, attendre ~60 s, vérifier le point d'entrée
telnet <ip-cam> 24

# 3. baker la caméra (dump → injection → flash → relecture md5)
python3 installer/install.py <ip-cam> --apply

# 4. retirer la carte, rebooter, vérifier
python3 tests/validate_cam.py <ip-cam> 8080
```

## État — ce qui est prouvé

| Composant | État |
|---|---|
| RTSP `main_ch` (1080p) depuis le mode usine | **vérifié**, 3 caméras, survit au reboot |
| Démon ONVIF PTZ (`lsc_onvif`, dans la caméra) | **vérifié** — 22/23 tests de conformité par caméra |
| `RelativeMove` boucle fermée + `GetStatus` | **vérifié**, ±0.06 en espace FOV |
| Presets (`home`, `level`) | fonctionnent, **non persistants au reboot** (voir limites) |
| Autotracking Frigate | **vérifié** bout en bout après le correctif de tilt |
| Sous-flux `videoSub` | vérifié sur `3.10.62.5`, **absent** sur deux builds |
| Installeur PC (`install.py`) | **fonctionne**, exécuté en vrai sur 2 caméras — en cours de conditionnement |
| Install SD seule | conçue, partiellement implémentée, **expérimentale** |
| Provisioning AP | composants construits et exécutés sur caméra, **pas encore câblés** |

## Limites (à lire avant de flasher)

1. **On s'appuie sur le « mode test usine » du firmware.** On ne remplace pas le
   firmware : on bascule la caméra dans un mode que le vendeur livre déjà. Une mise à
   jour vendeur peut casser le mécanisme.
2. **La caméra balaie ses moteurs et bipe au boot** (comportement vendeur de ce mode).
3. **Pas de DHCP en mode usine** — la caméra exige une **IP statique** écrite sur la carte.
4. **Les presets vivent en RAM** — après un reboot la caméra oublie `level`, donc un
   `return_preset: level` de Frigate retombe sur une position périmée jusqu'à ré-enregistrement.
5. **Les conventions de l'axe tilt** ne sont pas celles qu'on attend sur ce matériel :
   `y=0` n'est *pas* le milieu de la plage, et le signe est inversé par rapport à
   certains clients ONVIF. Ce projet le calibre à partir d'un preset `level` enregistré.
6. **Le sous-flux dépend du build** (encodeur présent, mais une seule session RTSP
   exposée sur certains builds).
7. Ce n'est pas un remplacement du cloud Tuya pour la *configuration* : notifications et
   enregistrement cloud disparaissent, par choix.

## Sécurité & retour arrière

Rien d'irréversible tant que vous gardez la partition `APP` d'origine : l'installeur la
dumpe avant de toucher à quoi que ce soit, et `docs/en/rollback.md` explique comment
remettre une caméra d'usine. Le pire cas sur ces modèles est un reflash par telnet, ce
que nous avons fait des dizaines de fois. **Garantie : void.** À vous de voir.

## Légal

Licence MIT (voir `LICENSE`). Le firmware vendeur a été rétro-ingéniéré pour
interopérabilité — un droit établi dans la plupart des juridictions — et nous **ne
redistribuons aucun binaire vendeur** : les patcheurs sont des scripts qui modifient un
dump de *votre* caméra. Les composants tiers et leurs licences sont dans `THIRD_PARTY.md`.

Non affilié à Tuya, Anyka ou LSC. Utilisation à vos risques.

## Documentation

Toute la documentation est en anglais : [docs/en/](docs/en/). Points d'entrée :
[installation card-free](docs/en/install-pc.md) ·
[SD seule](docs/en/install-sd.md) ·
[limites](docs/en/limitations.md) ·
[retour arrière](docs/en/rollback.md) ·
[Frigate](docs/en/frigate.md).

## Crédits

Ce travail s'appuie sur ces projets (aucun n'est affilié à ce dépôt, aucun de leurs auteurs
ne l'endosse) :

| Projet | Licence | Ce qu'il a apporté |
|---|---|---|
| **hostapd** / **wpa_supplicant** (Jouni Malinen et contributeurs) | BSD-3-Clause | le provisioning en point d'accès ; notre build ARMv5 et l'enveloppe de compatibilité `-B` sont dans [`ap-mode/`](ap-mode/README.md) |
| **libnl** | LGPL-2.1 | lié statiquement dans ce build hostapd, pour le pilote `nl80211` |
| **uClibc** | LGPL-2.1 | la libc des caméras ; nos binaires ARM sont liés contre elle |
| **busybox** | GPL-2.0 | déjà sur la caméra ; la couche pilote ses `telnetd`, `sed`, `awk` |
| **squashfs-tools** | GPL-2.0 | `mksquashfs` côté PC, pour construire l'image applicative |
| **Frigate** / **go2rtc** | MIT / MIT | le NVR qui consomme les flux et pilote le PTZ ONVIF ; la lecture de son client ONVIF est ce qui a rendu l'autotracking possible |
| **ONVIF** | spécification | l'interface ouverte que ce démon implémente (un sous-ensemble mesuré) |
| **Anyka · LSC** | firmware vendeur | la plateforme et le firmware ; aucun code vendeur n'est redistribué (cf. [THIRD_PARTY.md](THIRD_PARTY.md)) |
| **Famille de modèles DeepSeek** | — | le LLM local utilisé comme assistant, décrit en haut de ce fichier |

Si vous estimez que votre projet mérite une autre formulation ou une citation explicite,
ouvrez une issue — ce sera corrigé.

## Contribuer

Issues et pull requests bienvenues — surtout des **rapports de build** d'autres modèles
Anyka/Tuya PTZ (`share/VERSION`, md5 de `anyka_ipc`, présence de `:554`). Un modèle à la
fois : ouvrez une issue, on l'ajoute à la table de compatibilité.
