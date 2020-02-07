/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */
#include "defs.h"

struct debugnm debugnames[] = {
    {	"packet",	DEBUG_PKT,	2	},
    {	"pkt",		DEBUG_PKT,	3	},
    {	"pruning",	DEBUG_PRUNE,	1	},
    {	"prunes",	DEBUG_PRUNE,	1	},
    {	"routing",	DEBUG_ROUTE,	1	},
    {	"routes",	DEBUG_ROUTE,	1	},
    {   "route-detail",	DEBUG_RTDETAIL, 6	},
    {   "rtdetail",	DEBUG_RTDETAIL, 2	},
    {	"neighbors",	DEBUG_PEER,	1	},
    {	"peers",	DEBUG_PEER,	2	},
    {   "kernel",       DEBUG_KERN,     2       },
    {	"cache",	DEBUG_CACHE,	1	},
    {	"timer",	DEBUG_TIMEOUT,	2	},
    {	"vif",		DEBUG_IF,	1	},
    {	"interface",	DEBUG_IF,	2	},
    {	"groups",	DEBUG_MEMBER,	1	},
    {	"membership",	DEBUG_MEMBER,	1	},
    {	"mtrace",	DEBUG_TRACE,	2	},
    {	"traceroute",	DEBUG_TRACE,	2	},
    {	"igmp",		DEBUG_IGMP,	1	},
    {	"icmp",		DEBUG_ICMP,	2	},
    {	"rsrr",		DEBUG_RSRR,	2	},
    {	"all",		DEBUG_ALL,	1	},
    {	"3",		DEBUG_ALL,	1	}	/* compat. */
};

int debug_list(int mask, char *buf, size_t len)
{
    struct debugnm *d;
    size_t i;

    memset(buf, 0, len);
    for (i = 0, d = debugnames; i < ARRAY_LEN(debugnames); i++, d++) {
	if (!(mask & d->level))
	    continue;

	if (mask != (int)DEBUG_ALL)
	    mask &= ~d->level;

	strlcat(buf, d->name, len);

	if (mask && i + 1 < ARRAY_LEN(debugnames))
	    strlcat(buf, ", ", len);
    }

    return 0;
}

void debug_print(void)
{
    char buf[768];

    if (!debug_list(DEBUG_ALL, buf, sizeof(buf))) {
	char line[82] = "  ";
	char *ptr;

	ptr = strtok(buf, " ");
	while (ptr) {
	    char *sys = ptr;
	    char buf[20];

	    ptr = strtok(NULL, " ");

	    /* Flush line */
	    if (strlen(line) + strlen(sys) + 3 >= sizeof(line)) {
		puts(line);
		strlcpy(line, "  ", sizeof(line));
	    }

	    if (ptr)
		snprintf(buf, sizeof(buf), "%s ", sys);
	    else
		snprintf(buf, sizeof(buf), "%s", sys);

	    strlcat(line, buf, sizeof(line));
	}

	puts(line);
    }
}

int debug_parse(char *arg)
{
    struct debugnm *d;
    size_t i, len;
    char *next = NULL;
    int sys = 0;

    if (!arg || !strlen(arg) || strstr(arg, "none"))
	return sys;

    while (arg) {
	next = strchr(arg, ',');
	if (next)
	    *next++ = '\0';

	len = strlen(arg);
	for (i = 0, d = debugnames; i < ARRAY_LEN(debugnames); i++, d++) {
	    if (len >= d->nchars && strncmp(d->name, arg, len) == 0)
		break;
	}

	if (i == ARRAY_LEN(debugnames)) {
	    fprintf(stderr, "Unknown debug level: %s\n", arg);
	    return DEBUG_PARSE_ERR;
	}

	sys |= d->level;
	arg = next;
    }

    return sys;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
