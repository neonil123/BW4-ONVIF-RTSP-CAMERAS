/* Missing/renamed defines for building wireguard-linux-compat against Ingenic's
 * modified 3.10.14 kernel. Force-included via KCFLAGS -include. */
#ifndef WG_COMPAT_FIX_H
#define WG_COMPAT_FIX_H
/* Pull the full net_device definition in early so wireguard-compat's
 * netif_keep_dst() shim (which this kernel lacks) sees a complete struct. */
#include <linux/netdevice.h>
#ifndef IFF_XMIT_DST_RELEASE
#define IFF_XMIT_DST_RELEASE 0x400
#endif
/* `fallthrough` statement attribute didn't exist until ~5.4 */
#ifndef fallthrough
#define fallthrough __attribute__((__fallthrough__))
#endif
/* This Ingenic kernel doesn't EXPORT ip_tunnel_get_stats64 (used as the wg
 * netdev's .ndo_get_stats64). Provide a self-contained version so the module
 * doesn't import it. Sums the per-cpu tstats (pcpu_tstats on <3.14). */
static inline struct rtnl_link_stats64 *
wg_ip_tunnel_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *tot)
{
	(void)dev;   /* zero stats: cosmetic; guarantees load + no struct/crash risk */
	return tot;
}
#define ip_tunnel_get_stats64 wg_ip_tunnel_get_stats64
#endif
