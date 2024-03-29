/*
 * This tool requests configuration info from a multicast router
 * and prints the reply (if any).  Invoke it as:
 *
 *	mrinfo router-name-or-address
 *
 * Written Wed Mar 24 1993 by Van Jacobson (adapted from the
 * multicast mapper written by Pavel Curtis).
 *
 * The lawyers insist we include the following UC copyright notice.
 * The mapper from which this is derived contained a Xerox copyright
 * notice which follows the UC one.  Try not to get depressed noting
 * that the legal gibberish is larger than the program.
 *
 * Copyright (c) 1993 Regents of the University of California.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ---------------------------------
 * Copyright (c) 1992, 2001 Xerox Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.

 * Neither name of the Xerox, PARC, nor the names of its contributors may be used
 * to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE XEROX CORPORATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <arpa/inet.h>
#include <err.h>
#include <netdb.h>
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

#include "defs.h"

#define DEFAULT_TIMEOUT	4	/* How long to wait before retrying requests */
#define DEFAULT_RETRIES 3	/* How many times to ask each router */

uint32_t  our_addr, target_addr = 0;	/* in NET order */
int       debug = 0;
int       mrt_table_id = 0;		/* dummy, unused */
int       nflag = 0;
int       retries = DEFAULT_RETRIES;
int       timeout = DEFAULT_TIMEOUT;
int       target_level = 0;
vifi_t    numvifs;              /* to keep loader happy */
				/* (see COPY_TABLES macro called in kern.c) */

void      ask(uint32_t dst);
void      ask2(uint32_t dst);

/*
 * Log errors and other messages to stderr, according to the severity of the
 * message and the current debug level.  For errors of severity LOG_ERR or
 * worse, terminate the program.
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

/*
 * Send a neighbors-list request.
 */
void ask(uint32_t dst)
{
	send_igmp(our_addr, dst, IGMP_DVMRP, DVMRP_ASK_NEIGHBORS,
		  htonl(MROUTED_LEVEL), 0);
}

void ask2(uint32_t dst)
{
	send_igmp(our_addr, dst, IGMP_DVMRP, DVMRP_ASK_NEIGHBORS2,
		  htonl(MROUTED_LEVEL), 0);
}

/*
 * Process an incoming neighbor-list message.
 */
void accept_neighbors(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
	uint8_t *ep = p + datalen;
	char *name;
#define GET_ADDR(a) (a  = ((uint32_t)*p++ << 24), a += ((uint32_t)*p++ << 16), \
		     a += ((uint32_t)*p++ << 8),  a += *p++)

	name = inet_name(src, nflag);
	printf("%s (%s):\n", inet_fmt(src, s1, sizeof(s1)), name ? name : "N/A");
	while (p < ep) {
		uint32_t laddr;
		uint8_t metric;
		uint8_t thresh;
		int ncount;

		GET_ADDR(laddr);
		laddr = htonl(laddr);
		metric = *p++;
		thresh = *p++;
		ncount = *p++;
		while (--ncount >= 0) {
			uint32_t neighbor;

			GET_ADDR(neighbor);
			neighbor = htonl(neighbor);
			printf("  %s -> ", inet_fmt(laddr, s1, sizeof(s1)));

			name = inet_name(neighbor, nflag);
			printf("%s (%s) [%d/%d]\n", inet_fmt(neighbor, s1, sizeof(s1)),
			       name ? name : "N/A", metric, thresh);
		}
	}
}

void accept_neighbors2(uint32_t src, uint32_t dst, uint8_t *p, size_t datalen, uint32_t level)
{
	uint8_t *ep = p + datalen;
	uint32_t broken_cisco = ((level & 0xffff) == 0x020a); /* 10.2 */
	/* well, only possibly_broken_cisco, but that's too long to type. */
	uint32_t majvers = level & 0xff;
	uint32_t minvers = (level >> 8) & 0xff;
	char *name;

	name = inet_name(src, nflag);
	printf("%s (%s) [", inet_fmt(src, s1, sizeof(s1)), name ? name : "N/A");
	if (majvers == 3 && minvers == 0xff)
		printf("DVMRPv3 compliant");
	else
		printf("version %d.%d", majvers, minvers);
	printf ("]:\n");

	while (p < ep) {
		uint8_t metric;
		uint8_t thresh;
		uint8_t flags;
		int ncount;
		uint32_t laddr = *(uint32_t*)p;

		p += 4;
		metric = *p++;
		thresh = *p++;
		flags = *p++;
		ncount = *p++;
		if (broken_cisco && ncount == 0)	/* dumb Ciscos */
			ncount = 1;
		if (broken_cisco && ncount > 15)	/* dumb Ciscos */
			ncount = ncount & 0xf;
		while (--ncount >= 0 && p < ep) {
			uint32_t neighbor = *(uint32_t*)p;

			p += 4;

			name = inet_name(neighbor, nflag);
			printf("  %s -> ", inet_fmt(laddr, s1, sizeof(s1)));
			printf("%s (%s) [%d/%d", inet_fmt(neighbor, s1, sizeof(s1)),
			       name ? name : "N/A", metric, thresh);
			if (flags & DVMRP_NF_TUNNEL)
				printf("/tunnel");
			if (flags & DVMRP_NF_SRCRT)
				printf("/srcrt");
			if (flags & DVMRP_NF_PIM)
				printf("/pim");
			if (flags & DVMRP_NF_QUERIER)
				printf("/querier");
			if (flags & DVMRP_NF_DISABLED)
				printf("/disabled");
			if (flags & DVMRP_NF_DOWN)
				printf("/down");
			if (flags & DVMRP_NF_LEAF)
				printf("/leaf");
			printf("]\n");
		}
	}
}

int usage(int code)
{
	printf("Usage: mrinfo [-hn] [-d level] [-r count] [-t seconds] [router]\n");

	return code;
}

int main(int argc, char *argv[])
{
	struct timeval et;
	struct addrinfo *result, *rp;
	struct addrinfo hints;
	const char *errstr;
	uid_t uid;
	char *host;
	int tries, trynew, ch, rc;

	while ((ch = getopt(argc, argv, "d:hnr:t:")) != -1) {
		switch (ch) {
		case 'd':
			debug = strtonum(optarg, 0, 3, &errstr);
			if (errstr) {
				warnx("debug level %s", errstr);
				debug = 3;
			}
			break;

		case 'h':
			return usage(0);

		case 'n':
			++nflag;
			break;

		case 'r':
			retries = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr) {
				warnx("retries %s", errstr);
				return usage(1);
			}
			break;

		case 't':
			timeout = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr) {
				warnx("timeout %s", errstr);
				return usage(1);
			}
			break;

		default:
			return usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (geteuid() != 0) {
		fprintf(stderr, "mrinfo: must be root\n");
		exit(1);
	}

	igmp_init();

	uid = getuid();
	if (setuid(uid) == -1)
		err(1, "setuid");

	setlinebuf(stderr);

	if (argc > 1)
		return usage(1);
	if (argc == 1)
		host = argv[0];
	else
		host = "127.0.0.1";

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	rc = getaddrinfo(host, NULL, &hints, &result);
	if (rc) {
		fprintf(stderr, "mrinfo: %s\n", gai_strerror(rc));
		return 1;
	}

	if (debug)
		fprintf(stderr, "Debug level %u\n", debug);

	/* Check all addresses; mrouters often have unreachable interfaces */
	for (rp = result; rp; rp = rp->ai_next) {
		struct sockaddr_in *sin;
		struct sockaddr sa;
		socklen_t len = sizeof(sa);
		int sd;

		/* Find a good local address for us. */
		sd = socket(AF_INET, SOCK_DGRAM, 0);
		if (sd < 0
		    || connect(sd, rp->ai_addr, rp->ai_addrlen) < 0
		    || getsockname(sd, &sa, &len) < 0) {
			perror("Determining local address");
			freeaddrinfo(result);
			exit(1);
		}
		close(sd);

		/* Our local address */
		sin = (struct sockaddr_in *)&sa;
		our_addr = sin->sin_addr.s_addr;

		/* Remote mrouted address */
		sin = (struct sockaddr_in *)rp->ai_addr;
		target_addr = sin->sin_addr.s_addr;

		tries = 0;
		trynew = 1;
		/*
		 * New strategy: send 'ask2' for two timeouts, then fall back
		 * to 'ask', since it's not very likely that we are going to
		 * find someone who only responds to 'ask' these days
		 */
		ask2(target_addr);

		gettimeofday(&et, 0);
		et.tv_sec += timeout;

		/* Main receive loop */
		for (;;) {
			fd_set  fds;
			struct timeval tv, now;
			int     count;
			ssize_t recvlen;
			socklen_t dummy = 0;
			uint32_t src, dst, group;
			struct ip *ip;
			struct igmp *igmp;
			size_t ipdatalen, iphdrlen, igmpdatalen;

			FD_ZERO(&fds);
			if (igmp_socket >= (int)FD_SETSIZE)
				logit(LOG_ERR, 0, "Descriptor too big");
			FD_SET(igmp_socket, &fds);

			gettimeofday(&now, 0);
			tv.tv_sec = et.tv_sec - now.tv_sec;
			tv.tv_usec = et.tv_usec - now.tv_usec;

			if (tv.tv_usec < 0) {
				tv.tv_usec += 1000000L;
				--tv.tv_sec;
			}
			if (tv.tv_sec < 0)
				tv.tv_sec = tv.tv_usec = 0;

			count = select(igmp_socket + 1, &fds, 0, 0, &tv);

			if (count < 0) {
				if (errno != EINTR)
					perror("select");
				continue;
			} else if (count == 0) {
				logit(LOG_DEBUG, 0, "Timed out receiving neighbor lists");
				if (++tries > retries)
					break;
				/* If we've tried ASK_NEIGHBORS2 twice with
				 * no response, fall back to ASK_NEIGHBORS
				 */
				if (tries == 2 && target_level == 0)
					trynew = 0;
				if (target_level == 0 && trynew == 0)
					ask(target_addr);
				else
					ask2(target_addr);
				gettimeofday(&et, 0);
				et.tv_sec += timeout;
				continue;
			}

			recvlen = recvfrom(igmp_socket, recv_buf, RECV_BUF_SIZE, 0, NULL, &dummy);
			if (recvlen <= 0) {
				if (recvlen && errno != EINTR)
					perror("recvfrom");
				continue;
			}

			if (recvlen < (ssize_t)sizeof(struct ip)) {
				logit(LOG_WARNING, 0, "packet too short (%zd bytes) for IP header", recvlen);
				continue;
			}

			ip = (struct ip *)recv_buf;
			if (ip->ip_p == 0)
				continue;	/* Request to install cache entry */

			src = ip->ip_src.s_addr;
			dst = ip->ip_dst.s_addr;
			iphdrlen = ip->ip_hl << 2;
#ifdef HAVE_IP_HDRINCL_BSD_ORDER
			ipdatalen = ip->ip_len - iphdrlen;
#else
			ipdatalen = ntohs(ip->ip_len) - iphdrlen;
#endif
			if (iphdrlen + ipdatalen != (size_t)recvlen) {
				logit(LOG_WARNING, 0, "packet shorter (%zd bytes) than hdr+data length (%zu+%zu)",
				      recvlen, iphdrlen, ipdatalen);
				continue;
			}

			igmp = (struct igmp *)(recv_buf + iphdrlen);
			group = igmp->igmp_group.s_addr;
			if (ipdatalen < IGMP_MINLEN) {
				logit(LOG_WARNING, 0, "IP data field too short (%zu bytes) for IGMP, from %s",
				      ipdatalen, inet_fmt(src, s1, sizeof(s1)));
				continue;
			}
			igmpdatalen = ipdatalen - IGMP_MINLEN;
			if (igmp->igmp_type != IGMP_DVMRP)
				continue;

			switch (igmp->igmp_code) {
			case DVMRP_NEIGHBORS:
			case DVMRP_NEIGHBORS2:
				if (src != target_addr) {
					fprintf(stderr, "mrinfo: got reply from %s",
						inet_fmt(src, s1, sizeof(s1)));
					fprintf(stderr, " instead of %s\n",
						inet_fmt(target_addr, s1, sizeof(s1)));
					/*continue;*/
				}
				break;

			default:
				continue;	/* ignore all other DVMRP messages */
			}

			switch (igmp->igmp_code) {
			case DVMRP_NEIGHBORS:
				if (group) {
					/* knows about DVMRP_NEIGHBORS2 msg */
					if (target_level == 0) {
						target_level = ntohl(group);
						ask2(target_addr);
					}
				} else {
					accept_neighbors(src, dst, (uint8_t *)(igmp + 1),
							 igmpdatalen, ntohl(group));
					freeaddrinfo(result);
					exit(0);
				}
				break;

			case DVMRP_NEIGHBORS2:
				accept_neighbors2(src, dst, (uint8_t *)(igmp + 1),
						  igmpdatalen, ntohl(group));
				freeaddrinfo(result);
				exit(0);
			}
		}
	}

	freeaddrinfo(result);
	exit(1);
}

/* dummies */
void accept_probe(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
}
void accept_group_report(int ifi, uint32_t src, uint32_t dst, uint32_t group, int r_type)
{
}
void accept_report(uint32_t src, uint32_t dst, char *p, size_t datalen, uint32_t level)
{
}
void accept_neighbor_request(uint32_t src, uint32_t dst)
{
}
void accept_neighbor_request2(uint32_t src, uint32_t dst)
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
void check_vif_state(void)
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
