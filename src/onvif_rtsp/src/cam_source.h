/* cam_source — the camera hub. C port of VStarcamStreamSource in
 * tools/cam_rtsp_proxy.py: ONE reader thread opens the camera's local
 * livestream.cgi, reassembles VStarcam frames into H.264 access units
 * (synthesized monotonic 90 kHz timestamps), tracks the latest SPS/PPS, and
 * fans each AU out to every subscribed RTSP client's own bounded queue
 * (drop-oldest on overflow so a slow/stuck client can never stall the
 * reader). Reconnects on EOF/error with capped exponential backoff, exactly
 * like the Python reference (the device EOFs livestream.cgi every ~123 s).
 */
#ifndef CAM_CAM_SOURCE_H
#define CAM_CAM_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "config.h"

#define CAM_MAX_NALS_PER_AU 8
#define CAM_QUEUE_SIZE      120   /* ~8s @ 15fps of slack before drop-oldest */
#define CAM_PARAM_WAIT_SEC  10.0  /* max time get_params() blocks for first IDR */

/* One reassembled access unit, refcounted so the fan-out can hand the same
 * allocation to N subscriber queues without copying (an IDR AU can be
 * several tens of KB). Freed when the refcount hits 0. */
typedef struct au_buf {
    int      refcount;     /* protected by g_aubuf_mutex in the .c file */
    uint32_t ts;
    int      n_nals;
    size_t   nal_len[CAM_MAX_NALS_PER_AU];
    uint8_t *nal_ptr[CAM_MAX_NALS_PER_AU]; /* point into data[] below */
    uint8_t  data[];
} au_buf_t;

void au_buf_release(au_buf_t *b);

typedef struct {
    au_buf_t **ring;
    int cap, head, count;
    int started;   /* gate: only accept AUs starting at the next IDR */
    int dropped;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    char label[40];
} cam_sub_t;

#define CAM_MAX_PARAM_LEN 256   /* generous headroom over a typical <=32-byte SPS/PPS */

typedef struct {
    uint8_t sps[CAM_MAX_PARAM_LEN]; int sps_len;
    uint8_t pps[CAM_MAX_PARAM_LEN]; int pps_len;
    int width, height;   /* parsed from the SPS (h264_sps.c); 0 = not yet known */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} cam_params_t;

typedef struct cam_source {
    cam_config_t cfg;

    cam_params_t params;

    pthread_mutex_t subs_mu;
    cam_sub_t *subs[64];
    int n_subs;

    pthread_t reader_thread;
    int started;
    volatile int stop;
} cam_source_t;

void cam_source_init(cam_source_t *src, const cam_config_t *cfg);
void cam_source_start(cam_source_t *src);   /* idempotent */
void cam_source_stop(cam_source_t *src);

/* Blocks up to CAM_PARAM_WAIT_SEC for the first SPS/PPS, then returns
 * immediately on every later call (matches get_params()). Copies into the
 * caller's buffers; returns the SPS/PPS lengths (0 if still unknown). */
void cam_source_get_params(cam_source_t *src, uint8_t *sps, int *sps_len,
                            uint8_t *pps, int *pps_len);

/* Actual coded picture resolution parsed from the live SPS (see
 * h264_sps.c). Returns 1 and fills width/height if known yet, 0 if no SPS
 * has been observed/parsed successfully so far (caller should fall back to
 * its configured default). Does not block. */
int cam_source_get_resolution(cam_source_t *src, int *width, int *height);

/* Registers a new subscriber; starts the reader thread if not already
 * running. Returned pointer is owned by the hub -- call cam_source_unsubscribe
 * when done. */
cam_sub_t *cam_source_subscribe(cam_source_t *src, const char *label);
void cam_source_unsubscribe(cam_source_t *src, cam_sub_t *sub);

/* Blocking pop with a timeout (seconds); returns NULL on timeout, or an
 * au_buf_t* the caller must release with au_buf_release() when done. */
au_buf_t *cam_sub_get(cam_sub_t *sub, double timeout_sec);

#endif /* CAM_CAM_SOURCE_H */
