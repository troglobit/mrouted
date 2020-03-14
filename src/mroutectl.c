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

#include "defs.h"
#include <getopt.h>
#include <poll.h>
#ifdef HAVE_TERMIOS_H
# include <termios.h>
#endif

struct cmd {
	char        *cmd;
	struct cmd  *ctx;
	int        (*cb)(char *arg);
	int         op;
};

static int plain = 0;
static int detail = 0;
static int heading = 1;


static int do_connect(void)
{
	struct sockaddr_un sun;
	int sd;

	sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (-1 == sd)
		goto error;

#ifdef HAVE_SOCKADDR_UN_SUN_LEN
	sun.sun_len = 0;	/* <- correct length is set by the OS */
#endif
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, _PATH_MROUTED_SOCK, sizeof(sun.sun_path));
	if (connect(sd, (struct sockaddr*)&sun, sizeof(sun)) == -1) {
		close(sd);
		goto error;
	}

	return sd;
error:
	if (errno == ENOENT)
		fprintf(stderr, "Cannot connect to mrouted, verify it has started.\n");
	else
		perror("Failed connecting to mrouted");

	return -1;
}

#define ESC "\033"
static int get_width(void)
{
	int ret = 74;
#ifdef HAVE_TERMIOS_H
	char buf[42];
	struct termios tc, saved;
	struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };

	memset(buf, 0, sizeof(buf));
	tcgetattr(STDERR_FILENO, &tc);
	saved = tc;
	tc.c_cflag |= (CLOCAL | CREAD);
	tc.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	tcsetattr(STDERR_FILENO, TCSANOW, &tc);
	fprintf(stderr, ESC "7" ESC "[r" ESC "[999;999H" ESC "[6n");

	if (poll(&fd, 1, 300) > 0) {
		int row, col;

		if (scanf(ESC "[%d;%dR", &row, &col) == 2)
			ret = col;
	}

	fprintf(stderr, ESC "8");
	tcsetattr(STDERR_FILENO, TCSANOW, &saved);
#endif
	return ret;
}

static char *chomp(char *str)
{
	char *p;

	if (!str || strlen(str) < 1) {
		errno = EINVAL;
		return NULL;
	}

	p = str + strlen(str) - 1;
        while (*p == '\n')
		*p-- = 0;

	return str;
}

static void print(char *line)
{
	int len, head = 0, title = 0;

	chomp(line);

	/* Table headings, or repeat headers, end with a '=' */
	len = (int)strlen(line) - 1;
	if (len > 0) {
		if (line[len] == '=')
			head = 1;
		if (line[len] == '_' )
			title = 1;

		if (head || title) {
			if (!heading)
				return;

			line[len] = 0;
			if (!plain) {
				if (head)
					len = get_width() - len;
				else
					len = get_width();
			}
		}
	}

	if (!head && !title) {
		puts(line);
		return;
	}

	if (!plain) {
		if (head)
			fprintf(stdout, "\e[7m%s%*s\e[0m\n", line, len < 0 ? 0 : len, "");
		else
			fprintf(stdout, "\e[4m%*s\e[0m\n\e[1m%s\e[0m\n", len < 0 ? 0 : len, "", line);
	} else {
		len = get_width();

		if (title) {
			fprintf(stdout, "\n%s ", line);
			len -= strlen(line) + 1;
		} else
			fprintf(stdout, "%s\n", line);

		while (len--)
			fputc('=', stdout);
		fputs("\n", stdout);
	}
}

static struct ipc *do_cmd(int cmd, int detail, char *buf, size_t len)
{
	static struct ipc msg;
	struct pollfd pfd;
	int sd;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = cmd;
	msg.detail = detail;
	if (buf) {
		if (len >= sizeof(msg.buf)) {
			errno = EINVAL;
			return NULL;
		}
		memcpy(msg.buf, buf, len);
	}

	sd = do_connect();
	if (-1 == sd)
		return NULL;

	if (write(sd, &msg, sizeof(msg)) == -1) {
		close(sd);
		return NULL;
	}

	pfd.fd = sd;
	pfd.events = POLLIN;
	while (poll(&pfd, 1, 2000) > 0) {
		ssize_t len;

		len = read(sd, &msg, sizeof(msg));
		if (len != sizeof(msg) || msg.cmd)
			break;

		msg.sentry = 0;
		print(msg.buf);
	}

	close(sd);

	return &msg;
}

static int do_set(int cmd, char *arg)
{
	struct ipc *msg;

	if (!arg)
		arg = "";

	msg = do_cmd(cmd, 0, arg, strlen(arg));
	if (!msg)
		return 1;

	if (strlen(msg->buf) > 1)
		puts(msg->buf);

	return 0;
}

static int set_debug(char *arg)
{
	int rc;

	if (!strcmp(arg, "?")) {
		debug_print();
		return 0;
	}

	rc = debug_parse(arg);
	if ((int)DEBUG_PARSE_ERR == rc)
		return 1;

	return do_set(IPC_DEBUG_CMD, arg);
}

static int set_loglevel(char *arg)
{
	return do_set(IPC_LOGLEVEL_CMD, arg);
}

static int show_generic(int cmd, int detail)
{
	if (!do_cmd(cmd, detail, NULL, 0))
		return -1;

	return 0;
}

/* mroutectl show igmp => show igmp ifaces + show igmp groups */
static int show_igmp(char *arg)
{
	return show_generic(IPC_SHOW_IGMP_CMD, detail);
}

static int show_status(char *arg)
{
	return show_generic(IPC_SHOW_STATUS_CMD, detail);
}

static int usage(int rc)
{
	printf("Usage: mroutectl [OPTIONS] [COMMAND]\n"
	       "\n"
	       "Options:\n"
	       "  -d, --detail            Detailed output, where applicable\n"
	       "  -p, --plain             Use plain table headings, no ctrl chars\n"
	       "  -h, --help              This help text\n"
	       "  -t, --no-heading        Skip table headings\n"
	       "  -v, --version           Show mrouted version mroutectl is built against\n"
	       "\n"
	       "Commands:\n"
	       "  debug [? | none | SYS]  Debug subystem(s), separate multiple with comma\n"
	       "  help                    This help text\n"
	       "  kill                    Kill running mrouted, like SIGTERM\n"
	       "  log [? | none | LEVEL]  Set log level: none, err, notice*, info, debug\n"
	       "  restart                 Restart mrouted and reload .conf file, like SIGHUP\n"
	       "  show version            Show version, and uptime (-d), of running mrouted\n"
	       "  show status             Show status summary, default\n"
	       "  show compat             Show status, compat mode, previously `mrouted -r`\n"
	       "  show igmp groups        Show IGMP group memberships\n"
	       "  show igmp interface     Show IGMP interface status\n"
	       "  show interfaces         Show interface table\n"
	       "  show mfc                Show multicast forwarding cache\n"
	       "  show neighbor           Show information about DVMRP neighbors\n"
	       "  show routes             Show DVMRP routing table\n");

	fputs("\nValid debug subsystems:\n", stderr);
	debug_print();

	printf("\nBug report address: %-40s\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
	printf("Project homepage: %s\n", PACKAGE_URL);
#endif

	return rc;
}

static int help(char *arg)
{
	(void)arg;
	return usage(0);
}

static int string_match(const char *a, const char *b)
{
   size_t min = MIN(strlen(a), strlen(b));

   return !strncasecmp(a, b, min);
}

static int cmd_parse(int argc, char *argv[], struct cmd *command)
{
	int i;

	for (i = 0; argc > 0 && command[i].cmd; i++) {
		if (!string_match(command[i].cmd, argv[0]))
			continue;

		/* If more args, then pass parsing to sub-context */
		if (command[i].ctx && argc > 1)
			return cmd_parse(argc - 1, &argv[1], command[i].ctx);

		/* If out of args, despite sub-context, then check for local cb */
		if (command[i].cb) {
			char arg[80] = "";
			int j;

			for (j = 1; j < argc; j++) {
				if (j > 1)
					strlcat(arg, " ", sizeof(arg));
				strlcat(arg, argv[j], sizeof(arg));
			}

			return command[i].cb(arg);
		}

		return show_generic(command[i].op, detail);
	}

	return usage(1);
}

int main(int argc, char *argv[])
{
	struct option long_options[] = {
		{ "detail",     0, NULL, 'd' },
		{ "plain",      0, NULL, 'p' },
		{ "no-heading", 0, NULL, 't' },
		{ "help",       0, NULL, 'h' },
		{ "version",    0, NULL, 'v' },
		{ 0 }
	};
	struct cmd igmp[] = {
		{ "groups",     NULL, NULL,         IPC_SHOW_IGMP_GROUP_CMD },
		{ "interfaces", NULL, NULL,         IPC_SHOW_IGMP_IFACE_CMD },
		{ "ifaces",     NULL, NULL,         IPC_SHOW_IGMP_IFACE_CMD }, /* alias */
		{ 0 }
	};
	struct cmd show[] = {
		{ "compat",     NULL, NULL,         IPC_SHOW_COMPAT_CMD     },
		{ "routes",     NULL, NULL,         IPC_SHOW_ROUTES_CMD     },
		{ "igmp",       igmp, show_igmp,    0                       },
		{ "interfaces", NULL, NULL,         IPC_SHOW_IFACE_CMD      },
		{ "ifaces",     NULL, NULL,         IPC_SHOW_IFACE_CMD      }, /* alias */
		{ "mfc",        NULL, NULL,         IPC_SHOW_MFC_CMD        },
		{ "neighbor",   NULL, NULL,         IPC_SHOW_NEIGH_CMD      },
		{ "status",     NULL, NULL,         IPC_SHOW_STATUS_CMD     },
		{ "version",    NULL, NULL,         IPC_VERSION_CMD         },
		{ 0 }
	};
	struct cmd command[] = {
		{ "debug",      NULL, set_debug,    0                       },
		{ "help",       NULL, help,         0                       },
		{ "kill",       NULL, NULL,         IPC_KILL_CMD            },
		{ "log",        NULL, set_loglevel, 0                       },
		{ "restart",    NULL, NULL,         IPC_RESTART_CMD         },
		{ "show",       show, show_status,  0                       },
		{ 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "dh?ptv", long_options, NULL)) != EOF) {
		switch(c) {
		case 'd':
			detail = 1;
			break;

		case 'h':
		case '?':
			return usage(0);

		case 'p':
			plain = 1;
			break;

		case 't':
			heading = 0;
			break;

		case 'v':
			printf("%s\n", versionstring);
			return 0;
		}
	}

	if (optind >= argc)
		return show_status(NULL);

	return cmd_parse(argc - optind, &argv[optind], command);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
