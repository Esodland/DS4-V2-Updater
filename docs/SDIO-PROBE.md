# Sonde SDIO passive (`sdiotrace`)

## Pourquoi cette couche

La campagne controlee du 2026-08-04 a reproduit le quota de reconnexions de la
DS4 V2 : trois etablissements a 373--432 ms, puis deux echecs a 5646--5661 ms.
Les premiers filtres ne voyaient aucun canal L2CAP et avaient situe la
divergence sous HID. Une capture brute ulterieure de `SceBt+0x6558` a corrige
ce constat : la V2 envoie bien deux `Connection Request` L2CAP et SceBt cree
les deux contextes de canal correspondants. La divergence est donc apres cette
negociation initiale, avant les rapports HID de `SceDs3`. La fiche d'appairage
de la V2 dans `system.dreg` reste identique avant et apres ces echecs.

Les captures HCI Windows excluent aussi un defaut Bluetooth Classic general de
la manette : appairage, link key, chiffrement et les deux canaux HID sont
normaux sur le PC. La prochaine couche observable sur la PS TV est donc le
transport SDIO qui relie la pile au composant radio.

## Mise a jour 2026-08-05

Cette conclusion etait prudente avant la trace de completion L2CAP native. La
V2 atteint bien la pile SceBt, et une finalisation du premier canal par
SceBt+0x64F8 declenche sa configuration L2CAP puis la demande HID Interrupt.
Le transport SDIO ne porte donc plus la priorite : la divergence restante est
post-configuration dans SceBt/SceDs3. `sdiotrace` n'a pas ete deploye pendant
cette campagne et sa cartographie reste disponible uniquement comme plan de
secours. Voir le rapport
[BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md](BT-RECONNECTION-INVESTIGATION-2026-08-04-05.md).

## Cartographie statique, firmware 3.65

Les modules du boot image donnent une chaine claire :

```text
SceDs3 -> SceBt -> SceWlanBt -> SceSdifForDriver -> puce Wi-Fi / Bluetooth
```

`SceBt` importe sept fonctions de `SceSdifForDriver`, mais elles ne sont
appelees que par son chemin d'initialisation. Le flux utile est dans
`SceWlanBt`, qui importe treize fonctions de la meme bibliotheque. L'import
`0xD0F78D9B` est appele depuis 27 emplacements de `SceWlanBt`; les appels
preparent cinq mots ABI (r0--r3 et un mot sur la pile). On observe notamment
des tampons de 40 et 256 octets, ce qui en fait le meilleur point de comptage
des echanges SDIO sans supposer un format de paquet.

L'unique fonction SDIO deja nommee par VitaSDK dans cet ensemble est
`0x6A8235FC`, `ksceSdifGetSdContextPartValidateSdio`. Le nom de `0xD0F78D9B`
n'est pas documente; `sdiotrace` l'appelle donc volontairement `xfer`, sans
lui attribuer un protocole ou une direction inventee.

## Ce que fait `sdiotrace`

Le plugin pose **un seul** hook d'import :

```text
SceWlanBt : SceSdifForDriver : 0xD0F78D9B
```

Pour chaque transfert assez grand (`minlen=8` par defaut), il note seulement :
le contexte, l'adresse ou commande, la taille, l'adresse du tampon, le mode et
le code de retour. Il ne lit pas les octets du tampon, ne les garde pas, ne les
modifie pas et n'effectue aucune I/O depuis le hook.

Les garanties sont les memes que pour `ds3trace` : demarrage differe, armement
a usage unique (`ur0:/tai/sdiotrace.on` est supprime avant la pose du hook),
buffer RAM dans le hook et ecriture disque par un thread distinct. La trace est
bornee a 30 s et 1000 lignes par defaut, puis le hook est retire.

## Campagne minimale proposee

1. Compiler et deployer `sdiotrace`, arme une seule fois, sans aucune autre
   sonde active.
2. Apres le delai de 30 s, faire **une** reconnexion V2 qui reussit ou echoue.
3. Recuperer le journal. La mesure decisive est la presence, le rythme et les
   retours de `xfer` pendant la fenetre `0x08 -> 0x05/0x06` deja etablie par
   `ds3trace`.
4. Refaire une seule fois sur l'autre resultat (succes versus echec), si le
   quota actuel le permet. Aucune deuxieme manette V2 n'est necessaire.

Si le chemin SDIO differe deja dans les metadonnees, on pourra isoler un
transport ou un timeout. S'il est identique, la prochaine sonde devra etre
posee au-dessus de SDIO, sur les deux imports `SceWlanBtForDriver` de `SceBt`.
Dans les deux cas, lire le contenu des tampons reste une etape ulterieure, pas
un effet de bord de cette premiere sonde.

## Resultat du premier essai (echec V2)

La capture `logs/sdiotrace-20260804-153943.txt` est saine : 60 transferts,
aucune ligne perdue et tous les retours a zero. Elle expose toutefois un polling
periodique de 40 puis 256 octets toutes les 4,8 s, sur le meme contexte et les
memes adresses (`0x00000000` et `0x00010000`). Ce motif ne change pas pendant
l'echec de reconnexion rapporte par l'utilisateur. La sonde SDIO voit donc le
cote Wi-Fi partage de `SceWlanBt`, pas le transport Bluetooth qui decide de la
connexion.

L'etape suivante est `wlanbttrace`, une sonde encore plus conservative qui ne
voit que les deux valeurs scalaires echangees entre `SceBt` et `SceWlanBt`.

## Resultat de `wlanbttrace` (echec V2)

`logs/wlanbttrace-20260804-154655.txt` a pose les deux hooks avec succes, puis
ne releve **zero appel** a `0x0FC89113` et `0x4C96C3B7` pendant toute la fenetre
de 30 s. Cette interface est donc une initialisation ou un controle hors du
chemin de reconnexion, et non une piste de flux a approfondir.

Le reverse de `SceBt` a ensuite precise la cause des anciens plantages de
sondes d'abandon : `+0x0DDA` est le `push` situe *apres* l'entree reelle
`+0x0DD8`, qui charge `r3` depuis la structure de peripherique. Reprendre a
`+0x0DDA` fait reprendre l'original avec `r3` non initialise. `btfailtrace`
cible `+0x0DD8` et ne journalise que l'adresse du peripherique avant/apres
l'appel; son argument unique est etabli par les appels directs du binaire.
