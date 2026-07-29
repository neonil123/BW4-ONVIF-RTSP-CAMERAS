/* camweb.so — LD_PRELOAD shim that starts vp_project's dead-code local web/CGI server.
 *
 * QC3 (VeePai BW6, Ingenic T23N) ships with no ESP32, and on this build the
 * bootstrap for vp_project's 127.0.0.1:81 web/CGI server (vp_web_create_socket) was
 * compiled out — the function is present but unreachable (see PATCH_DESIGN_1B.md).
 *
 * vp_project is ET_EXEC / non-PIE (fixed load @0x400000, no ASLR on kernel 3.10), so the
 * function is at a fixed runtime VA. It is a no-arg, idempotent routine that binds the
 * listen socket and stores the fd at a fixed global. We spawn a worker thread that calls
 * it in a retry loop until the fd goes valid, then exits. Fully reversible: remove the .so
 * / drop LD_PRELOAD from the launcher.
 *
 * Freestanding: no libc is linked in. pthread_create/pthread_detach/sleep/unsetenv are
 * left undefined and resolved at load time against vp_project's already-loaded uClibc
 * (it links libpthread), so there is no libc-ABI/version coupling to this .so.
 *
 * Critical LD_PRELOAD guard: the constructor calls unsetenv("LD_PRELOAD") before it
 * spawns anything, so busybox children (e.g. udhcpc) forked later do NOT inherit the
 * preload chain and fail to resolve pthread symbols -- which would take down WiFi/DHCP.
 * This shim must therefore lead the LD_PRELOAD list. [verified hard dependency]
 *
 * Addresses are from static RE of builds/exfil/vp_project.bin (verify before deploy):
 *   vp_web_create_socket : 0x0047db44  (no args; self-guarded; binds 127.0.0.1:81)
 *   web listen fd (int)   : 0x007e8db8  (-1 until bound)
 */

#define CREATE_SOCKET_ADDR  0x0047db44u
#define WEB_FD_ADDR         0x007e8db8u

/* resolved from the host process' uClibc at load time (o32 ABI, identical calling conv) */
extern int  pthread_create(unsigned long *thread, const void *attr,
                           void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);
extern unsigned int sleep(unsigned int seconds);
extern int  unsetenv(const char *name);

static void *camweb_worker(void *arg)
{
    volatile int *fd = (volatile int *)WEB_FD_ADDR;
    int (*create_web)(void) = (int (*)(void))CREATE_SOCKET_ADDR;

    (void)arg;
    /* let vp_project finish its own init and bring vnet0 up before we poke it */
    sleep(8);
    /* create_web() is idempotent: it early-returns once the socket exists, and retries
       its own bind on EADDRINUSE. Keep calling until the listen fd becomes valid. */
    while (*fd < 0) {
        create_web();
        if (*fd >= 0)
            break;
        sleep(3);
    }
    return (void *)0;
}

__attribute__((constructor))
static void camweb_init(void)
{
    unsigned long t;
    /* CRITICAL: strip LD_PRELOAD from the environment first so busybox children
       (udhcpc) don't inherit the chain and fail to resolve pthread -- this is what
       keeps WiFi/DHCP alive. Must run before anything forks. */
    unsetenv("LD_PRELOAD");
    if (pthread_create(&t, (void *)0, camweb_worker, (void *)0) == 0)
        pthread_detach(t);
}
