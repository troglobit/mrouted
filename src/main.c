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

int haveterminal = 1;
int did_final_init = 0;

int cache_lifetime 	= DEFAULT_CACHE_LIFETIME;
int prune_lifetime	= AVERAGE_PRUNE_LIFETIME;

int startupdelay = 0;
int mrt_table_id = 0;

int debug = 0;
int running = 1;
int use_syslog = 1;
time_t mrouted_init_time;

char *config_file = NULL;
char *pid_file    = NULL;
char *sock_file   = NULL;

static char *ident = PACKAGE_NAME;

/*
 * Forward declarations.
 */
static void final_init     (int, void *);
static void fasttimer      (int, void *);
static void timer          (int, void *);
static void handle_signals (int, void *);
static int  timeout        (int);
static void cleanup        (void);


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

/*
 * _PATH_MROUTED_GENID is the configurable fallback and old default used
 * by mrouted, which does not comply with FHS.  We only read that if it
 * exists, otherwise we use the system _PATH_VARDB, which works on all
 * *BSD and GLIBC based Linux systems.  Some Linux systms don't have the
 * correct FHS /var/lib/misc for that define, so we check for that too.
 */
static FILE *fopen_genid(const char *mode)
{
    const char *path = _PATH_VARDB;
    char fn[80];

    /* If old /var/lib/mrouted.genid exists, use that for compat. */
    snprintf(fn, sizeof(fn), _PATH_MROUTED_GENID, ident);
    if (access(fn, F_OK)) {
#ifdef __linux__
	/*
	 * Workaround for Linux systems where _PATH_VARDB is /var/db but
	 * the rootfs doesn't have it.  Let's check for /var/lib/misc
	 */
	if (access(path, W_OK))
	    path = PRESERVEDIR "/misc";
#endif
	if (!access(path, W_OK))
	    snprintf(fn, sizeof(fn), "%s/%s.genid", path, ident);
    }

    /* If all fails we fall back to try _PATH_MROUTED_GENID */
    return fopen(fn, mode);
}

static uint32_t rand_genid(void)
{
	struct timespec tv;

	clock_gettime(CLOCK_MONOTONIC, &tv);

	return (uint32_t)tv.tv_sec;	/* for a while after 2038 */
}

static void init_genid(void)
{
    FILE *fp;

    fp = fopen_genid("r");
    if (fp) {
	uint32_t prev_genid;
	int ret;

	ret = fscanf(fp, "%u", &prev_genid);
	(void)fclose(fp);

	if (ret == 1)
	    dvmrp_genid = prev_genid + 1;
	else
	    dvmrp_genid = rand_genid();
    } else
	dvmrp_genid = rand_genid();

    fp = fopen_genid("w");
    if (fp) {
	fprintf(fp, "%u\n", dvmrp_genid);
	(void)fclose(fp);
    }
}

static int compose_paths(void)
{
    /* Default .conf file path: "/etc" + '/' + "pimd" + ".conf" */
    if (!config_file) {
	size_t len = strlen(SYSCONFDIR) + strlen(ident) + 7;

	config_file = malloc(len);
	if (!config_file) {
	    logit(LOG_ERR, errno, "Failed allocating memory, exiting.");
	    exit(1);
	}

	snprintf(config_file, len, _PATH_MROUTED_CONF, ident);
    }

    /* Default is to let pidfile() API construct PID file from ident */
    if (!pid_file)
	pid_file = strdup(ident);

    return 0;
}

static int usage(int code)
{
    printf("Usage: mrouted [-himnpsv] [-f FILE] [-i NAME] [-d SYS[,SYS...]] [-l LEVEL]\n"
	   "                          [-p FILE] [-u FILE] [-w SEC]\n"
	   "\n"
	   "  -d, --debug=SYS[,SYS]    Debug subsystem(s), see below for valid system names\n"
	   "  -f, --config=FILE        Configuration file to use, default /etc/mrouted.conf\n"
	   "  -h, --help               Show this help text\n"
	   "  -i, --ident=NAME         Identity for syslog, .cfg & .pid file, default: mrouted\n"
	   "  -l, --loglevel=LEVEL     Set log level: none, err, notice (default), info, debug\n"
	   "  -n, --foreground         Run in foreground, do not detach from controlling terminal\n"
	   "  -p, --pidfile=FILE       File to store process ID for signaling daemon\n"
	   "  -s, --syslog             Log to syslog, default unless running in --foreground\n"
#ifdef __linux__
	   "  -t, --table-id=ID        Set multicast routing table ID.  Allowed table ID#:\n"
	   "                           0 .. 999999999.  Default: 0 (use default table)\n"
#endif
	   "  -u, --ipc=FILE           Override UNIX domain socket, default from identity, -i\n"
	   "  -v, --version            Show mrouted version\n"
	   "  -w, --startup-delay=SEC  Startup delay before forwarding\n");

    fputs("\nValid debug subsystems:\n", stderr);
    debug_print();

    printf("\nBug report address: %-40s\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
    printf("Project homepage: %s\n", PACKAGE_URL);
#endif

    return code;
}

int main(int argc, char *argv[])
{
    struct option long_options[] = {
	{ "debug",         2, 0, 'd' },
	{ "config",        1, 0, 'f' },
	{ "help",          0, 0, 'h' },
	{ "ident",         1, 0, 'i' },
	{ "loglevel",      1, 0, 'l' },
	{ "foreground",    0, 0, 'n' },
	{ "pidfile",       1, 0, 'p' },
	{ "syslog",        0, 0, 's' },
#ifdef __linux__
	{ "table-id",      1, 0, 't' },
#endif
	{ "ipc",           1, 0, 'u' },
	{ "version",       0, 0, 'v' },
	{ "startup-delay", 1, 0, 'w' },
	{ NULL, 0, 0, 0 }
    };
    int foreground = 0;
    int vers, ch;

    while ((ch = getopt_long(argc, argv, "d:f:hi:l:np:st:u:vw:", long_options, NULL)) != EOF) {
#ifdef __linux__
	const char *errstr = NULL;
#endif

	switch (ch) {
	    case 'd':
		if (!strcmp(optarg, "?")) {
		    debug_print();
		    return 0;
		}

		debug = debug_parse(optarg);
		if ((int)DEBUG_PARSE_ERR == debug)
		    return usage(1);
		break;

	    case 'f':
		config_file = optarg;
		break;

	    case 'h':
		return usage(0);

	    case 'i':	/* --ident=NAME */
		ident = optarg;
		break;

	    case 'l':
		if (!strcmp(optarg, "?")) {
		    char buf[128];

		    log_list(buf, sizeof(buf));
		    return !puts(buf);
		}

		loglevel = log_str2lvl(optarg);
		if (-1 == loglevel)
		    return usage(1);
		break;

	    case 'n':
		foreground = 1;
		use_syslog--;
		break;
	    case 'p':	/* --pidfile=NAME */
		pid_file = strdup(optarg);
		break;

	    case 's':	/* --syslog */
		use_syslog++;
		break;

	    case 't':
#ifndef __linux__
		errx(1, "-t ID is currently only supported on Linux");
#else
		mrt_table_id = strtonum(optarg, 0, 999999999, &errstr);
		if (errstr) {
		    fprintf(stderr, "Table ID %s!\n", errstr);
		    return usage(1);
		}
#endif
		break;

	    case 'u':
		sock_file = strdup(optarg);
		break;

	    case 'v':
		printf("%s\n", versionstring);
		return 0;

	    case 'w':
		startupdelay = atoi(optarg);
		break;

	    default:
		return usage(1);
	}
    }

    /* Check for unsupported command line arguments */
    argc -= optind;
    if (argc > 0)
	return usage(1);

    if (geteuid() != 0) {
	fprintf(stderr, "%s: must be root\n", ident);
	exit(1);
    }

    compose_paths();
    setlinebuf(stderr);

    if (debug != 0) {
	char buf[512];

	debug_list(debug, buf, sizeof(buf));
	fprintf(stderr, "debug level 0x%x (%s)\n", debug, buf);
    }

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
	    (void)ioctl(fd, TIOCNOTTY, NULL);
	    (void)close(fd);
	}
#else
	if (setsid() < 0)
	    perror("setsid");
#endif
    }

    /*
     * Setup logging
     */
    log_init(ident);
    logit(LOG_DEBUG, 0, "%s starting", versionstring);

    do_randomize();

    /*
     * Get generation id
     */
    init_genid();

    pev_init();
    igmp_init();

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
    ipc_init(sock_file, ident);

    pev_timer_add(0, 1000000, fasttimer, NULL);
    pev_timer_add(0, TIMER_INTERVAL * 1000000, timer, NULL);

    pev_sig_add(SIGHUP,  handle_signals, NULL);
    pev_sig_add(SIGINT,  handle_signals, NULL);
    pev_sig_add(SIGTERM, handle_signals, NULL);
    pev_sig_add(SIGUSR1, handle_signals, NULL);
    pev_sig_add(SIGUSR2, handle_signals, NULL);

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
    if (startupdelay > 0)
	pev_timer_add(startupdelay * 1000000, 0, final_init, NULL);
    else
	final_init(0, NULL);

    /* Signal world we are now ready to start taking calls */
    if (pidfile(pid_file))
	logit(LOG_WARNING, errno, "Cannot create pidfile");

    pev_run();

    logit(LOG_NOTICE, 0, "%s exiting", versionstring);
    cleanup();

    return 0;
}

static void final_init(int id, void *arg)
{
    char *s = (char *)arg;

    logit(LOG_NOTICE, 0, "%s%s", versionstring, s ? s : "");
    if (s)
	free(s);
    if (id > 0)
	pev_timer_del(id);

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
static void fasttimer(int id, void *arg)
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
static void timer(int id, void *arg)
{
    age_routes();	/* Advance the timers in the route entries  */
    age_vifs();		/* Advance the timers for neighbors         */
    age_table_entry();	/* Advance the timers for the cache entries */

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
    virtual_time += pev_timer_get(id) / 1000000;
}

static void cleanup(void)
{
    static int in_cleanup = 0;

    if (!in_cleanup) {
	in_cleanup++;
	expire_all_routes();
	report_to_all_neighbors(ALL_ROUTES);

	free_all_prunes();
	free_all_routes();
	stop_all_vifs();

	if (did_final_init)
	    k_stop_dvmrp();
	close(udp_socket);

	igmp_exit();
    }
}

/*
 * Signal handler.  Take note of the fact that the signal arrived
 * so that the main loop can take care of it.
 */
static void handle_signals(int sig, void *arg)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
	pev_exit(0);
	break;

    case SIGHUP:
	restart();
	break;

    case SIGUSR1:
	logit(LOG_INFO, 0, "SIGUSR1 is no longer supported, use mroutectl instead.");
	break;

    case SIGUSR2:
	logit(LOG_INFO, 0, "SIGUSR2 is no longer supported, use mroutectl instead.");
	break;
    }
}

/*
 * Restart mrouted
 */
void restart(void)
{
    char *s;

    s = strdup (" restart");
    if (s == NULL)
	logit(LOG_ERR, 0, "out of memory");

    /*
     * reset all the entries
     */
    free_all_prunes();
    free_all_routes();
    stop_all_vifs();
    k_stop_dvmrp();
    igmp_exit();
#ifndef IOCTL_OK_ON_RAW_SOCKET
    close(udp_socket);
#endif
    did_final_init = 0;

    /*
     * start processing again
     */
    init_genid();

    igmp_init();
    init_routes();
    init_ktable();
    init_vifs();
    /*XXX Schedule final_init() as main does? */
    final_init(0, s);

    /* Touch PID file to acknowledge SIGHUP */
    pidfile(pid_file);
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

    snprintf(p, SCALETIMEBUFLEN, "%2d:%02d:%02d", (int)(t / 3600),
	     (int)((t % 3600) / 60), (int)(t % 60));

    return p;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "cc-mode"
 * End:
 */
