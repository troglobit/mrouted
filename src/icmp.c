/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"

static int	icmp_socket;

static void	icmp_handler(int);
static char *	icmp_name(struct icmp *);

void init_icmp(void)
{
    if ((icmp_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0)
	logit(LOG_ERR, errno, "ICMP socket");

    register_input_handler(icmp_socket, icmp_handler);

    IF_DEBUG(DEBUG_ICMP)
    logit(LOG_DEBUG, 0, "registering icmp socket fd %d\n", icmp_socket);
}

static void icmp_handler(int fd)
{
    uint8_t icmp_buf[RECV_BUF_SIZE];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t len;
    int iphdrlen, ipdatalen;
    struct icmp *icmp;
    struct ip *ip;
    vifi_t i;
    struct uvif *v;
    uint32_t src;

    memset(icmp_buf, 0, sizeof(icmp_buf));
    while ((len = recvfrom(fd, icmp_buf, sizeof(icmp_buf), 0, (struct sockaddr *)&from, &fromlen)) < 0) {
	if (errno == EINTR)
	    continue;

	logit(LOG_WARNING, errno, "ICMP");
	return;
    }

    ip        = (struct ip *)icmp_buf;
    iphdrlen  = ip->ip_hl << 2;

    /* Sanity check resulting header size */
    if (iphdrlen > 60) {
	logit(LOG_WARNING, 0, "Received an invadlid ICMP frame.");
	return;
    }

#ifdef HAVE_IP_HDRINCL_BSD_ORDER
    ipdatalen = ip->ip_len - iphdrlen;
#else
    ipdatalen = ntohs(ip->ip_len) - iphdrlen;
#endif

    if (iphdrlen + ipdatalen != len) {
	/* Malformed ICMP, just return. */
	IF_DEBUG(DEBUG_ICMP) {
	    logit(LOG_DEBUG, 0, "hdr %d data %d != rcv %zd", iphdrlen, ipdatalen, len);
	}

	return;
    }

    if (ipdatalen < ICMP_MINLEN + (int)sizeof(struct ip)) {
	/* Not enough data for us to be interested in it. */
	return;
    }

    src = ip->ip_src.s_addr;
    icmp = (struct icmp *)(icmp_buf + iphdrlen);
    IF_DEBUG(DEBUG_ICMP) {
	logit(LOG_DEBUG, 0, "got ICMP type %d from %s",
	      icmp->icmp_type, inet_fmt(src, s1, sizeof(s1)));
    }

    /*
     * Eventually:
     * have registry of ICMP listeners, by type, code and ICMP_ID
     * (and maybe fields of the original packet too -- maybe need a
     * generalized packet filter!) to allow ping and traceroute
     * from the monitoring tool.
     */
    switch (icmp->icmp_type) {
	case ICMP_UNREACH:
	case ICMP_TIMXCEED:
	    /* Look at returned packet to see if it's us sending on a tunnel */
	    ip = &icmp->icmp_ip;
	    if (ip->ip_p != IPPROTO_IGMP && ip->ip_p != IPPROTO_IPIP)
		return;
	    for (v = uvifs, i = 0; i < numvifs; v++, i++) {
		if (ip->ip_src.s_addr == v->uv_lcl_addr &&
		    ip->ip_dst.s_addr == v->uv_dst_addr) {
		    char *p;
		    int n;
		    /*
		     * I sent this packet on this vif.
		     */
		    n = ++v->uv_icmp_warn;
		    while (n && !(n & 1))
			n >>= 1;
		    if (n == 1 && ((p = icmp_name(icmp)) != NULL))
			logit(LOG_WARNING, 0, "Received ICMP %s from %s %s %s on vif %d",
			    p, inet_fmt(src, s1, sizeof(s1)), "for traffic sent to",
			    inet_fmt(ip->ip_dst.s_addr, s2, sizeof(s2)),
			    i);

		    break;
		}
	    }
	    break;
    }
}

/*
 * Return NULL for ICMP informational messages.
 * Return string describing the error for ICMP errors.
 */
static char *icmp_name(struct icmp *icmp)
{
    static char retval[30];

    switch (icmp->icmp_type) {
	case ICMP_UNREACH:
	    switch (icmp->icmp_code) {
		case ICMP_UNREACH_NET:
		    return "network unreachable";
		case ICMP_UNREACH_HOST:
		    return "host unreachable";
		case ICMP_UNREACH_PROTOCOL:
		    return "protocol unreachable";
		case ICMP_UNREACH_PORT:
		    return "port unreachable";
		case ICMP_UNREACH_NEEDFRAG:
		    return "needs fragmentation";
		case ICMP_UNREACH_SRCFAIL:
		    return "source route failed";
#ifndef ICMP_UNREACH_NET_UNKNOWN
#define ICMP_UNREACH_NET_UNKNOWN	6
#endif
		case ICMP_UNREACH_NET_UNKNOWN:
		    return "network unknown";
#ifndef ICMP_UNREACH_HOST_UNKNOWN
#define ICMP_UNREACH_HOST_UNKNOWN	7
#endif
		case ICMP_UNREACH_HOST_UNKNOWN:
		    return "host unknown";
#ifndef ICMP_UNREACH_ISOLATED
#define ICMP_UNREACH_ISOLATED		8
#endif
		case ICMP_UNREACH_ISOLATED:
		    return "source host isolated";
#ifndef ICMP_UNREACH_NET_PROHIB
#define ICMP_UNREACH_NET_PROHIB		9
#endif
		case ICMP_UNREACH_NET_PROHIB:
		    return "network access prohibited";
#ifndef ICMP_UNREACH_HOST_PROHIB
#define ICMP_UNREACH_HOST_PROHIB	10
#endif
		case ICMP_UNREACH_HOST_PROHIB:
		    return "host access prohibited";
#ifndef ICMP_UNREACH_TOSNET
#define ICMP_UNREACH_TOSNET		11
#endif
		case ICMP_UNREACH_TOSNET:
		    return "bad TOS for net";
#ifndef ICMP_UNREACH_TOSHOST
#define ICMP_UNREACH_TOSHOST		12
#endif
		case ICMP_UNREACH_TOSHOST:
		    return "bad TOS for host";
#ifndef ICMP_UNREACH_FILTER_PROHIB
#define ICMP_UNREACH_FILTER_PROHIB	13
#endif
		case ICMP_UNREACH_FILTER_PROHIB:
		    return "prohibited by filter";
#ifndef ICMP_UNREACH_HOST_PRECEDENCE
#define ICMP_UNREACH_HOST_PRECEDENCE	14
#endif
		case ICMP_UNREACH_HOST_PRECEDENCE:
		    return "host precedence violation";
#ifndef ICMP_UNREACH_PRECEDENCE_CUTOFF
#define ICMP_UNREACH_PRECEDENCE_CUTOFF	15
#endif
		case ICMP_UNREACH_PRECEDENCE_CUTOFF:
		    return "precedence cutoff";
		default:
		    snprintf(retval, sizeof(retval), "unreachable code %d", icmp->icmp_code);
		    return retval;
	    }
	case ICMP_SOURCEQUENCH:
	    return "source quench";
	case ICMP_REDIRECT:
	    return NULL;	/* XXX */
	case ICMP_TIMXCEED:
	    switch (icmp->icmp_code) {
		case ICMP_TIMXCEED_INTRANS:
		    return "time exceeded in transit";
		case ICMP_TIMXCEED_REASS:
		    return "time exceeded in reassembly";
		default:
		    snprintf(retval, sizeof(retval), "time exceeded code %d", icmp->icmp_code);
		    return retval;
	    }
	case ICMP_PARAMPROB:
	    switch (icmp->icmp_code) {
#ifndef ICMP_PARAMPROB_OPTABSENT
#define ICMP_PARAMPROB_OPTABSENT	1
#endif
		case ICMP_PARAMPROB_OPTABSENT:
		    return "required option absent";
		default:
		    snprintf(retval, sizeof(retval), "parameter problem code %d", icmp->icmp_code);
		    return retval;
	    }
    }
    return NULL;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
