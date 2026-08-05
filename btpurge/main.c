/*
 * btpurge — suppression ciblee d'une fiche de la table d'appairage Bluetooth
 *
 * Motif : la table de la PS TV contient une entree parasite
 *   MAC F8:6B:14:C3:62:24 — VID:PID 0000:0000 — link key entierement a zero
 * Un enregistrement malforme qu'un parcours de table de SceBt pourrait mal
 * digerer. On le retire proprement par l'API, pas en editant system.dreg :
 * SceRegMgr garde ses ecritures en RAM et ecraserait toute edition du fichier.
 *
 * Regles de surete respectees :
 *  1. Aucune I/O dans un hook — ce plugin ne hooke rien, il agit une fois
 *     depuis un thread, apres un delai laissant SceBt s'initialiser.
 *  2. Armement a usage unique : rien ne se passe sans `ur0:/tai/btpurge.on`,
 *     que le plugin supprime AVANT d'agir. Un essai qui plante laisse donc la
 *     console desarmee au demarrage suivant, et elle repart seule.
 *
 * Config  : ur0:/tai/btpurge.cfg    ex.  mac=F86B14C36224 delay=20
 * Journal : ur0:/log/btpurge.txt
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR   "ur0:/log"
#define LOG_FILE  LOG_DIR "/btpurge.txt"
#define CFG_FILE  "ur0:/tai/btpurge.cfg"
#define ARM_FILE  "ur0:/tai/btpurge.on"

/* F8:6B:14:C3:62:24 -> mac1 = deux premiers octets, mac0 = quatre derniers */
#define DEFAUT_MAC1 0x0000F86B
#define DEFAUT_MAC0 0x14C36224
#define DEFAUT_DELAY 20

static unsigned int cfg_mac0 = DEFAUT_MAC0;
static unsigned int cfg_mac1 = DEFAUT_MAC1;
static int cfg_delay = DEFAUT_DELAY;

static SceUID thread_uid = -1;

/* --- Journal -------------------------------------------------------------- */

static void TRACE(const char *fmt, ...)
{
	char tmp[224];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	SceUID fd = ksceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	int n = 0;
	while (tmp[n])
		n++;
	ksceIoWrite(fd, tmp, n);
	ksceIoClose(fd);
}

/* --- Configuration -------------------------------------------------------- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int str_len(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	return n;
}

/* cherche "cle" dans buf, rend l'index du caractere suivant, ou -1 */
static int cfg_trouve(const char *buf, int len, const char *cle)
{
	int klen = str_len(cle);
	for (int i = 0; i + klen <= len; i++) {
		int j = 0;
		while (j < klen && buf[i + j] == cle[j])
			j++;
		if (j == klen)
			return i + klen;
	}
	return -1;
}

static int cfg_entier(const char *buf, int len, const char *cle)
{
	int i = cfg_trouve(buf, len, cle);
	if (i < 0)
		return -1;
	int v = 0, vus = 0;
	while (i < len && buf[i] >= '0' && buf[i] <= '9') {
		v = v * 10 + (buf[i] - '0');
		i++;
		vus++;
	}
	return vus ? v : -1;
}

/* mac=F86B14C36224 : 12 chiffres hexa dans l'ordre d'affichage */
static void cfg_mac(const char *buf, int len)
{
	int i = cfg_trouve(buf, len, "mac=");
	if (i < 0 || i + 12 > len)
		return;

	unsigned char o[6];
	for (int k = 0; k < 6; k++) {
		int hi = hexval(buf[i + 2 * k]);
		int lo = hexval(buf[i + 2 * k + 1]);
		if (hi < 0 || lo < 0)
			return;
		o[k] = (unsigned char)((hi << 4) | lo);
	}
	cfg_mac1 = ((unsigned int)o[0] << 8) | o[1];
	cfg_mac0 = ((unsigned int)o[2] << 24) | ((unsigned int)o[3] << 16)
	         | ((unsigned int)o[4] << 8) | o[5];
}

static void cfg_load(void)
{
	char buf[160];
	SceUID fd = ksceIoOpen(CFG_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int n = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';

	cfg_mac(buf, n);
	int v = cfg_entier(buf, n, "delay=");
	if (v >= 5 && v <= 120)
		cfg_delay = v;
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

/* --- Action --------------------------------------------------------------- */

static int thread_purge(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	TRACE("[purge] attente de %d s pour laisser SceBt s'initialiser\n", cfg_delay);
	ksceKernelDelayThread((SceUInt32)cfg_delay * 1000 * 1000);

	TRACE("[purge] ksceBtDeleteRegisteredInfo(mac0=%08X, mac1=%08X)\n",
	      cfg_mac0, cfg_mac1);

	int res = ksceBtDeleteRegisteredInfo(cfg_mac0, cfg_mac1);

	TRACE("[purge] resultat -> 0x%08X %s\n", res,
	      res < 0 ? "(ECHEC)" : "(ok)");
	TRACE("[purge] termine. Redemarrer pour flusher le registre, puis dumper.\n");

	return 0;
}

/* --- Cycle de vie --------------------------------------------------------- */

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	ksceIoMkdir(LOG_DIR, 0777);
	TRACE("\n=== btpurge demarre ===\n");

	if (!consommer_armement())
		return SCE_KERNEL_START_SUCCESS;

	cfg_load();
	TRACE("[cfg] cible mac0=%08X mac1=%08X delay=%d s\n",
	      cfg_mac0, cfg_mac1, cfg_delay);

	thread_uid = ksceKernelCreateThread("btpurge", thread_purge,
	                                    0x3C, 0x2000, 0, 0x10000, 0);
	if (thread_uid < 0) {
		TRACE("[purge] !! creation du thread impossible : 0x%08X\n", thread_uid);
		return SCE_KERNEL_START_SUCCESS;
	}
	ksceKernelStartThread(thread_uid, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	if (thread_uid >= 0) {
		ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
		ksceKernelDeleteThread(thread_uid);
		thread_uid = -1;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
