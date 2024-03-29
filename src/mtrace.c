/*
 * mtrace.c
 *
 * This tool traces the branch of a multicast tree from a source to a
 * receiver for a particular multicast group and gives statistics
 * about packet rate and loss for each hop along the path.  It can
 * usually be invoked just as
 *
 * 	mtrace source
 *
 * to trace the route from that source to the local host for a default
 * group when only the route is desired and not group-specific packet
 * counts.  See the usage line for more complex forms.
 *
 *
 * Released 4 Apr 1995.  This program was adapted by Steve Casner
 * (USC/ISI) from a prototype written by Ajit Thyagarajan (UDel and
 * Xerox PARC).  It attempts to parallel in command syntax and output
 * format the unicast traceroute program written by Van Jacobson (LBL)
 * for the parts where that makes sense.
 *
 * Copyright (c) 1998-2001.
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

#include "defs.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <ifaddrs.h>
#include <memory.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef SUNOS5
#include <sys/systeminfo.h>
#endif
#include <sys/time.h>

#define DEFAULT_TIMEOUT	3	/* How long to wait before retrying requests */
#define DEFAULT_RETRIES 3	/* How many times to try */
#define MAXHOPS UNREACHABLE	/* Don't need more hops than max metric */
#define UNICAST_TTL 255		/* TTL for unicast response */
#define MULTICAST_TTL1 64	/* Default TTL for multicast query/response */
#define MULTICAST_TTL_INC 32	/* TTL increment for increase after timeout */
#define MULTICAST_TTL_MAX 192	/* Maximum TTL allowed (protect low-BW links */

struct resp_buf {
    uint32_t qtime;		/* Time query was issued */
    uint32_t rtime;		/* Time response was received */
    int	len;			/* Number of reports or length of data */
    struct igmp igmp;		/* IGMP header */
    union {
	struct {
	    struct tr_query q;		/* Query/response header */
	    struct tr_resp r[MAXHOPS];	/* Per-hop reports */
	} t;
	char d[MAX_DVMRP_DATA_LEN];	/* Neighbor data */
    } u;
} base, incr[2];

#define qhdr u.t.q
#define resps u.t.r
#define ndata u.d

char names[MAXHOPS][40];
int reset[MAXHOPS];			/* To get around 3.4 bug, ... */
int swaps[MAXHOPS];			/* To get around 3.6 bug, ... */

int timeout = DEFAULT_TIMEOUT;
int nqueries = DEFAULT_RETRIES;
int numeric = FALSE;
int debug = 0;
int mrt_table_id = 0;		        /* dummy, unused */
int passive = FALSE;
int multicast = FALSE;
int statint = 10;
int verbose = 0;

uint32_t defgrp;			/* Default group if not specified */
uint32_t query_cast;			/* All routers multicast addr */
uint32_t resp_cast;			/* Mtrace response multicast addr */

uint32_t lcl_addr = 0;			/* This host address, in NET order */
uint32_t dst_netmask;			/* netmask to go with qdst */

/*
 * Query/response parameters, all initialized to zero and set later
 * to default values or from options.
 */
uint32_t qsrc  = 0;		/* Source address in the query */
uint32_t qgrp  = 0;		/* Group address in the query */
uint32_t qdst  = 0;		/* Destination (receiver) address in query */
uint8_t  qno   = 0;		/* Max number of hops to query */
uint32_t raddr = 0;		/* Address where response should be sent */
int      qttl  = 0;		/* TTL for the query packet */
uint8_t  rttl  = 0;		/* TTL for the response packet */
uint32_t gwy   = 0;		/* User-supplied last-hop router address */
uint32_t tdst  = 0;		/* Address where trace is sent (last-hop) */

vifi_t  numvifs;		/* to keep loader happy */
				/* (see kern.c) */

uint32_t		host_addr(char *name);
/* uint32_t is promoted uint8_t */
char *			proto_type(uint32_t type);
char *			flag_type(uint32_t type);

uint32_t		get_netmask(int s, uint32_t dst);
int			get_ttl(struct resp_buf *buf);
int			t_diff(uint32_t a, uint32_t b);
uint32_t		fixtime(uint32_t time);
int			send_recv(uint32_t dst, int type, int code, int tries, struct resp_buf *save);
char *			print_host(uint32_t addr);
char *			print_host2(uint32_t addr1, uint32_t addr2);
void			print_trace(int index, struct resp_buf *buf);
int			what_kind(struct resp_buf *buf, char *why);
char *			scale(int *hop);
void			stat_line(struct tr_resp *r, struct tr_resp *s, int have_next, int *res);
int			print_stats(struct resp_buf *base, struct resp_buf *prev, struct resp_buf *new);
void			check_vif_state(void);
uint32_t		byteswap(uint32_t v);


uint32_t host_addr(char *name)
{
    uint32_t addr;
    int	i, dots = 3;
    char buf[40];
    char *ip = name;
    char *op = buf;

    /*
     * Undo BSD's favor -- take fewer than 4 octets as net/subnet address
     * if the name is all numeric.
     */
    for (i = sizeof(buf) - 7; i > 0; --i) {
	if (*ip == '.')
	    --dots;
	else if (*ip == '\0')
	    break;
	else if (!isdigit((int)*ip))
	    dots = 0;  /* Not numeric, don't add zeroes */

	*op++ = *ip++;
    }
    for (i = 0; i < dots; ++i) {
	*op++ = '.';
	*op++ = '0';
    }
    *op = '\0';

    if (dots <= 0) {
	struct sockaddr_in *sin;
	struct addrinfo *result;
	struct addrinfo hints;
	int rc;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	rc = getaddrinfo(name, NULL, &hints, &result);
	if (rc)
	    goto fallback;

	sin = (struct sockaddr_in *)result->ai_addr;
	addr = sin->sin_addr.s_addr;

	freeaddrinfo(result);
    } else {
      fallback:
	addr = inet_addr(buf);
	if (addr == INADDR_NONE) {
	    addr = 0;
	    printf("Could not parse %s as host name or address\n", name);
	}
    }

    return addr;
}


char *proto_type(uint32_t type)
{
    static char buf[80];

    switch (type) {
	case PROTO_DVMRP:
	    return "DVMRP";

	case PROTO_MOSPF:
	    return "MOSPF";

	case PROTO_PIM:
	    return "PIM";

	case PROTO_CBT:
	    return "CBT";

	default:
	    snprintf(buf, sizeof buf, "Unknown protocol code %d", type);
	    return buf;
    }
}


char *flag_type(uint32_t type)
{
    static char buf[80];

    switch (type) {
	case TR_NO_ERR:
	    return "";

	case TR_WRONG_IF:
	    return "Wrong interface";

	case TR_PRUNED:
	    return "Prune sent upstream";

	case TR_OPRUNED:
	    return "Output pruned";

	case TR_SCOPED:
	    return "Hit scope boundary";

	case TR_NO_RTE:
	    return "No route";

	case TR_OLD_ROUTER:
	    return "Next router no mtrace";

	case TR_NO_FWD:
	    return "Not forwarding";

	case TR_NO_SPACE:
	    return "No space in packet";

	default:
	    snprintf(buf, sizeof buf, "Unknown error code %d", type);
	    return buf;
    }
}

/*
 * If destination is on a local net, get the netmask, else set the
 * netmask to all ones.  There are two side effects: if the local
 * address was not explicitly set, and if the destination is on a
 * local net, use that one; in either case, verify that the local
 * address is valid.
 */
uint32_t get_netmask(int s, uint32_t dst)
{
    uint32_t if_addr, if_mask;
    uint32_t retval = 0xFFFFFFFF;
    int found = FALSE;
    struct ifaddrs *ifap, *ifa;

    if (getifaddrs(&ifap) != 0) {
	perror("getifaddrs");
	return retval;
    }

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
	if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
	    continue;

	if_addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
	if_mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr;
	if ((dst & if_mask) == (if_addr & if_mask)) {
	    retval = if_mask;
	    if (lcl_addr == 0)
		lcl_addr = if_addr;
	}
	if (lcl_addr == if_addr)
	    found = TRUE;
    }

    if (!found && lcl_addr != 0) {
	printf("Interface address is not valid\n");
	exit(1);
    }
    freeifaddrs(ifap);

    return retval;
}


int get_ttl(struct resp_buf *buf)
{
    struct tr_resp *b;
    uint32_t ttl;
    int rno;

    if (buf && (rno = buf->len) > 0) {
	b = buf->resps + rno - 1;
	ttl = b->tr_fttl;

	while (--rno > 0) {
	    --b;
	    if (ttl < b->tr_fttl)
		ttl = b->tr_fttl;
	    else
		++ttl;
	}

	ttl += MULTICAST_TTL_INC;
	if (ttl < MULTICAST_TTL1)
	    ttl = MULTICAST_TTL1;
	if (ttl > MULTICAST_TTL_MAX)
	    ttl = MULTICAST_TTL_MAX;

	return ttl;
    } else
	return(MULTICAST_TTL1);
}

/*
 * Calculate the difference between two 32-bit NTP timestamps and return
 * the result in milliseconds.
 */
int t_diff(uint32_t a, uint32_t b)
{
    int d = a - b;

    return (d * 125) >> 13;
}

/*
 * Fixup for incorrect time format in 3.3 mrouted.
 * This is possible because (JAN_1970 mod 64K) is quite close to 32K,
 * so correct and incorrect times will be far apart.
 */
uint32_t fixtime(uint32_t time)
{
    if (abs((int)(time-base.qtime)) > 0x3FFFFFFF)
        time = ((time & 0xFFFF0000) + (JAN_1970 << 16)) +
	    ((time & 0xFFFF) << 14) / 15625;
    return time;
}

/*
 * Swap bytes for poor little-endian machines that don't byte-swap
 */
uint32_t byteswap(uint32_t v)
{
    return ((v << 24) | ((v & 0xff00) << 8) |
	    ((v >> 8) & 0xff00) | (v >> 24));
}

int send_recv(uint32_t dst, int type, int code, int tries, struct resp_buf *save)
{
    struct timeval tq, tr, tv;
    struct ip *ip;
    struct igmp *igmp;
    struct tr_query *query, *rquery;
    size_t ipdatalen, iphdrlen;
    uint32_t local, group;
    int datalen;
    struct pollfd pfd[1];
    int count, len, i;
    size_t igmpdatalen;
    ssize_t recvlen;
    socklen_t dummy = 0;

    if (type == IGMP_MTRACE) {
	group = qgrp;
	datalen = sizeof(struct tr_query);
    } else {
	group = htonl(MROUTED_LEVEL);
	datalen = 0;
    }

    if (IN_MULTICAST(ntohl(dst)))
	local = lcl_addr;
    else
	local = INADDR_ANY;

    /*
     * If the reply address was not explicitly specified, start off
     * with the unicast address of this host.  Then, if there is no
     * response after trying half the tries with unicast, switch to
     * the standard multicast reply address.  If the TTL was also not
     * specified, set a multicast TTL and if needed increase it for the
     * last quarter of the tries.
     */
    query = (struct tr_query *)(send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN);
    query->tr_raddr = raddr ? raddr : multicast ? resp_cast : lcl_addr;
    query->tr_rttl  = rttl ? rttl :
	IN_MULTICAST(ntohl(query->tr_raddr)) ? get_ttl(save) : UNICAST_TTL;
    query->tr_src   = qsrc;
    query->tr_dst   = qdst;

    for (i = tries ; i > 0; --i) {
	if (tries == nqueries && raddr == 0) {
	    if (i == ((nqueries + 1) >> 1)) {
		query->tr_raddr = resp_cast;
		if (rttl == 0)
		    query->tr_rttl = get_ttl(save);
	    }
	    if (i <= ((nqueries + 3) >> 2) && rttl == 0) {
		query->tr_rttl += MULTICAST_TTL_INC;
		if (query->tr_rttl > MULTICAST_TTL_MAX)
		    query->tr_rttl = MULTICAST_TTL_MAX;
	    }
	}

	/*
	 * Change the qid for each request sent to avoid being confused
	 * by duplicate responses
	 */
	query->tr_qid  = ((uint32_t)random() >> 8);

	/*
	 * Set timer to calculate delays, then send query
	 */
	gettimeofday(&tq, 0);
	send_igmp(local, dst, type, code, group, datalen);

	/*
	 * Wait for response, discarding false alarms
	 */
	pfd[0].fd = igmp_socket;
	pfd[0].events = POLLIN;
	while (TRUE) {
	    gettimeofday(&tv, 0);
	    tv.tv_sec = tq.tv_sec + timeout - tv.tv_sec;
	    tv.tv_usec = tq.tv_usec - tv.tv_usec;
	    if (tv.tv_usec < 0) tv.tv_usec += 1000000L, --tv.tv_sec;
	    if (tv.tv_sec < 0) tv.tv_sec = tv.tv_usec = 0;

	    count = poll(pfd, 1, tv.tv_sec * 1000);

	    if (count < 0) {
		if (errno != EINTR) perror("poll");
		continue;
	    }
	    if (count == 0) {
		printf("* ");
		fflush(stdout);
		break;
	    }

	    gettimeofday(&tr, 0);
	    recvlen = recvfrom(igmp_socket, recv_buf, RECV_BUF_SIZE, 0, NULL, &dummy);
	    if (recvlen <= 0) {
		if (recvlen && errno != EINTR) perror("recvfrom");
		continue;
	    }

	    if ((size_t)recvlen < sizeof(struct ip)) {
		fprintf(stderr, "packet too short (%zd bytes) for IP header", recvlen);
		continue;
	    }

	    ip = (struct ip *)recv_buf;
	    if (ip->ip_p == 0)	/* ignore cache creation requests */
		continue;

	    iphdrlen = ip->ip_hl << 2;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
	    ipdatalen = ip->ip_len - iphdrlen;
#else
	    ipdatalen = ntohs(ip->ip_len) - iphdrlen;
#endif
	    if (iphdrlen + ipdatalen != (size_t)recvlen) {
		fprintf(stderr, "packet shorter (%zd bytes) than hdr+data len (%zu+%zu)\n",
			recvlen, iphdrlen, ipdatalen);
		continue;
	    }

	    igmp = (struct igmp *) (recv_buf + iphdrlen);
	    if (ipdatalen < IGMP_MINLEN) {
		fprintf(stderr,
			"IP data field too short (%zu bytes) for IGMP from %s\n",
			ipdatalen, inet_fmt(ip->ip_src.s_addr, s1, sizeof(s1)));
		continue;
	    }
	    if (ipdatalen > MAX_DVMRP_DATA_LEN) {
		fprintf(stderr,
			"IP data field too long (%zu bytes) for IGMP from %s\n",
			ipdatalen, inet_fmt(ip->ip_src.s_addr, s1, sizeof(s1)));
		continue;
	    }
	    igmpdatalen = ipdatalen - IGMP_MINLEN;

	    switch (igmp->igmp_type) {

		case IGMP_DVMRP:
		    if (igmp->igmp_code != DVMRP_NEIGHBORS2)
			continue;
		    len = igmpdatalen;
		    /*
		     * Accept DVMRP_NEIGHBORS2 response if it comes from the
		     * address queried or if that address is one of the local
		     * addresses in the response.
		     */
		    if (ip->ip_src.s_addr != dst) {
			uint32_t *p = (uint32_t *)(igmp + 1);
			uint32_t *ep = p + (len >> 2);
			while (p < ep) {
			    uint32_t laddr = *p++;
			    int n = ntohl(*p++) & 0xFF;
			    if (laddr == dst) {
				ep = p + 1;		/* ensure p < ep after loop */
				break;
			    }
			    p += n;
			}
			if (p >= ep)
			    continue;
		    }
		    break;

		case IGMP_MTRACE:	    /* For backward compatibility with 3.3 */
		case IGMP_MTRACE_RESP:
		    if (igmpdatalen <= QLEN)
			continue;
		    if ((igmpdatalen - QLEN) % RLEN) {
			printf("packet with incorrect datalen\n");
			continue;
		    }

		    /*
		     * Ignore responses that don't match query.
		     */
		    rquery = (struct tr_query *)(igmp + 1);
		    if (rquery->tr_qid != query->tr_qid)
			continue;
		    if (rquery->tr_src != qsrc)
			continue;
		    if (rquery->tr_dst != qdst)
			continue;
		    len = (igmpdatalen - QLEN) / RLEN;

		    /*
		     * Ignore trace queries passing through this node when
		     * mtrace is run on an mrouter that is in the path
		     * (needed only because IGMP_MTRACE is accepted above
		     * for backward compatibility with multicast release 3.3).
		     */
		    if (igmp->igmp_type == IGMP_MTRACE) {
			struct tr_resp *r = (struct tr_resp *)(rquery+1) + len - 1;
			uint32_t smask;

			VAL_TO_MASK(smask, r->tr_smask);
			if (len < code && (r->tr_inaddr & smask) != (qsrc & smask)
			    && r->tr_rmtaddr != 0 && !(r->tr_rflags & 0x80))
			    continue;
		    }

		    /*
		     * A match, we'll keep this one.
		     */
		    if (len > code) {
			fprintf(stderr,
				"Num hops received (%d) exceeds request (%d)\n",
				len, code);
		    }
		    rquery->tr_raddr = query->tr_raddr;	/* Insure these are */
		    rquery->tr_rttl = query->tr_rttl;	/* as we sent them */
		    break;

		default:
		    continue;
	    }

	    /*
	     * Most of the sanity checking done at this point.
	     * Return this packet we have been waiting for.
	     */
	    if (save) {
		save->qtime = ((tq.tv_sec + JAN_1970) << 16) +
		    (tq.tv_usec << 10) / 15625;
		save->rtime = ((tr.tv_sec + JAN_1970) << 16) +
		    (tr.tv_usec << 10) / 15625;
		save->len = len;
		memmove((char *)&save->igmp, (char *)igmp, ipdatalen);
	    }

	    return recvlen;
	}
    }

    return 0;
}

/*
 * Most of this code is duplicated elsewhere.  I'm not sure if
 * the duplication is absolutely required or not.
 *
 * Ideally, this would keep track of ongoing statistics
 * collection and print out statistics.  (& keep track
 * of h-b-h traces and only print the longest)  For now,
 * it just snoops on what traces it can.
 */
void passive_mode(void)
{
    struct timeval tr;
    struct ip *ip;
    struct igmp *igmp;
    struct tr_resp *r;
    size_t ipdatalen, iphdrlen, igmpdatalen;
    size_t len;
    ssize_t recvlen;
    socklen_t dummy = 0;
    uint32_t smask;

    if (raddr) {
	if (IN_MULTICAST(ntohl(raddr)))
	    k_join(raddr, INADDR_ANY);
    } else {
	k_join(htonl(0xE0000120), INADDR_ANY);
    }

    while (1) {
	recvlen = recvfrom(igmp_socket, recv_buf, RECV_BUF_SIZE,
			   0, (struct sockaddr *)0, &dummy);
	gettimeofday(&tr, 0);

	if (recvlen <= 0) {
	    if (recvlen && errno != EINTR) perror("recvfrom");
	    continue;
	}

	if ((size_t)recvlen < sizeof(struct ip)) {
	    fprintf(stderr, "packet too short (%zd bytes) for IP header", recvlen);
	    continue;
	}

	ip = (struct ip *)recv_buf;
	if (ip->ip_p == 0)	/* ignore cache creation requests */
	    continue;

	iphdrlen = ip->ip_hl << 2;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
	ipdatalen = ip->ip_len - iphdrlen;
#else
	ipdatalen = ntohs(ip->ip_len) - iphdrlen;
#endif
	if (iphdrlen + ipdatalen != (size_t)recvlen) {
	    fprintf(stderr, "packet shorter (%zd bytes) than hdr+data len (%zu+%zu)\n",
		    recvlen, iphdrlen, ipdatalen);
	    continue;
	}

	igmp = (struct igmp *)(recv_buf + iphdrlen);
	if (ipdatalen < IGMP_MINLEN) {
	    fprintf(stderr, "IP data field too short (%zu bytes) for IGMP from %s\n",
		    ipdatalen, inet_fmt(ip->ip_src.s_addr, s1, sizeof(s1)));
	    continue;
	}
	if (ipdatalen > MAX_DVMRP_DATA_LEN) {
	    fprintf(stderr,
		    "IP data field too long (%zu bytes) for IGMP from %s\n",
		    ipdatalen, inet_fmt(ip->ip_src.s_addr, s1, sizeof(s1)));
	    continue;
	}
	igmpdatalen = ipdatalen - IGMP_MINLEN;

	switch (igmp->igmp_type) {
	    case IGMP_MTRACE:	    /* For backward compatibility with 3.3 */
	    case IGMP_MTRACE_RESP:
		if (igmpdatalen < QLEN)
		    continue;

		if ((igmpdatalen - QLEN) % RLEN) {
		    printf("packet with incorrect datalen\n");
		    continue;
		}

		len = (igmpdatalen - QLEN)/RLEN;
		break;

	    default:
		continue;
	}

	base.qtime = ((tr.tv_sec + JAN_1970) << 16) +
	    (tr.tv_usec << 10) / 15625;
	base.rtime = ((tr.tv_sec + JAN_1970) << 16) +
	    (tr.tv_usec << 10) / 15625;
	base.len = len;
	memmove((char *)&base.igmp, (char *)igmp, ipdatalen);
	/*
	 * If the user specified which traces to monitor,
	 * only accept traces that correspond to the
	 * request
	 */
	if ((qsrc != 0 && qsrc != base.qhdr.tr_src) ||
	    (qdst != 0 && qdst != base.qhdr.tr_dst) ||
	    (qgrp != 0 && qgrp != igmp->igmp_group.s_addr))
	    continue;

	printf("Mtrace from %s to %s via group %s (mxhop=%d)\n",
	       inet_fmt(base.qhdr.tr_dst, s1, sizeof(s1)), inet_fmt(base.qhdr.tr_src, s2, sizeof(s2)),
	       inet_fmt(igmp->igmp_group.s_addr, s3, sizeof(s3)), igmp->igmp_code);
	if (len == 0)
	    continue;
	printf("  0  ");
	print_host(base.qhdr.tr_dst);
	printf("\n");
	print_trace(1, &base);
	r = base.resps + base.len - 1;
	VAL_TO_MASK(smask, r->tr_smask);
	if ((r->tr_inaddr & smask) == (base.qhdr.tr_src & smask)) {
	    printf("%3d  ", -(base.len+1));
	    print_host(base.qhdr.tr_src);
	    printf("\n");
	} else if (r->tr_rmtaddr != 0) {
	    printf("%3d  ", -(base.len+1));
	    what_kind(&base, r->tr_rflags == TR_OLD_ROUTER ?
		      "doesn't support mtrace"
		      : "is the next hop");
	}
	printf("\n");
    }
}

char *print_host(uint32_t addr)
{
    return print_host2(addr, 0);
}

/*
 * On some routers, one interface has a name and the other doesn't.
 * We always print the address of the outgoing interface, but can
 * sometimes get the name from the incoming interface.  This might be
 * confusing but should be slightly more helpful than just a "?".
 */
char *print_host2(uint32_t addr1, uint32_t addr2)
{
    char *name;

    if (numeric) {
	printf("%s", inet_fmt(addr1, s1, sizeof(s1)));
	return "";
    }

    name = inet_name(addr1, 0);
    if (!name)
	name = inet_name(addr2, 0);
    if (!name)
	name = "?";
    printf("%s (%s)", name, inet_fmt(addr1, s1, sizeof(s1)));

    return name;
}

/*
 * Print responses as received (reverse path from dst to src)
 */
void print_trace(int index, struct resp_buf *buf)
{
    struct tr_resp *r;
    char *name;
    int i;
    int hop;
    char *ms;

    i = abs(index);
    r = buf->resps + i - 1;

    for (; i <= buf->len; ++i, ++r) {
	if (index > 0)
	    printf("%3d  ", -i);
	name = print_host2(r->tr_outaddr, r->tr_inaddr);
	printf("  %s  thresh^ %d", proto_type(r->tr_rproto), r->tr_fttl);
	if (verbose) {
	    hop = t_diff(fixtime(ntohl(r->tr_qarr)), buf->qtime);
	    ms = scale(&hop);
	    printf("  %d%s", hop, ms);
	}
	printf("  %s\n", flag_type(r->tr_rflags));
	memcpy(names[i-1], name, sizeof(names[0]) - 1);
	names[i-1][sizeof(names[0])-1] = '\0';
    }
}

/*
 * See what kind of router is the next hop
 */
int what_kind(struct resp_buf *buf, char *why)
{
    uint32_t smask;
    int retval;
    int hops = buf->len;
    struct tr_resp *r = buf->resps + hops - 1;
    uint32_t next = r->tr_rmtaddr;

    retval = send_recv(next, IGMP_DVMRP, DVMRP_ASK_NEIGHBORS2, 1, &incr[0]);
    print_host(next);
    if (retval) {
	uint32_t version = ntohl(incr[0].igmp.igmp_group.s_addr);
	uint32_t *p = (uint32_t *)incr[0].ndata;
	uint32_t *ep = p + (incr[0].len >> 2);
	char *type = "";

	retval = 0;
	switch (version & 0xFF) {
	    case 1:
		type = "proteon/mrouted ";
		retval = 1;
		break;

	    case 2:
	    case 3:
		if (((version >> 8) & 0xFF) < 3) retval = 1;
		/* Fall through */
	    case 4:
		type = "mrouted ";
		break;

	    case 10:
		type = "cisco ";
	}
	printf(" [%s%d.%d] %s\n",
	       type, version & 0xFF, (version >> 8) & 0xFF,
	       why);
	VAL_TO_MASK(smask, r->tr_smask);
	while (p < ep) {
	    uint32_t laddr = *p++;
	    int flags = (ntohl(*p) & 0xFF00) >> 8;
	    int n = ntohl(*p++) & 0xFF;
	    if (!(flags & (DVMRP_NF_DOWN | DVMRP_NF_DISABLED)) &&
		(laddr & smask) == (qsrc & smask)) {
		printf("%3d  ", -(hops+2));
		print_host(qsrc);
		printf("\n");
		return 1;
	    }
	    p += n;
	}

	return retval;
    }

    printf(" %s\n", why);

    return 0;
}


char *scale(int *hop)
{
    if (*hop > -1000 && *hop < 10000)
	return " ms";

    *hop /= 1000;
    if (*hop > -1000 && *hop < 10000)
	return " s ";

    return "s ";
}

/*
 * Calculate and print one line of packet loss and packet rate statistics.
 * Checks for count of all ones from mrouted 2.3 that doesn't have counters.
 */
#define NEITHER 0
#define INS     1
#define OUTS    2
#define BOTH    3
void stat_line(struct tr_resp *r, struct tr_resp *s, int have_next, int *rst)
{
    int timediff;
    int v_lost, v_pct;
    int g_lost, g_pct;
    int v_out = ntohl(s->tr_vifout) - ntohl(r->tr_vifout);
    int g_out = ntohl(s->tr_pktcnt) - ntohl(r->tr_pktcnt);
    int v_pps, g_pps;
    char v_str[8], g_str[8];
    int have = NEITHER;
    int res = *rst;

    timediff = (fixtime(ntohl(s->tr_qarr)) -
		fixtime(ntohl(r->tr_qarr))) >> 16;
    if (timediff == 0)
	timediff = 1;
    v_pps = v_out / timediff;
    g_pps = g_out / timediff;

    if (v_out && ((s->tr_vifout != 0xFFFFFFFF && s->tr_vifout != 0) ||
                  (r->tr_vifout != 0xFFFFFFFF && r->tr_vifout != 0)))
	have |= OUTS;

    if (have_next) {
	--r,  --s,  --rst;
	if ((s->tr_vifin != 0xFFFFFFFF && s->tr_vifin != 0) ||
	    (r->tr_vifin != 0xFFFFFFFF && r->tr_vifin != 0))
	    have |= INS;
	if (*rst)
	    res = 1;
    }

    switch (have) {
	case BOTH:
	    v_lost = v_out - (ntohl(s->tr_vifin) - ntohl(r->tr_vifin));
	    v_pct = (v_lost * 100 + (v_out >> 1)) / v_out;

	    if (-100 < v_pct && v_pct < 101 && v_out > 10)
		snprintf(v_str, sizeof v_str, "%3d", v_pct);
	    else
		memcpy(v_str, " --", 4);

	    g_lost = g_out - (ntohl(s->tr_pktcnt) - ntohl(r->tr_pktcnt));
	    if (g_out) g_pct = (g_lost * 100 + (g_out >> 1))/ g_out;
	    else g_pct = 0;
	    if (-100 < g_pct && g_pct < 101 && g_out > 10)
		snprintf(g_str, sizeof g_str, "%3d", g_pct);
	    else memcpy(g_str, " --", 4);

	    printf("%6d/%-5d=%s%%%4d pps",
		   v_lost, v_out, v_str, v_pps);
	    if (res)
		printf("\n");
	    else
		printf("%6d/%-5d=%s%%%4d pps\n",
		       g_lost, g_out, g_str, g_pps);
	    break;

	case INS:
	    v_out = ntohl(s->tr_vifin) - ntohl(r->tr_vifin);
	    v_pps = v_out / timediff;
	    /* Fall through */

	case OUTS:
	    printf("       %-5d     %4d pps",
		   v_out, v_pps);
	    if (res)
		printf("\n");
	    else
		printf("       %-5d     %4d pps\n",
		       g_out, g_pps);
	    break;

	case NEITHER:
	    printf("\n");
	    break;
    }

    if (debug > 2) {
	printf("\t\t\t\tv_in: %u ", ntohl(s->tr_vifin));
	printf("v_out: %u ", ntohl(s->tr_vifout));
	printf("pkts: %u\n", ntohl(s->tr_pktcnt));
	printf("\t\t\t\tv_in: %u ", ntohl(r->tr_vifin));
	printf("v_out: %u ", ntohl(r->tr_vifout));
	printf("pkts: %u\n", ntohl(r->tr_pktcnt));
	printf("\t\t\t\tv_in: %u ", ntohl(s->tr_vifin)-ntohl(r->tr_vifin));
	printf("v_out: %u ", ntohl(s->tr_vifout) - ntohl(r->tr_vifout));
	printf("pkts: %u ", ntohl(s->tr_pktcnt) - ntohl(r->tr_pktcnt));
	printf("time: %d\n", timediff);
	printf("\t\t\t\tres: %d\n", res);
    }
}

/*
 * Print responses with statistics for forward path (from src to dst)
 */
int print_stats(struct resp_buf *base, struct resp_buf *prev, struct resp_buf *new)
{
    int rtt, hop;
    char *ms;
    uint32_t smask;
    int rno = base->len - 1;
    struct tr_resp *b = base->resps + rno;
    struct tr_resp *p = prev->resps + rno;
    struct tr_resp *n = new->resps + rno;
    int *r = reset + rno;
    uint32_t resptime = new->rtime;
    uint32_t qarrtime = fixtime(ntohl(n->tr_qarr));
    uint32_t ttl = n->tr_fttl;
    int first = (base == prev);

    VAL_TO_MASK(smask, b->tr_smask);
    printf("  Source        Response Dest");
    printf("    Packet Statistics For     Only For Traffic\n");
    printf("%-15s %-15s  All Multicast Traffic     From %s\n",
	   ((b->tr_inaddr & smask) == (qsrc & smask)) ? s1 : "   * * *       ",
	   inet_fmt(base->qhdr.tr_raddr, s2, sizeof(s2)), inet_fmt(qsrc, s1, sizeof(s1)));
    rtt = t_diff(resptime, new->qtime);
    ms = scale(&rtt);
    printf("     %c       __/  rtt%5d%s    Lost/Sent = Pct  Rate       To %s\n",
	   first ? 'v' : '|', rtt, ms, inet_fmt(qgrp, s2, sizeof(s2)));
    if (!first) {
	hop = t_diff(resptime, qarrtime);
	ms = scale(&hop);
	printf("     v      /     hop%5d%s", hop, ms);
	printf("    ---------------------     --------------------\n");
    }
    if (debug > 2) {
	printf("\t\t\t\tv_in: %u ", ntohl(n->tr_vifin));
	printf("v_out: %u ", ntohl(n->tr_vifout));
	printf("pkts: %u\n", ntohl(n->tr_pktcnt));
	printf("\t\t\t\tv_in: %u ", ntohl(b->tr_vifin));
	printf("v_out: %u ", ntohl(b->tr_vifout));
	printf("pkts: %u\n", ntohl(b->tr_pktcnt));
	printf("\t\t\t\tv_in: %u ", ntohl(n->tr_vifin) - ntohl(b->tr_vifin));
	printf("v_out: %u ", ntohl(n->tr_vifout) - ntohl(b->tr_vifout));
	printf("pkts: %u\n", ntohl(n->tr_pktcnt) - ntohl(b->tr_pktcnt));
	printf("\t\t\t\treset: %d\n", *r);
    }

    while (TRUE) {
	if ((n->tr_inaddr != b->tr_inaddr) || (p->tr_inaddr != b->tr_inaddr))
	    return 1;		/* Route changed */

	if ((n->tr_inaddr != n->tr_outaddr))
	    printf("%-15s\n", inet_fmt(n->tr_inaddr, s1, sizeof(s1)));
	printf("%-15s %-14s %s\n", inet_fmt(n->tr_outaddr, s1, sizeof(s1)), names[rno],
	       flag_type(n->tr_rflags));

	if (rno-- < 1)
	    break;

	printf("     %c     ^      ttl%5d   ", first ? 'v' : '|', ttl);
	stat_line(p, n, TRUE, r);
	if (!first) {
	    resptime = qarrtime;
	    qarrtime = fixtime(ntohl((n-1)->tr_qarr));
	    hop = t_diff(resptime, qarrtime);
	    ms = scale(&hop);
	    printf("     v     |      hop%5d%s", hop, ms);
	    stat_line(b, n, TRUE, r);
	}

	--b, --p, --n, --r;
	if (ttl < n->tr_fttl)
	    ttl = n->tr_fttl;
	else
	    ++ttl;
    }

    printf("     %c      \\__   ttl%5d   ", first ? 'v' : '|', ttl);
    stat_line(p, n, FALSE, r);
    if (!first) {
	hop = t_diff(qarrtime, new->qtime);
	ms = scale(&hop);
	printf("     v         \\  hop%5d%s", hop, ms);
	stat_line(b, n, FALSE, r);
    }
    printf("%-15s %s\n", inet_fmt(qdst, s1, sizeof(s1)), inet_fmt(lcl_addr, s2, sizeof(s2)));
    printf("  Receiver      Query Source\n\n");

    return 0;
}


int usage(int code)
{
    printf("Usage: mtrace [-lMnpsv] [-d level] [-g gateway] [-i if_addr] [-m max_hops]\n"
	   "              [-q nqueries] [-r host] [-S stat_int] [-t ttl] [-w waittime]\n"
	   "              source [receiver] [group]\n");
    return code;
}

int main(int argc, char *argv[])
{
    int udp;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t recvlen;
    struct timeval tv;
    struct resp_buf *prev, *new;
    struct tr_resp *r;
    uint32_t smask;
    int rno;
    int hops, nexthop, tries;
    uint32_t lastout = 0;
    int numstats = 1;
    int waittime, ch;
    unsigned int seed;
    uid_t uid;
    const char *errstr;

    while ((ch = getopt(argc, argv, "d:g:hi:lm:Mnpq:r:sS:t:vw:")) != -1) {
        switch (ch) {
            case 'd':			/* Unlisted debug print option */
                debug = strtonum(optarg, 0, 3, &errstr);
                if (errstr) {
                    warnx("debug level %s", errstr);
                    debug = 3;
                }
                break;

            case 'g':			/* Last-hop gateway (dest of query) */
		gwy = host_addr(optarg);
                break;

	    case 'h':
		return usage(0);

            case 'l':			/* Loop updating stats indefinitely */
		numstats = 3153600;
		break;

            case 'm':			/* Max number of hops to trace */
                qno = strtonum(optarg, 1, MAXHOPS, &errstr);
                if (errstr) {
                    warnx("max hops %s", errstr);
                    qno = 0;
                }
                break;

            case 'M':			/* Use multicast for response */
		multicast = TRUE;
		break;

            case 'n':			/* Don't reverse map host addresses */
		numeric = TRUE;
		break;

            case 'p':			/* Passive listen for traces */
		passive = TRUE;
		break;

            case 'q':			/* Number of query retries */
                nqueries = strtonum(optarg, 1, 65535, &errstr);
                if (errstr) {
                    warnx("query retries %s", errstr);
                    qno = 0;
                }
                break;

            case 'r':			/* Dest for response packet */
		raddr = host_addr(optarg);
                break;

            case 's':			/* Short form, don't wait for stats */
		numstats = 0;
		break;

            case 'S':			/* Stat accumulation interval */
                statint = strtonum(optarg, 1, 65535, &errstr);
                if (errstr) {
                    warnx("stat accumulation interval %s", errstr);
                    statint = 10;
                }
                break;

            case 't':			/* TTL for query packet */
                qttl = strtonum(optarg, 1, 32767, &errstr);
                if (errstr) {
                    warnx("TTL for query packet %s", errstr);
                    qttl = 0;
                }
                rttl = qttl;
                break;

            case 'v':			/* Verbosity */
		verbose = TRUE;
		break;

            case 'w':			/* Time to wait for packet arrival */
                timeout = strtonum(optarg, 1, 65535, &errstr);
                if (errstr) {
                    warnx("TTL for query packet %s", errstr);
                    timeout = DEFAULT_TIMEOUT;
                }
                break;

            case 'i':			/* Local interface address */
		lcl_addr = host_addr(optarg);
                break;

            default:
		return usage(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (geteuid() != 0) {
        fprintf(stderr, "mtrace: must be root\n");
        exit(1);
    }

    igmp_init();

    uid = getuid();
    if (setuid(uid) == -1)
	err(1, "setuid");

    if (argc > 0 && (qsrc = host_addr(argv[0]))) {          /* Source of path */
	if (IN_MULTICAST(ntohl(qsrc)))
	    return usage(1);

	argv++;
	argc--;

	if (argc > 0 && (qdst = host_addr(argv[0]))) {      /* Dest of path */
	    argv++;
	    argc--;

	    if (argc > 0 && (qgrp = host_addr(argv[0]))) {  /* Path via group */
		argv++;
		argc--;
	    }

	    if (IN_MULTICAST(ntohl(qdst))) {
		uint32_t temp = qdst;

		qdst = qgrp;
		qgrp = temp;
		if (IN_MULTICAST(ntohl(qdst)))
		    return usage(1);
	    } else if (qgrp && !IN_MULTICAST(ntohl(qgrp)))
		return usage(1);
	}
    }

    if (passive) {
	passive_mode();
	return(0);
    }

    if (argc > 0 || qsrc == 0)
        return usage(1);

    /*
     * Set useful defaults for as many parameters as possible.
     */

    defgrp = htonl(0xE0020001);		/* MBone Audio (224.2.0.1) */
    query_cast = htonl(0xE0000002);	/* All routers multicast addr */
    resp_cast = htonl(0xE0000120);	/* Mtrace response multicast addr */
    if (qgrp == 0)
	qgrp = defgrp;

    /*
     * Get default local address for multicasts to use in setting defaults.
     */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_addr.s_addr = qgrp;
    addr.sin_port = htons(2000);	/* Any port above 1024 will do */

    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0 ||
	connect(udp, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	getsockname(udp, (struct sockaddr *)&addr, &addrlen) < 0) {
	perror("Determining local address");
	exit(1);
    }

#ifdef SUNOS5
    /*
     * SunOS 5.X prior to SunOS 2.6, getsockname returns 0 for udp socket.
     * This call to sysinfo will return the hostname.
     * If the default multicast interface (set with the route
     * for 224.0.0.0) is not the same as the hostname,
     * mtrace -i [if_addr] will have to be used.
     */
    if (addr.sin_addr.s_addr == 0) {
	struct hostent *hp;
	char myhostname[MAXHOSTNAMELEN];
	int error;

	error = sysinfo(SI_HOSTNAME, myhostname, sizeof(myhostname));
	if (error == -1) {
	    perror("Getting my hostname");
	    exit(1);
	}

	/* Note: skipped conversion to getaddrinfo() because SunOS 5.x */
	hp = gethostbyname(myhostname);
	if (hp == NULL || hp->h_addrtype != AF_INET ||
	    hp->h_length != sizeof(addr.sin_addr)) {
	    perror("Finding IP address for my hostname");
	    exit(1);
	}

	memcpy((char *)&addr.sin_addr.s_addr, hp->h_addr, hp->h_length);
    }
#endif

    /*
     * Default destination for path to be queried is the local host.
     */
    if (qdst == 0)
	qdst = lcl_addr ? lcl_addr : addr.sin_addr.s_addr;
    dst_netmask = get_netmask(udp, qdst);
    close(udp);
    if (lcl_addr == 0)
	lcl_addr = addr.sin_addr.s_addr;

    /*
     * Initialize the seed for random query identifiers.
     */
    gettimeofday(&tv, 0);
    seed = tv.tv_usec ^ lcl_addr;
    srandom(seed);

    /*
     * Protect against unicast queries to mrouted versions that might crash.
     */
    if (gwy && !IN_MULTICAST(ntohl(gwy)))
	if (send_recv(gwy, IGMP_DVMRP, DVMRP_ASK_NEIGHBORS2, 1, &incr[0])) {
	    int version;

	    version = ntohl(incr[0].igmp.igmp_group.s_addr) & 0xFFFF;
	    if (version == 0x0303 || version == 0x0503) {
		printf("Don't use -g to address an mrouted 3.%d, it might crash\n",
		       (version >> 8) & 0xFF);
		exit(0);
	    }
	}

    printf("Mtrace from %s to %s via group %s\n",
	   inet_fmt(qsrc, s1, sizeof(s1)),
	   inet_fmt(qdst, s2, sizeof(s2)),
	   inet_fmt(qgrp, s3, sizeof(s3)));

    if ((qdst & dst_netmask) == (qsrc & dst_netmask)) {
	printf("Source & receiver are directly connected, no path to trace\n");
	exit(0);
    }

    /*
     * If the response is to be a multicast address, make sure we 
     * are listening on that multicast address.
     */
    if (raddr) {
	if (IN_MULTICAST(ntohl(raddr)))
	    k_join(raddr, lcl_addr);
    } else {
	k_join(resp_cast, lcl_addr);
    }

    /*
     * If the destination is on the local net, the last-hop router can
     * be found by multicast to the all-routers multicast group.
     * Otherwise, use the group address that is the subject of the
     * query since by definition the last-hop router will be a member.
     * Set default TTLs for local remote multicasts.
     */
  restart:

    if (gwy == 0) {
	if ((qdst & dst_netmask) == (lcl_addr & dst_netmask))
	    tdst = query_cast;
	else
	    tdst = qgrp;
    } else {
        tdst = gwy;
    }

    if (IN_MULTICAST(ntohl(tdst))) {
	k_set_loop(1);	/* If I am running on a router, I need to hear this */
	if (tdst == query_cast)
	    k_set_ttl(qttl ? qttl : 1);
	else
	    k_set_ttl(qttl ? qttl : MULTICAST_TTL1);
    }

    /*
     * Try a query at the requested number of hops or MAXHOPS if unspecified.
     */
    if (qno == 0) {
	hops = MAXHOPS;
	tries = 1;
	printf("Querying full reverse path... ");
	fflush(stdout);
    } else {
	hops = qno;
	tries = nqueries;
	printf("Querying reverse path, maximum %d hops... ", qno);
	fflush(stdout);
    }
    base.rtime = 0;
    base.len = 0;

    recvlen = send_recv(tdst, IGMP_MTRACE, hops, tries, &base);

    /*
     * If the initial query was successful, print it.  Otherwise, if
     * the query max hop count is the default of zero, loop starting
     * from one until there is no response for four hops.  The extra
     * hops allow getting past an mtrace-capable mrouter that can't
     * send multicast packets because all phyints are disabled.
     */
    if (recvlen) {
	printf("\n  0  ");
	print_host(qdst);
	printf("\n");
	print_trace(1, &base);
	r = base.resps + base.len - 1;
	if (r->tr_rflags == TR_OLD_ROUTER || r->tr_rflags == TR_NO_SPACE ||
	    qno != 0) {
	    printf("%3d  ", -(base.len+1));
	    what_kind(&base, r->tr_rflags == TR_OLD_ROUTER ?
		      "doesn't support mtrace"
		      : "is the next hop");
	} else {
	    VAL_TO_MASK(smask, r->tr_smask);
	    if ((r->tr_inaddr & smask) == (qsrc & smask)) {
		printf("%3d  ", -(base.len+1));
		print_host(qsrc);
		printf("\n");
	    }
	}
    } else if (qno == 0) {
	printf("switching to hop-by-hop:\n  0  ");
	print_host(qdst);
	printf("\n");

	for (hops = 1, nexthop = 1; hops <= MAXHOPS; ++hops) {
	    printf("%3d  ", -hops);
	    fflush(stdout);

	    /*
	     * After a successful first hop, try switching to the unicast
	     * address of the last-hop router instead of multicasting the
	     * trace query.  This should be safe for mrouted versions 3.3
	     * and 3.5 because there is a long route timeout with metric
	     * infinity before a route disappears.  Switching to unicast
	     * reduces the amount of multicast traffic and avoids a bug
	     * with duplicate suppression in mrouted 3.5.
	     */
	    if (hops == 2 && gwy == 0 &&
		(recvlen = send_recv(lastout, IGMP_MTRACE, hops, 1, &base)))
		tdst = lastout;
	    else recvlen = send_recv(tdst, IGMP_MTRACE, hops, nqueries, &base);

	    if (recvlen == 0) {
		if (hops == 1) break;
		if (hops == nexthop) {
		    if (what_kind(&base, "didn't respond")) {
			/* the ask_neighbors determined that the
			 * not-responding router is the first-hop. */
			break;
		    }
		} else if (hops < nexthop + 3) {
		    printf("\n");
		} else {
		    printf("...giving up\n");
		    break;
		}
		continue;
	    }
	    r = base.resps + base.len - 1;
	    if (base.len == hops &&
		(hops == 1 || (base.resps+nexthop-2)->tr_outaddr == lastout)) {
	    	if (hops == nexthop) {
		    print_trace(-hops, &base);
		} else {
		    printf("\nResuming...\n");
		    print_trace(nexthop, &base);
		}
	    } else {
		if (base.len < hops) {
		    /*
		     * A shorter trace than requested means a fatal error
		     * occurred along the path, or that the route changed
		     * to a shorter one.
		     *
		     * If the trace is longer than the last one we received,
		     * then we are resuming from a skipped router (but there
		     * is still probably a problem).
		     *
		     * If the trace is shorter than the last one we
		     * received, then the route must have changed (and
		     * there is still probably a problem).
		     */
		    if (nexthop <= base.len) {
			printf("\nResuming...\n");
			print_trace(nexthop, &base);
		    } else if (nexthop > base.len + 1) {
			hops = base.len;
			printf("\nRoute must have changed...\n");
			print_trace(1, &base);
		    }
		} else {
		    /*
		     * The last hop address is not the same as it was;
		     * the route probably changed underneath us.
		     */
		    hops = base.len;
		    printf("\nRoute must have changed...\n");
		    print_trace(1, &base);
		}
	    }
	    lastout = r->tr_outaddr;

	    if (base.len < hops ||
		r->tr_rmtaddr == 0 || (r->tr_rflags & 0x80)) {
		VAL_TO_MASK(smask, r->tr_smask);
		if (r->tr_rmtaddr) {
		    if (hops != nexthop)
			printf("\n%3d  ", -(base.len+1));

		    what_kind(&base, r->tr_rflags == TR_OLD_ROUTER ?
			      "doesn't support mtrace" :
			      "would be the next hop");
		    /* XXX could do segmented trace if TR_NO_SPACE */
		} else if (r->tr_rflags == TR_NO_ERR &&
			   (r->tr_inaddr & smask) == (qsrc & smask)) {
		    printf("%3d  ", -(hops + 1));
		    print_host(qsrc);
		    printf("\n");
		}
		break;
	    }

	    nexthop = hops + 1;
	}
    }

    if (base.rtime == 0) {
	printf("Timed out receiving responses\n");
	if (IN_MULTICAST(ntohl(tdst))) {
            if (tdst == query_cast)
                printf("Perhaps no local router has a route for source %s\n",
                       inet_fmt(qsrc, s1, sizeof(s1)));
            else
                printf("Perhaps receiver %s is not a member of group %s,\n"
		       "or no router local to it has a route for source %s,\n"
		       "or multicast at ttl %d doesn't reach its last-hop router for that source\n",
                       inet_fmt(qdst, s2, sizeof(s2)), inet_fmt(qgrp, s3, sizeof(s3)), inet_fmt(qsrc, s1, sizeof(s1)),
                       qttl ? qttl : MULTICAST_TTL1);
        }
	exit(1);
    }

    printf("Round trip time %d ms\n\n", t_diff(base.rtime, base.qtime));

    /*
     * Use the saved response which was the longest one received,
     * and make additional probes after delay to measure loss.
     */
    raddr = base.qhdr.tr_raddr;
    rttl = base.qhdr.tr_rttl;
    gettimeofday(&tv, 0);
    waittime = statint - (((tv.tv_sec + JAN_1970) & 0xFFFF) - (base.qtime >> 16));
    prev = &base;
    new = &incr[numstats&1];

    while (numstats--) {
	if (waittime < 1) {
	    printf("\n");
	} else {
	    printf("Waiting to accumulate statistics... ");
	    fflush(stdout);
	    sleep((unsigned)waittime);
	}

	rno = base.len;
	recvlen = send_recv(tdst, IGMP_MTRACE, rno, nqueries, new);
	if (recvlen == 0) {
	    printf("Timed out.\n");
	    exit(1);
	}

	if (rno != new->len) {
	    printf("Trace length doesn't match:\n");
	    /*
	     * XXX Should this trace result be printed, or is that
	     * too verbose?  Perhaps it should just say restarting.
	     * But if the path is changing quickly, this may be the
	     * only snapshot of the current path.  But, if the path
	     * is changing that quickly, does the current path really
	     * matter?
	     */
	    print_trace(1, new);
	    printf("Restarting.\n\n");
	    numstats++;
	    goto restart;
	}

	printf("Results after %d seconds:\n\n", (int)((new->qtime - base.qtime) >> 16));
	if (print_stats(&base, prev, new)) {
	    printf("Route changed:\n");
	    print_trace(1, new);
	    printf("Restarting.\n\n");
	    goto restart;
	}
	prev = new;
	new = &incr[numstats&1];
	waittime = statint;
    }

    /*
     * If the response was multicast back, leave the group
     */
    if (raddr) {
	if (IN_MULTICAST(ntohl(raddr)))
	    k_leave(raddr, lcl_addr);
    } else {
	k_leave(resp_cast, lcl_addr);
    }

    return 0;
}

void check_vif_state(void)
{
    logit(LOG_WARNING, errno, "sendto");
}

/*
 * Log errors and other messages to stderr, according to the severity
 * of the message and the current debug level.  For errors of severity
 * LOG_ERR or worse, terminate the program.
 */
void logit(int severity, int syserr, const char *format, ...)
{
    va_list ap;

    switch (debug) {
	case 0:
	    if (severity > LOG_WARNING)
		return;
	    /* fallthrough */
	case 1:
	    if (severity > LOG_NOTICE)
		return;
	    /* fallthrough */
	case 2:
	    if (severity > LOG_INFO)
		return;
	    /* fallthrough */
	default:
	    if (severity == LOG_WARNING)
		fprintf(stderr, "warning - ");
	    va_start(ap, format);
	    vfprintf(stderr, format, ap);
	    va_end(ap);
	    if (syserr == 0)
		fprintf(stderr, "\n");
	    else
		fprintf(stderr, ": %s\n", strerror(syserr));
    }

    if (severity <= LOG_ERR)
	exit(1);
}

/* dummies */
void accept_probe(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
}

void accept_group_report(int ifi, uint32_t src, uint32_t dst, uint32_t group, int r_type)
{
}

void accept_neighbor_request2(uint32_t src, uint32_t dst)
{
}

void accept_report(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
}

void accept_neighbor_request(uint32_t src, uint32_t dst)
{
}

void accept_prune(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
}

void accept_graft(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
}

void accept_g_ack(uint32_t src, uint32_t dst, char *p, size_t datalen)
{
}

void add_table_entry(uint32_t origin, uint32_t mcastgrp)
{
}

void accept_leave_message(int ifi, uint32_t src, uint32_t dst, uint32_t group)
{
}

void accept_mtrace(uint32_t src, uint32_t dst, uint32_t group, char *data, uint8_t no, size_t datalen)
{
}

void accept_membership_query(int ifi, uint32_t src, uint32_t dst, uint32_t group, int tmo, int ver)
{
}

void accept_membership_report(int ifi, uint32_t src, uint32_t dst, struct igmpv3_report *report, ssize_t len)
{
}

void accept_neighbors(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
}

void accept_neighbors2(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
}

void accept_info_request(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen)
{
}

void accept_info_reply(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen)
{
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "cc-mode"
 * End:
 */
