/*
 * Copyright (c) 1993, 1998-2001.
 * The University of Southern California/Information Sciences Institute.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* RSRR code written by Daniel Zappala, USC Information Sciences Institute,
 * April 1995.
 */

/* May 1995 -- Added support for Route Change Notification */

#include "defs.h"
#include <sys/param.h>
#ifdef HAVE_SA_LEN
#include <stddef.h>		/* for offsetof */
#endif

/* 
 * Local RSRR variables.
 */
static int rsrr_socket;		/* interface to reservation protocol */
static char *rsrr_recv_buf;    	/* RSRR receive buffer */
static char *rsrr_send_buf;    	/* RSRR send buffer */

static struct sockaddr_un client_addr;
static socklen_t client_length = sizeof(client_addr);

/*
 * Procedure definitions needed internally.
 */
static void	rsrr_accept(size_t recvlen);
static void	rsrr_accept_iq(void);
static int	rsrr_accept_rq(struct rsrr_rq *route_query, uint8_t flags, struct gtable *gt_notify);
static void	rsrr_read(int);
static int	rsrr_send(int sendlen);
static void	rsrr_cache(struct gtable *gt, struct rsrr_rq *route_query);

/* Initialize RSRR socket */
void rsrr_init(void)
{
    int servlen;
    struct sockaddr_un serv_addr;

    rsrr_recv_buf = calloc(1, RSRR_MAX_LEN);
    rsrr_send_buf = calloc(1, RSRR_MAX_LEN);
    if (!rsrr_recv_buf || !rsrr_send_buf) {
	logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	return;
    }

    rsrr_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (rsrr_socket < 0) {
	logit(LOG_ERR, errno, "Failed creating RSRR socket");
	return;
    }

    unlink(RSRR_SERV_PATH);
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strlcpy(serv_addr.sun_path, RSRR_SERV_PATH, sizeof(serv_addr.sun_path));
#ifdef HAVE_SA_LEN
    servlen = offsetof(struct sockaddr_un, sun_path) + strlen(serv_addr.sun_path);
    serv_addr.sun_len = servlen;
#else
    servlen = sizeof(serv_addr.sun_family) + strlen(serv_addr.sun_path);
#endif
 
    if (bind(rsrr_socket, (struct sockaddr *) &serv_addr, servlen) < 0)
	logit(LOG_ERR, errno, "Cannot bind RSRR socket");

    if (register_input_handler(rsrr_socket, rsrr_read) < 0)
	logit(LOG_ERR, 0, "Could not register RSRR as an input handler");
}

/* Read a message from the RSRR socket */
static void rsrr_read(int fd)
{
    ssize_t rsrr_recvlen;
    
    memset(&client_addr, 0, sizeof(client_addr));
    rsrr_recvlen = recvfrom(fd, rsrr_recv_buf, RSRR_MAX_LEN,
			    0, (struct sockaddr *)&client_addr, &client_length);
    if (rsrr_recvlen < 0) {	
	if (errno != EINTR)
	    logit(LOG_ERR, errno, "RSRR recvfrom");
	return;
    }
    rsrr_accept(rsrr_recvlen);
}

/* Accept a message from the reservation protocol and take
 * appropriate action.
 */
static void rsrr_accept(size_t recvlen)
{
    struct rsrr_header *rsrr = (struct rsrr_header *)rsrr_recv_buf;
    struct rsrr_rq *route_query;
    
    if (recvlen < RSRR_HEADER_LEN) {
	logit(LOG_WARNING, 0, "Received RSRR packet of %zu bytes, which is less than MIN size %zu.",
	      recvlen, RSRR_HEADER_LEN);
	return;
    }
    
    if (rsrr->version > RSRR_MAX_VERSION || rsrr->version != 1) {
	logit(LOG_WARNING, 0, "Received RSRR packet version %d, which I don't understand",
	      rsrr->version);
	return;
    }

    switch (rsrr->type) {
	case RSRR_INITIAL_QUERY:
	    /* Send Initial Reply to client */
	    IF_DEBUG(DEBUG_RSRR) {
		logit(LOG_DEBUG, 0, "Received Initial Query\n");
	    }
	    rsrr_accept_iq();
	    break;

	case RSRR_ROUTE_QUERY:
	    /* Check size */
	    if (recvlen < RSRR_RQ_LEN) {
		logit(LOG_WARNING, 0, "Received Route Query of %zu bytes, which is too small", recvlen);
		break;
	    }
	    /* Get the query */
	    route_query = (struct rsrr_rq *) (rsrr_recv_buf + RSRR_HEADER_LEN);
	    IF_DEBUG(DEBUG_RSRR) {
		logit(LOG_DEBUG, 0,
		      "Received Route Query for src %s grp %s notification %d",
		      inet_fmt(route_query->source_addr.s_addr, s1, sizeof(s1)),
		      inet_fmt(route_query->dest_addr.s_addr, s2, sizeof(s2)),
		      BIT_TST(rsrr->flags,RSRR_NOTIFICATION_BIT));
	    }
	    /* Send Route Reply to client */
	    rsrr_accept_rq(route_query, rsrr->flags, NULL);
	    break;

	default:
	    logit(LOG_WARNING, 0, "Received RSRR packet type %d, which I don't handle", rsrr->type);
	    break;
    }
}

/* Send an Initial Reply to the reservation protocol. */
static void rsrr_accept_iq(void)
{
    struct rsrr_header *rsrr = (struct rsrr_header *)rsrr_send_buf;
    struct rsrr_vif *vif_list;
    struct uvif *v;
    int vifi, sendlen;
    
    /* Check for space.  There should be room for plenty of vifs,
     * but we should check anyway.
     */
    if (numvifs > RSRR_MAX_VIFS) {
	logit(LOG_WARNING, 0,
	      "Cannot send RSRR Route Reply because %u is too many vifs (%d)",
	      numvifs, RSRR_MAX_VIFS);
	return;
    }
    
    /* Set up message */
    rsrr->version = 1;
    rsrr->type = RSRR_INITIAL_REPLY;
    rsrr->flags = 0;
    rsrr->num = numvifs;
    
    vif_list = (struct rsrr_vif *)(rsrr_send_buf + RSRR_HEADER_LEN);
    
    /* Include the vif list. */
    for (vifi = 0, v = uvifs; vifi < numvifs; vifi++, v++) {
	vif_list[vifi].id = vifi;
	vif_list[vifi].status = 0;
	if (v->uv_flags & VIFF_DISABLED)
	    BIT_SET(vif_list[vifi].status,RSRR_DISABLED_BIT);
	vif_list[vifi].threshold = v->uv_threshold;
	vif_list[vifi].local_addr.s_addr = v->uv_lcl_addr;
    }
    
    /* Get the size. */
    sendlen = RSRR_HEADER_LEN + numvifs*RSRR_VIF_LEN;
    
    /* Send it. */
    IF_DEBUG(DEBUG_RSRR) {
	logit(LOG_DEBUG, 0, "Send RSRR Initial Reply");
    }
    rsrr_send(sendlen);
}

/* Send a Route Reply to the reservation protocol.  The Route Query
 * contains the query to which we are responding.  The flags contain
 * the incoming flags from the query or, for route change
 * notification, the flags that should be set for the reply.  The
 * kernel table entry contains the routing info to use for a route
 * change notification.
 */
/* XXX: must modify if your routing table structure/search is different */
static int rsrr_accept_rq(struct rsrr_rq *route_query, uint8_t flags, struct gtable *gt_notify)
{
    struct rsrr_header *rsrr = (struct rsrr_header *)rsrr_send_buf;
    struct rsrr_rr *route_reply;
    struct gtable *gt,local_g;
    struct rtentry *r;
    int sendlen;
    uint32_t mcastgrp;
    
    /* Set up message */
    rsrr->version = 1;
    rsrr->type = RSRR_ROUTE_REPLY;
    rsrr->flags = 0;
    rsrr->num = 0;
    
    route_reply = (struct rsrr_rr *) (rsrr_send_buf + RSRR_HEADER_LEN);
    route_reply->dest_addr.s_addr = route_query->dest_addr.s_addr;
    route_reply->source_addr.s_addr = route_query->source_addr.s_addr;
    route_reply->query_id = route_query->query_id;
    
    /* Blank routing entry for error. */
    route_reply->in_vif = 0;
    route_reply->reserved = 0;
    route_reply->out_vif_bm = 0;
    
    /* Get the size. */
    sendlen = RSRR_RR_LEN;

    /* If kernel table entry is defined, then we are sending a Route Reply
     * due to a Route Change Notification event.  Use the kernel table entry
     * to supply the routing info.
     */
    if (gt_notify && gt_notify->gt_route) {
	/* Set flags */
	rsrr->flags = flags;

	/* Include the routing entry. */
	route_reply->in_vif = gt_notify->gt_route->rt_parent;

	if (BIT_TST(flags, RSRR_NOTIFICATION_BIT))
	    route_reply->out_vif_bm = gt_notify->gt_grpmems;
	else
	    route_reply->out_vif_bm = 0;

    } else if (find_src_grp(route_query->source_addr.s_addr, 0, route_query->dest_addr.s_addr)) {

	/* Found kernel entry. Code taken from add_table_entry() */
	gt = gtp ? gtp->gt_gnext : kernel_table;
	
	/* Include the routing entry. */
	if (gt && gt->gt_route) {
	    route_reply->in_vif     = gt->gt_route->rt_parent;
	    route_reply->out_vif_bm = gt->gt_grpmems;

	    /* Cache reply if using route change notification. */
	    if (BIT_TST(flags, RSRR_NOTIFICATION_BIT)) {
		/* TODO: XXX: Originally the rsrr_cache() call was first, but
		 * I think this is incorrect, because rsrr_cache() checks the
		 * rsrr_send_buf "flag" first.
		 */
		BIT_SET(rsrr->flags, RSRR_NOTIFICATION_BIT);
		rsrr_cache(gt, route_query);
	    }
	}
    } else {

	/* No kernel entry; use routing table. */
	r = determine_route(route_query->source_addr.s_addr);
	if (r) {
	    /* We need to mimic what will happen if a data packet
	     * is forwarded by multicast routing -- the kernel will
	     * make an upcall and mrouted will install a route in the kernel.
	     * Our outgoing vif bitmap should reflect what that table
	     * will look like.  Grab code from add_table_entry().
	     * This is gross, but it's probably better to be accurate.
	     */
	    
	    gt = &local_g;
	    mcastgrp = route_query->dest_addr.s_addr;
	    
	    gt->gt_mcastgrp    	= mcastgrp;
	    gt->gt_grpmems	= 0;
	    gt->gt_scope	= 0;
	    gt->gt_route        = r;
	    
	    /* obtain the multicast group membership list */
	    determine_forwvifs(gt);
	    
	    /* Include the routing entry. */
	    route_reply->in_vif = gt->gt_route->rt_parent;
	    route_reply->out_vif_bm = gt->gt_grpmems;
	} else {
	    /* Set error bit. */
	    BIT_SET(rsrr->flags, RSRR_ERROR_BIT);
	}
    }
    
    IF_DEBUG(DEBUG_RSRR) {
	logit(LOG_DEBUG, 0, "%sSend RSRR Route Reply for src %s dst %s in vif %d out vif %d\n",
	      gt_notify ? "Route Change: " : "",
	      inet_fmt(route_reply->source_addr.s_addr, s1, sizeof(s1)),
	      inet_fmt(route_reply->dest_addr.s_addr, s2, sizeof(s2)),
	      route_reply->in_vif,route_reply->out_vif_bm);
    }

    /* Send it. */
    return rsrr_send(sendlen);
}

/* Send an RSRR message. */
static int rsrr_send(int sendlen)
{
    int error;
    
    /* Send it. */
    error = sendto(rsrr_socket, rsrr_send_buf, sendlen, 0,
		   (struct sockaddr *)&client_addr, client_length);
    
    /* Check for errors. */
    if (error < 0) {
	logit(LOG_WARNING, errno, "Failed send on RSRR socket");
    } else if (error != sendlen) {
	logit(LOG_WARNING, 0,
	    "Sent only %d out of %d bytes on RSRR socket\n", error, sendlen);
    }

    return error;
}

/* TODO: need to sort the rsrr_cache entries for faster access */
/* Cache a message being sent to a client.  Currently only used for
 * caching Route Reply messages for route change notification.
 */
static void rsrr_cache(struct gtable *gt, struct rsrr_rq *route_query)
{
    struct rsrr_cache *rc, **rcnp;
    struct rsrr_header *rsrr = (struct rsrr_header *)rsrr_send_buf;

    rcnp = &gt->gt_rsrr_cache;
    while ((rc = *rcnp)) {
	if ((rc->route_query.source_addr.s_addr == route_query->source_addr.s_addr) &&
	    (rc->route_query.dest_addr.s_addr == route_query->dest_addr.s_addr) &&
	    (!strcmp(rc->client_addr.sun_path,client_addr.sun_path))) {
	    /* Cache entry already exists.
	     * Check if route notification bit has been cleared.
	     */
	    if (!BIT_TST(rsrr->flags, RSRR_NOTIFICATION_BIT)) {
		/* Delete cache entry. */
		*rcnp = rc->next;
		free(rc);
	    } else {
		/* Update */
		/* TODO: XXX: No need to update iif, oifs, flags */
		rc->route_query.query_id = route_query->query_id;
		IF_DEBUG(DEBUG_RSRR) {
		    logit(LOG_DEBUG, 0,
			  "Update cached query id %u from client %s\n",
			  rc->route_query.query_id, rc->client_addr.sun_path);
		}
	    }

	    return;
	}

	rcnp = &rc->next;
    }

    /*
     * Cache entry doesn't already exist.
     * Create one and insert at front of list.
     */
    rc = calloc(1, sizeof(struct rsrr_cache));
    if (!rc) {
	logit(LOG_ERR, errno, "Failed allocating memory in %s:%s()", __FILE__, __func__);
	return;
    }

    rc->route_query.source_addr.s_addr = route_query->source_addr.s_addr;
    rc->route_query.dest_addr.s_addr = route_query->dest_addr.s_addr;
    rc->route_query.query_id = route_query->query_id;
    strlcpy(rc->client_addr.sun_path, client_addr.sun_path, sizeof(rc->client_addr.sun_path));
    rc->client_length = client_length;
    rc->next = gt->gt_rsrr_cache;
    gt->gt_rsrr_cache = rc;

    IF_DEBUG(DEBUG_RSRR) {
	logit(LOG_DEBUG, 0, "Cached query id %u from client %s\n",
	      rc->route_query.query_id, rc->client_addr.sun_path);
    }
}

/* Send all the messages in the cache for particular routing entry.
 * Currently this is used to send all the cached Route Reply messages
 * for route change notification.
 */
void rsrr_cache_send(struct gtable *gt, int notify)
{
    struct rsrr_cache *rc, **rcnp;
    uint8_t flags = 0;

    if (notify) {
	BIT_SET(flags, RSRR_NOTIFICATION_BIT);
    }

    rcnp = &gt->gt_rsrr_cache;
    while ((rc = *rcnp)) {
	if (rsrr_accept_rq(&rc->route_query, flags, gt) < 0) {
	    IF_DEBUG(DEBUG_RSRR) {
		logit(LOG_DEBUG, 0, "Deleting cached query id %u from client %s\n",
		      rc->route_query.query_id, rc->client_addr.sun_path);
	    }
	    /* Delete cache entry. */
	    *rcnp = rc->next;
	    free(rc);
	} else {
	    rcnp = &rc->next;
	}
    }
}

/* Clean the cache by deleting all entries. */
void rsrr_cache_clean(struct gtable *gt)
{
    struct rsrr_cache *rc, *rc_next;

    IF_DEBUG(DEBUG_RSRR) {
	logit(LOG_DEBUG, 0, "cleaning cache for group %s\n",
	      inet_fmt(gt->gt_mcastgrp, s1, sizeof(s1)));
    }
    rc = gt->gt_rsrr_cache;
    while (rc) {
	rc_next = rc->next;
	free(rc);
	rc = rc_next;
    }
    gt->gt_rsrr_cache = NULL;
}

void rsrr_clean(void)
{
    unlink(RSRR_SERV_PATH);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
