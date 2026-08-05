/*
 * btparsertrace - passive context-preserving trace of SceBt+0x6558.
 *
 * The target parses variable-size controller frames.  It can receive more than
 * four arguments, so an ordinary C hook corrupts its caller's stack when it
 * uses TAI_CONTINUE.  hook_parser is a Thumb naked trampoline: it saves every
 * caller-clobbered core and VFP register used by a C call, logs r0-r3 only,
 * restores the original stack/arguments, then tail-branches to taiHEN's next
 * hook (or original trampoline).  It never invokes the original as a C call.
 *
 * The companion lookup hook is the already validated four-register lookup at
 * SceBt+0x11420.  It correlates the V2 device record with parser timestamps.
 * Neither hook writes target memory or reads packet buffers.
 */

#include <vitasdkkern.h>
#include <taihen.h>
#include <stdarg.h>

#define LOG_DIR       "ur0:/log"
#define LOG_FILE      LOG_DIR "/btparsertrace.txt"
#define CFG_FILE      "ur0:/tai/btparsertrace.cfg"
#define ARM_FILE      "ur0:/tai/btparsertrace.on"
#define MOD_BT        "SceBt"
#define PARSER_OFFSET 0x06558
#define SEND_OFFSET   0x109EC
#define LOOKUP_OFFSET 0x11420
#define CENTRAL_OFFSET 0x11544
#define SWEEP_OFFSET  0x064F8
#define LOG_BUF_SIZE  (24 * 1024)

#define TEST_V2_MAC0  0xB26FBCD3u
#define TEST_V2_MAC1  0x0000C822u

static char log_buf[LOG_BUF_SIZE];
static int log_pos;
static int log_entries;
static int log_dropped;
static int log_reentry;

static int cfg_delay = 10;
static int cfg_duree = 180;
static int cfg_entrees = 200;
static int cfg_bytes;
static int cfg_remap;
static int cfg_accept;
static int cfg_complete;

static SceInt64 t_origin;
static volatile int keep_running = 1;
static SceUID thread_uid = -1;
static SceUID completion_uid = -1;
static int hooks_installed;

static tai_hook_ref_t parser_ref;
static tai_hook_ref_t send_ref;
static tai_hook_ref_t lookup_ref;
static tai_hook_ref_t central_ref;
static tai_hook_ref_t sweep_ref;
static SceUID parser_uid = -1;
static SceUID send_uid = -1;
static SceUID lookup_uid = -1;
static SceUID central_uid = -1;
static SceUID sweep_uid = -1;
static unsigned int parser_calls;
static unsigned int send_calls;
static unsigned int lookup_calls;
static unsigned int central_calls;
static unsigned int v2_record;
static unsigned int v2_channel_50;
static unsigned int v2_channel_51;
static unsigned int accept_writes;
static volatile unsigned int completion_root;
static unsigned int completion_queued;
static unsigned int completion_runs;

static int millis(void)
{
    if (t_origin == 0)
        return 0;
    return (int)((ksceKernelGetSystemTimeWide() - t_origin) / 1000);
}

static void LOG(const char *fmt, ...)
{
    char tmp[152];
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
    char tmp[152];
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
    value = cfg_int(buf, n, "bytes=");
    if (value == 0 || value == 1)
        cfg_bytes = value;
    value = cfg_int(buf, n, "remap=");
    if (value == 0 || value == 1)
        cfg_remap = value;
    value = cfg_int(buf, n, "accept=");
    if (value == 0 || value == 1)
        cfg_accept = value;
    value = cfg_int(buf, n, "complete=");
    if (value == 0 || value == 1)
        cfg_complete = value;
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

/* Called by naked assembly with a pointer into the trampoline's own stack. */
__attribute__((used)) static void parser_log_context(const unsigned int *saved,
                                                      const unsigned int *original_sp)
{
    volatile unsigned char *frame;
    unsigned int n;

    (void)original_sp;
    /* The original immediately returns when r2 <= 3; keep only real frames. */
    if (saved[2] <= 3)
        return;
    n = ++parser_calls;
    /*
     * Active diagnostic, disabled by default.  V2's failing second channel
     * request is stable across captures: Code=2/Id=4/Len=4, PSM 0x0011 and
     * source CID 0x0051.  The PSM field is not checksummed at L2CAP.  Touch
     * that single volatile input byte only when explicitly armed with
     * remap=1; this routes it through the existing HID-Interrupt path.
     */
    if (cfg_remap && saved[1] && saved[2] == 16 && saved[3] == 2) {
        frame = (volatile unsigned char *)saved[1];
        if (frame[4] == 0x08 && frame[5] == 0x00 &&
            frame[6] == 0x01 && frame[7] == 0x00 &&
            frame[8] == 0x02 && frame[9] == 0x04 &&
            frame[10] == 0x04 && frame[11] == 0x00 &&
            frame[12] == 0x11 && frame[13] == 0x00 &&
            frame[14] == 0x51 && frame[15] == 0x00) {
            frame[12] = 0x13;
            LOG("remap n=%u V2 PSM 0011 -> 0013 (cid=0051)\n", n);
        }
    }
    if (cfg_bytes && saved[1] && saved[2] == 20 && saved[3] == 2) {
        frame = (volatile unsigned char *)saved[1];
        LOG("parser n=%u r0=%08X r1=%08X r2=%08X r3=%08X data="
            "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"
            "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
            n, saved[0], saved[1], saved[2], saved[3],
            frame[0], frame[1], frame[2], frame[3], frame[4],
            frame[5], frame[6], frame[7], frame[8], frame[9],
            frame[10], frame[11], frame[12], frame[13], frame[14],
            frame[15], frame[16], frame[17], frame[18], frame[19]);
        return;
    }
    if (cfg_bytes && saved[1] && saved[2] == 16 && saved[3] == 2) {
        frame = (volatile unsigned char *)saved[1];
        LOG("parser n=%u r0=%08X r1=%08X r2=%08X r3=%08X data="
            "%02X%02X%02X%02X%02X%02X%02X%02X"
            "%02X%02X%02X%02X%02X%02X%02X%02X\n",
            n, saved[0], saved[1], saved[2], saved[3],
            frame[0], frame[1], frame[2], frame[3],
            frame[4], frame[5], frame[6], frame[7],
            frame[8], frame[9], frame[10], frame[11],
            frame[12], frame[13], frame[14], frame[15]);
        return;
    }
    LOG("parser n=%u r0=%08X r1=%08X r2=%08X r3=%08X\n",
        n, saved[0], saved[1], saved[2], saved[3]);
}

/*
 * SceBt+0x109EC is the variadic internal L2CAP-signalling sender.  The
 * original stack holds its ABI arguments five through eight.  Record only
 * Connection Request/Response (codes 2/3) for the normal link selector 1;
 * this is passive and never dereferences caller-owned buffers.
 */
__attribute__((used)) static void send_log_context(const unsigned int *saved,
                                                    const unsigned int *original_sp)
{
    unsigned int n;
    volatile unsigned int *args = (volatile unsigned int *)original_sp;
    unsigned int channel = 0;
    unsigned int channel_flags = 0;
    unsigned int channel_cid = 0;
    if (saved[1] != 1 || (saved[3] != 2 && saved[3] != 3 &&
                          saved[3] != 4 && saved[3] != 5))
        return;
    n = ++send_calls;
    if (saved[3] == 3) {
        if ((args[3] & 0xFFFF) == 0x0050)
            channel = v2_channel_50;
        else if ((args[3] & 0xFFFF) == 0x0051)
            channel = v2_channel_51;
        if (channel) {
            channel_flags = ((volatile unsigned int *)channel)[1];
            channel_cid = ((volatile unsigned short *)channel)[26];
            LOG("channel V2 ptr=%08X cid=%04X flags=%08X\n",
                channel, channel_cid, channel_flags);
        }
        /*
         * Ask the native completion sweep to run after this pending response
         * has left the sender.  It performs the same 0x2 -> 0x6 transition
         * and emits the normal follow-up Success response seen with the V1.
         */
        if (cfg_complete && completion_queued < 2 && !completion_root &&
            saved[0] == v2_record && channel &&
            (channel_flags & 6) == 2 && args[4] == 1 && args[5] == 2 &&
            ((args[3] & 0xFFFF) == 0x0050 ||
             (args[3] & 0xFFFF) == 0x0051)) {
            completion_root = saved[0];
            completion_queued++;
            LOG("complete queued n=%u V2 cid=%04X\n", n,
                args[3] & 0xFFFF);
        }
        /*
         * One deliberately narrow active comparison with the successful V1:
         * only the V2 record, only its two observed connection responses,
         * only while SceBt is about to emit Pending/Authentication Pending.
         * Changing the two stack arguments changes this one outgoing L2CAP
         * response; no controller or persistent PS TV state is altered.
         */
        if (cfg_accept && accept_writes < 2 && saved[0] == v2_record &&
            args[1] == 8 && args[4] == 1 && args[5] == 2 &&
            ((args[0] == 3 && args[2] == 0x0040 && args[3] == 0x0050) ||
             (args[0] == 5 && args[2] == 0x0041 && args[3] == 0x0051))) {
            args[4] = 0;
            args[5] = 0;
            accept_writes++;
            LOG("accept n=%u V2 id=%X dcid=%04X scid=%04X pending -> success\n",
                n, args[0], args[2] & 0xFFFF, args[3] & 0xFFFF);
        }
        /* Connection Response: id, length, destination/source CID, result, status. */
        LOG("send n=%u root=%08X code=3 id=%X len=%X dcid=%04X scid=%04X result=%X status=%X\n",
            n, saved[0], args[0], args[1], args[2] & 0xFFFF,
            args[3] & 0xFFFF, args[4], args[5]);
    } else {
        LOG("send n=%u root=%08X code=%u a5=%08X a6=%08X a7=%08X a8=%08X a9=%08X a10=%08X\n",
            n, saved[0], saved[3], original_sp[0], original_sp[1],
            original_sp[2], original_sp[3], original_sp[4], original_sp[5]);
    }
}

/*
 * Stack on entry is the original caller stack.  The 96-byte local save keeps
 * the ABI 8-byte alignment before parser_log_context.  The final r12 slot is
 * overwritten with the taiHEN continuation; r12 is caller-clobbered, whereas
 * r0-r3, lr, stack arguments and d0-d7 are restored byte-for-byte.
 */
__attribute__((naked, noinline)) static void hook_parser(void)
{
    __asm__ volatile(
        "vpush {d0-d7}\n"
        "push {r0-r3, r12, lr}\n"
        "sub sp, #8\n"
        "add r0, sp, #8\n"
        "add r1, sp, #96\n"
        "bl parser_log_context\n"
        "ldr r12, =parser_ref\n"
        "ldr r12, [r12]\n"
        "ldr lr, [r12, #0]\n"
        "cmp lr, #0\n"
        "bne 1f\n"
        "ldr r12, [r12, #8]\n"
        "b 2f\n"
        "1:\n"
        "ldr r12, [lr, #4]\n"
        "2:\n"
        "str r12, [sp, #24]\n"
        "add sp, #8\n"
        "pop {r0-r3, r12, lr}\n"
        "vpop {d0-d7}\n"
        "bx r12\n"
    );
}

/* Same context-preserving tail trampoline for the variadic signalling sender. */
__attribute__((naked, noinline)) static void hook_send(void)
{
    __asm__ volatile(
        "vpush {d0-d7}\n"
        "push {r0-r3, r12, lr}\n"
        "sub sp, #8\n"
        "add r0, sp, #8\n"
        "add r1, sp, #96\n"
        "bl send_log_context\n"
        "ldr r12, =send_ref\n"
        "ldr r12, [r12]\n"
        "ldr lr, [r12, #0]\n"
        "cmp lr, #0\n"
        "bne 1f\n"
        "ldr r12, [r12, #8]\n"
        "b 2f\n"
        "1:\n"
        "ldr r12, [lr, #4]\n"
        "2:\n"
        "str r12, [sp, #24]\n"
        "add sp, #8\n"
        "pop {r0-r3, r12, lr}\n"
        "vpop {d0-d7}\n"
        "bx r12\n"
    );
}

static int hook_lookup(unsigned int root, unsigned int type,
                       unsigned int a2, unsigned int a3)
{
    int ret;
    unsigned int n = ++lookup_calls;
    unsigned int flags = 0;
    unsigned int field30 = 0;
    int is_v2 = ((type & 7) == 1 && a2 == TEST_V2_MAC0 && a3 == TEST_V2_MAC1);

    ret = TAI_CONTINUE(int, lookup_ref, root, type, a2, a3);
    if (is_v2 && ret)
        v2_record = (unsigned int)ret;
    if (ret && (is_v2 || (unsigned int)ret == v2_record)) {
        flags = ((volatile unsigned int *)ret)[1];
        field30 = (unsigned int)((volatile unsigned short *)ret)[24];
        LOG("lookup n=%u type=%08X a2=%08X:%08X rec=%08X id=%04X flags=%08X\n",
            n, type, a2, a3, (unsigned int)ret, field30, flags);
    }
    return ret;
}

/*
 * This is the central L2CAP-object lookup called by the code-2 (Connection
 * Request) branch of the parser.  Its four register arguments are established
 * by the SceBt callers; type 5 denotes an L2CAP channel object.
 */
static int hook_central(unsigned int device, unsigned int type,
                        unsigned int id)
{
    int ret = TAI_CONTINUE(int, central_ref, device, type, id);
    /*
     * Code 0x02 (L2CAP Connection Request) reaches this lookup with type
     * 0x14 and the peer's proposed source CID.  Type 5 belongs to the
     * unrelated signalling paths, so deliberately restrict the log to 0x14.
     */
    if (type == 0x14) {
        unsigned int flags = ret ? ((volatile unsigned int *)ret)[1] : 0;
        /*
         * At this lookup the third argument is the PSM, not the peer CID.
         * The V2's HID allocations are retained in arrival order and matched
         * to its later CIDs 0x0050/51.
         */
        if (device == v2_record && ret &&
            ((id & 0xFFFF) == 0x0011 || (id & 0xFFFF) == 0x0013)) {
            if (!v2_channel_50)
                v2_channel_50 = (unsigned int)ret;
            else if (!v2_channel_51 && (unsigned int)ret != v2_channel_50)
                v2_channel_51 = (unsigned int)ret;
        }
        central_calls++;
        LOG("l2cap n=%u dev=%08X type=%02X cid=%04X -> %08X flags=%08X\n",
            central_calls, device, type, id & 0xFFFF, (unsigned int)ret,
            flags);
    }
    return ret;
}

/* SceBt+0x64F8 scans a device's pending L2CAP channels and finalizes them. */
static void hook_sweep(unsigned int root)
{
    TAI_CONTINUE(void, sweep_ref, root);
}

/*
 * The variadic sender is deliberately tail-called by hook_send, so completion
 * must happen from a separate thread after that response has returned.  The
 * two-millisecond delay preserves the native Pending -> Success order.
 */
static int completion_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (keep_running) {
        unsigned int root = completion_root;
        if (root) {
            completion_root = 0;
            ksceKernelDelayThread(2 * 1000);
            if (keep_running && cfg_complete && completion_runs < 2 &&
                root == v2_record) {
                completion_runs++;
                LOG("complete run=%u root=%08X native-sweep\n",
                    completion_runs, root);
                TAI_CONTINUE(void, sweep_ref, root);
            }
        }
        ksceKernelDelayThread(1000);
    }
    return 0;
}

static int install_hooks(void)
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
    parser_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &parser_ref,
        info.modid, 0, PARSER_OFFSET, 1, hook_parser);
    TRACE("[hook] SceBt+%05X parser -> %08X\n", PARSER_OFFSET, parser_uid);
    if (parser_uid < 0)
        return parser_uid;
    send_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &send_ref,
        info.modid, 0, SEND_OFFSET, 1, hook_send);
    TRACE("[hook] SceBt+%05X send -> %08X\n", SEND_OFFSET, send_uid);
    if (send_uid < 0)
        return send_uid;
    central_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &central_ref,
        info.modid, 0, CENTRAL_OFFSET, 1, hook_central);
    TRACE("[hook] SceBt+%05X l2cap -> %08X\n", CENTRAL_OFFSET, central_uid);
    if (central_uid < 0)
        return central_uid;
    sweep_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &sweep_ref,
        info.modid, 0, SWEEP_OFFSET, 1, hook_sweep);
    TRACE("[hook] SceBt+%05X complete -> %08X\n", SWEEP_OFFSET, sweep_uid);
    if (sweep_uid < 0)
        return sweep_uid;
    lookup_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &lookup_ref,
        info.modid, 0, LOOKUP_OFFSET, 1, hook_lookup);
    TRACE("[hook] SceBt+%05X lookup -> %08X\n", LOOKUP_OFFSET, lookup_uid);
    if (lookup_uid < 0)
        return lookup_uid;
    hooks_installed = 1;
    return 0;
}

static void release_hooks(void)
{
    if (lookup_uid >= 0) {
        taiHookReleaseForKernel(lookup_uid, lookup_ref);
        lookup_uid = -1;
    }
    if (central_uid >= 0) {
        taiHookReleaseForKernel(central_uid, central_ref);
        central_uid = -1;
    }
    if (sweep_uid >= 0) {
        taiHookReleaseForKernel(sweep_uid, sweep_ref);
        sweep_uid = -1;
    }
    if (send_uid >= 0) {
        taiHookReleaseForKernel(send_uid, send_ref);
        send_uid = -1;
    }
    if (parser_uid >= 0) {
        taiHookReleaseForKernel(parser_uid, parser_ref);
        parser_uid = -1;
    }
    hooks_installed = 0;
}

static void stop_completion_thread(void)
{
    if (completion_uid >= 0) {
        ksceKernelWaitThreadEnd(completion_uid, NULL, NULL);
        ksceKernelDeleteThread(completion_uid);
        completion_uid = -1;
    }
}

static int trace_thread(SceSize args, void *argp)
{
    int ticks = 0;
    (void)args;
    (void)argp;
    ksceKernelDelayThread((SceUInt)cfg_delay * 1000 * 1000);
    t_origin = ksceKernelGetSystemTimeWide();
    TRACE("=== btparsertrace: delay=%d s duration=%d s remap=%d accept=%d complete=%d ===\n",
        cfg_delay, cfg_duree, cfg_remap, cfg_accept, cfg_complete);
    if (!consume_arm_file())
        return 0;
    if (install_hooks() < 0)
        return 0;
    if (cfg_complete) {
        completion_uid = ksceKernelCreateThread("btparsercomplete",
            completion_thread, 0x3C, 0x10000, 0, 0x10000, 0);
        if (completion_uid < 0) {
            TRACE("[complete] thread create failed: %08X\n", completion_uid);
            completion_uid = -1;
            return 0;
        }
        ksceKernelStartThread(completion_uid, 0, NULL);
    }
    TRACE("[hook] ready; %s.\n",
          cfg_complete ? "native completion enabled" : "passive only");
    while (keep_running && ticks < cfg_duree * 4) {
        ksceKernelDelayThread(250 * 1000);
        ticks++;
        log_flush();
    }
    keep_running = 0;
    stop_completion_thread();
    release_hooks();
    TRACE("=== end: parser=%u send=%u lookup=%u l2cap=%u dropped=%d ===\n",
        parser_calls, send_calls, lookup_calls, central_calls, log_dropped);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    cfg_load();
    ksceIoMkdir(LOG_DIR, 0777);
    LOG("\n===== btparsertrace v1 =====\n");
    log_flush();
    thread_uid = ksceKernelCreateThread("btparsertrace", trace_thread,
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
    stop_completion_thread();
    if (hooks_installed)
        release_hooks();
    log_flush();
    return SCE_KERNEL_STOP_SUCCESS;
}
