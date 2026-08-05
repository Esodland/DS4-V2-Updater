/*
 * ds4v2reco - forcer le chemin de connexion qui fonctionne
 *
 * ========================================================================
 * CE QU'ON A MESURE
 * ========================================================================
 * Sur cette PS TV, la meme DualShock 4 v2 reussit ou echoue selon le CHEMIN
 * par lequel la connexion s'etablit -- pas selon son etat, ni le HID, ni les
 * feature reports. Releve du 2026-08-03, deux sessions a quelques minutes
 * d'intervalle, avec la meme manette :
 *
 *   APPAIRAGE NEUF, ça marche          RECONNEXION, ça echoue
 *   -------------------------          ----------------------
 *   0x01  resultat de recherche x4     0x08  connexion demandee
 *   0x04  demande de link key                (par la MANETTE)
 *   0x05  acceptee                     5612 ms d'attente
 *   0x0C  reponse au report 0x06       0x05  acceptee mais degradee
 *   0x0C  reponse a la calibration     0x06  deconnexion immediate
 *   0x0B  puis flux d'entree
 *
 * Deux lectures en decoulent, et elles renversent deux jours d'hypotheses :
 *
 * 1. **La manette repond parfaitement au feature report 0x06** -- en 36 ms,
 *    comme la v1. Ce report n'est donc pas un trait de ce modele, et n'a
 *    jamais ete la cause de quoi que ce soit.
 *
 * 2. **Le chemin « connexion entrante » echoue ; le chemin « la console
 *    initie apres une recherche » fonctionne.** La difference n'est pas dans
 *    la manette mais dans qui appelle qui.
 *
 * Explication plausible, non prouvee : apres une recherche, la console dispose
 * de parametres de page frais (horloge du peripherique, mode de balayage).
 * Sur une connexion entrante elle s'appuie sur ce qu'elle a stocke, et pour
 * cette manette ces valeurs ne permettent pas d'aboutir -- d'ou une expiration
 * a 5,6 s, compatible avec un delai de page Bluetooth.
 *
 * Indice retrospectif : ds34vita affichait « CONNECTED DS4 » six fois sur six
 * avec cette manette (test 19 du 2026-07-31). Son `case 0x02` appelle
 * `ksceBtStartConnect` a la fin d'une recherche : il empruntait le bon chemin
 * sans que personne sache pourquoi c'etait le bon.
 *
 * ========================================================================
 * CE QUE FAIT CE PLUGIN
 * ========================================================================
 * Il ecoute les evenements SceBt et, quand la manette visee appelle (0x08),
 * il force l'un des chemins alternatifs. **Aucun hook, aucun offset** : rien
 * que des fonctions exportees, donc portable d'un firmware a l'autre.
 *
 *   mode=0  observation seule, aucune action
 *   mode=1  sur 0x08 -> ksceBtStartConnect() immediatement
 *   mode=2  sur 0x08 -> StartDisconnect(), puis StartConnect() 200 ms apres
 *   mode=3  sur 0x08 -> ksceBtStartInquiry(), puis StartConnect() a la fin
 *           de la recherche (evenement 0x02). C'est la voie ds34vita.
 *   mode=4  laisser le chemin natif echouer, puis StartConnect() 500 ms
 *           apres la deconnexion (0x06).
 *
 * Les quatre modes se testent separement, et c'est voulu : on ignore encore si
 * ce qui compte est **le sens de la connexion** (mode 1) ou **la recherche
 * prealable** (mode 3). Un seul essai par mode tranchera.
 *
 * ⚠️ `ksceBtStartConnect` peut echouer en `CONNECT_START_BUSY` (0x802F0204) si
 * une recherche est en cours, ou en `CONNECT_START_CONNECTED` (0x802F020A) si
 * le lien existe deja. Ces codes sont attendus et journalises tels quels : ils
 * font partie de la reponse, pas du bruit.
 *
 * ========================================================================
 * SURETE
 * ========================================================================
 *  1. **Aucun hook.** Ce plugin n'intercepte rien -- il ecoute la file
 *     d'evenements et appelle des fonctions exportees. Pas de re-entrance
 *     possible, pas d'arite a deviner.
 *  2. **Armement a usage unique** : rien ne se passe sans `ur0:/tai/
 *     ds4v2reco.on`, et ce fichier est supprime avant toute action. Un essai
 *     qui plante laisse la console desarmee au demarrage suivant.
 *  3. **Thread differe** (`delay`, 30 s) : le demarrage reste intact et le FTP
 *     est monte avant qu'on touche a quoi que ce soit.
 *  4. **Une seule adresse visee**, et un nombre d'essais borne (`essais`) :
 *     pas de boucle de reconnexion infinie si ça ne marche pas.
 *  5. Journalisation en RAM dans la boucle, vidage disque seulement au repos --
 *     une ecriture coute ~25 ms et la fenetre utile est courte.
 *
 * Options : ur0:/tai/ds4v2reco.cfg
 *     mac=C822B26FBCD3 delay=30 mode=1 essais=3
 *
 * Journal : ur0:/log/ds4v2reco.txt
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR   "ur0:/log"
#define LOG_FILE  LOG_DIR "/ds4v2reco.txt"
#define CFG_FILE  "ur0:/tai/ds4v2reco.cfg"
#define ARM_FILE  "ur0:/tai/ds4v2reco.on"

#define DEFAUT_MAC1 0x0000C822
#define DEFAUT_MAC0 0xB26FBCD3

#define EVT_INQUIRY_RESULT     0x01
#define EVT_INQUIRY_FIN        0x02
#define EVT_LINK_KEY           0x04
#define EVT_CONNEXION_ACCEPTEE 0x05
#define EVT_DECONNEXION        0x06
#define EVT_CONNEXION_DEMANDEE 0x08

#define PERIODE_MS   20
#define LOG_BUF_SIZE (32 * 1024)

static char log_buf[LOG_BUF_SIZE];
static int  log_pos = 0, log_entries = 0, log_reentry = 0, log_dropped = 0;

static unsigned int cible_mac0 = DEFAUT_MAC0, cible_mac1 = DEFAUT_MAC1;
static int cfg_delay = 30, cfg_mode = 1, cfg_essais = 3;

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

	if (log_entries >= 1500) {
		log_dropped++;
		log_reentry = 0;
		return;
	}

	{
		char h[16];
		int hn = snprintf(h, sizeof(h), "%7d ", millis());
		if (hn > 0 && log_pos + hn < LOG_BUF_SIZE) {
			memcpy(log_buf + log_pos, h, hn);
			log_pos += hn;
		}
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

/* --- Configuration -------------------------------------------------------- */

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

static void cfg_mac(const char *buf, int len)
{
	const char *cle = "mac=";
	for (int i = 0; i + 4 < len; i++) {
		if (buf[i] != cle[0] || buf[i+1] != cle[1] ||
		    buf[i+2] != cle[2] || buf[i+3] != cle[3])
			continue;
		unsigned long long v = 0;
		int d = 0;
		for (; d < 12 && i + 4 + d < len; d++) {
			char c = buf[i + 4 + d];
			int x;
			if (c >= '0' && c <= '9')      x = c - '0';
			else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
			else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
			else break;
			v = (v << 4) | (unsigned)x;
		}
		if (d != 12)
			return;
		cible_mac1 = (unsigned int)((v >> 32) & 0xFFFF);
		cible_mac0 = (unsigned int)(v & 0xFFFFFFFF);
		return;
	}
}

static void cfg_load(void)
{
	char buf[160];
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
	if (v >= 10) cfg_delay = v;
	v = cfg_entier(buf, n, "mode=");
	if (v >= 0 && v <= 5) cfg_mode = v;
	v = cfg_entier(buf, n, "essais=");
	if (v >= 1 && v <= 60) cfg_essais = v;
}

/* --- Armement a usage unique ---------------------------------------------- */

static int consommer_armement(void)
{
	SceUID fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd < 0) {
		TRACE("[arme] %s absent : desarme, aucune action.\n", ARM_FILE);
		return 0;
	}
	ksceIoClose(fd);

	int res = ksceIoRemove(ARM_FILE);
	TRACE("[arme] armement consomme -> 0x%08X\n", res);
	if (res < 0) {
		TRACE("[arme] !! suppression impossible : essai ANNULE\n");
		return 0;
	}
	fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
		TRACE("[arme] !! toujours present : essai ANNULE\n");
		return 0;
	}
	return 1;
}

/* --- Etat ----------------------------------------------------------------- */

static SceUID id_callback = -1, thread_uid = -1;
static int doit_tourner = 1;

static int essais_faits = 0;
static int connect_planifie = 0;      /* echeance en ms, 0 = rien */
static int attente_fin_recherche = 0; /* mode 3 : on attend l'evenement 0x02 */

static int est_la_cible(unsigned int mac0, unsigned int mac1)
{
	return mac0 == cible_mac0 && mac1 == cible_mac1;
}

static void tenter_connexion(const char *pourquoi)
{
	if (essais_faits >= cfg_essais) {
		LOG("[reco] plafond de %d essais atteint, on s'arrete\n", cfg_essais);
		return;
	}
	essais_faits++;
	int r = ksceBtStartConnect(cible_mac0, cible_mac1);
	LOG("[reco] StartConnect (%s, essai %d/%d) -> 0x%08X%s\n",
	    pourquoi, essais_faits, cfg_essais, r,
	    r == 0 ? "  ACCEPTE" : "");
	if (r == (int)SCE_BT_ERROR_CONNECT_START_BUSY)
		LOG("[reco]   = CONNECT_START_BUSY : une recherche est en cours\n");
	else if (r == (int)SCE_BT_ERROR_CONNECT_START_CONNECTED)
		LOG("[reco]   = CONNECT_START_CONNECTED : lien deja etabli\n");
	else if (r == (int)SCE_BT_ERROR_CONNECT_START_NO_REG)
		LOG("[reco]   = NO_REG : la manette n'est pas appairee\n");
}

/* --- Traitement des evenements -------------------------------------------- */

static void traiter(const SceBtEvent *ev)
{
	int cible = est_la_cible(ev->mac0, ev->mac1);

	/* Fin de recherche : c'est ici que ds34vita declenche sa connexion. */
	if (ev->id == EVT_INQUIRY_FIN && attente_fin_recherche) {
		attente_fin_recherche = 0;
		LOG("[evt] 0x02 fin de recherche\n");
		tenter_connexion("apres recherche");
		return;
	}

	if (!cible) {
		static unsigned int autres = 0;
		if (++autres <= 6)
			LOG("[autre] id=0x%02X %08X:%08X\n", ev->id, ev->mac0, ev->mac1);
		return;
	}

	/* Le flux d'entree, s'il s'etablit, arrive par centaines. */
	if (ev->id == 0x0A) {
		static unsigned int entrees = 0;
		if (++entrees <= 3)
			LOG("[evt] 0x0A rapport d'entree n=%u  *** LE FLUX EST ETABLI ***\n",
			    entrees);
		return;
	}

	LOG("[evt] id=0x%02X  etat %d\n", ev->id,
	    ksceBtGetConnectingInfo(ev->mac0, ev->mac1));

	switch (ev->id) {

	case EVT_CONNEXION_DEMANDEE:
		/*
		 * La manette appelle. C'est le chemin qui echoue systematiquement --
		 * 5,6 s d'attente puis un 0x05 degrade. On tente autre chose.
		 */
		switch (cfg_mode) {
		case 1:
			tenter_connexion("sur 0x08");
			break;
		case 2: {
			int r = ksceBtStartDisconnect(cible_mac0, cible_mac1);
			LOG("[reco] StartDisconnect -> 0x%08X, connexion dans 200 ms\n", r);
			connect_planifie = millis() + 200;
			break;
		}
		case 3: {
			int r = ksceBtStartInquiry();
			LOG("[reco] StartInquiry -> 0x%08X, attente de la fin\n", r);
			attente_fin_recherche = (r >= 0);
			break;
		}
		default:
			LOG("[reco] mode=%d : on laisse faire\n", cfg_mode);
			break;
		}
		break;

	case EVT_DECONNEXION:
		if (cfg_mode == 4) {
			LOG("[reco] echec natif constate, connexion dans 500 ms\n");
			connect_planifie = millis() + 500;
		}
		break;

	default:
		break;
	}
}

/* --- Observation des transferts HID --------------------------------------- */

/*
 * Hook sur l'import `ksceBtHidTransfer` de SceDs3 -- par NID, donc sans offset
 * et portable.
 *
 * Pourquoi il est de retour : l'utilisateur voit la manette **changer de
 * couleur et vibrer**, ce qui ne peut venir que d'un rapport de sortie `0x11`
 * effectivement recu. Or le journal des evenements ne montrait aucun `0x0B`
 * (acquittement d'ecriture). Les deux ne peuvent pas etre vrais en meme temps :
 * soit un rapport part sans etre acquitte, soit il vient d'ailleurs. Ce hook
 * tranche.
 */
#define MOD_DS3           "SceDs3"
#define LIB_SCEBT_DRIVER  0xD48CA62D
#define NID_HID_TRANSFER  0xF9DCEC77

static tai_hook_ref_t ref_hid;
static SceUID uid_hid = -1;

static int hook_hid(unsigned int mac0, unsigned int mac1, SceBtHidRequest *req)
{
	unsigned char type = 0, id = 0;
	unsigned int len = 0;

	if (req) {
		type = req->type;
		id = req->unk09;
		len = req->length;
	}

	int ret = TAI_CONTINUE(int, ref_hid, mac0, mac1, req);

	if (est_la_cible(mac0, mac1)) {
		static unsigned int n = 0;
		if (++n <= 24)
			LOG("[hid] type=%u report=0x%02X len=%u -> 0x%08X\n",
			    type, id, len, ret);
	}
	return ret;
}

/* --- Thread --------------------------------------------------------------- */

static int callback_bt(int a, int b, int c, void *d)
{
	(void)a; (void)b; (void)c; (void)d;
	return 0;
}

static int thread_principal(SceSize args, void *argp)
{
	(void)args; (void)argp;

	ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);
	t_origine = ksceKernelGetSystemTimeWide();

	TRACE("=== ds4v2reco : reveil ; horloge en ms a gauche ===\n");
	TRACE("cible %08X:%08X  mode=%d  essais max=%d\n",
	      cible_mac0, cible_mac1, cfg_mode, cfg_essais);

	if (!consommer_armement())
		return 0;

	/* Le callback appartient au thread qui l'enregistre : SceBt ne delivre
	 * ses evenements qu'a son proprietaire. */
	id_callback = ksceKernelCreateCallback("ds4v2reco_cb", 0, callback_bt, NULL);
	if (id_callback < 0) {
		TRACE("[thread] CreateCallback 0x%08X -- abandon\n", id_callback);
		return 0;
	}
	TRACE("[thread] RegisterCallback -> 0x%08X\n",
	      ksceBtRegisterCallback(id_callback, 0, 0xFFFFFFFF, 0xFFFFFFFF));

	switch (cfg_mode) {
	case 0: TRACE("[mode 0] observation seule.\n"); break;
	case 1: TRACE("[mode 1] StartConnect des que la manette appelle.\n"); break;
	case 2: TRACE("[mode 2] StartDisconnect puis StartConnect.\n"); break;
	case 3: TRACE("[mode 3] recherche puis StartConnect (voie ds34vita).\n"); break;
	case 4: TRACE("[mode 4] laisser echouer, puis StartConnect.\n"); break;
	case 5: TRACE("[mode 5] StartConnect periodique, independamment des evenements.\n"); break;
	}

	uid_hid = taiHookFunctionImportForKernel(KERNEL_PID, &ref_hid,
		MOD_DS3, LIB_SCEBT_DRIVER, NID_HID_TRANSFER, hook_hid);
	TRACE("[hid] hook sur SceDs3->ksceBtHidTransfer -> 0x%08X\n", uid_hid);

	TRACE("[init] en attente de la manette (bouton PS)\n");

	/*
	 * Mode 5 : un premier StartConnect A FROID, avant toute activite de la
	 * manette.
	 *
	 * C'est la question qu'aucun essai n'a posee proprement. Les trois refus
	 * `NOT_CONNECTABLE` sont tous survenus alors qu'une tentative entrante etait
	 * en vol -- la manette repage toutes les ~80 ms, il n'y a jamais de fenetre
	 * calme une fois qu'elle est allumee. Ici la manette est encore eteinte :
	 * si le refus persiste, c'est une propriete de l'enregistrement et non un
	 * conflit, et toute la piste « faire initier la console » tombe.
	 */
	if (cfg_mode == 5) {
		int r = ksceBtStartConnect(cible_mac0, cible_mac1);
		TRACE("[reco] StartConnect A FROID (manette eteinte) -> 0x%08X\n", r);
		TRACE("[reco] allumez la manette maintenant ; nouvel essai toutes les 2 s\n");
	}

	int tours = 0;
	int tours_periodique = 0;

	while (doit_tourner) {
		ksceKernelCheckCallback();

		for (int drain = 0; drain < 16; drain++) {
			SceBtEvent ev;
			memset(&ev, 0, sizeof(ev));
			int res = ksceBtReadEvent(&ev, 1);
			if (res == (int)SCE_BT_ERROR_CB_OVERFLOW)
				continue;
			if (res < 0 || (ev.id == 0 && ev.mac0 == 0 && ev.mac1 == 0))
				break;
			traiter(&ev);
		}

		if (connect_planifie && millis() >= connect_planifie) {
			connect_planifie = 0;
			tenter_connexion("differe");
		}

		/* Mode 5 : rythme fixe, sans attendre le moindre evenement. */
		if (cfg_mode == 5 && ++tours_periodique >= 100) {
			tours_periodique = 0;
			tenter_connexion("periodique");
		}

		/* Vidage au repos seulement : une ecriture coute ~25 ms. */
		if (++tours >= 10) {
			tours = 0;
			log_flush();
		}

		ksceKernelDelayThread(PERIODE_MS * 1000);
	}

	if (id_callback >= 0) {
		ksceBtUnregisterCallback(id_callback);
		ksceKernelDeleteCallback(id_callback);
		id_callback = -1;
	}
	TRACE("=== fin ===\n");
	return 0;
}

/* --- Points d'entree ------------------------------------------------------ */

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	cfg_load();
	ksceIoMkdir(LOG_DIR, 0777);
	LOG("\n===== ds4v2reco v1.0 =====\n");
	LOG("cible %08X:%08X delay=%ds mode=%d essais=%d\n",
	    cible_mac0, cible_mac1, cfg_delay, cfg_mode, cfg_essais);
	log_flush();

	thread_uid = ksceKernelCreateThread("ds4v2reco", thread_principal,
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
	(void)argc; (void)args;
	doit_tourner = 0;
	if (thread_uid >= 0) {
		ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
		ksceKernelDeleteThread(thread_uid);
		thread_uid = -1;
	}
	LOG("arret (%d lignes perdues)\n", log_dropped);
	log_flush();
	return SCE_KERNEL_STOP_SUCCESS;
}
