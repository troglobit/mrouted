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

static struct sockaddr_un sun;
static int ipc_socket = -1;
static int detail = 0;

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

static int ipc_close(int sd, struct ipc *msg)
{
	msg->cmd = IPC_EOF_CMD;
	if (ipc_write(sd, msg)) {
		logit(LOG_WARNING, errno, "Failed sending EOF/ACK to client");
		return -1;
	}

	return 0;
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

	return ipc_close(sd, msg);
}

static void show_dump(int sd, struct ipc *msg)
{
	FILE *fp;

	fp = tmpfile();
	if (!fp) {
		logit(LOG_WARNING, errno, "Failed opening temporary file");
		return;
	}

	dump_vifs(fp);
	dump_routes(fp);
	dump_cache(fp);
	rewind(fp);
	ipc_send(sd, msg, fp);
	fclose(fp);
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
	case IPC_SHOW_DUMP_CMD:
		show_dump(client, &msg);
		break;

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

	if (register_input_handler(sd, ipc_handle) < 0)
		logit(LOG_ERR, 0, "Failed registering IPC handler");

#ifdef HAVE_SOCKADDR_UN_SUN_LEN
	sun.sun_len = 0;	/* <- correct length is set by the OS */
#endif
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, _PATH_MROUTED_SOCK, sizeof(sun.sun_path));

	unlink(sun.sun_path);
	logit(LOG_DEBUG, 0, "Binding IPC socket to %s", sun.sun_path);

	len = offsetof(struct sockaddr_un, sun_path) + strlen(sun.sun_path);
	if (bind(sd, (struct sockaddr *)&sun, len) < 0 || listen(sd, 1)) {
		logit(LOG_WARNING, errno, "Failed binding IPC socket, client disabled");
		close(sd);
		return;
	}

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
