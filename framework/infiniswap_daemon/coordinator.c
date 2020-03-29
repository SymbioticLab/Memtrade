#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <infiniband/verbs.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/queue.h>
#include <stdatomic.h>

#include "utils.h"

#include "rcommon.h"

long page_size;
int running;

static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_id *id);
static int on_disconnect(struct rdma_cm_id *id);
static void process_event(struct rdma_cm_event *event);
static void build_context(struct ibv_context *verbs);

static struct context *s_ctx = NULL;
atomic_int producer_id = ATOMIC_VAR_INIT(1);

struct consumer_list_t;
struct producer_list_t;

struct {
	char ip[200];
	int port;
	char producer_map[MAX_PRODUCER+2][210];
} globals;

struct coordinator_t {
	struct rdma_cm_id *rid;
	struct rdma_event_channel *rchannel;
};

struct producer_info_t {
	/* pointer to active producers list */
	struct producer_list_t *lstptr;
	char ip[200];
	int port;
	uint64_t rdmakey;
	int nslabs;
	int available_slabs;
	void *memaddr;
	int id;
	int* slab_availability_flag;
};

struct producer_list_t {
	struct producer_info_t *producer;
	struct producer_list_t *next;
	struct producer_list_t *prev;
} * _producerlist_head, *_producerlist_tail;

struct consumer_info_t {
	/* pointer to active consumer list */
	struct consumer_list_t *lstptr;
	struct producer_info_t *producer;
};

struct consumer_list_t {
	struct consumer_info_t *consumer;
	struct consumer_list_t *next;
	struct consumer_list_t *prev;
} * _consumerlist_head, *_consumerlist_tail;

/**********************************
 ***********************************/

static volatile bool aborted = false;

void sig_handler(int sig) {
	if (sig == SIGINT)
		aborted = true;
}

void register_signal_handler(void) {
	int r;
	struct sigaction sigint_handler = {
		.sa_handler = sig_handler
	};

	sigemptyset(&sigint_handler.sa_mask);

	r = sigaction(SIGINT, &sigint_handler, NULL);
	if (r < 0)
		pr_err("could not register signal handler");
}

/**********************************
	bitmap management
************************************/
#define BITMAP_MASK 0x1f
#define BITMAP_SHIFT 5

static void init_bitmap(struct producer_info_t* producer, int size) {
	size = (align_up(size, 1<<BITMAP_SHIFT))>>BITMAP_SHIFT;
	pr_info("bitmap size: %d", size);
	producer->slab_availability_flag = (int *) malloc(size*sizeof(int));
	memset(producer->slab_availability_flag, 0x00, size*sizeof(int));
}

static void set_bit(int* bitmap, int bit) {
	bitmap[bit>>BITMAP_SHIFT] |= 1 << (bit & BITMAP_MASK);
}

static void clear_bit(int* bitmap, int bit) {
	bitmap[bit>>BITMAP_SHIFT] &= ~1 << (bit & BITMAP_MASK);
}

static int get_bit(int* bitmap, int bit) {
	return ((bitmap[bit >> BITMAP_SHIFT] & (1 << (bit & BITMAP_MASK))) != 0);
}

/************************************
	producer list management
***********************************/

static void producerlist_init() {
	_producerlist_head = (struct producer_list_t *)malloc(sizeof(struct producer_list_t));
	_producerlist_tail = (struct producer_list_t *)malloc(sizeof(struct producer_list_t));
	_producerlist_head->producer = NULL;
	_producerlist_tail->producer = NULL;
	_producerlist_head->next = _producerlist_tail;
	_producerlist_head->prev = NULL;
	_producerlist_tail->next = NULL;
	_producerlist_tail->prev = _producerlist_head;
}

struct producer_info_t* find_producer_by_id(int id) {
	struct producer_list_t *serv = _producerlist_head->next;

	while (serv != _producerlist_tail) {
		if(serv->producer->id == id) {
			return serv->producer;
		}
		serv = serv->next;
	}
	return NULL;
}

static void get_producer_id(struct producer_info_t *producer) {
	int i;
	char hash[210];
	sprintf(hash, "%s:%d", producer->ip, producer->port);
	for(i = 1; i < MAX_PRODUCER && i < producer_id; i++){
		if(globals.producer_map[i] && strcmp(globals.producer_map[i],hash) == 0) {
			producer->id = i;
			return;
		}
	}
	producer->id = atomic_fetch_add_explicit(&producer_id, 1, memory_order_acquire);
	sprintf(globals.producer_map[producer->id], "%s:%d", producer->ip, producer->port);
	return;
}

static int is_duplicate_producer(struct producer_info_t *producer) {
	if(producer->ip == NULL || !producer->port) {
		pr_err("invalid producer ip or port");
		return -1;
	}

	struct producer_list_t *serv = _producerlist_head->next;

	while (serv != _producerlist_tail) {
		if(strcmp(serv->producer->ip, producer->ip) == 0 && serv->producer->port == producer->port) {
			producer->lstptr = serv->producer->lstptr;
			producer->id = serv->producer->id;
			serv->producer = producer;
			pr_info("producer already exists with id: %d", producer->id);
			return 1;
		}
		serv = serv->next;
	}
	return 0;
}

static void producerlist_add(struct producer_info_t *serv) {
	pr_debug("producerlist adding producer %p link %p head %p tail %p", serv, serv->lstptr, _producerlist_head, _producerlist_tail);

	if(is_duplicate_producer(serv) == 0) {
		struct producer_list_t *newserv = (struct producer_list_t *) malloc(sizeof(struct producer_list_t));
		serv->lstptr = newserv;
		newserv->producer = serv;
		newserv->next = _producerlist_head->next;
		newserv->prev = _producerlist_head;
		newserv->next->prev = newserv;
		_producerlist_head->next = newserv;
	}
}

static void producerlist_remove(struct producer_info_t *serv) {
	ASSERT(serv);

	pr_debug("producerlist remove producer %p link %p head %p tail %p", serv, serv->lstptr, _producerlist_head, _producerlist_tail);

	struct producer_list_t *oldserv = serv->lstptr;

	ASSERT(oldserv != _producerlist_head);
	ASSERT(oldserv != _producerlist_tail);

	oldserv->prev->next = oldserv->next;
	oldserv->next->prev = oldserv->prev;

	free(oldserv->producer);
	free(oldserv);
}

static void producerlist_destroy() {
	pr_debug("producerlist destroy");
	struct producer_list_t *serv = _producerlist_head->next;
	struct producer_list_t *oldserv = NULL;

	while (serv != _producerlist_tail) {
		oldserv = serv;
		serv = serv->next;
		producerlist_remove(oldserv->producer);
	}

	pr_debug("freeing head %p and tail %p", _producerlist_head, _producerlist_tail);
	free(_producerlist_head);
	free(_producerlist_tail);
}

/************************************
	consumer list management
***********************************/

static void consumerlist_init() {
	_consumerlist_head = (struct consumer_list_t *) malloc(sizeof(struct consumer_list_t));
	_consumerlist_tail = (struct consumer_list_t *) malloc(sizeof(struct consumer_list_t));
	_consumerlist_head->consumer = NULL;
	_consumerlist_tail->consumer = NULL;
	_consumerlist_head->next = _consumerlist_tail;
	_consumerlist_head->prev = NULL;
	_consumerlist_tail->next = NULL;
	_consumerlist_tail->prev = _consumerlist_head;
}

static void consumerlist_add(struct consumer_info_t *cli) {
	pr_debug("consumerlist adding consumer %p link %p head %p tail %p", cli, cli->lstptr, _consumerlist_head, _consumerlist_tail);

	struct consumer_list_t *newcli = (struct consumer_list_t *) malloc(sizeof(struct consumer_list_t));
	cli->lstptr = newcli;
	newcli->consumer = cli;
	newcli->next = _consumerlist_head->next;
	newcli->prev = _consumerlist_head;
	newcli->next->prev = newcli;
	_consumerlist_head->next = newcli;
}

static void consumerlist_remove(struct consumer_info_t *cli) {
	ASSERT(cli);

	pr_debug("consumerlist remove consumer %p link %p head %p tail %p", cli, cli->lstptr, _consumerlist_head, _consumerlist_tail);

	struct consumer_list_t *oldcli = cli->lstptr;

	ASSERT(oldcli != _consumerlist_head);
	ASSERT(oldcli != _consumerlist_tail);

	oldcli->prev->next = oldcli->next;
	oldcli->next->prev = oldcli->prev;

	free(oldcli->consumer);
	free(oldcli);
}

static void consumerlist_destroy() {
	pr_debug("consumerlist destroy");
	struct consumer_list_t *cli = _consumerlist_head->next;
	struct consumer_list_t *oldcli = NULL;

	while (cli != _consumerlist_tail) {
		oldcli = cli;
		cli = cli->next;
		consumerlist_remove(oldcli->consumer);
	}

	pr_debug("freeing head %p and tail %p", _consumerlist_head, _consumerlist_tail);
	free(_consumerlist_head);
	free(_consumerlist_tail);
}

/******************************************
	producer
 ******************************************/

static void coordinator_create(struct coordinator_t *rrs) {
	struct sockaddr_in addr;
	uint16_t port = globals.port;

	rrs->rchannel = NULL;
	rrs->rid = NULL;

	producerlist_init();
	consumerlist_init();

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	// address 0 - listen across all RDMA devices
	addr.sin_port = htons(port);
	inet_aton(globals.ip, &addr.sin_addr);

	ASSERT(rrs->rchannel = rdma_create_event_channel());
	ASSERTZ(rdma_create_id(rrs->rchannel, &rrs->rid, NULL, RDMA_PS_TCP));
	ASSERTZ(rdma_bind_addr(rrs->rid, (struct sockaddr *)&addr));

	build_context(rrs->rid->verbs);

	ASSERTZ(rdma_listen(rrs->rid, 10)); /* backlog=10 is arbitrary */

	port = ntohs(rdma_get_src_port(rrs->rid));
	pr_info("coordinator %s listening on port %d.", globals.ip, port);
}

static void coordinator_destroy(struct coordinator_t *rrs) {
	pr_debug("destroying pbcntrl");

	rdma_destroy_id(rrs->rid);
	rdma_destroy_event_channel(rrs->rchannel);

	producerlist_destroy();
	consumerlist_destroy();
}

static void coordinator_run() {
	struct rdma_cm_event *event = NULL;

	struct coordinator_t *rrs = (struct coordinator_t *) malloc(sizeof(struct coordinator_t));

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	pr_info("Pinning coordinator to core %d", PIN_COORDINATOR_CORE);
	CPU_SET(PIN_COORDINATOR_CORE, &cpuset);
	ASSERTZ(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));

	pr_debug("creating coordinator");
	coordinator_create(rrs);

	while ((rdma_get_cm_event(rrs->rchannel, &event) == 0) && !aborted) {
		struct rdma_cm_event event_copy;

		pr_debug("coordinator loop");

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		process_event(&event_copy);
	}

	pr_debug("destroying coordinator");
	coordinator_destroy(rrs);
}

/*********************************************
 *********************************************/

void usage() {
	printf("Usage ./central-coordinator [-s coordinator-ip] [-p port]\n");
	printf("Default coordinator address is %s\n", COORDINATOR_IP);
	printf("Default port is %d\n", COORDINATOR_PORT);
	printf("\n");
}

int main(int argc, char **argv) {
	int opt;
	register_signal_handler();

	strcpy(globals.ip, COORDINATOR_IP);
	globals.port = COORDINATOR_PORT;

	while ((opt = getopt(argc, argv, "hs:p:")) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return 0;
		case 's':
			strcpy(globals.ip, optarg);
			break;
		case 'p':
			globals.port = atoi(optarg);
			break;
		}
	}

	pr_info("coordinator started");
	coordinator_run();
	pr_info("coordinator stopped");
	return 0;
}

/********************************************
	Remote memory management
 *********************************************/
// TODO: code for bit handling of slabs
int is_slab_available(struct producer_info_t *serv, int slab) {
	if(slab > serv->nslabs)
		return 0;
	return (get_bit(serv->slab_availability_flag, slab) == 0);
}

int find_available_slabs(struct producer_info_t *serv) {
	int slab = 0;
	while ((slab < serv->nslabs) && (!is_slab_available(serv, slab))) {
		++slab;
	}
	ASSERT(slab < serv->nslabs);
	return slab;
}

void* addr_from_slab(struct producer_info_t *producer, int slab) {
	if(producer == NULL || slab < 0 || slab >= producer->nslabs )
		return NULL;
	return producer->memaddr + slab * RDMA_PRODUCER_SLAB_SIZE;
}

int slab_from_addr(struct producer_info_t *producer, uint64_t addr) {
	if(producer == NULL || addr < (uint64_t) producer->memaddr || addr >= (uint64_t) producer->memaddr + producer->nslabs*RDMA_PRODUCER_SLAB_SIZE)
		return -1;
	return (int)((addr - (uint64_t) producer->memaddr)/RDMA_PRODUCER_SLAB_SIZE);
}

// @return: the number of allocated slabs within the same producer
int alloc_next_slabs(int requested_slabs, struct producer_info_t **producer, uint64_t *slab_addr) {
	int granted_slabs = 0, i;
	int next_slab = -1;
	struct producer_info_t *cproducer = NULL;
	struct producer_list_t *serv = _producerlist_head->next;

	while ((serv != NULL) && (granted_slabs == 0)) {
		cproducer = serv->producer;
		if (cproducer->available_slabs > 0) {
			next_slab = find_available_slabs(cproducer);
			ASSERT(next_slab != -1);
			*slab_addr = (uint64_t)addr_from_slab(cproducer, next_slab);
			granted_slabs = (cproducer->nslabs - next_slab >= requested_slabs) ? requested_slabs : (cproducer->nslabs - next_slab);
			*producer = cproducer;
			pr_debug("%s: slab:%d, granted:%d, addr:%p", __func__, next_slab, granted_slabs, (void *) slab_addr);
			for(i=0; i<granted_slabs; i++)
				set_bit(cproducer->slab_availability_flag, next_slab+i);
			cproducer->available_slabs -= granted_slabs;
			return granted_slabs;
		}
		serv = serv->next;
	}

	pr_err("rack is out of memory!");
	return granted_slabs;
}

/**************************************************/

static void send_msg_done(struct connection *conn) {
	pr_debug("send_msg_done");

	conn->send_msg->type = MSG_DONE;
	conn->send_msg->data.addr = NULL;

	send_message(conn);
}

static void on_recv_producer_add(struct connection *conn) {
	pr_debug("on_recv_producer_add");

	struct consumer_info_t *cli = (struct consumer_info_t *)conn->peer;
	assert(cli->producer == NULL);
	struct producer_info_t *producer = (struct producer_info_t *)malloc(sizeof(struct producer_info_t));
	pr_info("adding producer %p (consumer id %p)", producer, cli);

	producer->lstptr = NULL;
	strcpy(producer->ip, conn->recv_msg->data.ip);
	producer->port = conn->recv_msg->data.port;
	producer->nslabs = conn->recv_msg->data.nslabs;
	producer->available_slabs = producer->nslabs;
	producer->memaddr = conn->recv_msg->data.addr;
	producer->rdmakey = conn->recv_msg->data.rdmakey;
	get_producer_id(producer);
	init_bitmap(producer, producer->available_slabs);
	cli->producer = producer;

	/* add to list of producers */
	producerlist_add(producer);
	pr_info("producer id: %d, ip: %s, port: %d, available slabs: %d, memaddr: %p, flag: %p, global_map: %s",
		producer->id, producer->ip, producer->port, producer->available_slabs, producer->memaddr, &producer->slab_availability_flag, globals.producer_map[producer->id]);

	post_receives(conn);

	send_msg_done(conn);
}

static void on_recv_producer_rem(struct connection *conn) {
	pr_debug("on_recv_producer_rem");

	struct consumer_info_t *cli = (struct consumer_info_t *)conn->peer;
	struct producer_info_t *producer = cli->producer;

	producer->nslabs = 0;
	producer->available_slabs = 0;
	cli->producer = NULL;
	producerlist_remove(producer);

	post_receives(conn);

	send_msg_done(conn);
}

static void on_recv_slabs_add(struct connection *conn) {
	pr_debug("on_recv_slab_add");

	//struct consumer_info_t *consumer = (struct consumer_info_t *)conn->peer;
	unsigned long slab_addr;

	// TODO: allocate slabs

	// find and allocate next available slabs
	int nrequested_slabs = conn->recv_msg->data.nslabs;

	while(nrequested_slabs > 0) {
		struct producer_info_t *producer;
		int granted_slabs = alloc_next_slabs(nrequested_slabs, &producer, &slab_addr);

		if (granted_slabs == 0)
			break;
		conn->send_msg->type = MSG_SLAB_ADD_PARTIAL;
		strcpy(conn->send_msg->data.ip, producer->ip);
		conn->send_msg->data.port = producer->port;
		conn->send_msg->data.id = producer->id;
		conn->send_msg->data.rdmakey = producer->rdmakey;
		conn->send_msg->data.nslabs = granted_slabs;
		conn->send_msg->data.addr = (void *)slab_addr;

		post_receives(conn);
		send_message(conn);
		pr_debug("%s: sent slab info. id:%d, addr:%p, nslabs:%d", __func__, conn->send_msg->data.id, conn->send_msg->data.addr, conn->send_msg->data.nslabs);
		nrequested_slabs -= granted_slabs;
		usleep(10);
	}
	conn->send_msg->type = MSG_DONE_SLAB_ADD;
	conn->send_msg->data.addr = NULL;
	post_receives(conn);
	send_message(conn);
}

static void on_recv_slabs_rem(struct connection *conn) {
	pr_debug("on_recv_slab_rem");

	//struct consumer_info_t *consumer = (struct consumer_info_t *)conn->peer;

	// TODO: remove slabs
	int id = conn->recv_msg->data.id;
	int nslabs = conn->recv_msg->data.nslabs;
	void *addr = conn->recv_msg->data.addr;

	struct producer_info_t *producer = find_producer_by_id(id);
	if(producer == NULL)
		goto out;
	int slab = slab_from_addr(producer, (unsigned long) addr);
	int i;
	pr_debug("%s, slab:%d, nslabs:%d, addr:%p",__func__, slab,nslabs, addr);
	for(i=0; i< nslabs; i++) {
		clear_bit(producer->slab_availability_flag, slab+i);
		producer->available_slabs++;
	}
out:
	post_receives(conn);
	send_msg_done(conn);
}

/*************************************
	RDMA
 *************************************/

static void on_completion(struct ibv_wc *wc) {
	pr_debug("completion..");
	struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

	if (wc->status != IBV_WC_SUCCESS) {
		pr_err("RDMA request failed with status %d: %s", wc->status, ibv_wc_status_str(wc->status));
		return;
	}

	if (wc->opcode & IBV_WC_RECV) {

		switch (conn->recv_msg->type) {
		case MSG_PRODUCER_ADD:
			on_recv_producer_add(conn);
			break;
		case MSG_PRODUCER_REM:
			on_recv_producer_rem(conn);
			break;
		case MSG_SLAB_ADD:
			on_recv_slabs_add(conn);
			break;
		case MSG_SLAB_REM:
			on_recv_slabs_rem(conn);
		default:
			BUG();
		}
	}
	else {
		pr_info("send completed successfully msg_type %d conn %p.", conn->send_msg->type, conn);
	}
}

void *poll_cq(void *arg) {
	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *ctx;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	pr_info("Pinning completion poller to core %d", PIN_CC_POLLER_CORE);
	CPU_SET(PIN_CC_POLLER_CORE, &cpuset);
	ASSERTZ(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));

	while (!ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx)) {
		ibv_ack_cq_events(cq, 1);
		ASSERTZ(ibv_req_notify_cq(cq, 0));

		while (ibv_poll_cq(cq, 1, &wc))
			on_completion(&wc);
	}

	return NULL;
}

void register_memory(struct connection *conn) {
	conn->send_msg = malloc(sizeof(struct message));
	conn->recv_msg = malloc(sizeof(struct message));

	ASSERT(conn->send_mr = ibv_reg_mr(
		s_ctx->pd,
		conn->send_msg,
		sizeof(struct message),
		IBV_ACCESS_LOCAL_WRITE)
	);

	ASSERT(conn->recv_mr = ibv_reg_mr(
		s_ctx->pd,
		conn->recv_msg,
		sizeof(struct message),
		IBV_ACCESS_LOCAL_WRITE)
	);
}

static void build_context(struct ibv_context *verbs) {
	if (s_ctx) {
		ASSERT(s_ctx->ctx == verbs);

		return;
	}

	s_ctx = (struct context *)malloc(sizeof(struct context));

	s_ctx->ctx = verbs;
	ASSERT(s_ctx->ctx);

	ASSERT(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
	ASSERT(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
	ASSERT(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
	ASSERTZ(ibv_req_notify_cq(s_ctx->cq, 0));

	ASSERTZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr) {
	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->send_cq = s_ctx->cq;
	qp_attr->recv_cq = s_ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = 10;
	qp_attr->cap.max_recv_wr = 10;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

void build_connection(struct rdma_cm_id *id) {
	struct connection *conn;
	struct ibv_qp_init_attr qp_attr;

	//build_context(id->verbs);
	build_qp_attr(&qp_attr);

	ASSERTZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));

	id->context = conn = (struct connection *)malloc(sizeof(struct connection));

	conn->id = id;
	conn->qp = id->qp;

	conn->connected = 0;

	register_memory(conn);
	post_receives(conn);
}

int on_connect_request(struct rdma_cm_id *id) {
	struct rdma_conn_param cm_params;

	pr_debug("received connection request");
	build_connection(id);
	build_params(&cm_params);
	ASSERTZ(rdma_accept(id, &cm_params));

	return 0;
}

int on_connection(struct rdma_cm_id *id) {
	struct connection *conn = (struct connection *)id->context;

	struct consumer_info_t *cli = (struct consumer_info_t *)malloc(sizeof(struct consumer_info_t));
	pr_info("consumer %p connected.\n", cli);
	cli->lstptr = NULL;
	cli->producer = NULL;
	conn->peer = (void *)cli;
	consumerlist_add(cli);

	conn->connected = 1;

	return 0;
}

int on_disconnect(struct rdma_cm_id *id) {
	struct connection *conn = (struct connection *)id->context;
	struct consumer_info_t *cli = conn->peer;
	pr_info("consumer (producer=%d)  %p disconnected.\n", !(cli->producer == NULL), cli);

	conn->connected = 0;

	destroy_connection(conn);
	return 0;
}

void process_event(struct rdma_cm_event *event) {

	pr_debug("process_event\n");

	if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
		ASSERTZ(on_connect_request(event->id));
	else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
		ASSERTZ(on_connection(event->id));
	else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
		ASSERTZ(on_disconnect(event->id));
	else
		BUG();
}
