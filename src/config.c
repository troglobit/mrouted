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

static TAILQ_HEAD(, uvif) vifs = TAILQ_HEAD_INITIALIZER(vifs);

void config_set_ifflag(uint32_t flag)
{
    struct uvif *uv;

    TAILQ_FOREACH(uv, &vifs, uv_link)
	uv->uv_flags |= flag;
}

struct uvif *config_find_ifname(char *nm)
{
    struct uvif *uv;

    if (!nm) {
	errno = EINVAL;
	return NULL;
    }

    TAILQ_FOREACH(uv, &vifs, uv_link) {
        if (!strcmp(uv->uv_name, nm))
            return uv;
    }

    return NULL;
}

struct uvif *config_find_ifaddr(in_addr_t addr)
{
    struct uvif *uv;

    TAILQ_FOREACH(uv, &vifs, uv_link) {
	if (!(uv->uv_flags & VIFF_TUNNEL) && addr == uv->uv_lcl_addr)
            return uv;
    }

    return NULL;
}

struct uvif *config_init_tunnel(in_addr_t lcl_addr, in_addr_t rmt_addr, uint32_t flags)
{
    const char *ifname;
    struct ifreq ifr;
    struct uvif *uv;

    uv = config_find_ifaddr(lcl_addr);
    if (!uv) {
	errno = ENOTMINE;
	return NULL;
    }
    ifname = uv->uv_name;

    if (((ntohl(lcl_addr) & IN_CLASSA_NET) >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET) {
	errno = ELOOPBACK;
	return NULL;
    }

    if (config_find_ifaddr(rmt_addr)) {
	errno = ERMTLOCAL;
	return NULL;
    }

    TAILQ_FOREACH(uv, &vifs, uv_link) {
	if (uv->uv_flags & VIFF_DISABLED)
	    continue;

	if (uv->uv_flags & VIFF_TUNNEL) {
	    if (rmt_addr == uv->uv_rmt_addr) {
		errno = EDUPLICATE;
		return NULL;
	    }

	    continue;
	}

	if ((rmt_addr & uv->uv_subnetmask) == uv->uv_subnet) {
	    logit(LOG_INFO, 0,
		  "Unnecessary tunnel to %s, same subnet as interface %s",
		  inet_fmt(rmt_addr, s1, sizeof(s1)), uv->uv_name);
	    return NULL;
	}
    }

    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
    if (ioctl(udp_socket, SIOCGIFFLAGS, &ifr) < 0) {
	logit(LOG_INFO, errno, "failed SIOCGIFFLAGS on %s", ifr.ifr_name);
	return NULL;
    }

    uv = calloc(1, sizeof(struct uvif));
    if (!uv) {
	logit(LOG_ERR, errno, "failed allocating memory for iflist");
	return NULL;
    }

    zero_vif(uv, 1);
    uv->uv_flags      = VIFF_TUNNEL | flags;
    uv->uv_flags     |= VIFF_OTUNNEL; /* XXX */
    uv->uv_lcl_addr   = lcl_addr;
    uv->uv_rmt_addr   = rmt_addr;
    uv->uv_dst_addr   = rmt_addr;
    strlcpy(uv->uv_name, ifr.ifr_name, sizeof(uv->uv_name));

    uv->uv_ifindex = if_nametoindex(uv->uv_name);
    if (!uv->uv_ifindex)
	logit(LOG_ERR, errno, "Failed reading ifindex for %s", uv->uv_name);

    if (!(ifr.ifr_flags & IFF_UP)) {
	uv->uv_flags |= VIFF_DOWN;
	vifs_down = TRUE;
    }

    TAILQ_INSERT_TAIL(&vifs, uv, uv_link);

    return uv;
}

void config_vifs_correlate(void)
{
    struct listaddr *al, *al_tmp;
    struct uvif *uv, *v, *tmp;
    vifi_t vifi;

    TAILQ_FOREACH_SAFE(v, &vifs, uv_link, tmp) {
	if (v->uv_flags & VIFF_DISABLED) {
	    logit(LOG_DEBUG, 0, "Skipping %s, disabled", v->uv_name);
	    continue;
	}

	/*
	 * Ignore any interface that is connected to the same subnet as
	 * one already installed in the uvifs[] array.
	 */
	UVIF_FOREACH(vifi, uv) {
	    if ((v->uv_lcl_addr & uv->uv_subnetmask) == uv->uv_subnet ||
		(uv->uv_subnet  &  v->uv_subnetmask) ==  v->uv_subnet) {
		logit(LOG_WARNING, 0, "ignoring %s, same subnet as %s",
		      v->uv_name, uv->uv_name);
		break;
	    }

	    /*
	     * Same interface, but cannot have multiple VIFs on same
	     * interface so add as secondary IP address to RPF
	     */
	    if (strcmp(v->uv_name, uv->uv_name) == 0) {
		struct phaddr *ph;

		ph = calloc(1, sizeof(*ph));
		if (!ph) {
		    logit(LOG_ERR, errno, "Failed allocating altnet on %s", uv->uv_name);
		    break;
		}

		logit(LOG_INFO, 0, "Installing %s subnet %s as an altnet", v->uv_name,
		      inet_fmts(v->uv_subnet, v->uv_subnetmask, s2, sizeof(s2)));

		ph->pa_subnet      = v->uv_subnet;
		ph->pa_subnetmask  = v->uv_subnetmask;
		ph->pa_subnetbcast = v->uv_subnetbcast;

		ph->pa_next = uv->uv_addrs;
		uv->uv_addrs = ph;
		break;
	    }
	}

	if (vifi != numvifs) {
	  drop:
	    TAILQ_REMOVE(&vifs, v, uv_link);
	    free(v);
	    continue;
	}

	/*
	 * If there is room in the uvifs array, install this interface.
	 */
	if (install_uvif(v))
	    goto drop;

	logit(LOG_INFO, 0, "Installing %s (%s on subnet %s) as VIF #%u, rate %d pps",
	      v->uv_name, inet_fmt(v->uv_lcl_addr, s1, sizeof(s1)),
	      inet_fmts(v->uv_subnet, v->uv_subnetmask, s2, sizeof(s2)),
	      vifi, v->uv_rate_limit);
    }

    /*
     * XXX: one future extension may be to keep this for adding/removing
     *      dynamic interfaces at runtime.  Now we re-init and let SIGHUP
     *      rebuild it to recheck since we tear down all vifs anyway.
     */
    TAILQ_INIT(&vifs);
}

/*
 * Query the kernel to find network interfaces that are multicast-capable
 * and install them in the uvifs array.
 */
void config_vifs_from_kernel(void)
{
    in_addr_t addr, mask, subnet;
    struct ifaddrs *ifa, *ifap;
    struct uvif *uv;
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
	if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
	    continue;

	/*
	 * Ignore loopback interfaces and interfaces that do not support
	 * multicast.
	 */
	flags = ifa->ifa_flags;
	if ((flags & (IFF_LOOPBACK|IFF_MULTICAST)) != IFF_MULTICAST)
	    continue;

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

	uv = calloc(1, sizeof(struct uvif));
        if (!uv) {
            logit(LOG_ERR, errno, "failed allocating memory for iflist");
            return;
        }

	zero_vif(uv, 0);

	strlcpy(uv->uv_name, ifa->ifa_name, sizeof(uv->uv_name));
	uv->uv_lcl_addr    = addr;
	uv->uv_subnet      = subnet;
	uv->uv_subnetmask  = mask;
	uv->uv_subnetbcast = subnet | ~mask;

	if (ifa->ifa_flags & IFF_POINTOPOINT)
	    uv->uv_flags |= VIFF_REXMIT_PRUNES;

	/*
	 * On Linux we can enumerate vifs using ifindex,
	 * no need for an IP address.  Also used for the
	 * VIF lookup in find_vif()
	 */
	uv->uv_ifindex = if_nametoindex(uv->uv_name);
	if (!uv->uv_ifindex)
	    logit(LOG_ERR, errno, "Failed reading ifindex for %s", uv->uv_name);
	/*
	 * If the interface is not yet up, set the vifs_down flag to
	 * remind us to check again later.
	 */
	if (!(flags & IFF_UP)) {
	    uv->uv_flags |= VIFF_DOWN;
	    vifs_down = TRUE;
	}

	TAILQ_INSERT_TAIL(&vifs, uv, uv_link);
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
