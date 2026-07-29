#include "cam_source.h"
#include "vstarcam_frame.h"
#include "rtp_h264.h"
#include "h264_sps.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ----------------------------------------------------------- au_buf refcount */
static pthread_mutex_t g_aubuf_mutex = PTHREAD_MUTEX_INITIALIZER;

static au_buf_t *au_buf_new(int n_nals, const uint8_t *const *nals, const size_t *lens, uint32_t ts) {
    size_t total = 0;
    for (int i = 0; i < n_nals; i++) total += lens[i];
    au_buf_t *b = (au_buf_t *)malloc(sizeof(au_buf_t) + total);
    if (!b) return NULL;
    b->refcount = 1;
    b->ts = ts;
    b->n_nals = n_nals;
    size_t off = 0;
    for (int i = 0; i < n_nals; i++) {
        memcpy(b->data + off, nals[i], lens[i]);
        b->nal_ptr[i] = b->data + off;
        b->nal_len[i] = lens[i];
        off += lens[i];
    }
    return b;
}

static void au_buf_addref(au_buf_t *b) {
    pthread_mutex_lock(&g_aubuf_mutex);
    b->refcount++;
    pthread_mutex_unlock(&g_aubuf_mutex);
}

void au_buf_release(au_buf_t *b) {
    if (!b) return;
    int z;
    pthread_mutex_lock(&g_aubuf_mutex);
    z = (--b->refcount == 0);
    pthread_mutex_unlock(&g_aubuf_mutex);
    if (z) free(b);
}

/* --------------------------------------------------------------- subscribers */
cam_sub_t *cam_source_subscribe(cam_source_t *src, const char *label) {
    cam_source_start(src);
    cam_sub_t *s = (cam_sub_t *)calloc(1, sizeof(cam_sub_t));
    s->cap = CAM_QUEUE_SIZE;
    s->ring = (au_buf_t **)calloc((size_t)s->cap, sizeof(au_buf_t *));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    snprintf(s->label, sizeof s->label, "%s", label ? label : "");

    pthread_mutex_lock(&src->subs_mu);
    src->subs[src->n_subs++] = s;
    int n = src->n_subs;
    pthread_mutex_unlock(&src->subs_mu);
    LOGI("client connect [%s] -> %d client(s)", s->label, n);
    return s;
}

void cam_source_unsubscribe(cam_source_t *src, cam_sub_t *sub) {
    pthread_mutex_lock(&src->subs_mu);
    int n = 0;
    for (int i = 0; i < src->n_subs; i++) {
        if (src->subs[i] == sub) {
            for (int j = i; j + 1 < src->n_subs; j++) src->subs[j] = src->subs[j + 1];
            src->n_subs--;
            i--;
        }
    }
    n = src->n_subs;
    pthread_mutex_unlock(&src->subs_mu);
    LOGI("client disconnect [%s] (dropped=%d) -> %d client(s)", sub->label, sub->dropped, n);

    pthread_mutex_lock(&sub->mu);
    for (int i = 0; i < sub->count; i++)
        au_buf_release(sub->ring[(sub->head + i) % sub->cap]);
    pthread_mutex_unlock(&sub->mu);
    pthread_mutex_destroy(&sub->mu);
    pthread_cond_destroy(&sub->cv);
    free(sub->ring);
    free(sub);
}

static void sub_push(cam_sub_t *s, au_buf_t *item) {
    pthread_mutex_lock(&s->mu);
    if (s->count == s->cap) {
        /* drop-oldest */
        int idx = s->head;
        au_buf_release(s->ring[idx]);
        s->head = (s->head + 1) % s->cap;
        s->count--;
        s->dropped++;
    }
    int tail = (s->head + s->count) % s->cap;
    s->ring[tail] = item; /* takes the ref the caller already added for us */
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

au_buf_t *cam_sub_get(cam_sub_t *sub, double timeout_sec) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)timeout_sec;
    deadline.tv_nsec += (long)((timeout_sec - (long)timeout_sec) * 1e9);
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }

    pthread_mutex_lock(&sub->mu);
    while (sub->count == 0) {
        int rc = pthread_cond_timedwait(&sub->cv, &sub->mu, &deadline);
        if (rc != 0) { pthread_mutex_unlock(&sub->mu); return NULL; }
    }
    au_buf_t *item = sub->ring[sub->head];
    sub->head = (sub->head + 1) % sub->cap;
    sub->count--;
    pthread_mutex_unlock(&sub->mu);
    return item;
}

static void fanout(cam_source_t *src, int n_nals, const uint8_t *const *nals,
                    const size_t *lens, uint32_t ts, int is_idr) {
    pthread_mutex_lock(&src->subs_mu);
    int n = src->n_subs;
    cam_sub_t *targets[64];
    memcpy(targets, src->subs, (size_t)n * sizeof(cam_sub_t *));
    pthread_mutex_unlock(&src->subs_mu);
    if (n == 0)
        return;

    au_buf_t *item = au_buf_new(n_nals, nals, lens, ts);
    if (!item)
        return;

    for (int i = 0; i < n; i++) {
        cam_sub_t *s = targets[i];
        pthread_mutex_lock(&s->mu);
        int started = s->started;
        if (!started && is_idr) { s->started = 1; started = 1; }
        pthread_mutex_unlock(&s->mu);
        if (!started)
            continue; /* hold this client until a clean GOP start */
        au_buf_addref(item);
        sub_push(s, item);
    }
    au_buf_release(item); /* release the fanout's own temp ref */
}

/* ------------------------------------------------------------------- params */
static void observe_params(cam_source_t *src, const uint8_t *nal, size_t len, int type) {
    if (len > CAM_MAX_PARAM_LEN)
        return; /* pathological; ignore rather than overflow */
    pthread_mutex_lock(&src->params.mu);
    if (type == 7) {
        if (src->params.sps_len != (int)len || memcmp(src->params.sps, nal, len) != 0) {
            memcpy(src->params.sps, nal, len);
            src->params.sps_len = (int)len;
            int w = 0, h = 0;
            if (h264_sps_get_resolution(nal, len, &w, &h)) {
                if (w != src->params.width || h != src->params.height)
                    LOGI("reader: SPS resolution %dx%d", w, h);
                src->params.width = w;
                src->params.height = h;
            }
            pthread_cond_broadcast(&src->params.cv);
        }
    } else if (type == 8) {
        if (src->params.pps_len != (int)len || memcmp(src->params.pps, nal, len) != 0) {
            memcpy(src->params.pps, nal, len);
            src->params.pps_len = (int)len;
            pthread_cond_broadcast(&src->params.cv);
        }
    }
    pthread_mutex_unlock(&src->params.mu);
}

void cam_source_get_params(cam_source_t *src, uint8_t *sps, int *sps_len,
                            uint8_t *pps, int *pps_len) {
    cam_source_start(src);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)CAM_PARAM_WAIT_SEC;

    pthread_mutex_lock(&src->params.mu);
    while (src->params.sps_len == 0 || src->params.pps_len == 0) {
        if (src->stop) break;
        int rc = pthread_cond_timedwait(&src->params.cv, &src->params.mu, &deadline);
        if (rc != 0) break; /* timed out: return whatever we have */
    }
    *sps_len = src->params.sps_len;
    if (*sps_len) memcpy(sps, src->params.sps, (size_t)*sps_len);
    *pps_len = src->params.pps_len;
    if (*pps_len) memcpy(pps, src->params.pps, (size_t)*pps_len);
    pthread_mutex_unlock(&src->params.mu);
}

int cam_source_get_resolution(cam_source_t *src, int *width, int *height) {
    pthread_mutex_lock(&src->params.mu);
    int known = (src->params.width > 0 && src->params.height > 0);
    if (known) {
        *width = src->params.width;
        *height = src->params.height;
    }
    pthread_mutex_unlock(&src->params.mu);
    return known;
}

/* ------------------------------------------------------------- reader thread */
/* Per-AU accumulation while walking the NALs of one reassembled video frame:
 * split off SPS/PPS (observed but not forwarded in the AU itself -- the RTSP
 * layer injects the latest params ahead of every IDR, exactly like Python's
 * `p = src.get_params()` on `is_idr`), keep the rest as "media" NALs. */
typedef struct {
    cam_source_t *src;
    const uint8_t *media_ptr[CAM_MAX_NALS_PER_AU];
    size_t media_len[CAM_MAX_NALS_PER_AU];
    int n_media;
    int is_idr;
} au_accum_t;

static void nal_visit(const uint8_t *nal, size_t len, void *ctx) {
    au_accum_t *a = (au_accum_t *)ctx;
    int t = vsc_nal_type(nal, len);
    if (t == 7 || t == 8) {
        observe_params(a->src, nal, len, t);
        return;
    }
    if (t == 5)
        a->is_idr = 1;
    if (a->n_media < CAM_MAX_NALS_PER_AU) {
        a->media_ptr[a->n_media] = nal;
        a->media_len[a->n_media] = len;
        a->n_media++;
    }
}

typedef struct {
    cam_source_t *src;
    vsc_reassembler_t *reassembler;
    uint32_t *synth_ts;
    uint32_t ts_step;
    long *au_this_conn;
} frame_visit_ctx_t;

static void frame_visit(const vsc_frame_t *f, void *ctx) {
    frame_visit_ctx_t *c = (frame_visit_ctx_t *)ctx;
    if (!vsc_payload_is_video(f->payload, f->payload_len))
        return; /* audio/metadata dropped for now, matches Python */

    au_accum_t acc;
    memset(&acc, 0, sizeof acc);
    acc.src = c->src;
    vsc_iter_annexb_nals(f->payload, f->payload_len, nal_visit, &acc);
    if (acc.n_media == 0)
        return;

    uint32_t ts = *c->synth_ts;
    *c->synth_ts += c->ts_step;
    fanout(c->src, acc.n_media, acc.media_ptr, acc.media_len, ts, acc.is_idr);
    (*c->au_this_conn)++;
}

/* Sends the fixed livestream.cgi GET and peeks the first bytes to tell a
 * successful raw-stream start (leading VSC magic) from an HTTP-wrapped error
 * response, exactly like _handshake() in okam_rtsp_proxy.py. On success,
 * out_head/out_head_len return the already-read bytes that must be fed to
 * the reassembler (they may contain the start of frame data). Returns a
 * connected socket fd, or -1 on any failure (logged). */
static int handshake(const okam_config_t *cfg, uint8_t *out_head, size_t *out_head_len, size_t head_cap) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg->cam_port);

    /* cam_host is "127.0.0.1" in the only deployment this daemon targets;
     * try the cheap dotted-quad path first (avoids pulling in the resolver
     * for the common case) and only fall back to getaddrinfo for a hostname. */
    if (inet_pton(AF_INET, cfg->cam_host, &sa.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%d", cfg->cam_port);
        if (getaddrinfo(cfg->cam_host, portstr, &hints, &res) != 0 || !res) {
            LOGE("reader: could not resolve cam_host %s", cfg->cam_host);
            return -1;
        }
        memcpy(&sa, res->ai_addr, sizeof sa);
        freeaddrinfo(res);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        LOGE("reader: connect %s:%d failed: %s", cfg->cam_host, cfg->cam_port, strerror(errno));
        close(fd); return -1;
    }

    char req[512];
    int qn = snprintf(req, sizeof req,
        "GET /livestream.cgi?loginuse=%s&loginpas=%s&user=%s&pwd=%s"
        "&vuid=%s&streamid=%d&substream=%d&audiostream=0&filename= HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n\r\n",
        "admin", cfg->devpw, "admin", cfg->devpw,
        cfg->vuid, cfg->streamid, cfg->substream, cfg->cam_host);
    if (write(fd, req, (size_t)qn) != qn) {
        LOGE("reader: request write failed: %s", strerror(errno));
        close(fd); return -1;
    }

    static const uint8_t magic_le[4] = { 0x55, 0xAA, 0x15, 0xA8 };
    size_t got = 0;
    while (got < 4) {
        ssize_t n = read(fd, out_head + got, head_cap - got);
        if (n <= 0) {
            LOGE("reader: server closed before sending any data "
                 "(check devpw/vuid/streamid=10/substream=2): %s",
                 n < 0 ? strerror(errno) : "EOF");
            close(fd); return -1;
        }
        got += (size_t)n;
    }
    if (memcmp(out_head, magic_le, 4) == 0) {
        *out_head_len = got;
        return fd; /* raw stream started immediately: success */
    }

    /* HTTP-wrapped error: drain a little more (bounded) and log it. */
    while (got < head_cap - 1) {
        ssize_t n = read(fd, out_head + got, head_cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        int found = 0;
        for (size_t i = 0; i + 4 <= got; i++) {
            if (out_head[i] == '\r' && out_head[i + 1] == '\n' &&
                out_head[i + 2] == '\r' && out_head[i + 3] == '\n') { found = 1; break; }
        }
        if (found) break;
    }
    out_head[got < head_cap ? got : head_cap - 1] = '\0';
    LOGE("reader: livestream rejected (no frame magic). first bytes=%02x%02x%02x%02x body=%.120s",
         out_head[0], out_head[1], out_head[2], out_head[3], out_head);
    close(fd);
    return -1;
}

static void *reader_loop(void *arg) {
    cam_source_t *src = (cam_source_t *)arg;
    vsc_reassembler_t r;
    vsc_init(&r);

    double fps = src->cfg.fps > 0 ? src->cfg.fps : 15.0;
    uint32_t ts_step = (uint32_t)(RTP_CLOCK_HZ / fps);
    uint32_t synth_ts = 0;
    long consecutive_fail = 0;
    double backoff = 0.3;
    const double MAX_BACKOFF = 5.0;
    double gap_start = -1;

    long stat_au = 0;
    double stat_start = monotonic_seconds();
    const double STATS_EVERY = 5.0;

    while (!src->stop) {
        long au_this_conn = 0;
        uint8_t head[8192];
        size_t head_len = 0;
        int fd = handshake(&src->cfg, head, &head_len, sizeof head);
        if (fd >= 0) {
            LOGI("reader: camera stream opened");
            frame_visit_ctx_t ctx = { src, &r, &synth_ts, ts_step, &au_this_conn };
            vsc_feed(&r, head, head_len, frame_visit, &ctx);
            /* count AUs produced from the handshake head bytes too */
            uint8_t buf[65536];
            for (;;) {
                if (src->stop) break;
                ssize_t n = read(fd, buf, sizeof buf);
                if (n <= 0) {
                    LOGW("reader: camera stream ended (EOF)");
                    break;
                }
                long before = au_this_conn;
                vsc_feed(&r, buf, (size_t)n, frame_visit, &ctx);
                stat_au += (au_this_conn - before);
                if (gap_start >= 0 && au_this_conn > before) {
                    LOGI("reader: reconnect gap %.2fs (first AU after EOF)",
                         monotonic_seconds() - gap_start);
                    gap_start = -1;
                }
                double now = monotonic_seconds();
                if (now - stat_start >= STATS_EVERY) {
                    pthread_mutex_lock(&src->subs_mu);
                    int nsub = src->n_subs;
                    pthread_mutex_unlock(&src->subs_mu);
                    LOGI("reader: %ld AU in %.1fs (%.1f fps), %d client(s)",
                         stat_au, now - stat_start, (double)stat_au / (now - stat_start), nsub);
                    stat_au = 0;
                    stat_start = now;
                }
            }
            close(fd);
        }

        if (src->stop)
            break;

        if (au_this_conn > 0) consecutive_fail = 0;
        else consecutive_fail++;

        double this_backoff = backoff * (double)(1L << (consecutive_fail > 10 ? 10 : consecutive_fail));
        if (this_backoff > MAX_BACKOFF) this_backoff = MAX_BACKOFF;
        vsc_reset(&r); /* fresh resync state after a drop */
        gap_start = monotonic_seconds();
        LOGI("reader: reconnecting in %.2fs (consecutive_fail=%ld)", this_backoff, consecutive_fail);
        sleep_seconds(this_backoff);
    }

    vsc_free(&r);
    LOGI("reader: stopped");
    return NULL;
}

void cam_source_init(cam_source_t *src, const okam_config_t *cfg) {
    memset(src, 0, sizeof *src);
    src->cfg = *cfg;
    pthread_mutex_init(&src->subs_mu, NULL);
    pthread_mutex_init(&src->params.mu, NULL);
    pthread_cond_init(&src->params.cv, NULL);
}

void cam_source_start(cam_source_t *src) {
    if (src->started)
        return;
    src->started = 1;
    pthread_create(&src->reader_thread, NULL, reader_loop, src);
}

void cam_source_stop(cam_source_t *src) {
    src->stop = 1;
}
