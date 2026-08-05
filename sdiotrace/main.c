/*
 * sdiotrace - passive SDIO metadata probe for the PS TV Bluetooth path.
 *
 * SceWlanBt is the module immediately below SceBt.  On firmware 3.65 its
 * import table contains SceSdifForDriver:D0F78D9B.  Every static caller
 * supplies five 32-bit ABI words:
 *
 *   (SDIO context, address/command, size, buffer, mode)
 *
 * This probe records these metadata and the return code only.  It never
 * reads, writes, retains or changes the buffer, and it never performs I/O
 * from the hook.  The log is flushed by a separate thread.
 *
 * Safety properties:
 *  - The hook is only installed after a delay, never in module_start.
 *  - ur0:/tai/sdiotrace.on is consumed before installing the hook.  A reboot
 *    after an unsuccessful attempt is therefore disarmed automatically.
 *  - All arguments and the return value are passed through unchanged with
 *    TAI_CONTINUE.
 *  - The sole targeted NID and its five-word ABI were checked against every
 *    caller in the 3.65 SceWlanBt boot module before this code was enabled.
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR   "ur0:/log"
#define LOG_FILE  LOG_DIR "/sdiotrace.txt"
#define CFG_FILE  "ur0:/tai/sdiotrace.cfg"
#define ARM_FILE  "ur0:/tai/sdiotrace.on"

#define MOD_WLANBT        "SceWlanBt"
#define LIB_SDIF_DRIVER   0x96D306FA
#define NID_SDIF_XFER     0xD0F78D9B

#define LOG_BUF_SIZE (48 * 1024)

static char log_buf[LOG_BUF_SIZE];
static int log_pos;
static int log_entries;
static int log_dropped;
static int log_reentry;

static int cfg_delay = 30;
static int cfg_duree = 30;
static int cfg_entrees = 1000;
static int cfg_minlen = 8;

static SceInt64 t_origin;
static SceUID thread_uid = -1;
static volatile int keep_running = 1;
static int hook_installed;

static unsigned int xfer_seen;
static unsigned int xfer_small;

static tai_hook_ref_t xfer_ref;
static SceUID xfer_uid = -1;

static int millis(void)
{
    if (t_origin == 0)
        return 0;
    return (int)((ksceKernelGetSystemTimeWide() - t_origin) / 1000);
}

/* Hook-safe: stack formatting and RAM copying only. */
static void LOG(const char *fmt, ...)
{
    char tmp[192];
    char prefix[16];
    va_list ap;
    int n;
    int prefix_len;

    if (log_reentry)
        return;
    log_reentry = 1;

    if (log_entries >= cfg_entrees) {
        log_dropped++;
        log_reentry = 0;
        return;
    }

    prefix_len = snprintf(prefix, sizeof(prefix), "%7d ", millis());
    if (prefix_len > 0 && log_pos + prefix_len < LOG_BUF_SIZE) {
        memcpy(log_buf + log_pos, prefix, prefix_len);
        log_pos += prefix_len;
    }

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        if (n >= (int)sizeof(tmp))
            n = (int)sizeof(tmp) - 1;
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

/* Never call this function from a hook. */
static void log_flush(void)
{
    SceUID fd;

    if (log_pos <= 0)
        return;

    fd = ksceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    ksceIoWrite(fd, log_buf, log_pos);
    ksceIoClose(fd);
    log_pos = 0;
    memset(log_buf, 0, sizeof(log_buf));
}

static void TRACE(const char *fmt, ...)
{
    char tmp[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    LOG("%s", tmp);
    log_flush();
}

static int cfg_int(const char *buf, int len, const char *key)
{
    int key_len = 0;
    int i;

    while (key[key_len])
        key_len++;

    for (i = 0; i + key_len < len; i++) {
        int j = 0;
        int value = 0;
        int digits = 0;

        while (j < key_len && buf[i + j] == key[j])
            j++;
        if (j != key_len)
            continue;

        while (i + key_len + digits < len) {
            char c = buf[i + key_len + digits];
            if (c < '0' || c > '9')
                break;
            value = value * 10 + c - '0';
            digits++;
        }
        if (digits)
            return value;
    }
    return -1;
}

static void cfg_load(void)
{
    char buf[128];
    SceUID fd;
    int n;
    int value;

    fd = ksceIoOpen(CFG_FILE, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    n = ksceIoRead(fd, buf, sizeof(buf) - 1);
    ksceIoClose(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    value = cfg_int(buf, n, "delay=");
    if (value >= 10)
        cfg_delay = value;
    value = cfg_int(buf, n, "duree=");
    if (value >= 1)
        cfg_duree = value;
    value = cfg_int(buf, n, "entrees=");
    if (value >= 100)
        cfg_entrees = value;
    value = cfg_int(buf, n, "minlen=");
    if (value >= 1 && value <= 4096)
        cfg_minlen = value;
}

static int consume_arm_file(void)
{
    SceUID fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
    int result;

    if (fd < 0) {
        TRACE("[arm] %s absent: probe remains disarmed.\n", ARM_FILE);
        return 0;
    }
    ksceIoClose(fd);

    result = ksceIoRemove(ARM_FILE);
    TRACE("[arm] consumed (remove -> 0x%08X).\n", result);
    if (result < 0)
        return 0;

    fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        ksceIoClose(fd);
        TRACE("[arm] file still present: cancelled.\n");
        return 0;
    }
    return 1;
}

/*
 * SceWlanBt imports this exact five-word ABI.  The names are deliberately
 * descriptive rather than speculative: the probe records only raw ABI
 * metadata and therefore remains valid while the undocumented SDIF symbol
 * has no public name.
 */
static int hook_sdif_xfer(unsigned int context, unsigned int address,
                          unsigned int size, void *buffer, unsigned int mode)
{
    int ret = TAI_CONTINUE(int, xfer_ref, context, address, size, buffer, mode);
    unsigned int n = ++xfer_seen;

    if ((int)size < cfg_minlen) {
        xfer_small++;
        return ret;
    }

    LOG("xfer n=%u ctx=%08X addr=%08X size=%u buf=%08X mode=%08X -> %08X\n",
        n, context, address, size, (unsigned int)buffer, mode, (unsigned int)ret);
    return ret;
}

static int install_hook(void)
{
    xfer_uid = taiHookFunctionImportForKernel(KERNEL_PID, &xfer_ref,
        MOD_WLANBT, LIB_SDIF_DRIVER, NID_SDIF_XFER, hook_sdif_xfer);
    TRACE("[hook] %s:SceSdifForDriver:%08X -> 0x%08X\n",
        MOD_WLANBT, NID_SDIF_XFER, xfer_uid);
    if (xfer_uid < 0)
        return xfer_uid;
    hook_installed = 1;
    return 0;
}

static void release_hook(void)
{
    if (xfer_uid >= 0) {
        taiHookReleaseForKernel(xfer_uid, xfer_ref);
        xfer_uid = -1;
    }
    hook_installed = 0;
}

static int trace_thread(SceSize args, void *argp)
{
    int ticks = 0;
    (void)args;
    (void)argp;

    ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);
    t_origin = ksceKernelGetSystemTimeWide();
    TRACE("=== sdiotrace v1: delay=%d s duration=%d s minlen=%d ===\n",
        cfg_delay, cfg_duree, cfg_minlen);

    if (!consume_arm_file())
        return 0;
    if (install_hook() < 0)
        return 0;

    TRACE("[hook] ready; power on the controller now.\n");
    while (keep_running && ticks < cfg_duree * 4) {
        ksceKernelDelayThread(250 * 1000);
        ticks++;
        log_flush();
    }

    release_hook();
    TRACE("=== end: all=%u filtered-small=%u dropped=%d ===\n",
        xfer_seen, xfer_small, log_dropped);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    cfg_load();
    ksceIoMkdir(LOG_DIR, 0777);
    LOG("\n===== sdiotrace v1 =====\n");
    log_flush();

    thread_uid = ksceKernelCreateThread("sdiotrace", trace_thread,
        0x3C, 0x10000, 0, 0x10000, 0);
    if (thread_uid < 0) {
        TRACE("thread create failed: 0x%08X\n", thread_uid);
        return SCE_KERNEL_START_SUCCESS;
    }
    ksceKernelStartThread(thread_uid, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    keep_running = 0;

    if (thread_uid >= 0) {
        ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
        ksceKernelDeleteThread(thread_uid);
        thread_uid = -1;
    }
    if (hook_installed)
        release_hook();
    log_flush();
    return SCE_KERNEL_STOP_SUCCESS;
}
