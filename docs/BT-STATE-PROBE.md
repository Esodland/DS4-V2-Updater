# Sondes d'etat interne SceBt (PS TV 3.65)

## Resultat etabli le 2026-08-04

La sonde passive `btlookuptrace` observe la recherche interne
`SceBt+0x11420(root, type, a2, a3)`. Pour `type & 7 == 1`, `a2:a3` est la
MAC interne de la V2 et le retour est un enregistrement de la table SceBt.
La sonde ne lit que `record+4` (drapeaux) et `record+0x30` (identifiant de
connexion), deux champs que le code SceBt lit lui-meme immediatement apres
cette recherche.

Lors d'une reconnexion qui echoue, la V2 est vue a l'adresse `01FD04A8`. Son
identifiant de connexion passe de `0` a `1`, puis de `0` a `2` pour la seconde
tentative. A `+21,369 s`, ses drapeaux deviennent `0x00002144` : ce mot contient
le bit `0x40` que `ksceBtGetConnectingInfo` traduit en etat 4. La bascule est
donc bien une decision interne de `SceBt`, avant HID et avant les rapports de
manette.

Les chemins internes suivants sont exclus par capture :

- `SceBt+0x0DD8` (finaliseur a un argument) : hook sain, zero appel ;
- `SceBt+0x1C4FC` (travailleur sans argument) : hook sain, zero appel ;
- le repartiteur qui appellerait `SceBt+0x11420` avec `type=0x21` : aucun appel
  `type=0x21` pendant l'echec.

Le candidat restant est le parseur a arite et pile variables
`SceBt+0x6558`, dont le chemin passe justement par la recherche de type 2 et
pose le bit `0x40` a `+0x66BC`. Il ne doit pas etre hooke par une fonction C :
un essai historique a confirme que sa convention d'appel est trop large.

Le trampoline assembleur `btparsertrace` preserve l'integralite des arguments
et de la pile puis branche vers l'original sans appel C. Sa premiere capture
est saine : deux appels identiques `(r0=01FC0010, r1=02006804, r2=16, r3=2)`
surviennent 10 ms avant le premier drapeau `0x4`; le verdict `0x2144` suit
5,257 s plus tard. Une lecture ulterieure est donc bornee aux seuls 16 octets
que ce parseur va lui-meme lire, et seulement lorsqu'elle est explicitement
activee par `bytes=1`.

## Mise a jour : L2CAP est bien atteint

La trame brute capturee pendant l'echec est :

```text
01 20 0C 00 08 00 01 00 02 03 04 00 11 00 50 00
```

Elle se decode sans ambiguite comme un ACL entrant (handle `0x0001`), suivi
d'une commande L2CAP `Connection Request` (code `0x02`) vers le PSM HID
Control `0x0011`, avec CID source `0x0050`. La tentative suivante arrive 35 ms
plus tard et reclame de nouveau `PSM=0x0011`, `CID source=0x0051`.

Le hook passif de `SceBt+0x11544`, au chemin exact de cette commande, observe
deux allocations valides de contexte de canal (`0x02151958` puis
`0x02151BC0`, espaces de `0x268`). La pile PS TV ne bloque donc ni avant ACL,
ni avant L2CAP, ni sur l'absence de memoire de canal. Le bit d'echec `0x40`
est toujours pose ~5,2 s plus tard, apres cette negociation initiale.

La conclusion historique « L2CAP ne demarre jamais » etait due au filtre
precedent, limite aux types 4, 5 et 7. Le chemin entrant emploie le type
`0x14`; ce filtre ne pouvait pas le voir. La suite du reverse doit suivre le
gestionnaire HID installe sur ces deux canaux et verifier la reponse L2CAP,
pas revenir a l'appairage ou au transport SDIO.

## Comparaison directe V1 reussie / V2 en echec

Une capture V1 faite avec le meme trampoline et les memes hooks donne le
temoin fonctionnel suivant. `code=3` est la reponse de connexion emise par la
PS TV ; les CID repondus correspondent bien a chaque requete recue.

| Manette | Requetes L2CAP recues | Reponse PS TV | Suite observee |
|---|---|---|---|
| V1 (succes) | `PSM 0x0001/CID 0x0040`, puis `0x0011/0x0041`, puis `0x0013/0x0042` | CID `0x40`, `0x41`, `0x42`, avec les identifiants correspondants | configuration, puis rapports HID continus |
| V2 (trajet natif en echec) | `PSM 0x0011/CID 0x0050`, puis **`0x0011/CID 0x0051`** | CID `0x40` puis `0x41`, identifiants `3` et `4` correctement repris, mais pending seulement | aucun paquet de configuration; timeout ~5,2 s |

Les CID de la V2 sont valides (la plage dynamique commence a `0x0040`) et la
PS TV les reprend correctement. La demande repetee de `0x0011` n'est plus une
cause racine candidate : l'essai de completion native decrit plus bas montre
que, des que le premier canal recoit sa reponse finale, la V2 enchaine sur la
configuration puis demande normalement `0x0013`. Cette repetition est donc la
consequence du premier canal laisse en attente.

## Essai actif borne

Un essai `remap=1` a ete arme apres la comparaison V1/V2. La garde exigeait
la trame V2 exacte `code=2,id=4,PSM=0x0011,CID=0x0051`. L'utilisateur a du
appuyer trois fois : la premiere tentative a echoue avant ACL, la seconde
n'a livre que la premiere demande `PSM=0x0011/CID=0x0050`, et la troisieme
n'a pas atteint SceBt. La garde n'a donc jamais correspondu et **aucune
ecriture RAM n'a ete executee**. La configuration locale est revenue a
`remap=0` et le module a ete retire de `config.txt` avant redemarrage.

Une repetition propre a ensuite declenche la garde : le journal confirme
`remap n=2 V2 PSM 0011 -> 0013 (cid=0051)`, puis la recherche de type `0x14`
sur `0x0013` et la reponse `code=3` correspondante. Le resultat utilisateur
reste un echec LED fixe puis extinction et le bit `0x40` arrive au meme delai.
Le routage du second PSM n'est donc pas le correctif; il est seulement une
divergence de protocole utile pour localiser le chemin.

Le premier essai arme avec `clear6=1` n'a pas vu la V2 et n'a donc rien ecrit.
Un second essai, cette fois avec trois tentatives V2 effectivement recues, a
execute une seule ecriture : `0x00002144 -> 0x00002104`. Vingt-six
millisecondes plus tard, la pile a tout de meme amene le mot a `0x00002184`
puis a retire l'enregistrement de connexion. Les deux tentatives suivantes ont
reproduit la suite native `0x00000004 -> 0x00002144 -> 0x000021C4`.

Effacer le bit `0x40` ne maintient donc pas le lien : c'est un symptome, pas le
levier de correction. Toutes les ecritures etaient en RAM, bornees a une fois,
et ont disparu au redemarrage. Le fichier de configuration versionne est revenu
a `clear6=0`, et le module a ete retire de `config.txt` avant redemarrage.

## Completion native du canal (mise a jour 2026-08-05)

Le reverse de `SceBt+0x64F8` a etabli le mecanisme qui produit la seconde
reponse V1 : le balayage parcourt les canaux, detecte le mot d'etat `0x2`, lui
ajoute `0x4` et appelle `SceBt+0x109EC`. Ce dernier emet la reponse L2CAP
finale `result=0,status=0`. Ce chemin ne construit donc pas une reponse par
forgage de paquet : il finalise le canal par le code natif de SceBt.

Avec `complete=1`, `btparsertrace` a conserve les deux seuls contextes V2
attendus et a declenche ce balayage une fois, 2 ms apres la reponse pending
exacte. La trace [btparsertrace-20260805-131642.txt](../logs/btparsertrace-20260805-131642.txt)
montre :

```text
8441  PSM 0011 / CID 0050 : flags 00000002, pending
8443  balayage natif : flags 00000006, Connection Response success
8465  Configuration Request V2, puis Configuration Response et Request PS TV
8475  PSM 0013 / CID 0051 : flags 00000002, pending
8477  balayage natif : flags 00000006, Connection Response success
13493 flags racine 00000144
13523 flags racine 000001C4
```

Le premier canal complete debloque donc reellement la configuration et le PSM
Interrupt, ce qui corrige la lecture precedente de la trame double `0x0011`.
Mais aucune configuration du second canal ni aucun rapport HID n'est arrive,
et la connexion a encore echoue. La disparition du bit `0x2000`
(`0x2144` devient `0x0144`) est une preuve de progression, pas une solution.

## Suite sure

La sonde est revenue a `complete=0`, le module a ete retire de `config.txt`
et la PS TV redemarree. Ne pas refaire les essais `remap`, `clear6` ou
completion tant que la manette n'emet pas une tentative vue par la console.
La prochaine mesure utile, lorsqu'un evenement V2 reviendra, est la phase entre
la seconde completion L2CAP et le timeout : authentification de canal, options
de configuration restantes et initialisation HID. Le rapport complet de la
campagne est [BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md](BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md).
