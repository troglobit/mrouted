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
 * Private macros.
 */
#define MAX_NUM_RT   4096

/*
 * Private types.
 */
struct newrt {
    uint32_t	mask;
    uint32_t	origin;
    int32_t	metric;
    int32_t	pad;
};

struct blaster_hdr {
    uint32_t	bh_src;
    uint32_t	bh_dst;
    uint32_t	bh_level;
    int32_t	bh_datalen;
};

/*
 * Exported variables.
 */
int routes_changed;			/* 1=>some routes have changed */
int delay_change_reports;		/* 1=>postpone change reports  */
unsigned int nroutes;			/* number of routes            */

/*
 * Private variables.
 */
static TAILQ_HEAD(rthead, rtentry) rtable;
static struct rtentry *rtp;		/* pointer to a route entry    */

/*
 * Private functions.
 */
static int  init_children_and_leaves (struct rtentry *r, vifi_t parent, int first);
static int  find_route               (uint32_t origin, uint32_t mask);
static void create_route             (uint32_t origin, uint32_t mask);
static void discard_route            (struct rtentry *rt);
static int  compare_rts              (const void *rt1, const void *rt2);
static struct rtentry *report_chunk  (int, struct rtentry *, vifi_t, uint32_t, int *);
static void queue_blaster_report     (vifi_t vifi, uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level);
static void process_blaster_report   (int id, void *vifip);


/*
 * Initialize the routing table and associated variables.
 */
void init_routes(void)
{
    TAILQ_INIT(&rtable);
    nroutes		 = 0;
    routes_changed       = FALSE;
    delay_change_reports = FALSE;
}


/*
 * Initialize the children bits for route 'r', along with the
 * associated dominant and subordinate data structures.
 * If first is set, initialize dominants, otherwise keep old
 * dominants on non-parent interfaces.
 * XXX Does this need a return value?
 */
static int init_children_and_leaves(struct rtentry *r, vifi_t parent, int first)
{
    vifbitmap_t old_children;
    nbrbitmap_t old_subords;
    struct uvif *uv;
    vifi_t vifi;

    VIFM_COPY(r->rt_children, old_children);
    NBRM_COPY(r->rt_subordinates, old_subords);

    VIFM_CLRALL(r->rt_children);

    UVIF_FOREACH(vifi, uv) {
	if (first || vifi == parent)
	    r->rt_dominants[vifi] = 0;

	if (vifi == parent || (uv->uv_flags & VIFF_NOFLOOD)
	    || AVOID_TRANSIT(vifi, uv, r)
	    || (!first && r->rt_dominants[vifi]))
	    NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
	else
	    NBRM_SETMASK(r->rt_subordinates, uv->uv_nbrmap);

	if (vifi != parent && !(uv->uv_flags & (VIFF_DOWN|VIFF_DISABLED)) &&
	    !(!first && r->rt_dominants[vifi])) {
	    VIFM_SET(vifi, r->rt_children);
	}
    }

    return (!VIFM_SAME(r->rt_children, old_children) ||
	    !NBRM_SAME(r->rt_subordinates, old_subords));
}


/*
 * A new vif has come up -- update the children bitmaps in all route
 * entries to take that into account.
 */
void add_vif_to_routes(vifi_t vifi)
{
    struct rtentry *r;
    struct uvif *uv;

    uv = find_uvif(vifi);
    TAILQ_FOREACH(r, &rtable, rt_link) {
	if (r->rt_metric != UNREACHABLE && !VIFM_ISSET(vifi, r->rt_children)) {
	    VIFM_SET(vifi, r->rt_children);
	    r->rt_dominants[vifi] = 0;
	    /*XXX isn't uv_nbrmap going to be empty?*/
	    NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
	    update_table_entry(r, r->rt_gateway);
	}
    }
}


/*
 * A vif has gone down -- expire all routes that have that vif as parent,
 * and update the children bitmaps in all other route entries to take into
 * account the failed vif.
 */
void delete_vif_from_routes(vifi_t vifi)
{
    struct rtentry *r;
    struct uvif *uv;

    uv = find_uvif(vifi);
    TAILQ_FOREACH(r, &rtable, rt_link) {
	if (r->rt_metric != UNREACHABLE) {
	    if (vifi == r->rt_parent) {
		del_table_entry(r, 0, DEL_ALL_ROUTES);
		r->rt_timer    = ROUTE_EXPIRE_TIME;
		r->rt_metric   = UNREACHABLE;
		r->rt_flags   |= RTF_CHANGED;
		routes_changed = TRUE;
	    } else if (VIFM_ISSET(vifi, r->rt_children)) {
		VIFM_CLR(vifi, r->rt_children);
		NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
		update_table_entry(r, r->rt_gateway);
	    } else {
		r->rt_dominants[vifi] = 0;
	    }
	}
    }
}


/*
 * A new neighbor has come up.  If we're flooding on the neighbor's
 * vif, mark that neighbor as subordinate for all routes whose parent
 * is not this vif.
 */
void add_neighbor_to_routes(vifi_t vifi, uint32_t index)
{
    struct rtentry *r;
    struct uvif *uv;

    uv = find_uvif(vifi);
    if (uv->uv_flags & VIFF_NOFLOOD)
	return;

    TAILQ_FOREACH(r, &rtable, rt_link) {
	if (r->rt_metric != UNREACHABLE && r->rt_parent != vifi && !AVOID_TRANSIT(vifi, uv, r)) {
	    NBRM_SET(index, r->rt_subordinates);
	    update_table_entry(r, r->rt_gateway);
	}
    }
}


/*
 * A neighbor has failed or become unreachable.  If that neighbor was
 * considered a dominant or subordinate router in any route entries,
 * take appropriate action.  Expire all routes this neighbor advertised
 * to us.
 */
void delete_neighbor_from_routes(uint32_t addr, vifi_t vifi, uint32_t index)
{
    struct rtentry *r;
    struct uvif *uv;

    uv = find_uvif(vifi);
    TAILQ_FOREACH(r, &rtable, rt_link) {
	if (r->rt_metric != UNREACHABLE) {
	    if (r->rt_parent == vifi && r->rt_gateway == addr) {
		del_table_entry(r, 0, DEL_ALL_ROUTES);
		r->rt_timer    = ROUTE_EXPIRE_TIME;
		r->rt_metric   = UNREACHABLE;
		r->rt_flags   |= RTF_CHANGED;
		routes_changed = TRUE;
	    } else if (r->rt_dominants[vifi] == addr) {
		VIFM_SET(vifi, r->rt_children);
		r->rt_dominants[vifi] = 0;
		if ((uv->uv_flags & VIFF_NOFLOOD) || AVOID_TRANSIT(vifi, uv, r))
		    NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
		else
		    NBRM_SETMASK(r->rt_subordinates, uv->uv_nbrmap);
		update_table_entry(r, r->rt_gateway);
	    } else if (NBRM_ISSET(index, r->rt_subordinates)) {
		NBRM_CLR(index, r->rt_subordinates);
		update_table_entry(r, r->rt_gateway);
	    }
	}
    }
}


/*
 * Prepare for a sequence of ordered route updates by initializing a pointer
 * to the start of the routing table.  The pointer is used to remember our
 * position in the routing table in order to avoid searching from the
 * beginning for each update; this relies on having the route reports in
 * a single message be in the same order as the route entries in the routing
 * table.
 *
 * find_route() expects rtp to be the preceding entry in the linked list
 * where route insertion takes place.  We need to be able to insert routes
 * before at the list head (routing table).
 */
void start_route_updates(void)
{
    rtp = NULL;
}


/*
 * Starting at the route entry following the one to which 'rtp' points,
 * look for a route entry matching the specified origin and mask.  If a
 * match is found, return TRUE and leave 'rtp' pointing at the found entry.
 * If no match is found, return FALSE and leave 'rtp' pointing to the route
 * entry preceding the point at which the new origin should be inserted.
 * This code is optimized for the normal case in which the first entry to
 * be examined is the matching entry.
 */
static int find_route(uint32_t origin, uint32_t mask)
{
    struct rtentry *r;

    /*
     * If rtp is NULL, we are preceding rtable, so our first search
     * candidate should be the rtable.
     */
    r = rtp ? rtp : TAILQ_FIRST(&rtable);
    while (r != NULL) {
	if (origin == r->rt_origin && mask == r->rt_originmask) {
	    rtp = r;
	    return TRUE;
	}

	if (ntohl(mask) < ntohl(r->rt_originmask) ||
	    (mask == r->rt_originmask &&
	     ntohl(origin) < ntohl(r->rt_origin))) {
	    rtp = r;
	    r = TAILQ_NEXT(r, rt_link);
	} else
	    break;
    }

    return FALSE;
}

/*
 * Create a new routing table entry for the specified origin and link it into
 * the routing table.  The shared variable 'rtp' is assumed to point to the
 * routing entry after which the new one should be inserted.  It is left
 * pointing to the new entry.
 *
 * Only the origin, originmask, originwidth and flags fields are initialized
 * in the new route entry; the caller is responsible for filling in the rest.
 */
static void create_route(uint32_t origin, uint32_t mask)
{
    struct rtentry *rt;

    rt = calloc(1, sizeof(struct rtentry));
    if (!rt) {
	logit(LOG_ERR, errno, "Failed allocating 'struct rtentry' in %s:%s()", __FILE__, __func__);
	return;
    }

    rt->rt_dominants = calloc(1, numvifs * sizeof(uint32_t));
    if (!rt->rt_dominants) {
	free(rt);
	logit(LOG_ERR, errno, "Failed allocating 'rt_dominants' in %s:%s()", __FILE__, __func__);
	return;
    }

    rt->rt_origin     = origin;
    rt->rt_originmask = mask;
    if      (((char *)&mask)[3] != 0) rt->rt_originwidth = 4;
    else if (((char *)&mask)[2] != 0) rt->rt_originwidth = 3;
    else if (((char *)&mask)[1] != 0) rt->rt_originwidth = 2;
    else                              rt->rt_originwidth = 1;
    rt->rt_flags = 0;
    rt->rt_groups = NULL;

    VIFM_CLRALL(rt->rt_children);
    NBRM_CLRALL(rt->rt_subordinates);
    NBRM_CLRALL(rt->rt_subordadv);

    if (rtp)
	TAILQ_INSERT_AFTER(&rtable, rtp, rt, rt_link);
    else
	TAILQ_INSERT_HEAD(&rtable, rt, rt_link);

    rtp = rt;
    ++nroutes;
}


/*
 * Discard the routing table entry following the one to which 'rt' points.
 *         [.|prev|.]--->[.|rt|.]<---[.|next|.]
 */
static void discard_route(struct rtentry *rt)
{
    struct uvif *uv;

    if (!rt)
	return;

    /* Update meta pointers */
    if (rtp == rt)
	rtp = TAILQ_NEXT(rt, rt_link);

    TAILQ_REMOVE(&rtable, rt, rt_link);

    /* Update the books */
    uv = find_uvif(rt->rt_parent);
    uv->uv_nroutes--;
    /*???nbr???.al_nroutes--;*/
    --nroutes;

    free(rt->rt_dominants);
    free(rt);
}


/*
 * Process a route report for a single origin, creating or updating the
 * corresponding routing table entry if necessary.  'src' is either the
 * address of a neighboring router from which the report arrived, or zero
 * to indicate a change of status of one of our own interfaces.
 */
void update_route(uint32_t origin, uint32_t mask, uint32_t metric, uint32_t src, vifi_t vifi, struct listaddr *n)
{
    uint32_t adj_metric;
    struct rtentry *r;
    struct uvif *uv;

    uv = find_uvif(vifi);

    /*
     * Compute an adjusted metric, taking into account the cost of the
     * subnet or tunnel over which the report arrived, and normalizing
     * all unreachable/poisoned metrics into a single value.
     */
    if (src != 0 && (metric < 1 || metric >= 2*UNREACHABLE)) {
	logit(LOG_WARNING, 0, "%s reports out-of-range metric %u for origin %s",
	    inet_fmt(src, s1, sizeof(s1)), metric, inet_fmts(origin, mask, s2, sizeof(s2)));
	return;
    }
    adj_metric = metric + uv->uv_metric;
    if (adj_metric > UNREACHABLE) adj_metric = UNREACHABLE;

    /*
     * Look up the reported origin in the routing table.
     */
    if (!find_route(origin, mask)) {
	/*
	 * Not found.
	 * Don't create a new entry if the report says it's unreachable,
	 * or if the reported origin and mask are invalid.
	 */
	if (adj_metric == UNREACHABLE) {
	    return;
	}
	if (src != 0 && !inet_valid_subnet(origin, mask)) {
	    logit(LOG_WARNING, 0, "%s reports an invalid origin (%s) and/or mask (%08x)",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmt(origin, s2, sizeof(s2)), ntohl(mask));
	    return;
	}

	IF_DEBUG(DEBUG_RTDETAIL) {
	    logit(LOG_DEBUG, 0, "%s (origin %s) advertising new route %s",
		  src ? inet_fmt(src, s1, sizeof(s1)) : "we are",
		  inet_fmt(origin, s2, sizeof(s2)),
		  inet_fmts(origin, mask, s2, sizeof(s2)));
	}

	/*
	 * OK, create the new routing entry.  'rtp' will be left pointing
	 * to the new entry.
	 */
	create_route(origin, mask);
	uv->uv_nroutes++;
	/*n->al_nroutes++;*/

	rtp->rt_metric = UNREACHABLE;	/* temporary; updated below */
    }

    /*
     * We now have a routing entry for the reported origin.  Update it?
     */
    r = rtp;
    if (r->rt_metric == UNREACHABLE) {
	/*
	 * The routing entry is for a formerly-unreachable or new origin.
	 * If the report claims reachability, update the entry to use
	 * the reported route.
	 */
	if (adj_metric == UNREACHABLE)
	    return;

	IF_DEBUG(DEBUG_RTDETAIL) {
	    logit(LOG_DEBUG, 0, "%s advertises %s with adj_metric %d (ours was %d)",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmts(origin, mask, s2, sizeof(s2)),
		  adj_metric, r->rt_metric);
	}

	/*
	 * Now "steal away" any sources that belong under this route
	 * by deleting any cache entries they might have created
	 * and allowing the kernel to re-request them.
	 *
	 * If we haven't performed final initialization yet and are
	 * just collecting the routing table, we can't have any
	 * sources so we don't perform this step.
	 */
	if (did_final_init)
	    steal_sources(rtp);

	r->rt_parent   = vifi;
	r->rt_gateway  = src;
	init_children_and_leaves(r, vifi, 1);

	r->rt_timer    = 0;
	r->rt_metric   = adj_metric;
	r->rt_flags   |= RTF_CHANGED;
	routes_changed = TRUE;
	update_table_entry(r, r->rt_gateway);
    } else if (src == r->rt_gateway) {
	/*
	 * The report has come either from the interface directly-connected
	 * to the origin subnet (src and r->rt_gateway both equal zero) or
	 * from the gateway we have chosen as the best first-hop gateway back
	 * towards the origin (src and r->rt_gateway not equal zero).  Reset
	 * the route timer and, if the reported metric has changed, update
	 * our entry accordingly.
	 */
	r->rt_timer = 0;

	IF_DEBUG(DEBUG_RTDETAIL) {
	    logit(LOG_DEBUG, 0, "%s (current parent) advertises %s with adj_metric %d (ours was %d)",
		  inet_fmt(src, s1, sizeof(s1)), inet_fmts(origin, mask, s2, sizeof(s2)),
		  adj_metric, r->rt_metric);
	}

	if (adj_metric == r->rt_metric)
	    return;

	if (adj_metric == UNREACHABLE) {
	    del_table_entry(r, 0, DEL_ALL_ROUTES);
	    r->rt_timer = ROUTE_EXPIRE_TIME;
	}
	r->rt_metric   = adj_metric;
	r->rt_flags   |= RTF_CHANGED;
	routes_changed = TRUE;
    } else if (src == 0 ||
	       (r->rt_gateway != 0 &&
		(adj_metric < r->rt_metric ||
		 (adj_metric == r->rt_metric &&
		  (ntohl(src) < ntohl(r->rt_gateway) ||
		   r->rt_timer >= ROUTE_SWITCH_TIME))))) {
	/*
	 * The report is for an origin we consider reachable; the report
	 * comes either from one of our own interfaces or from a gateway
	 * other than the one we have chosen as the best first-hop gateway
	 * back towards the origin.  If the source of the update is one of
	 * our own interfaces, or if the origin is not a directly-connected
	 * subnet and the reported metric for that origin is better than
	 * what our routing entry says, update the entry to use the new
	 * gateway and metric.  We also switch gateways if the reported
	 * metric is the same as the one in the route entry and the gateway
	 * associated with the route entry has not been heard from recently,
	 * or if the metric is the same but the reporting gateway has a lower
	 * IP address than the gateway associated with the route entry.
	 * Did you get all that?
	 */
	uint32_t old_gateway;
	vifi_t old_parent;

	old_gateway = r->rt_gateway;
	old_parent = r->rt_parent;
	r->rt_gateway = src;
	r->rt_parent = vifi;

	IF_DEBUG(DEBUG_RTDETAIL) {
	    logit(LOG_DEBUG, 0, "%s (new parent) on vif %d advertises %s with adj_metric %d (old parent was %s on vif %d, metric %d)",
		  inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
		  adj_metric, inet_fmt(old_gateway, s3, sizeof(s3)), old_parent, r->rt_metric);
	}

	if (old_parent != vifi) {
	    struct uvif *ov = find_uvif(old_parent);

	    init_children_and_leaves(r, vifi, 0);
	    ov->uv_nroutes--;
	    uv->uv_nroutes++;
	}
	if (old_gateway != src) {
	    update_table_entry(r, old_gateway);
	    /*???old_gateway???->al_nroutes--;*/
	    /*n->al_nroutes++;*/
	}
	r->rt_timer    = 0;
	r->rt_metric   = adj_metric;
	r->rt_flags   |= RTF_CHANGED;
	routes_changed = TRUE;
    } else if (vifi != r->rt_parent) {
	/*
	 * The report came from a vif other than the route's parent vif.
	 * Update the children info, if necessary.
	 */
	if (AVOID_TRANSIT(vifi, uv, r)) {
	    /*
	     * The route's parent is a vif from which we're not supposed
	     * to transit onto this vif.  Simply ignore the update.
	     */
	    IF_DEBUG(DEBUG_RTDETAIL) {
		logit(LOG_DEBUG, 0, "%s on vif %d advertises %s with metric %d (ignored due to NOTRANSIT)",
		      inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)), metric);
	    }
	} else if (VIFM_ISSET(vifi, r->rt_children)) {
	    /*
	     * Vif is a child vif for this route.
	     */
	    if (metric  < r->rt_metric ||
		(metric == r->rt_metric &&
		 ntohl(src) < ntohl(uv->uv_lcl_addr))) {
		/*
		 * Neighbor has lower metric to origin (or has same metric
		 * and lower IP address) -- it becomes the dominant router,
		 * and vif is no longer a child for me.
		 */
		VIFM_CLR(vifi, r->rt_children);
		r->rt_dominants[vifi] = src;
		/* XXX
		 * We don't necessarily want to forget about subordinateness
		 * so that we can become the dominant quickly if the current
		 * dominant fails.
		 */
		NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
		update_table_entry(r, r->rt_gateway);
		IF_DEBUG(DEBUG_RTDETAIL) {
		    logit(LOG_DEBUG, 0, "%s on vif %d becomes dominant for %s with metric %d",
			  inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
			  metric);
		}
	    } else if (metric > UNREACHABLE) {	/* "poisoned reverse" */
		/*
		 * Neighbor considers this vif to be on path to route's
		 * origin; record this neighbor as subordinate
		 */
		if (!NBRM_ISSET(n->al_index, r->rt_subordinates)) {
		    IF_DEBUG(DEBUG_RTDETAIL) {
			logit(LOG_DEBUG, 0, "%s on vif %d becomes subordinate for %s with poison-reverse metric %d",
			      inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
			      metric - UNREACHABLE);
		    }
		    NBRM_SET(n->al_index, r->rt_subordinates);
		    update_table_entry(r, r->rt_gateway);
		} else {
		    IF_DEBUG(DEBUG_RTDETAIL) {
			logit(LOG_DEBUG, 0, "%s on vif %d confirms subordinateness for %s with poison-reverse metric %d",
			      inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
			      metric - UNREACHABLE);
		    }
		}
		NBRM_SET(n->al_index, r->rt_subordadv);
	    } else if (NBRM_ISSET(n->al_index, r->rt_subordinates)) {
		/*
		 * Current subordinate no longer considers this vif to be on
		 * path to route's origin; it is no longer a subordinate
		 * router.
		 */
		IF_DEBUG(DEBUG_RTDETAIL) {
		    logit(LOG_DEBUG, 0, "%s on vif %d is no longer a subordinate for %s with metric %d",
			  inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
			  metric);
		}
		NBRM_CLR(n->al_index, r->rt_subordinates);
		update_table_entry(r, r->rt_gateway);
	    }
	} else if (src == r->rt_dominants[vifi] &&
		   (metric  > r->rt_metric ||
		    (metric == r->rt_metric &&
		     ntohl(src) > ntohl(uv->uv_lcl_addr)))) {
	    /*
	     * Current dominant no longer has a lower metric to origin
	     * (or same metric and lower IP address); we adopt the vif
	     * as our own child.
	     */
	    IF_DEBUG(DEBUG_RTDETAIL) {
		logit(LOG_DEBUG, 0, "%s (current dominant) on vif %d is no longer dominant for %s with metric %d",
		      inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
		      metric);
	    }

	    VIFM_SET(vifi, r->rt_children);
	    r->rt_dominants[vifi] = 0;

	    if (uv->uv_flags & VIFF_NOFLOOD)
		NBRM_CLRMASK(r->rt_subordinates, uv->uv_nbrmap);
	    else
		NBRM_SETMASK(r->rt_subordinates, uv->uv_nbrmap);

	    if (metric > UNREACHABLE) {
		NBRM_SET(n->al_index, r->rt_subordinates);
		NBRM_SET(n->al_index, r->rt_subordadv);
	    }
	    update_table_entry(r, r->rt_gateway);
	} else {
	    IF_DEBUG(DEBUG_RTDETAIL) {
		logit(LOG_DEBUG, 0, "%s on vif %d advertises %s with metric %d (ignored)",
		      inet_fmt(src, s1, sizeof(s1)), vifi, inet_fmts(origin, mask, s2, sizeof(s2)),
		      metric);
	    }
	}
    }
}


/*
 * On every timer interrupt, advance the timer in each routing entry.
 */
void age_routes(void)
{
    extern uint32_t virtual_time;		/* from main.c */
    struct rtentry *r, *tmp;

    TAILQ_FOREACH_SAFE(r, &rtable, rt_link, tmp) {
	if ((r->rt_timer += TIMER_INTERVAL) >= ROUTE_DISCARD_TIME) {
	    /*
	     * Time to garbage-collect the route entry.
	     */
	    del_table_entry(r, 0, DEL_ALL_ROUTES);
	    discard_route(r);
	} else if (r->rt_timer >= ROUTE_EXPIRE_TIME &&
		 r->rt_metric != UNREACHABLE) {
	    /*
	     * Time to expire the route entry.  If the gateway is zero,
	     * i.e., it is a route to a directly-connected subnet, just
	     * set the timer back to zero; such routes expire only when
	     * the interface to the subnet goes down.
	     */
	    if (r->rt_gateway == 0) {
		r->rt_timer = 0;
	    } else {
		del_table_entry(r, 0, DEL_ALL_ROUTES);
		r->rt_metric   = UNREACHABLE;
		r->rt_flags   |= RTF_CHANGED;
		routes_changed = TRUE;
	    }
	} else if (virtual_time % (ROUTE_REPORT_INTERVAL * 2) == 0) {
	    /*
	     * Time out subordinateness that hasn't been reported in
	     * the last 2 intervals.
	     */
	    if (!NBRM_SAME(r->rt_subordinates, r->rt_subordadv)) {
		IF_DEBUG(DEBUG_ROUTE) {
		    logit(LOG_DEBUG, 0, "rt %s sub 0x%08x%08x subadv 0x%08x%08x metric %d",
			  RT_FMT(r, s1), r->rt_subordinates.hi, r->rt_subordinates.lo,
			  r->rt_subordadv.hi, r->rt_subordadv.lo, r->rt_metric);
		}
		NBRM_MASK(r->rt_subordinates, r->rt_subordadv);
		update_table_entry(r, r->rt_gateway);
	    }
	    NBRM_CLRALL(r->rt_subordadv);
	}
    }
}


/*
 * Mark all routes as unreachable.  This function is called only from
 * hup() in preparation for informing all neighbors that we are going
 * off the air.  For consistency, we ought also to delete all reachable
 * route entries from the kernel, but since we are about to exit we rely
 * on the kernel to do its own cleanup -- no point in making all those
 * expensive kernel calls now.
 */
void expire_all_routes(void)
{
    struct rtentry *r;

    TAILQ_FOREACH(r, &rtable, rt_link) {
	r->rt_metric   = UNREACHABLE;
	r->rt_flags   |= RTF_CHANGED;
	routes_changed = TRUE;
    }
}


/*
 * Delete all the routes in the routing table.
 */
void free_all_routes(void)
{
    struct rtentry *r, *tmp;

    TAILQ_FOREACH_SAFE(r, &rtable, rt_link, tmp)
	discard_route(r);
}


/*
 * Process an incoming neighbor probe message.
 */
void accept_probe(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
    vifi_t vifi;

    vifi = find_vif_direct(src, dst);
    if (vifi == NO_VIF) {
	logit(LOG_WARNING, 0, "Ignoring probe from non-neighbor %s" , inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    update_neighbor(vifi, src, DVMRP_PROBE, p, datalen, level);
}

static int compare_rts(const void *rt1, const void *rt2)
{
    struct newrt *r1 = (struct newrt *)rt1;
    struct newrt *r2 = (struct newrt *)rt2;
    uint32_t m1 = ntohl(r1->mask);
    uint32_t m2 = ntohl(r2->mask);
    uint32_t o1, o2;

    if (m1 > m2)
	return -1;
    if (m1 < m2)
	return 1;

    /* masks are equal */
    o1 = ntohl(r1->origin);
    o2 = ntohl(r2->origin);
    if (o1 > o2)
	return -1;
    if (o1 < o2)
	return 1;

    return 0;
}

/*
 * Queue a route report from a route-blaster.
 * If the timer isn't running to process these reports,
 * start it.
 */
static void queue_blaster_report(vifi_t vifi, uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
    struct blaster_hdr *bh;
    struct uvif *uv;
    int bblen = sizeof(*bh) + ((datalen + 3) & ~3);

    uv = find_uvif(vifi);
    if (uv->uv_blasterend - uv->uv_blasterbuf + bblen > uv->uv_blasterlen) {
	int end = uv->uv_blasterend - uv->uv_blasterbuf;
	int cur = uv->uv_blastercur - uv->uv_blasterbuf;

	uv->uv_blasterlen *= 2;
	IF_DEBUG(DEBUG_IF)
	    logit(LOG_DEBUG, 0, "Increasing blasterbuf to %d bytes", uv->uv_blasterlen);

	uv->uv_blasterbuf = realloc(uv->uv_blasterbuf, uv->uv_blasterlen);
	if (uv->uv_blasterbuf == NULL) {
	    logit(LOG_WARNING, errno, "Turning off blaster on vif %d", vifi);
	    uv->uv_blasterlen = 0;
	    uv->uv_blasterend = uv->uv_blastercur = NULL;
	    uv->uv_flags &= ~VIFF_BLASTER;
	    return;
	}
	uv->uv_blasterend = uv->uv_blasterbuf + end;
	uv->uv_blastercur = uv->uv_blasterbuf + cur;
    }
    bh = (struct blaster_hdr *)uv->uv_blasterend;
    bh->bh_src = src;
    bh->bh_dst = dst;
    bh->bh_level = level;
    bh->bh_datalen = datalen;
    memmove((char *)(bh + 1), p, datalen);
    uv->uv_blasterend += bblen;

    if (uv->uv_blastertimer == 0) {
	int *i = malloc(sizeof(int));

	if (!i) {
	    logit(LOG_ERR, errno, "%s(): Failed allocating memory", __func__);
	    return;
	}

	*i = vifi;
	uv->uv_blastertimer = pev_timer_add(5000000, 0, process_blaster_report, i);
    }
}

/*
 * Periodic process; process up to 5 of the routes in the route-blaster
 * queue.  If there are more routes remaining, reschedule myself to run
 * in 1 second.
 */
static void process_blaster_report(int id, void *vifip)
{
    vifi_t vifi = *(int *)vifip;
    struct blaster_hdr *bh;
    struct uvif *uv;
    int i;

    uv = find_uvif(vifi);

    IF_DEBUG(DEBUG_ROUTE) {
	logit(LOG_DEBUG, 0, "Processing vif %d blasted routes", vifi);
    }

    for (i = 0; i < 5; i++) {
	if (uv->uv_blastercur >= uv->uv_blasterend)
		break;

	bh = (struct blaster_hdr *)uv->uv_blastercur;
	uv->uv_blastercur += sizeof(*bh) + ((bh->bh_datalen + 3) & ~3);

	accept_report(bh->bh_src, bh->bh_dst, (char *)(bh + 1), -bh->bh_datalen, bh->bh_level);
    }

    if (uv->uv_blastercur >= uv->uv_blasterend) {
	uv->uv_blastercur = uv->uv_blasterbuf;
	uv->uv_blasterend = uv->uv_blasterbuf;
	uv->uv_blastertimer = 0;
	free(vifip);

	IF_DEBUG(DEBUG_ROUTE) {
	    logit(LOG_DEBUG, 0, "Finish processing vif %d blaster", vifi);
	}

	pev_timer_del(id);
    } else {
	IF_DEBUG(DEBUG_ROUTE) {
	    logit(LOG_DEBUG, 0, "More blasted routes to come on vif %d", vifi);
	}

	pev_timer_set(id, 1000000);
    }
}

/*
 * Process an incoming route report message.
 * If the report arrived on a vif marked as a "blaster", then just
 * queue it and return; queue_blaster_report() will schedule it for
 * processing later.  If datalen is negative, then this is actually
 * a queued report so actually process it instead of queueing it.
 */
void accept_report(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
    static struct newrt rt[MAX_NUM_RT]; /* Use heap instead of stack */ /* XXX: fixme */
    struct listaddr *nbr;
    struct uvif *uv;
    uint32_t origin;
    uint32_t mask;
    size_t width, i;
    size_t nrt = 0;
    vifi_t vifi;
    int metric;

    /*
     * Emulate a stack variable.  We use the heap insted of the stack
     * to prevent stack overflow on systems that cannot do stack realloc
     * at runtime, e.g., non-MMU Linux systems.
     */
    memset(rt, 0, MAX_NUM_RT * sizeof(rt[0]));

    if ((vifi = find_vif_direct(src, dst)) == NO_VIF) {
	logit(LOG_INFO, 0, "Ignoring route report from non-neighbor %s",
	      inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    uv = find_uvif(vifi);
    if (uv->uv_flags & VIFF_BLASTER) {
	if (datalen > 0) {
	    queue_blaster_report(vifi, src, dst, p, datalen, level);
	    return;
	} else {
	    datalen = -datalen;
	}
    }

    nbr = update_neighbor(vifi, src, DVMRP_REPORT, NULL, 0, level);
    if (!nbr)
	return;

    if (datalen > 2 * 4096) {
	logit(LOG_INFO, 0, "Ignoring oversized (%zu bytes) route report from %s",
	      datalen, inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    while (datalen > 0  && nrt < MAX_NUM_RT) { /* Loop through per-mask lists. */
	if (datalen < 3) {
	    logit(LOG_WARNING, 0, "Received truncated route report from %s", 
		  inet_fmt(src, s1, sizeof(s1)));
	    return;
	}

	((uint8_t *)&mask)[0] = 0xff;            width = 1;
	if ((((uint8_t *)&mask)[1] = *p++) != 0) width = 2;
	if ((((uint8_t *)&mask)[2] = *p++) != 0) width = 3;
	if ((((uint8_t *)&mask)[3] = *p++) != 0) width = 4;
	if (!inet_valid_mask(ntohl(mask))) {
	    logit(LOG_WARNING, 0, "%s reports bogus netmask 0x%08x (%s)",
		  inet_fmt(src, s1, sizeof(s1)), ntohl(mask), inet_fmt(mask, s2, sizeof(s2)));
	    return;
	}
	datalen -= 3;

	do {			/* Loop through (origin, metric) pairs */
	    if (datalen < width + 1) {
		logit(LOG_WARNING, 0, "Received truncated route report from %s", 
		      inet_fmt(src, s1, sizeof(s1)));
		return;
	    }
	    origin = 0;
	    for (i = 0; i < width; ++i)
		((char *)&origin)[i] = *p++;
	    metric = *p++;
	    datalen -= width + 1;
	    rt[nrt].mask   = mask;
	    rt[nrt].origin = origin;
	    rt[nrt].metric = (metric & 0x7f);
	    ++nrt;
	} while (!(metric & 0x80) && nrt < MAX_NUM_RT);
    }

    qsort((char *)rt, nrt, sizeof(rt[0]), compare_rts);
    start_route_updates();

    /*
     * If the last entry is default, change mask from 0xff000000 to 0
     */
    if (nrt > 0 && rt[nrt - 1].origin == 0)
	rt[nrt - 1].mask = 0;

    IF_DEBUG(DEBUG_ROUTE) {
	logit(LOG_DEBUG, 0, "Updating %zu routes from %s to %s", nrt,
	      inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
    }

    for (i = 0; i < nrt; ++i) {
	if (i > 0 && rt[i].origin == rt[i - 1].origin && rt[i].mask == rt[i - 1].mask) {
	    logit(LOG_WARNING, 0, "%s reports duplicate route for %s",
                  inet_fmt(src, s1, sizeof(s1)), inet_fmts(rt[i].origin, rt[i].mask, s2, sizeof(s2)));
	    continue;
	}
	/* Only filter non-poisoned updates. */
	if (uv->uv_filter && rt[i].metric < UNREACHABLE) {
	    struct vf_element *vfe;
	    int match = 0;

	    for (vfe = uv->uv_filter->vf_filter; vfe; vfe = vfe->vfe_next) {
		if (vfe->vfe_flags & VFEF_EXACT) {
		    if ((vfe->vfe_addr == rt[i].origin) && (vfe->vfe_mask == rt[i].mask)) {
			match = 1;
			break;
		    }
		} else {
		    if ((rt[i].origin & vfe->vfe_mask) == vfe->vfe_addr) {
			match = 1;
			break;
		    }
		}
	    }
	    if ((uv->uv_filter->vf_type == VFT_ACCEPT && match == 0) ||
		(uv->uv_filter->vf_type == VFT_DENY && match == 1)) {
		IF_DEBUG(DEBUG_ROUTE) {
		    logit(LOG_DEBUG, 0, "%s skipped on vif %d because it %s %s",
			  inet_fmts(rt[i].origin, rt[i].mask, s1, sizeof(s1)),
			  vifi, match ? "matches" : "doesn't match",
			  match ? inet_fmts(vfe->vfe_addr, vfe->vfe_mask, s2, sizeof(s2))
			  : "the filter");
		}
#if 0
		rt[i].metric += vfe->vfe_addmetric;
		if (rt[i].metric > UNREACHABLE)
#endif
		    rt[i].metric = UNREACHABLE;
	    }
	}
	update_route(rt[i].origin, rt[i].mask, rt[i].metric, src, vifi, nbr);
    }

    if (routes_changed && !delay_change_reports)
	report_to_all_neighbors(CHANGED_ROUTES);
}


/*
 * Send a route report message to destination 'dst', via virtual interface
 * 'vifi'.  'type' specifies ALL_ROUTES or CHANGED_ROUTES.
 */
void report(int type, vifi_t vifi, uint32_t dst)
{
    struct rtentry *rt = TAILQ_LAST(&rtable, rthead);
    int dummy;

    while (rt)
	rt = report_chunk(type, rt, vifi, dst, &dummy);
}


/*
 * Send a route report message to all neighboring routers.
 * 'type' specifies ALL_ROUTES or CHANGED_ROUTES.
 */
void report_to_all_neighbors(int type)
{
    int routes_changed_before;
    struct rtentry *r;
    struct uvif *uv;
    vifi_t vifi;

    /*
     * Remember the state of the global routes_changed flag before
     * generating the reports, and clear the flag.
     */
    routes_changed_before = routes_changed;
    routes_changed = FALSE;

    UVIF_FOREACH(vifi, uv) {
	if (!NBRM_ISEMPTY(uv->uv_nbrmap))
	    report(type, vifi, uv->uv_dst_addr);
    }

    /*
     * If there were changed routes before we sent the reports AND
     * if no new changes occurred while sending the reports, clear
     * the change flags in the individual route entries.  If changes
     * did occur while sending the reports, new reports will be
     * generated at the next timer interrupt.
     */
    if (routes_changed_before && !routes_changed) {
	TAILQ_FOREACH(r, &rtable, rt_link)
	    r->rt_flags &= ~RTF_CHANGED;
    }

    /*
     * Set a flag to inhibit further reports of changed routes until the
     * next timer interrupt.  This is to alleviate update storms.
     */
    delay_change_reports = TRUE;
}

/*
 * Send a route report message to destination 'dst', via virtual interface
 * 'vifi'.  'type' specifies ALL_ROUTES or CHANGED_ROUTES.
 */
static struct rtentry *report_chunk(int type, struct rtentry *rt, vifi_t vifi, uint32_t dst, int *nrt)
{
    struct rtentry *r;
    struct uvif *uv;
    uint32_t mask = 0;
    int datalen = 0;
    int width = 0;
    int admetric;
    uint8_t *p;
    int metric;
    int i;

    *nrt = 0;
    uv = find_uvif(vifi);
    if (!uv)
	return NULL;
    admetric = uv->uv_admetric;

    p = send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;

    for (r = rt; r != TAILQ_END(&rtable); r = TAILQ_PREV(r, rthead, rt_link)) {
	if (type == CHANGED_ROUTES && !(r->rt_flags & RTF_CHANGED)) {
	    (*nrt)++;
	    continue;
	}

	/*
	 * Do not poison-reverse a route for a directly-connected
	 * subnetwork on that subnetwork.  This can cause loops when
	 * some router on the subnetwork is misconfigured.
	 */
	if (r->rt_gateway == 0 && r->rt_parent == vifi) {
	    (*nrt)++;
	    continue;
	}

	if (uv->uv_filter && uv->uv_filter->vf_flags & VFF_BIDIR) {
	    struct vf_element *vfe;
	    int match = 0;

	    for (vfe = uv->uv_filter->vf_filter; vfe; vfe = vfe->vfe_next) {
		if (vfe->vfe_flags & VFEF_EXACT) {
		    if ((vfe->vfe_addr == r->rt_origin) &&
			(vfe->vfe_mask == r->rt_originmask)) {
			    match = 1;
			    break;
		    }

		    continue;
		}

		if ((r->rt_origin & vfe->vfe_mask) == vfe->vfe_addr) {
		    match = 1;
		    break;
		}
	    }

	    if ((uv->uv_filter->vf_type == VFT_ACCEPT && match == 0) ||
		(uv->uv_filter->vf_type == VFT_DENY   && match == 1)) {
		IF_DEBUG(DEBUG_ROUTE) {
		    logit(LOG_DEBUG, 0, "%s not reported on vif %d because it %s %s",
			  RT_FMT(r, s1), vifi,
			  (match
			   ? "matches"
			   : "doesn't match"),
			  (match
			   ? inet_fmts(vfe->vfe_addr, vfe->vfe_mask, s2, sizeof(s2))
			   : "the filter"));
		}
		(*nrt)++;
		continue;
	    }
	}

	/*
	 * If there is no room for this route in the current message,
	 * send it & return how many routes we sent.
	 */
	if (datalen + ((r->rt_originmask == mask)
		       ? (width + 1)
		       : (r->rt_originwidth + 4)) > MAX_DVMRP_DATA_LEN) {
	    *(p-1) |= 0x80;
	    send_on_vif(uv, 0, DVMRP_REPORT, datalen);

	    return r;
	}

	if (r->rt_originmask != mask || datalen == 0) {
	    mask  = r->rt_originmask;
	    width = r->rt_originwidth;

	    if (datalen != 0)
		*(p - 1) |= 0x80;
	    *p++ = ((char *)&mask)[1];
	    *p++ = ((char *)&mask)[2];
	    *p++ = ((char *)&mask)[3];
	    datalen += 3;
	}

	for (i = 0; i < width; ++i)
	    *p++ = ((char *)&(r->rt_origin))[i];

	metric = r->rt_metric + admetric;
	if (metric > UNREACHABLE)
	    metric = UNREACHABLE;

	if (r->rt_parent != vifi && AVOID_TRANSIT(vifi, uv, r))
	    metric = UNREACHABLE;

	*p++ = (r->rt_parent == vifi && metric != UNREACHABLE)
	    ? (char)(metric + UNREACHABLE) /* "poisoned reverse" */
	    : (char)(metric);
	(*nrt)++;
	datalen += width + 1;
    }

    if (datalen != 0) {
	*(p-1) |= 0x80;
	send_on_vif(uv, 0, DVMRP_REPORT, datalen);
    }

    return r;
}

/*
 * send the next chunk of our routing table to all neighbors.
 * return the length of the smallest chunk we sent out.
 */
int report_next_chunk(void)
{
    static int start_rt = 0;
    struct rtentry *sr;
    struct uvif *uv;
    int min = 20000;
    vifi_t vifi;
    int n = 0;
    int i;

    if (nroutes <= 0)
	return 0;

    /*
     * find this round's starting route.
     */
    i = start_rt;
    TAILQ_FOREACH_REVERSE(sr, &rtable, rthead, rt_link) {
	if (--i < 0)
	    break;
    }

    /*
     * send one chunk of routes starting at this round's start to
     * all our neighbors.
     */
    UVIF_FOREACH(vifi, uv) {
	/* sr might turn up NULL above ... */
	if (sr && !NBRM_ISEMPTY(uv->uv_nbrmap)) {
	    report_chunk(ALL_ROUTES, sr, vifi, uv->uv_dst_addr, &n);
	    if (n < min)
		min = n;
	}
    }
    if (min == 20000)
	min = 0;	/* Neighborless router didn't send any routes */

    n = min;
    IF_DEBUG(DEBUG_ROUTE) {
	logit(LOG_INFO, 0, "update %d starting at %d of %d",
	      n, (nroutes - start_rt), nroutes);
    }

    start_rt = (start_rt + n) % nroutes;

    return n;
}


/*
 * Print the contents of the routing table on file 'fp'.
 */
void dump_routes(FILE *fp, int detail)
{
    struct rtentry *r;
    struct uvif *uv;
    vifi_t vifi;

    if (detail)
	fprintf(fp, "Multicast Routing Table (%u entr%s)\n", nroutes, nroutes == 1 ? "y" : "ies");
    fputs(" Origin-Subnet      From-Gateway    Metric Tmr Fl In-Vif  Out-Vifs=\n", fp);

    TAILQ_FOREACH(r, &rtable, rt_link) {
	fprintf(fp, " %-18s %-15s ",
		inet_fmts(r->rt_origin, r->rt_originmask, s1, sizeof(s1)),
		(r->rt_gateway == 0) ? "" : inet_fmt(r->rt_gateway, s2, sizeof(s2)));

	if (r->rt_metric == UNREACHABLE)
	    fprintf(fp, "  NR ");
	else
	    fprintf(fp, "%4u ", r->rt_metric);

	fprintf(fp, "  %3u %c%c %3u   ", r->rt_timer,
		(r->rt_flags & RTF_CHANGED) ? 'C' : '.',
		(r->rt_flags & RTF_HOLDDOWN) ? 'H' : '.',
		r->rt_parent);

	UVIF_FOREACH(vifi, uv) {
	    struct listaddr *n;
	    char l = '[';

	    if (VIFM_ISSET(vifi, r->rt_children)) {
		if ((uv->uv_flags & VIFF_TUNNEL) &&
		    !NBRM_ISSETMASK(uv->uv_nbrmap, r->rt_subordinates))
			/* Don't print out parenthood of a leaf tunnel. */
			continue;

		fprintf(fp, " %u", vifi);
		if (!NBRM_ISSETMASK(uv->uv_nbrmap, r->rt_subordinates))
		    fprintf(fp, "*");

		TAILQ_FOREACH(n, &uv->uv_neighbors, al_link) {
		    if (NBRM_ISSET(n->al_index, r->rt_subordinates)) {
			fprintf(fp, "%c%d", l, n->al_index);
			l = ',';
		    }
		}

		if (l == ',')
		    fprintf(fp, "]");
	    }
	}
	fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
}

struct rtentry *determine_route(uint32_t src)
{
    struct rtentry *rt;

    TAILQ_FOREACH(rt, &rtable, rt_link) {
	if (rt->rt_origin == (src & rt->rt_originmask) &&
	    rt->rt_metric != UNREACHABLE) 
	    break;
    }

    return rt;
}

struct rtentry *route_iter(struct rtentry **rt)
{
    if (!*rt)
	*rt = TAILQ_FIRST(&rtable);
    else
	*rt = TAILQ_NEXT(*rt, rt_link);

    return *rt;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "cc-mode"
 * End:
 */
