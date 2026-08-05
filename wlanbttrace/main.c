/*
 * wlanbttrace - passive probe of the SceBt -> SceWlanBt boundary.
 *
 * 3.65 SceBt imports exactly two SceWlanBtForDriver exports:
 *   0FC89113 : no input argument, returns a state word.
 *   4C96C3B7 : one 32-bit selector, returns a state/result word.
 *
 * Both ABIs are verified from their SceBt callers and SceWlanBt exports.
 * The hooks only copy these scalar values to a RAM log after forwarding the
 * unmodified call.  No packet buffer is involved and no I/O runs in a hook.
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR  "ur0:/log"
#define LOG_FILE LOG_DIR "/wlanbttrace.txt"
#define CFG_FILE "ur0:/tai/wlanbttrace.cfg"
#define ARM_FILE "ur0:/tai/wlanbttrace.on"

#define MOD_BT             "SceBt"
#define LIB_WLANBT_DRIVER  0xFD94FCE9
#define NID_GET_STATE      0x0FC89113
#define NID_SELECT         0x4C96C3B7

#define LOG_BUF_SIZE (32 * 1024)

static char log_buf[LOG_BUF_SIZE];
static int log_pos;
static int log_entries;
static int log_dropped;
static int log_reentry;

static int cfg_delay = 30;
static int cfg_duree = 30;
static int cfg_entrees = 600;

static SceInt64 t_origin;
static volatile int keep_running = 1;
static SceUID thread_uid = -1;
static int hooks_installed;

static unsigned int state_calls;
static unsigned int select_calls;

static tai_hook_ref_t state_ref;
static tai_hook_ref_t select_ref;
static SceUID state_uid = -1;
static SceUID select_uid = -1;

static int millis(void)
{
    if (t_origin == 0)
        return 0;
    return (int)((ksceKernelGetSystemTimeWide() - t_origin) / 1000);
}

static void LOG(const char *fmt, ...)
{
    char tmp[160];
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
    char tmp[160];
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
    SceUID fd = ksceIoOpen(CFG_FILE, SCE_O_RDONLY, 0);
    int n;
    int value;
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

static int hook_get_state(void)
{
    int ret = TAI_CONTINUE(int, state_ref);
    LOG("state n=%u -> %08X\n", ++state_calls, (unsigned int)ret);
    return ret;
}

static int hook_select(unsigned int selector)
{
    int ret = TAI_CONTINUE(int, select_ref, selector);
    LOG("select n=%u id=%u (0x%X) -> %08X\n", ++select_calls,
        selector, selector, (unsigned int)ret);
    return ret;
}

static int install_hooks(void)
{
    state_uid = taiHookFunctionImportForKernel(KERNEL_PID, &state_ref,
        MOD_BT, LIB_WLANBT_DRIVER, NID_GET_STATE, hook_get_state);
    select_uid = taiHookFunctionImportForKernel(KERNEL_PID, &select_ref,
        MOD_BT, LIB_WLANBT_DRIVER, NID_SELECT, hook_select);
    TRACE("[hook] state=%08X select=%08X\n", state_uid, select_uid);
    if (state_uid < 0 || select_uid < 0)
        return -1;
    hooks_installed = 1;
    return 0;
}

static void release_hooks(void)
{
    if (select_uid >= 0) {
        taiHookReleaseForKernel(select_uid, select_ref);
        select_uid = -1;
    }
    if (state_uid >= 0) {
        taiHookReleaseForKernel(state_uid, state_ref);
        state_uid = -1;
    }
    hooks_installed = 0;
}

static int trace_thread(SceSize args, void *argp)
{
    int ticks = 0;
    (void)args;
    (void)argp;
    ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);
    t_origin = ksceKernelGetSystemTimeWide();
    TRACE("=== wlanbttrace: delay=%d s duration=%d s ===\n", cfg_delay, cfg_duree);
    if (!consume_arm_file())
        return 0;
    if (install_hooks() < 0) {
        release_hooks();
        return 0;
    }
    TRACE("[hook] ready; power on the controller now.\n");
    while (keep_running && ticks < cfg_duree * 4) {
        ksceKernelDelayThread(250 * 1000);
        ticks++;
        log_flush();
    }
    release_hooks();
    TRACE("=== end: state=%u select=%u dropped=%d ===\n",
        state_calls, select_calls, log_dropped);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    cfg_load();
    ksceIoMkdir(LOG_DIR, 0777);
    LOG("\n===== wlanbttrace v1 =====\n");
    log_flush();
    thread_uid = ksceKernelCreateThread("wlanbttrace", trace_thread,
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
    if (hooks_installed)
        release_hooks();
    log_flush();
    return SCE_KERNEL_STOP_SUCCESS;
}
