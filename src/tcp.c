/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2018, CESAR. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <hal/linux_log.h>

#include "node.h"

static int tcp_probe(void)
{

	return 0;
}

static void tcp_remove(void)
{

}

static int tcp_listen(void)
{
	int err, sock, enable = 1;
	struct sockaddr_in addr;

	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
		return -errno;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable,
						sizeof(enable)) == -1) {
		err = errno;
		hal_log_error("tcp setsockopt(SO_REUSEADDR): %s(%d)",
							strerror(err), err);
		close(sock);
		return -err;
	}

	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &enable,
						sizeof(enable)) == -1) {
		err = errno;
		hal_log_error("tcp setsockopt(TCP_NODELAY): %s(%d)",
							strerror(err), err);
		close(sock);
		return -err;
	}

	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(9994);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		err = -errno;
		close(sock);
		return err;
	}

	if (listen(sock, 1) == -1) {
		err = -errno;
		close(sock);
		return err;
	}

	return sock;
}

static int tcp_accept(int srv_sockfd)
{
	int sockfd;

	sockfd = accept(srv_sockfd, NULL, NULL);
	if (sockfd == -1)
		return -errno;

	return sockfd;
}

static ssize_t tcp_recv(int sockfd, void *buffer, size_t len)
{
	return recv(sockfd, buffer, len, 0);
}

static ssize_t tcp_send(int sockfd, const void *buffer, size_t len)
{
	return send(sockfd, buffer, len, 0);
}

struct node_ops tcp_ops = {
	.name = "TCP",
	.probe = tcp_probe,
	.remove = tcp_remove,

	.listen = tcp_listen,
	.accept = tcp_accept,
	.recv = tcp_recv,
	.send = tcp_send
};
