/*
 * ds4v2fix - sonde et correctifs PS TV pour la DualShock 4 v2 (PID 0x09CC)
 *
 * Objectif de cette version : trouver OU la PS TV range l'appairage d'une
 * manette. Methode differentielle - on observe une v1, qui se reconnecte seule
 * au demarrage, donc dont l'appairage est forcement persiste quelque part ;
 * puis on refait la meme mesure avec une v2 et on compare.
 *
 * TROIS REGLES DE SURETE, apprises d'un boot loop provoque le 2026-07-31 :
 *
 *  1. Aucune I/O dans un hook. Les hooks n'ecrivent que dans un tampon en RAM.
 *     La version precedente appelait ksceIoOpen() depuis un hook pose sur
 *     SceBt -> SceRegMgr ; comme ecrire un fichier consulte le registre, chaque
 *     log rappelait le hook : recursion infinie et console non bootable.
 *
 *  2. Les hooks sont poses par le thread differe, pas par module_start().
 *     Le demarrage reste donc toujours sain : si un hook fait planter la
 *     console, cela arrive apres que le reseau soit monte, et l'acces FTP
 *     reste disponible pour retirer le plugin.
 *
 *  3. Garde-fou de re-entrance sur le logger, et nombre d'entrees borne.
 *
 * Options : ur0:/tai/ds4v2fix.cfg (facultatif)
 *     spoof=0 block=0 rescue=0 replay=0 delay=30
 *   spoof : 1 = ksceBtGetVidPid() renvoie 0x05C4 au lieu de 0x09CC
 *   block : 0 = ne bloque rien, 1 = protege les MAC v2 de la suppression,
 *           2 = bloque toute suppression
 *   delay : secondes avant la pose des hooks (defaut 30, minimum 10)
 *
 * Journal : ur0:/log/ds4v2fix.txt
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define DS4_VID    0x054C
#define DS4_V1_PID 0x05C4
#define DS4_V2_PID 0x09CC

/* MAC interne de la V2 testee (C8:22:B2:6F:BC:D3). */
#define TEST_V2_MAC0 0xB26FBCD3
#define TEST_V2_MAC1 0x0000C822

#define LOG_DIR  "ur0:/log"
#define LOG_FILE LOG_DIR "/ds4v2fix.txt"
#define CFG_FILE "ur0:/tai/ds4v2fix.cfg"

/* Exports SceBtForDriver */
#define NID_GET_VID_PID       0x780A5557
#define NID_DELETE_REGISTERED 0xE6F659E0
#define NID_GET_REGISTERED    0xF86D25E2
#define NID_START_CONNECT     0x6059113A
#define NID_START_DISCONNECT  0x50710281
#define NID_HID_TRANSFER      0xF9DCEC77
#define NID_READ_EVENT        0x5ABB9A9D

/* SceRegMgrForDriver : ou l'appairage est peut-etre persiste */
#define LIB_REGMGR           0xB2223AEB
#define NID_REG_GET_KEY_BIN  0x0B98D646
#define NID_REG_SET_KEY_BIN  0x566A1793
#define NID_REG_GET_KEY_INT  0x16DDF3DC
#define NID_REG_SET_KEY_INT  0xD72EA399

#define LOG_BUF_SIZE    (32 * 1024)
#define LOG_MAX_ENTRIES 900

static char log_buf[LOG_BUF_SIZE];
static int  log_pos = 0;
static int  log_entries = 0;
static int  log_reentry = 0;
static int  log_dropped = 0;

static int cfg_spoof = 0;
static int cfg_block = 0;
static int cfg_delay = 30;
static int cfg_rescue = 0;
static int cfg_replay = 0;

#define V2_MAX 4
static unsigned int v2_mac0[V2_MAX];
static unsigned int v2_mac1[V2_MAX];
static int v2_count = 0;

static SceUID probe_thread_uid = -1;
static int stop_requested = 0;
static void *last_v2_feature_buffer = NULL;
static unsigned int last_v2_feature_length = 0;
static int v2_feature_dumped = 0;
static int v2_replay_pending = 0;

/* Réponse capturée sur la V2 après une association réussie. */
static const unsigned char v2_feature_response[53] = {
	0x06, 0x4D, 0x61, 0x79, 0x20, 0x31, 0x37, 0x20,
	0x32, 0x30, 0x31, 0x36, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x30, 0x36, 0x3A, 0x33, 0x36, 0x3A, 0x32,
	0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x04, 0x64, 0x01, 0x00, 0x00,
	0x00, 0x08, 0x70, 0x00, 0x02, 0x00, 0x80, 0x03,
	0x00, 0x5A, 0xD7, 0xEE, 0xCA
};

static int is_test_v2(unsigned int mac0, unsigned int mac1)
{
	return mac0 == TEST_V2_MAC0 && mac1 == TEST_V2_MAC1;
}

/*
 * Experience experimentale : rapport de sortie DS4 Bluetooth long (0x11).
 * Les buffers sont statiques car cette fonction peut etre appelee depuis un
 * hook ; aucune I/O ni allocation ne doit se produire ici.
 */
static int send_test_v2_long_report(unsigned int mac0, unsigned int mac1)
{
	static SceBtHidRequest request;
	static unsigned char buffer[76];

	memset(&request, 0, sizeof(request));
	memset(buffer, 0, sizeof(buffer));
	buffer[0] = 0x11;
	buffer[1] = 0x03;
	buffer[6] = 0x0D;
	request.type = 1;
	request.buffer = buffer;
	request.length = sizeof(buffer);
	request.next = &request;

	return ksceBtHidTransfer(mac0, mac1, &request);
}

/*
 * Journalisation utilisable depuis un hook : formatage sur la pile puis copie
 * en RAM. Aucun acces disque, aucune allocation.
 */
static void LOG(const char *fmt, ...)
{
	char tmp[224];
	va_list ap;
	int n;

	if (log_reentry)
		return;
	log_reentry = 1;

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

/* Ecriture disque : reservee au thread, jamais appelee depuis un hook. */
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

static int is_known_v2(unsigned int mac0, unsigned int mac1)
{
	for (int i = 0; i < v2_count; i++) {
		if (v2_mac0[i] == mac0 && v2_mac1[i] == mac1)
			return 1;
	}
	return 0;
}

static void remember_v2(unsigned int mac0, unsigned int mac1)
{
	if (is_known_v2(mac0, mac1) || v2_count >= V2_MAX)
		return;
	v2_mac0[v2_count] = mac0;
	v2_mac1[v2_count] = mac1;
	v2_count++;
}

#define DECL_HOOK(name, ...) \
	static tai_hook_ref_t name##_ref; \
	static SceUID name##_uid = -1; \
	static int name##_func(__VA_ARGS__)

#define BIND_EXPORT(name, module, nid) \
	do { \
		name##_uid = taiHookFunctionExportForKernel(KERNEL_PID, \
			&name##_ref, (module), TAI_ANY_LIBRARY, (nid), name##_func); \
		LOG("  export %-22s %s (0x%08X)\n", #name, \
			name##_uid < 0 ? "KO" : "OK", name##_uid); \
	} while (0)

#define BIND_IMPORT(name, module, lib, nid) \
	do { \
		name##_uid = taiHookFunctionImportForKernel(KERNEL_PID, \
			&name##_ref, (module), (lib), (nid), name##_func); \
		LOG("  import %-22s %s (0x%08X)\n", #name, \
			name##_uid < 0 ? "KO" : "OK", name##_uid); \
	} while (0)

#define UNBIND(name) \
	do { \
		if (name##_uid >= 0) { \
			taiHookReleaseForKernel(name##_uid, name##_ref); \
			name##_uid = -1; \
		} \
	} while (0)

DECL_HOOK(h_get_vid_pid, unsigned int mac0, unsigned int mac1, unsigned short vid_pid[2])
{
	int ret = TAI_CONTINUE(int, h_get_vid_pid_ref, mac0, mac1, vid_pid);

	if (ret < 0 || vid_pid == NULL)
		return ret;

	if (vid_pid[0] == DS4_VID && vid_pid[1] == DS4_V2_PID) {
		remember_v2(mac0, mac1);
		if (cfg_spoof) {
			vid_pid[1] = DS4_V1_PID;
			LOG("GetVidPid %08X:%08X 054C:09CC -> spoofe 05C4\n", mac0, mac1);
			return ret;
		}
	}

	LOG("GetVidPid %08X:%08X = %04X:%04X\n", mac0, mac1, vid_pid[0], vid_pid[1]);
	return ret;
}

DECL_HOOK(h_delete_registered, unsigned int mac0, unsigned int mac1)
{
	int known = is_known_v2(mac0, mac1);

	if (cfg_block == 2 || (cfg_block == 1 && known)) {
		LOG("DeleteRegisteredInfo %08X:%08X -> BLOQUE\n", mac0, mac1);
		return 0;
	}

	LOG("DeleteRegisteredInfo %08X:%08X (v2=%d)\n", mac0, mac1, known);
	return TAI_CONTINUE(int, h_delete_registered_ref, mac0, mac1);
}

DECL_HOOK(h_get_registered, int device, int unk, SceBtRegisteredInfo *info, SceSize info_size)
{
	int ret = TAI_CONTINUE(int, h_get_registered_ref, device, unk, info, info_size);

	if (ret >= 0 && info != NULL && info_size >= sizeof(SceBtRegisteredInfo) && info->mac[0]) {
		LOG("GetRegisteredInfo dev=%d %02X:%02X:%02X:%02X:%02X:%02X %04X:%04X \"%s\"\n",
			device, info->mac[0], info->mac[1], info->mac[2],
			info->mac[3], info->mac[4], info->mac[5],
			info->vid, info->pid, info->name);
	}
	return ret;
}

/*
 * La question du moment : qui coupe la connexion ? Ces trois hooks tracent le
 * cycle de vie du lien. Un StartDisconnect juste avant l'extinction du voyant
 * designe l'appelant ; son absence signifierait que la coupure vient d'ailleurs
 * (interne a SceBt, ou de la manette elle-meme).
 */
DECL_HOOK(h_start_connect, unsigned int mac0, unsigned int mac1)
{
	LOG("StartConnect %08X:%08X\n", mac0, mac1);
	return TAI_CONTINUE(int, h_start_connect_ref, mac0, mac1);
}

DECL_HOOK(h_start_disconnect, unsigned int mac0, unsigned int mac1)
{
	LOG("*** StartDisconnect %08X:%08X ***\n", mac0, mac1);
	return TAI_CONTINUE(int, h_start_disconnect_ref, mac0, mac1);
}

DECL_HOOK(h_hid_transfer, unsigned int mac0, unsigned int mac1, SceBtHidRequest *req)
{
	if (req != NULL) {
		/*
		 * Les rapports d'entree arrivent en boucle (type 0) et la V1
		 * reconnectee peut saturer le journal avant que la V2 soit testee.
		 * Ils ne sont pas necessaires pour comparer les sequences
		 * d'initialisation : conserver seulement les transferts de controle.
		 */
		if (req->type == 0)
			return TAI_CONTINUE(int, h_hid_transfer_ref, mac0, mac1, req);

		if (cfg_rescue && is_test_v2(mac0, mac1) &&
			req->type == 2 && req->length == 53 && req->unk09 == 0x06) {
			int rescue_ret = send_test_v2_long_report(mac0, mac1);
			LOG("RESCUE V2 apres 0x06 : long 0x11 -> 0x%08X\n", rescue_ret);
		}

		if (is_test_v2(mac0, mac1) && req->type == 2 && req->length == 53) {
			last_v2_feature_buffer = req->buffer;
			last_v2_feature_length = req->length;
			if (cfg_replay && req->unk09 == 0x06 && req->buffer != NULL) {
				memcpy(req->buffer, v2_feature_response,
					sizeof(v2_feature_response));
				v2_replay_pending = 1;
				LOG("REPLAY V2 0x06 : reponse 53 octets pre-remplie\n");
			}
		}

		/*
		 * Pour une requete de lecture feature (type 2), le buffer est le
		 * receptacle de la reponse, donc vide a l'aller : l'identifiant du
		 * report demande se trouve dans les champs d'en-tete.
		 */
		LOG("HidTransfer %08X:%08X type=%d len=%d | hdr %08X %08X %02X %02X %02X\n",
			mac0, mac1, req->type, (int)req->length,
			req->unk00, req->unk04, req->unk09, req->unk0A, req->unk0B);

		/*
		 * Les premiers octets identifient le report demande. Le type 2
		 * (feature) est celui qui nous interesse : c'est la requete de 53
		 * octets restee sans reponse qui bloque l'initialisation de la v2,
		 * la ou la v1 enchaine sur une seconde requete de 41 octets.
		 */
		if (req->buffer != NULL && req->length >= 8) {
			const unsigned char *b = (const unsigned char *)req->buffer;
			LOG("    -> %02X %02X %02X %02X %02X %02X %02X %02X\n",
				b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
		} else if (req->buffer != NULL && req->length >= 4) {
			const unsigned char *b = (const unsigned char *)req->buffer;
			LOG("    -> %02X %02X %02X %02X\n", b[0], b[1], b[2], b[3]);
		}
	}
	return TAI_CONTINUE(int, h_hid_transfer_ref, mac0, mac1, req);
}

/*
 * Le payload de reponse est rempli de facon asynchrone. On observe donc le
 * tampon memorise au moment ou SceBt livre l'evenement au consommateur.
 */
DECL_HOOK(h_read_event, SceBtEvent *events, int num_events)
{
	int ret = TAI_CONTINUE(int, h_read_event_ref, events, num_events);

	/*
	 * Mode expérimental : si SceBt n'a fourni aucun événement pour le 0x06,
	 * présenter au consommateur l'événement de fin de transfert attendu.
	 * Le mode est désactivé par défaut et limité à la MAC de test.
	 */
	if (cfg_replay && v2_replay_pending && ret == 0 &&
		events != NULL && num_events > 0) {
		memset(&events[0], 0, sizeof(events[0]));
		events[0].id = 0x0A;
		events[0].mac0 = TEST_V2_MAC0;
		events[0].mac1 = TEST_V2_MAC1;
		v2_replay_pending = 0;
		LOG("REPLAY V2 : evenement 0x0A synthetique\n");
		ret = 1;
	}

	if (ret > 0 && events != NULL) {
		for (int i = 0; i < ret && i < num_events; i++) {
			if (events[i].mac0 == TEST_V2_MAC0 &&
				events[i].mac1 == TEST_V2_MAC1) {
				LOG("ReadEvent V2 id=0x%02X ret=%d\n", events[i].id, ret);
				if (events[i].id == 0x0A && last_v2_feature_buffer != NULL &&
					last_v2_feature_length >= 8) {
					const unsigned char *b = (const unsigned char *)last_v2_feature_buffer;
					LOG("    V2 response: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
					if (!v2_feature_dumped && last_v2_feature_length == 53) {
						v2_feature_dumped = 1;
						for (int off = 0; off < 53; off += 16) {
							LOG("    V2 feature +%02X:", off);
							for (int j = off; j < off + 16 && j < 53; j++)
								LOG(" %02X", b[j]);
							LOG("\n");
						}
					}
				}
			}
		}
	}

	return ret;
}

/* Ce que SceBt demande au registre : la cle nous dira ou vit l'appairage. */
DECL_HOOK(h_reg_set_bin, const char *category, const char *name, void *buf, SceSize size)
{
	LOG("REG SET BIN %s/%s (%d o)\n", category, name, (int)size);
	return TAI_CONTINUE(int, h_reg_set_bin_ref, category, name, buf, size);
}

DECL_HOOK(h_reg_get_bin, const char *category, const char *name, void *buf, SceSize size)
{
	int ret = TAI_CONTINUE(int, h_reg_get_bin_ref, category, name, buf, size);
	LOG("REG GET BIN %s/%s (%d o) -> 0x%08X\n", category, name, (int)size, ret);
	return ret;
}

DECL_HOOK(h_reg_set_int, const char *category, const char *name, int value)
{
	LOG("REG SET INT %s/%s = %d\n", category, name, value);
	return TAI_CONTINUE(int, h_reg_set_int_ref, category, name, value);
}

DECL_HOOK(h_reg_get_int, const char *category, const char *name, int *value)
{
	int ret = TAI_CONTINUE(int, h_reg_get_int_ref, category, name, value);
	LOG("REG GET INT %s/%s -> 0x%08X (%d)\n", category, name, ret,
		(ret >= 0 && value) ? *value : -1);
	return ret;
}

static void bind_all_hooks(void)
{
	LOG("--- pose des hooks ---\n");

	BIND_EXPORT(h_get_vid_pid,       "SceBt", NID_GET_VID_PID);
	BIND_EXPORT(h_delete_registered, "SceBt", NID_DELETE_REGISTERED);
	BIND_EXPORT(h_get_registered,    "SceBt", NID_GET_REGISTERED);
	BIND_EXPORT(h_start_connect,     "SceBt", NID_START_CONNECT);
	BIND_EXPORT(h_start_disconnect,  "SceBt", NID_START_DISCONNECT);
	BIND_EXPORT(h_hid_transfer,      "SceBt", NID_HID_TRANSFER);
	BIND_EXPORT(h_read_event,        "SceBt", NID_READ_EVENT);

	BIND_IMPORT(h_reg_set_bin, "SceBt", LIB_REGMGR, NID_REG_SET_KEY_BIN);
	BIND_IMPORT(h_reg_get_bin, "SceBt", LIB_REGMGR, NID_REG_GET_KEY_BIN);
	BIND_IMPORT(h_reg_set_int, "SceBt", LIB_REGMGR, NID_REG_SET_KEY_INT);
	BIND_IMPORT(h_reg_get_int, "SceBt", LIB_REGMGR, NID_REG_GET_KEY_INT);

	LOG("--- hooks poses, observation en cours ---\n");
}

static void unbind_all_hooks(void)
{
	UNBIND(h_reg_get_int);
	UNBIND(h_reg_set_int);
	UNBIND(h_reg_get_bin);
	UNBIND(h_reg_set_bin);
	UNBIND(h_hid_transfer);
	UNBIND(h_read_event);
	UNBIND(h_start_disconnect);
	UNBIND(h_start_connect);
	UNBIND(h_get_registered);
	UNBIND(h_delete_registered);
	UNBIND(h_get_vid_pid);
}

static void dump_registered(void)
{
	SceBtRegisteredInfo info;

	LOG("--- appairages enregistres ---\n");
	for (int dev = 0; dev < 8; dev++) {
		memset(&info, 0, sizeof(info));
		int ret = ksceBtGetRegisteredInfo(dev, 0, &info, sizeof(info));
		if (ret < 0 || info.mac[0] == 0)
			continue;

		LOG("  dev=%d %02X:%02X:%02X:%02X:%02X:%02X %04X:%04X \"%s\"%s\n",
			dev, info.mac[0], info.mac[1], info.mac[2],
			info.mac[3], info.mac[4], info.mac[5],
			info.vid, info.pid, info.name,
			(info.vid == DS4_VID && info.pid == DS4_V2_PID) ? "  <== v2" :
			(info.vid == DS4_VID && info.pid == DS4_V1_PID) ? "  <== v1" : "");
	}
	LOG("--- fin ---\n");
}

/*
 * Le demarrage est laisse totalement intact : on attend que le systeme soit
 * lance et le reseau disponible avant de poser quoi que ce soit.
 */
static int probe_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);

	bind_all_hooks();
	log_flush();

	dump_registered();
	log_flush();

	while (!stop_requested) {
		ksceKernelDelayThread(5 * 1000 * 1000);
		log_flush();
	}

	log_flush();
	return 0;
}

static int cfg_digit(const char *buf, int len, const char *key)
{
	int klen = 0;
	while (key[klen])
		klen++;

	for (int i = 0; i + klen < len; i++) {
		int j = 0;
		while (j < klen && buf[i + j] == key[j])
			j++;
		if (j == klen) {
			int v = 0, d = 0;
			while (i + klen + d < len && buf[i + klen + d] >= '0' && buf[i + klen + d] <= '9') {
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
	char buf[128];
	int n, v;

	SceUID fd = ksceIoOpen(CFG_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	n = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';

	v = cfg_digit(buf, n, "spoof=");
	if (v == 0 || v == 1)
		cfg_spoof = v;
	v = cfg_digit(buf, n, "block=");
	if (v >= 0 && v <= 2)
		cfg_block = v;
	v = cfg_digit(buf, n, "delay=");
	if (v >= 10)
		cfg_delay = v;
	v = cfg_digit(buf, n, "rescue=");
	if (v == 0 || v == 1)
		cfg_rescue = v;
	v = cfg_digit(buf, n, "replay=");
	if (v == 0 || v == 1)
		cfg_replay = v;
}

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	cfg_load();

	/*
	 * Seule I/O du chemin de demarrage, et elle a fait ses preuves : aucun
	 * hook n'est encore pose, donc rien ne peut boucler.
	 */
	ksceIoMkdir(LOG_DIR, 0777);
	LOG("\n===== ds4v2fix =====\n");
	LOG("spoof=%d block=%d rescue=%d replay=%d delay=%ds\n",
		cfg_spoof, cfg_block, cfg_rescue, cfg_replay, cfg_delay);
	LOG("hooks poses dans %d s (demarrage laisse intact)\n", cfg_delay);
	log_flush();

	probe_thread_uid = ksceKernelCreateThread("ds4v2fix_probe", probe_thread,
		0x3C, 0x4000, 0, 0x10000, 0);
	if (probe_thread_uid < 0) {
		LOG("thread KO 0x%08X\n", probe_thread_uid);
		log_flush();
		return SCE_KERNEL_START_SUCCESS;
	}
	ksceKernelStartThread(probe_thread_uid, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	stop_requested = 1;

	if (probe_thread_uid >= 0) {
		ksceKernelWaitThreadEnd(probe_thread_uid, NULL, NULL);
		ksceKernelDeleteThread(probe_thread_uid);
		probe_thread_uid = -1;
	}

	unbind_all_hooks();

	LOG("arret (%d entrees perdues)\n", log_dropped);
	log_flush();

	return SCE_KERNEL_STOP_SUCCESS;
}
