# DS4-V2-Updater

Notes de recherche autour d'une manette **DualShock 4 V2 (CUH-ZCT2E)** : d'abord
sa mise à jour firmware depuis un PC sans PS4, puis son fonctionnement sur
**PlayStation TV**.

> Ce README est un **document de reprise**. Il condense tout ce qui a été appris
> pour pouvoir redémarrer le projet plus tard (avec Claude, Codex, ou seul).
> État au **2026-08-05**. Le bilan technique actuel est dans
> [`docs/BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md`](docs/BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md).

Le document couvre deux volets distincts, menés dans cet ordre :

| Volet | Sujet | État |
|---|---|---|
| **I** (§1-8) | Updater firmware DS4 depuis un PC, sans PS4 | Bloqué sur la crypto Sony, rien codé |
| **II** (§11-16) | Faire fonctionner la DS4 v2 sur PS TV | Blocage post-L2CAP largement isolé ; correctif définitif non trouvé |

Sources (§17) et glossaire (§18) sont communs aux deux volets. Le **journal
exhaustif des 26 tests** menés le 2026-07-31 est en §16. Les tests 74 à 77 et
leur interprétation sont documentés dans le rapport de campagne d'août 2026.

---

## 1. Le besoin de départ

Mettre à jour le firmware d'une DS4 V2 (CUH-ZCT2E) alors qu'on n'a **pas de PS4**
sous la main. Question dérivée : est-ce qu'un outil PC / communautaire est faisable ?

---

## 2. Constat principal

- **Sony n'a jamais publié d'utilitaire PC de mise à jour firmware pour la DS4.**
- L'outil officiel *« Firmware Updater for DualSense »* (Windows/macOS) ne concerne
  **que la manette PS5 (DualSense)**, pas la DS4.
- Le firmware d'une DS4 ne se met à jour qu'**en USB, branchée sur une PS4**
  (ou PS5 pour certaines opérations), automatiquement, via le système de la console.

**Solutions immédiates sans PS4 à soi** : PS4 d'un proche, magasin/réparateur,
médiathèque équipée. Le flash prend ~2 min, aucune manipulation.

> À noter : les mises à jour firmware DS4 sont **rarissimes** et ne corrigent
> quasiment rien de visible côté utilisateur. Avant de vouloir « mettre à jour »,
> vérifier que le vrai problème n'est pas la **calibration** (voir §6).

---

## 3. Pourquoi un « updater PC » est difficile

Chaîne de mise à jour connue (via reverse engineering) :

1. Entrée en mode DFU : `SET 0xa2 [0x01]`, puis reset `SET 0xa0 0x04`.
2. La manette se ré-énumère : `054c:05c4` (normal) → **`054c:0856` (DFU)**.
3. Upload firmware par **chunks** : `SET 0xf0 0x00` ... puis finalisation `SET 0xf0 0x01`.

Le protocole HID (GET report type `0x01`, SET report type `0x09`) est **documenté**.
**Ce n'est PAS le blocage.**

Le blocage réel :

- **Le firmware est chiffré ET signé par Sony.** C'est très probablement le
  **bootloader de la manette** qui vérifie (pas la console). La console ne fait
  que **relayer un blob déjà scellé**.
- **Personne n'a d'image firmware flashable publique.** L'auteur du reverse
  cherchait lui-même une image compatible.
- La flash du MCU (famille **Fujitsu/Spansion FM3**, ARM Cortex-M3) a une
  **protection en lecture** → renvoie `0xFF` en mode programmation série.

### Le pivot qui décide de TOUT

> **La séquence de flash est-elle rejouable (replay) ?**
>
> - Si le blob signé est **poussé tel quel**, sans nonce ni clé de session
>   par-manette → **un replay de capture suffit**, projet viable.
> - S'il y a un **challenge-response par device** → une capture n'est pas
>   rejouable, il faudrait casser la crypto Sony → **mur**.
>
> fail0verflow documente que la manette fait déjà une **authentification RSA
> (challenge-response)** avec la console. Donc de la crypto par-device existe
> dans ce protocole. **Personne n'a démontré publiquement un replay DS4 réussi.**
> On ne connaît pas la réponse tant qu'une **capture réelle** n'a pas été analysée.

### Limite éthique/légale posée

- **OK** : rejouer l'image **signée par Sony** sur **sa propre** manette (= ce que
  fait la console). C'est du replay, pas un contournement.
- **NON** : casser le chiffrement/la signature pour faire tourner un firmware
  **modifié**. Hors scope, pas d'aide là-dessus.

Autres risques : matching modèle strict (**ZCT1 ≠ ZCT2**, MCU/firmware différents →
mauvaise image = brique), anti-rollback éventuel, **brique** en cas de flash
interrompu. L'auteur de `ds4-tools` prévient : « sois prêt à jeter ta manette ».
Il a lui-même briqué une DS4 dans un mode série non documenté (part 6).

---

## 4. La piste PS4 jailbreakée

Une PS4 jailbreakée est l'**instrument idéal** pour la phase de capture :

1. **Hook USB logiciel** pendant une vraie mise à jour → capture la séquence de
   chunks + le payload, **et** le côté console (ce qui déclenche l'update, présence
   éventuelle d'un nonce). **Méthode préférée** : zéro risque de timing, la plus riche.
2. Localiser le payload firmware stocké dans le système de la console.
3. Déchiffrement PUP possible sur certains firmwares (selon l'état de la scène).

> **Intérêt communautaire** : il suffit d'**une seule personne** avec une PS4
> jailbreakée pour capturer **une fois**. Tout le monde en profite ensuite —
> *si* le replay marche.

---

## 5. Capture matérielle (man-in-the-middle USB)

Deux familles :

- **Tap passif (sniffer)** : écoute le bus, ne touche à rien. ← **ce qu'on veut.**
- **Proxy actif (vrai MITM)** : se fait passer pour la manette, peut modifier.
  **Dangereux ici** : casse sur les ré-énumérations (celles du DFU !) → risque de
  brique. **Ne jamais mettre un proxy actif pendant le flash.**

| Outil | Prix approx. | Vitesses | Passif / Actif | Note |
|---|---|---|---|---|
| **Cynthion** (Great Scott Gadgets) | ~250-300 € | Low/Full/High | Passif (Packetry) + Actif (LUNA/Facedancer) | **Recommandé.** 3 ports intégrés, rien à souder. |
| Total Phase **Beagle USB 480** | >1000 € | USB 2.0 | Passif | Référence industrielle, surdimensionné. |
| **Raspberry Pi Zero** (USB gadget) | ~15 € | Full Speed | Actif (proxy) | Le plus reproductible, mais mauvais pour la phase flash. |
| **GreatFET One** + Facedancer/USBProxy | ~100 € | Full Speed | Actif | Intermédiaire, artisanal. |

Un **tap passif** se fiche des ré-énumérations DFU → sûr pour la phase de flash.
Un tap passif répond aussi au **pivot du §3** : il révèle si le chemin d'update
embarque un nonce.

**DS4 côté USB** : composite HID + USB Audio, `bcdUSB 2.00`, `bMaxPacketSize0 = 64`,
VID `0x054C` / PID `0x05C4`. Probablement **Full Speed** — vérifier avec `lsusb -v`
avant d'acheter un outil mono-vitesse. Cynthion couvre les 3 vitesses (pas le souci).

---

## 6. Ce qui existe DÉJÀ (ne pas réinventer)

La « boîte à outils DS4 communautaire » est **en grande partie déjà faite** :

- **[dualshock-tools.github.io](https://dualshock-tools.github.io/)** — web app
  **WebHID** (Chrome/Edge, Firefox via extension), rien à installer. Fait : infos
  software **et hardware**, calibration centre + range des sticks (normal/expert),
  finetuning, visualisation temps réel + check de circularité, changements
  permanents/temporaires, restauration, reboot. DS4, DualSense, DS Edge, VR2.
  → **Ne fait PAS la calibration gyro/IMU. Ne fait PAS de firmware.**
  → Précise : *« cannot fix stick drift »* (le drift est mécanique, calibration ≠ réparation).
- **[dualshock-tools/ds4-tools](https://github.com/dualshock-tools/ds4-tools)** —
  CLI Python : `ds4-tool.py` (commandes non documentées), `ds4-calibration-tool.py`,
  `ds5-calibration-tool.py` (expérimental). **Bonne base de code réutilisable** pour
  le dialogue HID. Windows : driver via **Zadig** requis.
- **[DS4Windows](https://ds4windows.dev/)** / **DualSenseX** — usage (remap, profils
  par jeu, gyro pour le jeu, émulation Xbox). Très mûrs, autre besoin.

### Les vrais trous (non couverts par l'existant)

1. **Calibration gyro/IMU** : absente de la web app, alors que le reverse documente
   la restauration des valeurs IMU. **Créneau réel, risque modéré.**
2. **Firmware** (capture / décodage / replay DFU) : **rien de public.** = le pivot
   spéculatif du §3.

---

## 7. Verdict de faisabilité

- **Outil diag/calibration PC** : faisable… mais **déjà fait** (§6). Refaire = exercice.
- **Contribution gyro/IMU** : faisable, valeur réelle. Idéalement en **PR sur
  dualshock-tools** plutôt qu'un énième clone.
- **Updater firmware complet** : **~50/50**. Le chiffre ne bougera qu'après **une
  capture** analysée (pivot §3). Facteurs négatifs : firmwares rares & sans bénéfice
  visible, matching modèle strict, risque de brique, ironie « il faut une PS4 pour
  fabriquer l'outil censé s'en passer ».

---

## 8. Prochaines étapes proposées (ordre conseillé)

Faire d'abord ce qui a de la valeur **quelle que soit** la réponse au pivot :

1. **Décodeur de capture** (aucune manette touchée) : parser un pcap / log de hook,
   reconstituer la séquence de feature reports HID, isoler les chunks `SET 0xf0 0x00`,
   **détecter la présence d'un nonce**, réassembler le blob. → **Tranche le pivot §3.**
2. **Une capture** : hook logiciel sur PS4 jailbreakée (préféré) ou tap passif Cynthion.
3. **Selon le résultat du pivot** :
   - replay possible → écrire l'updater (replay du blob signé, sa propre manette).
   - replay impossible → s'arrêter là côté firmware ; se rabattre sur la
     **contribution gyro/IMU** à dualshock-tools.

**Décisions ouvertes à trancher au redémarrage :**
- Stack : Python (`hidapi`, base `ds4-tools`) **ou** web app WebHID (zéro install) ?
- A-t-on accès à une PS4 jailbreakée (modèle + firmware) ?
- Cible : contribution gyro sur l'existant, ou pari firmware ?

---

# Volet II — DS4 v2 sur PlayStation TV

## 11. Le pivot

Le volet I bute sur un mur (crypto Sony, §3). Le volet II attaque un problème
concret et, lui, solvable : faire fonctionner la **DS4 v2 (PID `0x09CC`)** sur une
**PS TV** sous HENkaku/**Enso**. Même manette, tout autre sujet.

**Symptômes de départ :**

- Ne fonctionne pas du tout en USB filaire.
- L'appairage Bluetooth natif est capricieux (a nécessité l'aide d'une v1).
- **Ne se reconnecte pas au redémarrage** ← le vrai irritant.
- Input lag ressenti une fois connectée.

Une fois appairée à la main, la manette **fonctionne bien en Bluetooth natif**.
Le problème est donc la *reconnexion automatique*, pas l'usage.

---

## 12. Environnement (installé le 2026-07-31)

| Élément | Détail |
|---|---|
| VitaSDK | `C:\vitasdk\vitasdk` — autobuild `master-win-v2.540`, **gcc 15.2.0** |
| taiHEN | via `vdpm taihen` |
| cmake / ninja | 4.4.1 / 1.13.2 (scoop) |
| GNU make | `C:\vitasdk\vitasdk\bin\make.exe` — requis par `vita_create_stubs` |
| Console | PS TV sous Enso, FTP `172.20.10.2:1337` |
| VitaCompanion | installé — expose le FTP (1337) et un serveur de commandes (1338) **dès le boot** |

**Trois correctifs** ont été nécessaires pour compiler du code de 2020 avec des
outils de 2026 : `cmake_minimum_required` 2.8 → 3.10 ; suppression d'un
`add_dependencies` invalide (depuis CMake 3.20, `vita.cmake` nomme la cible
`.skprx-self` et gère la dépendance seul) ; 8 casts `(uintptr_t)` → `(void *)`
(`-Wint-conversion` est une erreur depuis GCC 14).

> SourceForge est injoignable depuis cette machine (HTTP 000) : `scoop install make`
> et `winget install ezwinports.make` échouent tous deux pour cette raison, pas à
> cause du gestionnaire de paquets. Prendre les binaires sur GitHub.

---

## 13. Contenu du dépôt

```
plugin/      fork de MERLev/ds34vita (51 commits, remote « upstream »)
             → référence de lecture, pas la base produit. build.ps1 fourni.
ds4v2fix/    sonde d'observation par hooks sur les exports/imports de SceBt
ds4v2bt/     plugin qui mène lui-même le dialogue HID (voir §15.8)
ds3trace/    traceur du pilote SceDs3 — le livrable réutilisable (§15.9)
refs/        matériel de reverse, non versionné :
               EyeToyPSVita/          dépôt de référence (§15.7)
               PSVita-RE-tools/       outillage TeamFAPS
               bootimage-modules/     56 modules kernel extraits (§15.10)
               bt.asm, ds3.asm        désassemblages Thumb
logs/        journaux rapatriés, sauvegardes de config.txt
```

`ds4v2fix` se pilote par `ur0:/tai/ds4v2fix.cfg` (une ligne) :
`spoof=0 block=0 rescue=0 replay=0 delay=30`. Journal dans
`ur0:/log/ds4v2fix.txt`.

`replay=1` est un mode expérimental, désactivé par défaut : il ne concerne que
la MAC de la V2 étudiée et tente de fournir la réponse capturée au feature
report `0x06`. Il doit rester désactivé en usage normal.

**Rien n'est commité en git** à ce stade.

### Règles de sûreté apprises à la dure

1. **Jamais d'I/O fichier dans un hook kernel.** Une première version journalisait
   via `ksceIoOpen` depuis un hook posé sur `SceBt → SceRegMgr` ; comme écrire un
   fichier consulte le registre, chaque log rappelait le hook → **récursion
   infinie, console en boot loop**. Les hooks n'écrivent plus que dans un tampon
   RAM, un thread dédié fait le flush.
2. **Poser les hooks depuis un thread différé (30 s), pas dans `module_start()`.**
   Le démarrage reste sain : un plantage éventuel survient après que le réseau
   soit monté, donc le FTP reste accessible pour retirer le plugin.
3. **Sortie de boot loop** : une boucle qui retente l'upload FTP toutes les 2 s
   attrape la fenêtre réseau du démarrage (prise à la 4ᵉ tentative, ~8 s) et
   permet de réécrire `ur0:/tai/config.txt` sans la ligne fautive. Garder une
   copie locale du `config.txt` sain **avant** toute modification.
4. **Armement à usage unique** *(ajouté le 2026-08-03, après un second boot
   loop)*. Le plugin ne fait rien tant qu'un fichier `ur0:/tai/<nom>.on` est
   absent, et il le **supprime avant de toucher à la moindre API**, en
   vérifiant qu'il a disparu. Un essai consomme son armement : s'il plante, le
   démarrage suivant trouve le plugin désarmé et la console revient seule.
   **Un boot loop devient structurellement impossible.** La règle 3 reposait
   sur le fait de gagner une course contre la fenêtre de démarrage ; celle-ci
   supprime la course. Elle a rendu indolores les quatre plantages du
   2026-08-03.
5. **Journaliser avant l'appel, pas après.** Une fonction qui écrit *et vide sur
   disque* dans la foulée, appelée avant chaque appel système risqué : un
   plantage laisse alors sur le disque le nom de la fonction qui n'est jamais
   revenue. Sans ça, le tampon RAM part avec le crash et le journal ne dit rien
   de sa propre mort — erreur commise deux fois dans la même journée.
6. **Ne pas laisser cette journalisation dans un chemin critique.** Chaque vidage
   coûte ~25 ms (ouverture, écriture, fermeture sur `ur0:`). Dans un gestionnaire
   d'événements, trois lignes suffisent à rater une fenêtre de 50 ms. Règle
   pratique : **émettre d'abord, journaliser ensuite, et en RAM seulement** ; le
   vidage disque appartient au thread, quand il ne se passe rien.

---

## 14. Découvertes

### 14.1 La table d'appairage Bluetooth

Elle est dans **`vd0:/registry/system.dreg`** (524 288 o), à partir de `0x4e00`,
un enregistrement tous les **`0x800`** octets :

| Offset relatif | Champ |
|---|---|
| `+0x60` | MAC, 6 octets, **ordre inverse** de l'affichage usuel |
| `+0x70` | link key Bluetooth (16 octets) |
| `+0x88` | VID (uint16 LE) |
| `+0x8a` | PID (uint16 LE) |
| `+0xe0` | nom, chaîne ASCII terminée par zéro |

Contenu relevé (enregistrement vide = MAC à zéro) :

| Slot | MAC | VID:PID | Nom |
|---|---|---|---|
| `0x6600` | A4:15:66:6D:D1:A6 | `054C:05C4` | **V1** |
| `0x6e00` | C8:22:B2:6F:BC:D3 | `054C:05C4` | **V2** |
| `0x7600` | C6:63:D6:C5:21:30 | `0000:0000` | enceinte Bluetooth |
| `0x7e00` | 39:B0:14:8D:18:48 | `054C:0CE6` | DualSense (appairée via ds34vita) |

L'écriture passe par la clé registre **`/CONFIG/BT/01/info`** (1928 octets),
visible en hookant `SceBt → ksceRegMgrSetKeyBin`. **Seule une mise en appairage
neuve déclenche cette écriture** ; une simple reconnexion ne touche pas au
registre — d'où un premier test resté vide et mal interprété.

### 14.2 Hooks : exports contre imports

`taiHookFunctionExportForKernel` n'intercepte que les appels venant **d'autres
modules**. Le code interne de SceBt n'appelle pas ses propres exports : trois
hooks posés avec succès sur `ksceBtGetVidPid`, `ksceBtDeleteRegisteredInfo` et
`ksceBtGetRegisteredInfo` sont restés **totalement muets** pendant une
reconnexion complète. C'est pourquoi ds34vita hooke des offsets internes en dur
(`0x22999C8 - 0x2280000`), au prix d'une dépendance au firmware.

`taiHookFunctionImportForKernel` est le bon compromis : un appel inter-modules
passe forcément par la table d'imports. Hooker `SceBt → SceRegMgr` a
immédiatement révélé `/CONFIG/BT/01/info`, là où les exports ne donnaient rien.

À noter aussi : `ksceBtGetRegisteredInfo` **ne liste pas les manettes**, seulement
les périphériques Bluetooth génériques (l'enceinte). Ne pas en conclure qu'aucune
manette n'est appairée.

### 14.3 Le feature report `0x81`

Mesure différentielle en USB via dualshock-tools :

| | V1 | V2 |
|---|---|---|
| PID | `054C:05C4` | `054C:09CC` |
| Carte | JDM-001 (`HW 0100:3100`) | JDM-040 (`HW 0100:6404`) |
| Build Date | Aug 3 2013 | **May 17 2016** |
| Report `0xA3` | répond | répond |
| Report `0x81` | **répond** | **échoue** |
| Verdict de l'outil | `original` | `clone` |

La V1 sert de témoin et prouve que WebHID/Windows ne bloquent rien : l'échec de la
V2 est réel.

**Mais l'étiquette « clone » est un faux positif documenté.** Le report `0x81`
n'est pas implémenté par toutes les DS4, y compris des CUH-ZCT2 authentiques :
le noyau Linux a dû être corrigé pour ce cas (patch *« HID: sony: Support for DS4
clones that do not implement feature report 0x81 »*, puis pilote `hid-playstation`
depuis la 6.2), et **DS4Windows lit la MAC via le report `0x12`**, qui fonctionne
partout. La manette vient de chez Darty et sa Build Date de mai 2016 précède de
quatre mois la sortie commerciale du modèle : première série plausible.

Le profil décrit dans ces échanges noyau colle exactement au cas observé :
**ces manettes fonctionnent en Bluetooth mais coincent en USB.**

---

## 15. Où on en est

> **État au 2026-08-05.** Les §15.1 à 15.18 restent des relevés historiques.
> Le feature report `0x06` a été infirmé comme cause le 3 août ; la capture du
> 5 août montre que la completion L2CAP native débloque la configuration mais
> pas encore HID. La synthèse à jour est dans
> [`docs/BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md`](docs/BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md).
>
> En une phrase : le verdict d'échec tombe **avant** que le pilote n'émette quoi
> que ce soit, et la manette ne fonctionne toujours pas.

### 15.1 ds34vita voit et connecte la V2

Fork recompilé avec `add_definitions(-DLOG_DISC)`, plus **une ligne ajoutée** :
`log_flush()` en fin de `bt_cb_func`. Sans elle, `LOG()` ne fait que remplir un
tampon de 16 Ko vidé uniquement au déchargement du module — donc jamais en usage
réel. C'est sûr, `bt_cb_func` étant un callback exécuté dans le contexte du thread
BT et non dans un hook.

Trace obtenue (`ux0:/log/ds34vita.txt`), ds4v2fix désactivé :

```
CONNECTED DS4[B26FBCD3 0000C822] TO PORT 1     (×4, une par tentative)
```

`B26FBCD3` + `0000C822` = **C8:22:B2:6F:BC:D3**, la V2. Donc ds34vita **la détecte,
l'identifie comme DS4 et la connecte**. Aucun événement `0x09` (connexion sans
appairage) : l'appairage est bien reconnu.

### Deux faux symptômes à ne pas suivre :

- **`PORT 1` est normal.** `get_free_port()` boucle volontairement à partir de
  `i = 1` (`main.c:329`) : le port 0 est réservé au système, et les hooks `SceCtrl`
  réinjectent `controllers[1]` quand le jeu lit le port 0.
- **Les échecs de hook `SceTouch` / `SceMotion` (`0x90010002`) sont attendus** :
  la PS TV n'a ni écran tactile ni gyroscope.

### 15.2 Deux bugs dans ds34vita — `vid_pid` non initialisé

**C'est le résultat principal de la journée.** Aux deux endroits où ds34vita
identifie un périphérique, le tableau `vid_pid` n'est pas initialisé et le code de
retour de `ksceBtGetVidPid()` n'est pas vérifié. Quand cet appel échoue — ce qui
est le cas avec la V2 de ce projet, qui ne répond pas aux requêtes
d'identification — le code teste **des résidus de pile**, différents à chaque
tentative.

**`case 0x05` (connexion acceptée)** — la manette est prise pour ce que le hasard
désigne :

| Résidu de pile | Branche | Symptôme observé |
|---|---|---|
| ≈ `054C:0CE6` | `is_ds5` → report `0x31` (73 o) | **LED bleue + vibration**, puis déconnexion |
| ≈ `054C:05C4/09CC` | `is_ds4` → report `0x11` (13 o) | **LED rose**, la manette fonctionne |
| autre | aucune | **LED blanche**, aucun report, déconnexion |

Une DS4 qui reçoit un report DualSense de 73 octets au lieu de 13 le décode de
travers : d'où la couleur et les moteurs. Cela explique intégralement le
comportement erratique (six tentatives, trois comportements différents).

**`case 0x01` (résultat d'inquiry)** — même défaut, conséquence plus gênante : le
code de retour n'est même pas récupéré, et si les résidus ressemblent à une DS4,
`ksceBtStopInquiry()` **coupe la recherche Bluetooth en cours**, y compris celle
lancée depuis les paramètres système, et pour n'importe quel appareil. Symptôme :
l'interface d'appairage ne trouve plus rien.

**Correctifs appliqués dans le fork** (`plugin/main.c`) :

1. `vid_pid` initialisé à `{0, 0}` aux deux endroits, tests conditionnés à
   `result == 0`.
2. **Repli par le nom** : si l'identification VID/PID échoue mais que l'appareil
   s'annonce `Wireless Controller`, il est traité en DS4 (`is_wireless_controller_name()`,
   comparaison manuelle, `str*()` n'étant pas disponible côté kernel).
3. `log_reset()` passé en mode ajout (`plugin/log.c`) : l'amont vidait le journal à
   chaque démarrage, faisant perdre la trace du test précédent.
4. `LOG` complet du `0x05` et du `0x01` : code de retour, VID/PID lu, nom.

### Les deux enregistrements sont identiques

Diff binaire des blocs V1 (`0x6600`, se reconnecte) et V2 (`0x6e00`, ne revient
pas), sur le même fichier :

| Champ | V1 | V2 | |
|---|---|---|---|
| `+0x68` classe BT | `08 25 00 00` | `08 25 00 00` | identique |
| `+0x88` VID:PID | `4c 05 c4 05` | `4c 05 c4 05` | identique |
| `+0x8c` flags | `00 01 00 02` | `00 01 00 02` | identique |
| `+0xe0` nom | `Wireless Controller` | `Wireless Controller` | identique |

Seules différences : MAC, link key, un compteur (`+0x6c`, V1=7 / V2=11) et un
horodatage (`+0xa0`). **Le registre n'explique donc rien** : la V2 y est décrite
exactement comme la V1. Le problème est comportemental, pas dans les données.

### Le test de la fenêtre de démarrage : négatif

Hypothèse testée : la manette tenterait de se reconnecter avant que la pile BT de
la console soit prête. Protocole — démarrage complet, attente de 30 s
supplémentaires, *puis* appui sur PS.

Résultat : **comportement inchangé**. Clignotement blanc ~1 s, puis **fixe ~3 s**,
puis extinction. Ce n'est donc pas une histoire de timing.

Le détail qui compte : le **voyant fixe signifie que le lien Bluetooth est
établi**. La link key est donc acceptée et l'authentification BT réussit. Le rejet
survient *après*, au niveau supérieur — et il vient de la console, qui coupe.

### L'angle mort

Pendant toute cette séquence, **aucun des 7 hooks ne capte quoi que ce soit** :
ni les exports SceBt, ni les imports vers SceRegMgr. Le dialogue de reconnexion se
déroule intégralement à l'intérieur de SceBt, hors de portée de tout hook posé sur
ses interfaces (cf. §14.2).

Il reste une seule fenêtre d'observation sans reverse d'offsets : **ds34vita
consomme les événements Bluetooth via un callback (`bt_cb_func`)** et journalise
chaque type d'événement (`0x01` inquiry, `0x05` connexion acceptée, `0x09`
connexion sans appairage…).

### 15.3 Firmware 3.65 — les offsets sont valides

Console en **3.65 avec Enso**. L'offset `0x22999C8` date du premier commit de
ds4vita (2016-12-26, donc firmware 3.60) et n'a jamais bougé — ce qui a d'abord
fait craindre une incompatibilité. **C'est faux** : `VitaControl`, activement
maintenu et largement déployé sur les deux firmwares Enso, utilise exactement le
même offset. Et surtout, la preuve est venue du log (voir 15.4) : l'événement
« connexion acceptée » arrive bien, donc le hook qui force `*data |= 0x11000`
fait son travail. SceBt n'a pas changé entre 3.60 et 3.65.

### 15.4 VitaControl testé — la manette décroche toute seule

[VitaControl](https://github.com/Hydr8gon/VitaControl) (Hydr8gon) est le
successeur de ds34vita : réécrit en orienté objet, maintenu, et il déclare le PID
de la v2 en entrée de première classe :

```cpp
DECL_CONTROLLER(0x054C, 0x09CC, DualShock4Controller);
```

Il reproduit le même bug `vid_pid` non initialisé, mais **sans conséquence** :
son `switch` ne matche que des valeurs exactes, donc un résidu de pile ne
déclenche rien. Il n'a par ailleurs qu'un seul hook à offset au lieu de deux.

Testé avec `ds4v2fix` en observateur (hooks sur `ksceBtGetVidPid`,
`ksceBtHidTransfer`, `ksceBtStartConnect`, `ksceBtStartDisconnect`), le cycle est
identique à chaque tentative :

```
GetVidPid B26FBCD3:0000C822 = 054C:05C4     identification OK
HidTransfer type=1 len=78
    -> report 11 C0 20 F3                    report 0x11 « mode étendu » + CRC
HidTransfer type=2 len=53                    requête feature
                                             ← puis plus rien
```

**Deux conclusions majeures :**

1. **Aucun `StartDisconnect` n'est jamais appelé.** Personne ne commande la
   coupure — ni la console, ni le plugin. **C'est la manette qui décroche**,
   après être restée sans réponse. Toute la recherche d'un « rejet » côté console
   portait donc à faux.
2. **La manette ne répond pas au report étendu.** Elle reçoit les 78 octets, la
   requête feature, et n'émet jamais de rapport d'entrée.

Or ds34vita, lui, envoie un report `0x11` **court de 13 octets, sans CRC ni mode
étendu** — et c'est le seul cas de la journée où la manette a réellement
fonctionné (LED rose, navigation dans les menus). L'hypothèse de travail est donc
que **cette v2 accepte le report court et ignore le report étendu**, ce qui
rejoint son comportement en USB face au report `0x81`.

### 15.5 ~~CAUSE RACINE~~ — le feature report `0x06`

> ⚠️ **RÉVISÉ LE 2026-08-03. Le `0x06` n'est pas la cause racine.** La mesure
> reste exacte, sa conclusion non — voir **§15.11**. Le report part *après* que
> le verdict d'échec est rendu, dans un canal HID déjà condamné. La section est
> conservée telle quelle : elle documente un symptôme réel, et l'erreur
> d'interprétation vaut d'être lisible.

Trace obtenue en hookant `ksceBtHidTransfer`, **sans aucun plugin de manette
actif** : c'est donc le pilote natif seul. Le champ `unk09` de `SceBtHidRequest`
porte l'identifiant du report demandé.

| Requête | Report | V1 | V2 |
|---|---|---|---|
| `type=2 len=53` | **`0x06`** | ✅ répond | ❌ **silence** |
| `type=2 len=41` | `0x05` (calibration) | ✅ répond | jamais atteinte |
| `type=0 len=80` | rapports d'entrée | **435** | **0** |
| `type=1 len=76` | `0x11` | écriture | jamais atteinte |

**La V2 ne répond pas au feature report `0x06`.** C'est la première question que
la PS TV pose à la connexion, et tout le reste en dépend : sans réponse, pas de
calibration, pas de passage en mode rapport, aucune entrée. La console attend
~3 secondes puis lâche le lien — d'où le voyant blanc fixe suivi de l'extinction.

C'est le **même trait qu'en USB**, où cette manette ignore le report `0x81`
(§14.3). En USB c'est anodin (seule la lecture de la MAC échoue) ; en Bluetooth
c'est bloquant. Le verdict `clone` de dualshock-tools était une mauvaise
étiquette, mais il pointait un vrai trait matériel — le même.

**Correction impossible sans reverse.** Fabriquer la réponse à la place de la
manette se heurte au caractère **asynchrone** de `ksceBtHidTransfer` : le système
n'attend pas le retour de la fonction mais un événement de complétion émis par
SceBt, qu'un hook sur l'appel ne peut pas produire. Il faudrait descendre dans
SceBt par offsets, sur un firmware sans garantie.

**Contournement connu** : la manette fonctionne après chaque appairage neuf,
jusqu'à extinction — y compris sans redémarrer la console (§16, test 20).

### 15.6 Capture et tentative de réinjection

La réponse au `0x06` n'est donc pas absente en toutes circonstances. Lors d'une
association neuve, la V2 répond bien avec 53 octets :

```text
06 4D 61 79 20 31 37 20 32 30 31 36 00 00 00 00
00 30 36 3A 33 36 3A 32 36 00 00 00 00 00 00 00
00 00 01 04 64 01 00 00 00 08 70 00 02 00 80 03
00 5A D7 EE CA
```

La séquence observée est alors `0x06` → événement `0x0A` → `0x05` → `0x11`,
et la manette fonctionne. Après reconnexion, le `0x06` reste sans événement de
complétion.

Une tentative expérimentale a pré-rempli le tampon avec cette réponse et a
synthétisé l'événement `0x0A` dans le hook `SceBtReadEvent`. Le binaire a été
chargé avec `replay=1`, puis désactivé immédiatement après le redémarrage
(`replay=0`). Le résultat fonctionnel n'a pas encore été validé par une
reconnexion physique ; aucune conclusion de correction ne doit être tirée.

Le test confirme toutefois le point technique important : remplir le tampon ne
suffit pas nécessairement, car la chaîne attend aussi l'événement interne de
SceBt. Toute poursuite devra rester limitée à cette MAC et conserver le filet
de sécurité FTP/VitaCompanion.

---

## 15 bis. Journée du 2026-08-03

### 15.7 EyeToyPSVita — le projet frère

`github.com/Esodland/EyeToyPSVita` (même auteur) contient un plugin kernel qui
fait fonctionner une **PS Move** sur Vita/PS TV via `SceBt` : appairage, flux
HID, capteurs. C'est le même problème résolu sur une autre manette, et il a
fourni le point de départ de la journée.

Ce qui s'y reprend directement :

- **La séquence qui marche**, et qui ne demande aucun rapport feature :
  `0x05` → sortie (`type=1`) → `0x0B` → armer la lecture (`type=0`) → `0x0A` →
  données + réarmement.
- `ksceBtHidTransfer` en `type=2`/`3` **accepte la requête, émet un `0x0C`, et
  n'écrit jamais le tampon** : l'accusé de réception ne vaut pas réponse. Même
  conclusion que §15.5, obtenue par un autre chemin.
- Un **callback appartient au thread qui l'enregistre** : `ksceBtRegisterCallback`
  rend 0 depuis un autre thread, puis chaque lecture échoue en
  `CB_NOT_REGISTERED`. Panne muette qui imite un périphérique absent.
- `ksceBtReadEvent` **ne bloque pas** : succès avec événement vide. Boucle naïve
  = console figée.
- ⛔ **Ne pas neutraliser le contrôle de taille à `SceBt+0x12DA6`** : dépassement
  de tampon noyau sur un chemin partagé par tous les périphériques Bluetooth.

Méthode reprise aussi : remplir un tampon de sonde d'un **témoin** (`0xA5`)
plutôt que de zéros. Un tampon pré-rempli de ce qu'on espère lire ne peut rien
prouver — c'est exactement le défaut du mode `replay` de §15.6.

### 15.8 `ds4v2bt` — mener le dialogue soi-même

Plugin écrit d'après la séquence PS Move : sur `0x05`, émettre le rapport de
sortie `0x11` et armer la lecture, sans jamais poser de feature report.

Il fonctionne — et il ne peut pas gagner. La mesure horodatée l'a montré : le
lien vit **~20 ms** après le `0x05`. Les deux requêtes sont *acceptées*
(retour 0) et n'ont pas le temps d'aboutir. Aucune optimisation ne rattrape ça,
parce que le problème est en amont (§15.11).

Deux enseignements de conception au passage :

- ds34vita reposte une lecture sur `0x0A`, `0x0B` **et** `0x0C`, et le correctif
  du 2026-07-31 en ajoutait une quatrième au `0x05` : plusieurs requêtes en vol
  sur une **seule structure statique partagée**. Sortie et lecture ont chacune
  la leur ; deux lectures, non.
- `SceBtHidRequest` doit être **statique**, jamais sur la pile : SceBt est
  asynchrone et rien ne garantit qu'il la consomme avant le retour de l'appel.

### 15.9 `ds3trace` — le bon instrument

**`SceDs3` est le pilote de manette, pas `SceBt`.** SceBt est la pile Bluetooth
en dessous. L'indice était là depuis le 31 juillet : hooker l'*export*
`ksceBtHidTransfer` captait des appels, or un module n'appelle pas ses propres
exports — l'appelant était donc un autre module.

Sa table d'imports (relevée dans `ds3.elf`) ne contient que **cinq** fonctions
Bluetooth :

```
ksceBtRegisterCallback   ksceBtReadEvent   ksceBtHidTransfer
ksceBtGetRegisteredInfo  ksceBtStartDisconnect
```

Les hooker **côté imports** donne une visibilité complète sur le dialogue
pilote/pile — et **sans un seul offset**, donc portable d'un firmware à l'autre,
contrairement aux hooks en dur de ds3vita/ds4vita/ds34vita. C'est le livrable
réutilisable de la journée : purement observateur, `offsets=0` par défaut.

### 15.10 Où trouver les binaires kernel

Il n'existe **ni `bt.skprx` ni `ds3.skprx`** dans `os0:/kd/` — les modules noyau
sont empaquetés dans **`os0:kd/bootimage.skprx`**. Chaîne complète :

1. **FAGDec** en *mode ELF* sur `os0:kd/bootimage.skprx`
   → `ux0:/FAGDec/kd/bootimage.skprx.elf`
2. **`psp2-kernel-bootimage-extract`** (fourni précompilé par TeamFAPS)
   → **56 modules `.elf`** d'un coup : `bt` (143 ko), `ds3` (55 ko), `hid`,
   `ctrl`, `usbd`…

Segment texte de `bt.elf` : offset fichier `0xA0`, taille `0x21484`, vaddr
`0x81000000`. Idem pour `ds3.elf` avec `0xCD74`.

**Les offsets hérités du 3.60 sont valides sur le 3.65** — dette du projet
soldée, vérifiée au désassemblage :

| Offset | Vérification |
|---|---|
| `0x199C8` | `push {r3,r4,r5,lr}`, début de fonction ; lit bien `[base+0x14a4]` |
| `0x147E4` | `stmdb sp!, {r4-fp,lr}`, début de fonction |
| `0x12DA6` | milieu de fonction, pose l'erreur `0x802F4007` — le contrôle de taille à ne pas toucher |

### 15.11 CAUSE RÉELLE — le verdict tombe avant le pilote

Trace différentielle des deux manettes, même horloge, même fichier :

```
V1   0x08 → +335 ms → SceBt+0x199C8 (accepté, drapeaux 0)
                    → +526 ms → état 3 → 3800 rapports d'entrée

V2   0x08 → [0x199C8 JAMAIS appelée] → 5605 ms → état 4 → mort en 26 ms
```

**Trois faits, tous reproduits :**

1. **L'état 4 n'est pas une phase, c'est un verdict.**
   `ksceBtGetConnectingInfo` (SceBt+0x6BC) ne calcule rien : il lit un mot de
   drapeaux à `dev+4`.

   ```
   6ec:  ldr   r2, [r0, #4]     mot de drapeaux
   6ee:  lsls  r1, r2, #25      teste le bit 6  (0x40)
   6f2:  movmi r4, #4           bit 6 posé      → ÉTAT 4
   700:  lsls  r2, r2, #23      teste le bit 8  (0x100)
   704:  movpl r4, #2           bit 8 absent    → état 2
   710:  movs  r4, #3           bit 8 posé, bit 6 absent → ÉTAT 3
   ```

2. **`SceBt+0x199C8` — la décision d'acceptation, celle que quatre projets
   hookent depuis des années — n'est jamais appelée pour la V2.** Trois essais,
   zéro appel ; un essai V1, un appel à +335 ms avec `drapeaux = 00000000`.
   Ces projets contournaient ce drapeau sans savoir ce qu'il signifiait.

3. **`ksceBtStartDisconnect` n'est jamais appelé**, ni pour l'une ni pour
   l'autre. Confirmation prise cette fois **depuis le pilote** : personne ne
   rejette la manette.

Le `0x06` est donc une victime : il part dans un canal déjà condamné. Les 5,6 s
sont métronomiques (5604, 5605, 5626 ms) — un délai d'attente, pas une lenteur.

⚠️ **Observation qui ne se traduit par rien :** l'utilisateur a vu la LED se
comporter différemment d'un essai à l'autre (clignotement ou non). Le journal
est identique à la milliseconde sur les trois. Cette variation est côté manette
et n'a aucune contrepartie dans la console.

### 15.12 Trois erreurs dans les en-têtes VitaSDK

Chacune se tient seule, indépendamment de ce projet.

| Ce que dit `psp2kern/bt.h` | Ce que la mesure montre |
|---|---|
| `SceBtRegisteredInfo` fait `0x100` (avec `BUILD_ASSERT_EQ`) | **`0x200`** — et `ksceBtGetRegisteredInfo` **ignore** le paramètre de taille : il écrit les offsets 256 à 511 quoi qu'on lui annonce |
| `ksceBtGetRegisteredInfo(int device, …)` | SceDs3 lui passe **`mac0`** en premier argument, pas un index |
| `1 = disconnected?, 2 = connecting?, 5 = connected?` | **3 = connecté et fonctionnel**, **4 = échec**. Le 5 n'apparaît jamais |

La première est un **dépassement de tampon** : un `SceBtRegisteredInfo` en
variable locale fait déborder 256 octets sur le cadre de pile. Le crash ne
survient pas pendant l'appel mais **au retour de la fonction appelante**,
adresse de retour écrasée — signature qui envoie chercher le problème partout
sauf au bon endroit. Trois boot loops.

Piège dans le piège : `ds4v2fix` faisait le même appel avec la même variable
locale sans jamais planter, la disposition de son cadre faisant tomber le
débordement sur de l'inoffensif. **Un code qui marche ne prouve rien ici.**

### 15.14 Conclusion historique — L2CAP ne démarre jamais (corrigée ensuite)

*Cette section remplace la conclusion de §15.11, qui situait encore l'échec trop
haut. Elle est elle-même corrigée par la capture brute du 2026-08-04 : le filtre
historique n'observait que les types 4, 5 et 7, alors que les requêtes L2CAP
entrantes de la V2 emploient le type `0x14`. Conserver le détail ci-dessous
explique cette fausse piste, mais sa conclusion « L2CAP ne démarre jamais »
n'est plus valide. Voir [`docs/BT-STATE-PROBE.md`](docs/BT-STATE-PROBE.md).*

#### La chaîne, telle qu'elle est établie

1. `SceBt+0x199C8` est **l'entrée 1** d'une table de trois gestionnaires en
   `0x8101F858`, installée dans un contexte à `ctx+0x3C` par `SceBt+0x1ACC0`.
2. Le **seul** site qui la déréférence est `SceBt+0x69C6..0x69F8` :

   ```
   69c8:  movs r1, #5           recherche d'un objet de TYPE 5
   69ca:  ldrh r2, [sp, #128]   avec un identifiant 16 bits
   69ce:  bl   0x11544          → NULL ? on abandonne
   69dc:  ldr  r3, [r0, #60]    ctx->table  → NULL ? on abandonne
   69ec:  ldr  r3, [r3, #4]     table[1] = 0x199C8
   69f8:  blx  r3               APPEL(périphérique, contexte)
   ```
3. `SceBt+0x11544` est la **recherche centrale d'objets** de SceBt : 47 sites
   d'appel, types 4, 5, 7, 20, 36. Prologue propre, **trois arguments en
   registres** — ce qui en fait un point d'accroche sûr, contrairement à §15.13.

#### Le décodage du protocole, offert par la V1

| Temps après `0x08` | Événement |
|---|---|
| +285 ms | `type=5 id=0x0041` — premier canal |
| +338 ms | `SceBt+0x199C8` appelée, drapeaux `00000000` |
| +508 ms | **état 3**, la manette fonctionne |
| +539 ms | `type=4 id=0x0011` ↔ `type=5 id=0x0042` — même objet |
| +667 ms | `type=4 id=0x0013` ↔ `type=5 id=0x0043` — même objet |

`0x0011` et `0x0013` sont les **PSM HID du standard Bluetooth** (Control et
Interrupt). D'où la lecture des deux types :

- **type 4 = PSM**, le service demandé ;
- **type 5 = CID**, le canal alloué, dynamique à partir de `0x0040`.

Appariement : CID `0x42` = canal HID Control, CID `0x43` = canal HID Interrupt.

#### Ce que fait effectivement la V2 (correction)

La capture brute montre deux `L2CAP Connection Request` entrants :
`PSM=0x0011/CID=0x0050`, puis **à nouveau** `PSM=0x0011/CID=0x0051`.
`SceBt+0x11544` retourne un contexte distinct et valide pour chaque requête,
et SceBt répond avec les CID `0x40` puis `0x41`, sans erreur. Le témoin V1
réussi ouvre au contraire `0x0001`, `0x0011`, puis `0x0013`, et enchaîne sur
la configuration et les rapports HID. Le lien ACL et le début de L2CAP sont
donc établis ; le verdict `0x2144` survient après environ 5,2 s parce que la
V2 ne poursuit pas cette négociation.

Les 5,6 s métronomiques (5597, 5605, 5626) sont maintenant à attribuer à un
délai d'attente de négociation L2CAP/HID interne, et non plus à une page ACL.
La valeur exacte du compteur reste à identifier.

#### Conséquence, et elle est nette

L'appairage et le registre restent écartés par les mesures précédentes. En
revanche, HID/L2CAP n'est plus hors de cause : ils sont maintenant la couche
pertinente à suivre. Les observations précédentes restent utiles, mais le
point de blocage a été remonté de la liaison ACL vers la négociation L2CAP.

Le défaut est à présent dans la négociation L2CAP/HID de `SceBt`, donc à une
couche qu'un plugin kernel peut corriger. Tracer le SDIO n'est plus la piste
prioritaire : les paquets ACL et les réponses L2CAP sont déjà visibles et
cohérents.

### 15.15 La réassociation — le `0x06` n'a JAMAIS été en cause

Capture obtenue par hasard, et c'est la plus importante des deux jours : une
**réassociation faite pendant que les sondes tournaient**, donc la même manette
qui fonctionne puis qui ne fonctionne plus, dans le même fichier.

```
798541  0x05  connexion acceptée
798541  HidTransfer type=2 report=0x06 len=53
798577  0x0C  ← RÉPONSE, en 36 ms
798577  HidTransfer type=2 report=0x05 len=41   (calibration)
798650  0x0C  ← réponse
798664  HidTransfer type=1 report=0x11 len=76
798697  0x0B  acquittement
806863  0x0A  rapports d'entrée
807584  *** StartDisconnect ***  ← extinction de la manette par l'utilisateur
```

**La V2 répond au feature report `0x06` en 36 ms**, exactement comme la V1
(33 ms). Ce report n'est donc **pas un trait de ce modèle** : §14.3 et §15.5
décrivaient un symptôme de lien mort, pas une caractéristique matérielle.

`StartDisconnect` apparaît ici pour la première fois du projet — le hook
fonctionnait, simplement personne ne raccrochait jamais avant.

#### Les deux procédures ne sont pas les mêmes

```
APPAIRAGE NEUF (marche)        RECONNEXION (échoue)
0x01  résultat de recherche ×4  0x08  connexion demandée par la MANETTE
0x04  demande de link key       5612 ms
0x05  acceptée → tout suit      0x05  acceptée mais dégradée → mort
```

### 15.16 Résultat négatif — on ne peut pas faire initier la console

*Hypothèse tirée de §15.15 : si le chemin « la console initie » fonctionne,
forcer `ksceBtStartConnect` devrait débloquer. Testée en trois cycles avec le
plugin `ds4v2reco`. **Fausse.***

| Situation | Retour de `ksceBtStartConnect` |
|---|---|
| V2, sur l'événement `0x08` | `0x802F0203` |
| V2, 500 ms après l'échec natif | `0x802F0203` |
| V2, à froid, manette **éteinte** | `0x802F0203` |
| **V1 — celle qui fonctionne — à froid** | **`0x802F0203`** |

`0x802F0203` = `CONNECT_START_NOT_CONNECTABLE`. **Le témoin V1 est décisif : ce
code ne dit rien de la V2.**

Explication : une DS4 hors mode appairage **page l'hôte**, elle ne se met pas en
écoute. Elle n'est donc jamais « connectable », et la console ne peut
structurellement pas l'appeler. Ce qui rend le mode appairage différent n'est
pas le sens de la connexion mais le fait que **SHARE + PS rend la manette
connectable et découvrable** — la lecture de §15.15 était fausse sur ce point.

Relevé utile au passage : après chaque échec, **la manette repage toutes les
~80 ms**. Aucune fenêtre calme n'existe une fois qu'elle est allumée, ce qui
condamnait aussi les modes fondés sur un délai.

**Ne pas réessayer cette branche.** Toute approche supposant que la console peut
initier une connexion vers une DS4 hors appairage est vaine.

#### Une observation non expliquée

L'utilisateur a vu la manette **changer de couleur et vibrer** pendant ces
essais — ce qui ne peut venir que d'un rapport de sortie `0x11` reçu. Or aucun
acquittement d'écriture (`0x0B`) n'apparaît dans le journal correspondant. Les
deux faits ne se recouvrent pas, et **c'est consigné comme tel plutôt que
d'inventer une cause**.

### 15.17 Une reconnexion réussie, et trois explications qui tombent

Console **vanilla** — redémarrée, aucun plugin chargé. Après un appairage neuf
de la V2 avec la V1 éteinte, **la V2 a survécu à un cycle extinction/rallumage**.
C'est la première et la seule reconnexion réussie de tout le projet. Elle
contredit le plafond posé en §15.14, et elle n'a jamais été reproduite.

Trois explications ont été tentées dans la foulée, chacune tuée par le test
suivant :

| Hypothèse | Test | Verdict |
|---|---|---|
| Une 2ᵉ fiche manette casse la V2 | dump registre **post-reboot** | **fausse** — la fiche V1 est toujours en `0x6600`, la suppression UI n'a jamais eu lieu |
| C'est la connexion **active** d'une autre manette | V1 éteinte mais enregistrée | **fausse** — la V2 meurt quand même |
| C'est la **fraîcheur de la link key** | key neuve `d6f85d` + pile BT fraîche | **fausse** — ne se reconnecte pas |

#### Le piège méthodologique, et il a coûté la session

`SceRegMgr` garde ses écritures en RAM. **Un dump FTP pris console allumée est
périmé.** Pire : une suppression faite depuis Paramètres → Périphériques
Bluetooth **n'a jamais atteint le registre**, et l'UI a pourtant affiché une
suppression réussie. Toutes les conclusions tirées d'une table qu'on croyait
vidée étaient donc bâties sur du sable.

**Règle** : redémarrer la console avant chaque dump, et ne jamais croire l'UI de
suppression sans vérification au registre.

#### Suivi des link keys de la V2 (`0x6e00 + 0x70`)

| État | key | reconnexion |
|---|---|---|
| début de session | `adb606…` | non |
| appairage neuf, V1 éteinte | `648902…` | **oui** |
| après réappairage V1 | `648902…` inchangée | non |
| dernier essai | `d6f85d…` | non |

#### Deux faits neufs à expliquer

1. **Un appairage a écrit une link key sans ouvrir de session fonctionnelle.**
   Jusque-là, appairage réussi impliquait toujours session qui marche. Les deux
   étapes se sont dissociées.
2. En fin de session, **ni la V1 ni la V2** ne se réappairent par SHARE + PS.
   Repli : resynchronisation par USB.

#### Piste ouverte, jamais testée

La table contient une **entrée parasite** en `0x7600` : MAC
`F8:6B:14:C3:62:24`, VID:PID `0000:0000`, **link key entièrement à zéro**, nom
vide. Un enregistrement malformé qu'un parcours de table pourrait mal digérer.
La supprimer et rejouer l'appairage n'a jamais été fait.

#### Récupération sans manette

VitaCompanion reste joignable : FTP `172.20.10.2:1337`, serveur de commandes
`1338`. Un `printf "reboot\n"` sur le 1338 répond `Rebooting...` et redémarre la
console — ce qui flushe le registre au passage.

### 15.18 LA RÈGLE — un quota de reconnexions par appairage

Capture `ds3trace` en mode léger (`offsets=0`, hooks sur les imports
`SceDs3 → SceBtForDriver` uniquement), console vanilla, `ds34vita` absent.
Journal : `logs/ds3trace-postboot-06.txt`.

#### La première reconnexion V2 réussie du projet, horodatée

```
294981  StartDisconnect V2        extinction par l'utilisateur
295207  0x06 ×2                   deconnectee
296804  0x08                      la V2 rappelle           (+1597 ms)
297201  0x05  ACCEPTEE            +397 ms   ← et non 5,6 s
297201  report 0x06 len=53  ->  0x00000000
297219  etat 2 -> 3               apres 401 ms   ← ETAT 3 = FONCTIONNEL
297260  0x0C                      la V2 repond en 59 ms
297341  report 0x11  ->  0x0B acquitte
        ... flux d'entree continu, 23 200 rapports
```

Comportement d'une manette parfaitement supportée : 401 ms pour atteindre
l'état 3, contre 489 ms pour la V1. Le pilote natif `SceDs3` la gère sans
plugin.

#### Tous les cycles mesurés

| Après appairage | `0x06`→`0x08` | `0x08`→`0x05` | état | issue |
|---|---|---|---|---|
| **appairage `0x04` à 73547** | | | | |
| reco 1 | 1597 ms | **397 ms** | 2 → **3** | ✓ |
| reco 2 | 2089 ms | 5415 ms | 2 → 4 | ✗ |
| reco 3 | 2808 ms | 5614 ms | 2 → 4 | ✗ |
| reco 4 | 2798 ms | 5625 ms | 2 → 4 | ✗ |
| reco 5 | 2804 ms | 5400 ms | 2 → 4 | ✗ |
| reco 6 | 2789 ms | 5399 ms | 2 → 4 | ✗ |
| reco 7 | 1781 ms | 5390 ms | 2 → 4 | ✗ |
| **appairage `0x04` à 759934** | | | | |
| reco 1 | 8670 ms | **404 ms** | 2 → **3** | ✓ |
| reco 2 | **11429 ms** | **410 ms** | 2 → **3** | ✓ |
| reco 3 | 9916 ms | **371 ms** | 2 → **3** | ✓ |
| reco 4 | **11223 ms** | 5646 ms | 2 → 4 | ✗ |
| reco 5 | 7186 ms | 5447 ms | 2 → 4 | ✗ |
| reco 6 | 5830 ms | 5577 ms | 2 → 4 | ✗ |

#### Ce que ces chiffres établissent

**1. Le délai d'extinction n'est PAS la variable.** Reco 2 du second appairage
réussit à `11429 ms`, reco 4 échoue à `11223 ms` — 206 ms d'écart, issues
opposées. Les succès s'étalent de 1597 à 11429 ms, les échecs de 1781 à
11223 ms. Les deux distributions se recouvrent entièrement.

**2. La V1 n'est PAS la cause.** Elle est supprimée et éteinte depuis 81364, et
n'apparaît plus une seule fois dans la trace. Les six échecs du second appairage
se produisent sans elle. L'hypothèse « une deuxième fiche manette bloque la V2 »
est morte.

**3. Les deux manettes peuvent coexister.** À 74730, rapports `0x11` envoyés aux
deux, `0x0B` acquittés par les deux, simultanément. La coexistence n'est pas le
problème.

**4. La règle réelle :**

> Après un appairage neuf, la V2 dispose d'un **quota** de reconnexions —
> **1**, puis **3**, puis à nouveau **3** dans une réplication indépendante —
> après quoi l'échec devient
> **permanent et déterministe**. Seul un appairage neuf recharge le quota.

Le quota n'est pas constant : c'est donc une ressource qui se consomme ou se
dégrade, pas un compteur fixe.

#### Réplication contrôlée après les captures HCI — quota 3 confirmé

Le 2026-08-04, après les quatre captures HCI Windows, la V2 a été appairée à
neuf avec la PS TV. La V1 a servi uniquement à naviguer dans l'UI d'appairage,
puis a été éteinte avant les cycles V2. `ds3trace` était en mode passif
(`offsets=0 clear6=0`, 559 lignes, sous le plafond de 3000).

| Reconnexion | `0x08` → `0x05` | État final | Après `0x08` | `0x0C` / entrées | Issue |
|---|---:|---|---:|---|---|
| 1 | 413 ms | 2 → **3** | 432 ms | oui / oui | ✓ |
| 2 | 403 ms | 2 → **3** | 410 ms | oui / oui | ✓ |
| 3 | 373 ms | 2 → **3** | 389 ms | oui / oui | ✓ |
| 4 | 5661 ms | 2 → **4** | 5669 ms | non / non | ✗ |
| 5 | 5646 ms | 2 → **4** | 5656 ms | non / non | ✗ |

Les échecs 4 et 5 reproduisent donc exactement la signature A : le `0x05` finit
par arriver, le pilote poste le feature report `0x06`, mais aucune réponse
`0x0C` ni aucun rapport d'entrée ne suit ; le lien est déjà condamné par l'état
4. Une comparaison des dumps juste avant le cycle 1 et après le cycle 5 donne
**0 octet de différence** dans la fiche V2 `0x6e00`, link key incluse. Le
registre ne consomme donc pas ce quota.

La sonde a ensuite été retirée de `config.txt` et la PS TV redémarrée : retour à
la configuration vanilla.

#### Les trois signatures d'échec, désormais distinctes

| Signature | Description | Contexte observé |
|---|---|---|
| **A — quota épuisé** | `0x08` → 5,4-5,6 s état 2 → **état 4** → `0x06` | dominante, 15 occurrences |
| **B — éjection à chaud** | `0x05` accepté, `0x06` envoyé OK, puis `0x06` **30 ms** après | au boot (3782), V1 rechargée depuis le registre |
| **C — succès** | `0x08` → `0x05` en ~400 ms → **état 3** | 7 occurrences |

La signature B reste inexpliquée et n'a été vue qu'une fois. C'est la seule
piste où la présence de la V1 pourrait encore jouer.

#### Deux mesures négatives propres

- **Une reconnexion n'écrit rien dans le registre.** Dumps encadrant plusieurs
  tentatives dont des échecs : **0 octet** de différence sur toute la table.
  L'hypothèse « la console fait tourner la link key à chaque reconnexion » est
  falsifiée.
- **La suppression depuis l'UI n'atteint jamais le registre.** Confirmée deux
  fois par dump **post-reboot**, donc après flush. Elle n'agit qu'en RAM, et
  chaque redémarrage réinjecte la fiche.

#### Résultat négatif — `btpurge` ne modifie pas la fiche persistée

Test du **2026-08-04**, console vanilla sauf une sonde à la fois. Un instantané
de `system.dreg` pris avant l'essai a confirmé l'entrée parasite à `0x7600` :
MAC `F8:6B:14:C3:62:24`, VID:PID `0000:0000`, nom vide, link key entièrement
nulle. `btpurge` a été armé pour un seul démarrage, a attendu 20 s, puis a appelé
`ksceBtDeleteRegisteredInfo(14C36224, 0000F86B)` : retour **`0x00000000`**.

Après retrait du plugin, second redémarrage pour flusher `SceRegMgr`, et nouveau
dump : **l'entrée est toujours présente, octet pour octet identique** à `0x7600`.
Le registre contient bien quelques différences ailleurs (compteurs/horodatages
de boot), mais pas une seule dans la fiche ciblée.

Le succès de cette API ne signifie donc pas ici « suppression persistée » ; elle
peut au mieux vider un état transitoire que le démarrage réhydrate. **Ne pas
rejouer cette expérience** : elle ne recharge pas le quota et l'hypothèse de la
fiche parasite n'a plus de contournement connu par cette API.

La capture HCI comparative Windows est désormais faite (test 72) et confirme
que la V2 se reconnecte normalement sur un autre hôte. La suite utile redevient
donc l'observation externe sur **un autre exemplaire de DS4 V2**, avant toute
instrumentation SDIO.

### 15.13 Ce qui ne se hooke pas

Quatre plantages en tentant d'instrumenter les sites qui posent le bit 6.

`0x0DDA`, `0x1D50`, `0x1E9C`, `0x26A8` : trois essais, trois plantages à la
connexion, **sans qu'aucune sonde ne journalise son entrée** — y compris après
avoir déplacé la journalisation *avant* l'appel et supprimé toute lecture
mémoire. Le crash ne vient donc pas du corps de la sonde mais du branchement
lui-même.

`0x6558` : plante différemment — elle journalise son entrée, puis disparaît dans
`TAI_CONTINUE`. Arité supérieure à quatre, ce que son prologue trahissait
(`sub sp,#212`, `cmp r2,#3`).

**Leçon :** brancher une fonction C sur du code interne dont on ignore l'arité
et le contexte d'appel n'est pas fiable. Les seuls points d'accroche raisonnables
sont ceux dont un autre projet a prouvé l'usage, ou dont le désassemblage donne
l'arité sans ambiguïté. `0x199C8` remplit les deux conditions et se hooke
proprement.

⚠️ `0x199C8` n'est appelée **directement nulle part** dans SceBt : c'est un
gestionnaire atteint par appel indirect, donc enregistré dans une table de
dispatch. Remonter à ce qui la déclenche demande de cartographier cette table.

---

### Hypothèses éliminées, définitivement

| Hypothèse | Pourquoi elle tombe |
|---|---|
| Liste blanche de PID `09CC` | La PS TV enregistre déjà la V2 sous `05C4` — elle normalise le PID toute seule. Le spoof envisagé était déjà fait par le système. |
| La MAC est effacée au redémarrage | Elle n'est pas effacée : l'enregistrement est présent, avec link key fraîche et compteur incrémenté. |
| L'appairage échoue | Il réussit, et il est persisté (5 écritures de `/CONFIG/BT/01/info`). |
| Manette non authentique | Achat en circuit officiel, et `0x81` est un critère non fiable. |
| Fenêtre de timing au démarrage | Testé : démarrage complet + 30 s d'attente, comportement inchangé. |
| Offsets incompatibles 3.65 | VitaControl utilise les mêmes et l'événement « connexion acceptée » arrive bien. |
| Identification impossible | `ksceBtGetVidPid` **réussit** et renvoie `054C:05C4`, mesuré six fois. |
| La console rejette la manette | **Aucun `StartDisconnect` n'est jamais émis** : c'est la manette qui décroche. Reconfirmé le 3 août **depuis le pilote** (§15.11). |

*Ajouts du 2026-08-03 :*

| Hypothèse | Pourquoi elle tombe |
|---|---|
| Le feature report `0x06` est la cause | Il part **après** le verdict, dans un canal déjà condamné. Le lien meurt 26 ms plus tard — moins que les 33 ms que met la V1 à répondre (§15.11). |
| Entrée d'appairage bancale, SDP sauté | Le descripteur HID est lu, complet, **430 octets** (§15.10, test 38). Le piège d'EyeToyPSVita ne s'applique pas ici. |
| Il faut réappairer la manette | Non : l'entrée registre est saine et identique à celle de la V1 (test 37). |
| Un plugin peut mener le dialogue à la place du natif | Il le peut, mais le lien ne vit que **~20 ms** après le `0x05` : les requêtes sont acceptées et n'aboutissent jamais (§15.8). |
| `SceBt+0x199C8` est sur le chemin de la V2 | **Jamais appelée** en trois essais, alors qu'elle l'est pour la V1 à +335 ms (§15.11). |
| La variation du clignotement de la LED signifie quelque chose | Trois essais aux comportements de LED différents produisent un journal **identique à la milliseconde**. C'est côté manette. |
| Une mise à jour firmware de la manette réglerait le `0x06` | **Il n'existe aucune procédure de mise à jour pour une DS4.** Aucun menu sur PS4, aucun updater chez Sony — qui n'en publie que pour la DualSense. Seul le mode DFU reste, sans firmware correctif connu. Voir « Prochaines étapes ». |
| Le problème est dans le HID, le pilote, l'appairage ou le registre | L'appairage et le registre restent écartés ; la capture brute prouve que L2CAP est atteint. Le prochain point est le gestionnaire HID/L2CAP interne (mise à jour de §15.14). |

### Prochaines étapes

*État au 2026-08-04. Cette liste a été révisée trois fois ; les
pistes closes sont conservées en bas avec leur motif — c'est ce qui évite de les
rouvrir.*

#### Le point de blocage, en une phrase

La connexion **entrante** atteint la négociation L2CAP pour la V2 mais n'aboutit
pas, sur la même console. Le blocage se situe maintenant dans la suite du
gestionnaire L2CAP/HID interne de `SceBt`, avant les rapports de `SceDs3`.

#### 1. Une autre DS4 v2 — gratuit, et c'est le plus important

Le seul test qui tranche entre **« cet exemplaire »** et **« tous les v2 »**.
Ces deux réponses mènent à des projets opposés : dans le premier cas il n'y a
rien à corriger dans la console et tout développement est vain ; dans le second
il y a un correctif qui servirait à d'autres.

Un signe qu'on n'est peut-être pas seul :
[MiniVitaTV #17](https://github.com/TheOfficialFloW/MiniVitaTV/issues/17) — un
autre utilisateur, une CUH-ZCT2U, sur PS TV, qui demande *« Is it specific to
the CUH-ZCT2U version? »*. Sans réponse, et avec un symptôme différent (chez lui
le premier appairage échoue). Trop mince pour conclure, assez pour chercher.

**Tant que ce test n'est pas fait, tout le reste se développe à l'aveugle.**

#### 2. Capture HCI comparative sous Windows — réalisée, aucune divergence bloquante

Les quatre fenêtres V1/V2 (appairage neuf puis reconnexion PS seul) ont été
capturées avec WPR, décodées par `BTETLParse` et vérifiées dans Wireshark (test
72 ; rapport complet : [`docs/HCI-WINDOWS-COMPARISON.md`](docs/HCI-WINDOWS-COMPARISON.md)).
Zéro événement ETW n'a été perdu.

Dans les deux cas, l'appairage suit Secure Simple Pairing et se termine par une
link key puis `Encryption Change = success`. À la reconnexion, la V1 comme la V2
reçoivent `Connect Request`, réutilisent la link key, activent le chiffrement et
ouvrent avec succès les PSM HID Control (`0x0011`) et Interrupt (`0x0013`) en
moins de 0,71 s. Aucune erreur HCI, L2CAP ou branche propre à la V2 n'apparaît.

La capture ne permet pas de comparer les versions/fonctionnalités LMP : Windows
ne les a redemandées que pour la V1 dans cette fenêtre, puis les a mises en cache.
Elle ne prouve donc pas l'identité complète des deux contrôleurs, mais elle
écarte un défaut général d'appairage/reconnexion Bluetooth Classic de cet
exemplaire V2. C'est un résultat PC ; il ne remplace pas la trace de la pile PS
TV, où le quota de reconnexions reste le phénomène à expliquer.

⚠️ Une application ordinaire ne suffit toujours pas pour la PS TV : elle ne
verrait que les rapports HID, soit une couche au-dessus de celle qui échoue.

#### 3. Tracer le SDIO — coûteux, incertain, en dernier

`SceBt` importe 7 fonctions de `SceSdifForDriver` : c'est le transport réel vers
la puce WLAN/BT. Les hooker donnerait le trafic HCI brut de la PS TV, donc la
contrepartie exacte du point 2.

Mais c'est du transactionnel sans documentation, très volumineux, à décoder
depuis zéro. À ne tenter **que si le point 2 a identifié quoi y chercher** —
sinon c'est chercher une aiguille sans savoir à quoi elle ressemble.

#### 4. Publier ce qui ne dépend de rien

`ds3trace` est portable, purement observateur, `offsets=0` par défaut, et il
produit un diagnostic exploitable pour n'importe quelle manette récalcitrante.
Avec §15.9 à §15.16, c'est utilisable par d'autres **quel que soit le sort de
cette manette**.

Les trois écarts relevés dans les en-têtes VitaSDK (§15.12) tiennent aussi seuls,
mais **doivent être rejoués par un humain avant toute soumission** — ils sortent
d'une session d'instrumentation, pas d'une revue.

---

#### Pistes closes, avec leur motif

| Piste | Pourquoi elle est fermée |
|---|---|
| Mener le dialogue HID soi-même | Le lien vit ~20 ms après le `0x05` : les requêtes sont acceptées et n'aboutissent jamais (§15.8) |
| Corriger le feature report `0x06` | La manette y répond en 36 ms quand le lien est sain (§15.15) |
| Réappairer, nettoyer le registre | Le descripteur HID est lu, complet, et les entrées V1/V2 sont identiques (tests 37-38) |
| Forcer la console à initier la connexion | `NOT_CONNECTABLE` pour **les deux** manettes : une DS4 hors appairage n'est jamais connectable (§15.16) |
| Purger la fiche parasite par `ksceBtDeleteRegisteredInfo` | Retour `0`, mais fiche `0x7600` persistante et inchangée après redémarrage (test 71) |
| Hooks sur les sites posant le bit 6 | Quatre plantages ; seules les fonctions d'arité prouvée se hookent (§15.13) |
| Mise à jour du firmware de la manette | **Aucune procédure n'existe pour une DS4.** Vérifié deux fois : rien dans les menus d'une vraie PS4, et Sony ne publie d'updater que pour la DualSense. Les pages web décrivant un chemin « Manettes → Mettre à jour » le confondent avec la mise à jour de la console. Le mode DFU existe (`054C:0856`) mais aucun firmware correctif n'est connu, et la [partie 6 d'Al's blog](https://blog.the.al/2023/07/13/ds4-reverse-engineering-part-6.html) raconte une manette briquée. |
| La variation du clignotement de la LED | Trois essais aux comportements visuels différents produisent un journal identique à la milliseconde |

#### Observation en suspens

La manette **change de couleur et vibre** pendant certaines tentatives, alors
qu'aucun acquittement d'écriture (`0x0B`) n'apparaît au journal correspondant
(§15.16). Non expliqué, et laissé tel quel plutôt qu'habillé d'une cause.


## 16. Journal des tests (2026-07-31)

Tous les tests menés, dans l'ordre. « Console » = PS TV 3.65 Enso, FTP
`172.20.10.2:1337`.

| # | Test | Résultat | Ce qu'on en tire |
|---|---|---|---|
| 1 | Compilation de ds34vita avec VitaSDK 2026 | 3 correctifs nécessaires (cmake 2.8, `add_dependencies`, casts `uintptr_t`) | Toolchain validée, `.skprx` produit |
| 2 | Installation de VitaCompanion | OK, FTP + commandes dès le boot | Filet de sécurité pour la suite |
| 3 | Inventaire `ur0:/tai/` | ds34vita présent mais **absent de `config.txt`** | Il n'avait jamais été chargé lors des symptômes initiaux |
| 4 | `ds4v2fix` v1 — hooks sur les **exports** SceBt | **0 capture** pendant une reconnexion | Un export ne voit pas le code interne du module (§14.2) |
| 5 | `ds4v2fix` v2 — hooks sur les **imports** SceBt→RegMgr, log par I/O fichier | **BOOT LOOP** | Jamais d'I/O dans un hook : `ksceIoOpen` reboucle sur le registre |
| 6 | Récupération : polling FTP toutes les 2 s | Fenêtre attrapée à la 4ᵉ tentative (~8 s) | `config.txt` réécrit sans la ligne fautive, console sauvée |
| 7 | `ds4v2fix` v3 — log en RAM, hooks posés à +30 s | Stable, sonde OK | Le démarrage reste sain quoi qu'il arrive |
| 8 | Sonde `ksceBtGetRegisteredInfo` | Ne liste que l'enceinte Bluetooth | Cette API **ne montre pas les manettes** — piège d'interprétation |
| 9 | V1 témoin : extinction/rallumage, hooks RegMgr actifs | **Rien capturé** | Une reconnexion **ne réécrit pas** le registre ; seul un appairage neuf le fait |
| 10 | Lecture directe de `vd0:/registry/system.dreg` | Table d'appairage décodée | Emplacement + format des entrées (§14.1) |
| 11 | Appairage de la V2, hooks RegMgr actifs | `REG SET BIN /CONFIG/BT/01/info` ×9 | L'appairage **est** écrit et persisté |
| 12 | Diff binaire du registre avant/après | 24 octets, tous dans l'entrée V2 : link key régénérée, compteur +1 | L'appairage réussit vraiment |
| 13 | dualshock-tools sur la **V2** (USB) | `Device Type: clone`, report `0x81` échoue | Trait matériel réel, mais mauvaise étiquette |
| 14 | dualshock-tools sur la **V1** (USB) | `original`, MAC lue | **L'instrument fonctionne** — l'échec de la V2 est réel |
| 15 | Recherche : fiabilité du critère `0x81` | Faux positif documenté (patch noyau Linux, DS4Windows utilise `0x12`) | La piste « contrefaçon » est fermée |
| 16 | Diff des entrées registre V1 vs V2 | Classe BT, VID:PID, flags, nom **identiques** | Le registre n'explique rien |
| 17 | Fenêtre de démarrage : boot complet + 30 s d'attente | Comportement inchangé | Ce n'est pas un problème de timing |
| 18 | ds34vita compilé avec `LOG_DISC` | `CONNECTED DS4` mais comportement erratique (rose / blanc / bleu+vibration) | Découverte du bug `vid_pid` (§15.2) |
| 19 | ds34vita corrigé (`vid_pid` initialisé) | **6 connexions sur 6**, identification constante | Le correctif fonctionne ; le tirage aléatoire a disparu |
| 20 | **Extinction de la manette sans redémarrer la console** | Même échec | **Le redémarrage n'a jamais été le problème** : seule la session d'appairage fonctionne |
| 21 | VitaControl (successeur de ds34vita) | Identifie la V2, crée le contrôleur, mais aucune entrée | Le plugin n'est pas en cause |
| 22 | Hooks cycle de vie : `StartConnect` / `StartDisconnect` / `HidTransfer` | **`StartDisconnect` = 0** sur toute la journée | **Personne ne rejette la manette** — elle décroche seule |
| 23 | Comparaison du trafic HID V1 vs V2 | V1 : 1721 lectures — V2 : **0** | La V2 n'émet aucun rapport d'entrée |
| 24 | Correctif : amorçage de la boucle de lecture dans le `case 0x05` | 1 lecture postée par tentative, **aucune réponse** | L'amorce manquait bien, mais ne suffit pas |
| 25 | Différentiel V1/V2 **sans aucun plugin de manette** | V1 : `0x06` → `0x05` → 435 rapports · V2 : `0x06` → rien | Le blocage est **natif**, pas dû aux plugins |
| 26 | Journalisation de `unk09` (identifiant du report) | V2 muette sur le feature report **`0x06`** après reconnexion | **CAUSE RACINE** (§15.5) |
| 27 | Hook `SceBtReadEvent` + capture du tampon après association neuve | Réponse complète de 53 octets ; séquence `0x06` → `0x05` → `0x11` | La V2 sait répondre au `0x06` dans la session d'association (§15.6) |
| 28 | Mode `replay=1` : tampon pré-rempli + événement `0x0A` synthétique | Déployé après redémarrage, résultat fonctionnel non validé | Expérience réversible ; le tampon seul ne prouve pas la correction |
| 29 | Désactivation immédiate du replay | `replay=0`, redémarrage propre, FTP disponible | Configuration sûre restaurée |

---

## 16 bis. Journal des tests (2026-08-03 au 2026-08-05)

Console PS TV 3.65 Enso, `172.20.10.2` — FTP 1337, commandes vitacompanion 1338
(la commande `reboot` en texte brut sur un socket TCP suffit, ce qui a permis de
piloter tous les cycles depuis le PC sans manipulation physique).

| # | Test | Résultat | Ce qu'on en tire |
|---|---|---|---|
| 30 | Clonage et lecture d'EyeToyPSVita | séquence HID sans feature report, pièges `SceBt` | Point de départ de la journée (§15.7) |
| 31 | `ds4v2bt` v0.1 — dialogue mené par le plugin | **BOOT LOOP**, trois bannières et pas une ligne du thread | Le journal ne pouvait rien dire de sa propre mort |
| 32 | Récupération par polling FTP | attrapée à la **1ʳᵉ** tentative | Console saine en quelques secondes |
| 33 | v0.2 — armement à usage unique + journal vidé avant chaque appel | crans 1 à 4 franchis | **Un boot loop devient impossible** (§13, règle 4) |
| 34 | Bisection `step=` : GetConfiguration, CreateCallback, RegisterCallback | tous OK | Mon suspect principal était faux |
| 35 | Cran 5 — inventaire des appairages | **CRASH au retour de la fonction** | Signature d'une pile écrasée |
| 36 | Tampon statique surdimensionné + témoin `0xA5` | `ksceBtGetRegisteredInfo` écrit les offsets **256 à 511** | **Cause des trois boot loops** (§15.12) |
| 37 | Registre `system.dreg` rapatrié et redécodé | V1 et V2 présentes, entrées **identiques**, PID normalisé `05C4` | L'appairage n'explique rien |
| 38 | Sonde du descripteur HID au `0x05` | `0x1AE` = **430 octets, valides** | Le piège EyeToy (SDP sauté) est écarté : pas besoin de réappairer |
| 39 | Horodatage des journaux | `0x05` → `0x06` en **~20 ms** ; chaque `TRACE` coûte **25 ms** | L'instrumentation étouffait la réactivité qu'elle mesurait (§13, règle 6) |
| 40 | Émission avant journalisation | sortie et lecture **acceptées** (retour 0), zéro `0x0A` | Le lien est mort avant de rien porter (§15.8) |
| 41 | `bootimage.skprx` décrypté puis extrait | **56 modules kernel** | `bt.elf`, `ds3.elf` (§15.10) |
| 42 | Vérification des offsets 3.60 sur 3.65 | `0x199C8`, `0x147E4`, `0x12DA6` **valides** | Dette du projet soldée |
| 43 | Table d'imports de `ds3.elf` | SceDs3 n'utilise que **5** fonctions SceBt | Surface d'instrumentation minuscule (§15.9) |
| 44 | `ds3trace` — hooks sur les imports de SceDs3 | différentiel lisible du premier coup | **Le `0x06` n'est pas la cause** (§15.11) |
| 45 | Désassemblage de `ksceBtGetConnectingInfo` | **état 4 = bit 6 de `dev+4`** | L'état 4 est un verdict, pas une phase |
| 46 | Sonde d'état à 20 ms | V2 : 5606 ms en état 2 → **4** · V1 : 532 ms → **3** | Le SDK se trompe sur le sens des états |
| 47 | Sondes sur les 5 sites posant le bit 6 | **crash** à la connexion, `0x6558` seule journalise | Arité > 4 |
| 48 | 4 sondes, journal avant l'appel, sans lecture mémoire | **crash**, aucune sonde n'entre | Le branchement lui-même est en cause (§15.13) |
| 49 | Sonde unique sur `0x199C8` | **aucun plantage**, 3 essais V2 complets | Le seul point d'accroche fiable de SceBt |
| 50 | Même sonde, manette V1 | **appelée à +335 ms**, drapeaux `00000000` | `0x199C8` n'est **jamais** atteinte par la V2 |
| 51 | Remontée statique : qui appelle `0x199C8` | table de 3 gestionnaires en `0x8101F858`, un seul site de dispatch | Le chemin dépend d'une recherche `type=5` (§15.14) |
| 52 | Sonde sur `SceBt+0x11544`, filtre `type=5` | V1 : CID `0x41` trouvé à +271 ms · V2 : **aucune recherche** | La recherche n'échoue pas, elle n'est pas tentée |
| 53 | Filtre élargi aux types 4 et 7 | V1 : PSM `0x0011` et `0x0013` ↔ CID `0x42` et `0x43` · V2 : rien dans ce filtre | Conclusion ensuite corrigée : le chemin entrant V2 utilise le type `0x14` (§15.14, BT-STATE-PROBE) |
| 54 | **Réassociation captée avec les sondes actives** | La V2 répond au `0x06` en **36 ms**, séquence complète, flux d'entrée | **Le `0x06` n'a jamais été un trait de cette manette** (§15.15) |
| 55 | V2 appairée à un PC Windows, éteinte, rallumée | **Se reconnecte sans problème** | Sa couche liaison est saine ; le défaut est propre à la PS TV |
| 56 | `ds4v2reco` mode 1 — `StartConnect` sur `0x08` | `0x802F0203` ×3 | `NOT_CONNECTABLE` — tentative entrante en vol, pensait-on |
| 57 | Mode 4 — `StartConnect` après l'échec natif | `0x802F0203` ; la manette **repage toutes les ~80 ms** | Aucune fenêtre calme n'existe |
| 58 | Mode 5 — `StartConnect` à froid, puis **témoin sur la V1** | **`0x802F0203` pour les deux manettes** | Le code ne dit rien de la V2 : branche close (§15.16) |
| 59 | Console vanilla, appairage neuf V2, V1 **éteinte** | **La V2 survit à un cycle extinction/rallumage** | **Première reconnexion réussie du projet** (§15.17) |
| 60 | Réappairage de la V1, puis test V2 | La V2 remeurt | Une autre manette dans l'équation casse le succès |
| 61 | V1 **éteinte** mais enregistrée, test V2 | La V2 meurt quand même | Ce n'est pas la connexion active |
| 62 | Dump registre **post-reboot** (flushé) | La fiche V1 est **toujours en `0x6600`** | La suppression via l'UI n'a jamais atteint le registre — hypothèse « table » morte (§15.17) |
| 63 | V2 avec key neuve `d6f85d` + pile BT fraîche | Ne se reconnecte pas | **La fraîcheur de la link key ne suffit pas** : hypothèse falsifiée |
| 64 | SHARE + PS sur la V1 **et** la V2 | Aucune ne se réappaire | Sortie de session dégradée, repli USB |
| 65 | Deux dumps encadrant plusieurs reconnexions dont des échecs | **0 octet** de différence | Une reconnexion n'écrit rien : la link key ne tourne pas |
| 66 | `ds3trace` mode léger (`offsets=0`) rechargé, console vanilla | Aucun plantage, hooks posés sur les imports `SceDs3` | La configuration d'instrumentation sûre |
| 67 | **Reconnexion V2 capturée** | `0x08` → `0x05` en **397 ms**, état 2 → **3** | **Première reconnexion V2 réussie du projet** (§15.18) |
| 68 | 13 cycles extinction/rallumage tracés sur deux appairages | Succès de 1597 à 11429 ms, échecs de 1781 à 11223 ms | **Le délai n'est pas la variable** — distributions superposées |
| 69 | Idem, V1 supprimée et éteinte tout du long | 6 échecs consécutifs sans elle | **La V1 n'est pas la cause** |
| 70 | Comptage des succès par appairage | **1** puis **3** avant échec permanent | **Quota de reconnexions par appairage** (§15.18) |
| 71 | `btpurge` armé une fois sur la fiche parasite `F8:6B:14:C3:62:24` | `ksceBtDeleteRegisteredInfo` → **0**, mais entrée `0x7600` inchangée après reboot + flush | L'API ne supprime pas cette fiche de façon persistée ; branche close (§15.18) |
| 72 | Capture HCI Windows : V1/V2, appairage neuf + reconnexion | SSP, link key, chiffrement et PSM HID `0x0011`/`0x0013` réussissent pour les deux ; 0 événement perdu | Aucun défaut Bluetooth Classic général de la V2 ; la divergence est spécifique à la PS TV |
| 73 | Réplication PS TV : V2 réappairée, V1 éteinte, 5 cycles tracés | **3** succès à 373-432 ms, puis 2 échecs à 5646-5661 ms, fiche `0x6e00` inchangée | Quota de reconnexions confirmé ; le registre ne le consomme pas |
| 74 | Trace L2CAP V1/V2 par `btparsertrace` | V1 : pending puis success ; V2 : pending seul sur deux canaux `0x0011` | Le second `0x0011` est observé sur le trajet qui reste pending ; les contextes sont valides |
| 75 | Remap du second PSM et effacement du bit 6 | Le remap vers `0x0013` et `0x2144 -> 0x2104` sont exécutés une fois puis l'échec reste identique | Ni le PSM répété ni le bit 6 ne sont le correctif |
| 76 | Completion native `SceBt+0x64F8` | Les deux canaux passent `0x2 -> 0x6`, reçoivent success et le canal Control se configure ; pas de HID, puis `0x0144 -> 0x01C4` | Le point d'entrée final est prouvé ; blocage restant après la configuration |
| 77 | `ds4v2reco` mode 3 (inquiry puis StartConnect) | Aucune trame V2 et LED jamais allumée ; aucun événement à traiter | Essai non déclenché, donc sans conclusion sur le mode 3 |

### Incidents

- **Boot loop** (test 5) — I/O fichier dans un hook posé sur `SceBt → SceRegMgr`.
  Résolu par polling FTP. Règles de sûreté en §13.
- **Boot loop** (test 31) — `ksceBtGetRegisteredInfo` déborde sur la pile
  (§15.12). A motivé l'armement à usage unique.
- **Trois plantages à la connexion** (tests 47-48) — hooks sur des fonctions
  internes de SceBt d'arité inconnue. Aucune conséquence : l'armement à usage
  unique a ramené la console seule à chaque fois, sans intervention.
- **Blocage exigeant le bouton Reset** — vibration continue survivant à
  l'extinction, causée par le report DualSense de 73 octets envoyé par erreur à
  la DS4 (bug `vid_pid`, §15.2).
- **Paramètres Bluetooth inutilisables** avec ds34vita — `ksceBtStopInquiry()`
  coupe la recherche système dès qu'une DS4 est repérée. Comportement **voulu** du
  plugin, donc incompatible avec l'appairage par les Paramètres.

## 17. Sources

- fail0verflow — *PS4 Aux Hax 3: DualShock4* : https://fail0verflow.com/blog/2018/ps4-ds4/
- Al's blog — DS4 Reverse Engineering :
  - Part 3 (mode DFU, commandes) : https://blog.the.al/2023/01/03/ds4-reverse-engineering-part-3.html
  - Part 4 (flash, calibration IMU) : https://blog.the.al/2023/01/04/ds4-reverse-engineering-part-4.html
  - Part 6 (manette briquée, mode série) : https://blog.the.al/2023/07/13/ds4-reverse-engineering-part-6.html
- dualshock-tools/ds4-tools : https://github.com/dualshock-tools/ds4-tools
- dualshock-tools (web GUI) : https://dualshock-tools.github.io/
- PSXHAX — DS4 firmware dump & reversing tools : https://www.psxhax.com/threads/dualshock-4-ds4-ps4-firmware-dump-reversing-tools-by-ds4user.1159/
- DS4-USB — PS4 Developer wiki : https://www.psdevwiki.com/ps4/DS4-USB
- DualShock 4 — Eleccelerator wiki (descripteurs) : http://eleccelerator.com/wiki/index.php?title=DualShock_4
- Cynthion — Great Scott Gadgets : https://greatscottgadgets.com/cynthion/ · Crowd Supply : https://www.crowdsupply.com/great-scott-gadgets/cynthion
- Total Phase Beagle USB 480 : https://www.totalphase.com/products/beagle-usb480/
- DS4Windows : https://ds4windows.dev/

**Volet II (PS TV) :**

- ds34vita (MERLev) : https://github.com/MERLev/ds34vita
- ds4vita / ds3vita (xerpi) : https://github.com/xerpi/ds4vita
- **EyeToyPSVita (Esodland)** — plugin `SceBt` pilotant une PS Move de bout en
  bout, projet frère de celui-ci : https://github.com/Esodland/EyeToyPSVita
- **PSVita-RE-tools (TeamFAPS)** — `FAGDec` (décryption SELF→ELF),
  `psp2-kernel-bootimage-extract` (extraction des modules kernel du bootimage),
  `Kdumper`, `PSVita-error-code-resolver` :
  https://github.com/TeamFAPS/PSVita-RE-tools
- VitaCompanion : https://github.com/devnoname120/vitacompanion
- VitaSDK autobuilds : https://github.com/vitasdk/autobuilds
- Patch noyau Linux — *HID: sony: Support for DS4 clones that do not implement
  feature report 0x81* : https://lkml.iu.edu/hypermail/linux/kernel/2101.1/06546.html
- Patch *warn feature report 0x81* : https://lkml.kernel.org/lkml/20221030154058.10964-1-hcvcastro@gmail.com/T/
- hid-sony-clone-fix-dkms : https://github.com/Kyuunex/hid-sony-clone-fix-dkms

---

## 18. Glossaire rapide

- **DFU** : Device Firmware Update — mode boot alternatif pour flasher le firmware.
- **DS4 V2 / CUH-ZCT2** : révision 2016 de la DualShock 4 (barre lumineuse visible
  sur le touchpad, communication USB). `E` = version Europe.
- **HID** : Human Interface Device — classe USB des périphériques d'entrée.
- **MITM** : Man-In-The-Middle — dispositif intercalé sur le bus.
- **PUP** : PlayStation Update Package — format des mises à jour système PS4.
- **WebHID** : API navigateur pour parler à un périphérique HID sans driver.
- **Zadig** : outil Windows pour installer un driver USB générique (WinUSB/libusb).
