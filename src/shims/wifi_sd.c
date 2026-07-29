/* wifi_sd.so — SD-card WiFi onboarding shim for QC3 (VeePai BW6, T23N).
 *
 * Drop a file "/mnt/sda0/wifi.ini" (or wifi.txt) on the SD card:
 *     SSID=MyNetwork
 *     PASSWORD=secretpass
 * On boot this LD_PRELOAD shim reads it, parses SSID + password, and calls
 * vp_project's own WiFi-onboarding entry point with those credentials, exactly
 * as the QR/factory onboarding path does. No video is ever recorded to SD:
 * this shim makes ZERO calls into vp_project's record module — it only reads a
 * file and calls the WiFi-connect function.
 *
 * ---- Mechanism (static RE of builds/exfil/vp_project.bin, ET_EXEC base 0x400000) ----
 * WiFi-connect entry, o32 ABI (a0=ssid, a1=pwd, a2=mode):
 *     int fn(const char *ssid, const char *pwd, int mode)
 *     VA 0x00453a2c  ("vp_wifi apply credentials + connect")
 * Proven by disassembly:
 *   - 0x453a2c passes its a0/a1 straight into 0x48e134(ssid,pwd), the common
 *     sink of ALL onboarding paths (factory, QR, smartlink), which logs
 *     "ssid:%s pwd:%s", memsets the wifi cred struct at 0x8111b0, strncpy's
 *     ssid (max 0x20) and pwd (max 0x40), then tail-calls the connect 0x48ba1c.
 *   - vp_factory_mode_check_thread calls 0x453a2c with mode=2 (0x410a78);
 *     0x4341f4 / 0x48a190 / 0x452dc4 confirm a0=ssid, a1=pwd across 4 sites.
 * Self-guarded: 0x453a2c returns a negative error (no side effects) unless the
 * wifi state global at 0x7e8c20 == 2, so an early / wrong-state call is a no-op.
 * vp_project is non-PIE (fixed load 0x400000, no ASLR on kernel 3.10), so
 * 0x453a2c is a fixed runtime address, built here via absolute lui/ori.
 *
 * Freestanding: no libc is linked. open/read/close/rename/sleep/pthread_* are
 * left undefined and resolved at load time against vp_project's own uClibc
 * (exactly as tools/camweb/camweb.c already does for pthread/sleep).
 *
 * Reversible: delete the .so / drop it from LD_PRELOAD; no bytes of vp_project
 * are modified.
 */

#include "parse_ini.h"

/* --- WiFi-connect entry (VERIFY before every reflash: python app_constref /
       app_gotpage -d 0x453a2c against the vp_project in the partition) --- */
#define WIFI_CONNECT_ADDR   0x00453a2cu
#define WIFI_MODE           2            /* what vp_factory_mode_check_thread passes */

#define SD_PATH_INI   "/mnt/sda0/wifi.ini"
#define SD_PATH_TXT   "/mnt/sda0/wifi.txt"
#define SD_DONE_INI   "/mnt/sda0/wifi.ini.applied"
#define SD_DONE_TXT   "/mnt/sda0/wifi.txt.applied"

/* resolved from the host process' uClibc at load (o32 ABI, same calling conv) */
extern int  open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int  close(int fd);
extern int  rename(const char *oldp, const char *newp);
extern unsigned int sleep(unsigned int seconds);
extern int  pthread_create(unsigned long *thread, const void *attr,
                           void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);

#define O_RDONLY 0

/* creds live in the .so's .bss (zeroed by the loader) */
static char g_ssid[WSD_SSID_MAX + 4];
static char g_pass[WSD_PWD_MAX + 4];
static char g_buf[1024];

/* returns bytes read (>0), 0 if file absent/empty */
static int slurp(const char *path)
{
    int fd, total = 0;
    long r;
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    while (total < (int)sizeof(g_buf) - 1) {
        r = read(fd, g_buf + total, sizeof(g_buf) - 1 - total);
        if (r <= 0) break;
        total += (int)r;
    }
    close(fd);
    g_buf[total] = 0;
    return total;
}

static void *wifi_sd_worker(void *arg)
{
    int (*wifi_connect)(const char *, const char *, int) =
        (int (*)(const char *, const char *, int))WIFI_CONNECT_ADDR;
    const char *src_path = 0, *done_path = 0;
    int len, code, tries;

    (void)arg;

    /* let vp_project finish its own init and bring the AIC / vnet0 up; the SD
       is mounted at /mnt/sda0 before Linux userspace per the boot flow. Retry
       the read a few times in case the mount lands slightly after us. */
    sleep(10);
    for (tries = 0; tries < 12; tries++) {
        len = slurp(SD_PATH_INI);
        if (len > 0) { src_path = SD_PATH_INI; done_path = SD_DONE_INI; break; }
        len = slurp(SD_PATH_TXT);
        if (len > 0) { src_path = SD_PATH_TXT; done_path = SD_DONE_TXT; break; }
        sleep(5);
    }
    if (!src_path) return (void *)0;          /* no file -> clean no-op */

    code = wsd_parse(g_buf, len, g_ssid, sizeof(g_ssid), g_pass, sizeof(g_pass));
    if (code < 0) return (void *)0;           /* malformed -> clean no-op */

    /* Apply. 0x453a2c returns 0 on success, negative if the wifi subsystem is
       not yet in STA state (*(0x7e8c20)!=2). Retry so that whenever the state
       becomes ready our credentials take effect; bounded so we never spin. */
    for (tries = 0; tries < 24; tries++) {
        int r = wifi_connect(g_ssid,
                             (code == WSD_OK_OPEN) ? (const char *)0 : g_pass,
                             WIFI_MODE);
        if (r == 0) {
            /* success: rename so we don't re-apply and to signal the user.
               (Plaintext still on SD — README tells the user to remove it.) */
            rename(src_path, done_path);
            break;
        }
        sleep(3);
    }
    return (void *)0;
}

__attribute__((constructor))
static void wifi_sd_init(void)
{
    unsigned long t;
    if (pthread_create(&t, (void *)0, wifi_sd_worker, (void *)0) == 0)
        pthread_detach(t);
}
