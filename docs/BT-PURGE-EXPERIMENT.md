# Protocole d'essai — `btpurge`

Objectif : tester si l'entrée Bluetooth malformée `F8:6B:14:C3:62:24` contribue
au quota de reconnexions de la DS4 v2. L'essai ne conclut pas que cette entrée
est la cause : il teste uniquement cette hypothèse.

## Règles

- Une seule sonde kernel active à la fois.
- Ne jamais modifier directement `system.dreg` : `SceRegMgr` conserve un état
  en mémoire et écraserait le fichier au redémarrage.
- `btpurge` ne fait rien sans son fichier `.on`, qu'il supprime avant son appel
  à `ksceBtDeleteRegisteredInfo`.
- Une suppression d'entrée Bluetooth est une modification de l'état de la
  console. Elle se répare par un nouvel appairage ; ce n'est pas un flash ou
  une modification permanente du firmware.

## Préparation

Depuis la racine du dépôt :

```powershell
.\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Build
.\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Snapshot
```

Le second appel conserve une copie horodatée de `config.txt` et de
`vd0:/registry/system.dreg`. L'instantané doit être pris après un redémarrage
normal si l'on veut qu'il représente l'état effectivement écrit par le registre.

Vérifier immédiatement que l'entrée cible existe réellement dans cet instantané
(elle est stockée à l'envers dans le registre) :

```powershell
.\tools\Compare-BtRegistry.ps1 -Before .\logs\system.dreg.btpurge.<horodatage>.bin
```

## Essai

1. Vérifier que les autres sondes (`ds3trace`, `ds4v2bt`, `ds4v2reco`,
   `ds4v2fix`) sont désactivées dans `ur0:/tai/config.txt`.
2. Envoyer le module et sa configuration, l'ajouter à la section `*KERNEL`, puis
   armer exactement un essai :

   ```powershell
   .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Enable -Arm -Reboot
   ```

3. Le plugin attend 20 s par défaut, retire l'entrée cible,
   puis l'écrit dans `ur0:/log/btpurge.txt`. Il reste désarmé aux démarrages
   suivants.
4. Rapatrier le journal, puis retirer la ligne du plugin :

   ```powershell
   .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Log
   .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Disable -NoBuild
   ```

5. Redémarrer encore une fois pour forcer le flush de `SceRegMgr`, puis prendre
   un second instantané :

   ```powershell
   .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Snapshot
   ```

Critère immédiat : le journal doit confirmer un retour non négatif de
`ksceBtDeleteRegisteredInfo`, et le diff des deux `system.dreg` doit montrer la
disparition de l'entrée ciblée — pas une modification de l'entrée V2.

La vérification est automatisée par :

```powershell
.\tools\Compare-BtRegistry.ps1 `
  -Before .\logs\system.dreg.btpurge.<avant>.bin `
  -After  .\logs\system.dreg.btpurge.<apres>.bin
```

## Mesure après purge

Cette phase n'a pas été exécutée : le critère de persistance a échoué, donc une
nouvelle série de reconnexions n'aurait pas testé un état purgé.

1. Effectuer un appairage neuf de la DS4 v2.
2. Déployer `ds3trace` avec `offsets=0 clear6=0` et armer un essai.
3. Capturer au moins dix cycles extinction/rallumage de la V2, avec la V1
   éteinte et sans autre plugin de manette.
4. Consigner pour chaque cycle : délai `0x08 -> 0x05`, transition d'état,
   présence des événements `0x0C`, et issue fonctionnelle.

Un changement du nombre de reconnexions réussies ne suffit pas à conclure : il
faut comparer plusieurs appairages, car le quota observé varie déjà de un à
trois sans purge.

## Résultat du 2026-08-04

Le protocole a été mené jusqu'au bout sur la PS TV 3.65 :

- l'instantané initial confirme la fiche cible à `0x7600` avec MAC
  `F8:6B:14:C3:62:24`, VID/PID nuls, nom vide et link key nulle ;
- `btpurge` consomme l'armement et `ksceBtDeleteRegisteredInfo` retourne
  `0x00000000` ;
- après retrait du plugin, redémarrage de flush et second instantané, la fiche
  est toujours présente et identique.

Conclusion : dans cette configuration, le succès de l'API ne vaut pas
suppression persistée. Cette expérience est close ; ne pas la rejouer dans
l'espoir de recharger le quota de reconnexions.

## Retour arrière

En cas de souci de configuration, utiliser :

```powershell
.\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Rescue
```

Le script réinjecte par FTP la dernière sauvegarde `config.txt.bak-*` jusqu'à ce
que la fenêtre réseau du démarrage soit disponible.
