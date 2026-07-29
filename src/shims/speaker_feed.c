/* speaker_feed.so -- TALK-BACK: play audio on the camera speaker (IMP AO).
 * Mirror image of mic_capture.so (which pure-reads the mic AI dev1). This
 * RECEIVES S16LE/16 kHz/mono PCM over UDP 127.0.0.1:SPK_PORT and feeds it to
 * the speaker via vp_project's OWN vendor AO wrapper functions -- called by
 * absolute VA, scalar/pointer args only (the wrappers own the global AO
 * context @0x8120F0, so the null-&ctx crash class that killed raw IMP_AI
 * cannot occur here).
 *
 * AO is device 0 / channel 0, 16 kHz S16LE mono, 320-sample/640-byte frames
 * (verified two independent ways: rodata "insmod audio.ko samplerate=16000",
 * ao_init hardcodes numPerFrm=320, and the vendor RX path upsamples 8k->16k).
 *
 * Vendor wrappers (call THESE, never raw IMP_AO):
 *   ao_init(int samplerate,int bitwidth,int soundmode) @0x004c3bd0
 *       -- SetPubAttr->Enable->EnableChn; sets enabled-flag byte @0x812148;
 *          IDEMPOTENT (early-returns if already enabled); loads audio.ko.
 *   ao_send(void *pcm, unsigned len)                   @0x004c4000
 *       -- accumulates into a 640-byte global buffer; on each full frame
 *          calls IMP_AO_SendFrame(0,0,&frame,0). Returns -1 harmlessly if AO
 *          not enabled.
 *   ao_deinit(void)                                    @0x004c41fc
 *       -- clears the enabled flag, DisableChn/Disable.
 *
 * SAFETY (AO is a state-changing WRITE, unlike the pure-read mic):
 *   - OFF at rest. Lazy ao_init only on the first datagram of a talk burst.
 *   - OWNERSHIP: read the enabled-flag byte BEFORE ao_init. If already 1,
 *     vp_project (its native two-way-talk) owns AO -> we only ao_send, we
 *     NEVER ao_deinit (that would tear down the vendor's talk). We deinit
 *     only a channel our own ao_init transitioned 0->1, after SPK_IDLE_MS of
 *     silence.
 *   - SPK_FEED_OFF=1 kill switch (constructor returns, no thread).
 *   - SPK_DELAY start delay so vp_project's IMP is up first.
 *   - SPK_TEST tone mode exercises the whole AO path with NO network, as the
 *     Stage-A validation gate.
 *
 * Env: SPK_FEED_OFF, SPK_DELAY(=15), SPK_PORT(=5600), SPK_IDLE_MS(=2000),
 *      SPK_KEEPOPEN, SPK_TEST(=secs, default 3), SPK_TONE_HZ(=1000).
 *
 * Freestanding, like mic_capture.c: no libc linked; symbols resolved at load
 * from vp_project's uClibc/pthread. MIPS SOCK_DGRAM=1 (not 2!). MIPS socket
 * option constants also differ (SOL_SOCKET=0xffff, SO_RCVTIMEO=0x1006).
 */
#define AO_INIT    0x004c3bd0u
#define AO_SEND    0x004c4000u
#define AO_DEINIT  0x004c41fcu
#define AO_SETVOL  0x004c3eb8u   /* ao_setvol(float) -> IMP_AO_SetVol; vendor sets this at talk-start */
#define AO_SETGAIN 0x004c3f54u   /* ao_setgain(float) -> IMP_AO_SetGain */
#define AO_SOFT_UNMUTE 0x00582f0cu /* IMP_AO_Soft_UNMute(dev,chn) */
/* The ONLY thing that drains queued AO frames to /dev/dsp is _ao_play_thread,
 * which IMP_AO_EnableChn spawns. vp_project set the enabled-flag @0x812148, so
 * the vendor ao_init early-returns and our shim SKIPS Enable/EnableChn -> the
 * play thread for our frames may not be running -> SendFrame enqueues (ret=0)
 * but nothing plays. So force a real enable of dev0/chn0 before sending. */
#define AO_ENABLE     0x005811acu  /* IMP_AO_Enable(devId=0) */
#define AO_ENABLECHN  0x005814e8u  /* IMP_AO_EnableChn(devId=0, chn=0) */
#define AO_VOL_VA  0x006bdddcu   /* vendor's talk volume float (0.67) in rodata */
#define AO_GAIN_VA 0x006bdde0u   /* vendor's talk gain float   (0.81) in rodata */
#define AO_ENABLED_FLAG 0x00812148u   /* unsigned char: 1 == AO enabled */

#define AO_RATE   16000
#define AO_BITS   16
#define AO_MODE   1                   /* mono */
#define FRAME_SAMPLES 320
#define FRAME_BYTES   640             /* 320 * 2 */

#define LOG_PATH "/tmp/speaker_feed.log"
#ifndef SPK_DELAY_DEFAULT
#define SPK_DELAY_DEFAULT 15u
#endif

/* MIPS socket option constants (differ from x86!) */
#define M_SOL_SOCKET   0xffff
#define M_SO_RCVTIMEO  0x1006
#define M_SO_REUSEADDR 0x0004

extern char *getenv(const char *name);
extern unsigned int atoi(const char *s);
extern int  open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long count);
extern int  close(int fd);
extern int  socket(int domain, int type, int protocol);
extern int  bind(int fd, const void *addr, int addrlen);
extern int  setsockopt(int fd, int level, int optname, const void *optval, int optlen);
extern long recvfrom(int fd, void *buf, unsigned long len, int flags, void *src, int *srclen);
extern int  pthread_create(unsigned long *thread, const void *attr, void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);
extern unsigned int sleep(unsigned int seconds);
extern int  usleep(unsigned int usec);

#define O_WRONLY 1
#define O_CREAT  0x0100
#define O_TRUNC  0x0200

typedef int (*fn_ao_init_t)(int, int, int);
typedef int (*fn_ao_send_t)(void *, unsigned int);
typedef int (*fn_ao_deinit_t)(void);
typedef int (*fn_ao_setf_t)(float);
typedef int (*fn_ao_unmute_t)(int, int);
typedef int (*fn_ao_en_t)(int);
typedef int (*fn_ao_enchn_t)(int, int);

static int g_log = -1;
static int s_len(const char *s){int n=0;while(s[n])n++;return n;}
static void s_puts(int fd,const char *s){if(fd>=0)write(fd,s,(unsigned long)s_len(s));}
static void u_itoa(long v,char *b){char t[24];int n=0,i=0;unsigned long u;int neg=v<0;u=neg?(unsigned long)(-v):(unsigned long)v;if(!u)t[n++]='0';while(u){t[n++]=(char)('0'+u%10);u/=10;}if(neg)b[i++]='-';while(n>0)b[i++]=t[--n];b[i]=0;}
static void logkv(const char *m,long v,int hv){char nb[24];if(g_log<0)return;s_puts(g_log,m);if(hv){u_itoa(v,nb);s_puts(g_log,nb);}s_puts(g_log,"\n");}

static fn_ao_init_t   ao_init   = (fn_ao_init_t)AO_INIT;
static fn_ao_send_t   ao_send   = (fn_ao_send_t)AO_SEND;
static fn_ao_deinit_t ao_deinit = (fn_ao_deinit_t)AO_DEINIT;

static int ao_is_enabled(void){ return *(volatile unsigned char *)AO_ENABLED_FLAG ? 1 : 0; }

/* Replicate the vendor's talk-start volume setup (ao_setvol + ao_setgain with
 * its own rodata float constants). Without this, playback is enabled but
 * effectively muted (the idle default), which is why ao_send returned 0 yet
 * nothing was audible. Called once per talk burst / tone. */
static void ao_set_talk_volume(void){
    fn_ao_en_t     enable  = (fn_ao_en_t)AO_ENABLE;
    fn_ao_enchn_t  enchn   = (fn_ao_enchn_t)AO_ENABLECHN;
    fn_ao_setf_t   setvol  = (fn_ao_setf_t)AO_SETVOL;
    fn_ao_setf_t   setgain = (fn_ao_setf_t)AO_SETGAIN;
    fn_ao_unmute_t unmute  = (fn_ao_unmute_t)AO_SOFT_UNMUTE;
    float vol  = *(volatile float *)AO_VOL_VA;
    float gain = *(volatile float *)AO_GAIN_VA;
    /* force a real channel enable so _ao_play_thread is running and drains our
     * SendFrame'd frames to /dev/dsp (vendor's ao_init early-returns since its
     * flag is set, so it never (re)spawns the play thread for us). */
    int re = enable(0);
    int rc = enchn(0, 0);
    int ru = unmute(0, 0);
    int rv = setvol(vol);
    int rg = setgain(gain);
    logkv("IMP_AO_Enable ret=", re, 1);
    logkv("IMP_AO_EnableChn ret=", rc, 1);
    logkv("IMP_AO_Soft_UNMute ret=", ru, 1);
    logkv("ao_setvol ret=", rv, 1);
    logkv("ao_setgain ret=", rg, 1);
}

/* one period of a sine, 32 entries, amplitude ~8000 (integer, no libm) */
static const short SINE32[32] = {
    0,1561,3062,4444,5657,6652,7391,7846,8000,7846,7391,6652,5657,4444,3062,1561,
    0,-1561,-3062,-4444,-5657,-6652,-7391,-7846,-8000,-7846,-7391,-6652,-5657,-4444,-3062,-1561
};

/* ---- Stage-A: play a synthesized tone (no network) to validate the AO path ---- */
static void tone_test(int secs, int hz){
    short frame[FRAME_SAMPLES];
    unsigned long acc = 0;
    /* 16.16 fixed phase step into the 32-entry table at 16 kHz.
     * 32*65536/16000 == 16384/125 (reduced) -> stays in 32-bit, no __udivdi3. */
    unsigned long inc = ((unsigned long)hz * 16384u) / 125u;
    int owned = 0, r, f, frames = secs * 50; /* 50 frames/s @ 20ms */

    if(!ao_is_enabled()){
        r = ao_init(AO_RATE, AO_BITS, AO_MODE);
        logkv("SPK_TEST ao_init ret=", r, 1);
        owned = ao_is_enabled();
        logkv("SPK_TEST we_own=", owned, 1);
    } else {
        logkv("SPK_TEST: AO already enabled (vp owns) -- sending without init", 0, 0);
    }
    logkv("SPK_TEST tone hz=", hz, 1);
    ao_set_talk_volume();   /* unmute / set the vendor's talk volume */
    for(f=0; f<frames; f++){
        int i;
        for(i=0;i<FRAME_SAMPLES;i++){ frame[i]=SINE32[(acc>>16)&31]; acc+=inc; }
        r = ao_send((void*)frame, FRAME_BYTES);
        if(f<3) logkv("SPK_TEST ao_send ret=", r, 1);
        usleep(20000); /* pace ~20ms/frame */
    }
    if(owned){ ao_deinit(); logkv("SPK_TEST ao_deinit (owned) done", 0, 0); }
    logkv("SPK_TEST done frames=", frames, 1);
}

/* build 127.0.0.1:port sockaddr_in (16 bytes) */
static void loopback_sa(unsigned char *sa, int port){
    sa[0]=2; sa[1]=0;                                  /* AF_INET */
    sa[2]=(unsigned char)((port>>8)&0xff); sa[3]=(unsigned char)(port&0xff); /* port net order */
    sa[4]=127; sa[5]=0; sa[6]=0; sa[7]=1;              /* 127.0.0.1 */
    sa[8]=0;sa[9]=0;sa[10]=0;sa[11]=0;sa[12]=0;sa[13]=0;sa[14]=0;sa[15]=0;
}

static void receive_loop(int port, int idle_ms, int keepopen){
    unsigned char sa[16];
    unsigned char buf[4096];
    int sock, one=1, owned=0, active=0, r;
    long silence_ticks = 0;               /* recvtimeouts (~0.5s each) while active */
    long idle_ticks = idle_ms / 500; if(idle_ticks<1) idle_ticks=1;
    struct { long s, us; } tv;            /* timeval */
    unsigned long total=0, th;

    sock = socket(2, 1, 0);               /* AF_INET, SOCK_DGRAM=1 on MIPS */
    logkv("socket fd=", sock, 1);
    if(sock < 0){ logkv("socket() failed", 0, 0); return; }
    setsockopt(sock, M_SOL_SOCKET, M_SO_REUSEADDR, &one, sizeof one);
    tv.s = 0; tv.us = 500000;             /* 0.5s recv timeout so idle-close can fire */
    setsockopt(sock, M_SOL_SOCKET, M_SO_RCVTIMEO, &tv, sizeof tv);
    loopback_sa(sa, port);
    if(bind(sock, sa, 16) != 0){ logkv("bind failed on port=", port, 1); close(sock); return; }
    logkv("speaker_feed: listening S16LE/16k on 127.0.0.1:", port, 1);

    (void)th;
    for(;;){
        r = (int)recvfrom(sock, buf, sizeof buf, 0, (void*)0, (int*)0);
        /* Stage-A trigger: a magic "SPKTONE!" datagram plays the isolated
         * tone (ao_init+tone+deinit), so AO can be validated on a clean-booted
         * baked image without any risky vp_project relaunch. */
        if(r == 8 && buf[0]=='S'&&buf[1]=='P'&&buf[2]=='K'&&buf[3]=='T'&&buf[4]=='O'&&buf[5]=='N'&&buf[6]=='E'&&buf[7]=='!'){
            logkv("magic SPKTONE! received -> isolated tone test", 0, 0);
            tone_test(3, 1000);
            continue;
        }
        if(r <= 0){
            /* timeout / idle: owner-only idle-close after silence */
            if(active && owned && !keepopen){
                if(++silence_ticks >= idle_ticks){
                    ao_deinit();
                    logkv("idle-close (owned) after silence; total frames=", (long)total, 1);
                    active = 0; owned = 0; silence_ticks = 0;
                }
            }
            continue;
        }
        silence_ticks = 0;
        if(!active){
            /* first datagram of a burst: lazy enable with ownership guard */
            if(ao_is_enabled()){
                owned = 0;                 /* vp_project owns AO -> feed only, never deinit */
                logkv("talk burst start: AO already enabled (vp owns), feeding only", 0, 0);
            } else {
                int ir = ao_init(AO_RATE, AO_BITS, AO_MODE);
                owned = ao_is_enabled() ? 1 : 0;
                logkv("talk burst start: ao_init ret=", ir, 1);
                logkv("  we_own=", owned, 1);
            }
            ao_set_talk_volume();   /* unmute / set the vendor's talk volume */
            active = 1;
        }
        r = ao_send(buf, (unsigned int)r);
        total++;
        if(total<=3) logkv("ao_send ret=", r, 1);
    }
}

static void *worker(void *arg){
    const char *e; int delay, port, idle_ms, keepopen, test_secs, tone_hz;
    (void)arg;
    e = getenv("SPK_DELAY");    delay    = (e&&*e)?(int)atoi(e):SPK_DELAY_DEFAULT;
    sleep((unsigned int)delay);
    g_log = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    logkv("speaker_feed: worker start (vendor AO wrappers; dev0/chn0 16k)", 0, 0);

    e = getenv("SPK_TEST");
    if(e){
        test_secs = (*e)?(int)atoi(e):3; if(test_secs<=0) test_secs=3;
        e = getenv("SPK_TONE_HZ"); tone_hz = (e&&*e)?(int)atoi(e):1000; if(tone_hz<=0) tone_hz=1000;
        logkv("MODE: SPK_TEST tone, secs=", test_secs, 1);
        tone_test(test_secs, tone_hz);
        if(g_log>=0){ close(g_log); g_log=-1; }
        return (void*)0;
    }

    e = getenv("SPK_PORT");     port     = (e&&*e)?(int)atoi(e):5600;
    e = getenv("SPK_IDLE_MS");  idle_ms  = (e&&*e)?(int)atoi(e):2000;
    keepopen = getenv("SPK_KEEPOPEN") ? 1 : 0;
    logkv("MODE: receive; port=", port, 1);
    receive_loop(port, idle_ms, keepopen);
    return (void*)0;
}

static volatile int g_started = 0;
__attribute__((constructor))
static void speaker_feed_init(void){
    unsigned long th;
    if(g_started) return; g_started = 1;
    if(getenv("SPK_FEED_OFF")) return;                 /* kill switch */
    if(pthread_create(&th,(void*)0,worker,(void*)0)==0) pthread_detach(th);
}
