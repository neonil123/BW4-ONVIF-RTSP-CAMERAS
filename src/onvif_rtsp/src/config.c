#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_CONF_PATH "/system/etc/cam_onvifd.conf"

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void set_field(cam_config_t *cfg, const char *key, const char *val) {
    if (!strcmp(key, "cam_host")) snprintf(cfg->cam_host, sizeof cfg->cam_host, "%s", val);
    else if (!strcmp(key, "cam_port")) cfg->cam_port = atoi(val);
    else if (!strcmp(key, "devpw")) snprintf(cfg->devpw, sizeof cfg->devpw, "%s", val);
    else if (!strcmp(key, "vuid")) snprintf(cfg->vuid, sizeof cfg->vuid, "%s", val);
    else if (!strcmp(key, "streamid")) cfg->streamid = atoi(val);
    else if (!strcmp(key, "substream")) cfg->substream = atoi(val);
    else if (!strcmp(key, "fps")) cfg->fps = atof(val);
    else if (!strcmp(key, "rtsp_port")) cfg->rtsp_port = atoi(val);
    else if (!strcmp(key, "rtsp_name")) snprintf(cfg->rtsp_name, sizeof cfg->rtsp_name, "%s", val);
    else if (!strcmp(key, "onvif_port")) cfg->onvif_port = atoi(val);
    else if (!strcmp(key, "device_ip")) snprintf(cfg->device_ip, sizeof cfg->device_ip, "%s", val);
    else if (!strcmp(key, "onvif_user")) snprintf(cfg->onvif_user, sizeof cfg->onvif_user, "%s", val);
    else if (!strcmp(key, "onvif_pass")) snprintf(cfg->onvif_pass, sizeof cfg->onvif_pass, "%s", val);
    else if (!strcmp(key, "manufacturer")) snprintf(cfg->manufacturer, sizeof cfg->manufacturer, "%s", val);
    else if (!strcmp(key, "model")) snprintf(cfg->model, sizeof cfg->model, "%s", val);
    else if (!strcmp(key, "serial")) snprintf(cfg->serial, sizeof cfg->serial, "%s", val);
    else if (!strcmp(key, "hardware_id")) snprintf(cfg->hardware_id, sizeof cfg->hardware_id, "%s", val);
    else if (!strcmp(key, "video_width")) cfg->video_width = atoi(val);
    else if (!strcmp(key, "video_height")) cfg->video_height = atoi(val);
    else if (!strcmp(key, "log_level")) cfg->log_level = atoi(val);
    /* unknown keys are ignored, forward-compat with a fuller conf file */
}

static void parse_file(cam_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        char *hash = strchr(s, '#');
        if (hash) *hash = '\0';
        trim(s);
        if (!*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s, *val = eq + 1;
        trim(key);
        trim(val);
        if (*key) set_field(cfg, key, val);
    }
    fclose(f);
}

void config_load(cam_config_t *cfg, const char *conf_path) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->cam_host, sizeof cfg->cam_host, "%s", "127.0.0.1");
    cfg->cam_port = 81;
    cfg->streamid = 10;
    cfg->substream = 2;
    cfg->fps = 15.0;
    cfg->rtsp_port = 554;
    snprintf(cfg->rtsp_name, sizeof cfg->rtsp_name, "%s", "live");
    cfg->onvif_port = 80; /* standard ONVIF port; NVRs/discovery libs default
                           * to :80 when they can't parse a port out of
                           * discovery. Confirmed free on this device
                           * (camweb/livestream.cgi uses :81). Override via
                           * conf onvif_port= or --onvif-port. */
    snprintf(cfg->onvif_user, sizeof cfg->onvif_user, "%s", "admin");
    snprintf(cfg->onvif_pass, sizeof cfg->onvif_pass, "%s", "admin");
    snprintf(cfg->manufacturer, sizeof cfg->manufacturer, "%s", "CAM");
    snprintf(cfg->model, sizeof cfg->model, "%s", "QC3-T23N");
    snprintf(cfg->serial, sizeof cfg->serial, "%s", "cam-onvifd");
    snprintf(cfg->hardware_id, sizeof cfg->hardware_id, "%s", "T23N");
    cfg->video_width = 1920;
    cfg->video_height = 1080;
    cfg->log_level = 2; /* LOG_INFO */

    parse_file(cfg, conf_path ? conf_path : DEFAULT_CONF_PATH);

    const char *e;
    if ((e = getenv("CAM_DEVPW")) && *e) snprintf(cfg->devpw, sizeof cfg->devpw, "%s", e);
    if ((e = getenv("CAM_VUID")) && *e) snprintf(cfg->vuid, sizeof cfg->vuid, "%s", e);
}
