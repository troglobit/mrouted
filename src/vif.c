/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"

/*
 * Exported variables.
 */
vifi_t	     numvifs;		/* number of vifs in use		    */
int	     vifs_down;		/* 1=>some interfaces are down		    */
int	     phys_vif;		/* An enabled vif with valid uv_lcl_addr    */
int	     udp_socket;	/* Some kernels don't support ioctls on raw */
				/* IP sockets, so we need a UDP socket too  */
int	     neighbor_vifs;	/* == 1 if I am a leaf		    	    */

/*
 * Private variables.
 */
static struct listaddr	*nbrs[MAXNBRS];	 /* array of neighbors		    */
static struct uvif      *uvifs[MAXVIFS]; /* user-level virtual interfaces   */

typedef struct {
    struct listaddr *g;
    vifi_t vifi;
    int    delay;
    int    num;
} cbk_t;

static int query_timerid = -1;
static int dvmrp_timerid = -1;

/*
 * Forward declarations.
 */
void start_vif          (vifi_t vifi);
static void start_vif2  (vifi_t vifi);
void stop_vif           (vifi_t vifi);

static void send_probe_on_vif  (struct uvif *v);

static void send_query         (struct uvif *v, uint32_t dst, int code, uint32_t group);
static int  info_version       (uint8_t *p, size_t plen);

static void delete_group_cb    (void *arg);
static int  delete_group_timer (vifi_t vifi, struct listaddr *g);

static void send_query_cb      (void *arg);
static int  send_query_timer   (vifi_t vifi, struct listaddr *g, int delay, int num);

static void group_version_cb   (void *arg);
static int  group_version_timer(vifi_t vifi, struct listaddr *g);

/*
 * Initialize the virtual interfaces, but do not install
 * them in the kernel.  Start routing on all vifs that are
 * not down or disabled.
 */
void init_vifs(void)
{
    int enabled_vifs, enabled_phyints;
    struct uvif *uv;
    vifi_t vifi;

    numvifs = 0;
    neighbor_vifs = 0;
    vifs_down = FALSE;

    /*
     * Configure the vifs based on the interface configuration of the
     * the kernel and the contents of the configuration file.
     * (Open a UDP socket for ioctl use in the config procedures if
     * the kernel can't handle IOCTL's on the IGMP socket.)
     */
#ifdef IOCTL_OK_ON_RAW_SOCKET
    udp_socket = igmp_socket;
#else
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0)
	logit(LOG_ERR, errno, "UDP socket");
#endif
    logit(LOG_INFO, 0, "Getting vifs from kernel interfaces");
    config_vifs_from_kernel();

    logit(LOG_INFO, 0, "Getting vifs from %s", config_file);
    config_vifs_from_file();

    logit(LOG_INFO, 0, "Correlating interfaces and configuration ...");
    config_vifs_correlate();

    /*
     * Quit if there are fewer than two enabled vifs.
     */
    enabled_vifs    = 0;
    enabled_phyints = 0;
    phys_vif	    = -1;
    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & VIFF_DISABLED) {
	    logit(LOG_INFO, 0, "%s is disabled; skipping", uv->uv_name);
	    continue;
	}

	++enabled_vifs;

	if (uv->uv_flags & VIFF_TUNNEL)
	    continue;

	if (phys_vif == -1)
	    phys_vif = vifi;

	++enabled_phyints;
    }

    if (enabled_vifs < 2)
	logit(LOG_ERR, 0, "Cannot forward: %s",
	      enabled_vifs == 0 ? "no enabled vifs" : "only one enabled vif");

    if (enabled_phyints == 0)
	logit(LOG_WARNING, 0, "No enabled interfaces, forwarding via tunnels only");

    logit(LOG_INFO, 0, "Installing vifs in mrouted ...");
    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & VIFF_DISABLED)
	    continue;

	if (uv->uv_flags & VIFF_DOWN) {
	    logit(LOG_INFO, 0, "%s is not yet up; vif #%u not in service",
		  uv->uv_name, vifi);
	    continue;
	}

	if (uv->uv_flags & VIFF_TUNNEL)
	    logit(LOG_INFO, 0, "vif #%d, tunnel %s -> %s", vifi,
		  inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)),
		  inet_fmt(uv->uv_rmt_addr, s2, sizeof(s2)));
	else
	    logit(LOG_INFO, 0, "vif #%d, phyint %s", vifi,
		  inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)));

	start_vif2(vifi);
    }

    /*
     * Periodically query for local group memberships on all subnets for
     * which this router is the elected querier.
     */
    if (query_timerid > 0)
	timer_clear(query_timerid);
    query_timerid = timer_set(igmp_query_interval, query_groups, NULL);

    /*
     * Periodically probe all VIFs for DVMRP neighbors
     */
    if (dvmrp_timerid > 0)
	timer_clear(dvmrp_timerid);
    dvmrp_timerid = timer_set(NEIGHBOR_PROBE_INTERVAL, query_dvmrp, NULL);
}

/*
 * Reload the virtual interfaces, removing gone vifs and add new vifs.
 */
void reload_vifs(void)
{
    config_vifs_from_reload();
}

/*
 * Initialize the passed vif with all appropriate default values.
 * "t" is true if a tunnel, or false if a phyint.
 *
 * Note: remember to re-init all relevant TAILQ's in init_vifs()!
 */
void zero_vif(struct uvif *uv, int t)
{
    uv->uv_flags	= 0;
    uv->uv_metric	= DEFAULT_METRIC;
    uv->uv_admetric	= 0;
    uv->uv_threshold	= DEFAULT_THRESHOLD;
    uv->uv_rate_limit	= t ? DEFAULT_TUN_RATE_LIMIT : DEFAULT_PHY_RATE_LIMIT;
    uv->uv_lcl_addr	= 0;
    uv->uv_rmt_addr	= 0;
    uv->uv_dst_addr	= t ? 0 : dvmrp_group;
    uv->uv_subnet	= 0;
    uv->uv_subnetmask	= 0;
    uv->uv_subnetbcast	= 0;
    uv->uv_name[0]	= '\0';
    TAILQ_INIT(&uv->uv_static);
    TAILQ_INIT(&uv->uv_join);
    TAILQ_INIT(&uv->uv_groups);
    TAILQ_INIT(&uv->uv_neighbors);
    NBRM_CLRALL(uv->uv_nbrmap);
    uv->uv_querier	= NULL;
    uv->uv_igmpv1_warn	= 0;
    uv->uv_prune_lifetime = 0;
    uv->uv_leaf_timer	= 0;
    uv->uv_acl		= NULL;
    uv->uv_addrs	= NULL;
    uv->uv_filter	= NULL;
    uv->uv_blasterbuf	= NULL;
    uv->uv_blastercur	= NULL;
    uv->uv_blasterend	= NULL;
    uv->uv_blasterlen	= 0;
    uv->uv_blastertimer	= 0;
    uv->uv_nbrup	= 0;
    uv->uv_icmp_warn	= 0;
    uv->uv_nroutes	= 0;
}

void blaster_alloc(struct uvif *uv)
{
    if (!uv)
	return;

    blaster_free(uv);

    uv->uv_blasterlen = 64 * 1024;
    uv->uv_blasterbuf = calloc(1, uv->uv_blasterlen);
    if (!uv->uv_blasterbuf) {
	logit(LOG_ERR, errno, "%s(): Failed allocating memory", __func__);
	return;
    }

    uv->uv_blastercur = uv->uv_blasterend = uv->uv_blasterbuf;
}

void blaster_free(struct uvif *uv)
{
    if (uv->uv_blasterbuf)
	free(uv->uv_blasterbuf);
    uv->uv_blasterbuf = NULL;

    if (uv->uv_blastertimer)
	uv->uv_blastertimer = timer_clear(uv->uv_blastertimer);
}

/*
 * Start routing on all virtual interfaces that are not down or
 * administratively disabled.
 */
void init_installvifs(void)
{
    struct listaddr *al, *tmp;
    struct uvif *uv;
    vifi_t vifi;

    logit(LOG_INFO, 0, "Installing vifs in kernel ...");
    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & VIFF_DISABLED) {
	    logit(LOG_DEBUG, 0, "%s is disabled", uv->uv_name);
	    continue;
	}

	if (uv->uv_flags & VIFF_DOWN) {
	    logit(LOG_INFO, 0, "%s is not yet up; vif #%u not in service",
		  uv->uv_name, vifi);
	    continue;
	}

	if (uv->uv_flags & VIFF_TUNNEL) {
	    logit(LOG_INFO, 0, "vif #%d, tunnel %s -> %s", vifi,
		  inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)),
		  inet_fmt(uv->uv_rmt_addr, s2, sizeof(s2)));

	    /* Set tunnel vif name, Linux use dvmrpN */
	    snprintf(uv->uv_name, sizeof(uv->uv_name), "dvmrp%d", vifi);
	} else {
	    logit(LOG_INFO, 0, "vif #%d, phyint %s", vifi,
		  inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)));
	}
	k_add_vif(vifi, uv);

	/* Install static routes/groups from mrouted.conf */
	TAILQ_FOREACH_SAFE(al, &uv->uv_static, al_link, tmp) {
	    in_addr_t group = al->al_addr;

	    TAILQ_REMOVE(&uv->uv_static, al, al_link);
	    TAILQ_INSERT_TAIL(&uv->uv_groups, al, al_link);

	    logit(LOG_INFO, 0, "    static group %s", inet_fmt(group, s3, sizeof(s3)));
	    update_lclgrp(vifi, group);
	    chkgrp_graft(vifi, group);
	}
    }
}

int install_uvif(struct uvif *uv)
{
    if (numvifs == MAXVIFS) {
	logit(LOG_WARNING, 0, "Too many vifs, ignoring %s", uv->uv_name);
	return 1;
    }

    uvifs[numvifs++] = uv;

    return 0;
}

int uninstall_uvif(struct uvif *uv)
{
    if (numvifs == 0) {
        logit(LOG_WARNING, 0, "No vifs, ignoring %s", uv->uv_name);
        return 1;
    }

    int i;
    for (i = 0; i < numvifs; i++) {
        if (uv->uv_ifindex == uvifs[i]->uv_ifindex) {
            break;
        }
    }

    if (i == numvifs)
        return 1;

    for (; i < numvifs - 1; i++) {
        uvifs[i] = uvifs[i + 1];
    }
    numvifs--;

    return 0;
}

/*
 * See if any interfaces have changed from up state to down, or vice versa,
 * including any non-multicast-capable interfaces that are in use as local
 * tunnel end-points.  Ignore interfaces that have been administratively
 * disabled.
 */
void check_vif_state(void)
{
    static int checking_vifs = 0;
    struct ifreq ifr;
    struct uvif *uv;
    vifi_t vifi;

    /*
     * If we get an error while checking, (e.g. two interfaces go down
     * at once, and we decide to send a prune out one of the failed ones)
     * then don't go into an infinite loop!
     */
    if (checking_vifs)
	return;

    vifs_down = FALSE;
    checking_vifs = 1;
    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & VIFF_DISABLED)
	    continue;

	memset(&ifr, 0, sizeof(ifr));
	memcpy(ifr.ifr_name, uv->uv_name, sizeof(ifr.ifr_name));
	if (ioctl(udp_socket, SIOCGIFFLAGS, &ifr) < 0)
	    logit(LOG_ERR, errno, "Failed ioctl SIOCGIFFLAGS for %s", ifr.ifr_name);

	if (uv->uv_flags & VIFF_DOWN) {
	    if (ifr.ifr_flags & IFF_UP) {
		logit(LOG_NOTICE, 0, "%s has come up; vif #%u now in service",
		      uv->uv_name, vifi);
		uv->uv_flags &= ~VIFF_DOWN;
		start_vif(vifi);
	    } else {
		vifs_down = TRUE;
	    }
	} else {
	    if (!(ifr.ifr_flags & IFF_UP)) {
		logit(LOG_NOTICE, 0, "%s has gone down; vif #%u taken out of service",
		    uv->uv_name, vifi);
		stop_vif(vifi);
		uv->uv_flags |= VIFF_DOWN;
		vifs_down = TRUE;
	    }
	}
    }

    checking_vifs = 0;
}

/*
 * Send a DVMRP message on the specified vif.  If DVMRP messages are
 * to be encapsulated and sent "inside" the tunnel, use the special
 * encapsulator.  If it's not a tunnel or DVMRP messages are to be
 * sent "beside" the tunnel, as required by earlier versions of mrouted,
 * then just send the message.
 */
void send_on_vif(struct uvif *v, uint32_t dst, int code, size_t datalen)
{
    uint32_t group = htonl(MROUTED_LEVEL | ((v->uv_flags & VIFF_LEAF) ? 0 : LEAF_FLAGS));

    /*
     * The UNIX kernel will not decapsulate unicasts.
     * Therefore, we don't send encapsulated unicasts.
     */
    if ((v->uv_flags & (VIFF_TUNNEL|VIFF_OTUNNEL)) == VIFF_TUNNEL &&
	((dst == 0) || IN_MULTICAST(ntohl(dst))))
	send_ipip(v->uv_lcl_addr, dst ? dst : dvmrp_group, IGMP_DVMRP,
						code, group, datalen, v);
    else
	send_igmp(v->uv_lcl_addr, dst ? dst : v->uv_dst_addr, IGMP_DVMRP,
						code, group, datalen);
}


/*
 * Send a probe message on vif v
 */
static void send_probe_on_vif(struct uvif *v)
{
    struct listaddr *nbr;
    uint8_t *p;
    size_t datalen = 0;
    int i;

    if ((v->uv_flags & VIFF_PASSIVE && TAILQ_EMPTY(&v->uv_neighbors)) ||
	(v->uv_flags & VIFF_FORCE_LEAF))
	return;

    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;

    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(dvmrp_genid))[i];
    datalen += 4;

    /*
     * add the neighbor list on the interface to the message
     */
    TAILQ_FOREACH(nbr, &v->uv_neighbors, al_link) {
	for (i = 0; i < 4; i++)
	    *p++ = ((char *)&nbr->al_addr)[i];
	datalen +=4;
    }

    send_on_vif(v, 0, DVMRP_PROBE, datalen);
}

static void send_query(struct uvif *v, uint32_t dst, int code, uint32_t group)
{
    int datalen = 4;

    /*
     * IGMP version to send depends on the compatibility mode of the
     * interface:
     *  - IGMPv2: routers MUST send Periodic Queries truncated at the
     *    Group Address field (i.e., 8 bytes long).
     *  - IGMPv1: routers MUST send Periodic Queries with a Max Response
     *    Time of 0
     */
    if (v->uv_flags & VIFF_IGMPV2) {
	datalen = 0;
    } else if (v->uv_flags & VIFF_IGMPV1) {
	datalen = 0;
	code = 0;
    }

    IF_DEBUG(DEBUG_IGMP) {
	logit(LOG_DEBUG, 0, "Sending %squery on %s",
	      (v->uv_flags & VIFF_IGMPV1) ? "v1 " :
	      (v->uv_flags & VIFF_IGMPV2) ? "v2 " : "v3 ",
	      v->uv_name);
    }

    send_igmp(v->uv_lcl_addr, dst, IGMP_MEMBERSHIP_QUERY,
	      code, group, datalen);
}

/*
 * Add a vifi to the kernel and start routing on it.
 */
void start_vif(vifi_t vifi)
{
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (!uv)
	return;

    /*
     * Install the interface in the kernel's vif structure.
     */
    k_add_vif(vifi, uv);
    start_vif2(vifi);
}

/*
 * Add a vifi to all the user-level data structures but don't add
 * it to the kernel yet.
 */
static void start_vif2(vifi_t vifi)
{
    struct listaddr *a;
    struct phaddr *p;
    struct uvif *uv;
    uint32_t src;

    uv = find_uvif(vifi);
    if (!uv)
	return;
    src = uv->uv_lcl_addr;

    /*
     * Update the existing route entries to take into account the new vif.
     */
    add_vif_to_routes(vifi);

    if (!(uv->uv_flags & VIFF_TUNNEL)) {
	/*
	 * Join the DVMRP multicast group on the interface.
	 * (This is not strictly necessary, since the kernel promiscuously
	 * receives IGMP packets addressed to ANY IP multicast group while
	 * multicast routing is enabled.  However, joining the group allows
	 * this host to receive non-IGMP packets as well, such as 'pings'.)
	 */
	k_join(dvmrp_group, src);

	/*
	 * Join the ALL-ROUTERS multicast group on the interface.
	 * This allows mtrace requests to loop back if they are run
	 * on the multicast router.
	 */
	k_join(allrtrs_group, src);

	/* Join INADDR_ALLRPTS_GROUP to support IGMPv3 membership reports */
	k_join(allreports_group, src);

	/*
	 * Some switches with IGMP snooping enabled do not properly
         * recognize mrouted as a dynamic multicast router port, so they
         * will block traffic from the sender to the mrouted router.  In
         * such a case, we might want to explicitely join a multicast
         * group on this interface.
	 */
	TAILQ_FOREACH(a, &uv->uv_join, al_link) {
	    uint32_t group = a->al_addr;

	    logit(LOG_INFO, 0, "Joining static group %s on %s from %s",
		  inet_fmt(group, s1, sizeof(s1)), uv->uv_name, config_file);
	    k_join(group, src);
	}

	/*
	 * Install an entry in the routing table for the subnet to which
	 * the interface is connected.
	 */
	start_route_updates();
	update_route(uv->uv_subnet, uv->uv_subnetmask, 0, 0, vifi, NULL);
	for (p = uv->uv_addrs; p; p = p->pa_next) {
	    start_route_updates();
	    update_route(p->pa_subnet, p->pa_subnetmask, 0, 0, vifi, NULL);
	}

	/*
	 * Until neighbors are discovered, assume responsibility for sending
	 * periodic group membership queries to the subnet.  Send the first
	 * query.
	 */
	uv->uv_flags |= VIFF_QUERIER;
	logit(LOG_INFO, 0, "Assuming querier duties on %s", uv->uv_name);
	send_query(uv, allhosts_group, igmp_response_interval * IGMP_TIMER_SCALE, 0);
    }

    uv->uv_leaf_timer = LEAF_CONFIRMATION_TIME;

    /*
     * Send a probe via the new vif to look for neighbors.
     */
    send_probe_on_vif(uv);
}

/*
 * Stop routing on the specified virtual interface.
 */
void stop_vif(vifi_t vifi)
{
    struct listaddr *a, *tmp;
    struct phaddr *p;
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (!uv)
	return;

    if (!(uv->uv_flags & VIFF_TUNNEL)) {
	/*
	 * Depart from the DVMRP multicast group on the interface.
	 */
	k_leave(dvmrp_group, uv->uv_lcl_addr);

	/*
	 * Depart from the ALL-ROUTERS multicast group on the interface.
	 */
	k_leave(allrtrs_group, uv->uv_lcl_addr);

	/*
	 * Depart from the ALL-REPORTS multicast group on the interface.
	 */
	k_leave(allreports_group, uv->uv_lcl_addr);

	/*
	 * Update the entry in the routing table for the subnet to which
	 * the interface is connected, to take into account the interface
	 * failure.
	 */
	start_route_updates();
	update_route(uv->uv_subnet, uv->uv_subnetmask, UNREACHABLE, 0, vifi, NULL);
	for (p = uv->uv_addrs; p; p = p->pa_next) {
	    start_route_updates();
	    update_route(p->pa_subnet, p->pa_subnetmask, UNREACHABLE, 0, vifi, NULL);
	}

	/*
	 * Discard all group addresses.  (No need to tell kernel;
	 * the k_del_vif() call, below, will clean up kernel state.)
	 */
	TAILQ_FOREACH_SAFE(a, &uv->uv_groups, al_link, tmp) {
	    TAILQ_REMOVE(&uv->uv_groups, a, al_link);
	    free(a);
	}

	IF_DEBUG(DEBUG_IGMP) {
	    logit(LOG_DEBUG, 0, "Releasing querier duties on vif %u", vifi);
	}
	uv->uv_flags &= ~VIFF_QUERIER;
    }

    /*
     * Update the existing route entries to take into account the vif failure.
     */
    delete_vif_from_routes(vifi);

    /*
     * Delete the interface from the kernel's vif structure.
     */
    k_del_vif(vifi, uv);

    /*
     * Discard all neighbor addresses.
     */
    if (!NBRM_ISEMPTY(uv->uv_nbrmap))
	neighbor_vifs--;

    TAILQ_FOREACH_SAFE(a, &uv->uv_neighbors, al_link, tmp) {
	TAILQ_REMOVE(&uv->uv_neighbors, a, al_link);
	nbrs[a->al_index] = NULL;
	free(a);
    }
    NBRM_CLRALL(uv->uv_nbrmap);
}


/*
 * stop routing on all vifs
 */
void stop_all_vifs(void)
{
    struct listaddr *a, *tmp;
    struct vif_acl *acl;
    struct phaddr *ph;
    struct uvif *uv;
    vifi_t vifi;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_querier) {
	    free(uv->uv_querier);
	    uv->uv_querier = NULL;
	}
	uv->uv_querier = NULL;

	TAILQ_FOREACH_SAFE(a, &uv->uv_join, al_link, tmp) {
	    uint32_t group = a->al_addr;

	    logit(LOG_INFO, 0, "Leaving static group %s on %s from %s",
		  inet_fmt(group, s1, sizeof(s1)), uv->uv_name, config_file);
	    k_leave(group, uv->uv_lcl_addr);
	    TAILQ_REMOVE(&uv->uv_join, a, al_link);
	    free(a);
	}

	TAILQ_FOREACH_SAFE(a, &uv->uv_groups, al_link, tmp) {
	    TAILQ_REMOVE(&uv->uv_groups, a, al_link);
	    free(a);
	}

	TAILQ_FOREACH_SAFE(a, &uv->uv_neighbors, al_link, tmp) {
	    TAILQ_REMOVE(&uv->uv_neighbors, a, al_link);
	    nbrs[a->al_index] = NULL;
	    free(a);
	}

	while (uv->uv_acl) {
	    acl = uv->uv_acl;
	    uv->uv_acl = acl->acl_next;
	    free(acl);
	}
	uv->uv_acl = NULL;

	while (uv->uv_addrs) {
	    ph = uv->uv_addrs;
	    uv->uv_addrs = ph->pa_next;
	    free(ph);
	}
	uv->uv_addrs = NULL;

	blaster_free(uv);
	free(uv);
    }
}

/*
 * Find user-level vif from VIF index
 */
struct uvif *find_uvif(vifi_t vifi)
{
    if (vifi >= numvifs || vifi == NO_VIF || !uvifs[vifi])
	return NULL;

    return uvifs[vifi];
}

/*
 * Find VIF from ifindex
 */
vifi_t find_vif(int ifi)
{
    struct uvif *uv;
    vifi_t vifi;

    if (ifi < 0)
	return NO_VIF;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_ifindex == ifi)
	    return vifi;
    }

    return NO_VIF;
}


/*
 * Find the virtual interface from which an incoming packet arrived,
 * based on the packet's source and destination IP addresses.
 */
vifi_t find_vif_direct(uint32_t src, uint32_t dst)
{
    struct phaddr *p;
    struct uvif *uv;
    vifi_t vifi;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & (VIFF_DOWN|VIFF_DISABLED))
	    continue;

	if (uv->uv_flags & VIFF_TUNNEL) {
	    if (src == uv->uv_rmt_addr && (dst == uv->uv_lcl_addr || dst == dvmrp_group))
		return vifi;

	    continue;
	}

	if ((src & uv->uv_subnetmask) == uv->uv_subnet &&
	    (uv->uv_subnetmask == 0xffffffff || src != uv->uv_subnetbcast))
	    return vifi;

	for (p = uv->uv_addrs; p; p = p->pa_next) {
	    if ((src & p->pa_subnetmask) == p->pa_subnet &&
		(p->pa_subnetmask == 0xffffffff || src != p->pa_subnetbcast))
		return vifi;
	}
    }

    return NO_VIF;
}

/*
 * Send group membership queries on each interface for which I am querier.
 * Note that technically, there should be a timer per interface, as the
 * dynamics of querier election can cause the "right" time to send a
 * query to be different on different interfaces.  However, this simple
 * implementation only ever sends queries sooner than the "right" time,
 * so can not cause loss of membership (but can send more packets than
 * necessary)
 */
void query_groups(void *arg)
{
    struct uvif *uv;
    vifi_t vifi;

    timer_set(igmp_query_interval, query_groups, arg);

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & (VIFF_DOWN | VIFF_DISABLED))
	    continue;

	if (uv->uv_flags & VIFF_QUERIER)
	    send_query(uv, allhosts_group, igmp_response_interval * IGMP_TIMER_SCALE, 0);
    }
}

/*
 * Time to send a probe on all vifs from which no neighbors have
 * been heard.  Also, check if any inoperative interfaces have now
 * come up.  (If they have, they will also be probed as part of
 * their initialization.)
 */
void query_dvmrp(void *arg)
{
    timer_set(NEIGHBOR_PROBE_INTERVAL, query_dvmrp, arg);

    probe_for_neighbors();

    if (vifs_down)
	check_vif_state();
}

/*
 * Process an incoming host membership query.  Warn about
 * IGMP version mismatches, perform querier election, and
 * handle group-specific queries when we're not the querier.
 */
void accept_membership_query(int ifi, uint32_t src, uint32_t dst, uint32_t group, int tmo, int ver)
{
    struct uvif *uv;
    vifi_t vifi;

    vifi = find_vif(ifi);
    if (vifi == NO_VIF)
	vifi = find_vif_direct(src, dst);

    uv = find_uvif(vifi);
    if (!uv || (uv->uv_flags & VIFF_TUNNEL)) {
	logit(LOG_INFO, 0, "Ignoring group membership query from non-adjacent host %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    if ((ver == 3 && (uv->uv_flags & VIFF_IGMPV2)) ||
	(ver == 2 && (uv->uv_flags & VIFF_IGMPV1))) {
	int i;

	/*
	 * Exponentially back-off warning rate
	 */
	i = ++uv->uv_igmpv1_warn;
	while (i && !(i & 1))
	    i >>= 1;

	if (i == 1) {
	    logit(LOG_WARNING, 0, "Received IGMPv%d report from %s on vif %u, "
		  "which is configured for IGMPv%d",
		  ver, inet_fmt(src, s1, sizeof(s1)), vifi,
		  uv->uv_flags & VIFF_IGMPV1 ? 1 : 2);
	}
    }

    if (uv->uv_querier == NULL || uv->uv_querier->al_addr != src) {
	uint32_t cur = uv->uv_querier ? uv->uv_querier->al_addr : uv->uv_lcl_addr;

	/*
	 * This might be:
	 * - A query from a new querier, with a lower source address
	 *   than the current querier (who might be me)
	 * - A query from a new router that just started up and doesn't
	 *   know who the querier is.
	 * - A proxy query (source address 0.0.0.0), never wins elections
	 */
	if (!ntohl(src)) {
	    logit(LOG_DEBUG, 0, "Ignoring proxy query on %s", uv->uv_name);
	    return;
	}

	if (ntohl(src) < ntohl(cur)) {
	    IF_DEBUG(DEBUG_IGMP) {
		logit(LOG_DEBUG, 0, "New querier %s (was %s) on vif %u", inet_fmt(src, s1, sizeof(s1)),
		      uv->uv_querier ? inet_fmt(uv->uv_querier->al_addr, s2, sizeof(s2)) : "me", vifi);
	    }

	    if (!uv->uv_querier) {
		uv->uv_querier = calloc(1, sizeof(struct listaddr));
		uv->uv_flags &= ~VIFF_QUERIER;
	    }

	    if (uv->uv_querier) {
		time(&uv->uv_querier->al_ctime);
		uv->uv_querier->al_addr = src;
	    }
	} else {
	    IF_DEBUG(DEBUG_IGMP) {
		logit(LOG_DEBUG, 0, "Ignoring query from %s; querier on vif %u is still %s",
		      inet_fmt(src, s1, sizeof(s1)), vifi,
		      uv->uv_querier ? inet_fmt(uv->uv_querier->al_addr, s2, sizeof(s2)) : "me");
	    }
	    return;
	}
    }

    /*
     * Reset the timer since we've received a query.
     */
    if (uv->uv_querier && src == uv->uv_querier->al_addr)
        uv->uv_querier->al_timer = 0;

    /*
     * If this is a Group-Specific query which we did not source,
     * we must set our membership timer to [Last Member Query Count] *
     * the [Max Response Time] in the packet.
     */
    if (!(uv->uv_flags & (VIFF_IGMPV1|VIFF_QUERIER))
	&& group != 0 && src != uv->uv_lcl_addr) {
	struct listaddr *g;

	IF_DEBUG(DEBUG_IGMP) {
	    logit(LOG_DEBUG, 0, "Group-specific membership query for %s from %s on vif %u, timer %d",
		  inet_fmt(group, s2, sizeof(s2)),
		  inet_fmt(src, s1, sizeof(s1)), vifi, tmo);
	}

	TAILQ_FOREACH(g, &uv->uv_groups, al_link) {
	    if (group == g->al_addr && g->al_query == 0) {
		if (g->al_timerid > 0)
		    g->al_timerid = timer_clear(g->al_timerid);

		if (g->al_query > 0)
		    g->al_query = timer_clear(g->al_query);

		/* setup a timeout to remove the group membership */
		g->al_timer = IGMP_LAST_MEMBER_QUERY_COUNT * tmo / IGMP_TIMER_SCALE;
		g->al_timerid = delete_group_timer(vifi, g);

		IF_DEBUG(DEBUG_IGMP) {
		    logit(LOG_DEBUG, 0, "Timer for grp %s on vif %u set to %d",
			  inet_fmt(group, s2, sizeof(s2)), vifi, g->al_timer);
		}
		break;
	    }
	}
    }
}

static void group_debug(struct listaddr *g, char *s, int is_change)
{
    IF_DEBUG(DEBUG_IGMP)
	logit(LOG_DEBUG, 0, "%sIGMP v%d compatibility mode for group %s",
	      is_change ? "Change to " : "", g->al_pv, s);
}

/*
 * Process an incoming group membership report.
 */
void accept_group_report(int ifi, uint32_t src, uint32_t dst, uint32_t group, int r_type)
{
    struct listaddr *g;
    struct uvif *uv;
    vifi_t vifi;

    inet_fmt(src, s1, sizeof(s1));
    inet_fmt(dst, s2, sizeof(s2));
    inet_fmt(group, s3, sizeof(s3));

    /* Do not filter LAN scoped groups */
    if (ntohl(group) <= INADDR_MAX_LOCAL_GROUP) { /* group <= 224.0.0.255? */
	IF_DEBUG(DEBUG_IGMP)
	    logit(LOG_DEBUG, 0, "    %-16s LAN scoped group, skipping.", s3);
	return;
    }

    vifi = find_vif(ifi);
    if (vifi == NO_VIF)
	vifi = find_vif_direct(src, dst);

    uv = find_uvif(vifi);
    if (!uv || (uv->uv_flags & VIFF_TUNNEL)) {
	logit(LOG_INFO, 0, "Ignoring group membership report from non-adjacent host %s", s1);
	return;
    }

    IF_DEBUG(DEBUG_IGMP)
	logit(LOG_INFO, 0, "Accepting group membership report: src %s, dst %s, grp %s", s1, s2, s3);

    /*
     * Look for the group in our group list; if found, reset its timer.
     */
    TAILQ_FOREACH(g, &uv->uv_groups, al_link) {
	int old_report = 0;

	if (group == g->al_addr) {
	    if (g->al_flags & NBRF_STATIC_GROUP) {
		IF_DEBUG(DEBUG_IGMP)
		    logit(LOG_DEBUG, 0, "Ignoring IGMP JOIN for static group %s on %s.", s3, s1);
		return;
	    }

	    switch (r_type) {
	    case IGMP_V1_MEMBERSHIP_REPORT:
		old_report = 1;
		if (g->al_pv > 1) {
		    g->al_pv = 1;
		    group_debug(g, s3, 1);
		}
		break;

	    case IGMP_V2_MEMBERSHIP_REPORT:
		old_report = 1;
		if (g->al_pv > 2) {
		    g->al_pv = 2;
		    group_debug(g, s3, 1);
		}
		break;

	    default:
		break;
	    }

	    g->al_reporter = src;

	    /** delete old timers, set a timer for expiration **/
	    g->al_timer = IGMP_GROUP_MEMBERSHIP_INTERVAL;

	    if (g->al_query > 0)
		g->al_query = timer_clear(g->al_query);

	    if (g->al_timerid > 0)
		g->al_timerid = timer_clear(g->al_timerid);

	    g->al_timerid = delete_group_timer(vifi, g);

	    /*
	     * Reset timer for switching version back every time an older
	     * version report is received
	     */
	    if (g->al_pv < 3 && old_report) {
		if (g->al_pv_timerid)
		    g->al_pv_timerid = timer_clear(g->al_pv_timerid);

		g->al_pv_timerid = group_version_timer(vifi, g);
	    }
	    break;
	}
    }

    /*
     * If not found, add it to the list and update kernel cache.
     */
    if (!g) {
	g = calloc(1, sizeof(struct listaddr));
	if (!g) {
	    logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	    return;
	}

	g->al_addr = group;

	switch (r_type) {
	case IGMP_V1_MEMBERSHIP_REPORT:
	    g->al_pv = 1;
	    break;

	case IGMP_V2_MEMBERSHIP_REPORT:
	    g->al_pv = 2;
	    break;

	default:
	    g->al_pv = 3;
	    break;
	}

	group_debug(g, s3, 0);

	/** set a timer for expiration **/
        g->al_query	= 0;
	g->al_timer	= IGMP_GROUP_MEMBERSHIP_INTERVAL;
	g->al_reporter	= src;
	g->al_timerid	= delete_group_timer(vifi, g);

	/*
	 * Set timer for swithing version back if an older version
	 * report is received
	 */
	if (g->al_pv < 3)
	    g->al_pv_timerid = group_version_timer(vifi, g);

	TAILQ_INSERT_TAIL(&uv->uv_groups, g, al_link);
	time(&g->al_ctime);

	update_lclgrp(vifi, group);
    }

    /* 
     * Check if a graft is necessary for this group
     */
    chkgrp_graft(vifi, group);
}

/*
 * Process an incoming IGMPv2 Leave Group message, an IGMPv3 BLOCK(), or
 * IGMPv3 TO_IN({}) membership report.  Handles older version hosts.
 *
 * We detect IGMPv3 by the dst always being 0.
 */
void accept_leave_message(int ifi, uint32_t src, uint32_t dst, uint32_t group)
{
    struct listaddr *g;
    struct uvif *uv;
    vifi_t vifi;

    inet_fmt(src, s1, sizeof(s1));
    inet_fmt(group, s3, sizeof(s3));

    vifi = find_vif(ifi);
    if (vifi == NO_VIF)
	vifi = find_vif_direct(src, dst);

    uv = find_uvif(vifi);
    if (!uv || (uv->uv_flags & VIFF_TUNNEL)) {
	logit(LOG_INFO, 0, "Ignoring group leave report from non-adjacent host %s", s1);
	return;
    }

    if (!(uv->uv_flags & VIFF_QUERIER) || (uv->uv_flags & VIFF_IGMPV1)) {
	IF_DEBUG(DEBUG_IGMP)
	    logit(LOG_DEBUG, 0, "Ignoring group leave, not querier or interface in IGMPv1 mode.");
	return;
    }

    /*
     * Look for the group in our group list in order to set up a short-timeout
     * query.
     */
    TAILQ_FOREACH(g, &uv->uv_groups, al_link) {
	if (group != g->al_addr)
	    continue;

	if (g->al_flags & NBRF_STATIC_GROUP) {
	    IF_DEBUG(DEBUG_IGMP)
		logit(LOG_DEBUG, 0, "Ignoring IGMP LEAVE for static group %s on %s.", s3, s1);
	    return;
	}

	/* Ignore IGMPv2 LEAVE in IGMPv1 mode, RFC3376, sec. 7.3.2. */
	if (g->al_pv == 1) {
	    IF_DEBUG(DEBUG_IGMP)
		logit(LOG_DEBUG, 0, "Ignoring IGMP LEAVE for %s on %s, IGMPv1 host exists.", s3, s1);
	    return;
	}

	/* Ignore IGMPv3 BLOCK in IGMPv2 mode, RFC3376, sec. 7.3.2. */
	if (g->al_pv == 2 && dst == 0) {
	    IF_DEBUG(DEBUG_IGMP)
		logit(LOG_DEBUG, 0, "Ignoring IGMP BLOCK/TO_IN({}) for %s on %s, IGMPv2 host exists.", s3, s1);
	    return;
	}

	/* still waiting for a reply to a query, ignore the leave */
	if (g->al_query) {
	    IF_DEBUG(DEBUG_IGMP)
		logit(LOG_DEBUG, 0, "Ignoring IGMP LEAVE for %s on %s, pending group-specific query.", s3, s1);
	    return;
	}

	/** delete old timer set a timer for expiration **/
	if (g->al_timerid > 0)
	    g->al_timerid = timer_clear(g->al_timerid);

	/** send a group specific query **/
	g->al_query = send_query_timer(vifi, g, igmp_last_member_interval,
				       IGMP_LAST_MEMBER_QUERY_COUNT);
	g->al_timer = igmp_last_member_interval * (IGMP_LAST_MEMBER_QUERY_COUNT + 1);
	g->al_timerid = delete_group_timer(vifi, g);

	IF_DEBUG(DEBUG_IGMP)
	    logit(LOG_DEBUG, 0, "Accepted group leave for %s on %s", s3, s1);

	return;
    }

    /*
     * We only get here when we couldn't find the group, or when there
     * still is a group-specific query pending, or when the group is in
     * older version compat, RFC3376.
     */
    IF_DEBUG(DEBUG_IGMP)
	logit(LOG_DEBUG, 0, "Ignoring IGMP LEAVE/BLOCK for %s on %s, group not found.", s3, s1);
}


/*
 * Loop through and process all sources in a v3 record.
 *
 * Parameters:
 *     r_type   Report type of IGMP message
 *     src      Src address of IGMP message
 *     dst      Multicast group
 *     sources  Pointer to the beginning of sources list in the IGMP message
 *     canary   Pointer to the end of IGMP message
 *
 * Returns:
 *     POSIX OK (0) if succeeded, non-zero on failure.
 */
int accept_sources(int ifi, int r_type, uint32_t src, uint32_t dst, uint8_t *sources,
    uint8_t *canary, int rec_num_sources)
{
    uint8_t *ptr;
    int j;

    for (j = 0, ptr = sources; j < rec_num_sources; ++j, src += 4) {
	struct in_addr *ina = (struct in_addr *)ptr;

        if ((ptr + 4) > canary) {
	    IF_DEBUG(DEBUG_IGMP)
		logit(LOG_DEBUG, 0, "Invalid IGMPv3 report, too many sources, would overflow.");
            return 1;
        }

	IF_DEBUG(DEBUG_IGMP)
	    logit(LOG_DEBUG, 0, "Add source (%s,%s)", inet_fmt(ina->s_addr, s2, sizeof(s2)),
		  inet_fmt(dst, s1, sizeof(s1)));

        accept_group_report(ifi, src, ina->s_addr, dst, r_type);
    }

    return 0;
}


/*
 * Handle IGMP v3 membership reports (join/leave)
 */
void accept_membership_report(int ifi, uint32_t src, uint32_t dst, struct igmpv3_report *report, ssize_t reportlen)
{
    uint8_t *canary = (uint8_t *)report + reportlen;
    struct igmpv3_grec *record;
    int num_groups, i;

    num_groups = ntohs(report->ngrec);
    if (num_groups < 0) {
	logit(LOG_INFO, 0, "Invalid Membership Report from %s: num_groups = %d",
	      inet_fmt(src, s1, sizeof(s1)), num_groups);
	return;
    }

    IF_DEBUG(DEBUG_IGMP) {
	logit(LOG_DEBUG, 0, "IGMP v3 report, %zd bytes, from %s to %s with %d group records.",
	      reportlen, inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)), num_groups);
    }

    record = &report->grec[0];

    for (i = 0; i < num_groups; i++) {
	struct in_addr  rec_group;
	uint8_t        *sources;
	int             rec_type;
	int             rec_auxdatalen;
	int             rec_num_sources;
	int             j, rc;
	char src_str[200];
	int record_size = 0;

	rec_num_sources = ntohs(record->grec_nsrcs);
	rec_auxdatalen = record->grec_auxwords;
	record_size = sizeof(struct igmpv3_grec) + sizeof(uint32_t) * rec_num_sources + rec_auxdatalen;
	if ((uint8_t *)record + record_size > canary) {
	    logit(LOG_INFO, 0, "Invalid group report %p > %p", (uint8_t *)record + record_size, canary);
	    return;
	}

	rec_type = record->grec_type;
	rec_group.s_addr = (in_addr_t)record->grec_mca;
	sources = (uint8_t *)record->grec_src;

	switch (rec_type) {
	    case IGMP_MODE_IS_EXCLUDE:
	    case IGMP_CHANGE_TO_EXCLUDE_MODE:
		if (rec_num_sources == 0) {
		    /* RFC 5790: TO_EX({}) can be interpreted as a (*,G)
		     *           join, i.e., to include all sources.
		     */
		    accept_group_report(ifi, src, 0, rec_group.s_addr, report->type);
		} else {
		    /* RFC 5790: LW-IGMPv3 does not use TO_EX({x}),
		     *           i.e., filter with non-null source.
		     */
		    IF_DEBUG(DEBUG_IGMP)
			logit(LOG_DEBUG, 0, "IS_EX/TO_EX({x}), not unsupported, RFC5790.");
		}
		break;

	    case IGMP_MODE_IS_INCLUDE:
	    case IGMP_CHANGE_TO_INCLUDE_MODE:
		if (rec_num_sources == 0) {
		    /* RFC5790: TO_IN({}) can be interpreted as an
		     *          IGMPv2 (*,G) leave.
		     */
		    accept_leave_message(ifi, src, 0, rec_group.s_addr);
		} else {
		    /* RFC5790: TO_IN({x}), regular RFC3376 (S,G)
		     *          join with >= 1 source, 'S'.
		     */
		    rc = accept_sources(ifi, report->type, src, rec_group.s_addr,
					sources, canary, rec_num_sources);
		    if (rc)
			return;
		}
		break;

	    case IGMP_ALLOW_NEW_SOURCES:
		/* RFC5790: Same as TO_IN({x}) */
		rc = accept_sources(ifi, report->type, src, rec_group.s_addr,
				    sources, canary, rec_num_sources);
		if (rc)
		    return;
		break;

	    case IGMP_BLOCK_OLD_SOURCES:
		/* RFC5790: Instead of TO_EX({x}) */
		for (j = 0; j < rec_num_sources; j++) {
		    uint8_t *gsrc = (uint8_t *)&record->grec_src[j];
		    struct in_addr *ina = (struct in_addr *)gsrc;

		    if (gsrc > canary) {
			logit(LOG_INFO, 0, "Invalid group record");
			return;
		    }

		    IF_DEBUG(DEBUG_IGMP)
			logit(LOG_DEBUG, 0, "Remove source[%d] (%s,%s)", j,
			      inet_fmt(ina->s_addr, s2, sizeof(s2)), inet_ntoa(rec_group));
		    accept_leave_message(ifi, src, 0, rec_group.s_addr);
		}
		break;

	    default:
		/* RFC3376: Unrecognized Record Type values MUST be silently ignored. */
		break;
	}

	record = (struct igmpv3_grec *)((uint8_t *)record + record_size);
    }
}


/*
 * Send a periodic probe on all vifs.
 * Useful to determine one-way interfaces.
 * Detect neighbor loss faster.
 */
void probe_for_neighbors(void)
{
    struct uvif *uv;
    vifi_t vifi;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & (VIFF_DOWN | VIFF_DISABLED))
	    continue;

	send_probe_on_vif(uv);
    }
}


/*
 * Send a list of all of our neighbors to the requestor, `src'.
 */
void accept_neighbor_request(uint32_t src, uint32_t dst)
{
    struct listaddr *la;
    uint32_t them = src;
    uint32_t temp_addr;
    uint8_t *p, *ncount;
    struct uvif *uv;
    vifi_t vifi;
    int	datalen;

#define PUT_ADDR(a)	temp_addr = ntohl(a); \
			*p++ = temp_addr >> 24; \
			*p++ = (temp_addr >> 16) & 0xFF; \
			*p++ = (temp_addr >> 8) & 0xFF; \
			*p++ = temp_addr & 0xFF;

    p = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
    datalen = 0;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_flags & VIFF_DISABLED)
	    continue;

	ncount = 0;

	TAILQ_FOREACH(la, &uv->uv_neighbors, al_link) {

	    /* Make sure that there's room for this neighbor... */
	    if (datalen + (ncount == 0 ? 4 + 3 + 4 : 4) > MAX_DVMRP_DATA_LEN) {
		send_igmp(INADDR_ANY, them, IGMP_DVMRP, DVMRP_NEIGHBORS,
			  htonl(MROUTED_LEVEL), datalen);
		p = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
		datalen = 0;
		ncount = 0;
	    }

	    /* Put out the header for this neighbor list... */
	    if (ncount == 0) {
		PUT_ADDR(uv->uv_lcl_addr);
		*p++ = uv->uv_metric;
		*p++ = uv->uv_threshold;
		ncount = p;
		*p++ = 0;
		datalen += 4 + 3;
	    }

	    PUT_ADDR(la->al_addr);
	    datalen += 4;
	    (*ncount)++;
	}
    }

    if (datalen != 0)
	send_igmp(INADDR_ANY, them, IGMP_DVMRP, DVMRP_NEIGHBORS,
			htonl(MROUTED_LEVEL), datalen);
}

/*
 * Send a list of all of our neighbors to the requestor, `src'.
 */
void accept_neighbor_request2(uint32_t src, uint32_t dst)
{
    struct listaddr *la;
    uint32_t them = src;
    uint8_t *p, *ncount;
    struct uvif *uv;
    vifi_t vifi;
    int	datalen;

    p = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
    datalen = 0;

    UVIF_FOREACH(vifi, uv) {
	uint16_t vflags = uv->uv_flags;
	uint8_t rflags = 0;

	if (vflags & VIFF_TUNNEL)
	    rflags |= DVMRP_NF_TUNNEL;
	if (vflags & VIFF_SRCRT)
	    rflags |= DVMRP_NF_SRCRT;
	if (vflags & VIFF_DOWN)
	    rflags |= DVMRP_NF_DOWN;
	if (vflags & VIFF_DISABLED)
	    rflags |= DVMRP_NF_DISABLED;
	if (vflags & VIFF_QUERIER)
	    rflags |= DVMRP_NF_QUERIER;
	if (vflags & VIFF_LEAF)
	    rflags |= DVMRP_NF_LEAF;

	ncount = 0;
	if (TAILQ_EMPTY(&uv->uv_neighbors)) {
	    /*
	     * include down & disabled interfaces and interfaces on
	     * leaf nets.
	     */
	    if (rflags & DVMRP_NF_TUNNEL)
		rflags |= DVMRP_NF_DOWN;
	    if (datalen > MAX_DVMRP_DATA_LEN - 12) {
		send_igmp(INADDR_ANY, them, IGMP_DVMRP, DVMRP_NEIGHBORS2,
			  htonl(MROUTED_LEVEL), datalen);
		p = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
		datalen = 0;
	    }
	    *(uint32_t*)p = uv->uv_lcl_addr;
	    p += 4;
	    *p++ = uv->uv_metric;
	    *p++ = uv->uv_threshold;
	    *p++ = rflags;
	    *p++ = 1;
	    *(uint32_t*)p =  uv->uv_rmt_addr;
	    p += 4;
	    datalen += 12;
	} else {
	    TAILQ_FOREACH(la, &uv->uv_neighbors, al_link) {
		/* Make sure that there's room for this neighbor... */
		if (datalen + (ncount == 0 ? 4+4+4 : 4) > MAX_DVMRP_DATA_LEN) {
		    send_igmp(INADDR_ANY, them, IGMP_DVMRP, DVMRP_NEIGHBORS2,
			      htonl(MROUTED_LEVEL), datalen);
		    p = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
		    datalen = 0;
		    ncount = 0;
		}

		/* Put out the header for this neighbor list... */
		if (ncount == 0) {
		    /* If it's a one-way tunnel, mark it down. */
		    if (rflags & DVMRP_NF_TUNNEL && la->al_flags & NBRF_ONEWAY)
			rflags |= DVMRP_NF_DOWN;
		    *(uint32_t*)p = uv->uv_lcl_addr;
		    p += 4;
		    *p++ = uv->uv_metric;
		    *p++ = uv->uv_threshold;
		    *p++ = rflags;
		    ncount = p;
		    *p++ = 0;
		    datalen += 4 + 4;
		}

		/* Don't report one-way peering on phyint at all */
		if (!(rflags & DVMRP_NF_TUNNEL) && la->al_flags & NBRF_ONEWAY)
		    continue;
		*(uint32_t*)p = la->al_addr;
		p += 4;
		datalen += 4;
		(*ncount)++;
	    }

	    if (*ncount == 0) {
		*(uint32_t*)p = uv->uv_rmt_addr;
		p += 4;
		datalen += 4;
		(*ncount)++;
	    }
	}
    }

    if (datalen != 0)
	send_igmp(INADDR_ANY, them, IGMP_DVMRP, DVMRP_NEIGHBORS2,
		htonl(MROUTED_LEVEL), datalen);
}

void accept_info_request(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen)
{
    uint8_t *q;
    int len;
    int outlen = 0;

    q = (uint8_t *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);

    /* To be general, this must deal properly with breaking up over-sized
     * packets.  That implies passing a length to each function, and
     * allowing each function to request to be called again.  Right now,
     * we're only implementing the one thing we are positive will fit into
     * a single packet, so we wimp out.
     */
    while (datalen > 0) {
	len = 0;
	switch (*p) {
	    case DVMRP_INFO_VERSION:
		/* Never let version be more than 100 bytes, see below for more. */
		len = info_version(q, strlen(versionstring));
		break;

	    case DVMRP_INFO_NEIGHBORS:
	    default:
		logit(LOG_INFO, 0, "Ignoring unknown info type %d", *p);
		break;
	}

	*(q + 1) = len++;
	outlen += len * 4;
	q += len * 4;
	len = (*(p+1) + 1) * 4;
	p += len;
	datalen -= len;
    }

    if (outlen != 0)
	send_igmp(INADDR_ANY, src, IGMP_DVMRP, DVMRP_INFO_REPLY,
		  htonl(MROUTED_LEVEL), outlen);
}

/*
 * Information response -- return version string
 */
static int info_version(uint8_t *p, size_t plen)
{
    int len;

    *p++ = DVMRP_INFO_VERSION;
    p++;	/* skip over length */
    *p++ = 0;	/* zero out */
    *p++ = 0;	/* reserved fields */

    /* XXX: We use sizeof(versionstring) instead of the available 
     *      space in send_buf[] because that buffer is 8192 bytes.
     *      It is not very likely our versionstring will ever be
     *      as long as 100 bytes, but it's better to limit the amount
     *      of data copied to send_buf since we do not want to risk
     *      sending MAX size frames. */
    len = strlcpy((char *)p, versionstring, plen);

    return ((len + 3) / 4);
}

/*
 * Process an incoming neighbor-list message.
 */
void accept_neighbors(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
    logit(LOG_INFO, 0, "Ignoring spurious DVMRP neighbor list from %s to %s",
	  inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
}


/*
 * Process an incoming neighbor-list message.
 */
void accept_neighbors2(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
    IF_DEBUG(DEBUG_PKT) {
	logit(LOG_DEBUG, 0, "Ignoring spurious DVMRP neighbor list2 from %s to %s",
	      inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
    }
}

/*
 * Process an incoming info reply message.
 */
void accept_info_reply(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen)
{
    IF_DEBUG(DEBUG_PKT) {
	logit(LOG_DEBUG, 0, "Ignoring spurious DVMRP info reply from %s to %s",
	      inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
    }
}


/*
 * Update the neighbor entry for neighbor 'addr' on vif 'vifi'.
 * 'msgtype' is the type of DVMRP message received from the neighbor.
 * Return the neighbor entry if 'addr' is a valid neighbor, FALSE otherwise.
 */
struct listaddr *update_neighbor(vifi_t vifi, uint32_t addr, int msgtype, char *p, size_t datalen, uint32_t level)
{
    int mv = (level >> 8) & 0xff;
    uint32_t send_tables = 0;
    int in_router_list = 0;
    int pv = level & 0xff;
    int do_reset = FALSE;
    uint32_t genid = 0;
    int has_genid = 0;
    int dvmrpspec = 0;
    struct listaddr *n;
    struct uvif *uv;
    size_t i;

    /*
     * Confirm that 'addr' is a valid neighbor address on vif 'vifi'.
     * IT IS ASSUMED that this was preceded by a call to
     * find_vif_direct(), which checks that 'addr' is either a valid
     * remote tunnel endpoint or a non-broadcast address belonging to a
     * directly-connected subnet.  Therefore, here we check only that
     * 'addr' is not our own address (due to an impostor or erroneous
     * loopback) or an address of the form {subnet,0} ("the unknown
     * host").  These checks are not performed in find_vif_direct()
     * because those types of address are acceptable for some types of
     * IGMP message (such as group membership reports).
     */
    uv = find_uvif(vifi);
    if (!uv)
	return NULL;

    if (!(uv->uv_flags & VIFF_TUNNEL) && (addr == uv->uv_lcl_addr ||
					  addr == uv->uv_subnet )) {
	logit(LOG_WARNING, 0, "Received DVMRP message from %s: %s",
	      (addr == uv->uv_lcl_addr) ? "self (check device loopback)" : "'the unknown host'",
	      inet_fmt(addr, s1, sizeof(s1)));
	return NULL;
    }

    /*
     * Ignore all neighbors on vifs forced into leaf mode
     */
    if (uv->uv_flags & VIFF_FORCE_LEAF)
	return NULL;

    /*
     * mrouted's version 3.3 and later include the generation ID
     * and the list of neighbors on the vif in their probe messages.
     */
    if (msgtype == DVMRP_PROBE && ((pv == 3 && mv > 2) ||
				   (pv > 3 && pv < 10))) {
	uint32_t router;

	IF_DEBUG(DEBUG_PEER) {
	    logit(LOG_DEBUG, 0, "Checking probe from %s (%d.%d) on vif %u",
		  inet_fmt(addr, s1, sizeof(s1)), pv, mv, vifi);
	}

	if (datalen < 4) {
	    logit(LOG_WARNING, 0, "Received truncated probe message from %s (len %zd)",
		  inet_fmt(addr, s1, sizeof(s1)), datalen);
	    return NULL;
	}

	has_genid = 1;

	for (i = 0; i < 4; i++)
	  ((char *)&genid)[i] = *p++;
	datalen -= 4;

	while (datalen > 0) {
	    if (datalen < 4) {
		logit(LOG_WARNING, 0, "Received truncated probe message from %s (len %zd)",
		      inet_fmt(addr, s1, sizeof(s1)), datalen);
		return NULL;
	    }

	    for (i = 0; i < 4; i++)
	      ((char *)&router)[i] = *p++;
	    datalen -= 4;

	    if (router == uv->uv_lcl_addr) {
		in_router_list = 1;
		break;
	    }
	}
    }

    if ((pv == 3 && mv == 255) || (pv > 3 && pv < 10))
	dvmrpspec = 1;

    /*
     * Look for addr in list of neighbors.
     */
    TAILQ_FOREACH(n, &uv->uv_neighbors, al_link) {
	if (addr == n->al_addr) {
	    break;
	}
    }

    if (n == NULL) {
	/*
	 * New neighbor.
	 *
	 * If this neighbor follows the DVMRP spec, start the probe
	 * handshake.  If not, then it doesn't require the probe
	 * handshake, so establish the peering immediately.
	 */
	if (dvmrpspec && (msgtype != DVMRP_PROBE))
	    return NULL;

	for (i = 0; i < MAXNBRS; i++)
	    if (nbrs[i] == NULL)
		break;

	if (i == MAXNBRS) {
	    /* XXX This is a severe new restriction. */
	    /* XXX want extensible bitmaps! */
	    logit(LOG_ERR, 0, "Cannot handle %zuth neighbor %s on vif %u",
		  MAXNBRS, inet_fmt(addr, s1, sizeof(s1)), vifi);
	    return NULL;	/* NOTREACHED */
	}

	/*
	 * Add it to our list of neighbors.
	 */
	IF_DEBUG(DEBUG_PEER) {
	    logit(LOG_DEBUG, 0, "New neighbor %s on vif %u v%d.%d nf 0x%02x idx %zu",
		  inet_fmt(addr, s1, sizeof(s1)), vifi, level & 0xff, (level >> 8) & 0xff,
		  (level >> 16) & 0xff, i);
	}

	n = calloc(1, sizeof(struct listaddr));
	if (!n) {
	    logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	    return NULL;
	}

	n->al_addr      = addr;
	n->al_pv	= pv;
	n->al_mv	= mv;
	n->al_genid	= has_genid ? genid : 0;
	n->al_index	= i;
	nbrs[i] = n;

	time(&n->al_ctime);
	n->al_timer     = 0;
	n->al_flags	= has_genid ? NBRF_GENID : 0;

	TAILQ_INSERT_TAIL(&uv->uv_neighbors, n, al_link);

	/*
	 * If we are not configured to peer with non-pruning routers,
	 * check the deprecated "I-know-how-to-prune" bit.  This bit
	 * was MBZ in early mrouted implementations (<3.5) and is required
	 * to be set by the DVMRPv3 specification.
	 */
	if (!(uv->uv_flags & VIFF_ALLOW_NONPRUNERS) &&
	    !((level & 0x020000) || (pv == 3 && mv < 5))) {
	    n->al_flags |= NBRF_TOOOLD;
	}

	/*
	 * If this router implements the DVMRPv3 spec, then don't peer
	 * with him if we haven't yet established a bidirectional connection.
	 */
	if (dvmrpspec) {
	    if (!in_router_list) {
		IF_DEBUG(DEBUG_PEER) {
		    logit(LOG_DEBUG, 0, "Waiting for probe from %s with my addr",
			  inet_fmt(addr, s1, sizeof(s1)));
		}
		n->al_flags |= NBRF_WAITING;
		return NULL;
	    }
	}

	if (n->al_flags & NBRF_DONTPEER) {
	    IF_DEBUG(DEBUG_PEER) {
		logit(LOG_DEBUG, 0, "Not peering with %s on vif %u because %x",
		      inet_fmt(addr, s1, sizeof(s1)), vifi, n->al_flags & NBRF_DONTPEER);
	    }
	    return NULL;
	}

	/*
	 * If we thought that we had no neighbors on this vif, send a route
	 * report to the vif.  If this is just a new neighbor on the same
	 * vif, send the route report just to the new neighbor.
	 */
	if (NBRM_ISEMPTY(uv->uv_nbrmap)) {
	    send_tables = uv->uv_dst_addr;
	    neighbor_vifs++;
	} else {
	    send_tables = addr;
	}


	NBRM_SET(i, uv->uv_nbrmap);
	add_neighbor_to_routes(vifi, i);
    } else {
	/*
	 * Found it.  Reset its timer.
	 */
	n->al_timer = 0;

	if (n->al_flags & NBRF_WAITING && msgtype == DVMRP_PROBE) {
	    n->al_flags &= ~NBRF_WAITING;
	    if (!in_router_list) {
		logit(LOG_WARNING, 0, "Possible one-way peering with %s on vif %u",
		      inet_fmt(addr, s1, sizeof(s1)), vifi);
		n->al_flags |= NBRF_ONEWAY;
		return NULL;
	    } else {
		if (NBRM_ISEMPTY(uv->uv_nbrmap)) {
		    send_tables = uv->uv_dst_addr;
		    neighbor_vifs++;
		} else {
		    send_tables = addr;
		}
		NBRM_SET(n->al_index, uv->uv_nbrmap);
		add_neighbor_to_routes(vifi, n->al_index);
		IF_DEBUG(DEBUG_PEER) {
		    logit(LOG_DEBUG, 0, "%s on vif %u exits WAITING",
			  inet_fmt(addr, s1, sizeof(s1)), vifi);
		}
	    }
	}

	if ((n->al_flags & NBRF_ONEWAY) && msgtype == DVMRP_PROBE) {
	    if (in_router_list) {
		if (NBRM_ISEMPTY(uv->uv_nbrmap))
		    neighbor_vifs++;
		NBRM_SET(n->al_index, uv->uv_nbrmap);
		add_neighbor_to_routes(vifi, n->al_index);
		logit(LOG_NOTICE, 0, "Peering with %s on vif %u is no longer one-way",
			inet_fmt(addr, s1, sizeof(s1)), vifi);
		n->al_flags &= ~NBRF_ONEWAY;
	    } else {
		/* XXX rate-limited warning message? */
		IF_DEBUG(DEBUG_PEER) {
		    logit(LOG_DEBUG, 0, "%s on vif %u is still ONEWAY",
			  inet_fmt(addr, s1, sizeof(s1)), vifi);
		}
	    }
	}

	/*
	 * When peering with a genid-capable but pre-DVMRP spec peer,
	 * we might bring up the peering with a route report and not
	 * remember his genid.  Assume that he doesn't send a route
	 * report and then reboot before sending a probe.
	 */
	if (has_genid && !(n->al_flags & NBRF_GENID)) {
	    n->al_flags |= NBRF_GENID;
	    n->al_genid = genid;
	}

	/*
	 * update the neighbors version and protocol number and genid
	 * if changed => router went down and came up, 
	 * so take action immediately.
	 */
	if ((n->al_pv != pv) ||
	    (n->al_mv != mv) ||
	    (has_genid && n->al_genid != genid)) {

	    do_reset = TRUE;
	    IF_DEBUG(DEBUG_PEER) {
		logit(LOG_DEBUG, 0, "Version/genid change neighbor %s [old:%d.%d/%8x, new:%d.%d/%8x]",
		      inet_fmt(addr, s1, sizeof(s1)),
		      n->al_pv, n->al_mv, n->al_genid, pv, mv, genid);
	    }

	    n->al_pv = pv;
	    n->al_mv = mv;
	    n->al_genid = genid;
	    time(&n->al_ctime);
	}

	if ((pv == 3 && mv > 2) || (pv > 3 && pv < 10)) {
	    if (!(n->al_flags & VIFF_ONEWAY) && has_genid && !in_router_list &&
				(time(NULL) - n->al_ctime > 20)) {
		if (NBRM_ISSET(n->al_index, uv->uv_nbrmap)) {
		    NBRM_CLR(n->al_index, uv->uv_nbrmap);
		    if (NBRM_ISEMPTY(uv->uv_nbrmap))
			neighbor_vifs--;
		}
		delete_neighbor_from_routes(addr, vifi, n->al_index);
		reset_neighbor_state(vifi, addr);
		logit(LOG_WARNING, 0, "Peering with %s on vif %u is one-way",
		      inet_fmt(addr, s1, sizeof(s1)), vifi);
		n->al_flags |= NBRF_ONEWAY;
	    }
	}

	if (n->al_flags & NBRF_DONTPEER) {
	    IF_DEBUG(DEBUG_PEER) {
		logit(LOG_DEBUG, 0, "Not peering with %s on vif %u because %x",
		      inet_fmt(addr, s1, sizeof(s1)), vifi, n->al_flags & NBRF_DONTPEER);
	    }
	    return NULL;
	}

	/* check "leaf" flag */
    }
    if (do_reset) {
	reset_neighbor_state(vifi, addr);
	if (!send_tables)
	    send_tables = addr;
    }
    if (send_tables) {
	send_probe_on_vif(uv);
	report(ALL_ROUTES, vifi, send_tables);
    }
    uv->uv_leaf_timer = 0;
    uv->uv_flags &= ~VIFF_LEAF;

    return n;
}

uint32_t vif_nbr_expire_time(struct listaddr *al)
{
    if ((al->al_pv == 3 && al->al_mv >= 3) || (al->al_pv > 3 && al->al_pv < 10))
	return NEIGHBOR_EXPIRE_TIME;

    return OLD_NEIGHBOR_EXPIRE_TIME;
}

/*
 * On every timer interrupt, advance the timer in each neighbor and
 * group entry on every vif.
 */
void age_vifs(void)
{
    struct listaddr *a, *tmp;
    struct uvif *uv;
    vifi_t vifi;

    UVIF_FOREACH(vifi, uv) {
	if (uv->uv_leaf_timer && (uv->uv_leaf_timer -= TIMER_INTERVAL == 0))
		uv->uv_flags |= VIFF_LEAF;

	TAILQ_FOREACH_SAFE(a, &uv->uv_neighbors, al_link, tmp) {
	    uint32_t exp_time = vif_nbr_expire_time(a);

	    a->al_timer += TIMER_INTERVAL;
	    if (a->al_timer < exp_time)
		continue;

	    IF_DEBUG(DEBUG_PEER) {
		logit(LOG_DEBUG, 0, "Neighbor %s (%d.%d) expired after %d seconds",
		      inet_fmt(a->al_addr, s1, sizeof(s1)), a->al_pv, a->al_mv, exp_time);
	    }

	    /*
	     * Neighbor has expired; delete it from the neighbor list,
	     * delete it from the 'dominants' and 'subordinates arrays of
	     * any route entries.
	     */
	    NBRM_CLR(a->al_index, uv->uv_nbrmap);
	    nbrs[a->al_index] = NULL;	/* XXX is it a good idea to reuse indxs? */
	    TAILQ_REMOVE(&uv->uv_neighbors, a, al_link);

	    delete_neighbor_from_routes(a->al_addr, vifi, a->al_index);
	    reset_neighbor_state(vifi, a->al_addr);

	    if (NBRM_ISEMPTY(uv->uv_nbrmap))
		neighbor_vifs--;

	    uv->uv_leaf_timer = LEAF_CONFIRMATION_TIME;

	    free(a);
	}

	if (uv->uv_querier) {
	    uv->uv_querier->al_timer += TIMER_INTERVAL;
	    if (uv->uv_querier->al_timer > router_timeout) {
		/*
		 * The current querier has timed out.  We must become the
		 * querier.
		 */
		logit(LOG_INFO, 0, "Querier %s timed out, assuming role on %s",
		      inet_fmt(uv->uv_querier->al_addr, s1, sizeof(s1)), uv->uv_name);
		free(uv->uv_querier);
		uv->uv_querier = NULL;
		uv->uv_flags |= VIFF_QUERIER;
		send_query(uv, allhosts_group, igmp_response_interval * IGMP_TIMER_SCALE, 0);
	    }
	}
    }
}

/*
 * Returns the neighbor info struct for a given neighbor
 */
struct listaddr *neighbor_info(vifi_t vifi, uint32_t addr)
{
    struct listaddr *al;
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (!uv)
	return NULL;

    TAILQ_FOREACH(al, &uv->uv_neighbors, al_link) {
	if (al->al_addr == addr)
	    return al;
    }

    return NULL;
}

static struct vnflags {
	int	vn_flag;
	char    vn_ch;
	char   *vn_name;
} vifflags[] = {
    { VIFF_DOWN,		  0, "down" },
	{ VIFF_DISABLED,	'!', "disabled" },
	{ VIFF_QUERIER,		'Q', "querier" },
	{ VIFF_ONEWAY,		'1', "one-way" },
	{ VIFF_LEAF,		'L', "leaf" },
	{ VIFF_IGMPV1,		  0, "IGMPv1" },
	{ VIFF_IGMPV2,		  0, "IGMPv2" },
	{ VIFF_REXMIT_PRUNES,	  0, "rexmit_prunes" },
	{ VIFF_PASSIVE,		'-', "passive" },
	{ VIFF_ALLOW_NONPRUNERS,  0, "allow_nonpruners" },
	{ VIFF_NOFLOOD,		  0, "noflood" },
	{ VIFF_NOTRANSIT,	  0, "notransit" },
	{ VIFF_BLASTER,		  0, "blaster" },
	{ VIFF_FORCE_LEAF,	  0, "force_leaf" },
	{ VIFF_OTUNNEL,		  0, "old-tunnel" },
};

char *vif_sflags(uint32_t flags)
{
    static char buf[10];
    size_t i, j = 0;

    /*
     * Skip useless flags, we only target ipc.c:show_iface() for now.
     */
    flags &= VIFF_DISABLED | VIFF_QUERIER | VIFF_ONEWAY | VIFF_LEAF | VIFF_PASSIVE;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < ARRAY_LEN(vifflags); i++) {
	if (!vifflags[i].vn_ch)
	    continue;

	if (flags & vifflags[i].vn_flag)
	    buf[j++] = vifflags[i].vn_ch;
    }

    return buf;
}

/*
 * Short forms of vn_name taken from JunOS
 * https://www.juniper.net/documentation/en_US/junos/topics/reference/command-summary/show-dvmrp-neighbors.html
 */
static struct vnflags nbrflags[] = {
	{ NBRF_LEAF,		'L', "leaf" },
	{ NBRF_GENID,		'G', "have-genid" },
	{ NBRF_WAITING,		'W', "waiting" },
	{ NBRF_ONEWAY,		'1', "one-way" },
	{ NBRF_TOOOLD,		'O', "too old" },
	{ NBRF_TOOMANYROUTES,	'!', "too many routes" },
	{ NBRF_NOTPRUNING,	'p', "not pruning?" },
};

char *vif_nbr_flags(uint16_t flags, char *buf, size_t len)
{
    size_t i;

    memset(buf, 0, len);
    for (i = 0; i < ARRAY_LEN(nbrflags); i++) {
	if (flags & nbrflags[i].vn_flag) {
	    if (buf[0])
		strlcat(buf, " ", len);
	    strlcat(buf, nbrflags[i].vn_name, len);
	}
    }

    return buf;
}

char *vif_nbr_sflags(uint16_t flags)
{
    static char buf[10];
    size_t i, j = 0;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < ARRAY_LEN(nbrflags); i++) {
	if (flags & nbrflags[i].vn_flag)
	    buf[j++] = nbrflags[i].vn_ch;
    }

    return buf;
}

/*
 * Print the contents of the uvifs array on file 'fp'.
 */
void dump_vifs(FILE *fp, int detail)
{
    struct vif_acl *acl;
    struct listaddr *a;
    struct phaddr *p;
    struct uvif *uv;
    vifi_t vifi;
    size_t i;
    time_t now;
    char *label;

    time(&now);
    if (detail) {
	fprintf(fp, "Virtual Interface Table ");
	if (neighbor_vifs == 1)
	    fprintf(fp, "(This host is a leaf)\n");
	else
	    fprintf(fp, "(%d neighbors)\n", neighbor_vifs);
    }

    fprintf(fp, "Vif  Name  Local-Address                               M  Thr  Rate   Flags=\n");

    UVIF_FOREACH(vifi, uv) {
	struct sioc_vif_req v_req = { 0 };

	fprintf(fp, "%2u %6s  %-15s %6s: %-18s %2u %3u  %5u  ",
		vifi,
		uv->uv_name,
		inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)),
		(uv->uv_flags & VIFF_TUNNEL) ?
			"tunnel":
			"subnet",
		(uv->uv_flags & VIFF_TUNNEL) ?
			inet_fmt(uv->uv_rmt_addr, s2, sizeof(s2)) :
			inet_fmts(uv->uv_subnet, uv->uv_subnetmask, s3, sizeof(s3)),
		uv->uv_metric,
		uv->uv_threshold,
		uv->uv_rate_limit);

	for (i = 0; i < sizeof(vifflags) / sizeof(struct vnflags); i++)
		if (uv->uv_flags & vifflags[i].vn_flag)
			fprintf(fp, " %s", vifflags[i].vn_name);

	fprintf(fp, "\n");
	/*
	fprintf(fp, "                          #routes: %d\n", uv->uv_nroutes);
	*/
	if (uv->uv_admetric != 0)
	    fprintf(fp, "                                        advert-metric %2u\n",
		uv->uv_admetric);

	label = "alternate subnets:";
	for (p = uv->uv_addrs; p; p = p->pa_next) {
	    fprintf(fp, "                %18s %s\n", label,
			inet_fmts(p->pa_subnet, p->pa_subnetmask, s1, sizeof(s1)));
	    label = "";
	}

	label = "peers:";
	TAILQ_FOREACH(a, &uv->uv_neighbors, al_link) {
	    fprintf(fp, "                            %6s %s (%d.%d) [%d]",
		    label, inet_fmt(a->al_addr, s1, sizeof(s1)), a->al_pv, a->al_mv,
		    a->al_index);
	    for (i = 0; i < sizeof(nbrflags) / sizeof(struct vnflags); i++)
		    if (a->al_flags & nbrflags[i].vn_flag)
			    fprintf(fp, " %s", nbrflags[i].vn_name);
	    fprintf(fp, " up %s\n", scaletime(now - a->al_ctime));
	    /*fprintf(fp, " #routes %d\n", a->al_nroutes);*/
	    label = "";
	}

	label = "group host (time left):";
	TAILQ_FOREACH(a, &uv->uv_groups, al_link) {
	    fprintf(fp, "           %23s %-15s %-15s (%s)\n",
		    label,
		    inet_fmt(a->al_addr, s1, sizeof(s1)),
		    inet_fmt(a->al_reporter, s2, sizeof(s2)),
		    scaletime(timer_get(a->al_timerid)));
	    label = "";
	}
	label = "boundaries:";
	for (acl = uv->uv_acl; acl; acl = acl->acl_next) {
	    fprintf(fp, "                       %11s %-18s\n", label,
			inet_fmts(acl->acl_addr, acl->acl_mask, s1, sizeof(s1)));
	    label = "";
	}
	if (uv->uv_filter) {
	    struct vf_element *vfe;
	    char lbuf[100];

	    snprintf(lbuf, sizeof(lbuf), "%5s %7s filter:",
		     uv->uv_filter->vf_flags & VFF_BIDIR  ? "bidir"  : "     ",
		     uv->uv_filter->vf_type == VFT_ACCEPT ? "accept" : "deny");
	    label = lbuf;
	    for (vfe = uv->uv_filter->vf_filter; vfe; vfe = vfe->vfe_next) {
		fprintf(fp, "           %23s %-18s%s\n",
			label,
			inet_fmts(vfe->vfe_addr, vfe->vfe_mask, s1, sizeof(s1)),
			vfe->vfe_flags & VFEF_EXACT ? " (exact)" : "");
		label = "";
	    }
	}
	if (!(uv->uv_flags & (VIFF_TUNNEL|VIFF_DOWN|VIFF_DISABLED))) {
	    fprintf(fp, "                     IGMP querier: ");
	    if (uv->uv_querier == NULL)
		if (uv->uv_flags & VIFF_QUERIER)
		    fprintf(fp, "%-18s (this system)\n",
				    inet_fmt(uv->uv_lcl_addr, s1, sizeof(s1)));
		else
		    fprintf(fp, "NONE - querier election failure?\n");
	    else
	    	fprintf(fp, "%-18s up %s last heard %s ago\n",
			inet_fmt(uv->uv_querier->al_addr, s1, sizeof(s1)),
			scaletime(now - uv->uv_querier->al_ctime),
			scaletime(uv->uv_querier->al_timer));
	}
	if (uv->uv_flags & VIFF_BLASTER)
	    fprintf(fp, "                  blasterbuf size: %dk\n",
			uv->uv_blasterlen / 1024);
	fprintf(fp, "                      Nbr bitmaps: 0x%08x%08x\n",/*XXX*/
			uv->uv_nbrmap.hi, uv->uv_nbrmap.lo);
	if (uv->uv_prune_lifetime != 0)
	    fprintf(fp, "                   Prune Lifetime: %d seconds\n",
					    uv->uv_prune_lifetime);

	v_req.vifi = vifi;
	if (did_final_init) {
	    if (ioctl(udp_socket, SIOCGETVIFCNT, &v_req) < 0) {
		logit(LOG_WARNING, errno, "Failed ioctl SIOCGETVIFCNT on vif %u", vifi);
	    } else {
		fprintf(fp, "                   pkts/bytes in : %lu/%lu\n",
			v_req.icount, v_req.ibytes);
		fprintf(fp, "                   pkts/bytes out: %lu/%lu\n",
			v_req.ocount, v_req.obytes);
	    }
	}
	fprintf(fp, "\n");
    }
}

/*
 * Time out old version compatibility mode
 */
static void group_version_cb(void *arg)
{
    cbk_t *cbk = (cbk_t *)arg;
    vifi_t vifi = cbk->vifi;
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (!uv)
	return;

    if (cbk->g->al_pv < 3)
	cbk->g->al_pv++;

    logit(LOG_INFO, 0, "Switching IGMP compatibility mode from v%d to v%d for group %s on %s",
	  cbk->g->al_pv - 1, cbk->g->al_pv, inet_fmt(cbk->g->al_addr, s1, sizeof(s1)), uv->uv_name);

    if (cbk->g->al_pv < 3)
	timer_set(IGMP_GROUP_MEMBERSHIP_INTERVAL, group_version_cb, cbk);
    else
	free(cbk);
}

/*
 * Set a timer to switch version back on a vif.
 */
static int group_version_timer(vifi_t vifi, struct listaddr *g)
{
    cbk_t *cbk;

    cbk = calloc(1, sizeof(cbk_t));
    if (!cbk) {
	logit(LOG_ERR, errno, "%s(): Failed allocating memory", __func__);
	return -1;
    }

    cbk->vifi = vifi;
    cbk->g = g;

    return timer_set(IGMP_GROUP_MEMBERSHIP_INTERVAL, group_version_cb, cbk);
}

/*
 * Time out record of a group membership on a vif
 */
static void delete_group_cb(void *arg)
{
    cbk_t *cbk = (cbk_t *)arg;
    struct listaddr *g = cbk->g;
    vifi_t vifi = cbk->vifi;
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (!uv)
	return;

    logit(LOG_DEBUG, 0, "Group membership timeout for %s on %s",
	  inet_fmt(cbk->g->al_addr, s1, sizeof(s1)), uv->uv_name);

    if (g->al_query > 0)
	g->al_query = timer_clear(g->al_query);

    if (g->al_pv_timerid > 0)
	g->al_pv_timerid = timer_clear(g->al_pv_timerid);

    delete_lclgrp(vifi, g->al_addr);
    TAILQ_REMOVE(&uv->uv_groups, g, al_link);
    free(g);

    free(cbk);
}

/*
 * Set a timer to delete the record of a group membership on a vif.
 */
static int delete_group_timer(vifi_t vifi, struct listaddr *g)
{
    cbk_t *cbk;

    cbk = calloc(1, sizeof(cbk_t));
    if (!cbk) {
	logit(LOG_ERR, errno, "%s(): Failed allocating memory", __func__);
	return -1;
    }

    cbk->g    = g;
    cbk->vifi = vifi;

    /* Record mtime for IPC "show igmp" */
    g->al_mtime = virtual_time;

    return timer_set(g->al_timer, delete_group_cb, cbk);
}

/*
 * Send a group-specific query.
 */
static int do_send_gsq(cbk_t *cbk)
{
    struct uvif *uv;

    uv = find_uvif(cbk->vifi);
    if (!uv)
	return -1;

    send_query(uv, cbk->g->al_addr, cbk->delay * IGMP_TIMER_SCALE, cbk->g->al_addr);
    if (--cbk->num == 0) {
	cbk->g->al_query = 0;	/* we're done, clear us from group */
	free(cbk);
	return 0;
    }

    return timer_set(cbk->delay, send_query_cb, cbk);
}

static void send_query_cb(void *arg)
{
    do_send_gsq((cbk_t *)arg);
}

/*
 * Set a timer to send a group-specific query.
 */
static int send_query_timer(vifi_t vifi, struct listaddr *g, int delay, int num)
{
    cbk_t *cbk;

    cbk = calloc(1, sizeof(cbk_t));
    if (!cbk) {
	logit(LOG_ERR, errno, "%s(): Failed allocating memory", __func__);
	return -1;
    }

    cbk->vifi  = vifi;
    cbk->g     = g;
    cbk->delay = delay;
    cbk->num   = num;

    return do_send_gsq(cbk);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "cc-mode"
 * End:
 */
