#include "rtsp_server.h"
#include "rtp_h264.h"
#include "rtp_pcmu.h"
#include "audio_source.h"
#include "vstarcam_frame.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define OKAM_SSRC       0xCAFEBABEu   /* video (track0) */
#define OKAM_AUDIO_SSRC 0xCAFED00Du   /* audio (track1), distinct from video */
#define CONN_BUF_CAP 8192
#define REQ_IDLE_TIMEOUT_SEC 30

#define TRACK_VIDEO 0
#define TRACK_AUDIO 1
#define N_TRACKS    2

/* Per-track transport, negotiated by its own SETUP. The client SETUPs each
 * track separately (dispatched by the .../trackN URL suffix); a track the
 * client never SETUPs stays configured==0 and is simply never sent. */
typedef struct {
    int  configured;
    int  has_interleaved;
    int  rtp_ch, rtcp_ch;
    int  has_udp;
    struct sockaddr_in udp_addr;
    int  udp_sock;
} rtsp_track_t;

typedef struct rtsp_session {
    char sid[16];
    rtsp_track_t track[N_TRACKS];
    volatile int playing;
    volatile int stop;
    pthread_t vthread;           /* video stream thread */
    pthread_t athread;           /* audio stream thread (started only if audio SETUP) */
    int  has_vthread;
    int  has_athread;
    int conn_fd;                 /* valid only while the owning connection is alive */
    pthread_mutex_t *send_mu;    /* the owning connection's write lock (TCP-interleaved sends) */
} rtsp_session_t;

typedef struct {
    rtsp_server_t *server;
    int fd;
    pthread_mutex_t send_mu;
} conn_ctx_t;

/* -------------------------------------------------------------- low-level IO */
static int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void conn_send(conn_ctx_t *c, const char *data, size_t len) {
    pthread_mutex_lock(&c->send_mu);
    write_all(c->fd, data, len);
    pthread_mutex_unlock(&c->send_mu);
}

static void session_send_rtp(rtsp_session_t *sess, int tk, const uint8_t *pkt, size_t len) {
    rtsp_track_t *t = &sess->track[tk];
    if (t->has_interleaved && sess->send_mu) {
        uint8_t hdr[4];
        hdr[0] = '$';
        hdr[1] = (uint8_t)t->rtp_ch;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        pthread_mutex_lock(sess->send_mu);
        if (write_all(sess->conn_fd, hdr, 4) == 0)
            write_all(sess->conn_fd, pkt, len);
        pthread_mutex_unlock(sess->send_mu);
    } else if (t->has_udp) {
        sendto(t->udp_sock, pkt, len, 0, (struct sockaddr *)&t->udp_addr, sizeof(t->udp_addr));
    }
}

/* The packetizers' callback is (pkt,len,ctx); adapt to session_send_rtp's
 * (sess,track,pkt,len) so rtp_h264.c/rtp_pcmu.c stay transport-agnostic. */
typedef struct { rtsp_session_t *sess; int track; } send_ctx_t;

static void rtp_cb_bridge(const uint8_t *pkt, size_t len, void *ctx) {
    send_ctx_t *sc = (send_ctx_t *)ctx;
    session_send_rtp(sc->sess, sc->track, pkt, len);
}

/* ------------------------------------------------------------------- replies */
static void reply_ok(conn_ctx_t *c, const char *cseq, const char *extra,
                      const char *body, size_t body_len) {
    char head[1024];
    int n = snprintf(head, sizeof head, "RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s", cseq, extra ? extra : "");
    if (body_len) {
        n += snprintf(head + n, sizeof(head) - (size_t)n,
                       "Content-Type: application/sdp\r\nContent-Length: %zu\r\n", body_len);
    }
    n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
    conn_send(c, head, (size_t)n);
    if (body_len)
        conn_send(c, body, body_len);
}

static void reply_405(conn_ctx_t *c, const char *cseq) {
    char head[128];
    int n = snprintf(head, sizeof head, "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %s\r\n\r\n", cseq);
    conn_send(c, head, (size_t)n);
}

/* ----------------------------------------------------------------------- SDP */
static void build_sdp(rtsp_server_t *rs, char *out, size_t out_cap) {
    uint8_t sps[CAM_MAX_PARAM_LEN], pps[CAM_MAX_PARAM_LEN];
    int sl = 0, pl = 0;
    cam_source_get_params(rs->src, sps, &sl, pps, &pl);

    char profile[8];
    if (sl >= 4)
        snprintf(profile, sizeof profile, "%02x%02x%02x", sps[1], sps[2], sps[3]);
    else
        snprintf(profile, sizeof profile, "%s", "42e01f");

    char sprop[900];
    sprop[0] = '\0';
    if (sl || pl) {
        /* b64_encoded_len(CAM_MAX_PARAM_LEN=256) == 344; +1 for the NUL. */
        char b64sps[352], b64pps[352];
        b64sps[0] = b64pps[0] = '\0';
        if (sl) b64_encode(sps, (size_t)sl, b64sps);
        if (pl) b64_encode(pps, (size_t)pl, b64pps);
        if (sl && pl)
            snprintf(sprop, sizeof sprop, ";sprop-parameter-sets=%s,%s", b64sps, b64pps);
        else
            snprintf(sprop, sizeof sprop, ";sprop-parameter-sets=%s", sl ? b64sps : b64pps);
    }

    /* Video media (track0) is byte-for-byte identical to the pre-audio SDP.
     * The audio media (track1, PCMU/8000) is always advertised so a client's
     * SETUP for track1 is legal; if the mic shim never sends anything the
     * audio stream thread simply idles and the RTP stream carries no audio,
     * which is a valid empty audio track (never breaks the video). */
    snprintf(out, out_cap,
             "v=0\r\n"
             "o=- 0 0 IN IP4 127.0.0.1\r\n"
             "s=okam-rtsp\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H264/90000\r\n"
             "a=fmtp:%d packetization-mode=1;profile-level-id=%s%s\r\n"
             "a=control:track0\r\n"
             "m=audio 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d PCMU/8000\r\n"
             "a=control:track1\r\n",
             RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, profile, sprop,
             RTP_PCMU_PAYLOAD_TYPE, RTP_PCMU_PAYLOAD_TYPE);
}

/* ------------------------------------------------------------- session helpers */
static rtsp_session_t *session_new(void) {
    rtsp_session_t *s = (rtsp_session_t *)calloc(1, sizeof *s);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long ms = (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
    snprintf(s->sid, sizeof s->sid, "%06lx", ms & 0xFFFFFFUL);
    for (int i = 0; i < N_TRACKS; i++)
        s->track[i].udp_sock = -1;
    return s;
}

static void session_free(rtsp_session_t *s) {
    if (!s) return;
    for (int i = 0; i < N_TRACKS; i++)
        if (s->track[i].udp_sock >= 0) close(s->track[i].udp_sock);
    free(s);
}

/* ------------------------------------------------------------ streaming loop */
typedef struct {
    rtsp_session_t *sess;
    rtsp_server_t *rs;
} stream_arg_t;

static void *stream_thread(void *arg_) {
    stream_arg_t *arg = (stream_arg_t *)arg_;
    rtsp_session_t *sess = arg->sess;
    rtsp_server_t *rs = arg->rs;
    free(arg);

    cam_sub_t *sub = cam_source_subscribe(rs->src, sess->sid);
    send_ctx_t sc = { sess, TRACK_VIDEO };
    uint16_t seq = 0;
    double start = monotonic_seconds();
    int have_base = 0;
    uint32_t base_ts = 0;

    while (!sess->stop) {
        au_buf_t *item = cam_sub_get(sub, 0.5);
        if (!item)
            continue;

        if (!have_base) { base_ts = item->ts; have_base = 1; }
        double target = start + (double)(item->ts - base_ts) / (double)RTP_CLOCK_HZ;
        double delay = target - monotonic_seconds();
        if (delay > 0) sleep_seconds(delay);

        int is_idr = 0;
        for (int i = 0; i < item->n_nals; i++)
            if (vsc_nal_type(item->nal_ptr[i], item->nal_len[i]) == 5) { is_idr = 1; break; }

        const uint8_t *nal_ptrs[CAM_MAX_NALS_PER_AU + 2];
        size_t nal_lens[CAM_MAX_NALS_PER_AU + 2];
        int n = 0;
        uint8_t sps[CAM_MAX_PARAM_LEN], pps[CAM_MAX_PARAM_LEN];
        int sl = 0, pl = 0;
        if (is_idr) {
            cam_source_get_params(rs->src, sps, &sl, pps, &pl); /* already known: returns immediately */
            if (sl) { nal_ptrs[n] = sps; nal_lens[n] = (size_t)sl; n++; }
            if (pl) { nal_ptrs[n] = pps; nal_lens[n] = (size_t)pl; n++; }
        }
        for (int i = 0; i < item->n_nals && n < CAM_MAX_NALS_PER_AU + 2; i++) {
            nal_ptrs[n] = item->nal_ptr[i];
            nal_lens[n] = item->nal_len[i];
            n++;
        }

        rtp_packetize_au(nal_ptrs, nal_lens, n, &seq, item->ts, OKAM_SSRC, RTP_DEFAULT_MTU,
                          rtp_cb_bridge, &sc);
        au_buf_release(item);
    }

    cam_source_unsubscribe(rs->src, sub);
    return NULL;
}

/* Audio stream thread: pulls 20 ms mu-law chunks from the mic hub, packetizes
 * each as one PCMU RTP packet, and sends on track1. Pacing is driven by the
 * mic source's real-time arrival (~50 chunks/sec); audio_sub_get blocks up to
 * 0.5 s, so when no mic data is flowing this thread just idles (sends nothing)
 * and never touches or blocks the video path. Started only when the client
 * actually SETUPs track1. */
static void *audio_stream_thread(void *arg_) {
    stream_arg_t *arg = (stream_arg_t *)arg_;
    rtsp_session_t *sess = arg->sess;
    rtsp_server_t *rs = arg->rs;
    free(arg);

    if (!rs->asrc)
        return NULL; /* no mic hub configured -> nothing to serve */

    audio_sub_t *sub = audio_source_subscribe(rs->asrc, sess->sid);
    if (!sub)
        return NULL;
    send_ctx_t sc = { sess, TRACK_AUDIO };
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint8_t chunk[AUD_CHUNK_BYTES];

    while (!sess->stop) {
        if (!audio_sub_get(sub, 0.5, chunk))
            continue; /* idle: no mic audio right now */
        rtp_pcmu_packetize(chunk, sizeof chunk, &seq, &ts, OKAM_AUDIO_SSRC,
                           rtp_cb_bridge, &sc);
    }

    audio_source_unsubscribe(rs->asrc, sub);
    return NULL;
}

/* ------------------------------------------------------------- request parse */
typedef struct { char key[32], val[512]; } hdr_kv_t;

static int find_header(hdr_kv_t *hdrs, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcasecmp(hdrs[i].key, key) == 0) return i;
    return -1;
}

static void dispatch(conn_ctx_t *c, rtsp_server_t *rs, const char *method, const char *url,
                      hdr_kv_t *hdrs, int nhdr, rtsp_session_t **psession) {
    int ci = find_header(hdrs, nhdr, "cseq");
    const char *cseq = ci >= 0 ? hdrs[ci].val : "0";

    if (!strcmp(method, "OPTIONS")) {
        reply_ok(c, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", NULL, 0);

    } else if (!strcmp(method, "DESCRIBE")) {
        char sdp[1600];
        build_sdp(rs, sdp, sizeof sdp);
        char extra[300];
        snprintf(extra, sizeof extra, "Content-Base: %s/\r\n", url);
        reply_ok(c, cseq, extra, sdp, strlen(sdp));

    } else if (!strcmp(method, "SETUP")) {
        if (!*psession) *psession = session_new();
        rtsp_session_t *s = *psession;
        /* One SETUP per track, dispatched by the .../trackN URL suffix
         * (default track0 for clients that don't append one). Video-only
         * behavior is unchanged: a client that only SETUPs track0 leaves
         * track1.configured==0 and no audio thread is ever started. */
        int tk = (url && strstr(url, "track1")) ? TRACK_AUDIO : TRACK_VIDEO;
        rtsp_track_t *tr = &s->track[tk];
        tr->configured = 1;
        int ti = find_header(hdrs, nhdr, "transport");
        const char *transport = ti >= 0 ? hdrs[ti].val : "";
        char extra[256];

        const char *inter = strstr(transport, "interleaved=");
        if (inter) {
            int a = tk == TRACK_AUDIO ? 2 : 0, b = tk == TRACK_AUDIO ? 3 : 1;
            sscanf(inter + strlen("interleaved="), "%d-%d", &a, &b);
            tr->has_interleaved = 1;
            tr->rtp_ch = a; tr->rtcp_ch = b;
            s->conn_fd = c->fd;
            s->send_mu = &c->send_mu;
            snprintf(extra, sizeof extra,
                     "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s\r\n",
                     a, b, s->sid);
        } else {
            const char *cp = strstr(transport, "client_port=");
            int a = 0, b = 0;
            if (cp) sscanf(cp + strlen("client_port="), "%d-%d", &a, &b);
            struct sockaddr_in peer;
            socklen_t pl = sizeof peer;
            getpeername(c->fd, (struct sockaddr *)&peer, &pl);
            tr->has_udp = 1;
            tr->udp_addr = peer;
            tr->udp_addr.sin_port = htons((uint16_t)a);
            tr->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in bindaddr;
            memset(&bindaddr, 0, sizeof bindaddr);
            bindaddr.sin_family = AF_INET;
            socklen_t bl = sizeof bindaddr;
            bind(tr->udp_sock, (struct sockaddr *)&bindaddr, sizeof bindaddr);
            getsockname(tr->udp_sock, (struct sockaddr *)&bindaddr, &bl);
            int srv_port = ntohs(bindaddr.sin_port);
            snprintf(extra, sizeof extra,
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                     "Session: %s\r\n",
                     a, b, srv_port, srv_port + 1, s->sid);
        }
        reply_ok(c, cseq, extra, NULL, 0);

    } else if (!strcmp(method, "PLAY")) {
        rtsp_session_t *s = *psession;
        int audio_on = s && s->track[TRACK_AUDIO].configured;
        char extra[160];
        /* RTP-Info stays exactly "url=track0" for a video-only session; the
         * audio track is only listed when the client actually set it up. */
        snprintf(extra, sizeof extra, "Session: %s\r\nRTP-Info: url=track0%s\r\n",
                 s ? s->sid : "0", audio_on ? ",url=track1" : "");
        reply_ok(c, cseq, extra, NULL, 0);
        if (s) {
            s->playing = 1;
            stream_arg_t *a = (stream_arg_t *)malloc(sizeof *a);
            a->sess = s; a->rs = rs;
            /* NOT detached: handle_conn joins these on teardown before freeing
             * the session/connection, so a thread can never dereference freed
             * session state or lock the destroyed send_mu (was a UAF crash on
             * abrupt client disconnect during PLAY). */
            if (pthread_create(&s->vthread, NULL, stream_thread, a) == 0)
                s->has_vthread = 1;
            else
                free(a);
            if (audio_on) {
                stream_arg_t *aa = (stream_arg_t *)malloc(sizeof *aa);
                aa->sess = s; aa->rs = rs;
                if (pthread_create(&s->athread, NULL, audio_stream_thread, aa) == 0)
                    s->has_athread = 1;
                else
                    free(aa);
            }
        }

    } else if (!strcmp(method, "GET_PARAMETER")) {
        char extra[64];
        snprintf(extra, sizeof extra, "Session: %s\r\n", *psession ? (*psession)->sid : "0");
        reply_ok(c, cseq, extra, NULL, 0);

    } else if (!strcmp(method, "TEARDOWN")) {
        if (*psession) (*psession)->stop = 1;
        reply_ok(c, cseq, "", NULL, 0);

    } else {
        reply_405(c, cseq);
    }
}

/* ------------------------------------------------------------- connection I/O */
static int recv_request(int fd, char *cbuf, size_t *clen, rtsp_session_t *session,
                         char *out_head, size_t out_cap) {
    /* Fill cbuf (persists across calls via *clen) until "\r\n\r\n" appears;
     * a recv timeout is normal during PLAY (matches Python: only a real EOF
     * or error tears the session down, not control-channel idleness). */
    for (;;) {
        for (size_t i = 0; i + 4 <= *clen; i++) {
            if (cbuf[i] == '\r' && cbuf[i + 1] == '\n' && cbuf[i + 2] == '\r' && cbuf[i + 3] == '\n') {
                size_t head_len = i; /* excludes the terminator */
                if (head_len >= out_cap) head_len = out_cap - 1;
                memcpy(out_head, cbuf, head_len);
                out_head[head_len] = '\0';
                size_t consumed = i + 4;
                memmove(cbuf, cbuf + consumed, *clen - consumed);
                *clen -= consumed;
                return (int)head_len;
            }
        }
        if (*clen >= CONN_BUF_CAP - 1)
            return -1; /* request too large; bail */
        ssize_t n = recv(fd, cbuf + *clen, CONN_BUF_CAP - 1 - *clen, 0);
        if (n > 0) {
            *clen += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1; /* EOF */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (session && session->stop)
                return -1;
            continue; /* idle timeout; keep waiting (QA parity with Python) */
        }
        if (errno == EINTR)
            continue;
        return -1; /* real error */
    }
}

static void *handle_conn(void *arg_) {
    conn_ctx_t *c = (conn_ctx_t *)arg_;
    rtsp_session_t *session = NULL;
    char cbuf[CONN_BUF_CAP];
    size_t clen = 0;
    char head[CONN_BUF_CAP];

    struct timeval tv = { REQ_IDLE_TIMEOUT_SEC, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    for (;;) {
        int hl = recv_request(c->fd, cbuf, &clen, session, head, sizeof head);
        if (hl < 0) break;

        /* parse "METHOD url VERSION\r\nKey: val\r\n..." */
        char method[16] = "", url[256] = "";
        const char *p = head;
        const char *nl = strstr(p, "\r\n");
        size_t line0len = nl ? (size_t)(nl - p) : strlen(p);
        {
            char line0[512];
            size_t n = line0len < sizeof line0 - 1 ? line0len : sizeof line0 - 1;
            memcpy(line0, p, n); line0[n] = '\0';
            sscanf(line0, "%15s %255s", method, url);
        }
        hdr_kv_t hdrs[16];
        int nhdr = 0;
        const char *lp = nl ? nl + 2 : (p + line0len);
        while (lp && *lp && nhdr < 16) {
            const char *lend = strstr(lp, "\r\n");
            size_t llen = lend ? (size_t)(lend - lp) : strlen(lp);
            const char *colon = memchr(lp, ':', llen);
            if (colon) {
                size_t klen = (size_t)(colon - lp);
                if (klen >= sizeof hdrs[0].key) klen = sizeof hdrs[0].key - 1;
                memcpy(hdrs[nhdr].key, lp, klen); hdrs[nhdr].key[klen] = '\0';
                const char *vs = colon + 1;
                while (vs < lp + llen && (*vs == ' ' || *vs == '\t')) vs++;
                size_t vlen = (size_t)((lp + llen) - vs);
                if (vlen >= sizeof hdrs[0].val) vlen = sizeof hdrs[0].val - 1;
                memcpy(hdrs[nhdr].val, vs, vlen); hdrs[nhdr].val[vlen] = '\0';
                nhdr++;
            }
            if (!lend) break;
            lp = lend + 2;
        }

        if (!method[0]) break;
        dispatch(c, c->server, method, url, hdrs, nhdr, &session);
    }

    if (session) {
        session->stop = 1;
        /* Join the streaming threads BEFORE freeing the session or the
         * connection: they reference session state and lock sess->send_mu
         * (== &c->send_mu), so they must be fully stopped before any of that
         * is destroyed. Each thread notices stop on its next <=0.5s poll, so
         * this returns promptly. This closes the use-after-free that crashed
         * the daemon when a client dropped mid-PLAY. */
        if (session->has_vthread) pthread_join(session->vthread, NULL);
        if (session->has_athread) pthread_join(session->athread, NULL);
        for (int i = 0; i < N_TRACKS; i++) {
            if (session->track[i].has_udp && session->track[i].udp_sock >= 0) {
                close(session->track[i].udp_sock);
                session->track[i].udp_sock = -1;
            }
        }
        session_free(session);
    }
    close(c->fd);
    pthread_mutex_destroy(&c->send_mu);
    free(c);
    return NULL;
}

static void *accept_loop(void *arg) {
    rtsp_server_t *rs = (rtsp_server_t *)arg;
    while (!rs->stop) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(rs->listen_fd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (rs->stop) break;
            continue;
        }
        conn_ctx_t *c = (conn_ctx_t *)calloc(1, sizeof *c);
        c->server = rs;
        c->fd = fd;
        pthread_mutex_init(&c->send_mu, NULL);
        pthread_t th;
        pthread_create(&th, NULL, handle_conn, c);
        pthread_detach(th);
    }
    return NULL;
}

void rtsp_server_init(rtsp_server_t *s, cam_source_t *src, audio_source_t *asrc,
                      int port, const char *stream_name) {
    memset(s, 0, sizeof *s);
    s->src = src;
    s->asrc = asrc;
    s->port = port;
    snprintf(s->stream_name, sizeof s->stream_name, "%s", stream_name ? stream_name : "live");
    s->listen_fd = -1;
}

int rtsp_server_serve_forever(rtsp_server_t *s) {
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) { LOGE("rtsp: socket() failed: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)s->port);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOGE("rtsp: bind :%d failed: %s", s->port, strerror(errno));
        return -1;
    }
    if (listen(s->listen_fd, 8) != 0) {
        LOGE("rtsp: listen failed: %s", strerror(errno));
        return -1;
    }
    LOGI("rtsp: listening on :%d/%s", s->port, s->stream_name);
    accept_loop(s);
    return 0;
}

void rtsp_server_stop(rtsp_server_t *s) {
    s->stop = 1;
    if (s->listen_fd >= 0) {
        shutdown(s->listen_fd, SHUT_RDWR);
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}
