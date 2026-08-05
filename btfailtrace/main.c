/*
 * btfailtrace - reusable one-point passive trace framework for SceBt.
 *
 * Default target, statically verified on the 3.65 SceBt image:
 *
 *   +0x0DD8  ldrh r3, [r0, #0x30]
 *   +0x0DDA  push {r4-r7,lr}
 *   ...
 *   +0x0E06  orr r3, r3, #0x40   (sets the state-4 failure flag)
 *
 * The old attempt started at +0x0DDA.  That is not a function boundary: it
 * skips the first instruction above when TAI_CONTINUE resumes, leaving r3
 * undefined.  This probe deliberately hooks the real entry +0x0DD8.  Its ABI
 * is one 32-bit device pointer (r0), confirmed by every direct caller.  The
 * pointer is logged but never dereferenced.
 *
 * The hook is passive and one-shot armed.  All I/O remains in the thread.
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR       "ur0:/log"
#define MOD_BT        "SceBt"
#define LOG_BUF_SIZE  (24 * 1024)

/*
 * btcoretrace compiles this same source with a zero-argument target.  The
 * target at +0x1C4FC is a self-contained SceBt worker: it does not consume
 * r0-r3 and returns the status which +0x26A4 turns into the state-4 verdict.
 * Keeping both variants in one source makes their safety framework identical.
 */
#ifdef BT_TRACE_CORE
#define TRACE_NAME       "btcoretrace"
#define LOG_FILE         LOG_DIR "/btcoretrace.txt"
#define CFG_FILE         "ur0:/tai/btcoretrace.cfg"
#define ARM_FILE         "ur0:/tai/btcoretrace.on"
#define BT_TARGET_OFFSET 0x1C4FC
#define BT_TARGET_NOARGS 1
#elif defined(BT_TRACE_LOOKUP)
/*
 * SceBt+0x11420 is a four-register lookup: (root, type, mac0, mac1).  For
 * type 0x21 it returns the registered device record matching the MAC pair.
 * The caller at +0x3A4C immediately reads record+0x30 and sets bit 6 when it
 * is nonzero.  A nonzero result is therefore safe to inspect at these two
 * scalar offsets; the lookup itself produced this in-module table pointer.
 */
#define TRACE_NAME       "btlookuptrace"
#define LOG_FILE         LOG_DIR "/btlookuptrace.txt"
#define CFG_FILE         "ur0:/tai/btlookuptrace.cfg"
#define ARM_FILE         "ur0:/tai/btlookuptrace.on"
#define BT_TARGET_OFFSET 0x11420
#define BT_TARGET_LOOKUP 1
#define TEST_V2_MAC0     0xB26FBCD3u
#define TEST_V2_MAC1     0x0000C822u
#else
#define TRACE_NAME       "btfailtrace"
#define LOG_FILE         LOG_DIR "/btfailtrace.txt"
#define CFG_FILE         "ur0:/tai/btfailtrace.cfg"
#define ARM_FILE         "ur0:/tai/btfailtrace.on"
#define BT_TARGET_OFFSET 0x0DD8
#endif

static char log_buf[LOG_BUF_SIZE];
static int log_pos;
static int log_entries;
static int log_dropped;
static int log_reentry;

static int cfg_delay = 30;
static int cfg_duree = 30;
static int cfg_entrees = 200;
static int cfg_clear6;

static SceInt64 t_origin;
static volatile int keep_running = 1;
static SceUID thread_uid = -1;
static int hook_installed;
static unsigned int target_calls;
static unsigned int target_clears;

static tai_hook_ref_t target_ref;
static SceUID target_uid = -1;

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
    value = cfg_int(buf, n, "clear6=");
    if (value == 0 || value == 1)
        cfg_clear6 = value;
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

#ifdef BT_TARGET_NOARGS
static int hook_target(void)
{
    int ret;
    unsigned int n = ++target_calls;

    LOG("target enter n=%u\n", n);
    ret = TAI_CONTINUE(int, target_ref);
    LOG("target leave n=%u -> %08X\n", n, (unsigned int)ret);
    return ret;
}
#elif defined(BT_TARGET_LOOKUP)
static int hook_target(unsigned int root, unsigned int type,
                       unsigned int mac0, unsigned int mac1)
{
    int ret;
    unsigned int n = ++target_calls;

    ret = TAI_CONTINUE(int, target_ref, root, type, mac0, mac1);
    if (n <= (unsigned int)cfg_entrees) {
        unsigned int flags = 0;
        unsigned int field30 = 0;
        if (ret) {
            flags = ((volatile unsigned int *)ret)[1];
            field30 = (unsigned int)((volatile unsigned short *)ret)[24];
        }
        if (cfg_clear6 && target_clears == 0 && ret &&
            (type & 7) == 1 && mac0 == TEST_V2_MAC0 &&
            mac1 == TEST_V2_MAC1 && (flags & 0x40)) {
            ((volatile unsigned int *)ret)[1] = flags & ~0x40u;
            target_clears++;
            LOG("lookup clear6 %08X -> %08X\n", flags, flags & ~0x40u);
        }
        LOG("lookup n=%u type=%08X mac=%08X:%08X ret=%08X field30=%04X flags=%08X\n",
            n, type, mac0, mac1, (unsigned int)ret, field30, flags);
    }
    return ret;
}
#else
static int hook_target(unsigned int device)
{
    int ret;
    unsigned int n = ++target_calls;

    LOG("target enter n=%u dev=%08X\n", n, device);
    ret = TAI_CONTINUE(int, target_ref, device);
    LOG("target leave n=%u -> %08X\n", n, (unsigned int)ret);
    return ret;
}
#endif

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
    target_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &target_ref,
        info.modid, 0, BT_TARGET_OFFSET, 1, hook_target);
    TRACE("[hook] SceBt+%05X -> %08X\n", BT_TARGET_OFFSET, target_uid);
    if (target_uid < 0)
        return target_uid;
    hook_installed = 1;
    return 0;
}

static void release_hook(void)
{
    if (target_uid >= 0) {
        taiHookReleaseForKernel(target_uid, target_ref);
        target_uid = -1;
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
    TRACE("=== %s: delay=%d s duration=%d s offset=0x%X ===\n",
        TRACE_NAME, cfg_delay, cfg_duree, BT_TARGET_OFFSET);
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
    TRACE("=== end: target=%u clear6=%u dropped=%d ===\n",
        target_calls, target_clears, log_dropped);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    cfg_load();
    ksceIoMkdir(LOG_DIR, 0777);
    LOG("\n===== %s v1 =====\n", TRACE_NAME);
    log_flush();
    thread_uid = ksceKernelCreateThread(TRACE_NAME, trace_thread,
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
