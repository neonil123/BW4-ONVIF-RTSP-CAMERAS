/* Configuration: /system/etc/cam_onvifd.conf (key=value, '#' comments),
 * overridable by environment variables CAM_DEVPW / CAM_VUID, overridable
 * again by CLI flags. No credential lookup logic lives here on purpose --
 * the operator populates devpw/vuid at install time. */
#ifndef CAM_CONFIG_H
#define CAM_CONFIG_H

typedef struct {
    char cam_host[64];      /* local H.264 source host, default 127.0.0.1 */
    int  cam_port;          /* default 81 */
    char devpw[64];         /* livestream.cgi loginpas/pwd */
    char vuid[64];          /* livestream.cgi vuid */
    int  streamid;          /* default 10 (main stream) */
    int  substream;         /* default 2 */
    double fps;             /* synthesized RTP clock fallback fps, default 15.0 */

    int  rtsp_port;         /* default 554 */
    char rtsp_name[32];     /* default "live" */

    int  onvif_port;        /* default 80 */
    char device_ip[64];     /* optional override; empty = auto-detect per client */
    char onvif_user[64];    /* ONVIF WS-Security username, default "admin" --
                             * this is what the operator types into the NVR;
                             * unrelated to devpw (the camera's internal :81
                             * credential) */
    char onvif_pass[64];    /* ONVIF WS-Security password, default "admin" */

    char manufacturer[32];
    char model[32];
    char serial[32];
    char hardware_id[32];
    int  video_width;       /* fallback only, default 1920 -- ONVIF advertises
                             * the real SPS-derived resolution (h264_sps.c)
                             * once the first SPS is observed; this is only
                             * used before that */
    int  video_height;      /* fallback only, default 1080 (see video_width) */

    int  log_level;
} cam_config_t;

/* Fills `cfg` with defaults, then overlays the conf file (if it exists) and
 * CAM_DEVPW/CAM_VUID env vars. `conf_path` may be NULL to use the default
 * /system/etc/cam_onvifd.conf. */
void config_load(cam_config_t *cfg, const char *conf_path);

#endif /* CAM_CONFIG_H */
