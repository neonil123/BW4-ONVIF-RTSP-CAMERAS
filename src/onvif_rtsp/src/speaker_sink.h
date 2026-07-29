/* speaker_sink -- talk-back ingress for cam_onvifd.
 *
 * Receives G.711 mu-law (PCMU/8000) talk audio from an NVR/app/phone on a UDP
 * port (default 5601), decodes to S16, upsamples 8k->16k (the vendor's own
 * averaging interpolation), reframes to 640-byte (320-sample) S16LE/16k frames,
 * and forwards them over loopback UDP to the speaker_feed.so shim inside
 * vp_project (default 127.0.0.1:5600), which plays them via IMP_AO.
 *
 * Off/idle unless talk audio actually arrives; never touches AO itself. */
#ifndef SPEAKER_SINK_H
#define SPEAKER_SINK_H

#include <pthread.h>

typedef struct {
    int   talk_port;              /* UDP port we RECEIVE mu-law talk on (0.0.0.0) */
    char  feed_host[64];          /* where the shim listens (127.0.0.1) */
    int   feed_port;              /* shim UDP port (5600) */
    volatile int stop;
    volatile int started;
    pthread_t thread;
} speaker_sink_t;

void speaker_sink_init(speaker_sink_t *s, int talk_port, const char *feed_host, int feed_port);
void speaker_sink_start(speaker_sink_t *s);
void speaker_sink_stop(speaker_sink_t *s);

/* Feed already-decoded/8k mu-law payload straight in (used by the ONVIF/RTSP
 * backchannel path, which hands us the RTP PCMU payload after stripping the
 * 12-byte header). Safe to call from any thread. */
void speaker_sink_push_mulaw(speaker_sink_t *s, const unsigned char *mulaw, int n);

#endif
