# Projet : PS TV - Fix et Spoofing pour DualShock 4 v2 (CUH-ZCT2)

## 1. Contexte et Problématique
La PlayStation TV (PS TV) souffre de conflits majeurs avec les manettes DualShock 4 v2 (PID : 0x09CC). 
- **Symptômes précis :** 
  - La manette ne fonctionne pas du tout en connexion filaire USB.
  - L'appairage Bluetooth natif échoue (nécessite l'aide d'une v1 pour forcer l'appairage).
  - La connexion n'est pas persistante au redémarrage (le registre rejette la MAC).
  - Input lag important une fois connectée.

## 2. Objectifs Techniques du Plugin Kernel (.skprx)
Ce plugin devra agir sur deux fronts simultanément : **SceBt** (Bluetooth) et **SceUsbd** (Filaire).

### Front A : La Connexion USB (SceUsbd)
La PS TV ignore la v2 en USB car son PID (0x09CC) n'est pas dans la liste blanche du pilote natif. Deux approches à tester :
1. **Spoofing d'énumération :** Hooker les fonctions de contrôle USB pour modifier à la volée le descripteur du périphérique et remplacer le PID 0x09CC par 0x05C4 (v1) avant que le pilote natif ne le lise.
2. **Pilote LDD Custom :** Utiliser `sceUsbdRegisterLdd` pour revendiquer le PID 0x09CC. Lire les endpoints USB HID (Endpoint d'interruption) et injecter les inputs manuellement via les hooks `SceCtrl`.

### Front B : La Connexion Bluetooth (SceBt)
1. Hooker la phase de *Handshake* pour forcer l'acceptation de l'adresse MAC.
2. S'assurer de la persistance de l'adresse MAC dans le registre système (`sceRegMgr`).
3. Analyser et adapter la fréquence de *polling* des paquets HID de la v2 pour éliminer l'input lag.

## 3. Directives pour l'Agent IA
- Cible : C natif, VitaSDK, taiHEN.
- Focus immédiat : Commence par générer le code pour l'enregistrement d'un driver USB custom via `sceUsbdRegisterLdd` pour le Vendor ID Sony (0x054C) et le Product ID de la v2 (0x09CC). 
- Fournir un squelette de code propre avec des logs abondants (via `sceClibPrintf` ou écriture dans `ux0:log.txt`) pour qu'on puisse voir ce qu'il se passe lors du branchement USB.
