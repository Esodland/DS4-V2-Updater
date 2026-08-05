# Comparaison HCI Windows — DS4 V1 / V2

Cette expérience compare une même machine Windows avec une DS4 V1 et une DS4
V2, à la couche HCI Bluetooth Classic. Elle ne remplace pas une trace HCI sur
PS TV : elle répond à une question plus limitée mais importante — la V2 a-t-elle
un défaut général d'appairage ou de reconnexion BR/EDR ?

## Résultat du 4 août 2026

Les quatre fenêtres demandées ont été capturées et décodées sans perte
d'événement :

| Scénario | ETL | HCI texte | PCAPNG |
|---|---:|---:|---:|
| V1, première connexion | 27 Mo | 953 Ko | 461 Ko |
| V1, reconnexion | 23 Mo | 833 Ko | 409 Ko |
| V2, première connexion | 28 Mo | 991 Ko | 468 Ko |
| V2, reconnexion | 21 Mo | 918 Ko | 325 Ko |

Les fichiers source sont volontairement locaux, sous `captures/hci/`, et ne
sont pas versionnés car ils contiennent des échanges d'appairage.

### Appairage neuf

Les deux manettes suivent la même séquence Secure Simple Pairing :

```text
Connection Complete (success)
→ Authentication Requested
→ Link Key Request
→ Link Key Request Negative Reply
→ IO Capability Request / Response
→ User Confirmation Request
→ Simple Pairing Complete (success)
→ Link Key Notification
→ Encryption Change (success)
```

| Mesure à partir de `Connection Complete` | V1 | V2 |
|---|---:|---:|
| Chiffrement actif | 1,576 s | 0,822 s |
| Statut d'appairage | succès | succès |
| Type d'appairage | SSP, sans PIN legacy | SSP, sans PIN legacy |

L'écart temporel provient de la phase de confirmation utilisateur et ne révèle
aucun échec HCI : toutes les commandes et tous les événements terminaux sont au
statut `0x00`.

### Reconnexion normale avec `PS` seul

Les deux manettes initient une connexion entrante, obtiennent la même réponse
de l'hôte, réutilisent leur link key et activent le chiffrement avec succès.

| Étape depuis `Connect Request` | V1 | V2 |
|---|---:|---:|
| `Accept Connection Request` envoyé par Windows | 0,071 ms | 0,042 ms |
| `Connection Complete` (succès) | 168,654 ms | 211,764 ms |
| `Link Key Request` | 496,943 ms | 376,074 ms |
| `Encryption Change` (succès) | 634,855 ms | 531,714 ms |
| Canaux HID utilisables | 705,040 ms | 628,579 ms |

Après chiffrement, ou pendant son achèvement selon l'ordonnancement de la pile,
la V1 et la V2 ouvrent toutes deux les deux canaux L2CAP attendus :

- HID-Control (`PSM 0x0011`) ;
- HID-Interrupt (`PSM 0x0013`).

Chaque canal reçoit `Connection Response - Success`, puis les deux réponses de
configuration L2CAP réussissent. Les rapports de manette commencent ensuite à
arriver. Les identifiants de canal diffèrent entre les sessions, ce qui est
normal : ils sont alloués dynamiquement.

La V1 réalise en plus une courte interrogation SDP avant la remise en route des
canaux HID. La V2 n'en a pas besoin dans cette fenêtre. Ce comportement de cache
de services n'est pas une erreur et n'empêche pas la V2 d'atteindre l'état HID
opérationnel.

Wireshark étiquette certains rapports HID DS4 comme `Malformed Packet`. Ils
apparaissent après l'ouverture réussie des canaux et sur **les deux** manettes ;
ce sont des limites du dissécteur vis-à-vis de ces rapports propriétaires, pas
un statut d'erreur HCI ou L2CAP observé dans la V2.

### Conclusion

La V2 n'a pas de divergence HCI persistante par rapport à la V1 sur ce PC :
elle s'appaire, réutilise sa clé, chiffre le lien et ouvre HID en moins de
0,7 seconde. Cette expérience écarte un défaut Bluetooth Classic général de la
manette V2.

Par inférence, le défaut de reconnexion sur PS TV reste plus probablement lié
à l'interaction spécifique de son hôte Bluetooth (état mémorisé, base de bande,
timing ou firmware) qu'à une incapacité générale de la V2 à se reconnecter. La
prochaine mesure utile est donc la matrice de reconnexions contrôlées sur PS TV,
corrélée avec le journal passif `ds3trace`, plutôt qu'une nouvelle capture PC.

## Croisement avec la campagne PS TV du 5 août

La mesure PS TV a depuis localisé le premier écart utile : les requêtes L2CAP
V2 reçoivent normalement seulement `Connection Pending / Authentication
Pending`, alors que le témoin V1 reçoit ensuite `Connection Successful`.
Forcer la finalisation par le chemin natif de `SceBt` déclenche bien la
configuration L2CAP et le PSM Interrupt de la V2 ; elle échoue encore avant les
rapports HID. Les captures Windows restent donc la preuve que la liaison de la
manette est saine, sans prétendre couvrir cette phase spécifique de la pile PS
TV. Le détail et les journaux sont dans
[BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md](BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md).

## Outillage et reproductibilité

- Profil WPR officiel Microsoft : `tools/vendor/BluetoothStack.wprp`, SHA-256
  `D69C983384B970E8C0380C48EBEBBA547F94C4A84C9F4912A10798B6B08CF704`.
- Décodeur : `BTETLParse.exe`, issu du paquet Microsoft Bluetooth Test Platform
  1.14.0. Le MSI téléchargé localement a pour SHA-256
  `0DF5A3E3AEDE62770333FAB8FD2E044FC3BD6C891226F2087698291E7BAD69CA`.
- Les sorties sont générées dans `captures/hci/decoded/`. Le script
  `Capture-BtHci.ps1` utilise désormais automatiquement
  `tools/vendor/BTETLParse.exe` lorsqu'il est présent.

Microsoft documente ce format : `BTETLParse` extrait les flux HCI d'un ETL et
peut produire du texte et du PCAPNG, lisible par Wireshark.

## Protocole pour une future répétition

Les labels admis sont `v1-pair`, `v1-reconnect`, `v2-pair` et `v2-reconnect`.
Pour chaque manette, démarrer la trace, faire la première connexion avec
`SHARE + PS`, arrêter, éteindre la manette, puis faire une deuxième capture avec
`PS` seul. La seconde capture doit montrer : connexion entrante, link key,
chiffrement, puis les PSM HID `0x0011` et `0x0013`.
