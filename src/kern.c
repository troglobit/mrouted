/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"

int curttl = 0;

/*
 * Open/init the multicast routing in the kernel and sets the
 * MRT_PIM (aka MRT_ASSERT) flag in the kernel.
 */
void k_init_dvmrp(void)
{
    int v = 1;

    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_INIT, (char *)&v, sizeof(int)) < 0) {
	if (errno == EADDRINUSE)
	    logit(LOG_ERR, 0, "Another multicast routing application is already running.");
	else
	    logit(LOG_ERR, errno, "Cannot enable multicast routing in kernel");
    }
}


/*
 * Stops the multicast routing in the kernel and resets the
 * MRT_PIM (aka MRT_ASSERT) flag in the kernel.
 */
void k_stop_dvmrp(void)
{
    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_DONE, (char *)NULL, 0) < 0)
	logit(LOG_WARNING, errno, "Cannot disable multicast routing in kernel");
}


/*
 * Set the socket receiving buffer. `bufsize` is the preferred size,
 * `minsize` is the smallest acceptable size.
 */
void k_set_rcvbuf(int bufsize, int minsize)
{
    int delta = bufsize / 2;
    int iter = 0;

    /*
     * Set the socket buffer.  If we can't set it as large as we want, search around
     * to try to find the highest acceptable value.  The highest acceptable value
     * being smaller than minsize is a fatal error.
     */
    if (setsockopt(igmp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&bufsize, sizeof(bufsize)) < 0) {
        bufsize -= delta;
        while (1) {
            iter++;
            if (delta > 1)
                delta /= 2;

            if (setsockopt(igmp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&bufsize, sizeof(bufsize)) < 0) {
                bufsize -= delta;
            } else {
                if (delta < 1024)
                    break;
                bufsize += delta;
            }
        }
        if (bufsize < minsize) {
            logit(LOG_ERR, 0, "OS-allowed recv buffer size %u < app min %u", bufsize, minsize);
            /*NOTREACHED*/
        }
    }
    IF_DEBUG(DEBUG_KERN) {
        logit(LOG_DEBUG, 0, "Got %d byte recv buffer size in %d iterations",
              bufsize, iter);
    }
}


/*
 * Set/reset the IP_HDRINCL option. My guess is we don't need it for raw
 * sockets, but having it here won't hurt. Well, unless you are running
 * an older version of FreeBSD (older than 2.2.2). If the multicast
 * raw packet is bigger than 208 bytes, then IP_HDRINCL triggers a bug
 * in the kernel and "panic". The kernel patch for netinet/ip_raw.c
 * coming with this distribution fixes it.
 */
void k_hdr_include(int bool)
{
#ifdef IP_HDRINCL
    if (setsockopt(igmp_socket, IPPROTO_IP, IP_HDRINCL, (char *)&bool, sizeof(bool)) < 0)
        logit(LOG_ERR, errno, "Failed setting socket IP_HDRINCL %u", bool);
#endif
}


/*
 * Set the default TTL for the multicast packets outgoing from this socket.
 */
void k_set_ttl(int t)
{
    uint8_t ttl;

    ttl = t;
    if (setsockopt(igmp_socket, IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl, sizeof(ttl)) < 0)
	logit(LOG_ERR, errno, "Failed setting IP_MULTICAST_TTL %u", ttl);

    curttl = t;
}


/*
 * Set/reset the IP_MULTICAST_LOOP. Set/reset is specified by "flag".
 */
void k_set_loop(int flag)
{
    uint8_t loop;

    loop = flag;
    if (setsockopt(igmp_socket, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&loop, sizeof(loop)) < 0)
        logit(LOG_ERR, errno, "Failed setting socket IP_MULTICAST_LOOP to %u", loop);
}


/*
 * Set the IP_MULTICAST_IF option on local interface ifa.
 */
void k_set_if(uint32_t ifa)
{
    struct in_addr adr;

    adr.s_addr = ifa;
    if (setsockopt(igmp_socket, IPPROTO_IP, IP_MULTICAST_IF, (char *)&adr, sizeof(adr)) < 0) {
        if (errno == EADDRNOTAVAIL || errno == EINVAL)
            return;
        logit(LOG_ERR, errno, "Failed setting IP_MULTICAST_IF to %s",
              inet_fmt(ifa, s1, sizeof(s1)));
    }
}


/*
 * Join a multicast group.
 */
void k_join(uint32_t grp, uint32_t ifa)
{
    struct ip_mreq mreq;

    mreq.imr_multiaddr.s_addr = grp;
    mreq.imr_interface.s_addr = ifa;

    if (setsockopt(igmp_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) < 0)
	logit(LOG_WARNING, errno, "Cannot join group %s on interface %s",
	      inet_fmt(grp, s1, sizeof(s1)), inet_fmt(ifa, s2, sizeof(s2)));
}


/*
 * Leave a multicast group.
 */
void k_leave(uint32_t grp, uint32_t ifa)
{
    struct ip_mreq mreq;

    mreq.imr_multiaddr.s_addr = grp;
    mreq.imr_interface.s_addr = ifa;

    if (setsockopt(igmp_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) < 0)
	logit(LOG_WARNING, errno, "Cannot leave group %s on interface %s",
	      inet_fmt(grp, s1, sizeof(s1)), inet_fmt(ifa, s2, sizeof(s2)));
}

/*
 * Fill struct vifctl using corresponding fields from struct uvif.
 */
static void uvif_to_vifctl(struct vifctl *vc, struct uvif *v)
{
    vc->vifc_flags           = v->uv_flags & VIFF_KERNEL_FLAGS;
    vc->vifc_threshold       = v->uv_threshold;
    vc->vifc_rate_limit	     = v->uv_rate_limit;
    vc->vifc_lcl_addr.s_addr = v->uv_lcl_addr;
    vc->vifc_rmt_addr.s_addr = v->uv_rmt_addr;
}

/*
 * Add a virtual interface in the kernel.
 */
void k_add_vif(vifi_t vifi, struct uvif *v)
{
    struct vifctl vc;

    vc.vifc_vifi = vifi;
    uvif_to_vifctl(&vc, v);
    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_ADD_VIF, (char *)&vc, sizeof(vc)) < 0)
	logit(LOG_ERR, errno, "Failed MRT_ADD_VIF(%d) for %s", vifi, v->uv_name);
}


/*
 * Delete a virtual interface in the kernel.
 */
void k_del_vif(vifi_t vifi, struct uvif *v)
{
    /*
     * Unfortunately Linux setsocopt MRT_DEL_VIF API differs a bit from the *BSD one.
     * It expects to receive a pointer to struct vifctl that corresponds to the VIF
     * we're going to delete.  *BSD systems on the other hand exepect only the index
     * of that VIF.
     */
#ifdef __linux__
    struct vifctl vc;

    vc.vifc_vifi = vifi;
    uvif_to_vifctl(&vc, v);

    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_DEL_VIF, (char *)&vc, sizeof(vc)) < 0)
#else /* *BSD et al. */
    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_DEL_VIF, (char *)&vifi, sizeof(vifi)) < 0)
#endif /* !__linux__ */
    {
        if (errno == EADDRNOTAVAIL || errno == EINVAL)
            return;

	logit(LOG_ERR, errno, "Failed MRT_DEL_VIF(%d), cannot delete VIF for %s", vifi, v->uv_name);
    }
}


/*
 * Adds a (source, mcastgrp) entry to the kernel
 */
void k_add_rg(uint32_t origin, struct gtable *g)
{
    struct mfcctl mc;
    vifi_t i;

    memset(&mc, 0, sizeof(mc));
    mc.mfcc_origin.s_addr = origin;
    mc.mfcc_mcastgrp.s_addr = g->gt_mcastgrp;
    mc.mfcc_parent = g->gt_route ? g->gt_route->rt_parent : NO_VIF;
    for (i = 0; i < numvifs; i++)
	mc.mfcc_ttls[i] = g->gt_ttls[i];

    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_ADD_MFC, (char *)&mc, sizeof(mc)) < 0) {
	char ttls[3 * MAXVIFS + 1] = { 0 };

	for (i = 0; i < numvifs; i++) {
	    char buf[10];

	    snprintf(buf, sizeof(buf), "%d%s", mc.mfcc_ttls[i], i + 1 < numvifs ? ", " : "");
	    strlcat(ttls, buf, sizeof(ttls));
	}

	logit(LOG_WARNING, errno, "Failed MRT_ADD_MFC(%s, %s) from vif %d to vif(s) %s",
	      inet_fmt(origin, s1, sizeof(s1)), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)),
	      mc.mfcc_parent, ttls);
    }
}


/*
 * Deletes a (source, mcastgrp) entry from the kernel
 */
int k_del_rg(uint32_t origin, struct gtable *g)
{
    struct mfcctl mc;

    memset(&mc, 0, sizeof(mc));
    mc.mfcc_origin.s_addr = origin;
    mc.mfcc_mcastgrp.s_addr = g->gt_mcastgrp;

    /* write to kernel space */
    if (setsockopt(igmp_socket, IPPROTO_IP, MRT_DEL_MFC, (char *)&mc, sizeof(mc)) < 0) {
	logit(LOG_WARNING, errno, "Failed MRT_DEL_MFC(%s %s)",
	      inet_fmt(origin, s1, sizeof(s1)), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));

	return -1;
    }

    return 0;
}	

/*
 * Get the kernel's idea of what version of mrouted needs to run with it.
 */
int k_get_version(void)
{
    int vers;
    socklen_t len = sizeof(vers);

    if (getsockopt(igmp_socket, IPPROTO_IP, MRT_VERSION, (char *)&vers, &len) < 0)
	logit(LOG_ERR, errno, "Failed MRT_VERSION(): Cannot read version of multicast routing stack");

    return vers;
}

#if 0
/*
 * Get packet counters
 */
int k_get_vif_count(vifi_t vifi, int *icount, int *ocount, int *ibytes, int *obytes)
{
    struct sioc_vif_req vreq;
    int retval = 0;

    vreq.vifi = vifi;
    if (ioctl(udp_socket, SIOCGETVIFCNT, (char *)&vreq) < 0) {
	logit(LOG_WARNING, errno, "SIOCGETVIFCNT on vif %d", vifi);
	vreq.icount = vreq.ocount = vreq.ibytes =
		vreq.obytes = 0xffffffff;
	retval = 1;
    }
    if (icount)
	*icount = vreq.icount;
    if (ocount)
	*ocount = vreq.ocount;
    if (ibytes)
	*ibytes = vreq.ibytes;
    if (obytes)
	*obytes = vreq.obytes;
    return retval;
}

/*
 * Get counters for a desired source and group.
 */
int k_get_sg_count(uint32_t src, uint32_t grp, int *pktcnt, int *bytecnt, int *wrong_if)
{
    struct sioc_sg_req sgreq;
    int retval = 0;

    sgreq.src.s_addr = src;
    sgreq.grp.s_addr = grp;
    if (ioctl(udp_socket, SIOCGETSGCNT, (char *)&sgreq) < 0) {
	logit(LOG_WARNING, errno, "SIOCGETSGCNT on (%s %s)",
	    inet_fmt(src, s1, sizeof(s1)), inet_fmt(grp, s2, sizeof(s2)));
	sgreq.pktcnt = sgreq.bytecnt = sgreq.wrong_if = 0xffffffff;
	return 1;
    }
    if (pktcnt)
    	*pktcnt = sgreq.pktcnt;
    if (bytecnt)
    	*bytecnt = sgreq.bytecnt;
    if (wrong_if)
    	*wrong_if = sgreq.wrong_if;

    return retval;
}
#endif

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
