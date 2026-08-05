/*
 * ds4v2bt - prise en charge directe de la DualShock 4 v2 sur PS TV
 *
 * ------------------------------------------------------------------------
 * L'IDEE
 * ------------------------------------------------------------------------
 * Le pilote natif de la PS TV pose le feature report 0x06 des la connexion et
 * attend sa reponse avant tout le reste. La DS4 v2 de ce projet ne repond
 * jamais a cette question apres une reconnexion : la sequence s'arrete la,
 * aucun rapport d'entree ne circule, et la manette raccroche d'elle-meme au
 * bout de ~3 s (README §15.5 ; aucun StartDisconnect n'est jamais emis).
 *
 * Ce plugin ne repare pas ce dialogue, il en mene un autre en parallele, qui
 * n'a jamais besoin du 0x06 :
 *
 *     0x05 connexion acceptee  ->  rapport de SORTIE 0x11  (type = 1)
 *                              ->  0x0B acquittement  ->  poster la LECTURE
 *                                                          (type = 0)
 *                              ->  0x0A  ->  donnees + REARMER la lecture
 *
 * C'est la sequence du plugin PS Move de EyeToyPSVita.
 *
 * ========================================================================
 * SURETE - lire avant de modifier quoi que ce soit
 * ========================================================================
 *
 * La version 0.1 a mis la console en BOOT LOOP le 2026-08-03, a +30 s, sans
 * aucune manette allumee. Elle a produit trois bannieres de demarrage et pas
 * une seule ligne de son thread : tout ce qui aurait designe le coupable etait
 * encore dans le tampon RAM au moment du crash. Deux lecons, appliquees ici.
 *
 * --- 1. ARMEMENT A USAGE UNIQUE ------------------------------------------
 *
 * Le plugin ne fait RIEN si le fichier ur0:/tai/ds4v2bt.on est absent. Il se
 * contente de le dire dans son journal et rend la main.
 *
 * Et quand le fichier est present, il le SUPPRIME avant de toucher a la
 * moindre API. Un essai consomme donc son armement : si cet essai plante la
 * console, le demarrage suivant trouve le plugin desarme et la console revient
 * toute seule. **Un boot loop est structurellement impossible.**
 *
 * C'est la propriete qui compte : la version precedente s'en remettait a une
 * course FTP contre la fenetre de demarrage. Elle a ete gagnee, mais elle
 * n'aurait pas du etre couru.
 *
 * --- 2. LE JOURNAL EST ECRIT AVANT L'APPEL, PAS APRES ---------------------
 *
 * Chaque appel systeme de la phase d'initialisation est annonce, le tampon est
 * VIDE SUR DISQUE, puis l'appel est fait, puis le code de retour est ecrit et
 * vide a son tour. Un plantage laisse donc sur le disque le nom de la fonction
 * qui n'est jamais revenue.
 *
 * --- 3. LA FENETRE MORTE, ET CE QUE LA BISECTION A MONTRE ----------------
 *
 * Bisection menee le 2026-08-03, un cran par redemarrage :
 *
 *     cran 1  journalisation seule                      OK
 *     cran 2  + ksceBtGetConfiguration()   -> 9         OK
 *     cran 3  + ksceKernelCreateCallback() -> 0x52E4F   OK
 *     cran 4  + ksceBtRegisterCallback()   -> 0         OK
 *     cran 5  + inventaire des appairages               *** CRASH ***
 *
 * L'inventaire lui-meme s'est termine : sa derniere ligne est sur le disque.
 * Ce qui a saute, c'est la suite -- et le cran 4, qui faisait exactement les
 * memes appels mais rendait la main tout de suite apres, passait.
 *
 * Lecture retenue : **etre enregistre aupres de SceBt sans consommer ses
 * evenements est fatal.** Le cran 4 s'enregistrait et se desenregistrait dans
 * la foulee ; le cran 5 laissait passer le temps de l'inventaire entre les
 * deux, sans jamais appeler ksceKernelCheckCallback() ni ksceBtReadEvent().
 *
 * D'ou la regle d'ecriture, qui vaut pour toute modification future :
 * **RIEN ne doit s'intercaler entre ksceBtRegisterCallback() et l'entree dans
 * la boucle qui draine la file.** Tout le travail preparatoire -- inventaire,
 * lecture de fichiers, suppression d'appairage -- se fait AVANT. C'est
 * pourquoi l'ordre des crans a change :
 *
 *     1  journalisation seule, aucun appel SceBt
 *     2  + ksceBtGetConfiguration()
 *     3  + ksceKernelCreateCallback()
 *     4  + ksceBtGetRegisteredInfo() (inventaire) -- toujours avant
 *     5  + ksceBtRegisterCallback()
 *     6  + boucle d'evenements complete  (defaut)
 *
 * ⚠️ Cette lecture reste une HYPOTHESE tant que le cran 6 n'a pas tourne. Elle
 * explique les cinq observations et rien ne la contredit, mais la seule preuve
 * recevable est un cran 6 qui tient.
 *
 * Combine a l'armement a usage unique, chaque redemarrage teste exactement un
 * cran et se desarme tout seul. Aucun essai ne peut couter plus qu'un
 * redemarrage.
 *
 * --- 4. Le reste, inchange depuis la 0.1 ---------------------------------
 *
 *  - AUCUN HOOK. Ce plugin n'intercepte rien, donc aucune re-entrance
 *    possible -- ce n'est pas la cause du boot loop de ds4v2fix v2.
 *  - Tout le travail est fait par un thread differe (`delay`, 30 s par
 *    defaut) : le demarrage reste intact et le FTP est monte avant nous.
 *  - Requetes et tampons STATIQUES, jamais sur la pile : SceBt est asynchrone
 *    et rien ne garantit qu'il les consomme avant le retour de l'appel.
 *  - Les actions sont restreintes a UNE adresse Bluetooth. La V1 temoin, qui
 *    fonctionne nativement, n'est jamais touchee.
 *  - Filet ultime inchange : maintenir L pendant le demarrage saute taiHEN.
 *
 * ------------------------------------------------------------------------
 * OPTIONS - ur0:/tai/ds4v2bt.cfg (facultatif)
 * ------------------------------------------------------------------------
 *     mac=C822B26FBCD3 delay=30 step=6 keepalive=2 outlen=13 forget=0 drive=1
 *
 *   mac       adresse BT visee, 12 chiffres hexa, sans separateur.
 *   delay     secondes avant que le thread ne commence (min 10, defaut 30).
 *   step      cran d'initialisation, 1 a 6 (defaut 6). Voir ci-dessus.
 *   keepalive secondes entre deux rappels du rapport de sortie (0 = aucun).
 *   outlen    taille du rapport 0x11 : 13 (court) ou 78 (etendu). Defaut 13.
 *   forget    1 = ksceBtDeleteRegisteredInfo() sur la manette visee. Supprime
 *             l'appairage : il faudra reappairer (SHARE + PS). Defaut 0.
 *   drive     1 = mener le dialogue (defaut). 0 = observer seulement.
 *
 * Armement : ur0:/tai/ds4v2bt.on   (fichier vide, consomme a chaque essai)
 * Journal   : ur0:/log/ds4v2bt.txt
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR   "ur0:/log"
#define LOG_FILE  LOG_DIR "/ds4v2bt.txt"
#define CFG_FILE  "ur0:/tai/ds4v2bt.cfg"
#define ARM_FILE  "ur0:/tai/ds4v2bt.on"

/* Adresse de la V2 de ce projet : C8:22:B2:6F:BC:D3.
 * Decoupage SceBt : mac1 = 16 bits de poids fort, mac0 = 32 bits de poids
 * faible. Non documente, et rien ne fonctionne sans. */
#define DEFAUT_MAC1 0x0000C822
#define DEFAUT_MAC0 0xB26FBCD3

/* Evenements SceBt :
 *   0x01 resultat de recherche      0x02 fin de recherche
 *   0x04 demande de link key        0x05 connexion acceptee
 *   0x06 deconnexion                0x08 connexion demandee
 *   0x09 connexion demandee sans appairage
 *   0x0A reponse a une requete type 0 (lecture)
 *   0x0B reponse a une requete type 1 (ecriture)
 *   0x0C reponse a une requete type 3 (feature) */
#define EVT_CONNEXION_ACCEPTEE 0x05
#define EVT_DECONNEXION        0x06
#define EVT_REPONSE_LECTURE    0x0A
#define EVT_REPONSE_ECRITURE   0x0B

#define TAILLE_ENTREE   128   /* un rapport 0x11 de DS4 fait 78 octets */
#define TAILLE_SORTIE   78    /* borne haute ; outlen decide de ce qu'on emet */

#define DESC_TEMOIN     0x5A  /* remplissage du tampon de descripteur HID */

#define PERIODE_MS      20    /* 50 tours par seconde */
#define TOURS_PAR_SEC   (1000 / PERIODE_MS)

/* --- Journal -------------------------------------------------------------- */

#define LOG_BUF_SIZE    (32 * 1024)
#define LOG_MAX_ENTRIES 1200

static char log_buf[LOG_BUF_SIZE];
static int  log_pos = 0;
static int  log_entries = 0;
static int  log_reentry = 0;
static int  log_dropped = 0;

/*
 * Origine des temps, posee au reveil du thread.
 *
 * Les journaux du 2026-08-03 ont tous butte sur la meme lacune : impossible de
 * dire si deux lignes sont separees par une microseconde ou par trois secondes.
 * C'est precisement ce qu'il fallait savoir pour comprendre la sequence
 * 0x05 -> 0x06, et ca a coute deux diagnostics faux. Chaque ligne porte donc
 * desormais son horodatage en millisecondes.
 */
static SceInt64 t_origine = 0;

static int millis(void)
{
	if (t_origine == 0)
		return 0;
	return (int)((ksceKernelGetSystemTimeWide() - t_origine) / 1000);
}

static void LOG(const char *fmt, ...)
{
	char tmp[224];
	va_list ap;
	int n;

	if (log_reentry)
		return;
	log_reentry = 1;

	/* Horodatage en tete de ligne, avant le message. */
	{
		char h[16];
		int hn = snprintf(h, sizeof(h), "%7d ", millis());
		if (hn > 0 && log_pos + hn < LOG_BUF_SIZE) {
			memcpy(log_buf + log_pos, h, hn);
			log_pos += hn;
		}
	}

	if (log_entries >= LOG_MAX_ENTRIES) {
		log_dropped++;
		log_reentry = 0;
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	if (n > 0) {
		if (n > (int)sizeof(tmp) - 1)
			n = sizeof(tmp) - 1;
		if (log_pos + n < LOG_BUF_SIZE) {
			memcpy(log_buf + log_pos, tmp, n);
			log_pos += n;
			log_entries++;
		} else {
			log_dropped++;
		}
	}

	log_reentry = 0;
}

static void log_flush(void)
{
	if (log_pos <= 0)
		return;

	SceUID fd = ksceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;

	ksceIoWrite(fd, log_buf, log_pos);
	ksceIoClose(fd);

	log_pos = 0;
	memset(log_buf, 0, sizeof(log_buf));
}

/*
 * Journaliser ET vider dans la foulee.
 *
 * C'est LA fonction de la phase d'initialisation : elle sert a annoncer un
 * appel systeme avant de le faire, de sorte qu'un plantage laisse son nom sur
 * le disque. La version 0.1 gardait tout en RAM et n'a rien pu dire de son
 * propre crash.
 */
static void TRACE(const char *fmt, ...)
{
	char tmp[224];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	LOG("%s", tmp);
	log_flush();
}

static void log_hexa(const unsigned char *donnees, int taille)
{
	for (int off = 0; off < taille; off += 16) {
		char ligne[80];
		int n = 0;
		for (int i = off; i < off + 16 && i < taille; i++)
			n += snprintf(ligne + n, sizeof(ligne) - n, " %02X", donnees[i]);
		ligne[n] = '\0';
		LOG("    +%02X:%s\n", off, ligne);
	}
}

/* --- Configuration -------------------------------------------------------- */

static unsigned int cible_mac0 = DEFAUT_MAC0;
static unsigned int cible_mac1 = DEFAUT_MAC1;
static int cfg_delay = 30;
static int cfg_step = 6;
static int cfg_keepalive = 2;
static int cfg_outlen = 13;
static int cfg_forget = 0;
static int cfg_drive = 1;

static int cfg_entier(const char *buf, int len, const char *cle)
{
	int klen = 0;
	while (cle[klen])
		klen++;

	for (int i = 0; i + klen < len; i++) {
		int j = 0;
		while (j < klen && buf[i + j] == cle[j])
			j++;
		if (j == klen) {
			int v = 0, d = 0;
			while (i + klen + d < len &&
			       buf[i + klen + d] >= '0' && buf[i + klen + d] <= '9') {
				v = v * 10 + (buf[i + klen + d] - '0');
				d++;
			}
			if (d > 0)
				return v;
		}
	}
	return -1;
}

/*
 * mac=C822B26FBCD3 -> mac1 = 0xC822, mac0 = 0xB26FBCD3.
 * Exige exactement 12 chiffres hexa : une adresse tronquee viserait un autre
 * appareil sans le dire.
 */
static int cfg_mac(const char *buf, int len)
{
	const char *cle = "mac=";
	const int klen = 4;

	for (int i = 0; i + klen < len; i++) {
		int j = 0;
		while (j < klen && buf[i + j] == cle[j])
			j++;
		if (j != klen)
			continue;

		unsigned long long v = 0;
		int d = 0;
		for (; d < 12 && i + klen + d < len; d++) {
			char c = buf[i + klen + d];
			int chiffre;
			if (c >= '0' && c <= '9')       chiffre = c - '0';
			else if (c >= 'A' && c <= 'F')  chiffre = c - 'A' + 10;
			else if (c >= 'a' && c <= 'f')  chiffre = c - 'a' + 10;
			else break;
			v = (v << 4) | (unsigned)chiffre;
		}
		if (d != 12)
			return 0;

		cible_mac1 = (unsigned int)((v >> 32) & 0xFFFF);
		cible_mac0 = (unsigned int)(v & 0xFFFFFFFF);
		return 1;
	}
	return 0;
}

static void cfg_load(void)
{
	char buf[192];
	int n, v;

	SceUID fd = ksceIoOpen(CFG_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	n = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';

	cfg_mac(buf, n);

	v = cfg_entier(buf, n, "delay=");
	if (v >= 10)
		cfg_delay = v;
	v = cfg_entier(buf, n, "step=");
	if (v >= 1 && v <= 6)
		cfg_step = v;
	v = cfg_entier(buf, n, "keepalive=");
	if (v >= 0 && v <= 60)
		cfg_keepalive = v;
	v = cfg_entier(buf, n, "outlen=");
	if (v == 13 || v == 78)
		cfg_outlen = v;
	v = cfg_entier(buf, n, "forget=");
	if (v == 0 || v == 1)
		cfg_forget = v;
	v = cfg_entier(buf, n, "drive=");
	if (v == 0 || v == 1)
		cfg_drive = v;
}

/* --- Armement a usage unique ---------------------------------------------- */

/*
 * Presence du fichier d'armement = autorisation de faire UN essai.
 *
 * Le fichier est supprime immediatement, avant le moindre appel risque. Si
 * l'essai plante la console, le demarrage suivant le trouve absent et ne fait
 * rien : la console revient d'elle-meme, sans course FTP et sans manipulation.
 *
 * La suppression est verifiee. Si elle echoue, on n'y va PAS : un armement qui
 * survivrait a son essai reintroduirait exactement le boot loop qu'on cherche
 * a rendre impossible.
 */
static int consommer_armement(void)
{
	SceUID fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd < 0) {
		TRACE("[arme] %s absent : plugin desarme, rien ne sera fait.\n", ARM_FILE);
		TRACE("[arme] pour armer un essai : deposer un fichier vide a cet emplacement.\n");
		return 0;
	}
	ksceIoClose(fd);

	int res = ksceIoRemove(ARM_FILE);
	TRACE("[arme] armement consomme (suppression -> 0x%08X)\n", res);
	if (res < 0) {
		TRACE("[arme] !! suppression impossible : essai ANNULE pour ne pas risquer\n");
		TRACE("[arme]    de rejouer le meme plantage au prochain demarrage.\n");
		return 0;
	}

	/* Verification effective plutot que confiance au code de retour. */
	fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
		TRACE("[arme] !! le fichier est toujours la : essai ANNULE\n");
		return 0;
	}

	TRACE("[arme] verifie absent. Un seul essai, quoi qu'il arrive.\n");
	return 1;
}

/* --- Etat de la liaison --------------------------------------------------- */

/*
 * Requetes et tampons STATIQUES. SceBt est asynchrone : rien ne garantit que
 * la requete soit consommee avant le retour de ksceBtHidTransfer, donc une
 * structure sur la pile serait relue apres destruction.
 */
static SceBtHidRequest requete_entree;
static SceBtHidRequest requete_sortie;
static unsigned char tampon_entree[TAILLE_ENTREE];
static unsigned char tampon_sortie[TAILLE_SORTIE];

/* Fenetre d'attente du canal HID apres le 0x05. La manette raccroche vers 3 s,
 * on se laisse un peu de marge pour que le journal montre l'echec plutot que de
 * s'arreter en silence. */
#define ATTENTE_MAX_MS 5000

static int connectee = 0;
static int attente_sortie = 0;    /* le canal HID n'est pas encore pret */
static int essais_sortie = 0;
static int dernier_etat = -1;
static int sortie_envoyee = 0;
static int lecture_engagee = 0;      /* une seule lecture en vol a la fois */
static unsigned int rapports_entree = 0;
static unsigned int acquittements = 0;
static int premiere_entree_vue = 0;

static SceUID id_callback = -1;
static SceUID thread_uid = -1;
static int doit_tourner = 1;

static int est_la_cible(unsigned int mac0, unsigned int mac1)
{
	return mac0 == cible_mac0 && mac1 == cible_mac1;
}

/* --- Emission ------------------------------------------------------------- */

/*
 * Rapport de sortie 0x11 de la DS4 en Bluetooth (type = 1, soit 0xA2 sur le
 * canal interrupt). C'est lui qui fait passer la manette en mode rapport
 * etendu, et il allume la LED -- temoin visuel gratuit.
 *
 *   [0]  0x11   identifiant du rapport
 *   [1]  0x80   cadence / drapeaux de liaison
 *   [2]  0x0F   active vibreur + LED + clignotement
 *   [6]  moteur droit (faible)      [7]  moteur gauche (fort)
 *   [8]  R      [9]  G      [10] B
 *   [11] duree allumee               [12] duree eteinte
 *
 * VERT vif, deliberement : le pilote natif allume bleu et ds34vita magenta.
 * La couleur dit du premier coup d'oeil qui pilote la manette.
 *
 * outlen = 13 par defaut : le releve du 2026-07-31 (README §15.4) laissait
 * penser que cette manette accepte le rapport court et ignore l'etendu. C'est
 * le seul endroit ou ce plugin parie plutot que de mesurer, d'ou l'option.
 */
static int envoyer_sortie(void)
{
	memset(tampon_sortie, 0, sizeof(tampon_sortie));
	tampon_sortie[0]  = 0x11;
	tampon_sortie[1]  = 0x80;
	tampon_sortie[2]  = 0x0F;
	tampon_sortie[8]  = 0x00;  /* R */
	tampon_sortie[9]  = 0xFF;  /* G */
	tampon_sortie[10] = 0x00;  /* B */
	tampon_sortie[11] = 0xFF;
	tampon_sortie[12] = 0x00;

	memset(&requete_sortie, 0, sizeof(requete_sortie));
	requete_sortie.type = 1;
	requete_sortie.buffer = tampon_sortie;
	requete_sortie.length = (unsigned int)cfg_outlen;
	requete_sortie.next = &requete_sortie;

	return ksceBtHidTransfer(cible_mac0, cible_mac1, &requete_sortie);
}

/* Armer une lecture. Une seule a la fois : le garde `lecture_engagee` est la
 * pour ca, et l'appelant doit l'avoir verifie. */
static int armer_lecture(void)
{
	memset(&requete_entree, 0, sizeof(requete_entree));
	memset(tampon_entree, 0, sizeof(tampon_entree));
	requete_entree.type = 0;
	requete_entree.buffer = tampon_entree;
	requete_entree.length = sizeof(tampon_entree);
	requete_entree.next = &requete_entree;

	return ksceBtHidTransfer(cible_mac0, cible_mac1, &requete_entree);
}

/* --- Traitement des evenements -------------------------------------------- */

static int drain_courant = 0;   /* rang de l'evenement dans le lot de drainage */

static void traiter_evenement(const SceBtEvent *ev)
{
	if (!est_la_cible(ev->mac0, ev->mac1)) {
		static unsigned int autres = 0;
		if (++autres <= 8)
			LOG("[autre] id=0x%02X mac=%08X:%08X\n", ev->id, ev->mac0, ev->mac1);
		return;
	}

	/*
	 * ⚠️ CHEMIN CRITIQUE : NE RIEN FAIRE AVANT D'AVOIR EMIS.
	 *
	 * Mesure du 2026-08-03, horodatage a l'appui : le lien vit **environ 50 ms**
	 * apres le 0x05 (etat 4 a 87108 ms, deja 1 a 87162 ms). Or un appel a TRACE
	 * coute ~25 ms -- il ouvre, ecrit et ferme un fichier sur ur0:. Trois lignes
	 * de journal suffisaient donc a rater toute la fenetre, et c'est exactement
	 * ce que faisait la version precedente : l'instrumentation posee pour
	 * traquer un boot loop etouffait la reactivite qu'elle devait mesurer.
	 *
	 * Regle qui en decoule, et qui vaut pour toute modification future :
	 * dans cette fonction on EMET d'abord, on journalise ensuite, et en RAM
	 * seulement (LOG, jamais TRACE). Le vidage sur disque est l'affaire de la
	 * boucle, quand il ne se passe rien.
	 */
	if (ev->id == EVT_CONNEXION_ACCEPTEE) {
		connectee = 1;
		premiere_entree_vue = 0;
		rapports_entree = 0;
		acquittements = 0;

		/*
		 * Les deux requetes partent d'affilee, sans rien entre elles.
		 *
		 * Sortie et lecture ont chacune leur structure statique : les poster
		 * ensemble ne les fait pas se marcher dessus. Ce qui n'etait pas le cas
		 * de deux LECTURES sur une meme structure -- le defaut du correctif
		 * ds34vita du 2026-07-31.
		 *
		 * On n'attend plus le 0x0B pour armer la lecture. Avec une fenetre de
		 * 50 ms, attendre un evenement intermediaire coute plus cher que le
		 * risque de poster tot.
		 */
		int r_out = cfg_drive ? envoyer_sortie() : 0;
		int r_rd  = cfg_drive ? armer_lecture()  : 0;

		/* Voila. Maintenant on peut parler. */
		int etat = ksceBtGetConnectingInfo(ev->mac0, ev->mac1);
		LOG("[evt] 0x05 acceptee (rang %d), etat %d\n", drain_courant, etat);

		if (!cfg_drive) {
			LOG("[hid] drive=0 : observation seule\n");
		} else {
			LOG("[hid] sortie 0x11 (%d o) -> 0x%08X | lecture -> 0x%08X\n",
			    cfg_outlen, r_out, r_rd);
			sortie_envoyee = (r_out >= 0);
			lecture_engagee = (r_rd >= 0);
			if (r_out < 0) {
				/* Rattrapage arme : la boucle retentera sans journaliser. */
				attente_sortie = 1;
				essais_sortie = 0;
				dernier_etat = etat;
				if (r_out == (int)SCE_BT_ERROR_HID_NOT_CONNECTED)
					LOG("[hid] HID_NOT_CONNECTED : rattrapage arme\n");
				else
					LOG("[hid] code de sortie inattendu : rattrapage arme\n");
			}
		}
		return;
	}

	/* Une fois le flux etabli les 0x0A arrivent par centaines : garder les
	 * premiers, puis se taire. C'est ce qui avait noye les journaux du
	 * 2026-07-31. */
	int bavard = 1;
	if (ev->id == EVT_REPONSE_LECTURE) {
		rapports_entree++;
		bavard = (rapports_entree <= 3);
		if (rapports_entree == 4)
			LOG("[hid] flux d'entree etabli, journal allege a partir d'ici\n");
	} else if (ev->id == EVT_REPONSE_ECRITURE) {
		acquittements++;
		bavard = (acquittements <= 3);
	}

	switch (ev->id) {

	case EVT_REPONSE_LECTURE:
		/* Le tampon porte le rapport qui vient d'arriver : le lire AVANT de
		 * rearmer, sinon on relit du vide. */
		if (!premiere_entree_vue) {
			premiere_entree_vue = 1;
			LOG("[hid] *** PREMIERE ENTREE ***  id=0x%02X\n", tampon_entree[0]);
			log_hexa(tampon_entree, 32);
		}
		if (cfg_drive) {
			int r = armer_lecture();
			if (r < 0) {
				lecture_engagee = 0;
				LOG("[hid] rearmement -> 0x%08X\n", r);
			}
		}
		break;

	case EVT_REPONSE_ECRITURE:
		/* La sortie est acquittee. La lecture est deja en vol depuis le 0x05,
		 * on ne la reposte donc pas -- une seule en vol a la fois. */
		if (cfg_drive && !lecture_engagee) {
			int r = armer_lecture();
			LOG("[hid] lecture (re)armee sur 0x0B -> 0x%08X\n", r);
			lecture_engagee = (r >= 0);
		}
		break;

	case EVT_DECONNEXION:
		LOG("[hid] 0x06 deconnexion apres %u entrees, %u acquittements\n",
		    rapports_entree, acquittements);
		connectee = 0;
		sortie_envoyee = 0;
		lecture_engagee = 0;
		break;

	default:
		break;
	}

	if (bavard) {
		/* Le rang de drainage dit si deux evenements sont arrives dans le MEME
		 * lot. C'est ce qui a montre que 0x05 et 0x06 se suivent de tres pres. */
		unsigned short vid_pid[2] = { 0, 0 };
		int r = ksceBtGetVidPid(ev->mac0, ev->mac1, vid_pid);
		LOG("[evt] id=0x%02X (rang %d)  vidpid 0x%08X %04X:%04X  etat %d\n",
		    ev->id, drain_courant, r, vid_pid[0], vid_pid[1],
		    ksceBtGetConnectingInfo(ev->mac0, ev->mac1));
	}
}

/* --- Nettoyage optionnel de l'appairage ----------------------------------- */

/*
 * forget=1 : forcer le chemin « appairage neuf ».
 *
 * Sur cette manette une association neuve fonctionne a tous les coups (README
 * §15.6) et une reconnexion echoue a tous les coups. La difference tient a ce
 * que SceBt fait d'une entree deja connue -- il saute l'interrogation SDP.
 *
 * Reserve : dans EyeToyPSVita ce symptome venait d'une entree FABRIQUEE a la
 * main ; ici l'entree a ete ecrite par SceBt lui-meme et elle est identique a
 * celle de la V1 (test 16). C'est une experience, pas un correctif annonce.
 *
 * Suppression par l'API, jamais par le registre : ksceBtDeleteRegisteredInfo
 * prend une adresse et non un index, donc aucun risque de viser un autre
 * appareil.
 */
static void oublier_appairage(void)
{
	TRACE("[forget] DeleteRegisteredInfo sur %08X:%08X...\n", cible_mac0, cible_mac1);
	int r = ksceBtDeleteRegisteredInfo(cible_mac0, cible_mac1);
	TRACE("[forget] -> 0x%08X ; remettre la manette en appairage (SHARE + PS)\n", r);
}

/*
 * Inventaire des appairages.
 *
 * ⚠️ CE TAMPON N'EST PAS SUR LA PILE, ET CE N'EST PAS UN DETAIL.
 *
 * Les deux crashs du 2026-08-03 (v0.2 cran 5, v0.3 cran 6) sont tombes au
 * MEME endroit : juste apres la derniere ligne de cette fonction, donc a son
 * RETOUR. Les deux versions plaçaient l'appel a des moments differents de
 * l'initialisation, ce qui elimine le voisinage comme explication. Un crash
 * qui survient au retour d'une fonction, et non pendant son execution, est la
 * signature d'une adresse de retour ecrasee.
 *
 * Le suspect est `ksceBtGetRegisteredInfo`, a qui l'on passe pourtant
 * sizeof(SceBtRegisteredInfo) = 0x100 -- la taille annoncee par l'en-tete du
 * SDK. Si le noyau ecrit davantage, ou ignore le parametre de taille, un
 * tampon local deborde sur le cadre de pile.
 *
 * D'ou : tampon STATIQUE, et surdimensionne a 4x la taille annoncee. Meme si
 * l'hypothese est fausse, ce tampon ne coute rien ; et si elle est juste, le
 * debordement tombe dans du remplissage inoffensif au lieu du cadre de pile.
 *
 * Le tampon est aussi rempli d'un TEMOIN avant chaque appel : ce qui depasse
 * 0x100 nous dira si le noyau ecrit vraiment au-dela.
 */
#define INFO_ANNONCE  ((int)sizeof(SceBtRegisteredInfo))   /* 0x100 */
#define INFO_RESERVE  (INFO_ANNONCE * 16)                  /* large exprès */
#define INFO_TEMOIN   0xA5

static unsigned char info_brut[INFO_RESERVE];

static void journal_appairages(void)
{
	TRACE("[appairages] inventaire ; sizeof(SceBtRegisteredInfo) = %d o, reserve = %d o\n",
	      INFO_ANNONCE, INFO_RESERVE);

	for (int dev = 0; dev < 8; dev++) {
		memset(info_brut, INFO_TEMOIN, sizeof(info_brut));

		TRACE("[appairages] GetRegisteredInfo(dev=%d)...\n", dev);
		int r = ksceBtGetRegisteredInfo(dev, 0,
			(SceBtRegisteredInfo *)info_brut, INFO_ANNONCE);
		TRACE("[appairages] dev=%d -> 0x%08X\n", dev, r);

		/*
			 * Jusqu'ou le noyau a-t-il ecrit ?
			 *
			 * Le premier octet touche au-dela de la taille annoncee dit qu'il
			 * y a debordement ; le dernier dit de combien, donc la taille
			 * REELLE de la structure sur ce firmware. C'est cette valeur qu'il
			 * faudra retenir, l'en-tete du SDK annonçant 0x100 a tort.
			 */
		int premier = -1, dernier = -1;
		for (int i = INFO_ANNONCE; i < INFO_RESERVE; i++)
			if (info_brut[i] != INFO_TEMOIN) {
				if (premier < 0) premier = i;
				dernier = i;
			}
		if (premier >= 0)
			TRACE("[appairages] !! ecrit AU-DELA de %d o : offsets %d a %d "
			      "-> taille reelle >= %d o (0x%X)\n",
			      INFO_ANNONCE, premier, dernier, dernier + 1, dernier + 1);

		if (r < 0)
			continue;

		/*
		 * Un emplacement vide laisse le tampon INTACT : le noyau n'ecrit rien
		 * et rend 0. Tester le contenu reviendrait donc a tester notre propre
		 * temoin -- c'est ce que faisait la version precedente, qui publiait
		 * de belles lignes « dev=1 A5:A5:... » sans le moindre peripherique
		 * derriere. Un tampon non ecrit est la seule preuve d'absence.
		 */
		int ecrit = 0;
		for (int i = 0; i < INFO_ANNONCE; i++)
			if (info_brut[i] != INFO_TEMOIN) { ecrit = 1; break; }
		if (!ecrit) {
			TRACE("[appairages] dev=%d : emplacement vide (tampon non ecrit)\n", dev);
			continue;
		}

		SceBtRegisteredInfo *info = (SceBtRegisteredInfo *)info_brut;
		if (info->mac[0] == 0 && info->mac[1] == 0)
			continue;

		/* Le nom vient du noyau : rien ne garantit qu'il soit termine. Sans
		 * cette precaution, le %s ci-dessous lit au-dela de la structure. */
		info->name[sizeof(info->name) - 1] = '\0';

		LOG("  dev=%d %02X:%02X:%02X:%02X:%02X:%02X %04X:%04X \"%s\"\n",
		    dev, info->mac[0], info->mac[1], info->mac[2],
		    info->mac[3], info->mac[4], info->mac[5],
		    info->vid, info->pid, info->name);
		log_flush();
	}
	TRACE("[appairages] fin de la boucle, retour de la fonction...\n");
}

/* --- Thread --------------------------------------------------------------- */

static int callback_bt(int notify_id, int notify_count, int notify_arg, void *donnees)
{
	(void)notify_id; (void)notify_count; (void)notify_arg; (void)donnees;
	return 0;
}

static int thread_principal(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	/* Le demarrage de la console est laisse totalement intact : on attend que
	 * le systeme soit lance et le reseau monte avant de toucher a SceBt. */
	ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);

	t_origine = ksceKernelGetSystemTimeWide();
	TRACE("=== thread reveille apres %d s ; horloge en ms a gauche ===\n", cfg_delay);

	/* AVANT toute API : consommer l'armement. Un essai, et un seul. */
	if (!consommer_armement())
		return 0;

	TRACE("[step] cran demande : %d\n", cfg_step);
	TRACE("[step] 1 journal seul | 2 GetConfiguration | 3 CreateCallback\n");
	TRACE("[step] 4 RegisterCallback | 5 GetRegisteredInfo | 6 boucle complete\n");

	if (cfg_step < 2) {
		TRACE("[step] cran 1 atteint sans encombre. Aucun appel SceBt fait.\n");
		return 0;
	}

	/*
	 * Cran 2. ksceBtGetConfiguration() est l'un des trois appels que ds4v2fix
	 * ne faisait pas -- et ds4v2fix tournait sans broncher au meme instant du
	 * demarrage. C'est donc l'un des trois suspects.
	 */
	TRACE("[step 2] appel de ksceBtGetConfiguration()...\n");
	int radio = ksceBtGetConfiguration();
	TRACE("[step 2] ksceBtGetConfiguration -> %d  (0 = eteinte, 9 = allumee)\n", radio);

	if (cfg_step < 3) {
		TRACE("[step] cran 2 atteint sans encombre.\n");
		return 0;
	}

	/* Cran 3. Le callback doit etre cree ET enregistre depuis CE thread : sur
	 * Vita un callback appartient au thread qui l'enregistre, et SceBt ne
	 * delivre ses evenements qu'a son proprietaire. Enregistre ailleurs, il
	 * rend 0 et chaque lecture echoue ensuite en CB_NOT_REGISTERED -- panne
	 * muette qui imite un peripherique absent. */
	TRACE("[step 3] appel de ksceKernelCreateCallback()...\n");
	id_callback = ksceKernelCreateCallback("ds4v2bt_cb", 0, callback_bt, NULL);
	TRACE("[step 3] ksceKernelCreateCallback -> 0x%08X\n", id_callback);
	if (id_callback < 0) {
		TRACE("[step 3] echec : arret propre du thread.\n");
		return 0;
	}

	if (cfg_step < 4) {
		TRACE("[step] cran 3 atteint sans encombre.\n");
		ksceKernelDeleteCallback(id_callback);
		id_callback = -1;
		return 0;
	}

	/*
	 * Cran 4 : l'inventaire des appairages, AVANT tout enregistrement.
	 *
	 * L'ordre a change apres la bisection du 2026-08-03, et ce n'est pas
	 * cosmetique -- voir le commentaire de tete, « la fenetre morte ». Tout ce
	 * qui peut etre fait avant de s'enregistrer doit l'etre avant.
	 */
	if (cfg_step >= 4) {
		journal_appairages();
		/* Marqueur decisif : si cette ligne manque alors que « fin de la
		 * boucle » est presente, le crash est au RETOUR de la fonction, donc
		 * dans son cadre de pile -- et non dans l'un de ses appels. */
		TRACE("[appairages] RETOUR OK, la pile a survecu.\n");
	}

	if (cfg_step < 5) {
		TRACE("[step] cran %d atteint sans encombre.\n", cfg_step);
		ksceKernelDeleteCallback(id_callback);
		id_callback = -1;
		return 0;
	}

	if (cfg_forget)
		oublier_appairage();

	/*
	 * Cran 5 : l'enregistrement, immediatement suivi de la boucle.
	 *
	 * Rien ne doit s'intercaler ici. C'est le seul enchainement teste comme
	 * sain, et la raison d'etre de la reorganisation ci-dessus.
	 */
	TRACE("[step 5] appel de ksceBtRegisterCallback(), puis boucle immediate...\n");
	int reg = ksceBtRegisterCallback(id_callback, 0, 0xFFFFFFFF, 0xFFFFFFFF);
	TRACE("[step 5] ksceBtRegisterCallback -> 0x%08X\n", reg);
	if (reg < 0) {
		TRACE("[step 5] echec : arret propre du thread.\n");
		ksceKernelDeleteCallback(id_callback);
		id_callback = -1;
		return 0;
	}

	/* Consommer tout de suite, avant meme la premiere temporisation. */
	ksceKernelCheckCallback();

	TRACE("[step 6] boucle d'evenements engagee.\n");
	TRACE("[init] en attente de la manette (bouton PS)\n");

	int tours = 0;
	int tours_flush = 0;
	int entretien_signale = 0;
	int premier_tour = 1;

	while (doit_tourner) {
		/* Rend la main aux callbacks kernel enregistres par les files SceBt. */
		ksceKernelCheckCallback();

		/*
		 * Vider la file a chaque tour, pas un evenement par tour.
		 *
		 * Etre enregistre et ne pas consommer assez vite est ce qui a tue la
		 * console au cran 5 de la bisection. Un evenement par tour de 20 ms
		 * plafonne a 50 par seconde, ce qui est peu quand le flux HID est
		 * etabli. On draine, avec une borne pour ne pas tourner indefiniment
		 * si la file se remplit aussi vite qu'on la vide.
		 */
		for (int drain = 0; drain < 16; drain++) {
			SceBtEvent ev;
			memset(&ev, 0, sizeof(ev));

			int res = ksceBtReadEvent(&ev, 1);

			if (premier_tour) {
				premier_tour = 0;
				TRACE("[boucle] premier ReadEvent -> 0x%08X\n", res);
			}

			/* Debordement de file : reessayer immediatement, comme ds34vita. */
			if (res == (int)SCE_BT_ERROR_CB_OVERFLOW)
				continue;

			/* ksceBtReadEvent NE BLOQUE PAS : il rend un succes avec un
			 * evenement vide quand il n'y a rien. Sans ce filtre et sans la
			 * temporisation en fin de boucle, la console sature. */
			if (res < 0 || (ev.id == 0 && ev.mac0 == 0 && ev.mac1 == 0))
				break;

			drain_courant = drain;
			traiter_evenement(&ev);
		}

		/*
		 * Attente active du canal HID.
		 *
		 * Le seul signal fiable que le canal est pret est que le transfert
		 * cesse de rendre HID_NOT_CONNECTED : aucun evenement ne l'annonce.
		 * On retente donc a chaque tour, en journalisant les changements
		 * d'etat au passage -- c'est la seule facon de savoir si l'etat 4
		 * finit par devenir 5, ou s'il n'evolue jamais.
		 */
		/*
		 * Rattrapage : la sortie du 0x05 a echoue, on retente.
		 *
		 * Sans la moindre I/O -- c'est tout l'interet. On compte les essais et
		 * on ne journalise que l'issue, une fois. Journaliser chaque tentative
		 * rendrait la boucle vingt fois plus lente que la fenetre qu'elle
		 * essaie d'attraper.
		 */
		if (cfg_drive && connectee && attente_sortie) {
			int etat = ksceBtGetConnectingInfo(cible_mac0, cible_mac1);
			int r = envoyer_sortie();
			essais_sortie++;

			if (r >= 0) {
				attente_sortie = 0;
				sortie_envoyee = 1;
				LOG("[hid] sortie ACCEPTEE au rattrapage : %d essais (%d ms), etat %d\n",
				    essais_sortie, essais_sortie * PERIODE_MS, etat);
				int rr = armer_lecture();
				LOG("[hid] lecture armee dans la foulee -> 0x%08X\n", rr);
				lecture_engagee = (rr >= 0);
			} else if (essais_sortie * PERIODE_MS >= ATTENTE_MAX_MS) {
				attente_sortie = 0;
				LOG("[hid] abandon apres %d ms : dernier code 0x%08X, etat %d\n",
				    ATTENTE_MAX_MS, r, etat);
			}
			if (etat != dernier_etat) {
				dernier_etat = etat;
				LOG("[hid] etat -> %d a %d ms de rattrapage\n",
				    etat, essais_sortie * PERIODE_MS);
			}
		}

		/*
		 * Entretien de la liaison. La manette raccroche apres ~3 s de silence
		 * (mesure du 2026-07-31) ; c'est precisement ce qui se passe avec le
		 * pilote natif, bloque sur son 0x06. Un rappel periodique du rapport
		 * de sortie coupe court a ce scenario.
		 */
		if (cfg_drive && cfg_keepalive > 0 && connectee && sortie_envoyee &&
		    ++tours >= cfg_keepalive * TOURS_PAR_SEC) {
			tours = 0;
			int r = envoyer_sortie();
			if (r < 0) {
				LOG("[hid] entretien -> 0x%08X\n", r);
				log_flush();
				connectee = 0;
			} else if (!entretien_signale) {
				entretien_signale = 1;
				LOG("[hid] entretien en cours (rappel toutes les %d s)\n",
				    cfg_keepalive);
			}
		}

		if (++tours_flush >= TOURS_PAR_SEC) {
			tours_flush = 0;
			log_flush();
		}

		ksceKernelDelayThread(PERIODE_MS * 1000);
	}

	if (connectee)
		ksceBtStartDisconnect(cible_mac0, cible_mac1);

	if (id_callback >= 0) {
		ksceBtUnregisterCallback(id_callback);
		ksceKernelDeleteCallback(id_callback);
		id_callback = -1;
	}

	TRACE("=== thread arrete ===\n");
	return 0;
}

/* --- Points d'entree ------------------------------------------------------ */

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	cfg_load();

	/* Seule I/O du chemin de demarrage, et elle est sans danger : aucun hook
	 * n'existe dans ce plugin, donc rien ne peut reboucler. */
	ksceIoMkdir(LOG_DIR, 0777);
	LOG("\n===== ds4v2bt v0.2 =====\n");
	LOG("cible %08X:%08X  delay=%ds step=%d keepalive=%ds outlen=%d forget=%d drive=%d\n",
	    cible_mac0, cible_mac1, cfg_delay, cfg_step, cfg_keepalive, cfg_outlen,
	    cfg_forget, cfg_drive);
	log_flush();

	/*
	 * Pile de 64 Ko et non 16.
	 *
	 * ksceBtGetRegisteredInfo s'est revele ecrire au-dela de la taille qu'on
	 * lui annonce (voir journal_appairages). Le tampon a ete sorti de la pile,
	 * ce qui traite la cause ; cette marge traite la classe de probleme --
	 * d'autres appels SceBt peuvent avoir le meme defaut, et 48 Ko de rabiot
	 * ne coutent rien face a une console qui ne demarre plus.
	 */
	thread_uid = ksceKernelCreateThread("ds4v2bt", thread_principal,
		0x3C, 0x10000, 0, 0x10000, 0);
	if (thread_uid < 0) {
		LOG("thread KO 0x%08X\n", thread_uid);
		log_flush();
		return SCE_KERNEL_START_SUCCESS;
	}
	ksceKernelStartThread(thread_uid, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	doit_tourner = 0;

	if (thread_uid >= 0) {
		ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
		ksceKernelDeleteThread(thread_uid);
		thread_uid = -1;
	}

	LOG("arret (%d entrees perdues)\n", log_dropped);
	log_flush();

	return SCE_KERNEL_STOP_SUCCESS;
}
