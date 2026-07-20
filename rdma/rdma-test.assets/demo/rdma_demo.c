/*
 * Minimal Soft-RoCE / RDMA demo (rdma_cm + RDMA Write)
 *
 * Build (Ubuntu 20.04):
 *   sudo apt install -y build-essential libibverbs-dev librdmacm-dev
 *   make
 *
 * Server (10.116.89.94):
 *   sudo ./rdma_demo server -a 10.116.89.94
 *
 * Client (10.116.89.201):
 *   ./rdma_demo client -a 10.116.89.94 -m "Hello RDMA from NUC!"
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT "7471"
#define MSG_CAP 256

struct mem_info {
	uint64_t addr;
	uint32_t rkey;
	uint32_t len;
};

struct conn_ctx {
	struct ibv_pd *pd;
	struct ibv_comp_channel *comp_channel;
	struct ibv_cq *cq;
	char *buf;
	size_t len;
	struct ibv_mr *mr;
};

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static struct conn_ctx *setup_conn(struct rdma_cm_id *id)
{
	struct conn_ctx *ctx = calloc(1, sizeof(*ctx));
	struct ibv_qp_init_attr qp_attr = {0};

	if (!ctx)
		die("calloc");

	ctx->pd = ibv_alloc_pd(id->verbs);
	if (!ctx->pd)
		die("ibv_alloc_pd");

	ctx->comp_channel = ibv_create_comp_channel(id->verbs);
	if (!ctx->comp_channel)
		die("ibv_create_comp_channel");

	ctx->cq = ibv_create_cq(id->verbs, 16, NULL, ctx->comp_channel, 0);
	if (!ctx->cq)
		die("ibv_create_cq");

	qp_attr.send_cq = ctx->cq;
	qp_attr.recv_cq = ctx->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 8;
	qp_attr.cap.max_recv_wr = 8;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	if (rdma_create_qp(id, ctx->pd, &qp_attr))
		die("rdma_create_qp");

	ctx->len = MSG_CAP;
	ctx->buf = calloc(1, ctx->len);
	if (!ctx->buf)
		die("calloc buf");

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->len,
			     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->mr)
		die("ibv_reg_mr");

	id->context = ctx;
	return ctx;
}

static void destroy_conn(struct rdma_cm_id *id)
{
	struct conn_ctx *ctx = id->context;

	if (!ctx)
		return;

	if (ctx->mr)
		ibv_dereg_mr(ctx->mr);
	free(ctx->buf);
	if (ctx->cq)
		ibv_destroy_cq(ctx->cq);
	if (ctx->comp_channel)
		ibv_destroy_comp_channel(ctx->comp_channel);
	if (ctx->pd)
		ibv_dealloc_pd(ctx->pd);
	free(ctx);
	id->context = NULL;
}

static int wait_send_done(struct conn_ctx *ctx, int timeout_ms)
{
	struct ibv_wc wc;
	int elapsed = 0;

	while (elapsed < timeout_ms) {
		int n = ibv_poll_cq(ctx->cq, 1, &wc);
		if (n < 0)
			die("ibv_poll_cq");
		if (n == 1) {
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "CQ error: %s\n",
					ibv_wc_status_str(wc.status));
				return -1;
			}
			return 0;
		}
		usleep(1000);
		elapsed += 1;
	}

	fprintf(stderr, "timeout waiting for send completion\n");
	return -1;
}

static int resolve_addr(struct rdma_cm_id *id, const char *host, const char *port)
{
	struct addrinfo hints = {0}, *res = NULL;
	int ret;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(host, port, &hints, &res);
	if (ret) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		return -1;
	}

	ret = rdma_resolve_addr(id, NULL, res->ai_addr, 2000);
	freeaddrinfo(res);
	return ret;
}

static int run_server(const char *bind_ip, const char *port)
{
	struct rdma_event_channel *ec = NULL;
	struct rdma_cm_id *listen_id = NULL, *conn_id = NULL;
	struct rdma_cm_event *event = NULL;
	struct sockaddr_in sin = {0};
	struct mem_info info;
	struct rdma_conn_param conn_param = {0};
	struct conn_ctx *ctx = NULL;
	bool got_data = false;

	ec = rdma_create_event_channel();
	if (!ec)
		die("rdma_create_event_channel");

	if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP))
		die("rdma_create_id listen");

	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)atoi(port));
	if (inet_pton(AF_INET, bind_ip, &sin.sin_addr) != 1)
		die("inet_pton bind_ip");

	if (rdma_bind_addr(listen_id, (struct sockaddr *)&sin))
		die("rdma_bind_addr");

	if (rdma_listen(listen_id, 1))
		die("rdma_listen");

	printf("[server] listening on %s:%s (waiting for client...)\n",
	       bind_ip, port);

	if (rdma_get_cm_event(ec, &event))
		die("rdma_get_cm_event");
	if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
		fprintf(stderr, "[server] expected CONNECT_REQUEST, got %d\n",
			event->event);
		rdma_ack_cm_event(event);
		return 1;
	}

	conn_id = event->id;
	printf("[server] got connect request\n");

	ctx = setup_conn(conn_id);
	memset(ctx->buf, 0, ctx->len);

	info.addr = (uint64_t)(uintptr_t)ctx->buf;
	info.rkey = ctx->mr->rkey;
	info.len = (uint32_t)ctx->len;

	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 7;
	conn_param.private_data = &info;
	conn_param.private_data_len = sizeof(info);

	if (rdma_accept(conn_id, &conn_param))
		die("rdma_accept");
	rdma_ack_cm_event(event);

	if (rdma_get_cm_event(ec, &event))
		die("rdma_get_cm_event established");
	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "[server] expected ESTABLISHED, got %d\n",
			event->event);
		rdma_ack_cm_event(event);
		return 1;
	}
	rdma_ack_cm_event(event);

	printf("[server] connection established, waiting for RDMA write...\n");

	for (int i = 0; i < 5000 && !got_data; i++) {
		if (ctx->buf[0] != '\0')
			got_data = true;
		else
			usleep(1000);
	}

	if (!got_data) {
		fprintf(stderr, "[server] no data received\n");
		return 1;
	}

	printf("[server] received: \"%s\"\n", ctx->buf);
	printf("[server] success\n");

	rdma_disconnect(conn_id);
	destroy_conn(conn_id);
	rdma_destroy_id(conn_id);
	rdma_destroy_id(listen_id);
	rdma_destroy_event_channel(ec);
	return 0;
}

static int run_client(const char *server_ip, const char *port, const char *message)
{
	struct rdma_event_channel *ec = NULL;
	struct rdma_cm_id *id = NULL;
	struct rdma_cm_event *event = NULL;
	struct rdma_conn_param conn_param = {0};
	struct conn_ctx *ctx = NULL;
	struct mem_info remote = {0};
	struct ibv_send_wr wr = {0}, *bad_wr = NULL;
	struct ibv_sge sge = {0};
	size_t msg_len;

	ec = rdma_create_event_channel();
	if (!ec)
		die("rdma_create_event_channel");

	if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP))
		die("rdma_create_id client");

	if (resolve_addr(id, server_ip, port))
		die("rdma_resolve_addr");

	if (rdma_get_cm_event(ec, &event))
		die("rdma_get_cm_event addr");
	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
		die("expected ADDR_RESOLVED");
	rdma_ack_cm_event(event);

	if (rdma_resolve_route(id, 2000))
		die("rdma_resolve_route");

	if (rdma_get_cm_event(ec, &event))
		die("rdma_get_cm_event route");
	if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
		die("expected ROUTE_RESOLVED");
	rdma_ack_cm_event(event);

	ctx = setup_conn(id);
	msg_len = strnlen(message, ctx->len - 1);
	memcpy(ctx->buf, message, msg_len);
	ctx->buf[msg_len] = '\0';

	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 7;

	if (rdma_connect(id, &conn_param))
		die("rdma_connect");

	if (rdma_get_cm_event(ec, &event))
		die("rdma_get_cm_event connect");
	if (event->event != RDMA_CM_EVENT_ESTABLISHED)
		die("expected ESTABLISHED");

	if (event->param.conn.private_data_len < sizeof(remote)) {
		fprintf(stderr, "[client] server did not return mem info\n");
		return 1;
	}
	memcpy(&remote, event->param.conn.private_data, sizeof(remote));
	rdma_ack_cm_event(event);

	printf("[client] connected, remote addr=0x%lx rkey=0x%x\n",
	       (unsigned long)remote.addr, remote.rkey);

	sge.addr = (uint64_t)(uintptr_t)ctx->buf;
	sge.length = (uint32_t)(msg_len + 1);
	sge.lkey = ctx->mr->lkey;

	wr.wr_id = 1;
	wr.opcode = IBV_WR_RDMA_WRITE;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = remote.addr;
	wr.wr.rdma.rkey = remote.rkey;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	if (ibv_post_send(id->qp, &wr, &bad_wr))
		die("ibv_post_send RDMA_WRITE");

	if (wait_send_done(ctx, 5000) != 0)
		return 1;

	printf("[client] RDMA write done: \"%s\"\n", ctx->buf);
	printf("[client] success\n");

	rdma_disconnect(id);
	destroy_conn(id);
	rdma_destroy_id(id);
	rdma_destroy_event_channel(ec);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s server -a <bind-ip> [-p port]\n"
		"  %s client -a <server-ip> [-p port] [-m message]\n"
		"\n"
		"Example:\n"
		"  sudo %s server -a 10.116.89.94\n"
		"  %s client -a 10.116.89.94 -m \"Hello RDMA!\"\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	const char *mode = NULL;
	const char *addr = NULL;
	const char *port = DEFAULT_PORT;
	const char *message = "Hello RDMA!";

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	mode = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-a") && i + 1 < argc)
			addr = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			message = argv[++i];
		else {
			usage(argv[0]);
			return 1;
		}
	}

	if (!addr) {
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(mode, "server"))
		return run_server(addr, port);
	if (!strcmp(mode, "client"))
		return run_client(addr, port, message);

	usage(argv[0]);
	return 1;
}

