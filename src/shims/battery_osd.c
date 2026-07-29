/* battery_osd.so  --  LD_PRELOAD shim that renders the live battery percentage
 * onto the QC3 (VeePai BW6, Ingenic T23N) video OSD overlay.
 *
 * Feature: show the real battery level on the on-screen timestamp overlay,
 * sourced from the SAME value the vendor sleep logic and the app use, with no
 * .text patch to vp_project.
 *
 * ===========================================================================
 * WHY THE VALUE SOURCE CHANGED (live evidence from the flashed unit)
 *   The MCU "percentage" byte 0x7fbf58 (VP_MCU_PERCENTAGE) is DEAD on this unit
 *   -- it stays 0 even on a healthy battery (USB unplugged, running fine). So is
 *   the sleep "direct %" byte 0x811ee0 (=0). The ONLY field with real data is the
 *   voltage word 0x811edc (=416 -> 4.16 V -> ~90%).
 *   The vendor's own battery getter 0x48d0ec already handles exactly this: it
 *   returns 0x811ee0 when that byte is 1..254, otherwise converts the raw voltage
 *   word 0x811edc through its voltage->% curve (0x48b068). Disasm of 0x48d0ec:
 *       48d0ec: lui $2,0x81 ; addiu $2,$2,7888      ; $2 = 0x811ed0
 *       48d0f4: lbu $9,16($2)                        ; direct% = *0x811ee0
 *       48d0f8: bnez $9,48d13c                       ; if direct != 0 use it
 *       48d100: lw  $4,12($2)                        ; raw = *0x811edc (u32 mV/10)
 *       48d104: beqz $4,48d144                       ; raw==0 -> return direct(0)
 *       48d114: jal 48b068 ; andi $4,$4,0xffff       ; % = curve(raw)
 *       48d11c..48d130: clamp to [1,100]             ; return in $2
 *       48d144: jr $31 ; move $2,$9                   ; else return direct byte
 *   Signature: int get_batt(void)  (o32; NO args; result in $v0/$2; range 0..100).
 *   It uses only ABSOLUTE addressing (no $gp / no $t9 dependency) and its only
 *   callee 0x48b068 is a pure table lookup -- so it is safe to call from this
 *   shim as an absolute function pointer (vp_project is ET_EXEC / fixed base,
 *   same fixed-VA technique the other shims use for data). We save/restore $gp
 *   around the call (compiler-generated) and never touch its inputs.
 *   => the OSD now shows the exact number the sleep logic compares and the app
 *      reports (~90 now), guaranteeing agreement.
 *
 * WHY THE LABEL IS LETTER-FREE ("BAT" was impossible in the OSD font)
 *   Live frames show "YYYY-MM-DD HH:MM:SS A 0%" -- the whole all-digit date and
 *   the "0%" render perfectly, but "BAT " renders as " A ": 'A' shows while 'B'
 *   and 'T' vanish. That is a PARTIAL OSD glyph set (the timestamp font carries
 *   digits / '-' / ':' / ' ' / '%' and only some letters), not a buffer bug --
 *   the format buffer we write is clean, but missing letter glyphs drop out at
 *   draw time, which no buffer content can fix. We therefore use a letter-free
 *   label built only from proven-rendering glyphs: "<date> NN%". (To re-attempt
 *   a text label once the font's real charset is known, set -DOSD_LABEL at build
 *   time; default is the safe numeric form.)
 *
 * ===========================================================================
 * OSD TARGET (verified against builds/exfil/vp_project.bin, ET_EXEC @0x400000)
 *   Chn0 OSD region-descriptor array: base 0x80d774, stride 0x6f0, idx 0..2
 *   (osd_region_set @0x47ed48). Idx0 = primary timestamp overlay: setup @0x450710
 *   seeds its buffer with the strftime FORMAT "%Y-%m-%d %H:%M:%S" (@0x6b52fc);
 *   the render @0x47e5e8 runs strftime(out[256],256,fmt,tm) each refresh (256-byte
 *   stack out -> a ~24-char line never truncates), gated on a dirty bit.
 *       text/format : 0x80d774 + 0*0x6f0 + 0xa0 = 0x0080d814  (char[64])
 *       dirty flags : 0x80d774 + 0*0x6f0 + 0x14 = 0x0080d788  (u16, set bit7)
 *   We overwrite idx0's format with "%Y-%m-%d %H:%M:%S NN%%" (strftime renders
 *   "%%" -> literal '%', the date codes keep ticking) and set the dirty bit
 *   exactly as osd_region_set() does (lhu; set bit7 0x0080; sh).
 *
 * Freestanding: no libc linked. pthread_create/pthread_detach/sleep are left
 * undefined and resolved at load time from vp_project's already-loaded uClibc.
 * Fully reversible: remove the .so / drop LD_PRELOAD from the launcher.
 * ===========================================================================
 */

/* --- fixed vp_project runtime addresses (verified via disasm). --- */
#define BATT_GETTER     0x0048d0ecu   /* int get_batt(void) -> % 0..100 (voltage-derived) */
#define OSD_DESC_BASE   0x0080d774u   /* chn0 OSD region-descriptor array base  */
#define OSD_STRIDE      0x000006f0u   /* per-region-index stride                */
#define OSD_TEXT_OFF    0x000000a0u   /* strftime format/text buffer (64 bytes) */
#define OSD_FLAG_OFF    0x00000014u   /* u16 flags; bit7 (0x0080) = redraw      */

#ifndef OSD_IDX
#define OSD_IDX         0u
#endif

/* Optional text label prefix before the number, e.g. -DOSD_LABEL='" BAT "'.
 * Default is letter-free (" ") because the timestamp OSD font drops several
 * uppercase glyphs (see header). The trailing '%' is always appended. */
#ifndef OSD_LABEL
#define OSD_LABEL " "
#endif

#ifndef OSD_START_DELAY
#define OSD_START_DELAY 30u
#endif
#ifndef OSD_PERIOD
#define OSD_PERIOD      3u
#endif

/* resolved from the host process' uClibc at load time (o32 ABI). */
extern int  pthread_create(unsigned long *thread, const void *attr,
                           void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);
extern unsigned int sleep(unsigned int seconds);

/* vendor battery getter, called by absolute VA (vp_project is fixed-base). */
typedef int (*batt_getter_fn)(void);

#define OSD_TEXT_ADDR (OSD_DESC_BASE + (OSD_IDX) * OSD_STRIDE + OSD_TEXT_OFF)
#define OSD_FLAG_ADDR (OSD_DESC_BASE + (OSD_IDX) * OSD_STRIDE + OSD_FLAG_OFF)

/* Build "%Y-%m-%d %H:%M:%S<LABEL>NN%%\0" into dst. No libc.
 * Returns the length written (excluding NUL). */
static int build_osd_format(char *dst, int pct)
{
    static const char pre[] = "%Y-%m-%d %H:%M:%S" OSD_LABEL;
    int i = 0;

    while (pre[i]) { dst[i] = pre[i]; i++; }

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    if (pct >= 100) {
        dst[i++] = '1'; dst[i++] = '0'; dst[i++] = '0';
    } else if (pct >= 10) {
        dst[i++] = (char)('0' + pct / 10);
        dst[i++] = (char)('0' + pct % 10);
    } else {
        dst[i++] = (char)('0' + pct);
    }

    dst[i++] = '%';   /* strftime: "%%" -> literal '%' */
    dst[i++] = '%';
    dst[i]   = '\0';
    return i;
}

static void *battery_osd_worker(void *arg)
{
    volatile char           *text  = (volatile char           *)OSD_TEXT_ADDR;
    volatile unsigned short *flags  = (volatile unsigned short *)OSD_FLAG_ADDR;
    batt_getter_fn get_batt = (batt_getter_fn)BATT_GETTER;
    char line[40];
    unsigned wait;

    (void)arg;

    /* Let vp_project finish MCU handshake + OSD region bring-up + battery module
     * init (so 0x811edc voltage is populated and the getter returns real %). */
    sleep(OSD_START_DELAY);

    /* Wait until idx0's format buffer is populated by the app (non-empty), so we
     * never write before the region's one-time memset+strncpy setup. Bounded. */
    for (wait = 0; wait < 120 && text[0] == 0; wait++)
        sleep(1);

    /* Re-read + re-write + re-assert the dirty bit UNCONDITIONALLY every period
     * so the overlay self-heals and always tracks the live percentage. */
    for (;;) {
        int pct = get_batt();           /* voltage-derived %, same as sleep/app  */
        int n   = build_osd_format(line, pct);
        int k;

        for (k = 0; k < n && k < 63; k++)
            text[k] = line[k];
        text[k] = '\0';

        /* assert the redraw dirty bit exactly as osd_region_set() does. */
        *flags = (unsigned short)(*flags | 0x0080u);

        sleep(OSD_PERIOD);
    }
    return (void *)0;
}

__attribute__((constructor))
static void battery_osd_init(void)
{
    unsigned long t;
    if (pthread_create(&t, (void *)0, battery_osd_worker, (void *)0) == 0)
        pthread_detach(t);
}
