/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

/*
 * Written by Steve Deering, Stanford University, February 1989.
 *
 * (An earlier version of DVMRP was implemented by David Waitzman of
 *  BBN STC by extending Berkeley's routed program.  Some of Waitzman's
 *  extensions have been incorporated into mrouted, but none of the
 *  original routed code has been adopted.)
 */

#include "defs.h"
#include <err.h>
#include <getopt.h>
#include <paths.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/stat.h>

extern char *configfilename;
char versionstring[MAX_VERSION_LEN];

static int haveterminal = 1;
int did_final_init = 0;

static int sighandled = 0;
#define	GOT_SIGINT	0x01
#define	GOT_SIGHUP	0x02
#define	GOT_SIGUSR1	0x04
#define	GOT_SIGUSR2	0x08

int cache_lifetime 	= DEFAULT_CACHE_LIFETIME;
int prune_lifetime	= AVERAGE_PRUNE_LIFETIME;

int startupdelay = DEFAULT_STARTUP_DELAY;
int vifstatedefault = 0;
int missingok = 0;

int debug = 0;
int running = 1;
time_t mrouted_init_time;

#define NHANDLERS	2
static struct ihandler {
    int fd;			/* File descriptor	*/
    ihfunc_t func;		/* Function to call	*/
} ihandlers[NHANDLERS];
static int nhandlers = 0;

static struct debugname {
    char	*name;
    uint32_t	 level;
    size_t	 nchars;
} debugnames[] = {
    {	"packet",	DEBUG_PKT,	2	},
    {	"pkt",		DEBUG_PKT,	3	},
    {	"pruning",	DEBUG_PRUNE,	1	},
    {	"prunes",	DEBUG_PRUNE,	1	},
    {	"routing",	DEBUG_ROUTE,	1	},
    {	"routes",	DEBUG_ROUTE,	1	},
    {   "route_detail",	DEBUG_RTDETAIL, 6	},
    {   "rtdetail",	DEBUG_RTDETAIL, 2	},
    {	"neighbors",	DEBUG_PEER,	1	},
    {	"peers",	DEBUG_PEER,	2	},
    {   "kernel",       DEBUG_KERN,     2       },
    {	"cache",	DEBUG_CACHE,	1	},
    {	"timeout",	DEBUG_TIMEOUT,	1	},
    {	"callout",	DEBUG_TIMEOUT,	2	},
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

/*
 * Forward declarations.
 */
static void final_init(void *);
static void fasttimer(void*);
static void timer(void*);
static void handle_signals(int);
static int  check_signals(void);
static int  timeout(int);
static void cleanup(void);
static void resetlogging(void *);

int register_input_handler(int fd, ihfunc_t func)
{
    if (nhandlers >= NHANDLERS)
	return -1;

    ihandlers[nhandlers].fd = fd;
    ihandlers[nhandlers++].func = func;

    return 0;
}

static void do_randomize(void)
{
#define rol32(data,shift) ((data) >> (shift)) | ((data) << (32 - (shift)))
   int fd;
   unsigned int seed;

   /* Setup a fallback seed based on quasi random. */
   seed = time(NULL) ^ gethostid();
   seed = rol32(seed, seed);

   fd = open("/dev/urandom", O_RDONLY);
   if (fd >= 0) {
       if (-1 == read(fd, &seed, sizeof(seed)))
	   warn("Failed reading entropy from /dev/urandom");
       close(fd);
  }

   srandom(seed);
}

static FILE *fopen_genid(char *mode)
{
    char fn[80];

    snprintf(fn, sizeof(fn), _PATH_MROUTED_GENID);

    return fopen(fn, mode);
}

static void init_gendid(void)
{
    FILE *fp;

    fp = fopen_genid("r");
    if (!fp) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	dvmrp_genid = (uint32_t)tv.tv_sec;	/* for a while after 2038 */
    } else {
	uint32_t prev_genid;
	int ret;

	ret = fscanf(fp, "%u", &prev_genid);
	if (ret == 1 && prev_genid == dvmrp_genid)
	    dvmrp_genid++;
	(void)fclose(fp);
    }

    fp = fopen_genid("w");
    if (fp) {
	fprintf(fp, "%u\n", dvmrp_genid);
	(void)fclose(fp);
    }
}

int debug_list(int mask, char *buf, size_t len)
{
    struct debugname *d;
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

int debug_parse(char *arg)
{
    struct debugname *d;
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

static int usage(int code)
{
    char buf[768];

    printf("Usage: mrouted [-hnprv] [-c file] [-d level[,level...]]\n"
	   "\n"
	   "  -c, --config=FILE          Configuration file to use, default /etc/mrouted.conf\n"
	   "  -d, --debug=LEVEL          Debug level, see below for valid levels\n"
	   "  -n, --foreground           Run in foreground, do not detach from calling terminal\n"
	   "  -h, --help                 Show this help text\n"
	   "      --no-interfaces        Disable all interfaces by default\n"
	   "      --missing-ok           Missing interfaces are OK\n"
	   "      --startup-delay=DELAY  Set startup delay before forwarding, default %d seconds\n"
	   "  -p                         Disable pruning.  Deprecated, compatibility option\n"
	   "  -r, --show-routes          Show state of VIFs and multicast routing tables\n"
	   "  -v, --version              Show mrouted version\n", DEFAULT_STARTUP_DELAY);

    fputs("\nValid debug levels:\n", stderr);
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

    return code;
}

int main(int argc, char *argv[])
{
    FILE *fp;
    int foreground = 0;
    int vers, n = -1, i, ch;
    struct pollfd *pfd;
    struct sigaction sa;
    struct option long_options[] = {
	{"config", 1, 0, 'c'},
	{"debug", 2, 0, 'd'},
	{"foreground", 0, 0, 'n'},
	{"help", 0, 0, 'h'},
	{"version", 0, 0, 'v'},
	{"no-intefaces", 0, 0, 'N'},
	{"missing-ok", 0, 0, 'M'},
	{"startup-delay", 1, 0, 'D'},
	{0, 0, 0, 0}
    };

    snprintf(versionstring, sizeof(versionstring), "mrouted version %s", PACKAGE_VERSION);

    while ((ch = getopt_long(argc, argv, "D:MNnc:d:fhpv", long_options, NULL)) != EOF) {
	switch (ch) {
	    case 'D':
		startupdelay = atoi(optarg);
		break;

	    case 'M':
		missingok++;
		break;

	    case 'N':
		vifstatedefault = VIFF_DISABLED;
		break;

	    case 'c':
		configfilename = optarg;
		break;

	    case 'd':
		debug = debug_parse(optarg);
		if ((int)DEBUG_PARSE_ERR == debug)
		    return usage(1);
		break;

	    case 'f': /* compat */
		/* fallthrough */
	    case 'n':
		foreground = 1;
		break;

	    case 'h':
		return usage(0);

	    case 'p':
		warnx("Disabling pruning is no longer supported.");
		break;

	    case 'v':
		printf("%s\n", versionstring);
		return 0;

	    default:
		return usage(1);
	}
    }

    /* Check for unsupported command line arguments */
    argc -= optind;
    if (argc > 0)
	return usage(1);

    if (geteuid() != 0) {
	fprintf(stderr, "%s: must be root\n", PACKAGE_NAME);
	exit(1);
    }
    setlinebuf(stderr);

    if (debug != 0) {
	struct debugname *d;
	char c;
	int tmpd = debug;

	fprintf(stderr, "debug level 0x%x ", debug);
	c = '(';
	for (d = debugnames; d < debugnames + ARRAY_LEN(debugnames); d++) {
	    if ((tmpd & d->level) == d->level) {
		tmpd &= ~d->level;
		fprintf(stderr, "%c%s", c, d->name);
		c = ',';
	    }
	}
	fprintf(stderr, ")\n");
    }

    /*
     * Setup logging
     */
#ifdef LOG_DAEMON
    (void)openlog("mrouted", LOG_PID, LOG_DAEMON);
    (void)setlogmask(LOG_UPTO(LOG_NOTICE));
#else
    (void)openlog("mrouted", LOG_PID);
#endif

    logit(LOG_DEBUG, 0, "%s starting", versionstring);

    do_randomize();

    /*
     * Get generation id
     */
    init_gendid();

    /* Start up the log rate-limiter */
    resetlogging(NULL);

    callout_init();
    init_igmp();
    init_icmp();
    init_ipip();
    init_routes();
    init_ktable();

    /*
     * Unfortunately, you can't k_get_version() unless you've
     * k_init_dvmrp()'d.  Now that we want to move the
     * k_init_dvmrp() to later in the initialization sequence,
     * we have to do the disgusting hack of initializing,
     * getting the version, then stopping the kernel multicast
     * forwarding.
     */
    k_init_dvmrp();
    vers = k_get_version();
    k_stop_dvmrp();
    /*XXX
     * This function must change whenever the kernel version changes
     */
    if ((((vers >> 8) & 0xff) != 3) || ((vers & 0xff) != 5))
	logit(LOG_ERR, 0, "kernel (v%d.%d)/mrouted (v%d.%d) version mismatch",
	      (vers >> 8) & 0xff, vers & 0xff, PROTOCOL_VERSION, MROUTED_VERSION);

    init_vifs();
    ipc_init();
#ifdef RSRR
    rsrr_init();
#endif

    sa.sa_handler = handle_signals;
    sa.sa_flags = 0;	/* Interrupt system calls */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    pfd = calloc(sizeof(struct pollfd), 1 + nhandlers);
    if (!pfd)
	err(1, "Failed allocating internal memory");

    pfd[0].fd = igmp_socket;
    pfd[0].events = POLLIN;
    for (i = 0; i < nhandlers; i++) {
	pfd[i + 1].fd = ihandlers[i].fd;
	pfd[i + 1].events = POLLIN;
    }

    /* schedule first timer interrupt */
    timer_setTimer(1, fasttimer, NULL);
    timer_setTimer(TIMER_INTERVAL, timer, NULL);

    if (!debug && !foreground) {
#ifdef TIOCNOTTY
	int fd;
#endif

	/* Detach from the terminal */
	haveterminal = 0;
	if (fork())
	    exit(0);

	(void)close(0);
	(void)close(1);
	(void)close(2);
	(void)open("/dev/null", O_RDONLY);
	(void)dup2(0, 1);
	(void)dup2(0, 2);
#ifdef TIOCNOTTY
	fd = open("/dev/tty", O_RDWR);
	if (fd >= 0) {
	    (void)ioctl(fd, TIOCNOTTY, (char *)0);
	    (void)close(fd);
	}
#else
	if (setsid() < 0)
	    perror("setsid");
#endif
    }

    if (pidfile(NULL))
	warn("Cannot create pidfile");

    /* XXX HACK
     * This will cause black holes for the first few seconds after startup,
     * since we are exchanging routes but not actually forwarding.
     * However, it eliminates much of the startup transient.
     *
     * It's possible that we can set a flag which says not to report any
     * routes (just accept reports) until this timer fires, and then
     * do a report_to_all_neighbors(ALL_ROUTES) immediately before
     * turning on DVMRP.
     */
    timer_setTimer(startupdelay, final_init, NULL);

    /*
     * Main receive loop.
     */
    while (running) {
	if (check_signals())
	    break;

	n = poll(pfd, nhandlers + 1, timeout(n) * 1000);
	if (n < 0) {
	    if (errno != EINTR)
		logit(LOG_WARNING, errno, "poll failed");
	    continue;
	}

	if (n > 0) {
	    if (pfd[0].revents & POLLIN) {
		ssize_t len;

		len = recv(igmp_socket, recv_buf, RECV_BUF_SIZE, 0);
		if (len < 0) {
		    if (errno != EINTR)
			logit(LOG_ERR, errno, "recvfrom");
		    continue;
		}
		accept_igmp(len);
	    }

	    for (i = 0; i < nhandlers; i++) {
		if (pfd[i + 1].revents & POLLIN)
		    (*ihandlers[i].func)(ihandlers[i].fd);
	    }
	}
    }

    logit(LOG_NOTICE, 0, "%s exiting", versionstring);
    free(pfd);
    cleanup();

    return 0;
}

static void final_init(void *i)
{
    char *s = (char *)i;

    logit(LOG_NOTICE, 0, "%s%s", versionstring, s ? s : "");
    if (s)
	free(s);

    k_init_dvmrp();		/* enable DVMRP routing in kernel */

    /*
     * Install the vifs in the kernel as late as possible in the
     * initialization sequence.
     */
    init_installvifs();

    time(&mrouted_init_time);
    did_final_init = 1;
}

/*
 * routine invoked every second.  Its main goal is to cycle through
 * the routing table and send partial updates to all neighbors at a
 * rate that will cause the entire table to be sent in ROUTE_REPORT_INTERVAL
 * seconds.  Also, every TIMER_INTERVAL seconds it calls timer() to
 * do all the other time-based processing.
 */
static void fasttimer(void *arg)
{
    static unsigned int tlast;
    static unsigned int nsent;
    unsigned int t = tlast + 1;
    int n;

    /*
     * if we're in the last second, send everything that's left.
     * otherwise send at least the fraction we should have sent by now.
     */
    if (t >= ROUTE_REPORT_INTERVAL) {
	int nleft = nroutes - nsent;

	while (nleft > 0) {
	    if ((n = report_next_chunk()) <= 0)
		break;
	    nleft -= n;
	}

	tlast = 0;
	nsent = 0;
    } else {
	unsigned int ncum = nroutes * t / ROUTE_REPORT_INTERVAL;

	while (nsent < ncum) {
	    if ((n = report_next_chunk()) <= 0)
		break;
	    nsent += n;
	}

	tlast = t;
    }

    timer_setTimer(1, fasttimer, NULL);
}

/*
 * The 'virtual_time' variable is initialized to a value that will cause the
 * first invocation of timer() to send a probe or route report to all vifs
 * and send group membership queries to all subnets for which this router is
 * querier.  This first invocation occurs approximately TIMER_INTERVAL seconds
 * after the router starts up.   Note that probes for neighbors and queries
 * for group memberships are also sent at start-up time, as part of initial-
 * ization.  This repetition after a short interval is desirable for quickly
 * building up topology and membership information in the presence of possible
 * packet loss.
 *
 * 'virtual_time' advances at a rate that is only a crude approximation of
 * real time, because it does not take into account any time spent processing,
 * and because the timer intervals are sometimes shrunk by a random amount to
 * avoid unwanted synchronization with other routers.
 */

uint32_t virtual_time = 0;


/*
 * Timer routine.  Performs periodic neighbor probing, route reporting, and
 * group querying duties, and drives various timers in routing entries and
 * virtual interface data structures.
 */
static void timer(void *arg)
{
    age_routes();	/* Advance the timers in the route entries     */
    age_vifs();		/* Advance the timers for neighbors */
    age_table_entry();	/* Advance the timers for the cache entries */

    if (virtual_time % IGMP_QUERY_INTERVAL == 0) {
	/*
	 * Time to query the local group memberships on all subnets
	 * for which this router is the elected querier.
	 */
	query_groups();
    }

    if (virtual_time % NEIGHBOR_PROBE_INTERVAL == 0) {
	/*
	 * Time to send a probe on all vifs from which no neighbors have
	 * been heard.  Also, check if any inoperative interfaces have now
	 * come up.  (If they have, they will also be probed as part of
	 * their initialization.)
	 */
	probe_for_neighbors();

	if (vifs_down)
	    check_vif_state();
    }

    delay_change_reports = FALSE;
    if (routes_changed) {
	/*
	 * Some routes have changed since the last timer interrupt, but
	 * have not been reported yet.  Report the changed routes to all
	 * neighbors.
	 */
	report_to_all_neighbors(CHANGED_ROUTES);
    }

    /*
     * Advance virtual time
     */
    virtual_time += TIMER_INTERVAL;
    timer_setTimer(TIMER_INTERVAL, timer, NULL);
}

/*
 * Handle timeout queue.
 *
 * If select + packet processing took more than 1 second,
 * or if there is a timeout pending, age the timeout queue.
 *
 * If not, collect usec in difftime to make sure that the
 * time doesn't drift too badly.
 *
 * If the timeout handlers took more than 1 second,
 * age the timeout queue again.  XXX This introduces the
 * potential for infinite loops!
 */
static int timeout(int n)
{
    static struct timeval difftime, curtime, lasttime;
    static int init = 1;
    int secs;

    /* Next timeout to poll() */
    secs = timer_nextTimer();

    do {
	/*
	 * If poll() timed out, then there's no other activity to
	 * account for and we don't need to call gettimeofday.
	 */
	if (n == 0) {
	    curtime.tv_sec = lasttime.tv_sec + secs;
	    curtime.tv_usec = lasttime.tv_usec;
	    n = -1; /* don't do this next time through the loop */
	} else {
	    gettimeofday(&curtime, NULL);
	    if (init) {
		init = 0;	/* First time only */
		lasttime = curtime;
		difftime.tv_usec = 0;
	    }
	}

	difftime.tv_sec = curtime.tv_sec - lasttime.tv_sec;
	difftime.tv_usec += curtime.tv_usec - lasttime.tv_usec;
	while (difftime.tv_usec > 1000000) {
	    difftime.tv_sec++;
	    difftime.tv_usec -= 1000000;
	}

	if (difftime.tv_usec < 0) {
	    difftime.tv_sec--;
	    difftime.tv_usec += 1000000;
	}
	lasttime = curtime;

	if (secs == 0 || difftime.tv_sec > 0)
	    age_callout_queue(difftime.tv_sec);

	secs = -1;
    } while (difftime.tv_sec > 0);

    return secs;
}

static void cleanup(void)
{
    static int in_cleanup = 0;

    if (!in_cleanup) {
	in_cleanup++;
#ifdef RSRR
	rsrr_clean();
#endif
	expire_all_routes();
	report_to_all_neighbors(ALL_ROUTES);
	if (did_final_init)
	    k_stop_dvmrp();
    }
}

/*
 * Signal handler.  Take note of the fact that the signal arrived
 * so that the main loop can take care of it.
 */
static void handle_signals(int sig)
{
    switch (sig) {
	case SIGINT:
	case SIGTERM:
	    sighandled |= GOT_SIGINT;
	    break;

	case SIGHUP:
	    sighandled |= GOT_SIGHUP;
	    break;

	case SIGUSR1:
	    sighandled |= GOT_SIGUSR1;
	    break;

	case SIGUSR2:
	    sighandled |= GOT_SIGUSR2;
	    break;
    }
}

static int check_signals(void)
{
    if (!sighandled)
	return 0;

    if (sighandled & GOT_SIGINT) {
	sighandled &= ~GOT_SIGINT;
	return 1;
    }

    if (sighandled & GOT_SIGHUP) {
	sighandled &= ~GOT_SIGHUP;
	restart();
    }

    if (sighandled & GOT_SIGUSR1) {
	sighandled &= ~GOT_SIGUSR1;
	logit(LOG_INFO, 0, "SIGUSR1 is no longer supported, use mroutectl instead.");
    }

    if (sighandled & GOT_SIGUSR2) {
	sighandled &= ~GOT_SIGUSR2;
	logit(LOG_INFO, 0, "SIGUSR2 is no longer supported, use mroutectl instead.");
    }

    return 0;
}

/*
 * Restart mrouted
 */
void restart(void)
{
    FILE *fp;
    char *s;

    s = strdup (" restart");
    if (s == NULL)
	logit(LOG_ERR, 0, "out of memory");

    /*
     * reset all the entries
     */
    free_all_prunes();
    free_all_routes();
    free_all_callouts();
    stop_all_vifs();
    k_stop_dvmrp();
    close(igmp_socket);
    close(udp_socket);
    did_final_init = 0;

    /*
     * start processing again
     */
    init_gendid();

    init_igmp();
    init_routes();
    init_ktable();
    init_vifs();
    /*XXX Schedule final_init() as main does? */
    final_init(s);

    /* schedule timer interrupts */
    timer_setTimer(1, fasttimer, NULL);
    timer_setTimer(TIMER_INTERVAL, timer, NULL);
}

#define LOG_MAX_MSGS	20	/* if > 20/minute then shut up for a while */
#define LOG_SHUT_UP	600	/* shut up for 10 minutes */
static int log_nmsgs = 0;

static void resetlogging(void *arg)
{
    int nxttime = 60;
    void *narg = NULL;

    if (arg == NULL && log_nmsgs > LOG_MAX_MSGS) {
	nxttime = LOG_SHUT_UP;
	narg = (void *)&log_nmsgs;	/* just need some valid void * */
	syslog(LOG_WARNING, "logging too fast, shutting up for %d minutes",
			LOG_SHUT_UP / 60);
    } else {
	log_nmsgs = 0;
    }

    timer_setTimer(nxttime, resetlogging, narg);
}

#define SCALETIMEBUFLEN 27
char *scaletime(time_t t)
{
    static char buf1[SCALETIMEBUFLEN];
    static char buf2[SCALETIMEBUFLEN];
    static char *buf = buf1;
    char *p;

    p = buf;
    if (buf == buf1)
	buf = buf2;
    else
	buf = buf1;

    snprintf(p, SCALETIMEBUFLEN, "%2ld:%02ld:%02ld", t / 3600, (t % 3600) / 60, t % 60);

    return p;
}

#ifdef RINGBUFFER
#define NLOGMSGS 10000
#define LOGMSGSIZE 200
char *logmsg[NLOGMSGS];
static int logmsgno = 0;

void printringbuf(void)
{
    FILE *f;
    int i;

    f = fopen("/var/tmp/mrouted.log", "a");
    if (f == NULL) {
	logit(LOG_ERR, errno, "Cannot open /var/tmp/mrouted.log");
	/*NOTREACHED*/
    }
    fprintf(f, "--------------------------------------------\n");

    i = (logmsgno + 1) % NLOGMSGS;

    while (i != logmsgno) {
	if (*logmsg[i]) {
	    fprintf(f, "%s\n", logmsg[i]);
	    *logmsg[i] = '\0';
	}
	i = (i + 1) % NLOGMSGS;
    }

    fclose(f);
}
#endif

/*
 * Log errors and other messages to the system log daemon and to stderr,
 * according to the severity of the message and the current debug level.
 * For errors of severity LOG_ERR or worse, terminate the program.
 */
void logit(int severity, int syserr, const char *format, ...)
{
    va_list ap;
    static char fmt[211] = "warning - ";
    char *msg;
    struct timeval now;
    time_t now_sec;
    struct tm *thyme;
#ifdef RINGBUFFER
    static int ringbufinit = 0;
#endif

    va_start(ap, format);
    vsnprintf(&fmt[10], sizeof(fmt) - 10, format, ap);
    va_end(ap);
    msg = (severity == LOG_WARNING) ? fmt : &fmt[10];

#ifdef RINGBUFFER
    if (!ringbufinit) {
	int i;

	for (i = 0; i < NLOGMSGS; i++) {
	    logmsg[i] = malloc(LOGMSGSIZE);
	    if (logmsg[i] == 0) {
		syslog(LOG_ERR, "Out of memory");
		exit(1);
	    }
	    *logmsg[i] = 0;
	}
	ringbufinit = 1;
    }
    gettimeofday(&now, NULL);
    now_sec = now.tv_sec;
    thyme = localtime(&now_sec);
    snprintf(logmsg[logmsgno++], LOGMSGSIZE, "%02d:%02d:%02d.%03ld %s err %d",
	     thyme->tm_hour, thyme->tm_min, thyme->tm_sec,
	     now.tv_usec / 1000, msg, syserr);
    logmsgno %= NLOGMSGS;
    if (severity <= LOG_NOTICE)
#endif
    /*
     * Log to stderr if we haven't forked yet and it's a warning or worse,
     * or if we're debugging.
     */
    if (haveterminal && (debug || severity <= LOG_WARNING)) {
	gettimeofday(&now, NULL);
	now_sec = now.tv_sec;
	thyme = localtime(&now_sec);
	if (!debug)
	    fprintf(stderr, "%s: ", PACKAGE_NAME);
	fprintf(stderr, "%02d:%02d:%02d.%03ld %s", thyme->tm_hour,
		    thyme->tm_min, thyme->tm_sec, now.tv_usec / 1000, msg);
	if (syserr == 0)
	    fprintf(stderr, "\n");
	else
	    fprintf(stderr, ": %s\n", strerror(syserr));
    }

    /*
     * Always log things that are worse than warnings, no matter what
     * the log_nmsgs rate limiter says.
     * Only count things worse than debugging in the rate limiter
     * (since if you put daemon.debug in syslog.conf you probably
     * actually want to log the debugging messages so they shouldn't
     * be rate-limited)
     */
    if ((severity < LOG_WARNING) || (log_nmsgs < LOG_MAX_MSGS)) {
	if (severity < LOG_DEBUG)
	    log_nmsgs++;
	if (syserr != 0) {
	    errno = syserr;
	    syslog(severity, "%s: %s", msg, strerror(errno));
	} else
	    syslog(severity, "%s", msg);
    }

    if (severity <= LOG_ERR) exit(1);
}

#ifdef DEBUG_MFC
void md_log(int what, uint32_t origin, uint32_t mcastgrp)
{
    static FILE *f = NULL;
    struct timeval tv;
    uint32_t buf[4];

    if (!f) {
	if ((f = fopen("/tmp/mrouted.clog", "w")) == NULL) {
	    logit(LOG_ERR, errno, "open /tmp/mrouted.clog");
	}
    }

    gettimeofday(&tv, NULL);
    buf[0] = tv.tv_sec;
    buf[1] = what;
    buf[2] = origin;
    buf[3] = mcastgrp;

    fwrite(buf, sizeof(uint32_t), 4, f);
}
#endif

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
