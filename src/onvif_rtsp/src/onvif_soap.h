/* onvif_soap — minimal ONVIF Device+Media SOAP service on one HTTP endpoint
 * (http://<ip>:<port>/onvif/device_service serves both, matching several
 * real minimal-firmware ONVIF stacks). No XML parser: like the reference
 * onvif_srvd/prudynt-t CGI implementation bundled in
 * builds/prudynt_bundle/onvif, the SOAP action is identified by a substring
 * search on the request body, and the reply is a template with a handful of
 * %s-style fields filled in.
 *
 * Access policy matches real Profile-S cameras: capability-discovery
 * operations (GetDeviceInformation, GetCapabilities, GetServices,
 * GetServiceCapabilities, GetScopes, GetProfiles/GetProfile,
 * GetVideoSources, GetVideo{Source,Encoder}Configuration(s)[Options],
 * GetAudio*, GetSystemDateAndTime, GetHostname, GetWsdlUrl) all succeed
 * WITHOUT authentication -- that's what lets an NVR's add-camera wizard
 * (Synology Surveillance Station confirmed) enumerate/classify the camera
 * before it has credentials. Only the actual media-access operation,
 * GetStreamUri, requires authentication -- ANY of a valid WS-Security
 * UsernameToken (wsse.c), HTTP Digest (RFC 2617, what most NVRs incl.
 * Synology actually use for that step), or HTTP Basic (httpauth.c). An
 * unauthenticated/invalid GetStreamUri gets HTTP 401 with
 * WWW-Authenticate challenges (Digest + Basic) so the client's normal
 * HTTP-auth retry flow engages -- credentials are always accepted
 * additively even on the pre-auth ops, just never required there.
 */
#ifndef OKAM_ONVIF_SOAP_H
#define OKAM_ONVIF_SOAP_H

#include <pthread.h>
#include "config.h"
#include "cam_source.h"
#include "httpauth.h"

typedef struct {
    okam_config_t cfg;
    cam_source_t *src;   /* for the live SPS-derived resolution (h264_sps.c) */
    char uuid[40];
    httpauth_ctx_t httpauth;
    pthread_mutex_t raw_log_mu;
    int raw_log_count;   /* caps how many requests get dumped to /tmp/onvif_raw.log */
    volatile int stop;
    int listen_fd;
} onvif_soap_t;

/* device_uuid: pass the same UUID given to onvif_wsd_init. `src` supplies
 * the live stream resolution (falls back to cfg->video_width/height until
 * the first SPS is observed); pass NULL to always use the configured
 * default (e.g. for standalone testing without a camera hub). */
void onvif_soap_init(onvif_soap_t *s, const okam_config_t *cfg, const char *device_uuid,
                      cam_source_t *src);
int onvif_soap_run(onvif_soap_t *s);   /* blocking accept loop */
void onvif_soap_stop(onvif_soap_t *s);

#endif /* OKAM_ONVIF_SOAP_H */
