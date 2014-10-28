/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */
#ifndef __MROUTED_DEFS_H__
#define __MROUTED_DEFS_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#if defined(__bsdi__) || (defined(SunOS) && SunOS < 50)
#include <sys/sockio.h>
#endif /* bsdi || SunOS 4.x */
#include <time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/igmp.h>
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <osreldate.h>
#endif /* __FreeBSD__ */
#if defined(__bsdi__) || (defined(__FreeBSD__) && __FreeBSD_version >= 220000) || defined(__FreeBSD_kernel__)
#define rtentry kernel_rtentry
#include <net/route.h>
#undef rtentry
#endif /* bsdi or __FreeBSD_version >= 220000 */
#ifdef __linux__
#define _LINUX_IN_H             /* For Linux <= 2.6.25 */
#include <linux/types.h>
#include <linux/mroute.h>
#else
#include <netinet/ip_mroute.h>
#endif
#if defined(HAVE_STRLCPY)
#include <string.h>
#endif
#if defined(HAVE_STRTONUM)
#include <stdlib.h>
#endif
#if defined(HAVE_PIDFILE)
#if defined(OpenBSD) || defined(NetBSD)
#include <util.h>
#else
#include <libutil.h>
#endif
#endif
#ifdef RSRR
#include <sys/un.h>
#endif /* RSRR */

typedef void (*cfunc_t) (void*);
typedef void (*ihfunc_t) (int);

#include "dvmrp.h"
#include "igmpv2.h"
#include "vif.h"
#include "route.h"
#include "prune.h"
#include "pathnames.h"
#ifdef RSRR
#include "rsrr.h"
#include "rsrr_var.h"
#endif /* RSRR */

/*
 * Miscellaneous constants and macros.
 */

/* Older versions of UNIX don't really give us true raw sockets.
 * Instead, they expect ip_len and ip_off in host byte order, and also
 * provide them to us in that format when receiving raw frames.
 *
 * This list could probably be made longer, e.g., SunOS and __bsdi__
 */
#if defined(__NetBSD__) ||					\
    (defined(__FreeBSD__) && (__FreeBSD_version < 1100030)) ||	\
    (defined(__OpenBSD__) && (OpenBSD < 200311))
#define HAVE_IP_HDRINCL_BSD_ORDER
#endif

#define FALSE		0
#define TRUE		1

#define EQUAL(s1, s2)	(strcmp((s1), (s2)) == 0)
#define ARRAY_LEN(a)    (sizeof((a)) / sizeof((a)[0]))

#define TIMER_INTERVAL	ROUTE_MAX_REPORT_DELAY

#define PROTOCOL_VERSION 3  /* increment when packet format/content changes */
#define MROUTED_VERSION	 9  /* not in DVMRP packets at all */

#define	DVMRP_CONSTANT	0x000eff00	/* constant portion of 'group' field */

#define MROUTED_LEVEL  (DVMRP_CONSTANT | PROTOCOL_VERSION)
			    /* for IGMP 'group' field of DVMRP messages */

#define LEAF_FLAGS	(( vifs_with_neighbors == 1 ) ? 0x010000 : 0)
			    /* more for IGMP 'group' field of DVMRP messages */

#define	DEL_RTE_GROUP	0
#define	DEL_ALL_ROUTES	1
			    /* for Deleting kernel table entries */

#define JAN_1970	2208988800UL	/* 1970 - 1900 in seconds */

#ifdef RSRR
#define BIT_ZERO(X)      ((X) = 0)
#define BIT_SET(X,n)     ((X) |= 1 << (n))
#define BIT_CLR(X,n)     ((X) &= ~(1 << (n)))
#define BIT_TST(X,n)     ((X) & 1 << (n))
#endif /* RSRR */

#if defined(_AIX) || (defined(BSD) && BSD >= 199103)
#define	HAVE_SA_LEN
#endif

/*
 * External declarations for global variables and functions.
 */
#define RECV_BUF_SIZE 8192
extern char		*recv_buf;
extern char		*send_buf;
extern int		igmp_socket;
extern uint32_t		allhosts_group;
extern uint32_t		allrtrs_group;
extern uint32_t		dvmrp_group;
extern uint32_t		dvmrp_genid;
extern int		vifstatedefault;
extern int		missingok;

#define	IF_DEBUG(l)	if (debug && debug & (l))

#define	DEBUG_PKT	0x0001
#define	DEBUG_PRUNE	0x0002
#define	DEBUG_ROUTE	0x0004
#define	DEBUG_PEER	0x0008
#define	DEBUG_CACHE	0x0010
#define	DEBUG_TIMEOUT	0x0020
#define	DEBUG_IF	0x0040
#define	DEBUG_MEMBER	0x0080
#define	DEBUG_TRACE	0x0100
#define	DEBUG_IGMP	0x0200
#define	DEBUG_RTDETAIL	0x0400
#define	DEBUG_KERN	0x0800
#define	DEBUG_RSRR	0x1000
#define	DEBUG_ICMP	0x2000

#define	DEFAULT_DEBUG	0x02de	/* default if "-d" given without value */

extern int		debug;
extern int		did_final_init;

extern int		routes_changed;
extern int		delay_change_reports;
extern unsigned		nroutes;

extern struct uvif	uvifs[MAXVIFS];
extern vifi_t		numvifs;
extern int		vifs_down;
extern int		udp_socket;
extern int		vifs_with_neighbors;

#define MAX_INET_BUF_LEN 19
extern char		s1[MAX_INET_BUF_LEN];
extern char		s2[MAX_INET_BUF_LEN];
extern char		s3[MAX_INET_BUF_LEN];
extern char		s4[MAX_INET_BUF_LEN];

#define MAX_VERSION_LEN 100
extern char             versionstring[MAX_VERSION_LEN];

#if !(defined(BSD) && (BSD >= 199103)) && !defined(__linux__)
extern int		errno;
#endif

#ifndef IGMP_PIM
#define	IGMP_PIM	0x14
#endif
#ifndef IPPROTO_IPIP
#define	IPPROTO_IPIP	4
#endif

/*
 * The original multicast releases defined
 * IGMP_HOST_{MEMBERSHIP_QUERY,MEMBERSHIP_REPORT,NEW_MEMBERSHIP_REPORT
 *   ,LEAVE_MESSAGE}.  Later releases removed the HOST and inserted
 * the IGMP version number.  NetBSD inserted the version number in
 * a different way.  mrouted uses the new names, so we #define them
 * to the old ones if needed.
 */
#if !defined(IGMP_MEMBERSHIP_QUERY) && defined(IGMP_HOST_MEMBERSHIP_QUERY)
#define	IGMP_MEMBERSHIP_QUERY		IGMP_HOST_MEMBERSHIP_QUERY
#define	IGMP_V2_LEAVE_GROUP		IGMP_HOST_LEAVE_MESSAGE
#endif
#ifndef	IGMP_V1_MEMBERSHIP_REPORT
#ifdef	IGMP_HOST_MEMBERSHIP_REPORT
#define	IGMP_V1_MEMBERSHIP_REPORT	IGMP_HOST_MEMBERSHIP_REPORT
#define	IGMP_V2_MEMBERSHIP_REPORT	IGMP_HOST_NEW_MEMBERSHIP_REPORT
#endif
#ifdef	IGMP_v1_HOST_MEMBERSHIP_REPORT
#define	IGMP_V1_MEMBERSHIP_REPORT	IGMP_v1_HOST_MEMBERSHIP_REPORT
#define	IGMP_V2_MEMBERSHIP_REPORT	IGMP_v2_HOST_MEMBERSHIP_REPORT
#endif
#endif

/*
 * NetBSD also renamed the mtrace types.
 */
#if !defined(IGMP_MTRACE_RESP) && defined(IGMP_MTRACE_REPLY)
#define	IGMP_MTRACE_RESP		IGMP_MTRACE_REPLY
#define	IGMP_MTRACE			IGMP_MTRACE_QUERY
#endif

/* main.c */
extern char *		scaletime(time_t);
extern void		logit(int, int, const char *, ...);
extern int		register_input_handler(int, ihfunc_t);

/* igmp.c */
extern void		init_igmp(void);
extern void		accept_igmp(size_t);
extern size_t		build_igmp(uint32_t, uint32_t, int, int, uint32_t, int);
extern void		send_igmp(uint32_t, uint32_t, int, int, uint32_t, int);
extern char *		igmp_packet_kind(uint32_t, uint32_t);
extern int		igmp_debug_kind(uint32_t, uint32_t);

/* icmp.c */
extern void		init_icmp(void);

/* ipip.c */
extern void		init_ipip(void);
extern void		init_ipip_on_vif(struct uvif *);
extern void		send_ipip(uint32_t, uint32_t, int, int, uint32_t, int, struct uvif *);

/* callout.c */
extern void		callout_init(void);
extern void		free_all_callouts(void);
extern void		age_callout_queue(time_t);
extern int		timer_nextTimer(void);
extern int		timer_setTimer(time_t, cfunc_t, void *);
extern int		timer_clearTimer(int);
extern int		timer_leftTimer(int);

/* route.c */
extern void		init_routes(void);
extern void		start_route_updates(void);
extern void		update_route(uint32_t, uint32_t, uint32_t, uint32_t, vifi_t, struct listaddr *);
extern void		age_routes(void);
extern void		expire_all_routes(void);
extern void		free_all_routes(void);
extern void		accept_probe(uint32_t, uint32_t, char *, size_t, uint32_t);
extern void		accept_report(uint32_t, uint32_t, char *, size_t, uint32_t);
extern struct rtentry *	determine_route(uint32_t src);
extern void		report(int, vifi_t, uint32_t);
extern void		report_to_all_neighbors(int);
extern int		report_next_chunk(void);
extern void		blaster_alloc(vifi_t);
extern void		add_vif_to_routes(vifi_t);
extern void		delete_vif_from_routes(vifi_t);
extern void		add_neighbor_to_routes(vifi_t, uint32_t);
extern void		delete_neighbor_from_routes(uint32_t, vifi_t, uint32_t);
extern void		dump_routes(FILE *fp);

/* vif.c */
extern void		init_vifs(void);
extern void		zero_vif(struct uvif *, int);
extern void		init_installvifs(void);
extern void		check_vif_state(void);
extern void		send_on_vif(struct uvif *, uint32_t, int, size_t);
extern vifi_t		find_vif(uint32_t, uint32_t);
extern void		age_vifs(void);
extern void		dump_vifs(FILE *);
extern void		stop_all_vifs(void);
extern struct listaddr *neighbor_info(vifi_t, uint32_t);
extern void		accept_group_report(uint32_t, uint32_t, uint32_t, int);
extern void		query_groups(void);
extern void		probe_for_neighbors(void);
extern struct listaddr *update_neighbor(vifi_t, uint32_t, int, char *, size_t, uint32_t);
extern void		accept_neighbor_request(uint32_t, uint32_t);
extern void		accept_neighbor_request2(uint32_t, uint32_t);
extern void		accept_info_request(uint32_t, uint32_t, uint8_t *, size_t);
extern void		accept_info_reply(uint32_t, uint32_t, uint8_t *, size_t);
extern void		accept_neighbors(uint32_t, uint32_t, uint8_t *, size_t, uint32_t);
extern void		accept_neighbors2(uint32_t, uint32_t, uint8_t *, size_t, uint32_t);
extern void		accept_leave_message(uint32_t, uint32_t, uint32_t);
extern void		accept_membership_query(uint32_t, uint32_t, uint32_t, int);

/* config.c */
extern void		config_vifs_from_kernel(void);

/* cfparse.y */
extern void		config_vifs_from_file(void);

/* inet.c */
extern int		inet_valid_host(uint32_t);
extern int		inet_valid_mask(uint32_t);
extern int		inet_valid_subnet(uint32_t, uint32_t);
extern char *		inet_fmt(uint32_t, char *, size_t);
extern char *		inet_fmts(uint32_t, uint32_t, char *, size_t);
extern uint32_t		inet_parse(char *, int);
extern int		inet_cksum(uint16_t *, uint32_t);

/* prune.c */
extern unsigned		kroutes;
extern void		determine_forwvifs(struct gtable *);
extern void		send_prune_or_graft(struct gtable *);
extern void		add_table_entry(uint32_t, uint32_t);
extern void 		del_table_entry(struct rtentry *, uint32_t, uint32_t);
extern void		update_table_entry(struct rtentry *, uint32_t);
extern int		find_src_grp(uint32_t, uint32_t, uint32_t);
extern void		init_ktable(void);
extern void		steal_sources(struct rtentry *);
extern void		reset_neighbor_state(vifi_t, uint32_t);
extern int		grplst_mem(vifi_t, uint32_t);
extern void		free_all_prunes(void);
extern void 		age_table_entry(void);
extern void		dump_cache(FILE *);
extern void 		update_lclgrp(vifi_t, uint32_t);
extern void		delete_lclgrp(vifi_t, uint32_t);
extern void		chkgrp_graft(vifi_t, uint32_t);
extern void 		accept_prune(uint32_t, uint32_t, char *, size_t);
extern void		accept_graft(uint32_t, uint32_t, char *, size_t);
extern void 		accept_g_ack(uint32_t, uint32_t, char *, size_t);
/* uint32_t is promoted uint8_t */
extern void		accept_mtrace(uint32_t, uint32_t, uint32_t, char *, uint8_t, size_t);

/* kern.c */
extern void		k_set_rcvbuf(int, int);
extern void		k_hdr_include(int);
extern void		k_set_ttl(int);
extern void		k_set_loop(int);
extern void		k_set_if(uint32_t);
extern void		k_join(uint32_t, uint32_t);
extern void		k_leave(uint32_t, uint32_t);
extern void		k_init_dvmrp(void);
extern void		k_stop_dvmrp(void);
extern void		k_add_vif(vifi_t, struct uvif *);
extern void		k_del_vif(vifi_t, struct uvif *);
extern void		k_add_rg(uint32_t, struct gtable *);
extern int		k_del_rg(uint32_t, struct gtable *);
extern int		k_get_version(void);

#ifdef RSRR
/* prune.c */
extern struct gtable	*kernel_table;
extern struct gtable	*gtp;

/* rsrr.c */
extern void		rsrr_init(void);
extern void		rsrr_clean(void);
extern void		rsrr_cache_send(struct gtable *, int);
extern void		rsrr_cache_clean(struct gtable *);
#endif /* RSRR */

#ifdef __GNUC__
# define UNUSED __attribute__((unused))
#else
# define UNUSED /*empty*/
#endif

#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t siz);
#endif
#ifndef HAVE_STRTONUM
long long strtonum(const char *numstr, long long minval, long long maxval, const char **errstrp);
#endif
#ifndef HAVE_PIDFILE
int pidfile(const char *basename);
#endif

#endif /* __MROUTED_DEFS_H__ */
