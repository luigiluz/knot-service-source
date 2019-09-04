/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2019, CESAR. All rights reserved.
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

#ifndef  _GNU_SOURCE
#define  _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>
#include <errno.h>
#include <ell/ell.h>
#include <hal/linux_log.h>
#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#include "settings.h"
#include "amqp.h"

#define AMQP_CONNECTION_TIMEOUT_US 10000

struct amqp_context {
	amqp_connection_state_t conn;
	struct l_io *amqp_io;
	amqp_read_cb_t read_cb;
};

static struct amqp_context amqp_ctx;

static const char *amqp_server_exception_string(amqp_rpc_reply_t reply)
{
	amqp_connection_close_t *m = reply.reply.decoded;
	static char r[512];

	switch (reply.reply.id) {
	case AMQP_CONNECTION_CLOSE_METHOD:
		snprintf(r, sizeof(r),
			 "server connection error %uh, message: %.*s\n",
			 m->reply_code, (int)m->reply_text.len,
			 (char *)m->reply_text.bytes);
		break;
	case AMQP_CHANNEL_CLOSE_METHOD:
		snprintf(r, sizeof(r),
			 "server channel error %uh, message: %.*s\n",
			 m->reply_code, (int)m->reply_text.len,
			 (char *)m->reply_text.bytes);
		break;
	default:
		snprintf(r, sizeof(r),
			 "unknown server error, method id 0x%08X\n",
			 reply.reply.id);
		break;
	}

	return l_strdup(r);
}

static const char *amqp_rpc_reply_string(amqp_rpc_reply_t reply)
{
	switch (reply.reply_type) {
	case AMQP_RESPONSE_NONE:
		return "missing RPC reply type!";
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		return amqp_error_string2(reply.library_error);
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		return amqp_server_exception_string(reply);
	case AMQP_RESPONSE_NORMAL:
	default:
		return "";
	}
}

static char *amqp_bytes_to_new_string(amqp_bytes_t data)
{
	char *str = l_new(char, data.len + 1);

	memcpy(str, data.bytes, data.len);
	str[data.len] = '\0';
	return str;
}

static bool on_receive(struct l_io *io, void *user_data)
{
	amqp_rpc_reply_t res;
	amqp_envelope_t envelope;
	char *exchange, *routing_key, *body;
	struct timeval time_out = { .tv_usec = AMQP_CONNECTION_TIMEOUT_US };
	bool err = true;

	if (amqp_release_buffers_ok(amqp_ctx.conn))
		amqp_release_buffers(amqp_ctx.conn);

	res = amqp_consume_message(amqp_ctx.conn, &envelope, &time_out, 0);

	if (AMQP_RESPONSE_NORMAL != res.reply_type)
		return true;

	hal_log_dbg("Receive %u, exchange %.*s routingkey %.*s\n",
			(unsigned)envelope.delivery_tag,
			(int)envelope.exchange.len,
			(char *)envelope.exchange.bytes,
			(int)envelope.routing_key.len,
			(char *)envelope.routing_key.bytes);

	hal_log_dbg("Body: %.*s\n",
			(int)envelope.message.body.len,
			(char *)envelope.message.body.bytes);

	if (!amqp_ctx.read_cb) {
		hal_log_dbg("AMQP read callback is not set");
		amqp_destroy_envelope(&envelope);
		return false;
	}

	exchange = amqp_bytes_to_new_string(envelope.exchange);
	routing_key = amqp_bytes_to_new_string(envelope.routing_key);
	body = amqp_bytes_to_new_string(envelope.message.body);

	err = amqp_ctx.read_cb(exchange, routing_key, body, user_data);

	/* FIXME: Put msg on the queue again */
	hal_log_dbg("Destroy received envelope");
	amqp_destroy_envelope(&envelope);
	l_free(exchange);
	l_free(routing_key);
	l_free(body);

	return err;
}

static int start_connection(struct settings *settings)
{
	amqp_socket_t *socket;
	struct amqp_connection_info cinfo;
	amqp_rpc_reply_t r;
	struct timeval timeout = { .tv_usec = AMQP_CONNECTION_TIMEOUT_US };
	int status;

	hal_log_dbg("Trying to connect to rabbitmq");
	status = amqp_parse_url((char *) settings->rabbitmq_url, &cinfo);
	if (status) {
		hal_log_error("amqp_parse_url: %s", amqp_error_string2(status));
		return -1;
	}

	amqp_ctx.conn = amqp_new_connection();

	socket = amqp_tcp_socket_new(amqp_ctx.conn);
	if (!socket) {
		amqp_destroy_connection(amqp_ctx.conn);
		hal_log_error("error creating tcp socket\n");
		return -1;
	}

	status = amqp_socket_open_noblock(socket, cinfo.host, cinfo.port,
					  &timeout);
	if (status) {
		amqp_destroy_connection(amqp_ctx.conn);
		hal_log_error("error opening socket\n");
		return -1;
	}

	r = amqp_login(amqp_ctx.conn, cinfo.vhost,
			AMQP_DEFAULT_MAX_CHANNELS, AMQP_DEFAULT_FRAME_SIZE,
			AMQP_DEFAULT_HEARTBEAT, AMQP_SASL_METHOD_PLAIN,
			cinfo.user, cinfo.password);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_destroy_connection(amqp_ctx.conn);
		hal_log_error("amqp_login(): %s", amqp_rpc_reply_string(r));
		return -1;
	}

	hal_log_info("Connected to amqp://%s:%s@%s:%d/%s\n", cinfo.user,
		     cinfo.password, cinfo.host, cinfo.port, cinfo.vhost);

	amqp_channel_open(amqp_ctx.conn, 1);
	r = amqp_get_rpc_reply(amqp_ctx.conn);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_connection_close(amqp_ctx.conn, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(amqp_ctx.conn);
		hal_log_error("amqp_channel_open(): %s",
				amqp_rpc_reply_string(r));
		return -1;
	}

	return 0;
}

int8_t amqp_publish_persistent_message(const char *exchange,
				       const char *routing_keys,
				       const char *body)
{
	amqp_basic_properties_t props;
	amqp_rpc_reply_t resp;
	int8_t rc; // Return Code

	/* Declare the exchange as durable */
	amqp_exchange_declare(amqp_ctx.conn, 1,
			amqp_cstring_bytes(exchange),
			amqp_cstring_bytes("topic"),
			0 /* passive*/,
			1 /* durable */,
			0 /* auto_delete*/,
			0 /* internal */,
			amqp_empty_table);
	resp = amqp_get_rpc_reply(amqp_ctx.conn);
	if (resp.reply_type != AMQP_RESPONSE_NORMAL) {
		hal_log_error("amqp_exchange_declare(): %s",
				amqp_rpc_reply_string(resp));
		return -1;
	}

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG |
			AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");
	props.delivery_mode = AMQP_DELIVERY_PERSISTENT;
	rc = amqp_basic_publish(amqp_ctx.conn, 1,
			amqp_cstring_bytes(exchange),
			amqp_cstring_bytes(routing_keys),
			0 /* mandatory */,
			0 /* immediate */,
			&props, amqp_cstring_bytes(body));
	if (rc < 0)
		hal_log_error("amqp_basic_publish(): %s",
				amqp_error_string2(rc));
	return rc;
}

amqp_bytes_t amqp_declare_new_queue(const char *name)
{
	amqp_bytes_t queue;
	amqp_queue_declare_ok_t *r;

	r = amqp_queue_declare(amqp_ctx.conn, 1,
			amqp_cstring_bytes(name),
			0, /* passive */
			1, /* durable */
			0, /* exclusive */
			0, /* auto-delete */
			amqp_empty_table);

	if (amqp_get_rpc_reply(amqp_ctx.conn).reply_type !=
			       AMQP_RESPONSE_NORMAL) {
		hal_log_error("Error declaring queue name");
		queue.bytes = NULL;
	}

	queue = amqp_bytes_malloc_dup(r->queue);
	if (queue.bytes == NULL)
		hal_log_error("Out of memory while copying queue buffer");

	return queue;
}

int amqp_set_queue_to_consume(amqp_bytes_t queue,
			      const char *exchange,
			      const char *routing_key)
{
	if (exchange == NULL || routing_key == NULL)
		return -1;

	/* Declare the exchange as durable */
	amqp_exchange_declare(amqp_ctx.conn, 1,
			amqp_cstring_bytes(exchange),
			amqp_cstring_bytes("topic"),
			0 /* passive*/,
			1 /* durable */,
			0 /* auto_delete*/,
			0 /* internal */,
			amqp_empty_table);

	/* Set up to bind a queue to an exchange */
	amqp_queue_bind(amqp_ctx.conn, 1, queue,
			amqp_cstring_bytes(exchange),
			amqp_cstring_bytes(routing_key),
			amqp_empty_table);

	if (amqp_get_rpc_reply(amqp_ctx.conn).reply_type !=
			       AMQP_RESPONSE_NORMAL) {
		hal_log_error("Error while binding queue");
		return -1;
	}

	/* Start a queue consumer */
	amqp_basic_consume(amqp_ctx.conn, 1,
			queue,
			amqp_empty_bytes,
			0, /* no_local */
			1, /* no_ack */
			0, /* exclusive */
			amqp_empty_table);

	if (amqp_get_rpc_reply(amqp_ctx.conn).reply_type !=
			       AMQP_RESPONSE_NORMAL) {
		hal_log_error("Error while starting consumer");
		return -1;
	}

	return 0;
}

int amqp_set_read_cb(amqp_read_cb_t on_read, void *user_data)
{
	int err;

	amqp_ctx.read_cb = on_read;

	amqp_ctx.amqp_io = l_io_new(amqp_get_sockfd(amqp_ctx.conn));

	err = l_io_set_read_handler(amqp_ctx.amqp_io, on_receive,
				    &amqp_ctx.conn, user_data);
	if (!err) {
		l_io_destroy(amqp_ctx.amqp_io);
		hal_log_error("Error on set up read handler on AMQP io\n");
		return -1;
	}

	return 0;
}

int amqp_start(struct settings *settings)
{
	int err;

	err = start_connection(settings);
	if (err) {
		hal_log_error("Error on start connection\n");
		return -1;
	}

	return 0;
}

void amqp_stop(void)
{
	amqp_rpc_reply_t r;
	int err;

	if (amqp_ctx.amqp_io)
		l_io_destroy(amqp_ctx.amqp_io);

	if (!amqp_ctx.conn)
		return;

	r = amqp_channel_close(amqp_ctx.conn, 1, AMQP_REPLY_SUCCESS);
	if (r.reply_type != AMQP_RESPONSE_NORMAL)
		hal_log_error("amqp_channel_close: %s",
				amqp_rpc_reply_string(r));

	r = amqp_connection_close(amqp_ctx.conn, AMQP_REPLY_SUCCESS);
	if (r.reply_type != AMQP_RESPONSE_NORMAL)
		hal_log_error("amqp_connection_close: %s",
				amqp_rpc_reply_string(r));

	err = amqp_destroy_connection(amqp_ctx.conn);
	if (err < 0)
		hal_log_error("amqp_destroy_connection: %s",
				amqp_error_string2(err));
}