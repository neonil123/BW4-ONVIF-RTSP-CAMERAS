/* okam_onvifd — RTSP + ONVIF re-server for the O-KAM QC3 (Ingenic T23N).
 *
 * Opens the camera's own local H.264 source (livestream.cgi on 127.0.0.1:81),
 * re-serves it as standards RTSP on :554 (/live), and makes the camera
 * ONVIF-discoverable (WS-Discovery + a minimal Device/Media SOAP service on
 * :80, the standard ONVIF port) so any Profile-S NVR can add it natively --
 * no PC middleware.
 *
 * This is a line-for-line architectural port of the already-tested Python
 * prototype (tools/vstarcam_frame.py + tools/rtsp_server.py +
 * tools/okam_rtsp_proxy.py); see builds/features/onvif_rtsp/README.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include "config.h"
#include "util.h"
#include "cam_source.h"
#include "audio_source.h"
#include "speaker_sink.h"
#include "rtsp_server.h"
#include "onvif_wsd.h"
#include "onvif_soap.h"

static cam_source_t   g_cam;
static audio_source_t g_audio;
static speaker_sink_t g_spk;
static rtsp_server_t  g_rtsp;
static onvif_wsd_t    g_wsd;
static onvif_soap_t   g_soap;

static void on_signal(int sig) {
    (void)sig;
    rtsp_server_stop(&g_rtsp);
    onvif_wsd_stop(&g_wsd);
    onvif_soap_stop(&g_soap);
    cam_source_stop(&g_cam);
    audio_source_stop(&g_audio);
    speaker_sink_stop(&g_spk);
}

static void *wsd_thread_entry(void *arg) { onvif_wsd_run((onvif_wsd_t *)arg); return NULL; }
static void *soap_thread_entry(void *arg) { onvif_soap_run((onvif_soap_t *)arg); return NULL; }

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--conf PATH] [--devpw PW] [--vuid VUID] [--onvif-port N]\n"
        "          [--onvif-user USER] [--onvif-pass PASS]\n"
        "  Config defaults come from /system/etc/okam_onvifd.conf (or\n"
        "  --conf PATH), overridable by env OKAM_DEVPW/OKAM_VUID, overridable\n"
        "  again by --devpw/--vuid/--onvif-port/--onvif-user/--onvif-pass.\n"
        "  See okam_onvifd.conf.example.\n", argv0);
}

int main(int argc, char **argv) {
    const char *conf_path = NULL;
    const char *devpw_override = NULL;
    const char *vuid_override = NULL;
    const char *onvif_user_override = NULL;
    const char *onvif_pass_override = NULL;
    int onvif_port_override = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf_path = argv[++i];
        else if (!strcmp(argv[i], "--devpw") && i + 1 < argc) devpw_override = argv[++i];
        else if (!strcmp(argv[i], "--vuid") && i + 1 < argc) vuid_override = argv[++i];
        else if (!strcmp(argv[i], "--onvif-port") && i + 1 < argc) onvif_port_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--onvif-user") && i + 1 < argc) onvif_user_override = argv[++i];
        else if (!strcmp(argv[i], "--onvif-pass") && i + 1 < argc) onvif_pass_override = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    okam_config_t cfg;
    config_load(&cfg, conf_path);
    if (devpw_override) snprintf(cfg.devpw, sizeof cfg.devpw, "%s", devpw_override);
    if (vuid_override) snprintf(cfg.vuid, sizeof cfg.vuid, "%s", vuid_override);
    if (onvif_port_override) cfg.onvif_port = onvif_port_override;
    if (onvif_user_override) snprintf(cfg.onvif_user, sizeof cfg.onvif_user, "%s", onvif_user_override);
    if (onvif_pass_override) snprintf(cfg.onvif_pass, sizeof cfg.onvif_pass, "%s", onvif_pass_override);

    log_set_level(cfg.log_level);

    if (!cfg.devpw[0])
        LOGW("no devpw configured (conf file / OKAM_DEVPW / --devpw) -- "
             "the camera livestream.cgi handshake will fail until it is set");

    LOGI("okam_onvifd starting: cam=%s:%d streamid=%d/%d rtsp=:%d/%s onvif=:%d",
         cfg.cam_host, cfg.cam_port, cfg.streamid, cfg.substream,
         cfg.rtsp_port, cfg.rtsp_name, cfg.onvif_port);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* a client vanishing mid-write must not kill the daemon */

    cam_source_init(&g_cam, &cfg);
    cam_source_start(&g_cam);

    /* Mic hub for the optional PCMU audio track: binds 127.0.0.1:5599 and
     * waits for the mic_capture.so shim's S16LE/16k datagrams. Harmless (and
     * idle) if the shim is never running -- the stream stays video-only. */
    audio_source_init(&g_audio, AUD_UDP_HOST, AUD_UDP_PORT);
    audio_source_start(&g_audio);

    /* Talk-back ingress: receive G.711 mu-law/8k on UDP :5601, upsample to
     * 16k, and forward to the speaker_feed.so shim on 127.0.0.1:5600 which
     * plays it via IMP_AO. Idle until talk audio actually arrives. */
    speaker_sink_init(&g_spk, 5601, "127.0.0.1", 5600);
    speaker_sink_start(&g_spk);

    char device_uuid[40];
    gen_uuid(device_uuid); /* stable for this process's lifetime; shared by WSD + SOAP */

    onvif_wsd_init(&g_wsd, cfg.onvif_port, cfg.manufacturer, cfg.model, device_uuid);
    pthread_t wsd_th;
    pthread_create(&wsd_th, NULL, wsd_thread_entry, &g_wsd);
    pthread_detach(wsd_th);

    onvif_soap_init(&g_soap, &cfg, device_uuid, &g_cam);
    pthread_t soap_th;
    pthread_create(&soap_th, NULL, soap_thread_entry, &g_soap);
    pthread_detach(soap_th);

    rtsp_server_init(&g_rtsp, &g_cam, &g_audio, cfg.rtsp_port, cfg.rtsp_name);
    rtsp_server_serve_forever(&g_rtsp); /* blocks until rtsp_server_stop() */

    LOGI("okam_onvifd: shutting down");
    return 0;
}
