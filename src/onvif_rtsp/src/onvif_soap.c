#include "onvif_soap.h"
#include "util.h"
#include "wsse.h"
#include "httpauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define SOAP_RECV_CAP (32 * 1024)
#define SOAP_RESP_CAP (8 * 1024)

typedef struct {
    onvif_soap_t *s;
    int fd;
} conn_arg_t;

static const char *memmem_like(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

static void send_http(int fd, int code, const char *status, const char *body, size_t blen) {
    char head[256];
    int n = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/soap+xml; charset=utf-8\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      code, status, blen);
    if (write(fd, head, (size_t)n) < 0) return;
    if (blen) { ssize_t r = write(fd, body, blen); (void)r; }
}

/* HTTP 401 + WWW-Authenticate (Digest and Basic) challenge, carrying the
 * SOAP NotAuthorized fault as the body too (harmless for HTTP-Digest-only
 * clients, and still recognizable to SOAP/WS-Security-only ones). */
static void send_401(const httpauth_ctx_t *actx, int fd, const char *body, size_t blen) {
    char challenge[400];
    httpauth_challenge_headers(actx, challenge, sizeof challenge);
    char head[700];
    int n = snprintf(head, sizeof head,
                      "HTTP/1.1 401 Unauthorized\r\n"
                      "%s"
                      "Content-Type: application/soap+xml; charset=utf-8\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      challenge, blen);
    if (write(fd, head, (size_t)n) < 0) return;
    if (blen) { ssize_t r = write(fd, body, blen); (void)r; }
}

/* `out` must be at least sizeof(okam_config_t.device_ip) bytes (see call
 * sites: they size their local buffer to match, not the 16 bytes a bare
 * dotted-quad would need, since device_ip may hold a hostname override). */
static void device_ip(onvif_soap_t *s, int fd, char *out, size_t out_cap) {
    if (s->cfg.device_ip[0]) { snprintf(out, out_cap, "%s", s->cfg.device_ip); return; }
    local_ip_of_fd(fd, out);
}

/* ------------------------------------------------------------ SOAP templates */
static const char DEV_INFO_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tds:GetDeviceInformationResponse>\r\n"
    "    <tds:Manufacturer>%s</tds:Manufacturer>\r\n"
    "    <tds:Model>%s</tds:Model>\r\n"
    "    <tds:FirmwareVersion>1.0</tds:FirmwareVersion>\r\n"
    "    <tds:SerialNumber>%s</tds:SerialNumber>\r\n"
    "    <tds:HardwareId>%s</tds:HardwareId>\r\n"
    "  </tds:GetDeviceInformationResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char SYSTIME_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>\r\n"
    "    <tt:DateTimeType>NTP</tt:DateTimeType>\r\n"
    "    <tt:DaylightSavings>false</tt:DaylightSavings>\r\n"
    "    <tt:TimeZone><tt:TZ>GMT0</tt:TZ></tt:TimeZone>\r\n"
    "    <tt:UTCDateTime>\r\n"
    "      <tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>\r\n"
    "      <tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>\r\n"
    "    </tt:UTCDateTime>\r\n"
    "  </tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char CAPS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetCapabilitiesResponse><tds:Capabilities>\r\n"
    "    <tt:Device><tt:XAddr>%s</tt:XAddr>\r\n"
    "      <tt:Network><tt:IPFilter>false</tt:IPFilter></tt:Network>\r\n"
    "      <tt:System><tt:DiscoveryResolve>true</tt:DiscoveryResolve><tt:DiscoveryBye>true</tt:DiscoveryBye>\r\n"
    "        <tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tt:SupportedVersions>\r\n"
    "      </tt:System>\r\n"
    "    </tt:Device>\r\n"
    "    <tt:Events><tt:XAddr>%s</tt:XAddr>\r\n"
    "      <tt:WSSubscriptionPolicySupport>true</tt:WSSubscriptionPolicySupport>\r\n"
    "      <tt:WSPullPointSupport>true</tt:WSPullPointSupport>\r\n"
    "      <tt:WSPausableSubscriptionManagerInterfaceSupport>false</tt:WSPausableSubscriptionManagerInterfaceSupport>\r\n"
    "    </tt:Events>\r\n"
    "    <tt:Media><tt:XAddr>%s</tt:XAddr>\r\n"
    "      <tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP>\r\n"
    "        <tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities>\r\n"
    "    </tt:Media>\r\n"
    "  </tds:Capabilities></tds:GetCapabilitiesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char SERVICES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetServicesResponse>\r\n"
    "    <tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>\r\n"
    "      <tds:XAddr>%s</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tds:Version></tds:Service>\r\n"
    "    <tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>\r\n"
    "      <tds:XAddr>%s</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tds:Version></tds:Service>\r\n"
    "    <tds:Service><tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace>\r\n"
    "      <tds:XAddr>%s</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tds:Version></tds:Service>\r\n"
    "  </tds:GetServicesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char SCOPES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetScopesResponse>\r\n"
    "    <tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes>\r\n"
    "    <tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/%s</tt:ScopeItem></tds:Scopes>\r\n"
    "    <tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/%s</tt:ScopeItem></tds:Scopes>\r\n"
    "  </tds:GetScopesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char PROFILES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetProfilesResponse>\r\n"
    "    <trt:Profiles token=\"profile0\" fixed=\"true\">\r\n"
    "      <tt:Name>okam-main</tt:Name>\r\n"
    "      <tt:VideoSourceConfiguration token=\"vsconf0\">\r\n"
    "        <tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken>\r\n"
    "        <tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\r\n"
    "      </tt:VideoSourceConfiguration>\r\n"
    "      <tt:VideoEncoderConfiguration token=\"veconf0\">\r\n"
    "        <tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>\r\n"
    "        <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "        <tt:Quality>100</tt:Quality>\r\n"
    "        <tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>\r\n"
    "          <tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>\r\n"
    "        <tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>\r\n"
    "        <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "      </tt:VideoEncoderConfiguration>\r\n"
    "      <tt:AudioSourceConfiguration token=\"asconf0\">\r\n"
    "        <tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as0</tt:SourceToken>\r\n"
    "      </tt:AudioSourceConfiguration>\r\n"
    "      <tt:AudioEncoderConfiguration token=\"aeconf0\">\r\n"
    "        <tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding>\r\n"
    "        <tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>\r\n"
    "        <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "      </tt:AudioEncoderConfiguration>\r\n"
    "    </trt:Profiles>\r\n"
    "  </trt:GetProfilesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* Singular GetProfile -- same content as GetProfiles, just wrapped as one
 * trt:Profile instead of a list. Checked AFTER "GetProfiles" in dispatch()
 * so the plural (which "GetProfile" is a substring/prefix of) always wins
 * first, exactly like the GetVideo*Configuration(s) ordering below. */
static const char PROFILE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetProfileResponse>\r\n"
    "    <trt:Profile token=\"profile0\" fixed=\"true\">\r\n"
    "      <tt:Name>okam-main</tt:Name>\r\n"
    "      <tt:VideoSourceConfiguration token=\"vsconf0\">\r\n"
    "        <tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken>\r\n"
    "        <tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\r\n"
    "      </tt:VideoSourceConfiguration>\r\n"
    "      <tt:VideoEncoderConfiguration token=\"veconf0\">\r\n"
    "        <tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>\r\n"
    "        <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "        <tt:Quality>100</tt:Quality>\r\n"
    "        <tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>\r\n"
    "          <tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>\r\n"
    "        <tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>\r\n"
    "        <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "      </tt:VideoEncoderConfiguration>\r\n"
    "      <tt:AudioSourceConfiguration token=\"asconf0\">\r\n"
    "        <tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as0</tt:SourceToken>\r\n"
    "      </tt:AudioSourceConfiguration>\r\n"
    "      <tt:AudioEncoderConfiguration token=\"aeconf0\">\r\n"
    "        <tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding>\r\n"
    "        <tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>\r\n"
    "        <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "      </tt:AudioEncoderConfiguration>\r\n"
    "    </trt:Profile>\r\n"
    "  </trt:GetProfileResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* CreateProfile: this daemon relays ONE fixed vp_project stream, it can't
 * actually create a second independently-configurable profile -- but
 * Synology's activation flow calls this and expects success, so we "create"
 * it by echoing the requested token/Name back wrapped around the SAME real
 * VideoSource/VideoEncoder configuration GetProfiles already reports. Any
 * later GetStreamUri against this (or any) token still returns the one real
 * rtsp:// URL (see dispatch()'s GetStreamUri branch, which is token-agnostic
 * by construction -- it never reads ProfileToken at all). */
static const char CREATE_PROFILE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:CreateProfileResponse>\r\n"
    "    <trt:Profile token=\"%s\" fixed=\"false\">\r\n"
    "      <tt:Name>%s</tt:Name>\r\n"
    "      <tt:VideoSourceConfiguration token=\"vsconf0\">\r\n"
    "        <tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken>\r\n"
    "        <tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\r\n"
    "      </tt:VideoSourceConfiguration>\r\n"
    "      <tt:VideoEncoderConfiguration token=\"veconf0\">\r\n"
    "        <tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>\r\n"
    "        <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "        <tt:Quality>100</tt:Quality>\r\n"
    "        <tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>\r\n"
    "          <tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>\r\n"
    "        <tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>\r\n"
    "        <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "      </tt:VideoEncoderConfiguration>\r\n"
    "    </trt:Profile>\r\n"
    "  </trt:CreateProfileResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char GUARANTEED_INSTANCES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">\r\n"
    "  <SOAP-ENV:Body><trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>\r\n"
    "    <trt:TotalNumber>1</trt:TotalNumber>\r\n"
    "    <trt:JPEG>1</trt:JPEG>\r\n"
    "    <trt:H264>1</trt:H264>\r\n"
    "  </trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* GetNetworkInterfaces: one real NetworkInterface entry describing the
 * actual WLAN interface (name/HwAddress/IPv4 queried live, not guessed). */
static const char NETIFS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetNetworkInterfacesResponse>\r\n"
    "    <tds:NetworkInterfaces token=\"eth0\">\r\n"
    "      <tt:Enabled>true</tt:Enabled>\r\n"
    "      <tt:Info><tt:Name>%s</tt:Name><tt:HwAddress>%s</tt:HwAddress><tt:MTU>%d</tt:MTU></tt:Info>\r\n"
    "      <tt:IPv4>\r\n"
    "        <tt:Enabled>true</tt:Enabled>\r\n"
    "        <tt:Config>\r\n"
    "          <tt:Manual><tt:Address>%s</tt:Address><tt:PrefixLength>24</tt:PrefixLength></tt:Manual>\r\n"
    "          <tt:DHCP>true</tt:DHCP>\r\n"
    "        </tt:Config>\r\n"
    "      </tt:IPv4>\r\n"
    "    </tds:NetworkInterfaces>\r\n"
    "  </tds:GetNetworkInterfacesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char STREAM_URI_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetStreamUriResponse><trt:MediaUri>\r\n"
    "    <tt:Uri>%s</tt:Uri>\r\n"
    "    <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>\r\n"
    "    <tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>\r\n"
    "    <tt:Timeout>PT0S</tt:Timeout>\r\n"
    "  </trt:MediaUri></trt:GetStreamUriResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* GetSnapshotUri: same token-agnostic design as GetStreamUri above -- one
 * real snapshot source (the /onvif/snapshot proxy handler, see
 * fetch_snapshot()/handle_snapshot() below), served regardless of which
 * profile token was asked about. */
static const char SNAPSHOT_URI_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetSnapshotUriResponse><trt:MediaUri>\r\n"
    "    <tt:Uri>%s</tt:Uri>\r\n"
    "    <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>\r\n"
    "    <tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>\r\n"
    "    <tt:Timeout>PT0S</tt:Timeout>\r\n"
    "  </trt:MediaUri></trt:GetSnapshotUriResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char NTP_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetNTPResponse>\r\n"
    "    <tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP></tds:NTPInformation>\r\n"
    "  </tds:GetNTPResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* No relay outputs on this device -- an empty list is a normal, valid
 * answer (matches the GetAudio* empty-response style above). */
static const char RELAY_OUTPUTS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tds:GetRelayOutputsResponse/></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_SOURCES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoSourcesResponse>\r\n"
    "    <trt:VideoSources token=\"vs0\">\r\n"
    "      <tt:Framerate>%d</tt:Framerate>\r\n"
    "      <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "      <tt:Imaging/>\r\n"
    "    </trt:VideoSources>\r\n"
    "  </trt:GetVideoSourcesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_SRC_CONF_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoSourceConfigurationsResponse>\r\n"
    "    <trt:Configurations token=\"vsconf0\">\r\n"
    "      <tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken>\r\n"
    "      <tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\r\n"
    "    </trt:Configurations>\r\n"
    "  </trt:GetVideoSourceConfigurationsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_SRC_CONF_SINGLE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoSourceConfigurationResponse>\r\n"
    "    <trt:Configuration token=\"vsconf0\">\r\n"
    "      <tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken>\r\n"
    "      <tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\r\n"
    "    </trt:Configuration>\r\n"
    "  </trt:GetVideoSourceConfigurationResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_ENC_CONF_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoEncoderConfigurationsResponse>\r\n"
    "    <trt:Configurations token=\"veconf0\">\r\n"
    "      <tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>\r\n"
    "      <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "      <tt:Quality>100</tt:Quality>\r\n"
    "      <tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>\r\n"
    "        <tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>\r\n"
    "      <tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>\r\n"
    "      <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "    </trt:Configurations>\r\n"
    "  </trt:GetVideoEncoderConfigurationsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_ENC_CONF_SINGLE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoEncoderConfigurationResponse>\r\n"
    "    <trt:Configuration token=\"veconf0\">\r\n"
    "      <tt:Name>VideoEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>\r\n"
    "      <tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\r\n"
    "      <tt:Quality>100</tt:Quality>\r\n"
    "      <tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>\r\n"
    "        <tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>\r\n"
    "      <tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>\r\n"
    "      <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "    </trt:Configuration>\r\n"
    "  </trt:GetVideoEncoderConfigurationResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char VIDEO_ENC_OPTS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetVideoEncoderConfigurationOptionsResponse>\r\n"
    "    <trt:Options>\r\n"
    "      <tt:QualityRange><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:QualityRange>\r\n"
    "      <tt:H264>\r\n"
    "        <tt:ResolutionsAvailable><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:ResolutionsAvailable>\r\n"
    "        <tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>60</tt:Max></tt:GovLengthRange>\r\n"
    "        <tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>30</tt:Max></tt:FrameRateRange>\r\n"
    "        <tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>\r\n"
    "        <tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported>\r\n"
    "      </tt:H264>\r\n"
    "    </trt:Options>\r\n"
    "  </trt:GetVideoEncoderConfigurationOptionsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* -------------------------------------------------------------- audio track
 * G.711 mu-law (PCMU), 8 kHz mono -- the audio side of the RTSP SDP's second
 * media (m=audio ... PCMU/8000). ONVIF AudioEncoderConfiguration SampleRate
 * is expressed in kHz (so 8 == 8000 Hz) and Bitrate in kbps (64 == G.711's
 * 64 kbit/s). The single AudioSource (as0) is the camera's built-in mic,
 * relayed by the mic_capture.so shim -> audio_source.c. */
static const char AUDIO_SOURCES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioSourcesResponse>\r\n"
    "    <trt:AudioSources token=\"as0\"><tt:Channels>1</tt:Channels></trt:AudioSources>\r\n"
    "  </trt:GetAudioSourcesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char AUDIO_SRC_CONF_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioSourceConfigurationsResponse>\r\n"
    "    <trt:Configurations token=\"asconf0\">\r\n"
    "      <tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as0</tt:SourceToken>\r\n"
    "    </trt:Configurations>\r\n"
    "  </trt:GetAudioSourceConfigurationsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char AUDIO_SRC_CONF_SINGLE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioSourceConfigurationResponse>\r\n"
    "    <trt:Configuration token=\"asconf0\">\r\n"
    "      <tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as0</tt:SourceToken>\r\n"
    "    </trt:Configuration>\r\n"
    "  </trt:GetAudioSourceConfigurationResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char AUDIO_ENC_CONF_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioEncoderConfigurationsResponse>\r\n"
    "    <trt:Configurations token=\"aeconf0\">\r\n"
    "      <tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding>\r\n"
    "      <tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>\r\n"
    "      <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "    </trt:Configurations>\r\n"
    "  </trt:GetAudioEncoderConfigurationsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char AUDIO_ENC_CONF_SINGLE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioEncoderConfigurationResponse>\r\n"
    "    <trt:Configuration token=\"aeconf0\">\r\n"
    "      <tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding>\r\n"
    "      <tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>\r\n"
    "      <tt:SessionTimeout>PT10S</tt:SessionTimeout>\r\n"
    "    </trt:Configuration>\r\n"
    "  </trt:GetAudioEncoderConfigurationResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char AUDIO_ENC_OPTS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><trt:GetAudioEncoderConfigurationOptionsResponse>\r\n"
    "    <trt:Options>\r\n"
    "      <tt:Options>\r\n"
    "        <tt:Encoding>G711</tt:Encoding>\r\n"
    "        <tt:BitrateList><tt:Items>64</tt:Items></tt:BitrateList>\r\n"
    "        <tt:SampleRateList><tt:Items>8</tt:Items></tt:SampleRateList>\r\n"
    "      </tt:Options>\r\n"
    "    </trt:Options>\r\n"
    "  </trt:GetAudioEncoderConfigurationOptionsResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* Generic empty-list response for the remaining ops this camera has nothing
 * to report for -- an empty/absent list is a normal, valid ONVIF answer, not
 * an error. `%s` is the action name, giving a correctly-named but empty
 * *Response element without a bespoke template per action. */
static const char EMPTY_RESP_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">\r\n"
    "  <SOAP-ENV:Body><trt:%sResponse/></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char SVC_CAPS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">\r\n"
    "  <SOAP-ENV:Body><trt:GetServiceCapabilitiesResponse>\r\n"
    "    <trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">\r\n"
    "      <trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>\r\n"
    "      <trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/>\r\n"
    "    </trt:Capabilities>\r\n"
    "  </trt:GetServiceCapabilitiesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* Sent (HTTP 400) for any recognized-but-truly-unimplemented SOAP action,
 * instead of a bare 500 -- env:Receiver/ter:ActionNotSupported is the
 * standard ONVIF way to say "not this one", which well-behaved clients
 * tolerate for non-critical operations instead of aborting. */
static const char ACTION_NOT_SUPPORTED_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:ter=\"http://www.onvif.org/ver10/error\">\r\n"
    "  <SOAP-ENV:Body><SOAP-ENV:Fault>\r\n"
    "    <SOAP-ENV:Code>\r\n"
    "      <SOAP-ENV:Value>SOAP-ENV:Receiver</SOAP-ENV:Value>\r\n"
    "      <SOAP-ENV:Subcode><SOAP-ENV:Value>ter:ActionNotSupported</SOAP-ENV:Value></SOAP-ENV:Subcode>\r\n"
    "    </SOAP-ENV:Code>\r\n"
    "    <SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">Action Not Supported</SOAP-ENV:Text></SOAP-ENV:Reason>\r\n"
    "  </SOAP-ENV:Fault></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* ---------------------------------------------------------- Event service
 * Minimal ONVIF Event service: enough for a Profile-S NVR (confirmed:
 * Synology Surveillance Station) to finish its add-camera "activating"
 * step, which sets up a pull-point event subscription even though this
 * daemon never actually fires any events. No real motion detection is
 * implemented -- PullMessages always returns empty, which is a normal,
 * valid "nothing happened" answer, not an error. Namespaces match the real
 * ONVIF Event WSDL (verified against the working reference bundled at
 * builds/prudynt_bundle/onvif/www-onvif/events_service_files/). */
static const char EVENT_PROPS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\"\r\n"
    "                   xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\"\r\n"
    "                   xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"\r\n"
    "                   xmlns:tns1=\"http://www.onvif.org/ver10/topics\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tev:GetEventPropertiesResponse>\r\n"
    "    <tev:TopicNamespaceLocation>http://www.onvif.org/onvif/ver10/topics/topicns.xml</tev:TopicNamespaceLocation>\r\n"
    "    <wsnt:FixedTopicSet>false</wsnt:FixedTopicSet>\r\n"
    "    <wstop:TopicSet>\r\n"
    "      <tns1:RuleEngine>\r\n"
    "        <tns1:CellMotionDetector>\r\n"
    "          <tns1:Motion wstop:topic=\"true\">\r\n"
    "            <tt:MessageDescription IsProperty=\"true\">\r\n"
    "              <tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>\r\n"
    "              <tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xs:boolean\"/></tt:Data>\r\n"
    "            </tt:MessageDescription>\r\n"
    "          </tns1:Motion>\r\n"
    "        </tns1:CellMotionDetector>\r\n"
    "      </tns1:RuleEngine>\r\n"
    "      <tns1:VideoSource>\r\n"
    "        <tns1:MotionAlarm wstop:topic=\"true\">\r\n"
    "          <tt:MessageDescription IsProperty=\"true\">\r\n"
    "            <tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>\r\n"
    "            <tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xs:boolean\"/></tt:Data>\r\n"
    "          </tt:MessageDescription>\r\n"
    "        </tns1:MotionAlarm>\r\n"
    "      </tns1:VideoSource>\r\n"
    "    </wstop:TopicSet>\r\n"
    "    <wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet</wsnt:TopicExpressionDialect>\r\n"
    "    <tev:MessageContentFilterDialect>http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter</tev:MessageContentFilterDialect>\r\n"
    "  </tev:GetEventPropertiesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char EVENT_SVC_CAPS_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tev:GetServiceCapabilitiesResponse>\r\n"
    "    <tev:Capabilities WSSubscriptionPolicySupport=\"true\" WSPullPointSupport=\"true\"\r\n"
    "                      WSPausableSubscriptionManagerInterfaceSupport=\"false\"\r\n"
    "                      MaxNotificationProducers=\"1\" MaxPullPoints=\"1\"\r\n"
    "                      PersistentNotificationStorage=\"false\"/>\r\n"
    "  </tev:GetServiceCapabilitiesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char PULLPOINT_SUB_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:wsa=\"http://www.w3.org/2005/08/addressing\"\r\n"
    "                   xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\"\r\n"
    "                   xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tev:CreatePullPointSubscriptionResponse>\r\n"
    "    <tev:SubscriptionReference><wsa:Address>%s</wsa:Address></tev:SubscriptionReference>\r\n"
    "    <wsnt:CurrentTime>%s</wsnt:CurrentTime>\r\n"
    "    <wsnt:TerminationTime>%s</wsnt:TerminationTime>\r\n"
    "  </tev:CreatePullPointSubscriptionResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char PULL_MESSAGES_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tev:PullMessagesResponse>\r\n"
    "    <tev:CurrentTime>%s</tev:CurrentTime>\r\n"
    "    <tev:TerminationTime>%s</tev:TerminationTime>\r\n"
    "  </tev:PullMessagesResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char RENEW_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">\r\n"
    "  <SOAP-ENV:Body><wsnt:RenewResponse>\r\n"
    "    <wsnt:TerminationTime>%s</wsnt:TerminationTime>\r\n"
    "    <wsnt:CurrentTime>%s</wsnt:CurrentTime>\r\n"
    "  </wsnt:RenewResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char UNSUBSCRIBE_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">\r\n"
    "  <SOAP-ENV:Body><wsnt:UnsubscribeResponse/></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char SYNC_POINT_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tev:SetSynchronizationPointResponse/></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* "YYYY-MM-DDTHH:MM:SSZ" -- used for wsnt:CurrentTime/TerminationTime etc. */
static void format_iso8601(time_t t, char *out, size_t out_cap) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, out_cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* Standard ONVIF auth-failure fault: env:Sender / ter:NotAuthorized. Sent as
 * the body of an HTTP 401 (see send_401()) alongside WWW-Authenticate
 * challenges -- most NVRs (incl. Synology Surveillance Station) authenticate
 * ONVIF via HTTP Digest/Basic and need the 401 + challenge to engage their
 * retry-with-credentials flow; WS-Security-only clients still get a
 * recognizable SOAP fault in the body. */
static const char NOTAUTH_FAULT_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:ter=\"http://www.onvif.org/ver10/error\">\r\n"
    "  <SOAP-ENV:Body><SOAP-ENV:Fault>\r\n"
    "    <SOAP-ENV:Code>\r\n"
    "      <SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value>\r\n"
    "      <SOAP-ENV:Subcode><SOAP-ENV:Value>ter:NotAuthorized</SOAP-ENV:Value></SOAP-ENV:Subcode>\r\n"
    "    </SOAP-ENV:Code>\r\n"
    "    <SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">Sender not authorized</SOAP-ENV:Text></SOAP-ENV:Reason>\r\n"
    "  </SOAP-ENV:Fault></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* GetSystemDateAndTime, GetHostname, GetWsdlUrl are the ONVIF spec's carve-outs
 * that MUST stay reachable pre-auth (so a client can sync its clock / read
 * the WSDL / discover before it has credentials). Everything else on this
 * endpoint requires a valid wsse:UsernameToken. */
static const char HOSTNAME_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\r\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
    "  <SOAP-ENV:Body><tds:GetHostnameResponse><tds:HostnameInformation>\r\n"
    "    <tt:FromDHCP>false</tt:FromDHCP>\r\n"
    "    <tt:Name>%s</tt:Name>\r\n"
    "  </tds:HostnameInformation></tds:GetHostnameResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

static const char WSDLURL_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "                   xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\r\n"
    "  <SOAP-ENV:Body><tds:GetWsdlUrlResponse>\r\n"
    "    <tds:WsdlUrl>http://www.onvif.org/onvif/ver10/device/wsdl/devicemgmt.wsdl</tds:WsdlUrl>\r\n"
    "  </tds:GetWsdlUrlResponse></SOAP-ENV:Body>\r\n"
    "</SOAP-ENV:Envelope>\r\n";

/* Only the actual media-access operation requires authentication -- see the
 * access-policy note at the top of onvif_soap.h. Every other action on this
 * endpoint (capability discovery: GetDeviceInformation, GetCapabilities,
 * GetServices, GetServiceCapabilities, GetScopes, GetProfiles/GetProfile,
 * GetVideoSources, GetVideo{Source,Encoder}Configuration(s)[Options],
 * GetAudio*, GetSystemDateAndTime, GetHostname, GetWsdlUrl) succeeds
 * unauthenticated, matching real Profile-S cameras and letting an NVR's
 * add-camera wizard (confirmed: Synology Surveillance Station) enumerate
 * the camera before it has credentials. */
static int action_requires_auth(const char *body, size_t blen) {
    return memmem_like(body, blen, "GetStreamUri") != NULL;
}

/* Best-effort extraction of the actual SOAP action's element name (e.g.
 * "tev:SomeUnrecognizedOp") for logging when nothing in dispatch()'s
 * if/else chain matched -- finds "...:Body", skips to the end of that
 * start tag, then reads the following element's tag name verbatim
 * (namespace prefix included, since that's often useful context too). Not
 * a real XML parser, same minimal-scan style as the rest of this file. */
static void extract_unknown_op(const char *body, size_t blen, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s", "?");
    const char *b = memmem_like(body, blen, ":Body");
    if (!b) return;
    const char *gt = (const char *)memchr(b, '>', (size_t)((body + blen) - b));
    if (!gt) return;
    const char *p = gt + 1;
    const char *endbuf = body + blen;
    while (p < endbuf && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) p++;
    if (p >= endbuf || *p != '<') return;
    p++;
    const char *end = p;
    while (end < endbuf && *end != ' ' && *end != '>' && *end != '/' && *end != '\r' && *end != '\n')
        end++;
    size_t n = (size_t)(end - p);
    if (n == 0) return;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Live SPS-derived resolution if known yet, else the configured default
 * (see h264_sps.c / cam_source_get_resolution()). Substreams and sensors
 * differ, so the config value is only ever a fallback for before the first
 * SPS arrives. */
static void resolution(onvif_soap_t *s, int *width, int *height) {
    if (!s->src || !cam_source_get_resolution(s->src, width, height)) {
        *width = s->cfg.video_width;
        *height = s->cfg.video_height;
    }
}

/* ----------------------------------------------------- GetNetworkInterfaces
 * Deliberately self-contained here (its own getifaddrs() scan, not shared
 * with onvif_wsd.c's near-identical interface-finding code) so this feature
 * carries zero risk of changing WS-Discovery's behavior. Prefers "vnet0"
 * (this device's WLAN interface), falling back to the first UP,
 * non-loopback IPv4 interface. */
#define NETIF_PREFERRED "vnet0"

static int find_network_iface(char *ifname_out, size_t ifname_cap, char *ip_out, size_t ip_cap) {
    struct ifaddrs *ifas, *p;
    if (getifaddrs(&ifas) != 0)
        return -1;
    int found = 0;
    for (p = ifas; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, NETIF_PREFERRED) != 0) continue;
        snprintf(ifname_out, ifname_cap, "%s", p->ifa_name);
        inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr, ip_out, (socklen_t)ip_cap);
        found = 1;
        break;
    }
    if (!found) {
        for (p = ifas; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (p->ifa_flags & IFF_LOOPBACK) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;
            snprintf(ifname_out, ifname_cap, "%s", p->ifa_name);
            inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr, ip_out, (socklen_t)ip_cap);
            found = 1;
            break;
        }
    }
    freeifaddrs(ifas);
    return found ? 0 : -1;
}

static void get_iface_mac(const char *ifname, char *out_hex, size_t out_cap) {
    snprintf(out_hex, out_cap, "%s", "00:00:00:00:00:00");
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        const unsigned char *m = (const unsigned char *)ifr.ifr_hwaddr.sa_data;
        snprintf(out_hex, out_cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    close(sock);
}

/* Best-effort "<Tag>value</Tag>" extraction (any namespace prefix), used to
 * echo CreateProfile's requested Name/Token back in the response -- same
 * minimal-scan style as extract_unknown_op()/wsse.c, not a real XML parser.
 * Falls back to `fallback` if the tag isn't found or is empty. */
static void extract_simple_value(const char *body, size_t blen, const char *tag,
                                  char *out, size_t out_cap, const char *fallback) {
    snprintf(out, out_cap, "%s", fallback);
    size_t tlen = strlen(tag);
    const char *p = body;
    size_t remaining = blen;
    while (remaining >= tlen) {
        const char *hit = memmem_like(p, remaining, tag);
        if (!hit) return;
        int boundary_ok = (hit > body) && (hit[-1] == ':' || hit[-1] == '<');
        const char *after = hit + tlen;
        if (boundary_ok && after < body + blen && *after == '>') {
            const char *content = after + 1;
            const char *end = (const char *)memchr(content, '<', (size_t)((body + blen) - content));
            if (end && end > content) {
                size_t n = (size_t)(end - content);
                if (n >= out_cap) n = out_cap - 1;
                memcpy(out, content, n);
                out[n] = '\0';
                return;
            }
        }
        size_t skip = (size_t)(hit - p) + 1;
        p += skip;
        remaining -= skip;
    }
}

/* ------------------------------------------------------------ GET /onvif/snapshot
 * Proxies a real JPEG snapshot from vp_project's own local web server (the
 * same host:port and devpw/vuid credentials the video handshake already
 * uses -- see cam_source.c's handshake(), duplicated here rather than
 * shared, so this carries zero risk to the RTSP/video path). The camera
 * mislabels the response Content-Type (text/html) even though the body is
 * a real JPEG, so the only reliable signal is the JPEG magic (FF D8 FF),
 * not the upstream header. Tries /snapshot.cgi first, falls back to
 * /onvifsnapshot.cgi (both confirmed live on cam #1). */
static uint8_t *fetch_snapshot_path(const okam_config_t *cfg, const char *path, size_t *out_len) {
    *out_len = 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg->cam_port);
    if (inet_pton(AF_INET, cfg->cam_host, &sa.sin_addr) != 1)
        return NULL; /* cam_host is always 127.0.0.1 in this deployment */

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct timeval tv = { 8, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return NULL;
    }

    char req[512];
    int qn = snprintf(req, sizeof req,
                       "GET %s?loginuse=admin&loginpas=%s&user=admin&pwd=%s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Accept: */*\r\n"
                       "Connection: close\r\n\r\n",
                       path, cfg->devpw, cfg->devpw, cfg->cam_host);
    if (qn <= 0 || write(fd, req, (size_t)qn) != qn) {
        close(fd);
        return NULL;
    }

    /* Read the whole response (headers + body); the camera closes the
     * connection when done since we sent Connection: close. */
    size_t cap = 65536, len = 0;
    uint8_t *raw = (uint8_t *)malloc(cap);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            raw = (uint8_t *)realloc(raw, cap);
        }
        ssize_t n = read(fd, raw + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len > 4 * 1024 * 1024) break; /* sanity cap, real snapshots are ~20KB */
    }
    close(fd);

    const uint8_t *body = NULL;
    size_t body_len = 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            body = raw + i + 4;
            body_len = len - (i + 4);
            break;
        }
    }
    /* JPEG magic is the only trustworthy signal (Content-Type is mislabeled). */
    if (!body || body_len < 4 || body[0] != 0xFF || body[1] != 0xD8 || body[2] != 0xFF) {
        free(raw);
        return NULL;
    }

    uint8_t *out = (uint8_t *)malloc(body_len);
    memcpy(out, body, body_len);
    free(raw);
    *out_len = body_len;
    return out;
}

static uint8_t *fetch_snapshot(const okam_config_t *cfg, size_t *out_len) {
    static const char *paths[] = { "/snapshot.cgi", "/onvifsnapshot.cgi" };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        uint8_t *buf = fetch_snapshot_path(cfg, paths[i], out_len);
        if (buf) return buf;
    }
    return NULL;
}

/* No auth check here at all -- Synology fetches this with its ONVIF creds
 * (or none), not the camera's internal devpw, so this daemon injects
 * devpw/vuid upstream itself; matches the other pre-auth discovery paths. */
static void handle_snapshot(onvif_soap_t *s, int fd) {
    size_t jlen = 0;
    uint8_t *jpeg = fetch_snapshot(&s->cfg, &jlen);
    char head[200];
    if (!jpeg) {
        static const char msg[] = "snapshot unavailable";
        int n = snprintf(head, sizeof head,
                          "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          sizeof msg - 1);
        if (write(fd, head, (size_t)n) > 0)
            (void)!write(fd, msg, sizeof msg - 1);
        LOGI("onvif: op=GetSnapshot(proxy) auth=none() -> 502");
        return;
    }
    int n = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", jlen);
    if (write(fd, head, (size_t)n) > 0) {
        size_t off = 0;
        while (off < jlen) {
            ssize_t w = write(fd, jpeg + off, jlen - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    LOGI("onvif: op=GetSnapshot(proxy) auth=none() -> 200 (%zu bytes)", jlen);
    free(jpeg);
}

/* ---------------------------------------------------------------- dispatch
 * `method` is the HTTP request method (e.g. "POST") and `authz` the raw
 * "Authorization:" header value (NULL/empty if absent) -- both needed for
 * HTTP Digest validation. The Digest "uri=" parameter (from the header
 * itself, echoing what the client used to compute its own response hash)
 * is used for HA2, not the request line's Request-URI, matching what most
 * minimal/embedded ONVIF servers do (RFC 2617 permits this simplification;
 * this is a single-NVR LAN camera, not a security boundary worth the extra
 * request-splicing hardening). */
static void dispatch(onvif_soap_t *s, int fd, const char *method, const char *req_path,
                      const char *authz, const char *body, size_t blen) {
    char resp[SOAP_RESP_CAP];
    char ip[64];
    device_ip(s, fd, ip, sizeof ip);
    char xaddr[160];
    snprintf(xaddr, sizeof xaddr, "http://%s:%d/onvif/device_service", ip, s->cfg.onvif_port);
    char event_xaddr[160];
    snprintf(event_xaddr, sizeof event_xaddr, "http://%s:%d/onvif/event_service", ip, s->cfg.onvif_port);
    int vid_w, vid_h;
    resolution(s, &vid_w, &vid_h);

    /* Auth is evaluated for EVERY request (not just when required) so it
     * can be logged either way, and so credentials are accepted additively
     * even on the pre-auth ops (a client that already sends a valid token
     * on GetCapabilities, say, is never punished for it). Priority: a
     * valid WS-Security UsernameToken, else a valid HTTP Digest/Basic
     * Authorization header. */
    const char *auth_kind = "none";
    char auth_user[64] = "";
    int authed = 0;

    wsse_token_t tok;
    wsse_parse_token(body, blen, &tok);
    if (wsse_authenticate(&tok, s->cfg.onvif_user, s->cfg.onvif_pass)) {
        authed = 1;
        auth_kind = "wsse";
        snprintf(auth_user, sizeof auth_user, "%s", tok.username);
    } else if (tok.present) {
        /* a token was sent but didn't validate -- still worth reporting */
        auth_kind = "wsse";
        snprintf(auth_user, sizeof auth_user, "%s", tok.username);
    }

    if (!authed) {
        char http_user[64] = "", http_scheme[16] = "";
        int http_ok = httpauth_validate(&s->httpauth, authz, method,
                                         s->cfg.onvif_user, s->cfg.onvif_pass,
                                         http_user, sizeof http_user,
                                         http_scheme, sizeof http_scheme);
        LOGD("onvif: %s HTTP auth: header_present=%d scheme='%s' user='%s' -> %s",
             ip, authz && authz[0], http_scheme, http_user, http_ok ? "pass" : "fail");
        if (http_scheme[0]) {
            authed = http_ok;
            auth_kind = (strcasecmp(http_scheme, "Digest") == 0) ? "digest" : "basic";
            snprintf(auth_user, sizeof auth_user, "%s", http_user);
        }
    }

    char auth_desc[96];
    snprintf(auth_desc, sizeof auth_desc, "%s(%s)", auth_kind, auth_user);

    /* Only the media-access op (GetStreamUri) actually requires `authed`;
     * every capability-discovery op succeeds regardless (see
     * action_requires_auth()'s comment / onvif_soap.h's access-policy note
     * -- this is what lets an NVR's add-camera wizard enumerate the camera
     * before it has credentials). */
    if (action_requires_auth(body, blen) && !authed) {
        LOGI("onvif: op=GetStreamUri auth=%s -> 401", auth_desc);
        int fn = snprintf(resp, sizeof resp, "%s", NOTAUTH_FAULT_TMPL);
        send_401(&s->httpauth, fd, resp, (size_t)fn);
        return;
    }

    const char *op = "Unknown";
    char unknown_op[80]; /* backing storage for `op` in the unmatched-action
                          * case -- declared here, not inside that branch, so
                          * it stays alive for the LOGI() call after the
                          * if/else chain (a nested block's locals don't). */
    int n, status = 200;
    const char *status_text = "OK";

    if (memmem_like(body, blen, "GetStreamUri")) {
        /* Deliberately TOKEN-AGNOSTIC: never reads trt:ProfileToken from the
         * request at all. There is only one real stream (vp_project's), so
         * whatever profile token the client asks about (the fixed
         * "profile0", or one it made up via CreateProfile) gets the same
         * rtsp:// URL back. */
        op = "GetStreamUri";
        char uri[160];
        snprintf(uri, sizeof uri, "rtsp://%s:%d/%s", ip, s->cfg.rtsp_port, s->cfg.rtsp_name);
        n = snprintf(resp, sizeof resp, STREAM_URI_TMPL, uri);

    } else if (memmem_like(body, blen, "GetSnapshotUri")) {
        /* Token-agnostic, same reasoning as GetStreamUri: one real snapshot
         * source (the /onvif/snapshot GET proxy), regardless of profile. */
        op = "GetSnapshotUri";
        char snap_uri[160];
        snprintf(snap_uri, sizeof snap_uri, "http://%s:%d/onvif/snapshot", ip, s->cfg.onvif_port);
        n = snprintf(resp, sizeof resp, SNAPSHOT_URI_TMPL, snap_uri);

    } else if (memmem_like(body, blen, "GetNTP")) {
        op = "GetNTP";
        n = snprintf(resp, sizeof resp, "%s", NTP_TMPL);

    } else if (memmem_like(body, blen, "GetRelayOutputs")) {
        op = "GetRelayOutputs";
        n = snprintf(resp, sizeof resp, "%s", RELAY_OUTPUTS_TMPL);

    } else if (memmem_like(body, blen, "GetVideoEncoderConfigurationOptions")) {
        op = "GetVideoEncoderConfigurationOptions";
        n = snprintf(resp, sizeof resp, VIDEO_ENC_OPTS_TMPL, vid_w, vid_h);

    } else if (memmem_like(body, blen, "GetVideoEncoderConfigurations")) {
        op = "GetVideoEncoderConfigurations";
        n = snprintf(resp, sizeof resp, VIDEO_ENC_CONF_TMPL,
                     vid_w, vid_h, (int)s->cfg.fps, (int)s->cfg.fps * 2);

    } else if (memmem_like(body, blen, "GetVideoEncoderConfiguration")) {
        op = "GetVideoEncoderConfiguration";
        n = snprintf(resp, sizeof resp, VIDEO_ENC_CONF_SINGLE_TMPL,
                     vid_w, vid_h, (int)s->cfg.fps, (int)s->cfg.fps * 2);

    } else if (memmem_like(body, blen, "GetVideoSourceConfigurations")) {
        op = "GetVideoSourceConfigurations";
        n = snprintf(resp, sizeof resp, VIDEO_SRC_CONF_TMPL, vid_w, vid_h);

    } else if (memmem_like(body, blen, "GetVideoSourceConfiguration")) {
        op = "GetVideoSourceConfiguration";
        n = snprintf(resp, sizeof resp, VIDEO_SRC_CONF_SINGLE_TMPL, vid_w, vid_h);

    } else if (memmem_like(body, blen, "GetVideoSources")) {
        op = "GetVideoSources";
        n = snprintf(resp, sizeof resp, VIDEO_SOURCES_TMPL, (int)s->cfg.fps, vid_w, vid_h);

    } else if (memmem_like(body, blen, "GetAudioSourceConfigurations")) {
        op = "GetAudioSourceConfigurations";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_SRC_CONF_TMPL);

    } else if (memmem_like(body, blen, "GetAudioSourceConfiguration")) {
        op = "GetAudioSourceConfiguration";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_SRC_CONF_SINGLE_TMPL);

    } else if (memmem_like(body, blen, "GetAudioEncoderConfigurationOptions")) {
        op = "GetAudioEncoderConfigurationOptions";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_ENC_OPTS_TMPL);

    } else if (memmem_like(body, blen, "GetAudioEncoderConfigurations")) {
        op = "GetAudioEncoderConfigurations";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_ENC_CONF_TMPL);

    } else if (memmem_like(body, blen, "GetAudioEncoderConfiguration")) {
        op = "GetAudioEncoderConfiguration";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_ENC_CONF_SINGLE_TMPL);

    } else if (memmem_like(body, blen, "GetAudioSources")) {
        op = "GetAudioSources";
        n = snprintf(resp, sizeof resp, "%s", AUDIO_SOURCES_TMPL);

    } else if (memmem_like(body, blen, "GetProfiles")) {
        op = "GetProfiles";
        n = snprintf(resp, sizeof resp, PROFILES_TMPL,
                     vid_w, vid_h,
                     vid_w, vid_h,
                     (int)s->cfg.fps, (int)s->cfg.fps * 2);

    } else if (memmem_like(body, blen, "GetProfile")) {
        op = "GetProfile";
        n = snprintf(resp, sizeof resp, PROFILE_TMPL,
                     vid_w, vid_h,
                     vid_w, vid_h,
                     (int)s->cfg.fps, (int)s->cfg.fps * 2);

    } else if (memmem_like(body, blen, "CreateProfile")) {
        /* This daemon relays one fixed vp_project stream and can't actually
         * create an independent profile -- "create" it by echoing the
         * requested Name/Token wrapped around the same real config
         * GetProfiles reports, so Synology's activation flow gets a
         * success it can proceed from (see GetStreamUri below, which never
         * looks at ProfileToken and so serves this "new" profile fine). */
        op = "CreateProfile";
        char req_name[64], req_token[64];
        extract_simple_value(body, blen, "Name", req_name, sizeof req_name, "profile1");
        extract_simple_value(body, blen, "Token", req_token, sizeof req_token, req_name);
        n = snprintf(resp, sizeof resp, CREATE_PROFILE_TMPL,
                     req_token, req_name,
                     vid_w, vid_h,
                     vid_w, vid_h,
                     (int)s->cfg.fps, (int)s->cfg.fps * 2);

    } else if (memmem_like(body, blen, "GetGuaranteedNumberOfVideoEncoderInstances")) {
        op = "GetGuaranteedNumberOfVideoEncoderInstances";
        n = snprintf(resp, sizeof resp, "%s", GUARANTEED_INSTANCES_TMPL);

    } else if (memmem_like(body, blen, "SetVideoEncoderConfiguration")) {
        /* Encoder is fixed (whatever vp_project is actually producing) --
         * accept and ignore rather than error, matching the task's ask. */
        op = "SetVideoEncoderConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "SetVideoSourceConfiguration")) {
        op = "SetVideoSourceConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "AddVideoEncoderConfiguration")) {
        op = "AddVideoEncoderConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "AddVideoSourceConfiguration")) {
        op = "AddVideoSourceConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "RemoveVideoEncoderConfiguration")) {
        op = "RemoveVideoEncoderConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "RemoveVideoSourceConfiguration")) {
        op = "RemoveVideoSourceConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "AddAudioEncoderConfiguration")) {
        op = "AddAudioEncoderConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "AddAudioSourceConfiguration")) {
        op = "AddAudioSourceConfiguration";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "SetProfile")) {
        op = "SetProfile";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "DeleteProfile")) {
        op = "DeleteProfile";
        n = snprintf(resp, sizeof resp, EMPTY_RESP_TMPL, op);

    } else if (memmem_like(body, blen, "GetNetworkInterfaces")) {
        op = "GetNetworkInterfaces";
        /* IFNAMSIZ-sized (matches struct ifreq.ifr_name in get_iface_mac()
         * below, avoiding a truncation warning on the cross-toolchain). */
        char ifname[IFNAMSIZ] = "vnet0", ifip[16] = "0.0.0.0", mac[24];
        find_network_iface(ifname, sizeof ifname, ifip, sizeof ifip);
        get_iface_mac(ifname, mac, sizeof mac);
        n = snprintf(resp, sizeof resp, NETIFS_TMPL, ifname, mac, 1500, ifip);

    } else if (memmem_like(body, blen, "GetDeviceInformation")) {
        op = "GetDeviceInformation";
        n = snprintf(resp, sizeof resp, DEV_INFO_TMPL,
                     s->cfg.manufacturer, s->cfg.model, s->cfg.serial, s->cfg.hardware_id);

    } else if (memmem_like(body, blen, "GetSystemDateAndTime")) {
        op = "GetSystemDateAndTime";
        time_t now = time(NULL);
        struct tm tmv; gmtime_r(&now, &tmv);
        n = snprintf(resp, sizeof resp, SYSTIME_TMPL,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

    } else if (memmem_like(body, blen, "GetHostname")) {
        op = "GetHostname";
        n = snprintf(resp, sizeof resp, HOSTNAME_TMPL, s->cfg.model);

    } else if (memmem_like(body, blen, "GetWsdlUrl")) {
        op = "GetWsdlUrl";
        n = snprintf(resp, sizeof resp, "%s", WSDLURL_TMPL);

    } else if (memmem_like(body, blen, "GetServiceCapabilities")) {
        /* Media and Events each have their own GetServiceCapabilities with a
         * different response shape/namespace, and both share this one
         * action name -- disambiguate by which XAddr the client actually
         * targeted (real ONVIF services live at different XAddrs; we
         * advertise a distinct .../event_service one specifically so this
         * works), defaulting to Media if the path doesn't say otherwise. */
        if (req_path && memmem_like(req_path, strlen(req_path), "event")) {
            op = "GetServiceCapabilities(Events)";
            n = snprintf(resp, sizeof resp, "%s", EVENT_SVC_CAPS_TMPL);
        } else {
            op = "GetServiceCapabilities(Media)";
            n = snprintf(resp, sizeof resp, "%s", SVC_CAPS_TMPL);
        }

    } else if (memmem_like(body, blen, "GetServices")) {
        op = "GetServices";
        n = snprintf(resp, sizeof resp, SERVICES_TMPL, xaddr, xaddr, event_xaddr);

    } else if (memmem_like(body, blen, "GetCapabilities")) {
        op = "GetCapabilities";
        n = snprintf(resp, sizeof resp, CAPS_TMPL, xaddr, event_xaddr, xaddr);

    } else if (memmem_like(body, blen, "GetScopes")) {
        op = "GetScopes";
        n = snprintf(resp, sizeof resp, SCOPES_TMPL, s->cfg.model, s->cfg.manufacturer);

    } else if (memmem_like(body, blen, "GetEventProperties")) {
        op = "GetEventProperties";
        n = snprintf(resp, sizeof resp, "%s", EVENT_PROPS_TMPL);

    } else if (memmem_like(body, blen, "CreatePullPointSubscription")) {
        op = "CreatePullPointSubscription";
        char addr[160], now_s[40], term_s[40];
        snprintf(addr, sizeof addr, "http://%s:%d/onvif/Events/PullSub_0", ip, s->cfg.onvif_port);
        time_t now = time(NULL);
        format_iso8601(now, now_s, sizeof now_s);
        format_iso8601(now + 60, term_s, sizeof term_s);
        n = snprintf(resp, sizeof resp, PULLPOINT_SUB_TMPL, addr, now_s, term_s);

    } else if (memmem_like(body, blen, "PullMessages")) {
        op = "PullMessages";
        char now_s[40], term_s[40];
        time_t now = time(NULL);
        format_iso8601(now, now_s, sizeof now_s);
        format_iso8601(now + 60, term_s, sizeof term_s);
        n = snprintf(resp, sizeof resp, PULL_MESSAGES_TMPL, now_s, term_s);

    } else if (memmem_like(body, blen, "Renew")) {
        op = "Renew";
        char now_s[40], term_s[40];
        time_t now = time(NULL);
        format_iso8601(now, now_s, sizeof now_s);
        format_iso8601(now + 60, term_s, sizeof term_s);
        n = snprintf(resp, sizeof resp, RENEW_TMPL, term_s, now_s);

    } else if (memmem_like(body, blen, "Unsubscribe")) {
        op = "Unsubscribe";
        n = snprintf(resp, sizeof resp, "%s", UNSUBSCRIBE_TMPL);

    } else if (memmem_like(body, blen, "SetSynchronizationPoint")) {
        op = "SetSynchronizationPoint";
        n = snprintf(resp, sizeof resp, "%s", SYNC_POINT_TMPL);

    } else {
        extract_unknown_op(body, blen, unknown_op, sizeof unknown_op);
        op = unknown_op;
        n = snprintf(resp, sizeof resp, "%s", ACTION_NOT_SUPPORTED_TMPL);
        status = 400;
        status_text = "Bad Request";
    }

    LOGI("onvif: op=%s auth=%s -> %d", op, auth_desc, status);
    send_http(fd, status, status_text, resp, (size_t)n);
}

/* ------------------------------------------------------------- HTTP framing */
static void *handle_conn(void *arg_) {
    conn_arg_t *a = (conn_arg_t *)arg_;
    onvif_soap_t *s = a->s;
    int fd = a->fd;
    free(a);

    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    char buf[SOAP_RECV_CAP];
    size_t len = 0;
    const char *hdr_end = NULL;
    while (len < sizeof buf - 1) {
        ssize_t n = recv(fd, buf + len, sizeof buf - 1 - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = '\0';
        hdr_end = memmem_like(buf, len, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (!hdr_end) { close(fd); return NULL; }

    size_t hdr_len = (size_t)(hdr_end - buf) + 4;
    long content_length = 0;
    const char *cl = memmem_like(buf, hdr_len, "ontent-Length:");
    if (!cl) cl = memmem_like(buf, hdr_len, "ontent-length:");
    if (cl) content_length = strtol(cl + strlen("ontent-Length:"), NULL, 10);

    /* Request line: "METHOD SP Request-URI SP HTTP-Version". `path` is only
     * used to disambiguate the Media-vs-Events GetServiceCapabilities
     * action-name collision (see dispatch()) -- every other action is
     * still identified purely by body content, matching this daemon's
     * one-endpoint-for-everything design. */
    char method[16] = "", path[128] = "";
    sscanf(buf, "%15s %127s", method, path);

    /* GET /onvif/snapshot is a plain HTTP JPEG proxy, not a SOAP action --
     * short-circuit before any of the SOAP/Content-Length/body handling
     * below (a GET has no body, and this isn't dispatch()'d by action
     * name). See handle_snapshot()/fetch_snapshot() above. */
    if (!strcmp(method, "GET") && memmem_like(path, strlen(path), "snapshot")) {
        handle_snapshot(s, fd);
        close(fd);
        return NULL;
    }

    /* "Authorization:" header (case-tolerant on its leading letter, same
     * trick used for Content-Length above; every real client is
     * consistent about the rest of the header name's casing). */
    char authz[512] = "";
    const char *az = memmem_like(buf, hdr_len, "uthorization:"); /* matches both
                       "Authorization:" and "authorization:" (case-tolerant on
                       the leading letter only, like the Content-Length trick
                       above -- every real client is consistent about the
                       rest of a header name's casing) */
    if (az) {
        const char *v = az + strlen("uthorization:");
        while (*v == ' ' || *v == '\t') v++;
        const char *end = v;
        while (end < buf + hdr_len && *end != '\r' && *end != '\n') end++;
        size_t n = (size_t)(end - v);
        if (n >= sizeof authz) n = sizeof authz - 1;
        memcpy(authz, v, n);
        authz[n] = '\0';
    }

    size_t body_have = len - hdr_len;
    while (body_have < (size_t)content_length && len < sizeof buf - 1) {
        ssize_t n = recv(fd, buf + len, sizeof buf - 1 - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        body_have += (size_t)n;
    }

    const char *body = buf + hdr_len;
    size_t blen = len - hdr_len;

    /* Raw-capture the first ~15 requests (request line + all headers + up
     * to 400 bytes of body) to /tmp/onvif_raw.log for field debugging --
     * capped by count, not size, so this can't grow unbounded on tmpfs.
     * The FIRST write of each daemon run truncates rather than appends, so
     * a restarted daemon starts a fresh capture instead of growing the
     * file forever across restarts. */
    pthread_mutex_lock(&s->raw_log_mu);
    int do_raw = s->raw_log_count < 15;
    if (do_raw) s->raw_log_count++;
    int raw_idx = s->raw_log_count;
    pthread_mutex_unlock(&s->raw_log_mu);
    if (do_raw) {
        FILE *rf = fopen("/tmp/onvif_raw.log", raw_idx == 1 ? "w" : "a");
        if (rf) {
            fprintf(rf, "==== request #%d (headers %zu bytes, body %zu bytes) ====\n",
                    raw_idx, hdr_len, blen);
            fwrite(buf, 1, hdr_len, rf);
            size_t bshow = blen < 400 ? blen : 400;
            fwrite(body, 1, bshow, rf);
            fprintf(rf, "\n==== end request #%d%s ====\n\n", raw_idx,
                    blen > bshow ? " (body truncated)" : "");
            fclose(rf);
        }
    }

    dispatch(s, fd, method, path, authz, body, blen);

    close(fd);
    return NULL;
}

static void *accept_loop(void *arg) {
    onvif_soap_t *s = (onvif_soap_t *)arg;
    while (!s->stop) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(s->listen_fd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (s->stop) break;
            continue;
        }
        conn_arg_t *a = (conn_arg_t *)malloc(sizeof *a);
        a->s = s; a->fd = fd;
        pthread_t th;
        pthread_create(&th, NULL, handle_conn, a);
        pthread_detach(th);
    }
    return NULL;
}

void onvif_soap_init(onvif_soap_t *s, const okam_config_t *cfg, const char *device_uuid,
                      cam_source_t *src) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->src = src;
    s->listen_fd = -1;
    if (device_uuid && *device_uuid)
        snprintf(s->uuid, sizeof s->uuid, "%s", device_uuid);
    else
        gen_uuid(s->uuid);
    httpauth_init(&s->httpauth);
    pthread_mutex_init(&s->raw_log_mu, NULL);
}

int onvif_soap_run(onvif_soap_t *s) {
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) { LOGE("onvif: socket() failed: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)s->cfg.onvif_port);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOGE("onvif: bind :%d failed: %s", s->cfg.onvif_port, strerror(errno));
        return -1;
    }
    if (listen(s->listen_fd, 8) != 0) {
        LOGE("onvif: listen failed: %s", strerror(errno));
        return -1;
    }
    LOGI("onvif: SOAP service on :%d/onvif/device_service", s->cfg.onvif_port);
    accept_loop(s);
    return 0;
}

void onvif_soap_stop(onvif_soap_t *s) {
    s->stop = 1;
    if (s->listen_fd >= 0) {
        shutdown(s->listen_fd, SHUT_RDWR);
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}
