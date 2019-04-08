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
#ifdef notyet
int		raw_socket;		    /* socket for raw network I/O  */
#endif
/*
 *XXX For now, we just use the IGMP socket to send packets.
 * This is legal in BSD, because the protocol # is not checked
 * on raw sockets.  The k_* interfaces need to gain a socket
 * argument so that we can call them on the raw_socket also.
 */
#define	raw_socket	igmp_socket

/*
 * Private variables.
 */
static int rawid = 0;

/*
 * Open and initialize the raw socket.
 */
void init_ipip(void)
{
#ifdef notyet
    if ((raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
	logit(LOG_ERR, errno, "Raw IP socket");
#endif
}

/*
 * Allocate and fill in static IP header for encapsulating on a tunnel.
 */
void init_ipip_on_vif(struct uvif *v)
{
    struct ip *ip;

    if (v->uv_encap_hdr)
	return;

    ip = v->uv_encap_hdr = calloc(1, sizeof(struct ip));
    if (!ip) {
	logit(LOG_ERR, errno, "Out of memory when setting up IPIP tunnel");
	return;			/* Never reached */
    }

    /*
     * Fields zeroed that aren't filled in later:
     * - IP ID (let the kernel fill it in)
     * - Offset (we don't send fragments)
     * - Checksum (let the kernel fill it in)
     */
    memset(ip, 0, sizeof(struct ip));
    ip->ip_v   = IPVERSION;
    ip->ip_hl  = sizeof(struct ip) >> 2;
    ip->ip_tos = 0xc0;		/* Internet Control */
    ip->ip_ttl = MAXTTL;	/* applies to unicasts only */
    ip->ip_p   = IPPROTO_IPIP;
    ip->ip_src.s_addr = v->uv_lcl_addr;
    ip->ip_dst.s_addr = v->uv_rmt_addr;
}

/*
 * Call build_igmp() to build an IGMP message in the output packet buffer.
 * Then fill in the fields of the IP packet that build_igmp() left for the
 * kernel to fill in, and encapsulate the original packet with the
 * pre-created ip header for this vif.
 */
void send_ipip(uint32_t src, uint32_t dst, int type, int code, uint32_t group, int datalen, struct uvif *v)
{
    struct msghdr msg;
    struct iovec iov[2];
    struct sockaddr_in sdst;
    struct ip *ip;

    build_igmp(src, dst, type, code, group, datalen);
    ip = (struct ip *)send_buf;
    ip->ip_id = htons(rawid++);
    ip->ip_sum = 0;
    ip->ip_sum = inet_cksum((uint16_t *)ip, ip->ip_hl << 2);

    ip = v->uv_encap_hdr;
    ip->ip_len = 2 * MIN_IP_HEADER_LEN + IGMP_MINLEN + datalen;
#ifndef HAVE_IP_HDRINCL_BSD_ORDER
    ip->ip_len = htons(ip->ip_len);
#endif

    memset(&sdst, 0, sizeof(sdst));
    sdst.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
    sdst.sin_len = sizeof(sdst);
#endif
    sdst.sin_addr = ip->ip_dst;

    iov[0].iov_base = (caddr_t)v->uv_encap_hdr;
    iov[0].iov_len = sizeof(struct ip);
    iov[1].iov_base = (caddr_t)send_buf;
    iov[1].iov_len = MIN_IP_HEADER_LEN + IGMP_MINLEN + datalen;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (caddr_t)&sdst;
    msg.msg_namelen = sizeof(sdst);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (sendmsg(raw_socket, &msg, 0) < 0) {
	if (errno == ENETDOWN)
	    check_vif_state();
	else
	    logit(LOG_WARNING, errno,
		"sendmsg to %s on %s",
		inet_fmt(sdst.sin_addr.s_addr, s1, sizeof(s1)), inet_fmt(src, s2, sizeof(s2)));
    }

    IF_DEBUG(DEBUG_PKT|igmp_debug_kind(type, code))
    logit(LOG_DEBUG, 0, "SENT %s from %-15s to %s encaped to %s",
	igmp_packet_kind(type, code), src == INADDR_ANY ? "INADDR_ANY" :
				 inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)),
				 inet_fmt(sdst.sin_addr.s_addr, s3, sizeof(s3)));
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
