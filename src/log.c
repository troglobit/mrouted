/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#define SYSLOG_NAMES
#include "defs.h"
#include <stdarg.h>

#define LOG_MAX_MSGS	20	/* if > 20/minute then shut up for a while */
#define LOG_SHUT_UP	600	/* shut up for 10 minutes */

#ifndef INTERNAL_NOPRI
#define INTERNAL_NOPRI  0x10
#endif

CODE prionm[] =
{
	{ "none",    INTERNAL_NOPRI },		/* INTERNAL */
	{ "crit",    LOG_CRIT       },
	{ "alert",   LOG_ALERT      },
	{ "panic",   LOG_EMERG      },
	{ "error",   LOG_ERR        },
	{ "warning", LOG_WARNING    },
	{ "notice",  LOG_NOTICE     },
	{ "info",    LOG_INFO       },
	{ "debug",   LOG_DEBUG      },
	{ NULL, -1 }
};

int loglevel = LOG_NOTICE;

static char *log_name = PACKAGE_NAME;


int log_str2lvl(char *level)
{
    int i;

    for (i = 0; prionm[i].c_name; i++) {
	size_t len = MIN(strlen(prionm[i].c_name), strlen(level));

	if (!strncasecmp(prionm[i].c_name, level, len))
	    return prionm[i].c_val;
    }

    return atoi(level);
}

const char *log_lvl2str(int val)
{
    int i;

    for (i = 0; prionm[i].c_name; i++) {
	if (prionm[i].c_val == val)
	    return prionm[i].c_name;
    }

    return "unknown";
}

int log_list(char *buf, size_t len)
{
    int i;

    memset(buf, 0, len);
    for (i = 0; prionm[i].c_name; i++) {
	if (i > 0)
	    strlcat(buf, ", ", len);
	strlcat(buf, prionm[i].c_name, len);
    }

    return 0;
}



/*
 * Open connection to syslog daemon and set initial log level
 */
void log_init(char *ident)
{
    log_name = ident;
    if (!use_syslog)
	return;

    openlog(ident, LOG_PID, LOG_DAEMON);
    setlogmask(LOG_UPTO(loglevel));
}


/*
 * Log errors and other messages to the system log daemon and to stderr,
 * according to the severity of the message and the current debug level.
 * For errors of severity LOG_ERR or worse, terminate the program.
 */
void logit(int severity, int syserr, const char *format, ...)
{
    static char fmt[211] = "warning - ";
    const struct tm *thyme;
    struct timeval now;
    time_t now_sec;
    va_list ap;
    char *msg;

    va_start(ap, format);
    vsnprintf(&fmt[10], sizeof(fmt) - 10, format, ap);
    va_end(ap);
    msg = (severity == LOG_WARNING) ? fmt : &fmt[10];

    if (!use_syslog) {
	if (severity > loglevel)
	    return;

	/* Only OK use-case for unsafe gettimeofday(), logging. */
	gettimeofday(&now, NULL);
	now_sec = now.tv_sec;
	thyme = localtime(&now_sec);
//	if (!debug)
	    fprintf(stderr, "%s: ", log_name);
	fprintf(stderr, "%02d:%02d:%02d.%03d %s", thyme->tm_hour,
		thyme->tm_min, thyme->tm_sec, (int)(now.tv_usec / 1000), msg);
	if (syserr == 0)
	    fprintf(stderr, "\n");
	else
	    fprintf(stderr, ": %s\n", strerror(syserr));

	goto end;
    }

    if (syserr != 0) {
	errno = syserr;
	syslog(severity, "%s: %s", msg, strerror(errno));
    } else
	syslog(severity, "%s", msg);

  end:
    if (severity <= LOG_ERR)
	exit(1);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "cc-mode"
 * End:
 */
