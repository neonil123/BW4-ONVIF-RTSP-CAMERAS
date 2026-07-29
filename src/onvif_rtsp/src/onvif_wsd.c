#include "onvif_wsd.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WSD_MCAST_ADDR    "239.255.255.250"
#define WSD_PORT          3702
#define WSD_RECV_BUF      8192
#define WSD_PRIMARY_IFACE "vnet0"   /* the QC3's WLAN interface (AIC8800 SDIO) */

/* Namespace prefixes (e/wsa/d/dn/tds) and element shape match exactly what
 * the acceptance harness (tools/onvif_test) expects -- some quick WS-
 * Discovery test harnesses match on literal prefixes rather than resolving
 * namespace URIs properly, so this isn't just cosmetic. */
static const char PROBE_MATCH_TMPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    "            xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\r\n"
    "            xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\r\n"
    "            xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"\r\n"
    "            xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\r\n"
    "    <e:Header>\r\n"
    "        <wsa:MessageID>urn:uuid:%s</wsa:MessageID>\r\n"
    "        <wsa:RelatesTo>%s</wsa:RelatesTo>\r\n"
    "        <wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>\r\n"
    "        <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>\r\n"
    "        <d:AppSequence InstanceId=\"0\" MessageNumber=\"%d\"/>\r\n"
    "    </e:Header>\r\n"
    "    <e:Body>\r\n"
    "        <d:ProbeMatches>\r\n"
    "            <d:ProbeMatch>\r\n"
    "                <wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address></wsa:EndpointReference>\r\n"
    "                <d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>\r\n"
    "                <d:Scopes>onvif://www.onvif.org/type/video_encoder"
                    " onvif://www.onvif.org/hardware/%s"
                    " onvif://www.onvif.org/name/%s"
                    " onvif://www.onvif.org/location/anywhere"
                    " onvif://www.onvif.org/Profile/Streaming</d:Scopes>\r\n"
    "                <d:XAddrs>%s</d:XAddrs>\r\n"
    "                <d:MetadataVersion>1</d:MetadataVersion>\r\n"
    "            </d:ProbeMatch>\r\n"
    "        </d:ProbeMatches>\r\n"
    "    </e:Body>\r\n"
    "</e:Envelope>\r\n";

void onvif_wsd_init(onvif_wsd_t *w, int onvif_port, const char *manufacturer,
                     const char *model, const char *device_uuid) {
    memset(w, 0, sizeof *w);
    w->port = onvif_port;
    w->sock = -1;
    if (device_uuid && *device_uuid)
        snprintf(w->uuid, sizeof w->uuid, "%s", device_uuid);
    else
        gen_uuid(w->uuid);
    snprintf(w->manufacturer, sizeof w->manufacturer, "%s", manufacturer);
    snprintf(w->model, sizeof w->model, "%s", model);
}

/* Tiny local memmem (avoid depending on the GNU extension being present on
 * every target libc). */
static const char *memmem_like(const void *hay, size_t hlen, const void *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return NULL;
    const char *h = (const char *)hay;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, needle, nlen) == 0)
            return h + i;
    return NULL;
}

/* Extracts the contents of the first <...MessageID>...</...MessageID> tag
 * (namespace-prefix agnostic) so we can echo it back as RelatesTo, matching
 * what every WS-Discovery client expects to correlate the reply. */
static void extract_message_id(const char *body, size_t len, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s", "urn:uuid:00000000-0000-0000-0000-000000000000");
    const char *tag = "MessageID>";
    const char *p = memmem_like(body, len, tag, strlen(tag));
    if (!p) return;
    p += strlen(tag);
    const char *end = memchr(p, '<', (size_t)((body + len) - p));
    if (!end) return;
    size_t n = (size_t)(end - p);
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Finds the interface to join the WS-Discovery multicast group on, and its
 * IPv4 address (needed for ProbeMatch XAddrs since a UDP responder has no
 * per-client accepted socket to getsockname() on, unlike the RTSP/SOAP
 * servers). INADDR_ANY joins (imr_interface = 0) fail with ENODEV on this
 * device's WiFi driver (AIC8800 SDIO) -- an explicit interface is required.
 * Prefers WSD_PRIMARY_IFACE ("vnet0"); falls back to the first UP,
 * non-loopback IPv4 interface found so the daemon still works if the
 * interface is ever renamed. Returns 0 and fills ifname_out/addr_out on
 * success, -1 if no usable interface has an IPv4 address yet (e.g. WiFi
 * still associating). */
static int wsd_find_iface(char *ifname_out, size_t ifname_cap, struct in_addr *addr_out) {
    struct ifaddrs *ifas, *p;
    if (getifaddrs(&ifas) != 0)
        return -1;

    int found = 0;
    for (p = ifas; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(p->ifa_name, WSD_PRIMARY_IFACE) != 0)
            continue;
        *addr_out = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
        snprintf(ifname_out, ifname_cap, "%s", p->ifa_name);
        found = 1;
        break;
    }
    if (!found) {
        for (p = ifas; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
                continue;
            if (p->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!(p->ifa_flags & IFF_UP))
                continue;
            *addr_out = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
            snprintf(ifname_out, ifname_cap, "%s", p->ifa_name);
            found = 1;
            break;
        }
    }
    freeifaddrs(ifas);
    return found ? 0 : -1;
}

/* Joins 239.255.255.250 on a specific interface. Tries the ifindex-based
 * ip_mreqn form first (more robust on drivers that reject an
 * address-derived join), then falls back to the address-based ip_mreq
 * form. Returns 0 on success. */
static int wsd_join_group(int sock, const char *ifname, struct in_addr ifaddr) {
    struct ip_mreqn mreqn;
    memset(&mreqn, 0, sizeof mreqn);
    inet_pton(AF_INET, WSD_MCAST_ADDR, &mreqn.imr_multiaddr);
    mreqn.imr_address = ifaddr;
    mreqn.imr_ifindex = (int)if_nametoindex(ifname);
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreqn, sizeof mreqn) == 0)
        return 0;
    LOGW("wsd: IP_ADD_MEMBERSHIP (ifindex=%d, %s) failed: %s",
         mreqn.imr_ifindex, ifname, strerror(errno));

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof mreq);
    inet_pton(AF_INET, WSD_MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface = ifaddr;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) == 0)
        return 0;
    LOGW("wsd: IP_ADD_MEMBERSHIP (addr=%s) failed: %s", inet_ntoa(ifaddr), strerror(errno));
    return -1;
}

int onvif_wsd_run(onvif_wsd_t *w) {
    w->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (w->sock < 0) { LOGE("wsd: socket() failed: %s", strerror(errno)); return -1; }

    int one = 1;
    setsockopt(w->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(w->sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(WSD_PORT);
    if (bind(w->sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        LOGE("wsd: bind :%d failed: %s", WSD_PORT, strerror(errno));
        close(w->sock); w->sock = -1;
        return -1;
    }

    struct in_addr ifaddr;
    ifaddr.s_addr = INADDR_ANY;
    if (wsd_find_iface(w->iface_name, sizeof w->iface_name, &ifaddr) == 0) {
        snprintf(w->iface_ip, sizeof w->iface_ip, "%s", inet_ntoa(ifaddr));
        LOGI("wsd: joining %s on interface %s (%s)", WSD_MCAST_ADDR, w->iface_name, w->iface_ip);
        if (wsd_join_group(w->sock, w->iface_name, ifaddr) != 0)
            LOGE("wsd: could not join multicast group on any interface "
                 "(probing will not be seen until this is fixed)");
    } else {
        LOGW("wsd: no UP IPv4 interface found yet (WiFi still associating?); "
             "falling back to INADDR_ANY (may ENODEV on this driver)");
        if (wsd_join_group(w->sock, "", ifaddr) != 0)
            LOGE("wsd: could not join multicast group (probing will not be seen)");
    }

    LOGI("wsd: WS-Discovery responder on %s:%d", WSD_MCAST_ADDR, WSD_PORT);

    char buf[WSD_RECV_BUF];
    int msgnum = 1;
    while (!w->stop) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(w->sock, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (w->stop) break;
            continue;
        }
        buf[n] = '\0';
        /* Logged at INFO (not DEBUG) for now -- this is exactly the receipt
         * confirmation needed to debug "join works but no reply seen"
         * reports without requiring log_level=3 in the field. */
        LOGI("wsd: recv %zd bytes from %s:%d", n, inet_ntoa(from.sin_addr), ntohs(from.sin_port));

        /* Answer any Probe liberally (regardless of its Types filter), and
         * never reply to a foreign/echoed ProbeMatch(es). memmem_like's
         * needle length must exclude memory the string doesn't contain: no
         * trailing NUL is matched since we pass strlen(), not sizeof(). */
        int has_probe = memmem_like(buf, (size_t)n, "Probe", 5) != NULL;
        int has_match = memmem_like(buf, (size_t)n, "ProbeMatch", 10) != NULL;
        if (!has_probe || has_match) {
            LOGI("wsd: ignoring datagram (has_probe=%d has_match=%d)", has_probe, has_match);
            continue;
        }

        char rel_to[128];
        extract_message_id(buf, (size_t)n, rel_to, sizeof rel_to);

        char my_uuid[40];
        gen_uuid(my_uuid);
        char local_ip[16];
        if (w->iface_ip[0])
            snprintf(local_ip, sizeof local_ip, "%s", w->iface_ip);
        else
            local_ip_for(from.sin_addr, local_ip); /* best-effort fallback */
        char xaddr[128];
        snprintf(xaddr, sizeof xaddr, "http://%s:%d/onvif/device_service", local_ip, w->port);

        char name[80];
        snprintf(name, sizeof name, "%s_%s", w->manufacturer, w->model);

        char resp[2048];
        int rn = snprintf(resp, sizeof resp, PROBE_MATCH_TMPL,
                           my_uuid, rel_to, msgnum++, w->uuid, w->model, name, xaddr);
        /* Reply UNICAST directly to the Probe's source address+port (`from`,
         * as filled by recvfrom above) -- never to the multicast group or
         * to :3702 -- since that's the ephemeral socket the client is
         * actually listening on. */
        ssize_t sent = sendto(w->sock, resp, (size_t)rn, 0, (struct sockaddr *)&from, fl);
        if (sent < 0)
            LOGE("wsd: sendto ProbeMatch reply failed: %s", strerror(errno));
        else
            LOGI("wsd: ProbeMatch -> %s:%d (%d bytes, %s)",
                 inet_ntoa(from.sin_addr), ntohs(from.sin_port), (int)sent, xaddr);
    }

    close(w->sock);
    w->sock = -1;
    LOGI("wsd: stopped");
    return 0;
}

void onvif_wsd_stop(onvif_wsd_t *w) {
    w->stop = 1;
    if (w->sock >= 0)
        shutdown(w->sock, SHUT_RDWR);
}
