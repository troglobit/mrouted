/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"

extern int cache_lifetime;
extern int prune_lifetime;
extern struct rtentry *routing_table;

extern int phys_vif;
extern int allow_black_holes;

/*
 * randomize value to obtain a value between .5x and 1.5x
 * in order to prevent synchronization
 */
#define JITTERED_VALUE(x) ((x) / 2 + ((int)random() % (x)))
#define	CACHE_LIFETIME(x) JITTERED_VALUE(x) /* XXX */

struct gtable *kernel_table;		/* ptr to list of kernel grp entries*/
struct gtable *kernel_no_route;		/* list of grp entries w/o routes   */
struct gtable *gtp;			/* pointer for kernel rt entries    */
unsigned int kroutes;			/* current number of cache entries  */

/****************************************************************************
                       Functions that are local to prune.c
****************************************************************************/
static int		scoped_addr(vifi_t vifi, uint32_t addr);
static void		prun_add_ttls(struct gtable *gt);
static int		pruning_neighbor(vifi_t vifi, uint32_t addr);
static int		can_mtrace(vifi_t vifi, uint32_t addr);
static struct ptable *	find_prune_entry(uint32_t vr, struct ptable *pt);
static void		remove_sources(struct gtable *gt);
static void		rexmit_prune(void *arg);
static void		expire_prune(vifi_t vifi, struct gtable *gt);
static void		send_prune(struct gtable *gt);
static void		send_graft(struct gtable *gt);
static void		send_graft_ack(uint32_t src, uint32_t dst, uint32_t origin, uint32_t grp, vifi_t vifi);
static void		update_kernel(struct gtable *g);

/*
 * Updates the ttl values for each vif.
 */
static void prun_add_ttls(struct gtable *gt)
{
    struct uvif *v;
    vifi_t vifi;

    for (vifi = 0, v = uvifs; vifi < numvifs; ++vifi, ++v) {
	if (VIFM_ISSET(vifi, gt->gt_grpmems))
	    gt->gt_ttls[vifi] = v->uv_threshold;
	else
	    gt->gt_ttls[vifi] = 0;
    }
}

/*
 * checks for scoped multicast addresses
 * XXX I want to make the check of allow_black_holes based on ALLOW_BLACK_HOLES
 * but macros are not functions.
 */
#define GET_SCOPE(gt) {                                                 \
	vifi_t _i;                                                      \
	VIFM_CLRALL((gt)->gt_scope);                                    \
	if (allow_black_holes ||                                        \
	    (ntohl((gt)->gt_mcastgrp) & 0xff000000) == 0xef000000)      \
	    for (_i = 0; _i < numvifs; _i++)                            \
		if (scoped_addr(_i, (gt)->gt_mcastgrp))                 \
		    VIFM_SET(_i, (gt)->gt_scope);                       \
	if ((gt)->gt_route == NULL                                      \
            || ((gt)->gt_route->rt_parent != NO_VIF                     \
                && VIFM_ISSET((gt)->gt_route->rt_parent, (gt)->gt_scope))) \
            VIFM_SETALL((gt)->gt_scope);                                \
    }

#define	APPLY_SCOPE(gt)	VIFM_CLR_MASK((gt)->gt_grpmems, (gt)->gt_scope)

#define	GET_MEMBERSHIP(gt, vifi) {                                      \
	if ((gt)->gt_route                                              \
	    && VIFM_ISSET((vifi), (gt)->gt_route->rt_children)          \
	    && (!SUBS_ARE_PRUNED((gt)->gt_route->rt_subordinates,       \
                                 uvifs[vifi].uv_nbrmap, (gt)->gt_prunes) \
                || grplst_mem((vifi), (gt)->gt_mcastgrp)))              \
            VIFM_SET((vifi), (gt)->gt_grpmems);                         \
    }

static int scoped_addr(vifi_t vifi, uint32_t addr)
{
    struct vif_acl *acl;

    for (acl = uvifs[vifi].uv_acl; acl; acl = acl->acl_next)
	if ((addr & acl->acl_mask) == acl->acl_addr)
	    return 1;

    return 0;
}

/*
 * Determine the list of outgoing vifs, based upon
 * route subordinates, prunes received, and group
 * memberships.
 */
void determine_forwvifs(struct gtable *gt)
{
    vifi_t i;

    VIFM_CLRALL(gt->gt_grpmems);
    for (i = 0; i < numvifs; i++) {
	GET_MEMBERSHIP(gt, i);
    }
    GET_SCOPE(gt);
    APPLY_SCOPE(gt);
}

/*
 * Send a prune or a graft if necessary.
 */
void send_prune_or_graft(struct gtable *gt)
{
    if (VIFM_ISEMPTY(gt->gt_grpmems))
	send_prune(gt);
    else if (gt->gt_prsent_timer)
	send_graft(gt);
}

/*
 * Determine if mcastgrp has a listener on vifi
 */
int grplst_mem(vifi_t vifi, uint32_t mcastgrp)
{
    struct listaddr *g;
    struct uvif *v;

    v = &uvifs[vifi];

    for (g = v->uv_groups; g != NULL; g = g->al_next)
	if (mcastgrp == g->al_addr)
	    return 1;

    return 0;
}

/*
 * Finds the group entry with the specified source and netmask.
 * If netmask is 0, it uses the route's netmask.
 *
 * Returns TRUE if found a match, and the global variable gtp is left
 * pointing to entry before the found entry.
 * Returns FALSE if no exact match found, gtp is left pointing to before
 * the entry in question belongs, or is NULL if the it belongs at the
 * head of the list.
 */
int find_src_grp(uint32_t src, uint32_t mask, uint32_t grp)
{
    struct gtable *gt;

    gtp = NULL;
    gt = kernel_table;
    while (gt != NULL) {
	if (grp == gt->gt_mcastgrp &&
	    (mask
	     ? (gt->gt_route->rt_origin     == src  &&
		gt->gt_route->rt_originmask == mask)
	     :
	     ((src & gt->gt_route->rt_originmask) ==
		     gt->gt_route->rt_origin)))
	    return TRUE;
	if (ntohl(grp) > ntohl(gt->gt_mcastgrp) ||
	    (grp == gt->gt_mcastgrp &&
	     (ntohl(mask) < ntohl(gt->gt_route->rt_originmask) ||
	      (mask == gt->gt_route->rt_originmask &&
	       (ntohl(src) > ntohl(gt->gt_route->rt_origin)))))) {
	    gtp = gt;
	    gt = gt->gt_gnext;
	}
	else
	    break;
    }
    return FALSE;
}

/*
 * Check if the neighbor supports pruning
 */
static int pruning_neighbor(vifi_t vifi, uint32_t addr)
{
    struct listaddr *n = neighbor_info(vifi, addr);
    int vers;

    if (n == NULL)
	return 0;

    vers = NBR_VERS(n);
    return (vers >= 0x0300 && ((vers & 0xff00) != 0x0a00));
}

/*
 * Can the neighbor in question handle multicast traceroute?
 */
static int can_mtrace(vifi_t vifi, uint32_t addr)
{
    struct listaddr *n = neighbor_info(vifi, addr);
    int vers;

    if (n == NULL)
	return 1;	/* fail "safe" */

    vers = NBR_VERS(n);
    return (vers >= 0x0303 && ((vers & 0xff00) != 0x0a00));
}

/*
 * Returns the prune entry of the router, or NULL if none exists
 */
static struct ptable *find_prune_entry(uint32_t vr, struct ptable *pt)
{
    while (pt) {
	if (pt->pt_router == vr)
	    return pt;
	pt = pt->pt_next;
    }

    return NULL;
}

/*
 * Remove all the sources hanging off the group table entry from the kernel
 * cache.  Remember the packet counts wherever possible, to keep the mtrace
 * counters consistent.  This prepares for possible prune retransmission,
 * either on a multi-access network or when a prune that we sent upstream
 * has expired.
 */
static void remove_sources(struct gtable *gt)
{
    struct stable *st;
    struct sioc_sg_req sg_req;

    sg_req.grp.s_addr = gt->gt_mcastgrp;

    /*
     * call k_del_rg() on every one of the gt->gt_srctbl entries
     * but first save the packet count so that the mtrace packet
     * counters can remain approximately correct.  There's a race
     * here but it's minor.
     */
    for (st = gt->gt_srctbl; st; st = st->st_next) {
	if (st->st_ctime == 0)
	    continue;

	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "remove_sources() deleting (%s %s) (next is %d sec)",
		  inet_fmt(st->st_origin, s1, sizeof(s1)),
		  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
		  gt->gt_prune_rexmit);
	}
	sg_req.src.s_addr = st->st_origin;
	if (ioctl(udp_socket, SIOCGETSGCNT, (char *)&sg_req) < 0) {
	    sg_req.pktcnt = 0;
	}
	k_del_rg(st->st_origin, gt);
	st->st_ctime = 0;	/* flag that it's not in the kernel any more */
	st->st_savpkt += sg_req.pktcnt;
	kroutes--;
    }

    /*
     * Now, add_table_entry will prune when asked to add a cache entry.
     */
}

/*
 * Prepare for possible prune retransmission
 */
static void rexmit_prune(void *arg)
{
    struct gtable *gt = *(struct gtable **)arg;

    free(arg);

    gt->gt_rexmit_timer = 0;

    /* Make sure we're still not forwarding traffic */
    if (!VIFM_ISEMPTY(gt->gt_grpmems)) {
	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "rexmit_prune() (%s %s): gm:%lx", RT_FMT(gt->gt_route, s1),
		  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), gt->gt_grpmems);
	}
	return;
    }

    remove_sources(gt);
}

/*
 * Send a prune message to the dominant router for
 * this source.
 *
 * Record an entry that a prune was sent for this group
 */
static void send_prune(struct gtable *gt)
{
    struct ptable *pt;
    struct uvif *v;
    uint8_t *p;
    int i, datalen;
    uint32_t dst;
    uint32_t tmp;
    int rexmitting = 0;

    /*
     * Can't process a prune if we don't have an associated route
     * or if the route points to a local interface.
     */
    if (gt->gt_route == NULL || gt->gt_route->rt_parent == NO_VIF || gt->gt_route->rt_gateway == 0)
	return;

    /* Don't send a prune to a non-pruning router */
    if (!pruning_neighbor(gt->gt_route->rt_parent, gt->gt_route->rt_gateway))
	return;

    /*
     * sends a prune message to the router upstream.
     */
    v = &uvifs[gt->gt_route->rt_parent];
#if 0
    dst = v->uv_flags & VIFF_TUNNEL ? dvmrp_group : gt->gt_route->rt_gateway; /*XXX*/
#else
    dst = gt->gt_route->rt_gateway;
#endif

    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;
    datalen = 0;

    /*
     * determine prune lifetime, if this isn't a retransmission.
     *
     * Use interface-specified lifetime if there is one.
     */
    if (gt->gt_prsent_timer == 0) {
	int l = prune_lifetime;

	if (v->uv_prune_lifetime != 0)
	    l = v->uv_prune_lifetime;

	gt->gt_prsent_timer = JITTERED_VALUE(l);
	for (pt = gt->gt_pruntbl; pt; pt = pt->pt_next) {
	    if (pt->pt_timer < gt->gt_prsent_timer)
		gt->gt_prsent_timer = pt->pt_timer;
	}
    } else if (gt->gt_prsent_timer < 0) {
	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "Asked to rexmit? (%s,%s)/%d on vif %u to %s with negative time",
		  RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
		  gt->gt_prsent_timer, gt->gt_route->rt_parent,
		  inet_fmt(gt->gt_route->rt_gateway, s3, sizeof(s3)));
	}
	return;
    } else {
	rexmitting = 1;
    }

    if (rexmitting && !(v->uv_flags & VIFF_REXMIT_PRUNES)) {
	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "Not rexmitting prune for (%s %s)/%d on vif %u to %s",
		  RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
		  gt->gt_prsent_timer, gt->gt_route->rt_parent,
		  inet_fmt(gt->gt_route->rt_gateway, s3, sizeof(s3)));
	}
	return;
    }

    if (gt->gt_prsent_timer <= MIN_PRUNE_LIFE) {
	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "Not bothering to send prune for (%s,%s)/%d on vif %u to %s because it's too short",
		  RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
		  gt->gt_prsent_timer, gt->gt_route->rt_parent,
		  inet_fmt(gt->gt_route->rt_gateway, s3, sizeof(s3)));
	}
	return;
    }

    /*
     * If we have a graft pending, cancel graft retransmission
     */
    gt->gt_grftsnt = 0;

    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(gt->gt_route->rt_origin))[i];
    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(gt->gt_mcastgrp))[i];
    tmp = htonl(gt->gt_prsent_timer);
    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(tmp))[i];
    datalen += 12;

    send_on_vif(v, dst, DVMRP_PRUNE, datalen);

    IF_DEBUG(DEBUG_PRUNE) {
	logit(LOG_DEBUG, 0, "%s prune for (%s %s)/%d on vif %u to %s",
	      rexmitting ? "rexmitted" : "sent",
	      RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
	      gt->gt_prsent_timer, gt->gt_route->rt_parent,
	      inet_fmt(gt->gt_route->rt_gateway, s3, sizeof(s3)));
    }

    if ((v->uv_flags & VIFF_REXMIT_PRUNES) && gt->gt_rexmit_timer == 0 &&
	gt->gt_prsent_timer > gt->gt_prune_rexmit) {
	struct gtable **arg;

	arg = malloc(sizeof(struct gtable *));
	if (!arg) {
	    logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	    return;
	}

	*arg = gt;
	gt->gt_rexmit_timer = timer_set(JITTERED_VALUE(gt->gt_prune_rexmit), rexmit_prune, arg);
	gt->gt_prune_rexmit *= 2;
    }
}

/*
 * a prune was sent upstream
 * so, a graft has to be sent to annul the prune
 * set up a graft timer so that if an ack is not 
 * heard within that time, another graft request
 * is sent out.
 */
static void send_graft(struct gtable *gt)
{
    uint32_t dst;
    uint8_t *p;
    int i, datalen;

    /* Can't send a graft without an associated route */
    if (gt->gt_route == NULL || gt->gt_route->rt_parent == NO_VIF) {
	gt->gt_grftsnt = 0;
	return;
    }

    gt->gt_prsent_timer = 0;
    gt->gt_prune_rexmit = PRUNE_REXMIT_VAL;
    if (gt->gt_rexmit_timer)
	timer_clear(gt->gt_rexmit_timer);

    if (gt->gt_grftsnt == 0)
	gt->gt_grftsnt = 1;

#if 0
    dst = uvifs[gt->gt_route->rt_parent].uv_flags & VIFF_TUNNEL ? dvmrp_group : gt->gt_route->rt_gateway; /*XXX*/
#else
    dst = gt->gt_route->rt_gateway;
#endif

    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;
    datalen = 0;

    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(gt->gt_route->rt_origin))[i];
    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(gt->gt_mcastgrp))[i];
    datalen += 8;

    send_on_vif(&uvifs[gt->gt_route->rt_parent], dst, DVMRP_GRAFT, datalen);
    IF_DEBUG(DEBUG_PRUNE) {
	logit(LOG_DEBUG, 0, "Sent graft for (%s %s) to %s on vif %u",
	      RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
	      inet_fmt(gt->gt_route->rt_gateway, s3, sizeof(s3)), gt->gt_route->rt_parent);
    }
}

/*
 * Send an ack that a graft was received
 */
static void send_graft_ack(uint32_t src, uint32_t dst, uint32_t origin, uint32_t grp, vifi_t vifi)
{
    uint8_t *p;
    int i, datalen;

    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;
    datalen = 0;

    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(origin))[i];
    for (i = 0; i < 4; i++)
	*p++ = ((char *)&(grp))[i];
    datalen += 8;

    if (vifi == NO_VIF)
	send_igmp(src, dst, IGMP_DVMRP, DVMRP_GRAFT_ACK,
		  htonl(MROUTED_LEVEL), datalen);
    else {
#if 0
	if (uvifs[vifi].uv_flags & VIFF_TUNNEL)
	    dst = dvmrp_group;	/* XXX */
#endif
	send_on_vif(&uvifs[vifi], dst, DVMRP_GRAFT_ACK, datalen);
    }

    IF_DEBUG(DEBUG_PRUNE) {
	if (vifi == NO_VIF)
	    logit(LOG_DEBUG, 0, "Sent graft ack for (%s, %s) to %s",
		  inet_fmt(origin, s1, sizeof(s1)), inet_fmt(grp, s2, sizeof(s2)), inet_fmt(dst, s3, sizeof(s3)));
	else
	    logit(LOG_DEBUG, 0, "Sent graft ack for (%s, %s) to %s on vif %u",
		  inet_fmt(origin, s1, sizeof(s1)), inet_fmt(grp, s2, sizeof(s2)), inet_fmt(dst, s3, sizeof(s3)), vifi);
    }
}

/*
 * Update the kernel cache with all the routes hanging off the group entry
 */
static void update_kernel(struct gtable *g)
{
    struct stable *st;

    for (st = g->gt_srctbl; st; st = st->st_next)
	if (st->st_ctime != 0)
	    k_add_rg(st->st_origin, g);
}

/****************************************************************************
                          Functions that are used externally
****************************************************************************/

/*
 * Initialize the kernel table structure
 */
void init_ktable(void)
{
    kernel_table 	= NULL;
    kernel_no_route	= NULL;
    kroutes		= 0;
}

/* 
 * Add a new table entry for (origin, mcastgrp)
 */
void add_table_entry(uint32_t origin, uint32_t mcastgrp)
{
    struct rtentry *r;
    struct gtable *gt, **gtnp, *prev_gt;
    struct stable *st, **stnp;

    /*
     * Since we have to enable mrouting to get the version number,
     * some cache creation requests can sneak through.  Ignore them
     * since we're not going to do useful stuff until we've performed
     * final initialization.
     */
    if (!did_final_init)
	return;

    /* Don't create routing entries for the LAN scoped addresses */
    if (ntohl(mcastgrp) <= INADDR_MAX_LOCAL_GROUP) { /* group <= 224.0.0.255? */
	IF_DEBUG(DEBUG_CACHE)
	    logit(LOG_INFO, 0, "Not creating routing entry for LAN scoped group %s",
		  inet_fmt(mcastgrp, s1, sizeof(s1)));
	return;
    }

    r = determine_route(origin);
    prev_gt = NULL;
    if (r == NULL) {
	/*
	 * Look for it on the no_route table; if it is found then
	 * it will be detected as a duplicate below.
	 */
	for (gt = kernel_no_route; gt; gt = gt->gt_next) {
	    if (mcastgrp == gt->gt_mcastgrp &&
		gt->gt_srctbl && gt->gt_srctbl->st_origin == origin)
			break;
	}
	gtnp = &kernel_no_route;
    } else {
	gtnp = &r->rt_groups;
	while ((gt = *gtnp) != NULL) {
	    if (gt->gt_mcastgrp >= mcastgrp)
		break;
	    gtnp = &gt->gt_next;
	    prev_gt = gt;
	}
    }

    if (!gt || gt->gt_mcastgrp != mcastgrp) {
	gt = calloc(1, sizeof(struct gtable));
	if (!gt) {
	    logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	    return;
	}

	gt->gt_mcastgrp	    = mcastgrp;
	gt->gt_timer   	    = CACHE_LIFETIME(cache_lifetime);
	time(&gt->gt_ctime);
	gt->gt_prsent_timer = 0;
	gt->gt_grftsnt	    = 0;
	gt->gt_srctbl	    = NULL;
	gt->gt_pruntbl	    = NULL;
	gt->gt_route	    = r;
	gt->gt_rexmit_timer = 0;
	NBRM_CLRALL(gt->gt_prunes);
	gt->gt_prune_rexmit = PRUNE_REXMIT_VAL;
#ifdef RSRR
	gt->gt_rsrr_cache   = NULL;
#endif

	/* Calculate forwarding vifs */
	determine_forwvifs(gt);

	/* update ttls */
	prun_add_ttls(gt);

	gt->gt_next = *gtnp;
	*gtnp = gt;
	if (gt->gt_next)
	    gt->gt_next->gt_prev = gt;
	gt->gt_prev = prev_gt;

	if (r) {
	    if (find_src_grp(r->rt_origin, r->rt_originmask, gt->gt_mcastgrp)) {
		struct gtable *g = gtp ? gtp->gt_gnext : kernel_table;

		logit(LOG_WARNING, 0, "Entry for (%s %s) (rt:%p) exists (rt:%p)",
		      RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)),
		      r, g->gt_route);
	    } else {
		if (gtp) {
		    gt->gt_gnext = gtp->gt_gnext;
		    gt->gt_gprev = gtp;
		    gtp->gt_gnext = gt;
		} else {
		    gt->gt_gnext = kernel_table;
		    gt->gt_gprev = NULL;
		    kernel_table = gt;
		}
		if (gt->gt_gnext)
		    gt->gt_gnext->gt_gprev = gt;
	    }
	} else {
	    gt->gt_gnext = gt->gt_gprev = NULL;
	}
    }

    stnp = &gt->gt_srctbl;
    while ((st = *stnp) != NULL) {
	if (ntohl(st->st_origin) >= ntohl(origin))
	    break;
	stnp = &st->st_next;
    }

    if (!st || st->st_origin != origin) {
	st = calloc(1, sizeof(struct stable));
	if (!st) {
	    logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	    return;
	}

	st->st_origin = origin;
	st->st_pktcnt = 0;
	st->st_savpkt = 0;
	time(&st->st_ctime);
	st->st_next = *stnp;
	*stnp = st;
    } else {
	if (st->st_ctime == 0) {
	    /* An old source which we're keeping around for statistics */
	    time(&st->st_ctime);
	} else {
	    /* Ignore kernel->mrouted retransmissions */
	    if (time(0) - st->st_ctime > 5)
		logit(LOG_WARNING, 0, "Kernel entry already exists for (%s %s)",
		      inet_fmt(origin, s1, sizeof(s1)), inet_fmt(mcastgrp, s2, sizeof(s2)));
	    k_add_rg(origin, gt);
	    return;
	}
    }

    kroutes++;
    k_add_rg(origin, gt);

    IF_DEBUG(DEBUG_CACHE) {
	logit(LOG_DEBUG, 0, "Add cache entry (%s %s) gm:%lx, parent-vif:%d",
	      inet_fmt(origin, s1, sizeof(s1)),
	      inet_fmt(mcastgrp, s2, sizeof(s2)),
	      gt->gt_grpmems, r ? r->rt_parent : -1);
    }

    /*
     * If there are no downstream routers that want traffic for
     * this group, send (or retransmit) a prune upstream.
     */
    if (VIFM_ISEMPTY(gt->gt_grpmems))
	send_prune(gt);
}

/*
 * A router has gone down.  Remove prune state pertinent to that router.
 */
void reset_neighbor_state(vifi_t vifi, uint32_t addr)
{
    struct rtentry *r;
    struct gtable *g;
    struct ptable *pt, **ptnp;
    struct stable *st;

    for (g = kernel_table; g; g = g->gt_gnext) {
	r = g->gt_route;

	/*
	 * If neighbor was the parent, remove the prune sent state
	 * and all of the source cache info so that prunes get
	 * regenerated.
	 */
	if (vifi == r->rt_parent) {
	    if (addr == r->rt_gateway) {
		IF_DEBUG(DEBUG_PEER) {
		    logit(LOG_DEBUG, 0, "reset_neighbor_state() parent reset (%s %s)",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));
		}

		g->gt_prsent_timer = 0;
		g->gt_grftsnt = 0;
		while ((st = g->gt_srctbl) != NULL) {
		    g->gt_srctbl = st->st_next;
		    if (st->st_ctime != 0) {
			k_del_rg(st->st_origin, g);
			kroutes--;
		    }
		    free(st);
		}
	    }
	} else {
	    /*
	     * Remove any prunes that this router has sent us.
	     */
	    ptnp = &g->gt_pruntbl;
	    while ((pt = *ptnp) != NULL) {
		if (pt->pt_vifi == vifi && pt->pt_router == addr) {
		    NBRM_CLR(pt->pt_index, g->gt_prunes);
		    *ptnp = pt->pt_next;
		    free(pt);
		} else
		    ptnp = &pt->pt_next;
	    }

	    /*
	     * And see if we want to forward again.
	     */
	    if (!VIFM_ISSET(vifi, g->gt_grpmems)) {
		GET_MEMBERSHIP(g, vifi);
		APPLY_SCOPE(g);
		prun_add_ttls(g);

		/* Update kernel state */
		update_kernel(g);
#ifdef RSRR
		/* Send route change notification to reservation protocol. */
		rsrr_cache_send(g,1);
#endif

		/*
		 * If removing this prune causes us to start forwarding
		 * (e.g. the neighbor rebooted), and we sent a prune upstream,
		 * send a graft to cancel the prune.
		 */
		if (!VIFM_ISEMPTY(g->gt_grpmems) && g->gt_prsent_timer)
		    send_graft(g);

		IF_DEBUG(DEBUG_PEER) {
		    logit(LOG_DEBUG, 0, "Reset neighbor state (%s %s) gm:%lx",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
		}
	    }
	}
    }
}

/*
 * Delete table entry from the kernel
 * del_flag determines how many entries to delete
 */
void del_table_entry(struct rtentry *r, uint32_t mcastgrp, uint32_t del_flag)
{
    struct gtable *g, *prev_g;
    struct stable *st, *prev_st;
    struct ptable *pt, *prev_pt;

    if (del_flag == DEL_ALL_ROUTES) {
	g = r->rt_groups;
	while (g) {
	    IF_DEBUG(DEBUG_CACHE) {
		logit(LOG_DEBUG, 0, "del_table_entry() deleting (%s %s)",
		      RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));
	    }

	    st = g->gt_srctbl;
	    while (st) {
		if (st->st_ctime != 0) {
		    if (k_del_rg(st->st_origin, g) < 0) {
			logit(LOG_WARNING, errno, "del_table_entry() trying to delete (%s, %s)",
			      inet_fmt(st->st_origin, s1, sizeof(s1)),
			      inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));
		    }
		    kroutes--;
		}
		prev_st = st;
		st = st->st_next;
		free(prev_st);
	    }
	    g->gt_srctbl = NULL;

	    pt = g->gt_pruntbl;
	    while (pt) {
		prev_pt = pt;
		pt = pt->pt_next;
		free(prev_pt);
	    }
	    g->gt_pruntbl = NULL;

	    if (g->gt_gnext)
		g->gt_gnext->gt_gprev = g->gt_gprev;
	    if (g->gt_gprev)
		g->gt_gprev->gt_gnext = g->gt_gnext;
	    else
		kernel_table = g->gt_gnext;

#ifdef RSRR
	    /* Send route change notification to reservation protocol. */
	    rsrr_cache_send(g,0);
	    rsrr_cache_clean(g);
#endif
	    if (g->gt_rexmit_timer)
		timer_clear(g->gt_rexmit_timer);

	    prev_g = g;
	    g = g->gt_next;
	    free(prev_g);
	}
	r->rt_groups = NULL;
    }

    /*
     * Dummy routine - someday this may be needed, so it is just there
     */
    if (del_flag == DEL_RTE_GROUP) {
	prev_g = (struct gtable *)&r->rt_groups;
	for (g = r->rt_groups; g; g = g->gt_next) {
	    if (g->gt_mcastgrp == mcastgrp) {
		IF_DEBUG(DEBUG_CACHE) {
		    logit(LOG_DEBUG, 0, "del_table_entry() deleting (%s %s)",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));
		}

		st = g->gt_srctbl;
		while (st) {
		    if (st->st_ctime != 0) {
			if (k_del_rg(st->st_origin, g) < 0) {
			    logit(LOG_WARNING, errno, "del_table_entry() trying to delete (%s, %s)",
				  inet_fmt(st->st_origin, s1, sizeof(s1)),
				  inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)));
			}
			kroutes--;
		    }
		    prev_st = st;
		    st = st->st_next;
		    free(prev_st);
		}
		g->gt_srctbl = NULL;

		pt = g->gt_pruntbl;
		while (pt) {
		    prev_pt = pt;
		    pt = pt->pt_next;
		    free(prev_pt);
		}
		g->gt_pruntbl = NULL;

		if (g->gt_gnext)
		    g->gt_gnext->gt_gprev = g->gt_gprev;
		if (g->gt_gprev)
		    g->gt_gprev->gt_gnext = g->gt_gnext;
		else
		    kernel_table = g->gt_gnext;

		if (prev_g != (struct gtable *)&r->rt_groups)
		    g->gt_next->gt_prev = prev_g;
		else
		    g->gt_next->gt_prev = NULL;
		prev_g->gt_next = g->gt_next;

		if (g->gt_rexmit_timer)
		    timer_clear(g->gt_rexmit_timer);
#ifdef RSRR
		/* Send route change notification to reservation protocol. */
		rsrr_cache_send(g,0);
		rsrr_cache_clean(g);
#endif
		free(g);
		g = prev_g;
	    } else {
		prev_g = g;
	    }
	}
    }
}

/*
 * update kernel table entry when a route entry changes
 */
void update_table_entry(struct rtentry *r, uint32_t old_parent_gw)
{
    struct gtable *g;
    struct ptable *pt, **ptnp;

    for (g = r->rt_groups; g; g = g->gt_next) {
	ptnp = &g->gt_pruntbl;
	/*
	 * Delete prune entries from non-children, or non-subordinates.
	 */
	while ((pt = *ptnp)) {
	    if (!VIFM_ISSET(pt->pt_vifi, r->rt_children) ||
		!NBRM_ISSET(pt->pt_index, r->rt_subordinates)) {

		IF_DEBUG(DEBUG_PRUNE) {
		    logit(LOG_DEBUG, 0, "update_table_entry() deleting prune for (%s %s) from %s on vif %u -%s%s",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)),
			  inet_fmt(pt->pt_router, s3, sizeof(s3)), pt->pt_vifi,
			  VIFM_ISSET(pt->pt_vifi, r->rt_children) ? "" : " not a child",
			  NBRM_ISSET(pt->pt_index, r->rt_subordinates) ? "" : " not a subordinate");
		}

		if (!NBRM_ISSET(pt->pt_index, g->gt_prunes)) {
		    logit(LOG_WARNING, 0, "gt_prunes lost track of (%s %s) from %s on vif %u",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)),
			  inet_fmt(pt->pt_router, s3, sizeof(s3)), pt->pt_vifi);
		}

		NBRM_CLR(pt->pt_index, g->gt_prunes);
		*ptnp = pt->pt_next;
		free(pt);
		continue;
	    }
	    ptnp = &((*ptnp)->pt_next);
	}

	IF_DEBUG(DEBUG_CACHE) {
	    logit(LOG_DEBUG, 0, "Updating cache entries (%s %s) old gm:%lx",
		  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
	}

	/*
	 * Forget about a prune or graft that we sent previously if we
	 * have a new parent router (since the new parent router will
	 * know nothing about what I sent to the previous parent).  The
	 * old parent will forget any prune state it is keeping for us.
	 */
	if (old_parent_gw != r->rt_gateway) {
	    g->gt_prsent_timer = 0;
	    g->gt_grftsnt = 0;
	}

	/* Recalculate membership */
	determine_forwvifs(g);
	/* send a prune or graft if needed. */
	send_prune_or_graft(g);

	IF_DEBUG(DEBUG_CACHE) {
	    logit(LOG_DEBUG, 0, "Updating cache entries (%s %s) new gm:%lx",
		  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
	}

	/* update ttls and add entry into kernel */
	prun_add_ttls(g);
	update_kernel(g);
#ifdef RSRR
	/* Send route change notification to reservation protocol. */
	rsrr_cache_send(g,1);
#endif
    }
}

/*
 * set the forwarding flag for all mcastgrps on this vifi
 */
void update_lclgrp(vifi_t vifi, uint32_t mcastgrp)
{
    struct rtentry *r;
    struct gtable *g;

    IF_DEBUG(DEBUG_MEMBER)
	logit(LOG_DEBUG, 0, "Group %s joined on vif %u", inet_fmt(mcastgrp, s1, sizeof(s1)), vifi);

    for (g = kernel_table; g; g = g->gt_gnext) {
	if (ntohl(mcastgrp) < ntohl(g->gt_mcastgrp))
	    break;

	r = g->gt_route;
	if (g->gt_mcastgrp == mcastgrp &&
	    VIFM_ISSET(vifi, r->rt_children)) {

	    VIFM_SET(vifi, g->gt_grpmems);
	    APPLY_SCOPE(g);
	    if (VIFM_ISEMPTY(g->gt_grpmems))
		continue;

	    prun_add_ttls(g);
	    IF_DEBUG(DEBUG_CACHE){
		logit(LOG_DEBUG, 0, "Update lclgrp (%s %s) gm:%lx", RT_FMT(r, s1),
		      inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
	    }

	    update_kernel(g);
#ifdef RSRR
	    /* Send route change notification to reservation protocol. */
	    rsrr_cache_send(g, 1);
#endif
	}
    }
}

/*
 * reset forwarding flag for all mcastgrps on this vifi
 */
void delete_lclgrp(vifi_t vifi, uint32_t mcastgrp)
{
    struct gtable *g;

    IF_DEBUG(DEBUG_MEMBER)
	logit(LOG_DEBUG, 0, "Group %s left on vif %u", inet_fmt(mcastgrp, s1, sizeof(s1)), vifi);

    for (g = kernel_table; g; g = g->gt_gnext) {
	if (ntohl(mcastgrp) < ntohl(g->gt_mcastgrp))
	    break;

	if (g->gt_mcastgrp == mcastgrp && VIFM_ISSET(vifi, g->gt_grpmems)) {
	    if (g->gt_route == NULL ||
		SUBS_ARE_PRUNED(g->gt_route->rt_subordinates, uvifs[vifi].uv_nbrmap, g->gt_prunes)) {
		VIFM_CLR(vifi, g->gt_grpmems);

		if (g->gt_route) {
		    IF_DEBUG(DEBUG_CACHE) {
			logit(LOG_DEBUG, 0, "Delete lclgrp (%s %s) gm:%lx",
			      RT_FMT(g->gt_route, s1),
			      inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
		    }
		}

		prun_add_ttls(g);
		update_kernel(g);
#ifdef RSRR
		/* Send route change notification to reservation protocol. */
		rsrr_cache_send(g, 1);
#endif

		/*
		 * If there are no more members of this particular group,
		 *  send prune upstream
		 */
		if (VIFM_ISEMPTY(g->gt_grpmems) && g->gt_route && g->gt_route->rt_gateway)
		    send_prune(g);
	    }
	}
    }
}

/*
 * Takes the prune message received and then strips it to
 * determine the (src, grp) pair to be pruned.
 *
 * Adds the router to the (src, grp) entry then.
 *
 * Determines if further packets have to be sent down that vif
 *
 * Determines if a corresponding prune message has to be generated
 */
void accept_prune(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
    uint32_t prun_src;
    uint32_t prun_grp;
    uint32_t prun_tmr;
    vifi_t vifi;
    int i;
    struct rtentry *r;
    struct gtable *g;
    struct ptable *pt;
    
    if ((vifi = find_vif(src, dst)) == NO_VIF) {
	logit(LOG_INFO, 0, "Ignoring prune report from non-neighbor %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }
    
    /* Check if enough data is present */
    if (datalen < 12) {
	logit(LOG_INFO, 0, "Non-decipherable prune from %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }
    
    for (i = 0; i< 4; i++)
	((char *)&prun_src)[i] = *p++;
    for (i = 0; i< 4; i++)
	((char *)&prun_grp)[i] = *p++;
    for (i = 0; i< 4; i++)
	((char *)&prun_tmr)[i] = *p++;
    prun_tmr = ntohl(prun_tmr);
    
    if (prun_tmr <= MIN_PRUNE_LIFE) {
	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_INFO, 0, "Ignoring prune from %s on vif %u for (%s %s)/%d, lifetime too short",
		  inet_fmt(src, s1, sizeof(s1)), vifi,
		  inet_fmt(prun_src, s2, sizeof(s2)), inet_fmt(prun_grp, s3, sizeof(s3)), prun_tmr);
	}
	return;
    }

    IF_DEBUG(DEBUG_PRUNE) {
	logit(LOG_DEBUG, 0, "%s on vif %u prunes (%s %s)/%d", inet_fmt(src, s1, sizeof(s1)),
	      vifi, inet_fmt(prun_src, s2, sizeof(s2)),
	      inet_fmt(prun_grp, s3, sizeof(s3)), prun_tmr);
    }

    /*
     * Find the subnet for the prune
     */
    if (find_src_grp(prun_src, 0, prun_grp)) {
	g = gtp ? gtp->gt_gnext : kernel_table;
    	r = g->gt_route;

	IF_DEBUG(DEBUG_PRUNE) {
	    logit(LOG_DEBUG, 0, "Found grp state, (%s %s), metric is %d, children are %lx, subords are %08x%08x",
		  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), r->rt_metric,
		  r->rt_children, r->rt_subordinates.hi, r->rt_subordinates.lo);
	}

	if (!VIFM_ISSET(vifi, r->rt_children)) {
	    IF_DEBUG(DEBUG_PRUNE) {
		logit(LOG_INFO, 0, "Ignoring prune from non-child %s for (%s %s) (dominant on vif %u is %s)",
		      inet_fmt(src, s1, sizeof(s1)), inet_fmt(prun_src, s2, sizeof(s2)),
		      inet_fmt(prun_grp, s3, sizeof(s3)), vifi, inet_fmt(r->rt_dominants[vifi], s4, sizeof(s4)));
	    }
	    return;
	}
	if (VIFM_ISSET(vifi, g->gt_scope)) {
	    logit(LOG_WARNING, 0, "Prune received from %s on scoped grp (%s %s)",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmt(prun_src, s2, sizeof(s2)),
		  inet_fmt(prun_grp, s3, sizeof(s3)));
	    return;
	}
	if ((pt = find_prune_entry(src, g->gt_pruntbl)) != NULL) {
	    IF_DEBUG(DEBUG_PRUNE) {
		logit(LOG_DEBUG, 0, "Duplicate prune received on vif %u from %s for (%s %s)/%d old timer: %d cur gm: %lx",
		      vifi, inet_fmt(src, s1, sizeof(s1)), inet_fmt(prun_src, s2, sizeof(s2)),
		      inet_fmt(prun_grp, s3, sizeof(s3)), prun_tmr, pt->pt_timer, g->gt_grpmems);
	    }
	    pt->pt_timer = prun_tmr;
	} else {
	    struct listaddr *n = neighbor_info(vifi, src);

	    if (!n) {
		logit(LOG_INFO, 0, "Prune from non-neighbor %s on vif %u!?",
		      inet_fmt(src, s1, sizeof(s1)), vifi);
		return;
	    }

	    /* allocate space for the prune structure */
	    pt = calloc(1, sizeof(struct ptable));
	    if (!pt) {
		logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
		return;
	    }

	    pt->pt_vifi = vifi;
	    pt->pt_router = src;
	    pt->pt_timer = prun_tmr;

	    pt->pt_next = g->gt_pruntbl;
	    g->gt_pruntbl = pt;

	    if (n) {
		pt->pt_index = n->al_index;
		NBRM_SET(n->al_index, g->gt_prunes);
	    }
	}

	/*
	 * check if any more packets need to be sent on the 
	 * vif which sent this message
	 */
	if (SUBS_ARE_PRUNED(r->rt_subordinates, uvifs[vifi].uv_nbrmap, g->gt_prunes) &&
	    !grplst_mem(vifi, prun_grp)) {
	    nbrbitmap_t tmp;

	    VIFM_CLR(vifi, g->gt_grpmems);
	    IF_DEBUG(DEBUG_PRUNE) {
		logit(LOG_DEBUG, 0, "vifnbrs=0x%08x%08x, subord=0x%08x%08x prunes=0x%08x%08x",
		      uvifs[vifi].uv_nbrmap.hi,uvifs[vifi].uv_nbrmap.lo,
		      r->rt_subordinates.hi, r->rt_subordinates.lo,
		      g->gt_prunes.hi, g->gt_prunes.lo);
	    }

	    /* XXX debugging */
	    NBRM_COPY(r->rt_subordinates, tmp);
	    NBRM_MASK(tmp, uvifs[vifi].uv_nbrmap);
	    if (!NBRM_ISSETALLMASK(g->gt_prunes, tmp))
		logit(LOG_WARNING, 0, "Subordinate error");
	    /* XXX end debugging */

	    IF_DEBUG(DEBUG_PRUNE|DEBUG_CACHE) {
		logit(LOG_DEBUG, 0, "Prune (%s %s), stop sending on vif %u, gm:%lx",
		      RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), vifi, g->gt_grpmems);
	    }

	    prun_add_ttls(g);
	    update_kernel(g);
#ifdef RSRR
	    /* Send route change notification to reservation protocol. */
	    rsrr_cache_send(g,1);
#endif
	}

	/*
	 * check if all the child routers have expressed no interest
	 * in this group and if this group does not exist in the 
	 * interface
	 * Send a prune message then upstream
	 */
	if (VIFM_ISEMPTY(g->gt_grpmems) && r->rt_gateway)
	    send_prune(g);
    } else {
	/*
	 * There is no kernel entry for this group.  Therefore, we can
	 * simply ignore the prune, as we are not forwarding this traffic
	 * downstream.
	 */
	IF_DEBUG(DEBUG_PRUNE|DEBUG_CACHE) {
	    logit(LOG_DEBUG, 0, "Prune message received with no kernel entry for (%s %s)/%d from %s",
		  inet_fmt(prun_src, s1, sizeof(s1)), inet_fmt(prun_grp, s2, sizeof(s2)),
		  prun_tmr, inet_fmt(src, s3, sizeof(s3)));
	}
	return;
    }
}

/*
 * Checks if this mcastgrp is present in the kernel table
 * If so and if a prune was sent, it sends a graft upwards
 */
void chkgrp_graft(vifi_t vifi, uint32_t mcastgrp)
{
    struct rtentry *r;
    struct gtable *g;

    for (g = kernel_table; g; g = g->gt_gnext) {
	if (ntohl(mcastgrp) < ntohl(g->gt_mcastgrp))
	    break;

	r = g->gt_route;
	if (g->gt_mcastgrp == mcastgrp && VIFM_ISSET(vifi, r->rt_children))
	    if (g->gt_prsent_timer) {
		VIFM_SET(vifi, g->gt_grpmems);

		/*
		 * If the vif that was joined was a scoped vif,
		 * ignore it ; don't graft back
		 */
		APPLY_SCOPE(g);
		if (VIFM_ISEMPTY(g->gt_grpmems))
		    continue;

		/* send graft upwards */
		send_graft(g);

		/* update cache timer*/
		g->gt_timer = CACHE_LIFETIME(cache_lifetime);

		IF_DEBUG(DEBUG_PRUNE|DEBUG_CACHE) {
		    logit(LOG_DEBUG, 0, "chkgrp graft (%s %s) gm:%lx",
			  RT_FMT(r, s1), inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
		}

		prun_add_ttls(g);
		update_kernel(g);
#ifdef RSRR
		/* Send route change notification to reservation protocol. */
		rsrr_cache_send(g, 1);
#endif
	    }
    }
}

/* determine the multicast group and src
 *
 * if it does, then determine if a prune was sent upstream.  if prune
 * sent upstream, send graft upstream and send ack downstream.
 *
 * if no prune sent upstream, change the forwarding bit for this
 * interface and send ack downstream.
 *
 * if no entry exists for this group send ack downstream.
 */
void accept_graft(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
    vifi_t 	vifi;
    uint32_t 	graft_src;
    uint32_t	graft_grp;
    int 	i;
    struct rtentry *r;
    struct gtable *g;
    struct ptable *pt, **ptnp;

    if (datalen < 8) {
	logit(LOG_INFO, 0, "Received non-decipherable graft from %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    for (i = 0; i< 4; i++)
	((char *)&graft_src)[i] = *p++;
    for (i = 0; i< 4; i++)
	((char *)&graft_grp)[i] = *p++;

    vifi = find_vif(src, dst);
    send_graft_ack(dst, src, graft_src, graft_grp, vifi);

    if (vifi == NO_VIF) {
	logit(LOG_INFO, 0, "Ignoring graft for (%s %s) from non-neighbor %s",
	      inet_fmt(graft_src, s2, sizeof(s2)), inet_fmt(graft_grp, s3, sizeof(s3)),
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    IF_DEBUG(DEBUG_PRUNE) {
	logit(LOG_DEBUG, 0, "%s on vif %u grafts (%s %s)", inet_fmt(src, s1, sizeof(s1)), vifi, 
	      inet_fmt(graft_src, s2, sizeof(s2)), inet_fmt(graft_grp, s3, sizeof(s3)));
    }

    /*
     * Find the subnet for the graft
     */
    if (find_src_grp(graft_src, 0, graft_grp)) {
	g = gtp ? gtp->gt_gnext : kernel_table;
	r = g->gt_route;

	if (VIFM_ISSET(vifi, g->gt_scope)) {
	    logit(LOG_WARNING, 0, "Graft received from %s on scoped grp (%s %s)",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmt(graft_src, s2, sizeof(s2)),
		  inet_fmt(graft_grp, s3, sizeof(s3)));
	    return;
	}

	ptnp = &g->gt_pruntbl;
	while ((pt = *ptnp) != NULL) {
	    if ((pt->pt_vifi == vifi) && (pt->pt_router == src)) {
		NBRM_CLR(pt->pt_index, g->gt_prunes);
		*ptnp = pt->pt_next;
		free(pt);

		VIFM_SET(vifi, g->gt_grpmems);
		IF_DEBUG(DEBUG_PRUNE|DEBUG_CACHE) {
		    logit(LOG_DEBUG, 0, "Accept graft (%s %s) gm:%lx", RT_FMT(r, s1),
			  inet_fmt(g->gt_mcastgrp, s2, sizeof(s2)), g->gt_grpmems);
		}

		prun_add_ttls(g);
		update_kernel(g);
#ifdef RSRR
		/* Send route change notification to reservation protocol. */
		rsrr_cache_send(g,1);
#endif
		break;
	    } else {
		ptnp = &pt->pt_next;
	    }
	}

	g->gt_timer = CACHE_LIFETIME(cache_lifetime);
	if (g->gt_prsent_timer)
	    /* send graft upwards */
	    send_graft(g);
    } else {
	/*
	 * We have no state for the source and group in question.
	 * This is fine, since we know that we have no prune state, and
	 * grafts are requests to remove prune state.
	 */
	IF_DEBUG(DEBUG_PRUNE){
	    logit(LOG_DEBUG, 0, "Graft received with no kernel entry for (%s %s) from %s",
		  inet_fmt(graft_src, s1, sizeof(s1)), inet_fmt(graft_grp, s2, sizeof(s2)),
		  inet_fmt(src, s3, sizeof(s3)));
	}
	return;
    }
}

/*
 * find out which group is involved first of all then determine if a
 * graft was sent.  if no graft sent, ignore the message if graft was
 * sent and the ack is from the right source, remove the graft timer so
 * that we don't have send a graft again
 */
void accept_g_ack(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
    struct gtable *g;
    vifi_t 	vifi;
    uint32_t 	grft_src;
    uint32_t	grft_grp;
    int 	i;

    if ((vifi = find_vif(src, dst)) == NO_VIF) {
	logit(LOG_INFO, 0, "Ignoring graft ack from non-neighbor %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    if (datalen > 8) {
	logit(LOG_INFO, 0, "Received non-decipherable graft ack from %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    for (i = 0; i< 4; i++)
	((char *)&grft_src)[i] = *p++;
    for (i = 0; i< 4; i++)
	((char *)&grft_grp)[i] = *p++;

    IF_DEBUG(DEBUG_PRUNE) {
	logit(LOG_DEBUG, 0, "%s on vif %u acks graft (%s, %s)", inet_fmt(src, s1, sizeof(s1)),
	      vifi, inet_fmt(grft_src, s2, sizeof(s2)), inet_fmt(grft_grp, s3, sizeof(s3)));
    }

    /*
     * Find the subnet for the graft ack
     */
    if (find_src_grp(grft_src, 0, grft_grp)) {
	g = gtp ? gtp->gt_gnext : kernel_table;
	g->gt_grftsnt = 0;
    } else {
	logit(LOG_WARNING, 0, "Received graft ACK with no kernel entry for (%s, %s) from %s",
	      inet_fmt(grft_src, s1, sizeof(s1)), inet_fmt(grft_grp, s2, sizeof(s2)),
	      inet_fmt(src, s3, sizeof(s3)));
	return;
    }
}


/*
 * free all prune entries and kernel routes
 * normally, this should inform the kernel that all of its routes
 * are going away, but this is only called by restart(), which is
 * about to call MRT_DONE which does that anyway.
 */
void free_all_prunes(void)
{
    struct rtentry *r;
    struct gtable *g, *prev_g;
    struct stable *s, *prev_s;
    struct ptable *p, *prev_p;

    for (r = routing_table; r; r = r->rt_next) {
	g = r->rt_groups;
	while (g) {
	    s = g->gt_srctbl;
	    while (s) {
		prev_s = s;
		s = s->st_next;
		free(prev_s);
	    }

	    p = g->gt_pruntbl;
	    while (p) {
		prev_p = p;
		p = p->pt_next;
		free(prev_p);
	    }

	    prev_g = g;
	    g = g->gt_next;
	    if (prev_g->gt_rexmit_timer)
		timer_clear(prev_g->gt_rexmit_timer);
	    free(prev_g);
	}
	r->rt_groups = NULL;
    }
    kernel_table = NULL;

    g = kernel_no_route;
    while (g) {
	if (g->gt_srctbl)
	    free(g->gt_srctbl);

	prev_g = g;
	g = g->gt_next;
	if (prev_g->gt_rexmit_timer)
	    timer_clear(prev_g->gt_rexmit_timer);
	free(prev_g);
    }
    kernel_no_route = NULL;
}

/*
 * When a new route is created, search
 * a) The less-specific part of the routing table
 * b) The route-less kernel table
 * for sources that the new route might want to handle.
 *
 * "Inheriting" these sources might be cleanest, but simply deleting
 * them is easier, and letting the kernel re-request them.
 */
void steal_sources(struct rtentry *rt)
{
    struct rtentry *rp;
    struct gtable *gt, **gtnp;
    struct stable *st, **stnp;

    for (rp = rt->rt_next; rp; rp = rp->rt_next) {
	if (rp->rt_groups == NULL)
	    continue;
	if ((rt->rt_origin & rp->rt_originmask) == rp->rt_origin) {
	    IF_DEBUG(DEBUG_ROUTE) {
		logit(LOG_DEBUG, 0, "Route for %s stealing sources from %s",
		      RT_FMT(rt, s1), RT_FMT(rp, s2));
	    }

	    for (gt = rp->rt_groups; gt; gt = gt->gt_next) {
		stnp = &gt->gt_srctbl;
		while ((st = *stnp) != NULL) {
		    if ((st->st_origin & rt->rt_originmask) == rt->rt_origin) {
			IF_DEBUG(DEBUG_ROUTE) {
			    logit(LOG_DEBUG, 0, "%s stealing (%s %s) from %s",
				  RT_FMT(rt, s1), inet_fmt(st->st_origin, s3, sizeof(s3)),
				  inet_fmt(gt->gt_mcastgrp, s4, sizeof(s4)), RT_FMT(rp, s2));
			}

			if (st->st_ctime != 0) {
			    if (k_del_rg(st->st_origin, gt) < 0) {
				logit(LOG_WARNING, errno, "steal_sources() trying to delete (%s, %s)",
				      inet_fmt(st->st_origin, s1, sizeof(s1)),
				      inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
			    }
			    kroutes--;
			}
			*stnp = st->st_next;
			free(st);
		    } else {
			stnp = &st->st_next;
		    }
		}
	    }
	}
    }

    gtnp = &kernel_no_route;
    while ((gt = *gtnp) != NULL) {
	if (gt->gt_srctbl &&
	    (gt->gt_srctbl->st_origin & rt->rt_originmask) == rt->rt_origin) {
	    IF_DEBUG(DEBUG_ROUTE) {
		logit(LOG_DEBUG, 0, "%s stealing (%s %s) from no_route table", RT_FMT(rt, s1), 
		      inet_fmt(gt->gt_srctbl->st_origin, s3, sizeof(s3)),
		      inet_fmt(gt->gt_mcastgrp, s4, sizeof(s4)));
	    }

	    if (gt->gt_srctbl->st_ctime != 0) {
		if (k_del_rg(gt->gt_srctbl->st_origin, gt) < 0) {
		    logit(LOG_WARNING, errno, "steal_sources() trying to delete (%s %s)",
			  inet_fmt(gt->gt_srctbl->st_origin, s1, sizeof(s1)),
			  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		}
		kroutes--;
	    }
	    free(gt->gt_srctbl);
	    *gtnp = gt->gt_next;
	    if (gt->gt_next)
		gt->gt_next->gt_prev = gt->gt_prev;
	    if (gt->gt_rexmit_timer)
		timer_clear(gt->gt_rexmit_timer);
	    free(gt);
	} else {
	    gtnp = &gt->gt_next;
	}
    }
}

/*
 * Advance the timers on all the cache entries.
 * If there are any entries whose timers have expired,
 * remove these entries from the kernel cache.
 */
void age_table_entry(void)
{
    struct rtentry *r;
    struct gtable *gt, **gtnptr;
    struct stable *st, **stnp;
    struct ptable *pt, **ptnp;
    struct sioc_sg_req sg_req;
    
    IF_DEBUG(DEBUG_PRUNE|DEBUG_CACHE) {
	logit(LOG_DEBUG, 0, "Aging forwarding cache entries");
    }
    
    gtnptr = &kernel_table;
    while ((gt = *gtnptr) != NULL) {
	vifi_t i; /* XXX Debugging */
	int fixit = 0; /* XXX Debugging */

	r = gt->gt_route;

	/* XXX Debugging... */
	for (i = 0; i < numvifs; i++) {
	    /*
	     * If we're not sending on this vif,
	     * And this group isn't scoped on this vif,
	     * And I'm the parent for this route on this vif,
	     * And there are subordinates on this vif,
	     * And all of the subordinates haven't pruned,
	     *		YELL LOUDLY
	     *		and remember to fix it up later
	     */
	    if (!VIFM_ISSET(i, gt->gt_grpmems) &&
		!VIFM_ISSET(i, gt->gt_scope) &&
		VIFM_ISSET(i, r->rt_children) &&
		NBRM_ISSETMASK(uvifs[i].uv_nbrmap, r->rt_subordinates) &&
		!SUBS_ARE_PRUNED(r->rt_subordinates, uvifs[i].uv_nbrmap, gt->gt_prunes)) {
		logit(LOG_WARNING, 0, "(%s %s) is blackholing on vif %u",
		      RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), i);
		fixit = 1;
	    }
	}
	if (fixit) {
	    logit(LOG_WARNING, 0, "Fixing membership for (%s %s) gm:%lx",
		  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), gt->gt_grpmems);
	    determine_forwvifs(gt);
	    send_prune_or_graft(gt);
	    logit(LOG_WARNING, 0, "Fixed  membership for (%s %s) gm:%lx",
		  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), gt->gt_grpmems);
	}
	/*DEBUG2*/
	/* If there are group members,
	 * and there are recent sources,
	 * and we have a route,
	 * and it's not directly connected,
	 * and we haven't sent a prune,
	 *	if there are any cache entries in the kernel
	 *	 [if there aren't we're probably waiting to rexmit],
	 *		YELL LOUDLY
	 *		and send a prune
	 */
	if (VIFM_ISEMPTY(gt->gt_grpmems) && gt->gt_srctbl && r && r->rt_gateway && gt->gt_prsent_timer == 0) {
	    for (st = gt->gt_srctbl; st; st = st->st_next) {
		if (st->st_ctime != 0)
		    break;
	    }

	    if (st != NULL) {
		logit(LOG_WARNING, 0, "Group members for (%s %s) is empty but no prune state!",
		      RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		send_prune_or_graft(gt);
	    }
	}
	/* XXX ...Debugging */

	/* advance the timer for the kernel entry */
	gt->gt_timer -= TIMER_INTERVAL;

	/* decrement prune timer if need be */
	if (gt->gt_prsent_timer > 0) {
	    gt->gt_prsent_timer -= TIMER_INTERVAL;
	    if (gt->gt_prsent_timer <= 0) {
		IF_DEBUG(DEBUG_PRUNE) {
		    logit(LOG_DEBUG, 0, "Upstream prune tmo (%s %s)", RT_FMT(r, s1),
			  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		}

		/* Reset the prune retransmission timer to its initial value */
		gt->gt_prune_rexmit = PRUNE_REXMIT_VAL;
		gt->gt_prsent_timer = -1;
	    }
	}

	/* retransmit graft with exponential backoff */
	if (gt->gt_grftsnt) {
	    int y;

	    y = ++gt->gt_grftsnt;
	    while (y && !(y & 1))
		y >>= 1;
	    if (y == 1)
		send_graft(gt);
	}

	/*
	 * Age prunes
	 *
	 * If a prune expires, forward again on that vif.
	 */
	ptnp = &gt->gt_pruntbl;
	while ((pt = *ptnp) != NULL) {
	    if ((pt->pt_timer -= TIMER_INTERVAL) <= 0) {
		IF_DEBUG(DEBUG_PRUNE) {
		    logit(LOG_DEBUG, 0, "Expire prune (%s %s) from %s on vif %u", 
			  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
			  inet_fmt(pt->pt_router, s3, sizeof(s3)), pt->pt_vifi);
		}

		if (gt->gt_prsent_timer > 0) {
		    logit(LOG_WARNING, 0, "Prune (%s %s) from %s on vif %u expires with %d left on prsent timer",
			  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
			  inet_fmt(pt->pt_router, s3, sizeof(s3)), pt->pt_vifi, gt->gt_prsent_timer);

		    /* Send a graft to heal the tree. */
		    send_graft(gt);
		}

		NBRM_CLR(pt->pt_index, gt->gt_prunes);
		expire_prune(pt->pt_vifi, gt);

		/* remove the router's prune entry and await new one */
		*ptnp = pt->pt_next;
		free(pt);
	    } else {
		ptnp = &pt->pt_next;
	    }
	}

	/*
	 * If the cache entry has expired, delete source table entries for
	 * silent sources.  If there are no source entries left, and there
	 * are no downstream prunes, then the entry is deleted.
	 * Otherwise, the cache entry's timer is refreshed.
	 */
	if (gt->gt_timer <= 0) {
	    IF_DEBUG(DEBUG_CACHE) {
		logit(LOG_DEBUG, 0, "(%s %s) timed out, checking for traffic",
		      RT_FMT(gt->gt_route, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
	    }
	    /* Check for traffic before deleting source entries */
	    sg_req.grp.s_addr = gt->gt_mcastgrp;
	    stnp = &gt->gt_srctbl;
	    while ((st = *stnp) != NULL) {
		/*
		 * Source entries with no ctime are not actually in the
		 * kernel; they have been removed by rexmit_prune() so
		 * are safe to remove from the list at this point.
		 */
		if (st->st_ctime) {
		    sg_req.src.s_addr = st->st_origin;
		    if (ioctl(udp_socket, SIOCGETSGCNT, (char *)&sg_req) < 0) {
			logit(LOG_WARNING, errno, "age_table_entry() Failed ioctl SIOCGETSGCNT for (%s %s)",
			      inet_fmt(st->st_origin, s1, sizeof(s1)),
			      inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));

			/* Make sure it gets deleted below */
			sg_req.pktcnt = st->st_pktcnt;
		    }
		} else {
		    sg_req.pktcnt = st->st_pktcnt;
		}
		if (sg_req.pktcnt == st->st_pktcnt) {
		    *stnp = st->st_next;
		    IF_DEBUG(DEBUG_CACHE) {
			logit(LOG_DEBUG, 0, "age_table_entry() deleting (%s %s)",
			      inet_fmt(st->st_origin, s1, sizeof(s1)),
			      inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		    }
		    if (st->st_ctime != 0) {
			if (k_del_rg(st->st_origin, gt) < 0) {
			    logit(LOG_WARNING, errno, "age_table_entry() trying to delete (%s %s)",
				  inet_fmt(st->st_origin, s1, sizeof(s1)),
				  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
			}
			kroutes--;
		    }
		    free(st);
		} else {
		    st->st_pktcnt = sg_req.pktcnt;
		    stnp = &st->st_next;
		}
	    }

	    /*
	     * Retain the group entry if we have downstream prunes or if
	     * there is at least one source in the list that still has
	     * traffic, or if our upstream prune timer or graft
	     * retransmission timer is running.
	     */
	    if (gt->gt_pruntbl != NULL || gt->gt_srctbl != NULL ||
		gt->gt_prsent_timer > 0 || gt->gt_grftsnt > 0) {
		IF_DEBUG(DEBUG_CACHE) {
		    logit(LOG_DEBUG, 0, "Refresh lifetime of cache entry %s%s%s%s(%s, %s)",
			  gt->gt_pruntbl          ? "(dstrm prunes) " : "",
			  gt->gt_srctbl           ? "(trfc flow) "    : "",
			  gt->gt_prsent_timer > 0 ? "(upstrm prune) " : "",
			  gt->gt_grftsnt > 0      ? "(grft rexmit) "  : "",
			  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		}
		gt->gt_timer = CACHE_LIFETIME(cache_lifetime);
		if (gt->gt_prsent_timer == -1) {
		    /*
		     * The upstream prune timed out.  Remove any kernel
		     * state.
		     */
		    gt->gt_prsent_timer = 0;
		    if (gt->gt_pruntbl) {
			logit(LOG_WARNING, 0, "Upstream prune for (%s %s) expires with downstream prunes active",
			      RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		    }
		    remove_sources(gt);
		}
		gtnptr = &gt->gt_gnext;
		continue;
	    }

	    IF_DEBUG(DEBUG_CACHE){
		logit(LOG_DEBUG, 0, "Timeout cache entry (%s, %s)",
		      RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
	    }

	    if (gt->gt_prev)
		gt->gt_prev->gt_next = gt->gt_next;
	    else
		gt->gt_route->rt_groups = gt->gt_next;
	    if (gt->gt_next)
		gt->gt_next->gt_prev = gt->gt_prev;

	    if (gt->gt_gprev) {
		gt->gt_gprev->gt_gnext = gt->gt_gnext;
		gtnptr = &gt->gt_gprev->gt_gnext;
	    } else {
		kernel_table = gt->gt_gnext;
		gtnptr = &kernel_table;
	    }
	    if (gt->gt_gnext)
		gt->gt_gnext->gt_gprev = gt->gt_gprev;

#ifdef RSRR
	    /* Send route change notification to reservation protocol. */
	    rsrr_cache_send(gt,0);
	    rsrr_cache_clean(gt);
#endif
	    if (gt->gt_rexmit_timer)
		timer_clear(gt->gt_rexmit_timer);

	    free(gt);
	} else {
	    if (gt->gt_prsent_timer == -1) {
		/*
		 * The upstream prune timed out.  Remove any kernel
		 * state.
		 */
		gt->gt_prsent_timer = 0;
		if (gt->gt_pruntbl) {
		    logit(LOG_WARNING, 0, "Upstream prune for (%s %s) expires with downstream prunes active",
			  RT_FMT(r, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		}
		remove_sources(gt);
	    }
	    gtnptr = &gt->gt_gnext;
	}
    }

    /*
     * When traversing the no_route table, the decision is much easier.
     * Just delete it if it has timed out.
     */
    gtnptr = &kernel_no_route;
    while ((gt = *gtnptr) != NULL) {
	/* advance the timer for the kernel entry */
	gt->gt_timer -= TIMER_INTERVAL;

	if (gt->gt_timer < 0) {
	    if (gt->gt_srctbl) {
		if (gt->gt_srctbl->st_ctime != 0) {
		    if (k_del_rg(gt->gt_srctbl->st_origin, gt) < 0) {
			logit(LOG_WARNING, errno, "age_table_entry() trying to delete no-route (%s %s)",
			      inet_fmt(gt->gt_srctbl->st_origin, s1, sizeof(s1)),
			      inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		    }
		    kroutes--;
		}
		free(gt->gt_srctbl);
	    }
	    *gtnptr = gt->gt_next;
	    if (gt->gt_next)
		gt->gt_next->gt_prev = gt->gt_prev;

	    if (gt->gt_rexmit_timer)
		timer_clear(gt->gt_rexmit_timer);

	    free(gt);
	} else {
	    gtnptr = &gt->gt_next;
	}
    }
}

/*
 * Modify the kernel to forward packets when one or multiple prunes that
 * were received on the vif given by vifi, for the group given by gt,
 * have expired.
 */
static void expire_prune(vifi_t vifi, struct gtable *gt)
{
    /*
     * No need to send a graft, any prunes that we sent
     * will expire before any prunes that we have received.
     * However, in the case that we did make a mistake,
     * send a graft to compensate.
     */
    if (gt->gt_prsent_timer >= MIN_PRUNE_LIFE) {
	IF_DEBUG(DEBUG_PRUNE)
	    logit(LOG_DEBUG, 0, "Prune expired with %d left on prsent_timer", gt->gt_prsent_timer);

        gt->gt_prsent_timer = 0;
	send_graft(gt);
    }

    /* modify the kernel entry to forward packets */
    if (!VIFM_ISSET(vifi, gt->gt_grpmems)) {
        struct rtentry *rt = gt->gt_route;

        VIFM_SET(vifi, gt->gt_grpmems);
	IF_DEBUG(DEBUG_CACHE) {
	    logit(LOG_DEBUG, 0, "Forwarding again (%s %s) gm:%lx vif:%d",
		  RT_FMT(rt, s1), inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), gt->gt_grpmems, vifi);
	}

        prun_add_ttls(gt);
        update_kernel(gt);
#ifdef RSRR
        /* Send route change notification to reservation protocol. */
        rsrr_cache_send(gt,1);
#endif
    }
}

/*
 * Print the contents of the cache table on file 'fp'.
 */
void dump_cache(FILE *fp, int detail)
{
    struct rtentry *r;
    struct gtable *gt;
    struct stable *st;
    struct ptable *pt;
    vifi_t i;
    char c;
    time_t thyme = time(NULL);

    if (detail)
	fprintf(fp, "Multicast Routing Cache Table (%d entries)\n", kroutes);
    fprintf(fp,
	    " Origin             Mcast-group         CTmr     Age      Ptmr Rx IVif Forwvifs=\n");
    fprintf(fp,
	    "<(prunesrc:vif[idx]/tmr) prunebitmap\n%s",
	    ">Source             Lifetime SavPkt         Pkts    Bytes RPFf\n");

    for (gt = kernel_no_route; gt; gt = gt->gt_next) {
	if (gt->gt_srctbl) {
	    fprintf(fp, " %-18s %-15s %-8s %-8s        - -1 (no route)\n",
		inet_fmts(gt->gt_srctbl->st_origin, 0xffffffff, s1, sizeof(s1)),
		inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)), scaletime(gt->gt_timer),
		scaletime(thyme - gt->gt_ctime));
	    fprintf(fp, ">%s\n", inet_fmt(gt->gt_srctbl->st_origin, s1, sizeof(s1)));
	}
    }

    for (gt = kernel_table; gt; gt = gt->gt_gnext) {
	r = gt->gt_route;
	fprintf(fp, " %-18s %-15s",
	    RT_FMT(r, s1),
	    inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));

	fprintf(fp, " %-8s", scaletime(gt->gt_timer));

	fprintf(fp, " %-8s %-8s ", scaletime(thyme - gt->gt_ctime),
		gt->gt_prsent_timer
		? scaletime(gt->gt_prsent_timer)
		: "       -");

	if (gt->gt_prune_rexmit) {
	    int i = gt->gt_prune_rexmit;
	    int n = 0;

	    while (i > PRUNE_REXMIT_VAL) {
		n++;
		i /= 2;
	    }
	    if (n == 0 && gt->gt_prsent_timer == 0)
		fprintf(fp, " -");
	    else
		fprintf(fp, "%2d", n);
	} else {
	    fprintf(fp, " -");
	}

	fprintf(fp, " %2u%c%c", r->rt_parent,
	    gt->gt_prsent_timer ? 'P' :
	   			  gt->gt_grftsnt ? 'G' : ' ',
	    VIFM_ISSET(r->rt_parent, gt->gt_scope) ? 'B' : ' ');

	for (i = 0; i < numvifs; ++i) {
	    if (VIFM_ISSET(i, gt->gt_grpmems))
		fprintf(fp, " %u ", i);
	    else if (VIFM_ISSET(i, r->rt_children) &&
		     NBRM_ISSETMASK(uvifs[i].uv_nbrmap, r->rt_subordinates))
		fprintf(fp, " %u%c", i,
			VIFM_ISSET(i, gt->gt_scope) ? 'b' : 
			SUBS_ARE_PRUNED(r->rt_subordinates,
			    uvifs[i].uv_nbrmap, gt->gt_prunes) ? 'p' : '!');
	}
	fprintf(fp, "\n");
	if (gt->gt_pruntbl) {
	    fprintf(fp, "<");
	    c = '(';
	    for (pt = gt->gt_pruntbl; pt; pt = pt->pt_next) {
		fprintf(fp, "%c%s:%d[%d]/%d", c, inet_fmt(pt->pt_router, s1, sizeof(s1)),
		    pt->pt_vifi, pt->pt_index, pt->pt_timer);
		c = ',';
	    }
	    fprintf(fp, ")");
	    fprintf(fp, " 0x%08x%08x\n", gt->gt_prunes.hi, gt->gt_prunes.lo);
	}
	for (st = gt->gt_srctbl; st; st = st->st_next) {
	    fprintf(fp, ">%-18s %-8s %6u", inet_fmt(st->st_origin, s1, sizeof(s1)),
		st->st_ctime ? scaletime(thyme - st->st_ctime) : "-",
		st->st_savpkt);
	    if (st->st_ctime) {
		struct sioc_sg_req sg_req;

		sg_req.src.s_addr = st->st_origin;
		sg_req.grp.s_addr = gt->gt_mcastgrp;
		if (ioctl(udp_socket, SIOCGETSGCNT, (char *)&sg_req) < 0) {
		    logit(LOG_WARNING, errno, "dump_cache() Failed ioctl SIOCGETSGCNT on (%s %s)",
			  inet_fmt(st->st_origin, s1, sizeof(s1)),
			  inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
		} else {
		    fprintf(fp, "     %8ld %8ld %4ld", sg_req.pktcnt,
			    sg_req.bytecnt, sg_req.wrong_if);
		}
	    }
	    fprintf(fp, "\n");
	}
    }
}

/*
 * Traceroute function which returns traceroute replies to the requesting
 * router. Also forwards the request to downstream routers.
 */
void accept_mtrace(uint32_t src, uint32_t dst, uint32_t group, char *data, uint8_t no, size_t datalen)
{
    uint8_t type;
    struct rtentry *rt;
    struct gtable *gt;
    struct tr_query *qry;
    struct tr_resp  *resp;
    int vifi;
    uint8_t *p;
    size_t rcount;
    int errcode = TR_NO_ERR;
    int resptype;
    struct timeval tp;
    struct sioc_vif_req v_req;
    struct sioc_sg_req sg_req;

    /* Remember qid across invocations */
    static uint32_t oqid = 0;

    /* timestamp the request/response */
    gettimeofday(&tp, 0);

    /*
     * Check if it is a query or a response
     */
    if (datalen == QLEN) {
	type = QUERY;
	IF_DEBUG(DEBUG_TRACE) {
	    logit(LOG_DEBUG, 0, "Initial traceroute query rcvd from %s to %s",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
	}
    } else if ((datalen - QLEN) % RLEN == 0) {
	type = RESP;
	IF_DEBUG(DEBUG_TRACE) {
	    logit(LOG_DEBUG, 0, "In-transit traceroute query rcvd from %s to %s",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
	}

	if (IN_MULTICAST(ntohl(dst))) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Dropping multicast response");
	    }
	    return;
	}
    } else {
	logit(LOG_INFO, 0, "Non decipherable traceroute request received from %s to %s",
	      inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
	return;
    }

    /*
     * if it is a packet with all reports filled, drop it
     */
    if ((rcount = (datalen - QLEN)/RLEN) == no) {
	IF_DEBUG(DEBUG_TRACE) {
	    logit(LOG_DEBUG, 0, "Packet with all reports filled in");
	}
	return;
    }

    qry = (struct tr_query *)data;

    IF_DEBUG(DEBUG_TRACE) {
	logit(LOG_DEBUG, 0, "s: %s g: %s d: %s ", inet_fmt(qry->tr_src, s1, sizeof(s1)),
	      inet_fmt(group, s2, sizeof(s2)), inet_fmt(qry->tr_dst, s3, sizeof(s3)));
	logit(LOG_DEBUG, 0, "rttl: %d rd: %s", qry->tr_rttl,
	      inet_fmt(qry->tr_raddr, s1, sizeof(s1)));
	logit(LOG_DEBUG, 0, "rcount:%zu, qid:%06x", rcount, qry->tr_qid);
    }

    /* determine the routing table entry for this traceroute */
    rt = determine_route(qry->tr_src);
    IF_DEBUG(DEBUG_TRACE) {
	if (rt) {
	    logit(LOG_DEBUG, 0, "rt parent vif: %d rtr: %s metric: %d",
		  rt->rt_parent, inet_fmt(rt->rt_gateway, s1, sizeof(s1)), rt->rt_metric);
	    logit(LOG_DEBUG, 0, "rt origin %s", RT_FMT(rt, s1));
	} else
	    logit(LOG_DEBUG, 0, "...no route");
    }

    /*
     * Query type packet - check if rte exists 
     * Check if the query destination is a vif connected to me.
     * and if so, whether I should start response back
     */
    if (type == QUERY) {
	if (oqid == qry->tr_qid) {
	    /*
	     * If the multicast router is a member of the group being
	     * queried, and the query is multicasted, then the router can
	     * receive multiple copies of the same query.  If we have already
	     * replied to this traceroute, just ignore it this time.
	     *
	     * This is not a total solution, but since if this fails you
	     * only get N copies, N <= the number of interfaces on the router,
	     * it is not fatal.
	     */
	    IF_DEBUG(DEBUG_TRACE)
		logit(LOG_DEBUG, 0, "Ignoring duplicate traceroute packet");

	    return;
	}

	if (rt == NULL) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Mcast traceroute: no route entry %s",
		      inet_fmt(qry->tr_src, s1, sizeof(s1)));
	    }
	    if (IN_MULTICAST(ntohl(dst)))
		return;
	}
	vifi = find_vif(qry->tr_dst, 0);

	if (vifi == NO_VIF) {
	    /* The traceroute destination is not on one of my subnet vifs. */
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Destination %s not an interface",
		      inet_fmt(qry->tr_dst, s1, sizeof(s1)));
	    }
	    if (IN_MULTICAST(ntohl(dst)))
		return;
	    errcode = TR_WRONG_IF;
	} else if (rt != NULL && !VIFM_ISSET(vifi, rt->rt_children)) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Destination %s not on forwarding tree for src %s",
		      inet_fmt(qry->tr_dst, s1, sizeof(s1)), inet_fmt(qry->tr_src, s2, sizeof(s2)));
	    }
	    if (IN_MULTICAST(ntohl(dst)))
		return;
	    errcode = TR_WRONG_IF;
	}
    } else {
	/*
	 * determine which interface the packet came in on
	 * RESP packets travel hop-by-hop so this either traversed
	 * a tunnel or came from a directly attached mrouter.
	 */
	vifi = find_vif(src, dst);
	if (vifi == NO_VIF) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Wrong interface for packet");
	    }
	    errcode = TR_WRONG_IF;
	}
    }

    /* Now that we've decided to send a response, save the qid */
    oqid = qry->tr_qid;

    IF_DEBUG(DEBUG_TRACE)
	logit(LOG_DEBUG, 0, "Sending traceroute response");

    /* copy the packet to the sending buffer */
    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;
    memmove(p, data, datalen);
    p += datalen;

    /*
     * If there is no room to insert our reply, coopt the previous hop
     * error indication to relay this fact.
     */
    if (p + sizeof(struct tr_resp) > send_buf + RECV_BUF_SIZE) {
	resp = (struct tr_resp *)p - 1;
	resp->tr_rflags = TR_NO_SPACE;
	rt = NULL;
	goto sendit;
    }

    /*
     * fill in initial response fields
     */
    resp = (struct tr_resp *)p;
    memset(resp, 0, sizeof(struct tr_resp));
    datalen += RLEN;

    resp->tr_qarr    = htonl(((tp.tv_sec + JAN_1970) << 16) +
			     ((tp.tv_usec << 10) / 15625));
    resp->tr_rproto  = PROTO_DVMRP;
    resp->tr_outaddr = (vifi == NO_VIF) ? dst : uvifs[vifi].uv_lcl_addr;
    resp->tr_fttl    = (vifi == NO_VIF) ? 0 : uvifs[vifi].uv_threshold;
    resp->tr_rflags  = errcode;

    /*
     * obtain # of packets out on interface
     */
    v_req.vifi = vifi;
    if (vifi != NO_VIF && ioctl(udp_socket, SIOCGETVIFCNT, (char *)&v_req) >= 0)
	resp->tr_vifout  =  htonl(v_req.ocount);
    else
	resp->tr_vifout  =  0xffffffff;

    /*
     * fill in scoping & pruning information
     */
    if (rt) {
	for (gt = rt->rt_groups; gt; gt = gt->gt_next) {
	    if (gt->gt_mcastgrp >= group)
		break;
	}
    } else {
	gt = NULL;
    }

    if (gt && gt->gt_mcastgrp == group) {
	struct stable *st;

	for (st = gt->gt_srctbl; st; st = st->st_next) {
	    if (qry->tr_src == st->st_origin)
		break;
	}

	sg_req.src.s_addr = qry->tr_src;
	sg_req.grp.s_addr = group;
	if (st && st->st_ctime != 0 &&
	    ioctl(udp_socket, SIOCGETSGCNT, (char *)&sg_req) >= 0)
	    resp->tr_pktcnt = htonl(sg_req.pktcnt + st->st_savpkt);
	else
	    resp->tr_pktcnt = htonl(st ? st->st_savpkt : 0xffffffff);

	if (vifi != NO_VIF && VIFM_ISSET(vifi, gt->gt_scope)) {
	    resp->tr_rflags = TR_SCOPED;
	} else if (gt->gt_prsent_timer) {
	    resp->tr_rflags = TR_PRUNED;
	} else if (vifi != NO_VIF && !VIFM_ISSET(vifi, gt->gt_grpmems)) {
	    if (!NBRM_ISEMPTY(uvifs[vifi].uv_nbrmap) &&
		SUBS_ARE_PRUNED(rt->rt_subordinates,
				uvifs[vifi].uv_nbrmap, gt->gt_prunes))
		resp->tr_rflags = TR_OPRUNED;
	    else
		resp->tr_rflags = TR_NO_FWD;
	}
    } else {
	if ((vifi != NO_VIF && scoped_addr(vifi, group)) ||
	    (rt && scoped_addr(rt->rt_parent, group)))
	    resp->tr_rflags = TR_SCOPED;
	else if (rt && vifi != NO_VIF && !VIFM_ISSET(vifi, rt->rt_children))
	    resp->tr_rflags = TR_NO_FWD;
    }

    /*
     *  if no rte exists, set NO_RTE error
     */
    if (!rt) {
	src = dst;		/* the dst address of resp. pkt */
	resp->tr_inaddr   = 0;
	resp->tr_rflags   = TR_NO_RTE;
	resp->tr_rmtaddr  = 0;
    } else {
	/* get # of packets in on interface */
	v_req.vifi = rt->rt_parent;
	if (ioctl(udp_socket, SIOCGETVIFCNT, (char *)&v_req) >= 0)
	    resp->tr_vifin = htonl(v_req.icount);
	else
	    resp->tr_vifin = 0xffffffff;

	MASK_TO_VAL(rt->rt_originmask, resp->tr_smask);
	src = uvifs[rt->rt_parent].uv_lcl_addr;
	resp->tr_inaddr = src;
	resp->tr_rmtaddr = rt->rt_gateway;
	if (vifi != NO_VIF && !VIFM_ISSET(vifi, rt->rt_children)) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Destination %s not on forwarding tree for src %s",
		      inet_fmt(qry->tr_dst, s1, sizeof(s1)), inet_fmt(qry->tr_src, s2, sizeof(s2)));
	    }
	    resp->tr_rflags = TR_WRONG_IF;
	}

	if (rt->rt_metric >= UNREACHABLE) {
	    resp->tr_rflags = TR_NO_RTE;
	    /* Hack to send reply directly */
	    rt = NULL;
	}
    }

sendit:
    /*
     * if metric is 1 or no. of reports is 1, send response to requestor
     * else send to upstream router.  If the upstream router can't handle
     * mtrace, set an error code and send to requestor anyway.
     */
    IF_DEBUG(DEBUG_TRACE)
	logit(LOG_DEBUG, 0, "rcount:%zu, no:%u", rcount, no);

    if ((rcount + 1 == no) || (rt == NULL) || (rt->rt_metric == 1)) {
	resptype = IGMP_MTRACE_RESP;
	dst = qry->tr_raddr;
    } else {
	if (!can_mtrace(rt->rt_parent, rt->rt_gateway)) {
	    dst = qry->tr_raddr;
	    resp->tr_rflags = TR_OLD_ROUTER;
	    resptype = IGMP_MTRACE_RESP;
	} else {
	    dst = rt->rt_gateway;
	    resptype = IGMP_MTRACE;
	}
    }

    if (IN_MULTICAST(ntohl(dst))) {
	/*
	 * Send the reply on a known multicast capable vif.
	 * If we don't have one, we can't source any multicasts anyway.
	 */
	if (phys_vif != -1) {
	    IF_DEBUG(DEBUG_TRACE) {
		logit(LOG_DEBUG, 0, "Sending reply to %s from %s", inet_fmt(dst, s1, sizeof(s1)),
		      inet_fmt(uvifs[phys_vif].uv_lcl_addr, s2, sizeof(s2)));
	    }
	    k_set_ttl(qry->tr_rttl);
	    send_igmp(uvifs[phys_vif].uv_lcl_addr, dst, resptype, no, group, datalen);
	    k_set_ttl(1);
	} else {
	    logit(LOG_INFO, 0, "No enabled phyints -- dropping traceroute reply");
	}
    } else {
	IF_DEBUG(DEBUG_TRACE) {
	    logit(LOG_DEBUG, 0, "Sending %s to %s from %s",
		  resptype == IGMP_MTRACE_RESP ?  "reply" : "request on",
		  inet_fmt(dst, s1, sizeof(s1)), inet_fmt(src, s2, sizeof(s2)));
	}
	send_igmp(src, dst, resptype, no, group, datalen);
    }
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
