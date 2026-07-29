/* rtsp_server — C port of tools/rtsp_server.py's RTSPServer, wired directly to
 * a cam_source_t hub (the Python version was source-agnostic via a Source
 * base class; here there is exactly one source, so we skip the vtable).
 *
 * Implements OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN/GET_PARAMETER, SDP with
 * sprop-parameter-sets, RTP over TCP-interleaved ($-framing, RFC 2326) and
 * plain UDP, one thread per accepted control connection plus one streaming
 * thread per PLAYing session.
 */
#ifndef CAM_RTSP_SERVER_H
#define CAM_RTSP_SERVER_H

#include "cam_source.h"
#include "audio_source.h"

typedef struct {
    cam_source_t *src;
    audio_source_t *asrc; /* mic hub for the optional PCMU audio track; may be
                           * NULL -> video-only, exactly as before audio */
    int  port;         /* listen port, e.g. 554 */
    char stream_name[32]; /* e.g. "live" -> rtsp://host:port/live */
    int  listen_fd;
    volatile int stop;
} rtsp_server_t;

void rtsp_server_init(rtsp_server_t *s, cam_source_t *src, audio_source_t *asrc,
                      int port, const char *stream_name);

/* Binds and accepts connections until rtsp_server_stop() is called from
 * another thread. Typically run on its own thread (or as main()'s last call
 * if nothing else needs the calling thread). */
int rtsp_server_serve_forever(rtsp_server_t *s);
void rtsp_server_stop(rtsp_server_t *s);

#endif /* CAM_RTSP_SERVER_H */
