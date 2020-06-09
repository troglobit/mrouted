/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */
#ifndef MROUTED_DEFS_H_
#define MROUTED_DEFS_H_

#include <config.h>
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
#include <sys/un.h>
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

typedef void (*cfunc_t) (void*);
typedef void (*ihfunc_t) (int);

#include "dvmrp.h"
#include "igmpv2.h"
#include "igmpv3.h"
#include "vif.h"
#include "route.h"
#include "prune.h"
#include "pathnames.h"
#ifdef RSRR
#include "rsrr.h"
#include "rsrr_var.h"
#endif

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

#define TIMER_INTERVAL	2

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
#endif

#if defined(_AIX) || (defined(BSD) && BSD >= 199103)
#define	HAVE_SA_LEN
#endif

/*
 * External declarations for global variables and functions.
 */
#define RECV_BUF_SIZE 8192
extern uint8_t		*recv_buf;
extern uint8_t		*send_buf;
extern int		igmp_socket;
extern int		router_alert;
extern uint32_t		allhosts_group;
extern uint32_t		allrtrs_group;
extern uint32_t		allreports_group;
extern uint32_t		dvmrp_group;
extern uint32_t		dvmrp_genid;
extern uint32_t		igmp_query_interval;
extern uint32_t		igmp_robustness;
extern uint32_t		virtual_time;

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
#define DEBUG_PARSE_ERR 0x80000000
#define DEBUG_ALL       0xffffffff

struct debugnm {
	char		*name;
	uint32_t	 level;
	size_t		 nchars;
};

extern int		debug;
extern int		loglevel;
extern int		use_syslog;
extern int		running;
extern int		haveterminal;
extern int		did_final_init;
extern time_t           mrouted_init_time;

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
#if defined(__FreeBSD__)		/* From FreeBSD 8.x */
#define IGMP_V3_MEMBERSHIP_REPORT       IGMP_v3_HOST_MEMBERSHIP_REPORT
#else
#define IGMP_V3_MEMBERSHIP_REPORT	0x22	/* Ver. 3 membership report */
#endif

/*
 * NetBSD also renamed the mtrace types.
 */
#if !defined(IGMP_MTRACE_RESP) && defined(IGMP_MTRACE_REPLY)
#define	IGMP_MTRACE_RESP		IGMP_MTRACE_REPLY
#define	IGMP_MTRACE			IGMP_MTRACE_QUERY
#endif

/* common.c */
extern int              debug_list(int mask, char *buf, size_t len);
extern void             debug_print(void);
extern int              debug_parse(char *arg);


/* main.c */
extern int              debug_list(int, char *, size_t);
extern int              debug_parse(char *);
extern void             restart(void);
extern char *		scaletime(time_t);
extern int		register_input_handler(int, ihfunc_t);

/* log.c */
extern void             log_init(void);
extern int		log_str2lvl(char *);
extern const char *	log_lvl2str(int);
extern int		log_list(char *, size_t);
extern void		logit(int, int, const char *, ...);
extern void             resetlogging(void *);

/* igmp.c */
extern void		igmp_init(void);
extern void		igmp_exit(void);
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

/* timer.c */
extern void		timer_init(void);
extern void		timer_exit(void);
extern void		timer_stop_all(void);
extern void		timer_age_queue(time_t);
extern int		timer_next_delay(void);
extern int		timer_set(time_t, cfunc_t, void *);
extern int		timer_get(int);
extern void		timer_clear(int);

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
extern void		dump_routes(FILE *, int);

/* vif.c */
extern void		init_vifs(void);
extern void		zero_vif(struct uvif *, int);
extern void		init_installvifs(void);
extern void		check_vif_state(void);
extern void		send_on_vif(struct uvif *, uint32_t, int, size_t);
extern vifi_t		find_vif(uint32_t, uint32_t);
extern uint32_t         vif_nbr_expire_time(struct listaddr *);
extern void		age_vifs(void);
extern char            *vif_sflags(uint32_t);
extern char            *vif_nbr_flags(uint16_t, char *, size_t);
extern char            *vif_nbr_sflags(uint16_t);
extern void		dump_vifs(FILE *, int);
extern void		stop_all_vifs(void);
extern struct listaddr *neighbor_info(vifi_t, uint32_t);
extern void		accept_group_report(uint32_t, uint32_t, uint32_t, int);
extern void		query_groups(void *);
extern void		query_dvmrp(void *);
extern void		probe_for_neighbors(void);
extern struct listaddr *update_neighbor(vifi_t, uint32_t, int, char *, size_t, uint32_t);
extern void		accept_neighbor_request(uint32_t, uint32_t);
extern void		accept_neighbor_request2(uint32_t, uint32_t);
extern void		accept_info_request(uint32_t, uint32_t, uint8_t *, size_t);
extern void		accept_info_reply(uint32_t, uint32_t, uint8_t *, size_t);
extern void		accept_neighbors(uint32_t, uint32_t, uint8_t *, size_t, uint32_t);
extern void		accept_neighbors2(uint32_t, uint32_t, uint8_t *, size_t, uint32_t);
extern void		accept_leave_message(uint32_t, uint32_t, uint32_t);
extern void		accept_membership_query(uint32_t, uint32_t, uint32_t, int, int);
extern void             accept_membership_report(uint32_t, uint32_t, struct igmpv3_report *, ssize_t);

/* config.c */
extern void		config_vifs_from_kernel(void);

/* cfparse.y */
extern void		config_vifs_from_file(void);

/* inet.c */
extern int		inet_valid_host(uint32_t);
extern int		inet_valid_mask(uint32_t);
extern int		inet_valid_subnet(uint32_t, uint32_t);
extern char            *inet_name(uint32_t, int);
extern char            *inet_fmt(uint32_t, char *, size_t);
extern char            *inet_fmts(uint32_t, uint32_t, char *, size_t);
extern uint32_t		inet_parse(char *, int);
extern int		inet_cksum(uint16_t *, uint32_t);

/* prune.c */
extern struct gtable	*kernel_table;
extern struct gtable	*kernel_no_route;
extern struct gtable	*gtp;

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
extern void		dump_cache(FILE *, int);
extern void 		update_lclgrp(vifi_t, uint32_t);
extern void		delete_lclgrp(vifi_t, uint32_t);
extern void		chkgrp_graft(vifi_t, uint32_t);
extern void 		accept_prune(uint32_t, uint32_t, char *, size_t);
extern void		accept_graft(uint32_t, uint32_t, char *, size_t);
extern void 		accept_g_ack(uint32_t, uint32_t, char *, size_t);
/* uint32_t is promoted uint8_t */
extern void		accept_mtrace(uint32_t, uint32_t, uint32_t, char *, uint8_t, size_t);

/* kern.c */
extern int              curttl;

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
/* rsrr.c */
extern void		rsrr_init(void);
extern void		rsrr_clean(void);
extern void		rsrr_cache_send(struct gtable *, int);
extern void		rsrr_cache_clean(struct gtable *);
#endif

#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

#ifndef HAVE_STRLCAT
size_t  strlcat(char *dst, const char *src, size_t siz);
#endif

#ifndef HAVE_STRTONUM
long long strtonum(const char *numstr, long long minval, long long maxval, const char **errstrp);
#endif
#ifndef HAVE_PIDFILE
int pidfile(const char *basename);
#endif

/*
 * mrouted <--> mroutectl IPC
 */
#define IPC_OK_CMD                0
#define IPC_RESTART_CMD           1
#define IPC_SHOW_STATUS_CMD       2
#define IPC_DEBUG_CMD             3
#define IPC_LOGLEVEL_CMD          4
#define IPC_VERSION_CMD           5
#define IPC_KILL_CMD              9
#define IPC_SHOW_IGMP_GROUP_CMD   10
#define IPC_SHOW_IGMP_IFACE_CMD   11
#define IPC_SHOW_IGMP_CMD         12
#define IPC_SHOW_IFACE_CMD        20
#define IPC_SHOW_MFC_CMD          21
#define IPC_SHOW_NEIGH_CMD        22
#define IPC_SHOW_ROUTES_CMD       23
#define IPC_SHOW_COMPAT_CMD       250
#define IPC_EOF_CMD               254
#define IPC_ERR_CMD               255

struct ipc {
	uint8_t cmd;
	uint8_t detail;

	char    buf[765];
	char    sentry;
};

/* ipc.c */
void ipc_init(void);
void ipc_exit(void);

/* Shared constants between mrouted and mroutectl */
static const char *versionstring = "mrouted version " PACKAGE_VERSION;

#endif /* MROUTED_DEFS_H_ */
