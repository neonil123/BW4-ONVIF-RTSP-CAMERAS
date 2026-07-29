#include "audio_source.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* --------------------------------------------------------------- G.711 mu-law
 * Standard ITU-T G.711 mu-law encoder (write-it-yourself, no LUT): sign +
 * 3-bit exponent + 4-bit mantissa of the biased magnitude, then invert. `s`
 * is taken at int width so negating INT16_MIN can never overflow. */
static uint8_t mulaw_encode(int s) {
    const int BIAS = 0x84;    /* 132 */
    const int CLIP = 32635;
    int sign = (s < 0) ? 0x80 : 0x00;
    if (sign) s = -s;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    int exponent = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exponent > 0; exponent--, mask >>= 1)
        ;
    int mantissa = (s >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

/* ------------------------------------------------------------- subscribers */
audio_sub_t *audio_source_subscribe(audio_source_t *src, const char *label) {
    audio_source_start(src);
    audio_sub_t *s = (audio_sub_t *)calloc(1, sizeof(audio_sub_t));
    s->cap = AUD_QUEUE_CHUNKS;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    snprintf(s->label, sizeof s->label, "%s", label ? label : "");

    pthread_mutex_lock(&src->subs_mu);
    int n = -1;
    if (src->n_subs < AUD_MAX_SUBS) {
        src->subs[src->n_subs++] = s;
        n = src->n_subs;
    }
    pthread_mutex_unlock(&src->subs_mu);
    if (n < 0) {
        /* over the (generous) subscriber cap -- never happens with 1-2
         * clients, but degrade gracefully rather than overflow the array. */
        pthread_mutex_destroy(&s->mu);
        pthread_cond_destroy(&s->cv);
        free(s);
        return NULL;
    }
    LOGI("audio: client subscribe [%s] -> %d listener(s)", s->label, n);
    return s;
}

void audio_source_unsubscribe(audio_source_t *src, audio_sub_t *sub) {
    if (!sub) return;
    pthread_mutex_lock(&src->subs_mu);
    for (int i = 0; i < src->n_subs; i++) {
        if (src->subs[i] == sub) {
            for (int j = i; j + 1 < src->n_subs; j++) src->subs[j] = src->subs[j + 1];
            src->n_subs--;
            i--;
        }
    }
    int n = src->n_subs;
    pthread_mutex_unlock(&src->subs_mu);
    LOGI("audio: client unsubscribe [%s] (dropped=%d) -> %d listener(s)",
         sub->label, sub->dropped, n);
    pthread_mutex_destroy(&sub->mu);
    pthread_cond_destroy(&sub->cv);
    free(sub);
}

static void sub_push(audio_sub_t *s, const uint8_t *chunk) {
    pthread_mutex_lock(&s->mu);
    if (s->count == s->cap) {
        /* drop-oldest: a stuck listener must never stall the reader/video */
        s->head = (s->head + 1) % s->cap;
        s->count--;
        s->dropped++;
    }
    int tail = (s->head + s->count) % s->cap;
    memcpy(s->ring[tail], chunk, AUD_CHUNK_BYTES);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

int audio_sub_get(audio_sub_t *sub, double timeout_sec, uint8_t *out) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)timeout_sec;
    deadline.tv_nsec += (long)((timeout_sec - (long)timeout_sec) * 1e9);
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }

    pthread_mutex_lock(&sub->mu);
    while (sub->count == 0) {
        int rc = pthread_cond_timedwait(&sub->cv, &sub->mu, &deadline);
        if (rc != 0) { pthread_mutex_unlock(&sub->mu); return 0; }
    }
    memcpy(out, sub->ring[sub->head], AUD_CHUNK_BYTES);
    sub->head = (sub->head + 1) % sub->cap;
    sub->count--;
    pthread_mutex_unlock(&sub->mu);
    return 1;
}

static void fanout(audio_source_t *src, const uint8_t *chunk) {
    /* Push while holding subs_mu so a concurrent audio_source_unsubscribe()
     * cannot remove+free a subscriber between us reading the array and pushing
     * to it (that was a use-after-free). sub_push never blocks (drop-oldest, no
     * cond wait), and it takes only the per-sub lock, so holding subs_mu across
     * the loop is cheap and deadlock-free (lock order is always subs_mu -> sub->mu). */
    pthread_mutex_lock(&src->subs_mu);
    for (int i = 0; i < src->n_subs; i++)
        sub_push(src->subs[i], chunk);
    pthread_mutex_unlock(&src->subs_mu);
}

/* ------------------------------------------------------------- reader thread */
static int open_udp(audio_source_t *src) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("audio: socket() failed: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct timeval tv = { 0, 500000 }; /* 0.5 s so the loop can notice stop */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)src->port);
    if (inet_pton(AF_INET, src->host, &sa.sin_addr) != 1)
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        LOGE("audio: bind %s:%d failed: %s", src->host, src->port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void *reader_loop(void *arg) {
    audio_source_t *src = (audio_source_t *)arg;
    int fd = open_udp(src);
    if (fd < 0) {
        LOGW("audio: mic UDP receiver disabled (bind failed) -- serving video only");
        return NULL;
    }
    LOGI("audio: listening for mic PCM on %s:%d (S16LE/16k mono -> 2:1 downsample -> PCMU/8k)", src->host, src->port);

    uint8_t buf[4096];
    uint8_t acc[AUD_CHUNK_BYTES];
    int acc_len = 0;
    int  have_prev = 0;   /* carry one S16 sample across datagram boundaries */
    int  prev_s = 0;      /* so 2:1 decimation pairs are never split */
    long stat_frames = 0;
    double stat_start = monotonic_seconds();
    const double STATS_EVERY = 10.0;

    while (!src->stop) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; /* idle: no shim sending, or just the 0.5s poll */
            LOGW("audio: recv error: %s", strerror(errno));
            continue;
        }
        if (n < 4)
            continue; /* too small to be a PCM frame */

        pthread_mutex_lock(&src->rx_mu);
        src->last_rx = monotonic_seconds();
        pthread_mutex_unlock(&src->rx_mu);
        stat_frames++;

        /* The IMP AI channel (dev1) delivers S16LE mono whose CONTENT is 16 kHz
         * (acoustic tone-loop measurement: served-as-8k audio came out exactly
         * one octave low, f_out=f_in/2, duration preserved -- the classic
         * 16k-samples-played-at-8k signature). /proc/jz/audio's nominal 16 kHz
         * is the truth; the earlier "12.5 f/s = 8k" reading was a coarse
         * log-tick artifact. So DOWNSAMPLE 2:1 (average adjacent pairs as a
         * cheap anti-alias low-pass, then decimate) to get real-pitch 8 kHz,
         * and mu-law-encode. 16000 samp/s in -> 8000 samp/s out = real-time
         * PCMU/8000, correct pitch, duration preserved, no ring overflow.
         * A single sample is carried across datagram boundaries so pairs are
         * never split; odd trailing bytes are ignored. */
        long in_samples = n / 2;
        for (long k = 0; k < in_samples; k++) {
            int s = (int16_t)((uint16_t)buf[2 * k] | ((uint16_t)buf[2 * k + 1] << 8));
            if (!have_prev) {
                prev_s = s;
                have_prev = 1;
                continue;
            }
            int avg = (prev_s + s) / 2;   /* 2:1 low-pass + decimate */
            have_prev = 0;
            acc[acc_len++] = mulaw_encode(avg);
            if (acc_len == AUD_CHUNK_BYTES) {
                fanout(src, acc);
                acc_len = 0;
            }
        }

        double now = monotonic_seconds();
        if (now - stat_start >= STATS_EVERY) {
            pthread_mutex_lock(&src->subs_mu);
            int nsub = src->n_subs;
            pthread_mutex_unlock(&src->subs_mu);
            LOGI("audio: %ld frames in %.1fs (%.1f/s), %d listener(s)",
                 stat_frames, now - stat_start, (double)stat_frames / (now - stat_start), nsub);
            stat_frames = 0;
            stat_start = now;
        }
    }

    close(fd);
    LOGI("audio: reader stopped");
    return NULL;
}

void audio_source_init(audio_source_t *src, const char *host, int port) {
    memset(src, 0, sizeof *src);
    snprintf(src->host, sizeof src->host, "%s", host && host[0] ? host : AUD_UDP_HOST);
    src->port = port > 0 ? port : AUD_UDP_PORT;
    pthread_mutex_init(&src->subs_mu, NULL);
    pthread_mutex_init(&src->rx_mu, NULL);
}

void audio_source_start(audio_source_t *src) {
    if (src->started)
        return;
    src->started = 1;
    pthread_create(&src->reader_thread, NULL, reader_loop, src);
}

void audio_source_stop(audio_source_t *src) {
    src->stop = 1;
}

int audio_source_flowing(audio_source_t *src) {
    pthread_mutex_lock(&src->rx_mu);
    double last = src->last_rx;
    pthread_mutex_unlock(&src->rx_mu);
    return last > 0 && (monotonic_seconds() - last) < 1.5;
}
