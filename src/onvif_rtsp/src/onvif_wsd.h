/* onvif_wsd — WS-Discovery responder (UDP multicast 239.255.255.250:3702).
 * Answers a Probe with a ProbeMatch pointing at our ONVIF device_service, so
 * Profile-S clients (Frigate, Blue Iris, Synology, Hikvision, Dahua NVRs...)
 * find the camera automatically. Response shape mirrors the working
 * onvif_srvd/prudynt-t template bundled in builds/prudynt_bundle/onvif
 * (wsd_files/ProbeMatches.xml) for maximum client compatibility.
 */
#ifndef OKAM_ONVIF_WSD_H
#define OKAM_ONVIF_WSD_H

typedef struct {
    int port;              /* device_service port, e.g. 80 */
    char uuid[40];         /* stable device UUID (urn:uuid:<uuid>) */
    char manufacturer[32];
    char model[32];
    char iface_name[16];   /* interface used for the multicast join, e.g. "vnet0" */
    char iface_ip[16];     /* that interface's IPv4, used to build XAddrs (no
                             * client socket to getsockname() on for UDP) */
    volatile int stop;
    int sock;
} onvif_wsd_t;

/* device_uuid: pass the same UUID given to onvif_soap_init so the
 * WS-Discovery EndpointReference matches GetDeviceInformation's identity;
 * pass NULL to have this module generate its own. */
void onvif_wsd_init(onvif_wsd_t *w, int onvif_port, const char *manufacturer,
                     const char *model, const char *device_uuid);
/* Blocks; run on its own thread. */
int onvif_wsd_run(onvif_wsd_t *w);
void onvif_wsd_stop(onvif_wsd_t *w);

#endif /* OKAM_ONVIF_WSD_H */
