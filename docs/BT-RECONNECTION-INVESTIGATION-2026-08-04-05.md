# Enquete reconnexion DS4 V2 / PS TV - 4 et 5 aout 2026

## Statut final de cette campagne

La reconnexion de la DualShock 4 V2 (CUH-ZCT2E) a ete isolee dans la pile
Bluetooth/L2CAP de la PS TV 3.65 Enso. Le controleur V2 est sain sur Windows :
il s'appaire, reutilise sa link key, chiffre le lien et ouvre les deux canaux
HID. Sur PS TV, le dialogue L2CAP est atteint mais la pile laisse normalement
les demandes de canal en Connection Pending / Authentication Pending.

Un essai de completion par le code natif de SceBt a amene les deux canaux V2
jusqu'a Connection Successful et a declenche la configuration L2CAP du canal
Control. C'est une etape de reverse importante, pas un correctif : aucun
rapport HID n'a suivi et l'etat de connexion a fini en echec.

Au dernier essai, la V2 n'a allume aucune LED et aucun evenement V2 n'est
arrive a la PS TV. Le test de connexion initiee par l'hote (mode 3) n'a donc
pas ete exerce. Cela ne permet pas de conclure que la manette est hors service
ou qu'elle est reinitialisee : seulement qu'aucune tentative radio n'a ete
observee pendant cette fenetre.

Il n'y a actuellement aucun plugin de recherche actif sur la PS TV. Tous les
essais actifs etaient one-shot, en RAM et suivis d'un retrait du module et d'un
redemarrage. Les configurations locales sont egalement revenues a un etat
inactif.

## Perimetre et materiel

| Element | Valeur |
|---|---|
| Console cible | PlayStation TV, firmware 3.65 Enso |
| Adresse cible | V2 C8:22:B2:6F:BC:D3 (ordre interne B26FBCD3:0000C822) |
| Manettes de reference | une DS4 V1 fonctionnelle, une DS4 V2 CUH-ZCT2E |
| Limite acceptee | aucun second exemplaire V2 disponible |
| Observation PS TV | plugins kernel passifs/one-shot, journaux RAM puis fichier |
| Observation PC | WPR + profil Bluetooth Microsoft, BTETLParse, Wireshark |

Le journal historique des tests 1 a 73 est dans le [README](../README.md#16-journal-des-tests-2026-07-31)
et dans sa suite [16 bis](../README.md#16-bis-journal-des-tests-2026-08-03).
Ce document est la source de verite pour la campagne 4--5 aout et corrige les
anciennes conclusions devenues trop fortes.

## Faits etablis, dans l'ordre des preuves

1. La V2 n'a pas un defaut Bluetooth Classic general. Les quatre captures
   Windows (premiere connexion et reconnexion, V1 et V2) sont completes et
   montrent SSP, link key, chiffrement et les PSM HID 0x0011 et 0x0013 pour les
   deux manettes. Voir [comparaison HCI Windows](HCI-WINDOWS-COMPARISON.md).
2. L'entree d'appairage PS TV et la link key ne changent pas entre les echecs ;
   reappairer peut rendre temporairement des reconnexions possibles mais ne
   fournit pas un correctif durable. Le phenomene de quota (1 puis 3 succes
   avant echec) a ete reproduit, V1 eteinte et meme supprimee de l'equation.
3. Sur PS TV, la V2 emet bien un ACL entrant puis une demande L2CAP HID
   Control. La premiere trame brute est
   01 20 0C 00 08 00 01 00 02 03 04 00 11 00 50 00 : handle ACL 1,
   Connection Request L2CAP, PSM 0x0011, CID source 0x0050.
4. SceBt+0x11544 alloue un contexte de canal valide pour chaque demande
   entrante V2. L'echec n'est donc ni avant ACL, ni avant L2CAP, ni une
   exhaustion d'allocation. La routine recueillie avec type=0x14 est la bonne ;
   l'ancien filtre type=4,5,7 avait masque ce chemin.
5. Dans le trajet d'echec natif, les deux demandes V2 recoivent uniquement
   Connection Pending / Authentication Pending (result=1,status=2). Puis,
   environ 5,2 s plus tard, les drapeaux racine deviennent 0x00002144, puis
   0x000021C4. Le bit 0x40 est le verdict expose comme etat 4, non le levier
   initial de la panne.
6. La V1 de reference recoit d'abord cette reponse pending puis une seconde
   reponse Connection Successful (result=0,status=0) pour chaque canal. Elle
   poursuit immediatement avec la configuration L2CAP et les rapports HID.

## Comparatif PS TV, V1 et V2

| Cas | Demandes de la manette | Reponses PS TV | Suite |
|---|---|---|---|
| V1 fonctionnelle | PSM 0x0001, 0x0011, 0x0013 | pending puis success pour chaque canal | configuration puis HID |
| V2, trajet natif en echec | 0x0011/CID 0x0050, puis a nouveau 0x0011/CID 0x0051 | pending seulement | timeout, 0x2144 |
| V2, completion native experimentale | 0x0011/CID 0x0050, configuration, puis 0x0013/CID 0x0051 | pending puis success natif sur les deux HID | pas de HID ; 0x0144 puis 0x01C4 |

Le second PSM 0x0011 observe dans le trajet d'echec ne doit donc plus etre
traite comme une cause racine. Quand le premier canal est vraiment finalise, la
V2 envoie la configuration puis le PSM Interrupt 0x0013 normalement. C'est une
consequence du premier canal reste pending, pas une incompatibilite fixe de la
V2.

## Essais et resultats de la campagne

| Essai | Methode | Resultat | Conclusion |
|---|---|---|---|
| Capture HCI V1/V2 | WPR et BTETLParse, paire neuve et reconnexion | aucune perte ; SSP, chiffrement et deux PSM HID reussis | la V2 et sa liaison BR/EDR sont fonctionnelles sur PC |
| Trace L2CAP passive | hooks SceBt+0x6558, +0x11544, +0x109EC, +0x11420 | demandes V2 et contextes de canal vus | localisation apres l'acceptation initiale L2CAP |
| Remap ponctuel | seconde demande V2 0x0011 vers 0x0013 en RAM | la requete et la reponse sont reroutees, puis echec identique | ne corrige pas la reconnexion ; piste close |
| Effacement clear6 | bit 0x40 retire une fois en RAM | 0x2144 vers 0x2104, puis retour natif vers echec | le bit est un symptome ; piste close |
| Reponse forgee success | result/status pending remplaces par success | la V2 envoie la configuration Control | confirme que la suite L2CAP est capable de demarrer ; pas un correctif acceptable |
| Completion native | appel one-shot du balayage SceBt+0x64F8 sur contextes V2 exacts | les deux reponses success sont emises par SceBt et la configuration Control arrive | preuve du chemin interne a completer ; echec toujours apres la configuration |
| StartConnect modes 1, 4, 5 | API publique sur evenement / a froid | 0x802F0203 pour V1 et V2 | une DS4 ainsi visee n'est pas connectable par cette API ; piste close |
| StartConnect mode 3 | inquiry puis connexion apres evenement V2 | aucune trame/evenement V2 ; LED jamais allumee | essai non declenche, ni succes ni echec interpretable |

## Detail de la completion native

Le code de SceBt+0x64F8 parcourt les canaux L2CAP. Quand un canal a un CID et
les bits d'etat 0x2, il ajoute 0x4 (donc 0x2 vers 0x6) puis appelle
SceBt+0x109EC, qui emet Connection Response / Success. C'est exactement la
seconde reponse observee chez la V1.

La sonde btparsertrace a conserve seulement les deux allocations V2 attendues,
dans leur ordre d'arrivee. Lorsque la reponse native pending exacte est vue,
elle programme une seule execution du balayage apres 2 ms. Aucun paquet L2CAP
n'est construit par le plugin : l'etat et la sortie sont produits par le code
natif de la PS TV.

La trace decisive est [btparsertrace-20260805-131642.txt](../logs/btparsertrace-20260805-131642.txt) :

    8441  V2 PSM 0011 / CID 0050 ; canal flags 00000002
    8441  Connection Response pending (result=1,status=2)
    8443  balayage natif ; canal flags 00000006
    8443  Connection Response success (result=0,status=0)
    8465  V2 Configuration Request ; reponse et demande de configuration PS TV
    8475  V2 PSM 0013 / CID 0051 ; canal flags 00000002
    8477  balayage natif ; canal flags 00000006 ; success
   13493  drapeaux racine 00000144
   13523  drapeaux racine 000001C4

Le changement 0x2144 vers 0x0144 est utile : la completion a evite le bit
0x2000 du trajet natif d'echec. Il reste toutefois une phase
post-configuration, probablement authentification/initialisation HID, qui
n'aboutit pas. Il serait faux de presenter ce resultat comme une manette
connectee ou comme une correction deployable.

## Sources, artefacts et reproductibilite

| Artefact | Usage |
|---|---|
| [BT-STATE-PROBE.md](BT-STATE-PROBE.md) | hooks, structures et conclusions L2CAP corrigees |
| [HCI-WINDOWS-COMPARISON.md](HCI-WINDOWS-COMPARISON.md) | preuve PC V1/V2 |
| [btparsertrace-20260805-131642.txt](../logs/btparsertrace-20260805-131642.txt) | completion native et configuration Control |
| [ds4v2reco-20260805-132140.txt](../logs/ds4v2reco-20260805-132140.txt) | dernier essai mode 3, sans evenement V2 |
| btparsertrace/main.c | sonde/recherche experimentale ; complete=0 par defaut |
| ds4v2reco/main.c | essai API publique mode 3 ; mode local remis a 0 |
| tools/Deploy-KernelProbe.ps1 | compile, depose, arme, recupere puis retire/reboot les sondes |

Les captures HCI brutes restent dans captures/hci/ et ne sont pas versionnees
car elles contiennent des echanges d'appairage. Elles ont ete decodees sans
perte par l'outillage Microsoft documente dans HCI-WINDOWS-COMPARISON.md.

## Etat de surete a la cloture

- PS TV : module de recherche retire de config.txt puis redemarrage effectue.
- btparsertrace/btparsertrace.cfg : remap=0, accept=0, complete=0.
- ds4v2reco/ds4v2reco.cfg : mode=0 ; la MAC est conservee seulement pour une
  future reproduction explicite.
- Aucune ecriture de registre, de link key ou de firmware de manette n'a ete
  faite par les essais de cette campagne.
- Les modifications de RAM etaient bornees a une tentative et perdues au reboot.

## Prochaine etape rationnelle

Ne pas repeter les essais fermes (remap, clear6, StartConnect direct) ni lancer
de sonde tant que la V2 n'emet pas une tentative visible. Quand la V2 produira
de nouveau un evenement, la mesure utile est de suivre la phase entre la seconde
completion L2CAP et le timeout : authentification de canal, options de
configuration restantes et initialisation HID dans SceBt/SceDs3.

Une nouvelle manette V2 permettrait de distinguer le comportement de cet
exemplaire de celui de la famille, mais elle n'est pas necessaire pour conserver
ou exploiter les resultats obtenus ici. Un reset physique de la manette est une
decision de l'utilisateur : les journaux actuels ne donnent pas de preuve qu'il
faille le faire, seulement l'absence de toute tentative radio dans le dernier
essai.
