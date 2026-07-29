#include "speaker_sink.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- G.711 mu-law decode (standard ITU-T; inverse of audio_source.c) ---- */
static short mulaw_decode(unsigned char u) {
    u = (unsigned char)~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (short)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/* The feed frame the shim/IMP_AO wants: 320 samples S16LE = 640 bytes = 20 ms @16k */
#define OUT_SAMPLES 320
#define OUT_BYTES   640

typedef struct {
    speaker_sink_t *s;
    int fd;                     /* forward socket to the shim */
    struct sockaddr_in dst;     /* shim addr */
    short  outbuf[OUT_SAMPLES]; /* accumulating 16k frame */
    int    outn;
    int    have_prev;
    short  prev;                /* last 8k sample, for the 2x interpolation carry */
} sink_ctx_t;

static void flush_if_full(sink_ctx_t *c) {
    if (c->outn == OUT_SAMPLES) {
        sendto(c->fd, c->outbuf, OUT_BYTES, 0, (struct sockaddr *)&c->dst, sizeof c->dst);
        c->outn = 0;
    }
}

/* mu-law byte -> S16 8k -> 2x upsample (vendor's y[2i]=x[i], y[2i+1]=(x[i]+x[i+1])/2)
 * -> accumulate 16k S16 into 640-byte frames -> forward to the shim. */
static void feed_mulaw(sink_ctx_t *c, const unsigned char *mulaw, int n) {
    for (int i = 0; i < n; i++) {
        short cur = mulaw_decode(mulaw[i]);
        if (!c->have_prev) { c->prev = cur; c->have_prev = 1; }
        /* emit the interpolated sample between prev and cur, then cur */
        short mid = (short)(((int)c->prev + (int)cur) / 2);
        c->outbuf[c->outn++] = c->prev; flush_if_full(c);
        c->outbuf[c->outn++] = mid;     flush_if_full(c);
        c->prev = cur;
    }
}

static sink_ctx_t g_pushctx;   /* used by speaker_sink_push_mulaw */
static int g_push_ready = 0;

void speaker_sink_push_mulaw(speaker_sink_t *s, const unsigned char *mulaw, int n) {
    if (!g_push_ready || n <= 0) return;
    (void)s;
    feed_mulaw(&g_pushctx, mulaw, n);
}

static void *sink_loop(void *arg) {
    speaker_sink_t *s = (speaker_sink_t *)arg;
    sink_ctx_t c;
    memset(&c, 0, sizeof c);
    c.s = s;

    /* forward socket to the shim */
    c.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c.fd < 0) { LOGE("talk: forward socket failed: %s", strerror(errno)); return NULL; }
    memset(&c.dst, 0, sizeof c.dst);
    c.dst.sin_family = AF_INET;
    c.dst.sin_port = htons((unsigned short)s->feed_port);
    if (inet_pton(AF_INET, s->feed_host, &c.dst.sin_addr) != 1)
        c.dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* share this ctx with the ONVIF backchannel push path */
    memcpy(&g_pushctx, &c, sizeof c);
    g_push_ready = 1;

    /* receive socket for the UDP talk endpoint */
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rfd < 0) { LOGE("talk: recv socket failed: %s", strerror(errno)); close(c.fd); return NULL; }
    int one = 1; setsockopt(rfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct timeval tv = { 0, 500000 }; setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in ra; memset(&ra, 0, sizeof ra);
    ra.sin_family = AF_INET; ra.sin_addr.s_addr = htonl(INADDR_ANY);
    ra.sin_port = htons((unsigned short)s->talk_port);
    if (bind(rfd, (struct sockaddr *)&ra, sizeof ra) != 0) {
        LOGW("talk: bind :%d failed (%s) -- UDP talk endpoint disabled", s->talk_port, strerror(errno));
        close(rfd); rfd = -1;
    } else {
        LOGI("talk: mu-law/8k ingress on 0.0.0.0:%d -> 16k -> shim %s:%d",
             s->talk_port, s->feed_host, s->feed_port);
    }

    unsigned char buf[2048];
    long stat_bytes = 0; double stat_t = monotonic_seconds();
    while (!s->stop) {
        if (rfd < 0) { sleep_seconds(0.5); continue; }
        ssize_t n = recv(rfd, buf, sizeof buf, 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOGW("talk: recv error: %s", strerror(errno));
            /* reset interpolation carry on a gap so bursts stay clean */
            c.have_prev = 0; c.outn = 0;
            continue;
        }
        feed_mulaw(&c, buf, (int)n);
        stat_bytes += n;
        double now = monotonic_seconds();
        if (now - stat_t >= 10.0) {
            LOGI("talk: %ld mu-law bytes in %.1fs (%.0f B/s)", stat_bytes, now - stat_t, stat_bytes / (now - stat_t));
            stat_bytes = 0; stat_t = now;
        }
    }
    if (rfd >= 0) close(rfd);
    close(c.fd);
    g_push_ready = 0;
    LOGI("talk: sink stopped");
    return NULL;
}

void speaker_sink_init(speaker_sink_t *s, int talk_port, const char *feed_host, int feed_port) {
    memset(s, 0, sizeof *s);
    s->talk_port = talk_port > 0 ? talk_port : 5601;
    snprintf(s->feed_host, sizeof s->feed_host, "%s", (feed_host && feed_host[0]) ? feed_host : "127.0.0.1");
    s->feed_port = feed_port > 0 ? feed_port : 5600;
}

void speaker_sink_start(speaker_sink_t *s) {
    if (s->started) return;
    s->started = 1;
    pthread_create(&s->thread, NULL, sink_loop, s);
}

void speaker_sink_stop(speaker_sink_t *s) {
    s->stop = 1;
}
