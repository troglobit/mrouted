/*
 * Copyright (c) 2018-2019 Joachim Nilsson <troglobit@gmail.com>
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

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "defs.h"
extern struct rtentry *routing_table;

static struct sockaddr_un sun;
static int ipc_socket = -1;

static int ipc_write(int sd, struct ipc *msg)
{
	ssize_t len;

	while ((len = write(sd, msg, sizeof(*msg)))) {
		if (-1 == len && EINTR == errno)
			continue;
		break;
	}

	if (len != sizeof(*msg))
		return -1;

	return 0;
}

static int ipc_close(int sd, struct ipc *msg, int status)
{
	msg->cmd = status;
	if (ipc_write(sd, msg)) {
		logit(LOG_WARNING, errno, "Failed sending reply (%d) to client", status);
		return -1;
	}

	return 0;
}

static void ipc_generic(int sd, struct ipc *msg, int (*cb)(void *), void *arg)
{
	int rc = IPC_EOF_CMD;

        if (cb(arg))
                rc = IPC_ERR_CMD;

	ipc_close(sd, msg, rc);
}


static int ipc_send(int sd, struct ipc *msg, FILE *fp)
{
	msg->cmd = IPC_OK_CMD;
	while (fgets(msg->buf, sizeof(msg->buf), fp)) {
		if (!ipc_write(sd, msg))
			continue;

		logit(LOG_WARNING, errno, "Failed communicating with client");
		return -1;
	}

	return ipc_close(sd, msg, IPC_EOF_CMD);
}

static void ipc_show(int sd, struct ipc *msg, void (*cb)(FILE *, int))
{
	FILE *fp;

	fp = tmpfile();
	if (!fp) {
		logit(LOG_WARNING, errno, "Failed opening temporary file");
		return;
	}

	cb(fp, msg->detail);

	rewind(fp);
	ipc_send(sd, msg, fp);
	fclose(fp);
}

static void show_dump(FILE *fp, int detail)
{
	dump_vifs(fp, detail);
	dump_routes(fp, detail);
	dump_cache(fp, detail);
}

static char *vif2name(int vif)
{
	struct uvif *uv;
	vifi_t vifi;

	for (vifi = 0, uv = uvifs; vifi < numvifs; vifi++, uv++) {
		if (vif == vifi)
			return uv->uv_name;
	}

	return NULL;
}

static const char *ifstate(struct uvif *uv)
{
	if (uv->uv_flags & VIFF_DOWN)
		return "Down";

	if (uv->uv_flags & VIFF_DISABLED)
		return "Disabled";

	return "Up";
}

static void show_iface(FILE *fp, int detail)
{
	struct listaddr *al;
	struct uvif *v;
	vifi_t vifi;
	time_t thyme = time(NULL);

	if (numvifs == 0)
		return;

	fputs("Interface Table_\n", fp);
	fprintf(fp, "%-15s %-15s %5s %4s %3s%10s %-5s=\n",
		"Address", "Interface", "State", "Cost", "TTL", "Uptime", "Flags");

	for (vifi = 0, v = uvifs; vifi < numvifs; vifi++, v++) {
		fprintf(fp, "%-15s %-15s %5s %4u %3u%10s %s\n",
			inet_fmt(v->uv_lcl_addr, s1, sizeof(s1)),
			v->uv_name,
			ifstate(v),
			v->uv_metric,
			v->uv_threshold, /* TTL scoping */
			"0:00:00",	 /* XXX fixme */
			vif_sflags(v->uv_flags));
	}
}

static void show_neighbor_header(FILE *fp, int detail)
{
	fputs("Neighbor Table_\n", fp);
	fprintf(fp, "%-15s %-15s %7s %-5s%10s %6s=\n",
		"Neighbor", "Interface", "Version", "Flags", "Uptime", "Expire");
}

static void show_neighbor(FILE *fp, int detail)
{
	struct listaddr *al;
	struct uvif *v;
	vifi_t vifi;
	time_t thyme = time(NULL);
	int once = 1;

	for (vifi = 0, v = uvifs; vifi < numvifs; vifi++, v++) {
		for (al = v->uv_neighbors; al; al = al->al_next) {
			char ver[10];

			if (once) {
				show_neighbor_header(fp, detail);
				once = 0;
			}

			/* Protocol version . mrouted version */
			snprintf(ver, sizeof(ver), "%d.%d", al->al_pv, al->al_mv);

			fprintf(fp, "%-15s %-16s%-7s %-5s%10s %5us\n",
				inet_fmt(al->al_addr, s1, sizeof(s1)),
				v->uv_name,
				ver,
				vif_nbr_sflags(al->al_flags),
				scaletime(thyme - al->al_ctime),
				vif_nbr_expire_time(al) - al->al_timer
				);
		}
	}
}

static void show_routes_header(FILE *fp, int detail)
{
	fputs("DVMRP Routing Table_\n", fp);
	if (!detail)
		fprintf(fp, "%-15s %-15s %-15s=\n",
			"Origin", "Neighbor", "Interface");
	else
		fprintf(fp, "%-15s %-15s %-15s%6s %8s=\n",
			"Origin", "Neighbor", "Interface", "Cost", "Expire");
}

static void show_routes(FILE *fp, int detail)
{
	struct rtentry *r;
	vifi_t i;
	int once = 1;

	if (!routing_table)
		return;

	for (r = routing_table; r; r = r->rt_next) {
		if (once) {
			show_routes_header(fp, detail);
			once = 0;
		}

		fprintf(fp, "%-15s %-15s %-15s",
			inet_fmts(r->rt_origin, r->rt_originmask, s1, sizeof(s1)),
			(r->rt_gateway == 0
			 ? "Local"
			 : inet_fmt(r->rt_gateway, s2, sizeof(s2))),
			vif2name(r->rt_parent));

		if (!detail)
			goto next;

		if (r->rt_metric == UNREACHABLE)
			fprintf(fp, "   NR ");
		else
			fprintf(fp, "  %4u ", r->rt_metric);

		if (r->rt_timer == 0)
			fprintf(fp, "%8s", "Never");
		else
			fprintf(fp, "%7us", r->rt_timer);

	next:
		fprintf(fp, "\n");
	}
}

static void show_mfc_header(FILE *fp, int detail)
{
	fputs("Multicast Forwarding Cache Table_\n", fp);
	if (!detail)
		fprintf(fp, "%-15s %-15s %-15s %s=\n",
			"Origin", "Group", "Inbound", "Outbound");
	else
		fprintf(fp, "%-15s %-15s %-15s %2s%10s %8s  %s=\n",
			"Origin", "Group", "Inbound", "<>", "Uptime", "Expire", "Outbound");
}

static void show_mfc(FILE *fp, int detail)
{
	struct rtentry *r;
	struct gtable *gt;
	struct stable *st;
	struct ptable *pt;
	time_t thyme = time(NULL);
	vifi_t i;
	char flags[5];
	char c;
	int once = 1;

	for (gt = kernel_no_route; gt; gt = gt->gt_next) {
		if (gt->gt_srctbl) {
			if (once) {
				show_mfc_header(fp, detail);
				once = 0;
			}

			fprintf(fp, "%-15s %-15s %-16s",
				inet_fmts(gt->gt_srctbl->st_origin, 0xffffffff, s1, sizeof(s1)),
				inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
				"-");			     /* No routes -> no incoming */

			if (detail)
				fprintf(fp, "%2s%10s %8s  ",
					"!!",			     /* No route */
					scaletime(thyme - gt->gt_ctime), /* Age/Uptime */
					scaletime(gt->gt_timer));	     /* Timeout/Expire */

			for (i = 0; i < numvifs; ++i) {
				if (VIFM_ISSET(i, gt->gt_grpmems))
					fprintf(fp, "%s ", vif2name(i));
			}
			fprintf(fp, "\n");
		}
	}

	for (gt = kernel_table; gt; gt = gt->gt_gnext) {
		int any = 0;

		/* Pruned upstream and no detail? */
		if (gt->gt_prsent_timer && !detail)
			continue;

		/* Pruned downstream, no outbounds, and no detail? */
		for (i = 0; i < numvifs; i++) {
			if (VIFM_ISSET(i, gt->gt_grpmems))
				any++;
		}
		if (!any && !detail)
			continue;

		if (once) {
			show_mfc_header(fp, detail);
			once = 0;
		}

		r = gt->gt_route;
		fprintf(fp, "%-15s %-15s %-16s",
			RT_FMT(r, s1),
			inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
			vif2name(r->rt_parent));

		if (detail) {
			snprintf(flags, sizeof(flags), "%c%c",
				 gt->gt_prsent_timer
				 ? 'P'
				 : (gt->gt_grftsnt
				    ? 'G'
				    : ' '),
				 VIFM_ISSET(r->rt_parent, gt->gt_scope)
				 ? 'B'
				 : ' ');
			fprintf(fp, "%2s%10s %8s  ",
				flags,
				scaletime(thyme - gt->gt_ctime),	/* Age/Uptime */
				scaletime(gt->gt_timer));		/* Timeout/Expire */
		}

		for (i = 0; i < numvifs; i++) {
			if (VIFM_ISSET(i, gt->gt_grpmems))
				fprintf(fp, "%s ", vif2name(i));
			else if (VIFM_ISSET(i, r->rt_children) &&
				 NBRM_ISSETMASK(uvifs[i].uv_nbrmap, r->rt_subordinates))
				fprintf(fp, "%s%s ", vif2name(i),
					VIFM_ISSET(i, gt->gt_scope)
					? ":b"
					: (SUBS_ARE_PRUNED(r->rt_subordinates, uvifs[i].uv_nbrmap, gt->gt_prunes)
					   ? ":p"
					   : ":!"));
		}
		fprintf(fp, "\n");
	}

	if (!detail)
		return;

	once = 1;
	for (gt = kernel_table; gt; gt = gt->gt_gnext) {
		if (gt->gt_prsent_timer)
			continue;

		r = gt->gt_route;
		for (st = gt->gt_srctbl; st; st = st->st_next) {
			if (once) {
				fprintf(fp, "\n%-15s %-15s %-15s   %10s %8s  %8s=\n",
					"Source", "Group", "Inbound", "Uptime", "Packets", "Bytes");
				once = 0;
			}

			fprintf(fp, "%-15s %-15s %-16s  ",
				inet_fmt(st->st_origin, s1, sizeof(s1)),
				inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)),
				vif2name(r->rt_parent));

			if (st->st_ctime) {
				struct sioc_sg_req rq;

				fprintf(fp, "%10s ", scaletime(thyme - st->st_ctime));

				rq.src.s_addr = st->st_origin;
				rq.grp.s_addr = gt->gt_mcastgrp;
				if (ioctl(udp_socket, SIOCGETSGCNT, &rq) < 0)
					logit(LOG_WARNING, errno, "Failed reading (S,G) count for (%s,%s)",
					      inet_fmt(st->st_origin, s1, sizeof(s1)),
					      inet_fmt(gt->gt_mcastgrp, s2, sizeof(s2)));
				else
					fprintf(fp, "%8ld  %8ld", rq.pktcnt, rq.bytecnt);
			} else
				fprintf(fp, "%10s", "-");
			fputs("\n", fp);
		}
	}
}

static uint32_t diff_vtime(uint32_t mtime)
{
	if (mtime > virtual_time)
		return mtime - virtual_time;

	return virtual_time - mtime;
}

static void show_igmp_group(FILE *fp, int detail)
{
	struct listaddr *group, *source;
	struct uvif *uv;
	vifi_t vifi;
	int once = 1;

	for (vifi = 0, uv = uvifs; vifi < numvifs; vifi++, uv++) {
		for (group = uv->uv_groups; group; group = group->al_next) {
			if (once) {
				fputs("IGMP Group Table_\n", fp);
				fprintf(fp, "%-16s  %-15s  %-15s  %6s=\n",
					"Interface", "Group", "Last Reporter", "Expire");
				once = 0;
			}

			fprintf(fp, "%-16s  %-15s  %-15s  %5us\n",
				uv->uv_name,
				inet_fmt(group->al_addr, s1, sizeof(s1)),
				inet_fmt(group->al_reporter, s2, sizeof(s2)),
				group->al_timer - diff_vtime(group->al_mtime));
		}
	}
}


static void show_igmp_iface(FILE *fp, int detail)
{
	struct listaddr *group;
	struct uvif *uv;
	vifi_t vifi;

	if (numvifs == 0)
		return;

	fputs("IGMP Interface Table_\n", fp);
	fprintf(fp, "%-16s  %-15s  %7s  %6s  %6s=\n",
		"Interface", "Querier", "Version", "Groups", "Expire");

	for (vifi = 0, uv = uvifs; vifi < numvifs; vifi++, uv++) {
		size_t num = 0;
		char timeout[10];
		int version;

		if (!uv->uv_querier) {
			strlcpy(s1, "Local", sizeof(s1));
			snprintf(timeout, sizeof(timeout), "%6s", "Never");
		} else {
			inet_fmt(uv->uv_querier->al_addr, s1, sizeof(s1));
			snprintf(timeout, sizeof(timeout), "%5us",
				 IGMP_OTHER_QUERIER_PRESENT_INTERVAL - uv->uv_querier->al_timer);
		}

		for (group = uv->uv_groups; group; group = group->al_next)
			num++;

		if (uv->uv_flags & VIFF_IGMPV1)
			version = 1;
		else if (uv->uv_flags & VIFF_IGMPV2)
			version = 2;
		else
			version = 3;

		fprintf(fp, "%-16s  %-15s  %7d  %6zu  %6s\n",
			uv->uv_name,
			s1,
			version,
			num,
			timeout);
	}
}

static void show_igmp(FILE *fp, int detail)
{
	show_igmp_iface(fp, detail);
	show_igmp_group(fp, detail);
}

static void show_status(FILE *fp, int detail)
{
	show_iface(fp, detail);
	show_neighbor(fp, detail);
	show_routes(fp, detail);
	show_mfc(fp, detail);
}

static void show_version(FILE *fp, int detail)
{
    time_t t;

    fprintf(fp, "%s ", versionstring);
    if (!detail) {
	    fputs("\n", fp);
	    return;
    }

    time(&t);
    if (did_final_init)
	    fprintf(fp, "up %s", scaletime(t - mrouted_init_time));
    else
	    fprintf(fp, "(not yet initialized)");
    fprintf(fp, " %s", ctime(&t));
}

static int do_debug(void *arg)
{
        struct ipc *msg = (struct ipc *)arg;

	if (!strcmp(msg->buf, "?"))
		return debug_list(DEBUG_ALL, msg->buf, sizeof(msg->buf));

	if (strlen(msg->buf)) {
		int rc = debug_parse(msg->buf);

		if ((int)DEBUG_PARSE_ERR == rc)
			return 1;

		/* Activate debugging of new subsystems */
		debug = rc;
	}

	/* Return list of activated subsystems */
	if (debug)
		debug_list(debug, msg->buf, sizeof(msg->buf));
	else
		snprintf(msg->buf, sizeof(msg->buf), "none");

	return 0;
}

static int do_loglevel(void *arg)
{
	struct ipc *msg = (struct ipc *)arg;
	int rc;

	if (!strcmp(msg->buf, "?"))
		return log_list(msg->buf, sizeof(msg->buf));

	if (!strlen(msg->buf)) {
		strlcpy(msg->buf, log_lvl2str(loglevel), sizeof(msg->buf));
		return 0;
	}

	rc = log_str2lvl(msg->buf);
	if (-1 == rc)
		return 1;

	logit(LOG_NOTICE, 0, "Setting new log level %s", log_lvl2str(rc));
	loglevel = rc;

	return 0;
}

static void ipc_handle(int sd)
{
	socklen_t socklen = 0;
	struct ipc msg;
	ssize_t len;
	int client;

	client = accept(sd, NULL, &socklen);
	if (client < 0)
		return;

	len = read(client, &msg, sizeof(msg));
	if (len < 0) {
		logit(LOG_WARNING, errno, "Failed reading IPC command");
		close(client);
		return;
	}

	switch (msg.cmd) {
	case IPC_VERSION_CMD:
		ipc_show(client, &msg, show_version);
		break;

	case IPC_KILL_CMD:
		running = 0;
		break;

	case IPC_RESTART_CMD:
		restart();
		break;

	case IPC_DEBUG_CMD:
		ipc_generic(client, &msg, do_debug, &msg);
		break;

	case IPC_LOGLEVEL_CMD:
		ipc_generic(client, &msg, do_loglevel, &msg);
		break;

	case IPC_SHOW_COMPAT_CMD:
		ipc_show(client, &msg, show_dump);
		break;

	case IPC_SHOW_NEIGH_CMD:
		ipc_show(client, &msg, show_neighbor);
		break;

	case IPC_SHOW_IGMP_GROUP_CMD:
		ipc_show(client, &msg, show_igmp_group);
		break;

	case IPC_SHOW_IGMP_IFACE_CMD:
		ipc_show(client, &msg, show_igmp_iface);
		break;

	case IPC_SHOW_IGMP_CMD:
		ipc_show(client, &msg, show_igmp);
		break;

	case IPC_SHOW_ROUTES_CMD:
		ipc_show(client, &msg, show_routes);
		break;

	case IPC_SHOW_IFACE_CMD:
		ipc_show(client, &msg, show_iface);
		break;

	case IPC_SHOW_MFC_CMD:
		ipc_show(client, &msg, show_mfc);
		break;

	case IPC_SHOW_STATUS_CMD:
		ipc_show(client, &msg, show_status);

	default:
		break;
	}

	close(client);
}

void ipc_init(void)
{
	socklen_t len;
	int sd;

	sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sd < 0) {
		logit(LOG_ERR, errno, "Failed creating IPC socket");
		return;
	}

#ifdef HAVE_SOCKADDR_UN_SUN_LEN
	sun.sun_len = 0;	/* <- correct length is set by the OS */
#endif
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, _PATH_MROUTED_SOCK, sizeof(sun.sun_path));

	unlink(sun.sun_path);
	logit(LOG_DEBUG, 0, "Binding IPC socket to %s", sun.sun_path);

	len = offsetof(struct sockaddr_un, sun_path) + strlen(sun.sun_path);
	if (bind(sd, (struct sockaddr *)&sun, len) < 0 || listen(sd, 1)) {
		logit(LOG_WARNING, errno, "Failed binding IPC socket, %s", sun.sun_path);
		logit(LOG_NOTICE, 0, "mroutectl client support disabled");
		close(sd);
		return;
	}

	if (register_input_handler(sd, ipc_handle) < 0)
		logit(LOG_ERR, 0, "Failed registering IPC handler");

	ipc_socket = sd;
}

void ipc_exit(void)
{
	if (ipc_socket > -1)
		close(ipc_socket);

	unlink(sun.sun_path);
	ipc_socket = -1;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
