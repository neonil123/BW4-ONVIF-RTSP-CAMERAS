/* mic_capture.so -- persistent LD_PRELOAD reader of the IMP AI mic (device 1,
 * channel 0) inside vp_project. PURE READ: PollingFrame/GetFrame/ReleaseFrame
 * only, no SetPubAttr/Enable/EnableChn -> zero state change, WiFi/video intact
 * (proven by imp_dev1_capture). Each 1280 B / 40 ms S16LE 16 kHz mono frame is
 * sent as one UDP datagram to MIC_DEST (default 127.0.0.1:5599) where
 * cam_onvifd turns it into an RTP audio track.
 *
 * Env:
 *   MIC_DEST=a.b.c.d:port  (default 127.0.0.1:5599)
 *   MIC_CAP_OFF=1          disable entirely (fail-safe)
 *   MIC_DELAY=<secs>       start delay so vp_project's IMP is ready (default 15)
 *
 * Read side addrs (a0=devId=1, from vp_project's own loop @0x4c389c):
 *   IMP_AI_PollingFrame 0x57c86c  GetFrame 0x57a580  ReleaseFrame 0x57c9e4
 */
#define IMP_AI_POLLINGFRAME 0x0057c86cu
#define IMP_AI_GETFRAME     0x0057a580u
#define IMP_AI_RELEASEFRAME 0x0057c9e4u
#define AI_DEV_ID 1
#define AI_CHN_ID 0
#define POLL_TIMEOUT_MS 1000u
#ifndef MIC_DELAY_DEFAULT
#define MIC_DELAY_DEFAULT 15u
#endif
#define LOG_PATH "/tmp/mic_capture.log"

extern char *getenv(const char *name);
extern int  *__errno_location(void);
extern int  open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long count);
extern int  close(int fd);
extern int  socket(int domain, int type, int protocol);
extern long sendto(int fd, const void *buf, unsigned long len, int flags, const void *dst, int dstlen);
extern int  pthread_create(unsigned long *thread, const void *attr, void *(*start)(void *), void *arg);
extern int  pthread_detach(unsigned long thread);
extern unsigned int sleep(unsigned int seconds);
extern unsigned int atoi(const char *s);

#define O_WRONLY 1
#define O_CREAT  0x0100
#define O_TRUNC  0x0200

typedef struct { int bitwidth; int soundmode; unsigned int *virAddr; unsigned int phyAddr; long long timeStamp; int seq; int len; } IMPAudioFrame;
typedef int (*fn_polling_t)(int, int, unsigned int);
typedef int (*fn_get_t)(int, int, IMPAudioFrame *, int);
typedef int (*fn_rel_t)(int, int, IMPAudioFrame *);

static int g_log = -1;
static int s_len(const char *s){int n=0;while(s[n])n++;return n;}
static void s_puts(int fd,const char *s){if(fd>=0)write(fd,s,(unsigned long)s_len(s));}
static void u_itoa(long v,char *b){char t[24];int n=0,i=0;unsigned long u;int neg=v<0;u=neg?(unsigned long)(-v):(unsigned long)v;if(!u)t[n++]='0';while(u){t[n++]=(char)('0'+u%10);u/=10;}if(neg)b[i++]='-';while(n>0)b[i++]=t[--n];b[i]=0;}
static void logkv(const char *m,long v,int hv){char nb[24];if(g_log<0)return;s_puts(g_log,m);if(hv){u_itoa(v,nb);s_puts(g_log,nb);}s_puts(g_log,"\n");}

/* build sockaddr_in (16 bytes) from "a.b.c.d:port"; returns port or 0 on error */
static int parse_dest(const char *s, unsigned char *sa){
    unsigned int oct[4]={0,0,0,0}, port=0, idx=0, val=0; int have=0; const char *p=s;
    while(*p && *p!=':'){ if(*p>='0'&&*p<='9'){ val=val*10+(*p-'0'); have=1; } else if(*p=='.'){ if(idx<4)oct[idx++]=val; val=0; } p++; }
    if(idx<4) oct[idx++]=val;
    if(*p==':'){ p++; port=atoi(p); }
    if(idx!=4 || !port || !have) return 0;
    sa[0]=2; sa[1]=0;                    /* AF_INET (little-endian short: 2,0) */
    sa[2]=(unsigned char)(port>>8); sa[3]=(unsigned char)(port&0xff);  /* port net order */
    sa[4]=(unsigned char)oct[0]; sa[5]=(unsigned char)oct[1]; sa[6]=(unsigned char)oct[2]; sa[7]=(unsigned char)oct[3];
    sa[8]=0;sa[9]=0;sa[10]=0;sa[11]=0;sa[12]=0;sa[13]=0;sa[14]=0;sa[15]=0;
    return (int)port;
}

static void *worker(void *arg){
    fn_polling_t Poll = (fn_polling_t)IMP_AI_POLLINGFRAME;
    fn_get_t     Get  = (fn_get_t)IMP_AI_GETFRAME;
    fn_rel_t     Rel  = (fn_rel_t)IMP_AI_RELEASEFRAME;
    unsigned char sa[16];
    const char *dest, *d;
    int sock, port, delay, ret;
    unsigned long frames=0, bytes=0, perr=0;
    long g_last_seq=0, g_d1=0, g_d2=0, g_dx=0;
    (void)arg;

    d = getenv("MIC_DELAY"); delay = (d && *d) ? (int)atoi(d) : MIC_DELAY_DEFAULT;
    sleep((unsigned int)delay);

    g_log = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    dest = getenv("MIC_DEST"); if(!dest || !*dest) dest = "127.0.0.1:5599";
    port = parse_dest(dest, sa);
    logkv("mic_capture: dev1 pure-read -> UDP, port=", port, 1);
    if(!port){ logkv("bad MIC_DEST, aborting",0,0); return (void*)0; }

    sock = socket(2, 1, 0);   /* AF_INET, SOCK_DGRAM=1 on MIPS (STREAM=2 -- swapped vs other arches) */
    logkv("socket fd=", sock, 1);
    if(sock < 0){ logkv("socket() failed",0,0); return (void*)0; }

    for(;;){
        IMPAudioFrame f; int i; long sret; for(i=0;i<(int)sizeof(f);i++) ((char*)&f)[i]=0;
        ret = Poll(AI_DEV_ID, AI_CHN_ID, POLL_TIMEOUT_MS);
        if(ret!=0){ perr++; if(perr%50==0) logkv("poll err count=",(long)perr,1); continue; }
        ret = Get(AI_DEV_ID, AI_CHN_ID, &f, 0);
        if(ret!=0){ continue; }
        if(f.virAddr && f.len>0){
            sret = sendto(sock, f.virAddr, (unsigned long)f.len, 0, sa, 16);
            frames++; bytes += (unsigned long)f.len;
            if(frames<=3){ logkv("sendto ret=",(long)sret,1); if(sret<0) logkv("  errno=",(long)*__errno_location(),1); }
            /* SEQ DIAGNOSTIC: log seq of first 40 frames + delta histogram.
             * delta==2 => a co-consumer (vp_project's own dev1 loop) steals every
             * other frame (real rate 25fps, split); delta==1 => dev1 native 12.5. */
            if(frames<=40){ logkv("seq=",(long)f.seq,1); }
            { long d = (long)f.seq - g_last_seq; g_last_seq=(long)f.seq;
              if(frames>1){ if(d==1)g_d1++; else if(d==2)g_d2++; else g_dx++; } }
            if(frames%250==0){ logkv("frames sent=",(long)frames,1);
              logkv("  seqdelta d1=",g_d1,1); logkv("  d2=",g_d2,1); logkv("  dOther=",g_dx,1); }
        }
        Rel(AI_DEV_ID, AI_CHN_ID, &f);
    }
    return (void*)0;
}

static volatile int g_started=0;
__attribute__((constructor))
static void init(void){
    unsigned long th;
    if(g_started) return; g_started=1;
    if(getenv("MIC_CAP_OFF")) return;
    if(pthread_create(&th,(void*)0,worker,(void*)0)==0) pthread_detach(th);
}
