/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include <ifaddrs.h>
#include "defs.h"
#include "queue.h"

struct iflist {
    struct uvif         ifl_uv;
    TAILQ_ENTRY(iflist) ifl_link;
};

static TAILQ_HEAD(ifi_head, iflist) ifl_kern = TAILQ_HEAD_INITIALIZER(ifl_kern);

void config_set_ifflag(uint32_t flag)
{
    struct iflist *ifl;

    TAILQ_FOREACH(ifl, &ifl_kern, ifl_link)
	ifl->ifl_uv.uv_flags |= flag;
}

struct uvif *config_find_ifname(char *nm)
{
    struct iflist *ifl;

    if (!nm) {
	errno = EINVAL;
	return NULL;
    }

    TAILQ_FOREACH(ifl, &ifl_kern, ifl_link) {
	struct uvif *uv = &ifl->ifl_uv;

        if (!strcmp(uv->uv_name, nm))
            return uv;
    }

    return NULL;
}

struct uvif *config_find_ifaddr(in_addr_t addr)
{
    struct iflist *ifl;

    TAILQ_FOREACH(ifl, &ifl_kern, ifl_link) {
	struct uvif *uv = &ifl->ifl_uv;

	if (!(uv->uv_flags & VIFF_TUNNEL) && addr == uv->uv_lcl_addr)
            return uv;
    }

    return NULL;
}

struct uvif *config_init_tunnel(in_addr_t lcl_addr, in_addr_t rmt_addr, uint32_t flags)
{
    const char *ifname;
    struct iflist *ifl;
    struct ifreq ffr;
    struct uvif *v;

    v = config_find_ifaddr(lcl_addr);
    if (!v) {
	errno = ENOENT;
	return NULL;
    }
    ifname = v->uv_name;

    if (((ntohl(lcl_addr) & IN_CLASSA_NET) >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET) {
	errno = ENONET;
	return NULL;
    }

    if (config_find_ifaddr(rmt_addr)) {
	errno = EADDRINUSE;
	return NULL;
    }

    TAILQ_FOREACH(ifl, &ifl_kern, ifl_link) {
	v = &ifl->ifl_uv;

	if (v->uv_flags & VIFF_DISABLED)
	    continue;

	if (v->uv_flags & VIFF_TUNNEL) {
	    if (rmt_addr == v->uv_rmt_addr) {
		errno = EEXIST;
		return NULL;
	    }

	    continue;
	}

	if ((rmt_addr & v->uv_subnetmask) == v->uv_subnet) {
	    logit(LOG_INFO, 0,
		  "Unnecessary tunnel to %s, same subnet as interface %s",
		  inet_fmt(rmt_addr, s1, sizeof(s1)), v->uv_name);
	    return NULL;
	}
    }

    strlcpy(ffr.ifr_name, ifname, sizeof(ffr.ifr_name));
    if (ioctl(udp_socket, SIOCGIFFLAGS, &ffr) < 0) {
	logit(LOG_INFO, errno, "failed SIOCGIFFLAGS on %s", ffr.ifr_name);
	return NULL;
    }

    ifl = calloc(1, sizeof(struct iflist));
    if (!ifl) {
	logit(LOG_ERR, errno, "failed allocating memory for iflist");
	return NULL;
    }

    v = &ifl->ifl_uv;

    zero_vif(v, 1);
    v->uv_flags      = VIFF_TUNNEL | flags;
    v->uv_flags     |= VIFF_OTUNNEL; /* XXX */
    v->uv_lcl_addr   = lcl_addr;
    v->uv_rmt_addr   = rmt_addr;
    v->uv_dst_addr   = rmt_addr;
    strlcpy(v->uv_name, ffr.ifr_name, sizeof(v->uv_name));

    if (!(ffr.ifr_flags & IFF_UP)) {
	v->uv_flags |= VIFF_DOWN;
	vifs_down = TRUE;
    }

    TAILQ_INSERT_TAIL(&ifl_kern, ifl, ifl_link);

    return v;
}

void config_vifs_correlate(void)
{
    struct iflist *ifl, *tmp;
    vifi_t vifi = 0;

    TAILQ_FOREACH(ifl, &ifl_kern, ifl_link) {
	struct uvif *uv = &ifl->ifl_uv;
	struct uvif *v;

	if (uv->uv_flags & VIFF_DISABLED) {
	    logit(LOG_DEBUG, 0, "Skipping %s, disabled", uv->uv_name);
	    continue;
	}

	/*
	 * Ignore any interface that is connected to the same subnet as
	 * one already installed in the uvifs array.
	 */
	for (vifi = 0, v = uvifs; vifi < numvifs; ++vifi, ++v) {
	    if (strcmp(v->uv_name, uv->uv_name) == 0) {
		logit(LOG_DEBUG, 0, "skipping %s (%s on subnet %s) (alias for vif#%u?)",
		      uv->uv_name, inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)),
		      inet_fmts(uv->uv_subnet, uv->uv_subnetmask, s2, sizeof(s2)), vifi);
		break;
	    }
	    if ((uv->uv_lcl_addr & v->uv_subnetmask) == v->uv_subnet ||
		(v->uv_subnet & uv->uv_subnetmask) == uv->uv_subnet) {
		logit(LOG_WARNING, 0, "ignoring %s, same subnet as %s",
		      uv->uv_name, v->uv_name);
		break;
	    }
	}

	if (vifi != numvifs)
	    continue;

	/*
	 * If there is room in the uvifs array, install this interface.
	 */
	if (numvifs == MAXVIFS) {
	    logit(LOG_WARNING, 0, "too many vifs, ignoring %s", uv->uv_name);
	    continue;
	}

	logit(LOG_INFO, 0, "Installing %s (%s on subnet %s) as VIF #%u, rate %d pps",
	      uv->uv_name, inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)),
	      inet_fmts(uv->uv_subnet, uv->uv_subnetmask, s2, sizeof(s2)),
	      numvifs, v->uv_rate_limit);

	uvifs[numvifs++] = *uv;
    }

    /*
     * XXX: one future extension may be to keep this for adding/removing
     *      dynamic interfaces at runtime.  Then it should probably only
     *      be freed on SIGHUP/exit().  Now we free it and let SIGHUP
     *      rebuild it to recheck since we tear down all vifs anyway.
     */
    TAILQ_FOREACH_SAFE(ifl, &ifl_kern, ifl_link, tmp)
	free(ifl);
}

/*
 * Query the kernel to find network interfaces that are multicast-capable
 * and install them in the uvifs array.
 */
void config_vifs_from_kernel(void)
{
    in_addr_t addr, mask, subnet;
    struct ifaddrs *ifa, *ifap;
    struct iflist *ifl;
    struct uvif *v;
    vifi_t vifi;
    int flags;

    if (getifaddrs(&ifap) < 0)
	logit(LOG_ERR, errno, "getifaddrs");

    /*
     * Loop through all of the interfaces.
     */
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
	/*
	 * Ignore any interface for an address family other than IP.
	 */
	if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
//	    logit(LOG_INFO, 0, "skipping (this instance of) %s, no IPv4 address.", ifa->ifa_name);
	    continue;
	}

	/*
	 * Ignore interfaces that do not support multicast.
	 */
	flags = ifa->ifa_flags;
	if (!(flags & IFF_MULTICAST)) {
	    logit(LOG_INFO, 0, "skipping %s, does not support multicast.", ifa->ifa_name);
	    continue;
	}

	/*
	 * Perform some sanity checks on the address and subnet, ignore any
	 * interface whose address and netmask do not define a valid subnet.
	 */
	addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
	mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr;
	subnet = addr & mask;
	if (!inet_valid_subnet(subnet, mask) || (addr != subnet && addr == (subnet & ~mask))) {
	    logit(LOG_WARNING, 0, "ignoring %s, has invalid address (%s) and/or mask (%s)",
		  ifa->ifa_name, inet_fmt(addr, s1, sizeof(s1)), inet_fmt(mask, s2, sizeof(s2)));
	    continue;
	}

	ifl = calloc(1, sizeof(struct iflist));
        if (!ifl) {
            logit(LOG_ERR, errno, "failed allocating memory for iflist");
            return;
        }

	v = &ifl->ifl_uv;
	zero_vif(v, 0);

	strlcpy(v->uv_name, ifa->ifa_name, sizeof(ifl->ifl_uv.uv_name));
	v->uv_lcl_addr    = addr;
	v->uv_subnet      = subnet;
	v->uv_subnetmask  = mask;
	v->uv_subnetbcast = subnet | ~mask;

	if (ifa->ifa_flags & IFF_POINTOPOINT)
	    v->uv_flags |= VIFF_REXMIT_PRUNES;

	/*
	 * If the interface is not yet up, set the vifs_down flag to
	 * remind us to check again later.
	 */
	if (!(flags & IFF_UP)) {
	    v->uv_flags |= VIFF_DOWN;
	    vifs_down = TRUE;
	}

	TAILQ_INSERT_TAIL(&ifl_kern, ifl, ifl_link);
    }

    freeifaddrs(ifap);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
