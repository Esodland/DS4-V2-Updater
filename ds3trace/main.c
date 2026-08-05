/*
 * ds3trace - traceur du pilote de manette de la PS Vita / PS TV
 *
 * ========================================================================
 * CE QUE C'EST
 * ========================================================================
 * `SceDs3` est le module noyau qui pilote les manettes DualShock en
 * Bluetooth sur Vita et PS TV. Ce n'est PAS `SceBt` : SceBt est la pile
 * Bluetooth en dessous, SceDs3 est le pilote de manette au-dessus.
 *
 * SceDs3 ne se sert que de CINQ fonctions de SceBt :
 *
 *     ksceBtRegisterCallback    ksceBtReadEvent      ksceBtHidTransfer
 *     ksceBtGetRegisteredInfo   ksceBtStartDisconnect
 *
 * Ce plugin les hooke toutes les cinq, cote IMPORTS de SceDs3, et journalise
 * chaque appel horodate. La totalite du dialogue entre le pilote et la pile
 * Bluetooth passe donc par ce journal.
 *
 * ------------------------------------------------------------------------
 * POURQUOI PAR LES IMPORTS
 * ------------------------------------------------------------------------
 * Deux raisons, et la seconde est la plus importante.
 *
 * 1. Un hook pose sur un EXPORT n'intercepte que les appels venant d'autres
 *    modules -- le code interne d'un module n'appelle pas ses propres
 *    exports. Hooker `SceBt`, pile Bluetooth, ne montre donc rien de ce que
 *    SceBt fait ; hooker ce que SceDs3 lui DEMANDE montre tout.
 *
 * 2. **Un hook d'import ne depend d'aucun offset.** Un appel inter-modules
 *    passe forcement par la table d'imports, qui se resout par NID. Ce
 *    plugin fonctionne donc sur n'importe quel firmware, sans adaptation et
 *    sans reverse -- contrairement aux hooks a offsets codes en dur de
 *    ds3vita / ds4vita / ds34vita, qui cassent a chaque mise a jour.
 *
 * Si vous reprenez une seule idee de ce fichier, que ce soit celle-la.
 *
 * ------------------------------------------------------------------------
 * A QUOI CA SERT
 * ------------------------------------------------------------------------
 * A repondre, mesure a l'appui, aux questions qu'on ne peut pas trancher de
 * l'exterieur quand une manette refuse de fonctionner :
 *
 *   - quels feature reports le pilote demande, dans quel ordre, et lesquels
 *     restent sans reponse ;
 *   - combien de temps il attend avant d'abandonner ;
 *   - QUI raccroche -- le pilote, ou la manette. `ksceBtStartDisconnect`
 *     etant importe par SceDs3, ce hook repond a la question directement :
 *     s'il n'est jamais appele, la console ne rejette rien.
 *
 * La bonne façon de s'en servir est DIFFERENTIELLE : tracer une manette qui
 * fonctionne, tracer celle qui ne fonctionne pas, comparer. Une trace seule
 * se prete a toutes les interpretations ; deux traces tranchent.
 *
 * ========================================================================
 * SURETE - lire avant de modifier
 * ========================================================================
 *
 *  1. **AUCUNE I/O DANS UN HOOK.** Les hooks n'ecrivent que dans un tampon
 *     RAM ; seul le thread ecrit sur le disque. Ecrire un fichier consulte
 *     le registre, et un hook qui declenche une I/O peut se rappeler
 *     lui-meme : recursion infinie, console non bootable. C'est arrive.
 *
 *  2. **Ce plugin n'est qu'un observateur.** Il ne modifie aucun argument,
 *     aucune valeur de retour, et n'emet aucune requete. Tous les hooks
 *     rendent la main a la fonction d'origine par TAI_CONTINUE.
 *
 *  3. **Armement a usage unique.** Rien ne se passe si `ur0:/tai/ds3trace.on`
 *     est absent ; et quand il est present, il est SUPPRIME avant que le
 *     moindre hook ne soit pose. Un essai consomme son armement : s'il plante
 *     la console, le demarrage suivant la trouve desarmee et elle revient
 *     toute seule. Un boot loop est structurellement impossible.
 *
 *  4. **Les hooks sont poses par le thread differe**, jamais par
 *     module_start : le demarrage reste sain, et le FTP est monte avant que
 *     ce plugin ne touche a quoi que ce soit.
 *
 *  5. Filet ultime : maintenir **L** pendant le demarrage saute taiHEN.
 *
 * ------------------------------------------------------------------------
 * OPTIONS - ur0:/tai/ds3trace.cfg (facultatif)
 * ------------------------------------------------------------------------
 *     delay=30 duree=0 entrees=3000
 *
 *   delay    secondes avant la pose des hooks (min 10, defaut 30).
 *   duree    secondes de trace avant retrait automatique des hooks
 *            (0 = sans limite, defaut). Utile pour une capture bornee.
 *   entrees  nombre maximal de lignes journalisees (defaut 3000). Une fois
 *            atteint, les hooks restent en place mais se taisent : une
 *            manette qui fonctionne emet des centaines de rapports par
 *            seconde et noierait tout le reste.
 *
 * Armement : ur0:/tai/ds3trace.on   (fichier vide, consomme a chaque essai)
 * Journal   : ur0:/log/ds3trace.txt
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR   "ur0:/log"
#define LOG_FILE  LOG_DIR "/ds3trace.txt"
#define CFG_FILE  "ur0:/tai/ds3trace.cfg"
#define ARM_FILE  "ur0:/tai/ds3trace.on"

/* Le module a instrumenter, et la bibliotheque qu'il importe. */
#define MOD_DS3            "SceDs3"
#define LIB_SCEBT_DRIVER   0xD48CA62D

/* Les cinq seules fonctions SceBt que SceDs3 importe, relevees dans sa table
 * d'imports (ds3.elf du bootimage 3.65). */
#define NID_REGISTER_CALLBACK  0x120AC1F7
#define NID_READ_EVENT         0x5ABB9A9D
#define NID_HID_TRANSFER       0xF9DCEC77
#define NID_GET_REGISTERED     0xF86D25E2
#define NID_START_DISCONNECT   0x50710281

#define LOG_BUF_SIZE  (48 * 1024)

static char log_buf[LOG_BUF_SIZE];
static int  log_pos = 0;
static int  log_entries = 0;
static int  log_reentry = 0;
static int  log_dropped = 0;

static int cfg_delay = 30;
static int cfg_duree = 0;
static int cfg_entrees = 3000;
static int cfg_offsets = 0;   /* sondes liees au firmware, cf. plus bas */
static int cfg_clear6 = 0;    /* experience : effacer le bit 6, cf. plus bas */

static SceInt64 t_origine = 0;

static int millis(void)
{
	if (t_origine == 0)
		return 0;
	return (int)((ksceKernelGetSystemTimeWide() - t_origine) / 1000);
}

/*
 * Journalisation appelable depuis un hook : formatage sur la pile, copie en
 * RAM, rien d'autre. Aucun acces disque, aucune allocation.
 *
 * Le garde de re-entrance n'est pas decoratif : ces hooks s'executent dans le
 * contexte de la pile Bluetooth, potentiellement en parallele.
 */
static void LOG(const char *fmt, ...)
{
	char tmp[224];
	va_list ap;
	int n;

	if (log_reentry)
		return;
	log_reentry = 1;

	if (log_entries >= cfg_entrees) {
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

/* Ecriture disque : reservee au thread, JAMAIS appelee depuis un hook. */
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

/* Journalise puis vide : reservee a l'initialisation, hors de tout hook. */
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

	v = cfg_entier(buf, n, "delay=");
	if (v >= 10)
		cfg_delay = v;
	v = cfg_entier(buf, n, "duree=");
	if (v >= 0)
		cfg_duree = v;
	v = cfg_entier(buf, n, "entrees=");
	if (v >= 100)
		cfg_entrees = v;
	v = cfg_entier(buf, n, "offsets=");
	if (v == 0 || v == 1)
		cfg_offsets = v;
	v = cfg_entier(buf, n, "clear6=");
	if (v == 0 || v == 1)
		cfg_clear6 = v;
}

/* --- Armement a usage unique ---------------------------------------------- */

static int consommer_armement(void)
{
	SceUID fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd < 0) {
		TRACE("[arme] %s absent : desarme, aucun hook ne sera pose.\n", ARM_FILE);
		return 0;
	}
	ksceIoClose(fd);

	int res = ksceIoRemove(ARM_FILE);
	TRACE("[arme] armement consomme (suppression -> 0x%08X)\n", res);
	if (res < 0) {
		TRACE("[arme] !! suppression impossible : essai ANNULE, pour ne pas\n");
		TRACE("[arme]    risquer de rejouer le meme plantage au prochain boot.\n");
		return 0;
	}

	/* Verification effective plutot que confiance au code de retour. */
	fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
		TRACE("[arme] !! le fichier est toujours la : essai ANNULE\n");
		return 0;
	}
	return 1;
}

/* --- Les cinq hooks ------------------------------------------------------- */

#define DECL_HOOK(nom, ...) \
	static tai_hook_ref_t nom##_ref; \
	static SceUID nom##_uid = -1; \
	static int nom##_func(__VA_ARGS__)

/*
 * Le coeur de l'instrument.
 *
 * `SceBtHidRequest` porte l'identifiant du report demande dans `unk09`, et le
 * type de transfert dans `type` :
 *     0 = lecture      1 = ecriture (rapport de sortie)
 *     2 = feature      3 = feature (autre forme)
 *
 * ⚠️ Un code de retour a 0 ne dit QUE « requete acceptee ». Les transferts de
 * SceBt sont asynchrones : la reponse, s'il y en a une, arrive plus tard sous
 * forme d'evenement (0x0A / 0x0B / 0x0C) visible dans le hook de ReadEvent.
 * Une requete acceptee et jamais suivie de son evenement est precisement le
 * symptome qu'on cherche.
 */
DECL_HOOK(h_hid_transfer, unsigned int mac0, unsigned int mac1, SceBtHidRequest *req)
{
	unsigned char type = 0, id = 0;
	unsigned int len = 0;

	if (req) {
		type = req->type;
		id = req->unk09;
		len = req->length;
	}

	int ret = TAI_CONTINUE(int, h_hid_transfer_ref, mac0, mac1, req);

	/* Les lectures (type 0) reviennent en boucle une fois le flux etabli et
	 * noieraient le journal : on les compte au lieu de les detailler. */
	if (type == 0) {
		static unsigned int lectures = 0;
		if (++lectures <= 4 || (lectures % 200) == 0)
			LOG("HidTransfer %08X:%08X  LECTURE  len=%u -> 0x%08X  (n=%u)\n",
			    mac0, mac1, len, ret, lectures);
		return ret;
	}

	LOG("HidTransfer %08X:%08X  type=%u report=0x%02X len=%u -> 0x%08X\n",
	    mac0, mac1, type, id, len, ret);
	return ret;
}

/*
 * La vue du pilote sur les evenements de la pile.
 *
 * Table des identifiants, relevee a la mesure :
 *   0x01 resultat de recherche    0x02 fin de recherche
 *   0x04 demande de link key      0x05 connexion acceptee
 *   0x06 deconnexion              0x08 connexion demandee
 *   0x09 connexion demandee sans appairage
 *   0x0A reponse a une lecture    0x0B reponse a une ecriture
 *   0x0C reponse a un feature
 */
/*
 * Cible de la sonde d'etat, alimentee par le hook d'evenements.
 *
 * Le hook se contente de DEPOSER une adresse ; c'est le thread qui interroge
 * et journalise. Un hook ne doit rien faire de long.
 */
static volatile unsigned int sonde_mac0 = 0, sonde_mac1 = 0;
static volatile int sonde_active = 0;

DECL_HOOK(h_read_event, SceBtEvent *events, int num_events)
{
	int ret = TAI_CONTINUE(int, h_read_event_ref, events, num_events);

	if (ret > 0 && events) {
		for (int i = 0; i < ret && i < num_events; i++) {
			/*
			 * 0x08 = connexion demandee : c'est le debut de la phase qui
			 * consomme 5,4 s sur la manette defaillante contre 0,5 s sur celle
			 * qui fonctionne. On arme la sonde ici, on la coupe au 0x06.
			 */
			if (events[i].id == 0x08) {
				sonde_mac0 = events[i].mac0;
				sonde_mac1 = events[i].mac1;
				sonde_active = 1;
			} else if (events[i].id == 0x06 &&
			           events[i].mac0 == sonde_mac0) {
				sonde_active = 0;
			}

			if (events[i].id == 0x0A) {
				static unsigned int entrees = 0;
				if (++entrees <= 4 || (entrees % 200) == 0)
					LOG("ReadEvent  0x0A  %08X:%08X  (n=%u)\n",
					    events[i].mac0, events[i].mac1, entrees);
				continue;
			}
			LOG("ReadEvent  0x%02X  %08X:%08X\n",
			    events[i].id, events[i].mac0, events[i].mac1);
		}
	}
	return ret;
}

/*
 * QUI raccroche ?
 *
 * C'est la question qu'aucune observation exterieure ne tranche. SceDs3
 * important cette fonction, ce hook y repond directement : s'il ne se
 * declenche jamais alors que la manette se deconnecte, c'est que la console
 * ne rejette rien -- la manette part d'elle-meme, ou la pile coupe plus bas.
 */
DECL_HOOK(h_start_disconnect, unsigned int mac0, unsigned int mac1)
{
	LOG("*** StartDisconnect %08X:%08X  <- le PILOTE raccroche ***\n", mac0, mac1);
	return TAI_CONTINUE(int, h_start_disconnect_ref, mac0, mac1);
}

DECL_HOOK(h_register_callback, SceUID cb, int unused, int f1, int f2)
{
	int ret = TAI_CONTINUE(int, h_register_callback_ref, cb, unused, f1, f2);
	LOG("RegisterCallback cb=0x%08X flags=%08X:%08X -> 0x%08X\n", cb, f1, f2, ret);
	return ret;
}

/*
 * ⚠️ Ne PAS lire `info` ici.
 *
 * Mesure du 2026-08-03 : `ksceBtGetRegisteredInfo` ecrit **0x200 octets** alors
 * que l'en-tete de VitaSDK declare la structure a 0x100, et il ignore le
 * parametre de taille. Le tampon appartient a SceDs3 et on ne sait pas comment
 * il est dimensionne : on se contente donc des arguments et du retour.
 */
DECL_HOOK(h_get_registered, int device, int unk, void *info, SceSize size)
{
	int ret = TAI_CONTINUE(int, h_get_registered_ref, device, unk, info, size);
	LOG("GetRegisteredInfo dev=%d taille_annoncee=%u -> 0x%08X\n",
	    device, (unsigned int)size, ret);
	return ret;
}

/* --- Sondes a offsets dans SceBt (optionnelles, offsets=1) ---------------- */

/*
 * ⚠️ TOUT CE QUI SUIT EST LIE AU FIRMWARE 3.65 ET DESACTIVE PAR DEFAUT.
 *
 * Le reste de ce plugin ne depend d'aucun offset et fonctionne partout. Cette
 * section-ci est l'exception, et elle est isolee pour que ça reste vrai : avec
 * `offsets=0` (le defaut), rien de ce code ne s'execute.
 *
 * CE QU'ON CHERCHE
 *
 * `ksceBtGetConnectingInfo` ne calcule rien : il lit un mot de drapeaux a
 * `dev+4` et rend 4 si le **bit 6 (0x40)** y est pose. L'etat 4 n'est donc pas
 * une phase de l'etablissement, c'est un verdict d'echec -- et c'est celui ou
 * la manette defaillante s'arrete, apres 5,6 s, la ou celle qui fonctionne
 * passe en etat 3 en 0,5 s.
 *
 * Cinq endroits dans SceBt posent ce bit puis rangent le mot en `+4` :
 * 0xE06, 0x1E36, 0x1F9A, 0x27F6, 0x66BC. Les quatre premiers partagent une
 * signature « abandon » : poser le drapeau et armer un temporisateur de
 * 3 000 000 µs.
 *
 * On ne hooke PAS ces adresses : elles sont au milieu de fonctions, et y
 * brancher du C ecraserait des registres vivants. On hooke les **entrees des
 * fonctions qui les contiennent**, ce qui est le cas d'usage normal de
 * taiHookFunctionOffsetForKernel -- et ce que font ds3vita / ds34vita depuis
 * toujours.
 *
 * Correlation : ces hooks n'ecrivent qu'un horodatage d'entree et de sortie,
 * et la sonde d'etat a 20 ms tourne en parallele dans le meme journal. La
 * fonction dont la sortie coincide avec la bascule 2 -> 4 est la coupable.
 *
 * RISQUE RESIDUEL, ET IL EST REEL : l'arite de ces fonctions internes est
 * inconnue. On les declare a quatre arguments registres (r0-r3), ce que leurs
 * prologues rendent plausible -- aucune ne lit d'argument sur la pile avant
 * son cadre local. Si l'une en prend davantage, TAI_CONTINUE reconstruira mal
 * l'appel. C'est precisement pour ce genre d'incertitude que l'armement est a
 * usage unique : un essai rate coute un redemarrage, pas une console.
 */

/*
 * ⛔ SceBt+0x6558 A ETE RETIREE DE CETTE LISTE. Ne pas la remettre telle quelle.
 *
 * Essai du 2026-08-03 : la console a redemarre au premier appel, survenu
 * 334 ms apres le 0x08, pendant l'etablissement de la connexion. La derniere
 * ligne du journal est son entree ; la sortie n'est jamais venue.
 *
 * Cause : son arite depasse quatre. Son prologue la trahissait -- `sub sp,#212`
 * pour un cadre local enorme, et un `cmp r2,#3` qui montre au moins trois
 * arguments utiles la ou les quatre autres n'en exploitent que un ou deux. Le
 * hook l'a appelee avec quatre registres et TAI_CONTINUE a reconstruit l'appel
 * de travers.
 *
 * Second indice, dans la meme ligne : le mot de drapeaux lu a `a0` valait 0 et
 * le garde d'adresse l'a rejete. Pour cette fonction, le premier argument n'est
 * donc PAS la structure peripherique -- contrairement aux quatre autres.
 *
 * Si elle doit etre instrumentee un jour, ce sera par un trampoline en
 * assembleur qui preserve le contexte, ou en hookant son appelant. Pas par une
 * fonction C.
 */
/*
 * ⛔ LES QUATRE AUTRES FONCTIONS SONT ABANDONNEES. Ne pas les remettre.
 *
 * 0x0DDA, 0x1D50, 0x1E9C, 0x26A8 : trois essais successifs, trois plantages a
 * la connexion de la manette, et **aucune n'a jamais journalise son entree** --
 * y compris apres avoir deplace la journalisation AVANT l'appel et supprime
 * toute lecture memoire. Le crash n'est donc pas dans le corps de la sonde : il
 * vient du branchement taiHEN sur ces adresses. Continuer a raboter le corps ne
 * menait nulle part.
 *
 * Il reste 0x6558, qui elle avait au moins journalise avant de planter (arite
 * superieure a quatre).
 */
#define NB_SONDES 2

static const unsigned int sonde_offsets[NB_SONDES] = {
	0x199C8,   /* gestionnaire d'acceptation, table[1] */
	0x11544,   /* recherche centrale d'objets, cf. plus bas */
};

static tai_hook_ref_t sonde_ref[NB_SONDES];
static SceUID sonde_uid[NB_SONDES] = { -1, -1 };
static unsigned int sonde_appels[NB_SONDES];

/*
 * Corps commun. `idx` identifie la sonde ; `dev` est le premier argument, qui
 * pointe la structure peripherique dans les fonctions ou on a pu le verifier.
 *
 * Le mot de drapeaux est lu AVANT et APRES l'appel : si le bit 6 apparait
 * entre les deux, cette fonction est celle qui a rendu le verdict. C'est une
 * preuve, pas une correlation.
 *
 * Lecture defensive : `dev` n'est pas garanti pointer une structure valide
 * dans les cinq cas, donc on ne lit que s'il ressemble a une adresse noyau
 * plausible et alignee.
 */
/*
 * ⚠️ PLUS AUCUNE LECTURE MEMOIRE ICI.
 *
 * La version precedente lisait le mot de drapeaux a `a0 + 4` avant l'appel,
 * pour prouver directement que le bit 6 y apparaissait. C'etait la bonne idee
 * et la mauvaise execution : rien ne garantit que `a0` pointe une structure
 * peripherique dans les quatre fonctions, ni meme qu'il soit mappe. Un garde
 * sur la valeur (>= 0x80000000, aligne) ne prouve pas qu'une adresse est
 * lisible. C'est un candidat serieux pour le plantage du second essai, ou
 * aucune sonde n'avait rien journalise -- donc ou le crash precede
 * TAI_CONTINUE.
 *
 * On se rabat sur ce qui ne peut pas nuire : entree, sortie, horodatage. La
 * correlation avec la sonde d'etat a 20 ms, qui tourne dans le meme journal,
 * suffit a designer la fonction dont la sortie coincide avec la bascule 2 -> 4.
 *
 * ET SURTOUT : l'entree est journalisee AVANT l'appel. Un plantage a
 * l'interieur laisse donc un « -> » sans « <- » en face, ce qui nomme le
 * coupable. La version precedente journalisait apres, et n'a rien pu dire de
 * son propre crash -- exactement la meme erreur que ds4v2bt v0.1, commise deux
 * fois dans la meme journee.
 */
/*
 * Sonde sur SceBt+0x199C8 : la decision d'acceptation.
 *
 * DEUX arguments, et ce n'est pas une supposition :
 *   r0 = structure peripherique -- la fonction lit `[r0,#4]` a sa deuxieme
 *        instruction, donc cette adresse est forcement mappee et valide ;
 *   r1 = contexte -- elle lit `[r1,#4]` juste apres.
 *
 * Quatre projets independants (ds3vita, ds4vita, ds34vita, EyeToyPSVita) la
 * hookent avec une fonction C a deux arguments, sur plusieurs firmwares. C'est
 * le seul point d'accroche de SceBt dont on ait une preuve d'usage.
 *
 * Ce qu'on y lit : le mot de drapeaux, et en particulier le **bit 6 (0x40)**
 * dont `ksceBtGetConnectingInfo` fait l'etat 4. On saura enfin s'il est deja
 * pose quand la decision d'acceptation se prend.
 *
 * `clear6=1` passe de l'observation a l'experience : le bit est efface avant
 * de rendre la main. Si le verdict d'echec n'est que ce bit, la connexion doit
 * alors se poursuivre. C'est reversible, borne a la manette visee, et couvert
 * par l'armement a usage unique.
 */
static unsigned int sonde_efface = 0;

static int sonde_acceptation(unsigned int dev, unsigned int ctx)
{
	unsigned int n = ++sonde_appels[0];
	unsigned int drapeaux = 0;

	if (dev)
		drapeaux = ((volatile unsigned int *)dev)[1];   /* dev + 4 */

	if (n <= 16)
		LOG("  -> 0x199C8 (n=%u) dev=%08X ctx=%08X drapeaux=%08X bit6=%d\n",
		    n, dev, ctx, drapeaux, (drapeaux >> 6) & 1);

	if (cfg_clear6 && dev && (drapeaux & 0x40)) {
		((volatile unsigned int *)dev)[1] = drapeaux & ~0x40u;
		sonde_efface++;
		LOG("  *** bit 6 EFFACE (%08X -> %08X), tentative %u ***\n",
		    drapeaux, drapeaux & ~0x40u, sonde_efface);
	}

	int ret = TAI_CONTINUE(int, sonde_ref[0], dev, ctx);

	if (n <= 16)
		LOG("  <- 0x199C8 sortie -> 0x%08X\n", ret);

	return ret;
}

/*
 * Sonde sur SceBt+0x11544 : la recherche centrale d'objets.
 *
 * POURQUOI ELLE, ET POURQUOI ELLE EST SÛRE
 *
 * C'est le point de passage de tout SceBt : 47 sites d'appel, avec les types
 * 4, 5, 7, 20 et 36. Son prologue est propre (`push {r3,r4-r7,lr}`) et son
 * arité est donnée sans ambiguïté par ses appelants -- trois arguments, en
 * registres : `(peripherique, type, identifiant)`.
 *
 * Le risque qui a fait planter quatre fois cet apres-midi etait une arite
 * SUPERIEURE a quatre, donc des arguments sur la pile que TAI_CONTINUE ne
 * reconstruit pas. Ici l'arite est inferieure : passer quatre registres a une
 * fonction qui en lit trois est sans consequence, le quatrieme est ignore.
 *
 * CE QU'ON CHERCHE
 *
 * Le seul chemin vers le gestionnaire d'acceptation `0x199C8` passe par le
 * site 0x69C6-0x69F8, qui le fait dependre de trois conditions :
 *
 *     r0 = 0x11544(peripherique, 5, identifiant)   -- non nul
 *     r3 = r0[0x3C]                                -- table de gestionnaires
 *     r3[4]                                        -- entree 1 = 0x199C8
 *
 * La V2 n'atteint jamais ce gestionnaire. Si la recherche de type 5 rend NULL
 * pour elle et un objet pour la V1, on saura que **le contexte de type 5 n'est
 * jamais cree** -- et la question devient : qui le cree, et pourquoi pas ici.
 *
 * Filtre serre : cette fonction est appelee en permanence, journaliser tous les
 * types noierait le journal en une seconde. On ne garde que le type 5.
 */
static unsigned int lookup5_appels = 0;
static unsigned int lookup5_nuls = 0;

static int sonde_lookup(unsigned int dev, unsigned int type, unsigned int id,
                        unsigned int a3)
{
	int ret = TAI_CONTINUE(int, sonde_ref[1], dev, type, id, a3);

	/*
	 * Filtre elargi apres la mesure du 2026-08-03.
	 *
	 * Le type 5 est un **canal L2CAP** : les identifiants observes sur la V1
	 * sont 0x0041, 0x0042, 0x0043 -- des CID alloues dynamiquement a partir de
	 * 0x0040. Et pour la V2, aucune recherche de type 5 n'est jamais tentee :
	 * elle n'obtient pas un seul canal.
	 *
	 * On regarde donc ce que SceBt fait a la place. Les types 4 et 7 sont
	 * journalises avec le 5 ; les types 20 et 36 sont seulement comptes, ils
	 * sont trop frequents et noieraient le reste.
	 */
	unsigned int n = ++lookup5_appels;

	if (type == 4 || type == 5 || type == 7) {
		static unsigned int vus[8];
		unsigned int idx = type & 7;
		if (ret == 0)
			lookup5_nuls++;
		/* Plafond par type, plus tous les echecs. */
		if (++vus[idx] <= 30 || ret == 0)
			LOG("  [lookup] type=%u id=0x%04X -> %s (0x%08X)  n=%u\n",
			    type, id & 0xFFFF, ret ? "trouve" : "NULL", ret, vus[idx]);
	} else if ((n % 500) == 0) {
		LOG("  [lookup] %u appels tous types confondus, %u nuls\n",
		    n, lookup5_nuls);
	}

	return ret;
}

static void *const sonde_func[NB_SONDES] = {
	(void *)sonde_acceptation,
	(void *)sonde_lookup
};

static void poser_les_sondes(void)
{
	tai_module_info_t info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);

	int r = taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &info);
	if (r < 0) {
		TRACE("[sonde] SceBt introuvable : 0x%08X\n", r);
		return;
	}

	TRACE("[sonde] offsets=1 : sondes 3.65 dans SceBt\n");
	for (int i = 0; i < NB_SONDES; i++) {
		sonde_uid[i] = taiHookFunctionOffsetForKernel(KERNEL_PID,
			&sonde_ref[i], info.modid, 0, sonde_offsets[i], 1,
			sonde_func[i]);
		TRACE("[sonde] SceBt+0x%05X -> 0x%08X\n", sonde_offsets[i], sonde_uid[i]);
	}
}

static void retirer_les_sondes(void)
{
	for (int i = NB_SONDES - 1; i >= 0; i--) {
		if (sonde_uid[i] >= 0) {
			taiHookReleaseForKernel(sonde_uid[i], sonde_ref[i]);
			sonde_uid[i] = -1;
		}
	}
}

/* --- Pose et retrait ------------------------------------------------------ */

#define POSER(nom, nid) \
	do { \
		nom##_uid = taiHookFunctionImportForKernel(KERNEL_PID, &nom##_ref, \
			MOD_DS3, LIB_SCEBT_DRIVER, (nid), nom##_func); \
		TRACE("[hook] %-22s %s (0x%08X)\n", #nom, \
			nom##_uid < 0 ? "ECHEC" : "ok", nom##_uid); \
	} while (0)

#define RETIRER(nom) \
	do { \
		if (nom##_uid >= 0) { \
			taiHookReleaseForKernel(nom##_uid, nom##_ref); \
			nom##_uid = -1; \
		} \
	} while (0)

static void poser_les_hooks(void)
{
	TRACE("[hook] pose sur les imports de %s -> SceBtForDriver\n", MOD_DS3);
	POSER(h_register_callback, NID_REGISTER_CALLBACK);
	POSER(h_read_event,        NID_READ_EVENT);
	POSER(h_hid_transfer,      NID_HID_TRANSFER);
	POSER(h_get_registered,    NID_GET_REGISTERED);
	POSER(h_start_disconnect,  NID_START_DISCONNECT);
	TRACE("[hook] en place. Allumez la manette.\n");
}

static void retirer_les_hooks(void)
{
	RETIRER(h_start_disconnect);
	RETIRER(h_get_registered);
	RETIRER(h_hid_transfer);
	RETIRER(h_read_event);
	RETIRER(h_register_callback);
}

/* --- Thread --------------------------------------------------------------- */

static SceUID thread_uid = -1;
static int doit_tourner = 1;
static int hooks_poses = 0;

static int thread_principal(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	/* Le demarrage est laisse totalement intact. */
	ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);

	t_origine = ksceKernelGetSystemTimeWide();
	TRACE("=== ds3trace : reveil apres %d s ; horloge en ms a gauche ===\n", cfg_delay);

	if (!consommer_armement())
		return 0;

	poser_les_hooks();
	if (cfg_offsets)
		poser_les_sondes();
	hooks_poses = 1;

	/*
	 * Sonde d'etat de connexion, echantillonnee a 20 ms.
	 *
	 * `ksceBtGetConnectingInfo` rend l'etat du lien. Valeurs relevees :
	 *     1 = deconnecte    2 = en cours    4 = ?    5 = connecte
	 *
	 * L'etat 4 n'est documente nulle part et c'est justement celui ou la
	 * manette defaillante s'arrete. Echantillonner pendant l'etablissement
	 * montre la suite exacte des transitions et le temps passe dans chacune --
	 * ce qu'aucun evenement ne dit, puisque SceBt n'en emet aucun entre le
	 * 0x08 et le 0x05.
	 *
	 * Coût : un appel toutes les 20 ms pendant la connexion, et une ligne de
	 * journal uniquement quand l'etat CHANGE. Aucune I/O ici.
	 */
	int etat_precedent = -1;
	int t_entree_etat = 0;
	int tours = 0;
	int demi_secondes = 0;
	int secondes = 0;

	while (doit_tourner) {
		ksceKernelDelayThread(20 * 1000);

		if (sonde_active) {
			int e = ksceBtGetConnectingInfo(sonde_mac0, sonde_mac1);
			if (e != etat_precedent) {
				int maintenant = millis();
				if (etat_precedent >= 0)
					LOG("  etat %d -> %d   (apres %d ms dans l'etat %d)\n",
					    etat_precedent, e, maintenant - t_entree_etat,
					    etat_precedent);
				else
					LOG("  etat initial %d  %08X:%08X\n",
					    e, sonde_mac0, sonde_mac1);
				etat_precedent = e;
				t_entree_etat = maintenant;
			}
		} else if (etat_precedent >= 0) {
			LOG("  sonde arretee (dernier etat %d)\n", etat_precedent);
			etat_precedent = -1;
		}

		/*
		 * Vidage toutes les 200 ms et non plus toutes les secondes.
		 *
		 * Le plantage du second essai a emporte tout ce qui n'avait pas encore
		 * ete ecrit -- soit potentiellement une seconde entiere, c'est-a-dire
		 * precisement la partie interessante. Cinq fois plus d'ecritures ne
		 * coutent rien : elles ont lieu dans CE thread, pas dans les hooks, donc
		 * elles ne ralentissent rien de ce qu'on mesure.
		 */
		if (++tours < 10)
			continue;
		tours = 0;
		log_flush();

		if (++demi_secondes < 5)
			continue;
		demi_secondes = 0;
		secondes++;

		if (cfg_duree > 0 && secondes >= cfg_duree) {
			TRACE("[hook] duree de %d s atteinte : retrait des hooks.\n", cfg_duree);
			if (cfg_offsets)
				retirer_les_sondes();
			retirer_les_hooks();
			hooks_poses = 0;
			break;
		}
	}

	TRACE("=== fin de trace (%d lignes perdues) ===\n", log_dropped);
	return 0;
}

/* --- Points d'entree ------------------------------------------------------ */

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	cfg_load();

	ksceIoMkdir(LOG_DIR, 0777);
	LOG("\n===== ds3trace v1.0 =====\n");
	LOG("delay=%ds duree=%ds entrees=%d\n", cfg_delay, cfg_duree, cfg_entrees);
	log_flush();

	/* Pile large : plusieurs API SceBt ecrivent au-dela de la taille qu'on
	 * leur annonce, et ces hooks s'executent sur la pile de leur appelant. */
	thread_uid = ksceKernelCreateThread("ds3trace", thread_principal,
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

	if (hooks_poses) {
		if (cfg_offsets)
			retirer_les_sondes();
		retirer_les_hooks();
	}

	log_flush();
	return SCE_KERNEL_STOP_SUCCESS;
}
