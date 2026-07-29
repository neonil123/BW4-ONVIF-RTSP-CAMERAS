/* pir_sleep.so — LD_PRELOAD shim that ENABLES vp_project's own vendor
 * "Smart_Electricity_Sleep" battery-saving feature on the O-KAM QC3
 * (VeePai BW6, Ingenic T23N): below <THRESHOLD>%% battery the camera enters the
 * vendor low-power / AOV deep-sleep (PIR-wake), and resumes normal streaming
 * once the battery recovers (vendor +10%% hysteresis). Solar+battery unit.
 *
 * =====================================================================
 * ADDRESSES — re-verified 2026-07-28 against builds/exfil/vp_project.bin
 * (ET_EXEC / non-PIE @0x400000; fixed runtime .bss VAs). The prior README's
 * addresses were actually CORRECT; they are re-proven here three independent
 * ways so the switch/threshold cannot be mis-set:
 *
 *   Config struct base = 0x0081e850
 *     getter 0x5d7fd8:  lui $2,0x82 ; addiu $2,$2,-6064  -> 0x820000-0x17b0 = 0x81e850
 *
 *   Smart_Electricity_Sleep_Switch  = config + 0x3008 -> VA 0x00821858  (u8, ==1 to enable)
 *   Smart_Electricity_Threshold     = config + 0x3009 -> VA 0x00821859  (u8, percent 30..100)
 *
 *   Decision routine 0x45a9c4 (called from the LPC/MCU-status event handler):
 *       $16 = config base (0x81e850, from getter 0x5d7fd8)
 *       $17 = session obj (from getter 0x5d7088)
 *       $2  = battery% (from getter 0x48d0ec)
 *       lw   $4,0xCF0($17)  ; if session in-call == 1 -> return  (NEVER sleep mid-stream)
 *       lbu  $4,0x3008($16) ; if switch != 1        -> return   (0x821858)
 *       lbu  $3,0x3009($16) ; thr = threshold                    (0x821859)
 *       sltu batt < thr        -> 0x48e92c(1)  ENTER SLEEP  (MCU cmd 0x30, PIR-wake)
 *       sltu batt > thr+10     -> 0x48e92c(0)  WAKE         (fixed +10 hysteresis)
 *   The CGI vp_cgi_low_power (~0x43ccxx) writes the SAME two bytes and clamps the
 *   threshold to [30,100] ("smart_sleep_switch:%d, smart_sleep_thrd:%d" @0x6ba5c3).
 *
 * We only ASSERT switch=1 / threshold=THRESHOLD into the live config and let the
 * vendor decision routine decide, sleep, PIR-wake, and resume. We never call any
 * vendor function and never touch the exit path -> worst case is cosmetic.
 * Fully reversible: drop the .so / remove LD_PRELOAD.
 *
 * ---------------------------------------------------------------------
 * BATTERY-SOURCE NOTE (must be confirmed on-device, see report):
 *   The OSD/app percentage is the MCU "percentage" field @0x7fbf58
 *   (VP_MCU_PERCENTAGE). The vendor SLEEP decision instead compares getter
 *   0x48d0ec, which returns the DIRECT byte @0x811ee0 (MCU msg+0x11) when it is
 *   non-zero/non-255, else a voltage curve of the raw word @0x811edc (MCU
 *   msg+0x0c). These are DIFFERENT MCU fields; whether they agree at a given
 *   charge can only be confirmed by a live read (0x7fbf58 vs 0x811ee0). If they
 *   diverge, the trigger point can be nudged via THRESHOLD or escalated to a
 *   1-instruction .text redirect of the decision's battery read (out of scope
 *   here — needs on-device approval).
 * =====================================================================
 */

#define CFG_SWITCH_ADDR  0x00821858u   /* config+0x3008  Smart_Electricity_Sleep_Switch (u8) */
#define CFG_THRESH_ADDR  0x00821859u   /* config+0x3009  Smart_Electricity_Threshold    (u8) */

/* Owner request: enter PIR-wake battery-saving mode below 50%.
 * Vendor wake hysteresis is a FIXED +10 in the decision routine (addiu $3,$3,10),
 * so it resumes normal streaming at ~60% -> no flap around 50%. Vendor-valid
 * range is 30..100 (CGI clamps <30 up to 30); 50 is in range. */
#define THRESHOLD        50

#define SETTLE_SECONDS   12   /* let vp_project finish config-load + LPC init before we write */
#define REASSERT_SECONDS 4    /* re-assert so a later app_param/config reload can't undo us     */

extern int  pthread_create(unsigned long *thread, const void *attr,
                           void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);
extern unsigned int sleep(unsigned int seconds);

static void *pir_sleep_worker(void *arg)
{
    volatile unsigned char *sw  = (volatile unsigned char *)CFG_SWITCH_ADDR;
    volatile unsigned char *thr = (volatile unsigned char *)CFG_THRESH_ADDR;

    (void)arg;

    /* Wait until vp_project has loaded app_param into the config struct, else its
       boot-time config-load would overwrite our bytes. */
    sleep(SETTLE_SECONDS);

    /* Unconditionally (re-)assert switch=ON / threshold=50 every few seconds so a
       later config reload, CGI write, or app toggle can never leave us disabled. */
    for (;;) {
        *sw  = 1;              /* Smart_Electricity_Sleep_Switch = ON  (0x821858) */
        *thr = THRESHOLD;      /* Smart_Electricity_Threshold    = 50% (0x821859) */
        sleep(REASSERT_SECONDS);
    }
    return (void *)0;
}

__attribute__((constructor))
static void pir_sleep_init(void)
{
    unsigned long t;
    if (pthread_create(&t, (void *)0, pir_sleep_worker, (void *)0) == 0)
        pthread_detach(t);
}
