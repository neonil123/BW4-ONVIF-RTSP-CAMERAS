/* audio_source — the microphone hub, the audio-track sibling of cam_source.
 *
 * A separate LD_PRELOAD shim (mic_capture.so) running inside the camera app
 * captures the mic and sends it as UDP datagrams to 127.0.0.1:5599. Each
 * datagram is one audio frame: raw signed 16-bit little-endian PCM, 16000 Hz,
 * mono, 1280 bytes = 640 samples = 40 ms (~25 datagrams/sec). If no shim is
 * running, nothing arrives and this hub simply stays idle -- video keeps
 * working, no audio is served, nothing blocks or crashes.
 *
 * ONE reader thread binds the UDP socket, downsamples 16k->8k (2:1 averaging
 * low-pass + decimate) and mu-law encodes to G.711 PCMU bytes (1 byte/sample),
 * then fans fixed 160-byte (20 ms) chunks out to every subscribed RTSP audio
 * stream thread's own bounded ring (drop-oldest on overflow, so a slow client
 * never stalls the reader or the video path). Mirrors cam_source's pub/sub.
 */
#ifndef OKAM_AUDIO_SOURCE_H
#define OKAM_AUDIO_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define AUD_UDP_HOST      "127.0.0.1"
#define AUD_UDP_PORT      5599
#define AUD_CHUNK_BYTES   160   /* 20 ms @ 8 kHz PCMU = 160 samples = 160 bytes */
#define AUD_QUEUE_CHUNKS  50    /* ~1 s of slack before drop-oldest */
#define AUD_MAX_SUBS      8

/* One subscriber: a bounded ring of fixed 160-byte mu-law chunks. */
typedef struct {
    uint8_t ring[AUD_QUEUE_CHUNKS][AUD_CHUNK_BYTES];
    int cap, head, count;
    int dropped;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    char label[40];
} audio_sub_t;

typedef struct audio_source {
    char host[64];
    int  port;

    pthread_mutex_t subs_mu;
    audio_sub_t *subs[AUD_MAX_SUBS];
    int n_subs;

    pthread_t reader_thread;
    int started;
    volatile int stop;

    pthread_mutex_t rx_mu;
    double last_rx;      /* monotonic time of the last datagram (0 = never) */
} audio_source_t;

void audio_source_init(audio_source_t *src, const char *host, int port);
void audio_source_start(audio_source_t *src);   /* idempotent */
void audio_source_stop(audio_source_t *src);

/* 1 if a mic datagram arrived within the last ~1.5 s, else 0. Does not block. */
int audio_source_flowing(audio_source_t *src);

/* Registers a new subscriber; starts the reader thread if not already
 * running. Returned pointer is owned by the hub -- call
 * audio_source_unsubscribe when done. */
audio_sub_t *audio_source_subscribe(audio_source_t *src, const char *label);
void audio_source_unsubscribe(audio_source_t *src, audio_sub_t *sub);

/* Blocking pop with a timeout (seconds). On success copies the next 160-byte
 * mu-law chunk into out[0..AUD_CHUNK_BYTES) and returns 1; returns 0 on
 * timeout (no audio available -- the caller should just idle). */
int audio_sub_get(audio_sub_t *sub, double timeout_sec, uint8_t *out);

#endif /* OKAM_AUDIO_SOURCE_H */
