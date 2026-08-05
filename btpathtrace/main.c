/*
 * btpathtrace - one-point passive trace of an alternate SceBt failure path.
 *
 * Firmware 3.65 static verification:
 *
 *   +0x26A4  add.w r1, r0, #0x35400
 *   +0x26A8  push {r4-r7,lr}
 *   ...
 *   +0x27F6  orr r3, r3, #0x40   (sets the state-4 failure flag)
 *
 * The boundary is +0x26A4, not the following push instruction.  Every direct
 * caller passes one 32-bit device pointer in r0.  The hook logs that scalar
 * only, never dereferences it, and performs no I/O in hook context.
 *
 * It is armed once: its arm file is consumed before the hook is installed.
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR  "ur0:/log"
#define LOG_FILE LOG_DIR "/btpathtrace.txt"
#define CFG_FILE "ur0:/tai/btpathtrace.cfg"
#define ARM_FILE "ur0:/tai/btpathtrace.on"

#define MOD_BT         "SceBt"
#define BT_PATH_OFFSET 0x26A4
#define LOG_BUF_SIZE   (24 * 1024)

static char log_buf[LOG_BUF_SIZE];
static int log_pos;
static int log_entries;
static int log_dropped;
static int log_reentry;

static int cfg_delay = 30;
static int cfg_duree = 30;
static int cfg_entrees = 200;

static SceInt64 t_origin;
static volatile int keep_running = 1;
static SceUID thread_uid = -1;
static int hook_installed;
static unsigned int path_calls;

static tai_hook_ref_t path_ref;
static SceUID path_uid = -1;

static int millis(void)
{
    if (t_origin == 0)
        return 0;
    return (int)((ksceKernelGetSystemTimeWide() - t_origin) / 1000);
}

static void LOG(const char *fmt, ...)
{
    char tmp[144];
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
    char tmp[144];
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
    char buf[96];
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
    if (value >= 50)
        cfg_entrees = value;
}

static int consume_arm_file(void)
{
    SceUID fd = ksceIoOpen(ARM_FILE, SCE_O_RDONLY, 0);
    int result;
    if (fd < 0) {
        TRACE("[arm] absent: disarmed.\n");
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
        TRACE("[arm] still present: cancelled.\n");
        return 0;
    }
    return 1;
}

static int hook_path(unsigned int device)
{
    int ret;
    unsigned int n = ++path_calls;

    LOG("path enter n=%u dev=%08X\n", n, device);
    ret = TAI_CONTINUE(int, path_ref, device);
    LOG("path leave n=%u -> %08X\n", n, (unsigned int)ret);
    return ret;
}

static int install_hook(void)
{
    tai_module_info_t info;
    int result;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    result = taiGetModuleInfoForKernel(KERNEL_PID, MOD_BT, &info);
    if (result < 0) {
        TRACE("[hook] SceBt unavailable: %08X\n", result);
        return result;
    }
    path_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &path_ref,
        info.modid, 0, BT_PATH_OFFSET, 1, hook_path);
    TRACE("[hook] SceBt+%05X -> %08X\n", BT_PATH_OFFSET, path_uid);
    if (path_uid < 0)
        return path_uid;
    hook_installed = 1;
    return 0;
}

static void release_hook(void)
{
    if (path_uid >= 0) {
        taiHookReleaseForKernel(path_uid, path_ref);
        path_uid = -1;
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
    TRACE("=== btpathtrace: delay=%d s duration=%d s offset=0x%X ===\n",
        cfg_delay, cfg_duree, BT_PATH_OFFSET);
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
    TRACE("=== end: path=%u dropped=%d ===\n", path_calls, log_dropped);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    cfg_load();
    ksceIoMkdir(LOG_DIR, 0777);
    LOG("\n===== btpathtrace v1 =====\n");
    log_flush();
    thread_uid = ksceKernelCreateThread("btpathtrace", trace_thread,
        0x3C, 0x10000, 0, 0x10000, 0);
    if (thread_uid < 0) {
        TRACE("thread create failed: %08X\n", thread_uid);
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
