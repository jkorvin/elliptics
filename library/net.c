/*
 * Copyright 2008+ Evgeniy Polyakov <zbr@ioremap.net>
 *
 * This file is part of Elliptics.
 *
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <netinet/tcp.h>

#include "elliptics.h"
#include "elliptics/packet.h"
#include "elliptics/interface.h"

#include "monitor/measure_points.h"
#include "library/logger.hpp"
#include "tests.h"
#include "access_context.h"

#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif


int dnet_fill_addr(struct dnet_addr *addr, const char *saddr, const int port, const int sock_type, const int proto)
{
	struct addrinfo *ai = NULL, hint;
	int err;
	char port_str[16];

	snprintf(port_str, sizeof(port_str), "%d", port);

	memset(&hint, 0, sizeof(struct addrinfo));

	hint.ai_family = addr->family;
	hint.ai_socktype = sock_type;
	hint.ai_protocol = proto;

	err = getaddrinfo(saddr, port_str, &hint, &ai);
	if (err || ai == NULL) {
		if (!err)
			err = -ENXIO;
		if (ai)
			goto err_out_free;

		goto err_out_exit;
	}

	if (addr->addr_len < ai->ai_addrlen) {
		err = -ENOBUFS;
		goto err_out_free;
	}

	addr->addr_len = ai->ai_addrlen;
	memcpy(addr->addr, ai->ai_addr, addr->addr_len);

err_out_free:
	freeaddrinfo(ai);
err_out_exit:
	return err;
}

/**
 * This function resolves DNS name or IP address and put end result into sockaddr structure
 * as well as fills dnet_add structure.
 */
int dnet_create_addr(struct dnet_addr *addr, const char *addr_str, int port, int family)
{
	memset(addr, 0, sizeof(struct dnet_addr));

	addr->addr_len = sizeof(addr->addr);
	addr->family = family;

	if (addr_str) {
		return dnet_fill_addr(addr, addr_str, port, SOCK_STREAM, IPPROTO_TCP);
	} else {
		if (addr->family != AF_INET6) {
			struct sockaddr_in *in = (struct sockaddr_in *)(addr->addr);
			in->sin_port = htons(port);
		} else {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)(addr->addr);
			in6->sin6_port = htons(port);
		}
	}

	return 0;
}

int dnet_create_addr_str(struct dnet_addr *addr, const char *addr_str, int addr_len)
{
	int port;
	int family;
	int err;
	char *tmp;

	// space for NULL-byte
	tmp = malloc(addr_len + 1);
	if (!tmp) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	snprintf(tmp, addr_len+1, "%s", addr_str);

	err = dnet_parse_addr(tmp, &port, &family);
	if (err) {
		goto err_out_free;
	}

	err = dnet_create_addr(addr, tmp, port, family);
	if (err) {
		goto err_out_free;
	}

	err = 0;

err_out_free:
	free(tmp);
err_out_exit:
	return err;
}

void dnet_state_clean(struct dnet_net_state *st)
{
	struct rb_node *rb_node;
	struct dnet_trans *t;
	int num = 0;

	while (1) {
		t = NULL;

		pthread_mutex_lock(&st->trans_lock);
		rb_node = rb_first(&st->trans_root);
		if (rb_node) {
			t = rb_entry(rb_node, struct dnet_trans, trans_entry);
			dnet_trans_get(t);

			dnet_trans_remove_nolock(st, t);
			list_del_init(&t->trans_list_entry);
		}
		pthread_mutex_unlock(&st->trans_lock);

		if (!t)
			break;

		dnet_trans_put(t);
		dnet_trans_put(t);
		num++;
	}

	dnet_log(st->n, DNET_LOG_NOTICE, "Cleaned state %s, transactions freed: %d", dnet_state_dump_addr(st), num);
}

/*
 * This function makes a copy of io_req to transfer memory ownership to another thread.
 * If target thread is net thread we can just make a copy and properly fill fd params.
 * If target thread is io thread (bypass for local transaction on server side, bypass flag
 * is set to 1) we need to allocate buffer and read fd content info this buffer.
 * Result should looks exactly as if was read from network socket. This CPU IO time is spent
 * in backend's IO pool.
 */
static struct dnet_io_req *dnet_io_req_copy(struct dnet_net_state *st, struct dnet_io_req *orig, int bypass)
{
	void *buf;
	size_t len = 0;
	struct dnet_io_req *r;
	int offset = 0;
	int err = 0;

	len = sizeof(struct dnet_io_req) + orig->dsize + orig->hsize;
	if (orig->fd >= 0 && orig->fsize && bypass) {
		len += orig->fsize;
	}

	buf = r = malloc(len);
	if (!r) {
		dnet_log(st->n, DNET_LOG_ERROR, "Not enough memory for io req queue fd: %d : %s %d", orig->fd, strerror(-err), err);
		return NULL;
	}
	memset(r, 0, sizeof(struct dnet_io_req));
	r->fd = -1;
	r->context = dnet_access_context_get(orig->context);

	if (orig->header && orig->hsize) {
		r->header = buf + sizeof(struct dnet_io_req);
		r->hsize = orig->hsize;

		offset = r->hsize;
		memcpy(r->header, orig->header, r->hsize);
	} else if (bypass) {
		r->header = buf + sizeof(struct dnet_io_req);
		r->hsize = 0;
	}

	if (orig->data && orig->dsize) {
		r->data = buf + sizeof(struct dnet_io_req) + offset;
		r->dsize = orig->dsize;

		offset += r->dsize;
		memcpy(r->data, orig->data, r->dsize);
	}

	if (orig->fd >= 0 && orig->fsize) {
		if (bypass) {
			if (r->data == NULL) {
				r->data = buf + sizeof(struct dnet_io_req) + offset;
			}

			err = dnet_read_ll(orig->fd, r->data + r->dsize, orig->fsize, orig->local_offset);
			if (err) {
				dnet_log(st->n, DNET_LOG_ERROR, "Error while reading data: %s %d", strerror(-err), err);
				goto err_out_free;
			}

			r->dsize += orig->fsize;
			r->on_exit = orig->on_exit;

		} else {
			r->fd = orig->fd;
			r->on_exit = orig->on_exit;
			r->local_offset = orig->local_offset;
			r->fsize = orig->fsize;
		}
	}

	if (bypass) {
		/*
		 * Fixup request.
		 * r->header and r->data are just a pointers to the same buffer.
		 * Here r->header could be any size, it depend on function that created io_req.
		 * In net thread r->hsize is ALWAYS sizeof(struct dnet_cmd) and r->data points
		 * exactly after struct dnet_cmd struct. We need to modify our copy to the same layout
		 * because dnet_schedule_io requires it.
		 */
		if (r->hsize != sizeof(struct dnet_cmd)) {
			r->data = r->header + sizeof(struct dnet_cmd);
			r->dsize += r->hsize - sizeof(struct dnet_cmd);
			r->hsize = sizeof(struct dnet_cmd);
		}
	}

	return r;

err_out_free:
	free(r);
	return NULL;
}

/*
 * Eventually we may end up with proper reference counters here, but for now let's just copy the whole buf.
 * Large data blocks are being sent through sendfile anyway, so it should not be _that_ costly operation.
 */
static int dnet_io_req_queue(struct dnet_net_state *st, struct dnet_io_req *orig)
{
	int err = 0;
	struct dnet_io_req *r;

	/*
	 * If destination is local node there is no need to copy it via net thread.
	 * Here it could be scheduled to the proper IO pool directly.
	 * If io_req hold fd instead of data it should be read here.
	 */
	if (st == st->n->st) {
		dnet_log(st->n, DNET_LOG_DEBUG, "Sending to local state: data: 0x%lx data_size: %d",
		         (unsigned long)orig->data, (int)orig->dsize);

		r = dnet_io_req_copy(st, orig, 1);
		if (!r) {
			err = -ENOMEM;
			goto err_out_exit;
		}

		clock_gettime(CLOCK_MONOTONIC_RAW, &st->rcv_start_ts);
		st->rcv_finish_ts = st->rcv_start_ts;

		r->st = dnet_state_get(st);
		dnet_schedule_io(st->n, r);

		return 0;
	}

	r = dnet_io_req_copy(st, orig, 0);
	if (!r) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &r->queue_start_ts);

	pthread_mutex_lock(&st->send_lock);
	list_add_tail(&r->req_entry, &st->send_list);

	if (!st->__need_exit)
		dnet_schedule_send(st);
	pthread_mutex_unlock(&st->send_lock);

	pthread_mutex_lock(&st->n->io->full_lock);
	list_stat_size_increase(&st->n->io->output_stats, 1);
	pthread_mutex_unlock(&st->n->io->full_lock);
	HANDY_COUNTER_INCREMENT("io.output.queue.size", 1);

err_out_exit:
	return err;
}

void dnet_io_req_free(struct dnet_io_req *r)
{
	if (r->fd >= 0 && r->fsize) {
		if (r->on_exit & DNET_IO_REQ_FLAGS_CACHE_FORGET)
			posix_fadvise(r->fd, r->local_offset, r->fsize, POSIX_FADV_DONTNEED);
		if (r->on_exit & DNET_IO_REQ_FLAGS_CLOSE)
			close(r->fd);
	}
	dnet_access_access_put(r->context);
	free(r);
}

ssize_t dnet_send_nolock(struct dnet_net_state *st, void *data, uint64_t size)
{
	ssize_t err = 0;
	struct dnet_node *n = st->n;

	while (size) {
		err = send(st->write_s, data, size, 0);
		if (err < 0) {
			err = -errno;
			if (err != -EAGAIN) {
				DNET_ERROR(n, "Failed to send packet: size: %" PRIu64 ", socket: %d", size,
				               st->write_s);
			}
			break;
		}

		if (err == 0) {
			dnet_log(n, DNET_LOG_ERROR, "Peer %s has dropped the connection: socket: %d.", dnet_state_dump_addr(st), st->write_s);
			err = -ECONNRESET;
			break;
		}

		data += err;
		size -= err;
		st->send_offset += err;

		err = 0;
	}

	return err;
}

ssize_t dnet_send(struct dnet_net_state *st, void *data, uint64_t size, struct dnet_access_context *context)
{
	struct dnet_io_req r;

	memset(&r, 0, sizeof(r));
	r.header = data;
	r.hsize = sizeof(struct dnet_cmd);
	if (size > r.hsize) {
		r.data = data + r.hsize;
		r.dsize = size - r.hsize;
	}
	r.fd = -1;
	r.context = context;

	return dnet_io_req_queue(st, &r);
}

ssize_t dnet_send_data(struct dnet_net_state *st,
                       void *header,
                       uint64_t hsize,
                       void *data,
                       uint64_t dsize,
                       struct dnet_access_context *context) {
	struct dnet_io_req r;

	memset(&r, 0, sizeof(r));
	r.header = header;
	r.hsize = hsize;
	r.data = data;
	r.dsize = dsize;
	r.fd = -1;
	r.context = context;

	return dnet_io_req_queue(st, &r);
}

static ssize_t dnet_send_fd_nolock(struct dnet_net_state *st, int fd, uint64_t offset, uint64_t dsize)
{
	ssize_t err = 0;

	while (dsize) {
		err = dnet_sendfile(st, fd, &offset, dsize);
		if (err < 0)
			break;
		if (err == 0) {
			err = -ENODATA;
			DNET_ERROR(st->n, "Looks like truncated file: fd: %d, offset: %" PRIu64 ", size: %" PRIu64 "",
			           fd, offset, dsize);
			break;
		}

		dsize -= err;
		st->send_offset += err;
		err = 0;
	}

	return err;
}

ssize_t dnet_send_fd(struct dnet_net_state *st, void *header, uint64_t hsize,
		int fd, uint64_t offset, uint64_t fsize, int on_exit, struct dnet_access_context *context)
{
	struct dnet_io_req r;

	memset(&r, 0, sizeof(r));
	r.header = header;
	r.hsize = hsize;
	r.fd = fd;
	r.on_exit = on_exit;
	r.local_offset = offset;
	r.fsize = fsize;
	r.context = context;

	return dnet_io_req_queue(st, &r);
}

void dnet_trans_update_timestamp(struct dnet_trans *t)
{
	clock_gettime(CLOCK_MONOTONIC_RAW, &t->time_ts);

	t->time_ts.tv_sec += t->wait_ts.tv_sec;
	t->time_ts.tv_nsec += t->wait_ts.tv_nsec;
}

int dnet_trans_send(struct dnet_trans *t, struct dnet_io_req *req)
{
	struct dnet_net_state *st = req->st;
	struct dnet_test_settings test_settings;
	int err;

	dnet_trans_get(t);

	pthread_mutex_lock(&st->trans_lock);
	err = dnet_trans_insert_nolock(st, t);
	if (!err) {
		dnet_trans_update_timestamp(t);
		dnet_trans_insert_timer_nolock(st, t);
	}
	pthread_mutex_unlock(&st->trans_lock);
	if (err)
		goto err_out_put;

	if (t->n->test_settings && !dnet_node_get_test_settings(t->n, &test_settings) &&
	    test_settings.commands_mask & (1 << t->command))
		goto err_out_put;

	err = dnet_io_req_queue(st, req);
	if (err)
		goto err_out_remove;

	dnet_trans_put(t);
	return 0;

err_out_remove:
	dnet_trans_remove(t);
err_out_put:
	dnet_trans_put(t);
	return err;
}

int dnet_add_reconnect_state(struct dnet_node *n, const struct dnet_addr *addr, unsigned int join_state)
{
	struct dnet_addr_storage *a, *it;
	int err = 0;

	if (!join_state || n->need_exit) {
		if (!join_state)
			dnet_log(n, DNET_LOG_INFO, "Do not add reconnection addr: %s, join state: 0x%x.",
				dnet_addr_string(addr), join_state);
		goto out_exit;
	}

	a = malloc(sizeof(struct dnet_addr_storage));
	if (!a) {
		err = -ENOMEM;
		goto out_exit;
	}
	memset(a, 0, sizeof(struct dnet_addr_storage));

	memcpy(&a->addr, addr, sizeof(struct dnet_addr));
	a->__join_state = join_state;

	pthread_mutex_lock(&n->reconnect_lock);
	list_for_each_entry(it, &n->reconnect_list, reconnect_entry) {
		if (!memcmp(&it->addr, &a->addr, sizeof(struct dnet_addr))) {
			dnet_log(n, DNET_LOG_INFO, "Address already exists in reconnection array: addr: %s, join state: 0x%x.",
				dnet_addr_string(&a->addr), join_state);
			err = -EEXIST;
			break;
		}
	}

	if (!err) {
		dnet_log(n, DNET_LOG_INFO, "Added reconnection addr: %s, join state: 0x%x.",
			dnet_addr_string(&a->addr), join_state);
		list_add_tail(&a->reconnect_entry, &n->reconnect_list);
		n->reconnect_num++;
	}
	pthread_mutex_unlock(&n->reconnect_lock);

	if (err)
		free(a);

out_exit:
	return err;
}

static int dnet_process_update_ids(struct dnet_net_state *st, struct dnet_cmd *cmd, struct dnet_id_container *id_container)
{
	struct dnet_backend_ids **backends;
	int i, err = 0;

	if (cmd->size < sizeof(struct dnet_id_container)) {
		err = -EINVAL;
		dnet_log(st->n, DNET_LOG_ERROR, "failed to validate route-list container from state: %s, it's too small, err: %d",
			dnet_state_dump_addr(st), err);
		goto err_out_exit;
	}

	err = dnet_validate_id_container(id_container, cmd->size);
	if (err) {
		dnet_log(st->n, DNET_LOG_ERROR, "failed to validate route-list container from state: %s, err: %d",
			dnet_state_dump_addr(st), err);
		goto err_out_exit;
	}

	backends = malloc(id_container->backends_count * sizeof(struct dnet_backends_id *));
	if (!backends) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	dnet_id_container_fill_backends(id_container, backends);

	for (i = 0; i < id_container->backends_count; ++i) {
		err = dnet_idc_update_backend(st, backends[i]);
		if (err) {
			dnet_log(st->n, DNET_LOG_ERROR, "Failed to update route-list: state: %s, backend: %d, err: %d",
				dnet_state_dump_addr(st),
				backends[i]->backend_id,
				err);
		} else {
			dnet_log(st->n, DNET_LOG_NOTICE, "Successfully updated route-list: state: %s, backend: %d",
				dnet_state_dump_addr(st),
				backends[i]->backend_id);
		}
	}

	free(backends);
err_out_exit:
	return err;
}

static int dnet_process_control(struct dnet_net_state *st, struct dnet_cmd *cmd, void *data)
{
	switch (cmd->cmd) {
	case DNET_CMD_UPDATE_IDS:
		return dnet_process_update_ids(st, cmd, data);
	default:
		return -ENOTSUP;
	}
}

static int dnet_process_reply(struct dnet_net_state *st, struct dnet_io_req *r) {
	int err = 0;
	struct dnet_node *n = st->n;
	struct dnet_trans *t = NULL;
	struct dnet_cmd *cmd = r->header;
	uint64_t tid = cmd->trans;
	uint64_t flags = cmd->flags;

	HANDY_COUNTER_INCREMENT("io.replies", 1);

	pthread_mutex_lock(&st->trans_lock);
	t = dnet_trans_search(st, tid);
	if (t) {
		if (!(flags & DNET_FLAGS_MORE)) {
			dnet_trans_remove_nolock(st, t);
		}

		/*
		 * Remove transaction for the duration of callback processing,
		 * otherwise timeout checking thread can catch up.
		 *
		 * Network thread also removes transaction from the tree, but network
		 * thread can read multiple replies and put multiple packets into the IO queue,
		 * which if processed here. Since code below inserts transaction into the timer tree
		 * again after its callback has been completed, someone has to remove it.
		 *
		 * It is safe to remove transaction multiple times, but subsequent insertion will lead to crash,
		 * if timestamp has been updated, since it is used as a key in the timer tree.
		 */
		dnet_trans_remove_timer_nolock(st, t);
	}
	pthread_mutex_unlock(&st->trans_lock);

	if (!t) {
		dnet_log(n, DNET_LOG_ERROR, "%s: could not find transaction for reply: trans %llu",
		         dnet_dump_id(&cmd->id), (unsigned long long)tid);
		err = 0;
		goto out;
	}

	++t->stats.recv_replies;
	t->stats.recv_size += r->hsize + r->dsize + r->fsize;
	t->stats.recv_queue_time += r->queue_time;
	t->stats.recv_time += r->recv_time;

	if (t->complete) {
		if (t->command == DNET_CMD_READ || t->command == DNET_CMD_READ_NEW) {
			uint64_t ioflags = 0;
			if ((t->command == DNET_CMD_READ) &&
			    (cmd->size >= sizeof(struct dnet_io_attr)) &&
			    (t->alloc_size >= sizeof(struct dnet_cmd) + sizeof(struct dnet_io_attr))) {
				struct dnet_io_attr *recv_io = (struct dnet_io_attr *)(cmd + 1);

				struct dnet_cmd *local_cmd = (struct dnet_cmd *)(t + 1);
				struct dnet_io_attr *local_io = (struct dnet_io_attr *)(local_cmd + 1);

				ioflags = local_io->flags = recv_io->flags;
				local_io->size = recv_io->size;
				local_io->offset = recv_io->offset;
				local_io->user_flags = recv_io->user_flags;
				local_io->total_size = recv_io->total_size;
				local_io->timestamp = recv_io->timestamp;

				dnet_convert_io_attr(local_io);
			}

			if (st && !(flags & DNET_FLAGS_MORE)) {
				struct timespec ts;
				long diff;

				clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
				diff = DIFF_TIMESPEC(t->start_ts, ts);

				dnet_update_backend_weight(st, cmd, ioflags, diff);
			}
		}
		t->complete(dnet_state_addr(t->st), cmd, t->priv);
	}

	dnet_trans_put(t);
	if (!(flags & DNET_FLAGS_MORE)) {
		memcpy(&t->cmd, cmd, sizeof(struct dnet_cmd));
		dnet_trans_put(t);
	} else {
		/*
		 * Put transaction back into the end of 'timer' tree with updated timestamp.
		 * Transaction had been removed from timer tree in @dnet_update_trans_timestamp_network() in network
		 * thread right after whole data was read.
		 */

		pthread_mutex_lock(&st->trans_lock);
		dnet_trans_update_timestamp(t);
		dnet_trans_insert_timer_nolock(st, t);
		pthread_mutex_unlock(&st->trans_lock);
	}

out:
	return err;
}

int dnet_process_recv(struct dnet_net_state *st, struct dnet_io_req *r) {
	int err = 0;
	struct dnet_node *n = st->n;
	struct dnet_net_state *forward_state = NULL;
	struct dnet_cmd *cmd = r->header;
	struct dnet_access_context *context = NULL;

	if (cmd->flags & DNET_FLAGS_REPLY) {
		err = dnet_process_reply(st, r);
		goto out;
	}

	// context is created only for requests, all replies should be handled within their request's context
	context = dnet_access_context_create(n);
	dnet_access_context_add_string(context, "cmd", dnet_cmd_string(cmd->cmd));
	dnet_access_context_add_uint(context, "trans", cmd->trans);
	dnet_access_context_add_string(context, "st", dnet_state_dump_addr(st));
	dnet_access_context_add_trace_id(context, cmd->trace_id);
	dnet_access_context_add_uint(context, "request_size", r->hsize + r->dsize + r->fsize);
	dnet_access_context_add_uint(context, "receive_time", r->recv_time);
	dnet_access_context_add_uint(context, "receive_queue_time", r->queue_time);

	err = dnet_process_control(st, cmd, r->data);
	if (err != -ENOTSUP) {
		dnet_access_context_add_string(context, "access", "server/control");
		goto out;
	}


	if (!(cmd->flags & DNET_FLAGS_DIRECT)) {
		forward_state = dnet_state_get_first(n, &cmd->id);
	}

	if (!forward_state || forward_state == st || forward_state == n->st) {
		dnet_state_put(forward_state);

		dnet_access_context_add_string(context, "access", "server");
		HANDY_COUNTER_INCREMENT("io.cmds", 1);
		err = dnet_process_cmd_raw(st, cmd, r->data, 0, r->queue_time, context);
	} else {
		dnet_access_context_add_string(context, "access", "server/forward");
		dnet_access_context_add_string(context, "forward", dnet_state_dump_addr(forward_state));
		HANDY_COUNTER_INCREMENT("io.forwards", 1);
		err = dnet_trans_forward(r, st, forward_state);
		if (err)
			goto err_out_put_forward;

		dnet_state_put(forward_state);
	}

out:
	dnet_access_access_put(context);
	return err;

err_out_put_forward:
	dnet_state_put(forward_state);
	dnet_access_access_put(context);
	return err;
}

void dnet_state_remove_nolock(struct dnet_net_state *st)
{
	list_del_init(&st->node_entry);
	list_del_init(&st->storage_state_entry);
	dnet_idc_destroy_nolock(st);
}

static void dnet_state_remove(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;

	pthread_mutex_lock(&n->state_lock);
	dnet_state_remove_nolock(st);
	pthread_mutex_unlock(&n->state_lock);
}

static void dnet_state_remove_and_shutdown(struct dnet_net_state *st, int error)
{
	int level = DNET_LOG_NOTICE;

	if (error && (error != -EUCLEAN && error != -EEXIST))
		level = DNET_LOG_ERROR;

	dnet_log(st->n, level, "%s: resetting state: %p: %s [%d], sockets: %d/%d",
			dnet_state_dump_addr(st), st, strerror(-error), error,
			st->read_s, st->write_s);

	pthread_mutex_lock(&st->send_lock);

	dnet_state_remove_nolock(st);

	if (!st->__need_exit) {
		if (!error)
			error = -123;

		st->__need_exit = error;

		shutdown(st->read_s, SHUT_RDWR);
		shutdown(st->write_s, SHUT_RDWR);

		//Wakes up sleeping threads and makes them exit because state is removed
		if ((atomic_read(&st->send_queue_size) > 0))
			pthread_cond_broadcast(&st->send_wait);
	}

	pthread_mutex_unlock(&st->send_lock);
}

int dnet_state_reset_nolock_noclean(struct dnet_net_state *st, int error, struct list_head *head)
{
	dnet_state_remove_and_shutdown(st, error);

	return dnet_trans_iterate_move_transaction(st, head);
}

void dnet_state_reset(struct dnet_net_state *st, int error)
{
	LIST_HEAD(head);

	/*
	 * Prevent route table access and update, check given state, move and then drop all its transactions
	 */
	pthread_mutex_lock(&st->n->state_lock);
	dnet_state_reset_nolock_noclean(st, error, &head);
	pthread_mutex_unlock(&st->n->state_lock);

	dnet_trans_clean_list(&head, error);
}


void dnet_sock_close(struct dnet_node *n, int s)
{
	char addr_str[128] = "no address";
	if (n->addr_num) {
		dnet_addr_string_raw(&n->addrs[0], addr_str, sizeof(addr_str));
	}
	dnet_log(n, DNET_LOG_NOTICE, "self: addr: %s, closing socket: %d", addr_str, s);

	shutdown(s, SHUT_RDWR);
	close(s);
}

void dnet_set_sockopt(struct dnet_node *n, int s)
{
	struct linger l;
	int opt;

	opt = 1;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &opt, 4);

	setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &n->keep_cnt, 4);
	opt = 10;
	setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &n->keep_idle, 4);
	opt = 10;
	setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &n->keep_interval, 4);

	l.l_onoff = 1;
	l.l_linger = 1;

	setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

	fcntl(s, F_SETFD, FD_CLOEXEC);
	fcntl(s, F_SETFL, O_NONBLOCK);
}

int dnet_setup_control_nolock(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;
	struct dnet_io *io = n->io;
	int err, pos;

	if (st->epoll_fd == -1) {
		pos = io->net_thread_pos;
		if (++io->net_thread_pos >= io->net_thread_num)
			io->net_thread_pos = 0;
		st->epoll_fd = io->net[pos].epoll_fd;

		pthread_mutex_lock(&st->send_lock);
		err = dnet_schedule_recv(st);
		if (err) {
			dnet_unschedule_all(st);
		}
		pthread_mutex_unlock(&st->send_lock);
		if (err)
			goto err_out_exit;
	}

	return 0;

err_out_exit:
	st->epoll_fd = -1;
	list_del_init(&st->storage_state_entry);
	return err;
}

static int dnet_auth_complete(struct dnet_addr *addr, struct dnet_cmd *cmd, void *priv)
{
	struct dnet_node *n = priv;
	struct dnet_net_state *state;

	if (!addr || !cmd)
		return -EPERM;

	/* this means this callback at least has state and cmd */
	if (!is_trans_destroyed(cmd)) {
		if (cmd->status == 0) {
			dnet_log(n, DNET_LOG_INFO, "%s: authentication request succeeded", dnet_addr_string(addr));
			return 0;
		}

		state = dnet_state_search_by_addr(n, addr);

		dnet_log(n, DNET_LOG_ERROR, "%s: authentication request failed: %d, state to reset: %p",
				dnet_addr_string(addr), cmd->status, state);

		if (state) {
			state->__join_state = 0;
			dnet_state_reset(state, -ECONNRESET);
		}
	}

	return cmd->status;
}

static int dnet_auth_send(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;
	struct dnet_trans_control ctl;
	struct dnet_auth a;

	memset(&a, 0, sizeof(struct dnet_auth));

	memcpy(a.cookie, n->cookie, DNET_AUTH_COOKIE_SIZE);
	dnet_convert_auth(&a);

	memset(&ctl, 0, sizeof(struct dnet_trans_control));

	ctl.cmd = DNET_CMD_AUTH;
	ctl.cflags = DNET_FLAGS_DIRECT | DNET_FLAGS_NEED_ACK;
	ctl.size = sizeof(struct dnet_auth);
	ctl.data = &a;

	ctl.complete = dnet_auth_complete;
	ctl.priv = n;

	return dnet_trans_alloc_send_state(NULL, st, &ctl);
}

int dnet_state_micro_init(struct dnet_net_state *st,
		struct dnet_node *n, struct dnet_addr *addr, int join)
{
	int err = 0;

	st->n = n;

	st->la = 1;

	INIT_LIST_HEAD(&st->node_entry);
	INIT_LIST_HEAD(&st->storage_state_entry);
	st->idc_root = RB_ROOT;
	err = pthread_rwlock_init(&st->idc_lock, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize idc mutex: err: %d", err);
		goto err_out;
	}

	st->trans_root = RB_ROOT;
	st->timer_root = RB_ROOT;

	st->epoll_fd = -1;

	err = pthread_mutex_init(&st->trans_lock, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize transaction mutex: %d", err);
		goto err_out_idc_destroy;
	}

	INIT_LIST_HEAD(&st->send_list);
	err = pthread_mutex_init(&st->send_lock, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize send mutex: %d", err);
		goto err_out_trans_destroy;
	}

	err = pthread_cond_init(&st->send_wait, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize send cond: %d", err);
		goto err_out_send_destroy;
	}

	atomic_init(&st->send_queue_size, 0);
	atomic_init(&st->refcnt, 1);

	memcpy(&st->addr, addr, sizeof(struct dnet_addr));

	dnet_schedule_command(st);
	st->__join_state = join;

	return 0;

err_out_send_destroy:
	pthread_mutex_destroy(&st->send_lock);
err_out_trans_destroy:
	pthread_mutex_destroy(&st->trans_lock);
err_out_idc_destroy:
	pthread_rwlock_destroy(&st->idc_lock);
err_out:
	return err;
}

int dnet_state_move_to_dht(struct dnet_net_state *st, struct dnet_addr *addrs, int addrs_count)
{
	struct dnet_net_state *other;
	struct dnet_node *n = st->n;
	int err = 0;

	pthread_mutex_lock(&n->state_lock);

	list_for_each_entry(other, &st->n->dht_state_list, node_entry) {
		if (dnet_addr_equal(other->addrs, addrs)) {
			pthread_mutex_unlock(&n->state_lock);

			dnet_state_reset(st, -EEXIST);

			return -EEXIST;
		}
	}

	memcpy(&st->addr, &addrs[st->idx], sizeof(struct dnet_addr));
	err = dnet_copy_addrs_nolock(st, addrs, addrs_count);

	if (!err) {
		list_move_tail(&st->node_entry, &st->n->dht_state_list);
		list_move_tail(&st->storage_state_entry, &st->n->storage_state_list);
	}

	pthread_mutex_unlock(&n->state_lock);

	if (err) {
		dnet_state_reset(st, err);
	}

	return err;
}

struct dnet_net_state *dnet_state_create(struct dnet_node *n,
		struct dnet_backend_ids **backends, int backends_count,
		struct dnet_addr *addr, int s, int *errp, int join, int server_node, int idx,
		int accepting_state, struct dnet_addr *addrs, int addrs_count)
{
	int err = -ENOMEM, i;
	struct dnet_net_state *st;

	if (server_node) {
		st = dnet_state_search_by_addr(n, addr);
		if (st) {
			err = -EEXIST;
			dnet_state_put(st);
			goto err_out_close;
		}
	}

	st = calloc(1, sizeof(struct dnet_net_state));
	if (!st)
		goto err_out_close;

	st->idx = idx;

	dnet_set_sockopt(n, s);

	if (accepting_state) {
		int sockets[2];

		st->accept_s = s;
		st->read_s = -1;
		st->write_s = -1;

		err = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, PF_UNIX, sockets);
		if (err < 0) {
			err = -errno;
			DNET_ERROR(n, "%s: failed to create socket pair", dnet_addr_string(addr));
			goto err_out_free;
		}

		st->read_s = sockets[0];
		st->write_s = sockets[1];
	} else {
		st->accept_s = -1;
		st->read_s = s;
		st->write_s = dup(s);
		if (st->write_s < 0) {
			err = -errno;
			DNET_ERROR(n, "%s: failed to duplicate socket", dnet_addr_string(addr));
			goto err_out_free;
		}
	}

	st->read_data.st = st;
	st->read_data.fd = st->read_s;

	st->write_data.st = st;
	st->write_data.fd = st->write_s;

	st->accept_data.st = st;
	st->accept_data.fd = st->accept_s;

	fcntl(st->write_s, F_SETFD, FD_CLOEXEC);

	dnet_log(n, DNET_LOG_DEBUG, "dnet_state_create: %s: sockets: %d/%d, server: %d, addrs_count: %d, backends_count: %d",
			dnet_addr_string(addr), st->read_s, st->write_s, server_node, addrs_count, backends_count);

	err = dnet_state_micro_init(st, n, addr, join);
	if (err)
		goto err_out_dup_destroy;

	if (n->client_prio && !accepting_state) {
		err = setsockopt(st->read_s, IPPROTO_IP, IP_TOS, &n->client_prio, 4);
		if (err) {
			err = -errno;
			DNET_ERROR(n, "could not set read client prio %d", n->client_prio);
		}
		err = setsockopt(st->write_s, IPPROTO_IP, IP_TOS, &n->client_prio, 4);
		if (err) {
			err = -errno;
			DNET_ERROR(n, "could not set write client prio %d", n->client_prio);
		}

		if (!err) {
			dnet_log(n, DNET_LOG_INFO, "%s: client net TOS value set to %d",
					dnet_addr_string(addr), n->client_prio);
		}
	}

	/*
	 * it is possible that state can be removed after inserted into route table,
	 * so we should grab a reference here and drop it after we are done
	 */
	dnet_state_get(st);

	if (server_node) {
		err = dnet_state_move_to_dht(st, addrs, addrs_count);
		if (err)
			goto err_out_send_destroy;

		for (i = 0; i < backends_count; ++i) {
			err = dnet_idc_update_backend(st, backends[i]);
			if (err) {
				dnet_state_reset(st, err);
				goto err_out_send_destroy;
			}
		}

		pthread_mutex_lock(&n->state_lock);
		err = dnet_setup_control_nolock(st);
		if (err)
			goto err_out_unlock;
		pthread_mutex_unlock(&n->state_lock);

		if (!accepting_state && st->__join_state == DNET_JOIN) {
			dnet_state_join(st);
			dnet_auth_send(st);

			dnet_state_set_server_prio(st);
		}
	} else {
		pthread_mutex_lock(&n->state_lock);
		list_add_tail(&st->node_entry, &n->empty_state_list);
		list_add_tail(&st->storage_state_entry, &n->storage_state_list);

		err = dnet_setup_control_nolock(st);
		if (err)
			goto err_out_unlock;
		pthread_mutex_unlock(&n->state_lock);
	}

	if (atomic_read(&st->refcnt) == 1) {
		err = st->__need_exit;
		if (!err)
			err = -ECONNRESET;
	}

	// do not release state if everything is ok
	// library/net.cpp:967 will use state to request route table from remote node and so on
	// but since state has been added into the route table it is not owned by 'creating' thread anymore,
	// in particular connection can be reset, network thread will pick up reset epoll event and call
	// dnet_state_reset() which will eventually kill state, while 'creating' thread is still using its pointer
	//
	// 'creating' thread must release state after it finished with it
	if (err) {
		dnet_state_put(st);
		goto err_out_exit;
	}

	return st;

err_out_send_destroy:
	pthread_mutex_lock(&n->state_lock);
err_out_unlock:
	list_del_init(&st->node_entry);
	list_del_init(&st->storage_state_entry);
	pthread_mutex_unlock(&n->state_lock);
	dnet_state_put(st);
	pthread_mutex_destroy(&st->send_lock);
	pthread_mutex_destroy(&st->trans_lock);
err_out_dup_destroy:
	dnet_sock_close(n, st->write_s);
err_out_free:
	free(st);
err_out_close:
	dnet_sock_close(n, s);

err_out_exit:
	if (err == -EEXIST)
		dnet_log(n, DNET_LOG_NOTICE, "%s: state already exists", dnet_addr_string(addr));
	*errp = err;
	return NULL;
}

int dnet_state_num(struct dnet_session *s)
{
	return dnet_node_state_num(s->node);
}

int dnet_node_state_num(struct dnet_node *n)
{
	struct dnet_net_state *st;
	int num = 0;

	pthread_mutex_lock(&n->state_lock);
	list_for_each_entry(st, &n->dht_state_list, node_entry) {
		num++;
	}
	pthread_mutex_unlock(&n->state_lock);

	return num;
}

static void dnet_state_send_clean(struct dnet_net_state *st)
{
	struct dnet_io_req *r, *tmp;
	uint64_t count = 0;

	list_for_each_entry_safe(r, tmp, &st->send_list, req_entry) {
		list_del(&r->req_entry);
		dnet_io_req_free(r);
		++count;
	}

	pthread_mutex_lock(&st->n->io->full_lock);
	list_stat_size_decrease(&st->n->io->output_stats, count);
	pthread_mutex_unlock(&st->n->io->full_lock);
	HANDY_COUNTER_DECREMENT("io.output.queue.size", count);
}

void dnet_state_destroy(struct dnet_net_state *st)
{
	dnet_log(st->n, DNET_LOG_NOTICE, "Going to destroy state %s [%p], socket: %d/%d, addr-num: %d.",
		dnet_addr_string(&st->addr), st, st->read_s, st->write_s, st->addr_num);

	dnet_state_remove(st);

	if (st->read_s >= 0) {
		dnet_sock_close(st->n, st->read_s);
		dnet_sock_close(st->n, st->write_s);
	}
	if (st->accept_s >= 0) {
		dnet_sock_close(st->n, st->accept_s);
	}

	dnet_state_clean(st);

	dnet_state_send_clean(st);

	pthread_rwlock_destroy(&st->idc_lock);
	pthread_mutex_destroy(&st->send_lock);
	pthread_mutex_destroy(&st->trans_lock);

	dnet_log(st->n, DNET_LOG_NOTICE, "Freeing state %s, socket: %d/%d, addr-num: %d.",
		dnet_addr_string(&st->addr), st->read_s, st->write_s, st->addr_num);

	free(st->addrs);

	memset(st, 0xff, sizeof(struct dnet_net_state));
	free(st);
}

int dnet_send_request(struct dnet_net_state *st, struct dnet_io_req *r)
{
	int cork;
	int err = 0;
	size_t offset = st->send_offset;
	const size_t total_size = r->dsize + r->hsize + r->fsize;
	uint64_t send_time = 0;

	if (total_size > sizeof(struct dnet_cmd)) {
		/* Use TCP_CORK to send headers and packet body in one piece */
		cork = 1;
		setsockopt(st->write_s, IPPROTO_TCP, TCP_CORK, &cork, 4);
	}

	if (st->send_offset == 0) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &st->send_start_ts);
		r->queue_time = DIFF_TIMESPEC(r->queue_start_ts, st->send_start_ts);
	}

	if (1) {
		struct dnet_cmd *cmd = r->header ? r->header : r->data;
		const enum dnet_log_level level = st->send_offset == 0 ? DNET_LOG_NOTICE : DNET_LOG_DEBUG;

		dnet_logger_set_trace_id(cmd->trace_id, cmd->flags & DNET_FLAGS_TRACE_BIT);
		dnet_log(st->n, level, "%s: %s: sending trans: %lld -> %s/%d: size: %llu, cflags: %s, start-sent: "
		                       "%zd/%zd, send-queue-time: %lu usecs",
		         dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), (unsigned long long)cmd->trans,
		         dnet_addr_string(&st->addr), cmd->backend_id, (unsigned long long)cmd->size,
		         dnet_flags_dump_cflags(cmd->flags), st->send_offset, total_size, r->queue_time);
	}

	if (r->hsize && r->header && st->send_offset < r->hsize) {
		err = dnet_send_nolock(st, r->header + offset, r->hsize - offset);
		if (err)
			goto err_out_exit;
	}

	if (r->dsize && r->data && st->send_offset < (r->dsize + r->hsize)) {
		offset = st->send_offset - r->hsize;
		err = dnet_send_nolock(st, r->data + offset, r->dsize - offset);
		if (err)
			goto err_out_exit;
	}

	if (r->fd >= 0 && r->fsize && st->send_offset < total_size) {
		offset = st->send_offset - r->dsize - r->hsize;
		err = dnet_send_fd_nolock(st, r->fd, r->local_offset + offset, r->fsize - offset);
		if (err)
			goto err_out_exit;
	}

	if (r->hsize > sizeof(struct dnet_cmd)) {
		struct dnet_cmd *cmd = r->header;
		int nonblocking = !!(cmd->flags & DNET_FLAGS_NOLOCK);

		dnet_log(st->n, DNET_LOG_DEBUG, "%s: %s: SENT %s cmd: %s: cmd-size: %llu, nonblocking: %d",
			dnet_state_dump_addr(st), dnet_dump_id(r->header),
			nonblocking ? "nonblocking" : "blocking",
			dnet_cmd_string(cmd->cmd),
			(unsigned long long)cmd->size, nonblocking);
	}

err_out_exit:

	if (1) {
		struct dnet_cmd *cmd = r->header ? r->header : r->data;
		enum dnet_log_level level = DNET_LOG_DEBUG;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		send_time = DIFF_TIMESPEC(st->send_start_ts, ts);

		if (st->send_offset == total_size) {
			level = !(cmd->flags & DNET_FLAGS_MORE) ? DNET_LOG_INFO : DNET_LOG_NOTICE;
		}
		dnet_log(st->n, level, "%s: %s: sending trans: %lld -> %s/%d: size: %llu, cflags: %s, finish-sent: "
		                       "%zd/%zd, send-queue-time: %lu usecs, send-time: %lu usecs",
		         dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), (unsigned long long)cmd->trans,
		         dnet_addr_string(&st->addr), cmd->backend_id, (unsigned long long)cmd->size,
		         dnet_flags_dump_cflags(cmd->flags), st->send_offset, total_size, r->queue_time, send_time);
		dnet_access_context_add_uint(r->context, "send_time", send_time);
		dnet_access_context_add_uint(r->context, "send_queue_time", r->queue_time);
		dnet_access_context_add_uint(r->context, "response_size", total_size);
	}
	dnet_logger_unset_trace_id();

	if (total_size > sizeof(struct dnet_cmd)) {
		cork = 0;
		setsockopt(st->write_s, IPPROTO_TCP, TCP_CORK, &cork, 4);
	}

	/*
	 * Flush TCP output pipeline if we've sent whole request.
	 *
	 * We do not destroy request here, it is postponed to caller.
	 * Function can be called without lock - default call path from network processing thread and dnet_process_send_single()
	 * or under st->send_lock, if queue was empty and dnet_send*() caller directly invoked this function from dnet_io_req_queue()
	 * instead of queuing.
	 */
	if (st->send_offset == total_size) {
		int nodelay = 1;
		setsockopt(st->write_s, IPPROTO_TCP, TCP_NODELAY, &nodelay, 4);
	}

	if (1) {
		struct dnet_cmd *cmd = r->header ? r->header : r->data;
		if (!(cmd->flags & DNET_FLAGS_REPLY)) {
			struct dnet_trans *t = NULL;
			pthread_mutex_lock(&st->trans_lock);
			t = dnet_trans_search(st, cmd->trans);
			if (t) {
				t->stats.send_queue_time = r->queue_time;
				t->stats.send_time = send_time;
			}
			pthread_mutex_unlock(&st->trans_lock);
			dnet_trans_put(t);
		}
	}

	return err;
}

int dnet_parse_addr(char *addr, int *portp, int *familyp)
{
	char *fam, *port;

	fam = strrchr(addr, DNET_CONF_ADDR_DELIM);
	if (!fam)
		goto err_out_print_wrong_param;
	*fam++ = 0;
	if (!fam)
		goto err_out_print_wrong_param;

	port = strrchr(addr, DNET_CONF_ADDR_DELIM);
	if (!port)
		goto err_out_print_wrong_param;
	*port++ = 0;
	if (!port)
		goto err_out_print_wrong_param;

	*familyp = atoi(fam);
	*portp = atoi(port);

	return 0;

err_out_print_wrong_param:
	fprintf(stderr, "Wrong address parameter '%s', should be 'addr%cport%cfamily'.",
				addr, DNET_CONF_ADDR_DELIM, DNET_CONF_ADDR_DELIM);
	return -EINVAL;
}
