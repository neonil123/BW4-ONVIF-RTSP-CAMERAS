/* wg_create <ifname> : create a WireGuard netdev via rtnetlink (the kernel module
 * registers the "wireguard" rtnl link kind). Replaces `ip link add <if> type
 * wireguard` on a device with no iproute2. Tiny static musl binary. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>

static void addattr(struct nlmsghdr *n, int type, const void *data, int alen){
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = RTA_LENGTH(alen);
    if (alen) memcpy(RTA_DATA(rta), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(RTA_LENGTH(alen));
}

int main(int argc, char **argv){
    const char *ifname = (argc > 1) ? argv[1] : "wg0";
    const char *kind = "wireguard";
    struct { struct nlmsghdr n; struct ifinfomsg i; char buf[512]; } req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.n.nlmsg_type  = RTM_NEWLINK;
    req.i.ifi_family  = AF_UNSPEC;

    addattr(&req.n, IFLA_IFNAME, ifname, strlen(ifname) + 1);
    /* nested IFLA_LINKINFO { IFLA_INFO_KIND = "wireguard" } */
    struct rtattr *linkinfo = (struct rtattr *)((char *)&req.n + NLMSG_ALIGN(req.n.nlmsg_len));
    linkinfo->rta_type = IFLA_LINKINFO;
    linkinfo->rta_len  = RTA_LENGTH(0);
    req.n.nlmsg_len = NLMSG_ALIGN(req.n.nlmsg_len) + RTA_ALIGN(RTA_LENGTH(0));
    addattr(&req.n, IFLA_INFO_KIND, kind, strlen(kind) + 1);
    linkinfo->rta_len = (char *)&req.n + req.n.nlmsg_len - (char *)linkinfo;

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_nl sa; memset(&sa, 0, sizeof(sa)); sa.nl_family = AF_NETLINK;
    if (send(fd, &req, req.n.nlmsg_len, 0) < 0) { perror("send"); return 2; }

    char rbuf[1024];
    int r = recv(fd, rbuf, sizeof(rbuf), 0);
    if (r >= (int)sizeof(struct nlmsghdr)) {
        struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
        if (h->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
            if (e->error && e->error != -EEXIST) {
                fprintf(stderr, "wg_create: netlink error %d (%s)\n", e->error, strerror(-e->error));
                return 3;
            }
        }
    }
    printf("wg_create: %s ready\n", ifname);
    return 0;
}
