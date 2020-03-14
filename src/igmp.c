/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"

#define PIM_QUERY           0
#define PIM_REGISTER        1
#define PIM_REGISTER_STOP   2
#define PIM_JOIN_PRUNE      3
#define PIM_RP_REACHABLE    4
#define PIM_ASSERT          5
#define PIM_GRAFT           6
#define PIM_GRAFT_ACK       7

/*
 * Exported variables.
 */
uint8_t		*recv_buf; 		     /* input packet buffer         */
uint8_t		*send_buf; 		     /* output packet buffer        */
int		igmp_socket;		     /* socket for all network I/O  */
int             router_alert;		     /* IP option Router Alert      */
uint32_t	igmp_query_interval;	     /* Default: 125 sec            */
uint32_t	igmp_robustness;	     /* Default: 2                  */
uint32_t	allhosts_group;		     /* All hosts addr in net order */
uint32_t	allrtrs_group;		     /* All-Routers "  in net order */
uint32_t	allreports_group;	     /* IGMPv3 member reports       */
uint32_t	dvmrp_group;		     /* DVMRP grp addr in net order */
uint32_t	dvmrp_genid;		     /* IGMP generation id          */

/*
 * Local function definitions.
 */
static int	igmp_log_level(uint32_t type, uint32_t code);

/*
 * Open and initialize the igmp socket, and fill in the non-changing
 * IP header fields in the output packet buffer.
 */
void igmp_init(void)
{
    struct ip *ip;
    uint8_t *ip_opt;

    recv_buf = calloc(1, RECV_BUF_SIZE);
    send_buf = calloc(1, RECV_BUF_SIZE);

    if (!recv_buf || !send_buf) {
	logit(LOG_ERR, errno, "Failed allocating Rx/Tx buffers");
	exit(1);
    }

    igmp_socket = socket(AF_INET, SOCK_RAW, IPPROTO_IGMP);
    if (igmp_socket < 0)
	logit(LOG_ERR, errno, "Failed creating IGMP socket");

    k_hdr_include(TRUE);	/* include IP header when sending */
    k_set_rcvbuf(256*1024,48*1024);	/* lots of input buffering        */
    k_set_ttl(1);		/* restrict multicasts to one hop */
    k_set_loop(FALSE);		/* disable multicast loopback     */

    /*
     * Fields zeroed that aren't filled in later:
     * - IP ID (let the kernel fill it in)
     * - Offset (we don't send fragments)
     * - Checksum (let the kernel fill it in)
     */
    ip         = (struct ip *)send_buf;
    ip->ip_v   = IPVERSION;
    ip->ip_hl  = IP_HEADER_RAOPT_LEN >> 2;
    ip->ip_tos = 0xc0;		/* Internet Control */
    ip->ip_ttl = MAXTTL;	/* applies to unicasts only */
    ip->ip_p   = IPPROTO_IGMP;

    /*
     * RFC2113 IP Router Alert.  Per spec this is required to
     * force certain routers/switches to inspect this frame.
     */
    ip_opt    = send_buf + sizeof(struct ip);
    ip_opt[0] = IPOPT_RA;
    ip_opt[1] = 4;
    ip_opt[2] = 0;
    ip_opt[3] = 0;

    allhosts_group   = htonl(INADDR_ALLHOSTS_GROUP);
    dvmrp_group      = htonl(INADDR_DVMRP_GROUP);
    allrtrs_group    = htonl(INADDR_ALLRTRS_GROUP);
    allreports_group = htonl(INADDR_ALLRPTS_GROUP);

    igmp_query_interval = IGMP_QUERY_INTERVAL_DEFAULT;
    igmp_robustness     = IGMP_ROBUSTNESS_DEFAULT;
    router_alert        = 1;
}

void igmp_exit(void)
{
    close(igmp_socket);
    free(recv_buf);
    free(send_buf);
}

char *igmp_packet_kind(uint32_t type, uint32_t code)
{
    static char unknown[20];

    switch (type) {
	case IGMP_MEMBERSHIP_QUERY:		return "membership query  ";
	case IGMP_V1_MEMBERSHIP_REPORT:		return "V1 member report  ";
	case IGMP_V2_MEMBERSHIP_REPORT:		return "V2 member report  ";
	case IGMP_V3_MEMBERSHIP_REPORT:		return "V3 member report  ";
	case IGMP_V2_LEAVE_GROUP:		return "leave message     ";
	case IGMP_DVMRP:
	  switch (code) {
	    case DVMRP_PROBE:			return "neighbor probe    ";
	    case DVMRP_REPORT:			return "route report      ";
	    case DVMRP_ASK_NEIGHBORS:		return "neighbor request  ";
	    case DVMRP_NEIGHBORS:		return "neighbor list     ";
	    case DVMRP_ASK_NEIGHBORS2:		return "neighbor request 2";
	    case DVMRP_NEIGHBORS2:		return "neighbor list 2   ";
	    case DVMRP_PRUNE:			return "prune message     ";
	    case DVMRP_GRAFT:			return "graft message     ";
	    case DVMRP_GRAFT_ACK:		return "graft message ack ";
	    case DVMRP_INFO_REQUEST:		return "info request      ";
	    case DVMRP_INFO_REPLY:		return "info reply        ";
	    default:
		    snprintf(unknown, sizeof(unknown), "unknown DVMRP %3d ", code);
		    return unknown;
	  }
 	case IGMP_PIM:
 	  switch (code) {
 	    case PIM_QUERY:			return "PIM Router-Query  ";
 	    case PIM_REGISTER:			return "PIM Register      ";
 	    case PIM_REGISTER_STOP:		return "PIM Register-Stop ";
 	    case PIM_JOIN_PRUNE:		return "PIM Join/Prune    ";
 	    case PIM_RP_REACHABLE:		return "PIM RP-Reachable  ";
 	    case PIM_ASSERT:			return "PIM Assert        ";
 	    case PIM_GRAFT:			return "PIM Graft         ";
 	    case PIM_GRAFT_ACK:			return "PIM Graft-Ack     ";
 	    default:
 		    snprintf(unknown, sizeof(unknown), "unknown PIM msg%3d", code);
		    return unknown;
 	  }
	case IGMP_MTRACE:			return "IGMP trace query  ";
	case IGMP_MTRACE_RESP:			return "IGMP trace reply  ";
	default:
		snprintf(unknown, sizeof(unknown), "unk: 0x%02x/0x%02x    ", type, code);
		return unknown;
    }
}

int igmp_debug_kind(uint32_t type, uint32_t code)
{
    switch (type) {
	case IGMP_MEMBERSHIP_QUERY:		return DEBUG_IGMP;
	case IGMP_V1_MEMBERSHIP_REPORT:		return DEBUG_IGMP;
	case IGMP_V2_MEMBERSHIP_REPORT:		return DEBUG_IGMP;
	case IGMP_V3_MEMBERSHIP_REPORT:		return DEBUG_IGMP;
	case IGMP_V2_LEAVE_GROUP:		return DEBUG_IGMP;
	case IGMP_DVMRP:
	  switch (code) {
	    case DVMRP_PROBE:			return DEBUG_PEER;
	    case DVMRP_REPORT:			return DEBUG_ROUTE;
	    case DVMRP_ASK_NEIGHBORS:		return 0;
	    case DVMRP_NEIGHBORS:		return 0;
	    case DVMRP_ASK_NEIGHBORS2:		return 0;
	    case DVMRP_NEIGHBORS2:		return 0;
	    case DVMRP_PRUNE:			return DEBUG_PRUNE;
	    case DVMRP_GRAFT:			return DEBUG_PRUNE;
	    case DVMRP_GRAFT_ACK:		return DEBUG_PRUNE;
	    case DVMRP_INFO_REQUEST:		return 0;
	    case DVMRP_INFO_REPLY:		return 0;
	    default:				return 0;
	  }
	case IGMP_PIM:
	  switch (code) {
	    case PIM_QUERY:			return 0;
	    case PIM_REGISTER:			return 0;
	    case PIM_REGISTER_STOP:		return 0;
	    case PIM_JOIN_PRUNE:		return 0;
	    case PIM_RP_REACHABLE:		return 0;
	    case PIM_ASSERT:			return 0;
	    case PIM_GRAFT:			return 0;
	    case PIM_GRAFT_ACK:			return 0;
	    default:				return 0;
	  }
	case IGMP_MTRACE:			return DEBUG_TRACE;
	case IGMP_MTRACE_RESP:			return DEBUG_TRACE;
	default:				return DEBUG_IGMP;
    }

    return 0;
}

/*
 * Process a newly received IGMP packet that is sitting in the input
 * packet buffer.
 */
void accept_igmp(size_t recvlen)
{
    struct igmp *igmp;
    struct ip *ip;
    uint32_t src, dst, group;
    int ipdatalen, iphdrlen, igmpdatalen;
    int igmp_version = 3;

    if (recvlen < sizeof(struct ip)) {
	logit(LOG_INFO, 0, "received packet too short (%zu bytes) for IP header", recvlen);
	return;
    }

    ip        = (struct ip *)recv_buf;
    src       = ip->ip_src.s_addr;
    dst       = ip->ip_dst.s_addr;

    /*
     * this is most likely a message from the kernel indicating that
     * a new src grp pair message has arrived and so, it would be 
     * necessary to install a route into the kernel for this.
     */
    if (ip->ip_p == 0) {
	if (src != 0 && dst != 0)
	    add_table_entry(src, dst);
	return;
    }

    iphdrlen  = ip->ip_hl << 2;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
    ipdatalen = ip->ip_len - iphdrlen;
#else
    ipdatalen = ntohs(ip->ip_len) - iphdrlen;
#endif

    if ((size_t)(iphdrlen + ipdatalen) != recvlen) {
	logit(LOG_INFO, 0,
	      "received packet from %s shorter (%zu bytes) than hdr+data length (%d+%d)",
	      inet_fmt(src, s1, sizeof(s1)), recvlen, iphdrlen, ipdatalen);
	return;
    }

    igmp        = (struct igmp *)(recv_buf + iphdrlen);
    group       = igmp->igmp_group.s_addr;
    igmpdatalen = ipdatalen - IGMP_MINLEN;
    if (igmpdatalen < 0) {
	logit(LOG_INFO, 0,  "received IP data field too short (%u bytes) for IGMP, from %s",
	      ipdatalen, inet_fmt(src, s1, sizeof(s1)));
	return;
    }

    IF_DEBUG(DEBUG_PKT | igmp_debug_kind(igmp->igmp_type, igmp->igmp_code)) {
	logit(LOG_DEBUG, 0, "RECV %s from %-15s to %s",
	      igmp_packet_kind(igmp->igmp_type, igmp->igmp_code),
	      inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
    }

    switch (igmp->igmp_type) {
	case IGMP_MEMBERSHIP_QUERY:
	    /* RFC 3376:7.1 */
	    if (ipdatalen == 8) {
		if (igmp->igmp_code == 0)
		    igmp_version = 1;
		else
		    igmp_version = 2;
	    } else if (ipdatalen >= 12) {
		igmp_version = 3;
	    } else {
		logit(LOG_INFO, 0, "Received invalid IGMP query: Max Resp Code = %d, length = %d",
		      igmp->igmp_code, ipdatalen);
	    }
	    accept_membership_query(src, dst, group, igmp->igmp_code, igmp_version);
	    return;

	case IGMP_V1_MEMBERSHIP_REPORT:
	case IGMP_V2_MEMBERSHIP_REPORT:
	    accept_group_report(src, dst, group, igmp->igmp_type);
	    return;

	case IGMP_V2_LEAVE_GROUP:
	    accept_leave_message(src, dst, group);
	    return;

	case IGMP_V3_MEMBERSHIP_REPORT:
	    if (igmpdatalen < IGMP_V3_GROUP_RECORD_MIN_SIZE) {
		logit(LOG_INFO, 0, "Too short IGMP v3 Membership report: igmpdatalen(%d) < MIN(%d)",
		      igmpdatalen, IGMP_V3_GROUP_RECORD_MIN_SIZE);
		return;
	    }
	    accept_membership_report(src, dst, (struct igmpv3_report *)(recv_buf + iphdrlen), recvlen - iphdrlen);
	    return;

	case IGMP_DVMRP:
	    group = ntohl(group);

	    switch (igmp->igmp_code) {
		case DVMRP_PROBE:
		    accept_probe(src, dst, (char *)(igmp + 1), igmpdatalen, group);
		    return;

		case DVMRP_REPORT:
		    accept_report(src, dst, (char *)(igmp + 1), igmpdatalen, group);
		    return;

		case DVMRP_ASK_NEIGHBORS:
		    accept_neighbor_request(src, dst);
		    return;

		case DVMRP_ASK_NEIGHBORS2:
		    accept_neighbor_request2(src, dst);
		    return;

		case DVMRP_NEIGHBORS:
		    accept_neighbors(src, dst, (uint8_t *)(igmp + 1), igmpdatalen, group);
		    return;

		case DVMRP_NEIGHBORS2:
		    accept_neighbors2(src, dst, (uint8_t *)(igmp + 1), igmpdatalen, group);
		    return;

		case DVMRP_PRUNE:
		    accept_prune(src, dst, (char *)(igmp + 1), igmpdatalen);
		    return;

		case DVMRP_GRAFT:
		    accept_graft(src, dst, (char *)(igmp + 1), igmpdatalen);
		    return;

		case DVMRP_GRAFT_ACK:
		    accept_g_ack(src, dst, (char *)(igmp + 1), igmpdatalen);
		    return;

		case DVMRP_INFO_REQUEST:
		    accept_info_request(src, dst, (uint8_t *)(igmp + 1), igmpdatalen);
		    return;

		case DVMRP_INFO_REPLY:
		    accept_info_reply(src, dst, (uint8_t *)(igmp + 1), igmpdatalen);
		    return;

		default:
		    logit(LOG_INFO, 0,
			  "ignoring unknown DVMRP message code %u from %s to %s",
			  igmp->igmp_code, inet_fmt(src, s1, sizeof(s1)),
			  inet_fmt(dst, s2, sizeof(s2)));
		    return;
	    }
	    break;

 	case IGMP_PIM:
	    break;

	case IGMP_MTRACE_RESP:
	    break;

	case IGMP_MTRACE:
	    accept_mtrace(src, dst, group, (char *)(igmp + 1),
			  igmp->igmp_code, igmpdatalen);
	    break;

	default:
	    logit(LOG_INFO, 0, "ignoring unknown IGMP message type %x from %s to %s",
		  igmp->igmp_type, inet_fmt(src, s1, sizeof(s1)),
		  inet_fmt(dst, s2, sizeof(s2)));
	    break;
    }
}

/*
 * Some IGMP messages are more important than others.  This routine
 * determines the logging level at which to log a send error (often
 * "No route to host").  This is important when there is asymmetric
 * reachability and someone is trying to, i.e., mrinfo me periodically.
 */
static int igmp_log_level(uint32_t type, uint32_t code)
{
    switch (type) {
	case IGMP_MTRACE_RESP:
	    return LOG_INFO;

	case IGMP_DVMRP:
	  switch (code) {
	    case DVMRP_NEIGHBORS:
	    case DVMRP_NEIGHBORS2:
		return LOG_INFO;
	  }
	  break;

	default:
	    break;
    }

    return LOG_WARNING;
}

/*
 * RFC-3376 states that Max Resp Code (MRC) and Querier's Query Interval Code
 * (QQIC) should be presented in floating point value if their value exceeds
 * 128. The following formula is used by IGMPv3 clients to calculate the
 * actual value of the floating point:
 *
 *       0 1 2 3 4 5 6 7
 *      +-+-+-+-+-+-+-+-+
 *      |1| exp | mant  |
 *      +-+-+-+-+-+-+-+-+
 *
 *   QQI / MRT = (mant | 0x10) << (exp + 3)
 *
 * This requires us to find the largest set (fls) bit in the 15-bit number
 * and set the exponent based on its index in the bits 15-8. ie.
 *
 *   exponent 0: igmp_fls(0000 0000 1000 0010)
 *   exponent 5: igmp_fls(0001 0000 0000 0000)
 *   exponent 7: igmp_fls(0111 0101 0000 0000)
 *
 * and set that as the exponent. The mantissa is set to the last 4 bits
 * remaining after the (3 + exponent) shifts to the right.
 *
 * Note!
 * The numbers 31744-32767 are the maximum we can present with floating
 * point that has an exponent of 3 and a mantissa of 4. After this the
 * implementation just wraps around back to zero.
 */
static inline uint8_t igmp_floating_point(unsigned int mantissa)
{
    unsigned int exponent;

    /* Wrap around numbers larger than 2^15, since those can not be
     * presented with 7-bit floating point. */
    mantissa &= 0x00007FFF;

    /* If top 8 bits are zero. */
    if (!(mantissa & 0x00007F80))
        return mantissa;

    /* Shift the mantissa and mark this code floating point. */
    mantissa >>= 3;
    /* At this point the actual exponent (bits 7-5) are still 0, but the
     * exponent might be incremented below. */
    exponent   = 0x00000080;

    /* If bits 7-4 are not zero. */
    if (mantissa & 0x00000F00) {
        mantissa >>= 4;
        /* The index of largest set bit is at least 4. */
        exponent  |= 0x00000040;
    }

    /* If bits 7-6 OR bits 3-2 are not zero. */
    if (mantissa & 0x000000C0) {
        mantissa >>= 2;
        /* The index of largest set bit is atleast 6 if we shifted the
         * mantissa earlier or atleast 2 if we did not shift it. */
        exponent  |= 0x00000020;
    }

    /* If bit 7 OR bit 3 OR bit 1 is not zero. */
    if (mantissa & 0x00000020) {
        mantissa >>= 1;
        /* The index of largest set bit is atleast 7 if we shifted the
         * mantissa two times earlier or atleast 3 if we shifted the
         * mantissa last time or atleast 1 if we did not shift it. */
        exponent  |= 0x00000010;
    }

    return exponent | (mantissa & 0x0000000F);
}

size_t build_query(uint32_t src, uint32_t dst, int type, int code, uint32_t group, int datalen)
{
    struct igmpv3_query *igmp;
    struct ip *ip;
    size_t igmp_len = IGMP_MINLEN + datalen;
    size_t len = IP_HEADER_RAOPT_LEN + igmp_len;

    ip                = (struct ip *)send_buf;
    ip->ip_src.s_addr = src;
    ip->ip_dst.s_addr = dst;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
    ip->ip_len        = len;
#else
    ip->ip_len        = htons(len);
#endif
    if (IN_MULTICAST(ntohl(dst)))
	ip->ip_ttl    = curttl;
    else
	ip->ip_ttl    = MAXTTL;

    igmp = (struct igmpv3_query *)(send_buf + IP_HEADER_RAOPT_LEN);
    memset(igmp, 0, sizeof(*igmp));

    igmp->type        = type;
    if (datalen >= 4)
        igmp->code    = igmp_floating_point(code);
    else
        igmp->code    = code;
    igmp->group       = group;
    igmp->csum        = 0;

    if (datalen >= 4) {
        igmp->qrv     = igmp_robustness;
        igmp->qqic    = igmp_floating_point(igmp_query_interval);
    }

    /* Note: calculate IGMP checksum last. */
    igmp->csum = inet_cksum((uint16_t *)igmp, igmp_len);

    return len;
}

/*
 * Construct an IGMP message in the output packet buffer.  The caller may
 * have already placed data in that buffer, of length 'datalen'.
 */
size_t build_igmp(uint32_t src, uint32_t dst, int type, int code, uint32_t group, int datalen)
{
    struct ip *ip;
    struct igmp *igmp;
    size_t igmp_len = IGMP_MINLEN + datalen;
    size_t len = IP_HEADER_RAOPT_LEN + IGMP_MINLEN + datalen;

    ip                      = (struct ip *)send_buf;
    ip->ip_src.s_addr       = src;
    ip->ip_dst.s_addr       = dst;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
    ip->ip_len              = len;
#else
    ip->ip_len              = htons(len);
#endif
    if (IN_MULTICAST(ntohl(dst)))
	ip->ip_ttl = curttl;
    else
	ip->ip_ttl = MAXTTL;

    igmp                    = (struct igmp *)(send_buf + IP_HEADER_RAOPT_LEN);
    igmp->igmp_type         = type;
    igmp->igmp_code         = code;
    igmp->igmp_group.s_addr = group;
    igmp->igmp_cksum        = 0;
    igmp->igmp_cksum        = inet_cksum((uint16_t *)igmp, igmp_len);

    return len;
}

/*
 * Call build_igmp() to build an IGMP message in the output packet buffer.
 * Then send the message from the interface with IP address 'src' to
 * destination 'dst'.
 */
void send_igmp(uint32_t src, uint32_t dst, int type, int code, uint32_t group, int datalen)
{
    struct sockaddr_in sin;
    struct ip *ip;
    size_t len;
    int rc, setloop = 0;

    /* Set IP header length,  router-alert is optional */
    ip        = (struct ip *)send_buf;
    ip->ip_hl = IP_HEADER_RAOPT_LEN >> 2;

    if (IGMP_MEMBERSHIP_QUERY == type)
       len = build_query(src, dst, type, code, group, datalen);
    else
       len = build_igmp(src, dst, type, code, group, datalen);

    if (IN_MULTICAST(ntohl(dst))) {
	k_set_if(src);
	if (type != IGMP_DVMRP || dst == allhosts_group) {
	    setloop = 1;
	    k_set_loop(TRUE);
	}
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
    sin.sin_len = sizeof(sin);
#endif
    sin.sin_addr.s_addr = dst;

    rc = sendto(igmp_socket, send_buf, len, 0, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0) {
	if (errno == ENETDOWN)
	    check_vif_state();
	else
	    logit(igmp_log_level(type, code), errno, "sendto to %s on %s",
		  inet_fmt(dst, s1, sizeof(s1)), inet_fmt(src, s2, sizeof(s2)));
    }

    if (setloop)
	    k_set_loop(FALSE);

    IF_DEBUG(DEBUG_PKT | igmp_debug_kind(type, code)) {
	logit(LOG_DEBUG, 0, "SENT %s from %-15s to %s", igmp_packet_kind(type, code),
	      src == INADDR_ANY ? "INADDR_ANY" : inet_fmt(src, s1, sizeof(s1)),
	      inet_fmt(dst, s2, sizeof(s2)));
    }
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
