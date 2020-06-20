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

extern char *configfilename;

int haveterminal = 1;
int did_final_init = 0;

static int sighandled = 0;
#define	GOT_SIGINT	0x01
#define	GOT_SIGHUP	0x02
#define	GOT_SIGUSR1	0x04
#define	GOT_SIGUSR2	0x08

int cache_lifetime 	= DEFAULT_CACHE_LIFETIME;
int prune_lifetime	= AVERAGE_PRUNE_LIFETIME;

int startupdelay = 0;

int debug = 0;
int running = 1;
int use_syslog = 1;
time_t mrouted_init_time;

#define NHANDLERS	2
static struct ihandler {
    int fd;			/* File descriptor	*/
    ihfunc_t func;		/* Function to call	*/
} ihandlers[NHANDLERS];
static int nhandlers = 0;

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
	struct timespec tv;

	clock_gettime(CLOCK_MONOTONIC, &tv);
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

static int usage(int code)
{
    printf("Usage: mrouted [-himnpsv] [-f FILE] [-d SYS[,SYS...]] [-l LEVEL]\n"
	   "\n"
	   "  -d, --debug=SYS[,SYS]    Debug subsystem(s), see below for valid system names\n"
	   "  -f, --config=FILE        Configuration file to use, default /etc/mrouted.conf\n"
	   "  -h, --help               Show this help text\n"
	   "  -l, --loglevel=LEVEL     Set log level: none, err, notice (default), info, debug\n"
	   "  -n, --foreground         Run in foreground, do not detach from controlling terminal\n"
	   "  -s, --syslog             Log to syslog, default unless running in --foreground\n"
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
    FILE *fp;
    int foreground = 0;
    int vers, n = -1, i, ch;
    struct pollfd *pfd;
    struct sigaction sa;
    struct option long_options[] = {
	{ "config",        1, 0, 'f' },
	{ "debug",         2, 0, 'd' },
	{ "foreground",    0, 0, 'n' },
	{ "help",          0, 0, 'h' },
	{ "loglevel",      1, 0, 'l' },
	{ "version",       0, 0, 'v' },
	{ "startup-delay", 1, 0, 'w' },
	{ NULL, 0, 0, 0 }
    };

    while ((ch = getopt_long(argc, argv, "d:f:hl:nsvw:", long_options, NULL)) != EOF) {
	switch (ch) {
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

	    case 'f':
		configfilename = optarg;
		break;

	    case 'd':
		if (!strcmp(optarg, "?")) {
		    debug_print();
		    return 0;
		}

		debug = debug_parse(optarg);
		if ((int)DEBUG_PARSE_ERR == debug)
		    return usage(1);
		break;

	    case 'n':
		foreground = 1;
		use_syslog--;
		break;

	    case 'h':
		return usage(0);

	    case 's':	/* --syslog */
		use_syslog++;
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
	fprintf(stderr, "%s: must be root\n", PACKAGE_NAME);
	exit(1);
    }
    setlinebuf(stderr);

    if (debug != 0) {
	char buf[512];

	debug_list(debug, buf, sizeof(buf));
	fprintf(stderr, "debug level 0x%x (%s)\n", debug, buf);
    }

    /*
     * Setup logging
     */
    log_init();
    logit(LOG_DEBUG, 0, "%s starting", versionstring);

    do_randomize();

    /*
     * Get generation id
     */
    init_gendid();

    timer_init();
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
    timer_set(1, fasttimer, NULL);
    timer_set(TIMER_INTERVAL, timer, NULL);

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
    if (startupdelay > 0)
	timer_set(startupdelay, final_init, NULL);
    else
	final_init(NULL);

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

    timer_set(1, fasttimer, NULL);
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
    timer_set(TIMER_INTERVAL, timer, NULL);

    age_routes();	/* Advance the timers in the route entries     */
    age_vifs();		/* Advance the timers for neighbors */
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
    virtual_time += TIMER_INTERVAL;
}

/*
 * Handle timeout queue.
 *
 * If poll() + packet processing took more than 1 second, or if there is
 * a timeout pending, age the timeout queue.  If not, collect usec in
 * difftime to make sure that the time doesn't drift too badly.
 *
 * XXX: If the timeout handlers took more than 1 second, age the timeout
 * queue again.  Note, this introduces the potential for infinite loops!
 */
static int timeout(int n)
{
    static struct timespec difftime, curtime, lasttime;
    static int init = 1, secs = 0;

    /* Age queue */
    do {
	/*
	 * If poll() timed out, then there's no other activity to
	 * account for and we don't need to call clock_gettime().
	 */
	if (n == 0) {
	    curtime.tv_sec = lasttime.tv_sec + secs;
	    curtime.tv_nsec = lasttime.tv_nsec;
	    n = -1; /* don't do this next time through the loop */
	} else {
	    clock_gettime(CLOCK_MONOTONIC, &curtime);
	    if (init) {
		init = 0;	/* First time only */
		lasttime = curtime;
		difftime.tv_nsec = 0;
	    }
	}

	difftime.tv_sec = curtime.tv_sec - lasttime.tv_sec;
	difftime.tv_nsec += curtime.tv_nsec - lasttime.tv_nsec;
	while (difftime.tv_nsec > 1000000000) {
	    difftime.tv_sec++;
	    difftime.tv_nsec -= 1000000000;
	}

	if (difftime.tv_nsec < 0) {
	    difftime.tv_sec--;
	    difftime.tv_nsec += 1000000000;
	}
	lasttime = curtime;

	if (secs == 0 || difftime.tv_sec > 0)
	    timer_age_queue(difftime.tv_sec);

	secs = -1;
    } while (difftime.tv_sec > 0);

    /* Next timer to wait for */
    secs = timer_next_delay();

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

	free_all_prunes();
	free_all_routes();
	stop_all_vifs();

	if (did_final_init)
	    k_stop_dvmrp();
	close(udp_socket);

	timer_exit();
	igmp_exit();
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
    timer_stop_all();
    stop_all_vifs();
    k_stop_dvmrp();
    igmp_exit();
    close(udp_socket);
    did_final_init = 0;

    /*
     * start processing again
     */
    init_gendid();

    igmp_init();
    init_routes();
    init_ktable();
    init_vifs();
    /*XXX Schedule final_init() as main does? */
    final_init(s);

    /* schedule timer interrupts */
    timer_set(1, fasttimer, NULL);
    timer_set(TIMER_INTERVAL, timer, NULL);
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

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
