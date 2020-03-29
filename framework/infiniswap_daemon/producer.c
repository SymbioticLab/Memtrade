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

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/queue.h>
#include <infiniband/verbs.h>

#include "utils.h"
#include "rcommon.h"

static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_id *id);
static int on_disconnect(struct rdma_cm_id *id);
static void process_event(struct rdma_cm_event *event);
static void build_context(struct ibv_context *verbs);

static struct context *s_ctx = NULL;

struct coordinator_conn_t;
struct consumer_list_t;
struct producer_t;

struct {
    char coord_ip[200];
    int coord_port;
    char producer[200];
    int port;
    uint64_t cluster_size;
    // TODO: this is duplicated with the rrs in the producer run function
    struct producer_t *rrs;
} globals;

// TODO: are ip+port duplicated? see globals.coord_ip+coord_port
struct coordinator_conn_t {
    char ip[200];
    int port;
    uint64_t rdmakey;
    struct rdma_event_channel *rchannel;
    struct rdma_cm_id *rid;
} _coord;

struct producer_t {
    struct rdma_cm_id *rid;
    struct rdma_event_channel *rchannel;

    struct ibv_mr *mr;
    uint64_t base_addr;
    uint64_t rdma_key;
};

struct client_info_t {
    // pointer to active client list
    struct consumer_list_t *lstptr;
};

struct consumer_list_t {
    struct client_info_t *client;
    struct consumer_list_t *next;
    struct consumer_list_t *prev;
} * _clilst_head, *_clilst_tail;

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

/************************************
  consumer list management
***********************************/

static void clilst_init() {
    _clilst_head = (struct consumer_list_t *)malloc(sizeof(struct consumer_list_t));
    _clilst_tail = (struct consumer_list_t *)malloc(sizeof(struct consumer_list_t));
    _clilst_head->client = NULL;
    _clilst_tail->client = NULL;
    _clilst_head->next = _clilst_tail;
    _clilst_head->prev = NULL;
    _clilst_tail->next = NULL;
    _clilst_tail->prev = _clilst_head;
}

static void clilst_add(struct client_info_t *cli) {
    pr_debug("clilst adding client %p link %p head %p tail %p", cli, cli->lstptr, _clilst_head, _clilst_tail);

    struct consumer_list_t *newcli = (struct consumer_list_t *)malloc(sizeof(struct consumer_list_t));
    cli->lstptr = newcli;
    newcli->client = cli;
    newcli->next = _clilst_head->next;
    newcli->prev = _clilst_head;
    newcli->next->prev = newcli;
    _clilst_head->next = newcli;
}

static void clilst_remove(struct client_info_t *cli) {
    ASSERT(cli);

    pr_debug("clilst remove client %p link %p head %p tail %p", cli, cli->lstptr, _clilst_head, _clilst_tail);

    struct consumer_list_t *oldcli = cli->lstptr;

    ASSERT(oldcli != _clilst_head);
    ASSERT(oldcli != _clilst_tail);

    oldcli->prev->next = oldcli->next;
    oldcli->next->prev = oldcli->prev;

    free(oldcli->client);
    free(oldcli);
}

static void clilst_destroy() {
    pr_debug("clilst destroy");
    struct consumer_list_t *cli = _clilst_head->next;
    struct consumer_list_t *oldcli = NULL;

    while (cli != _clilst_tail) {
        oldcli = cli;
        cli = cli->next;
        clilst_remove(oldcli->client);
    }

    pr_debug("freeing head %p and tail %p", _clilst_head, _clilst_tail);
    free(_clilst_head);
    free(_clilst_tail);
}

/***********************************
  export memory
 ***********************************/

static void producer_export_memory(struct producer_t *rrs, uint64_t num_clusters, size_t cluster_size) {
    void *ptr = NULL;
    size_t size = cluster_size * num_clusters;

    pr_info("exporting memory... num_clusters:%lu cluster_size:%lu total:%lu", num_clusters, cluster_size, (size_t)num_clusters * cluster_size);
    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED) {
        pr_err("error allocating memory region memory");
        ASSERT(0 && "mmap failed");
    }

    rrs->base_addr = (uint64_t)ptr;

    pr_debug("producer offering memory at %p %lu, size %lu", ptr, rrs->base_addr, size);

    struct ibv_mr *mr = NULL;
    ASSERT(mr = ibv_reg_mr(
        s_ctx->pd,
        ptr,
        size,
        IBV_ACCESS_LOCAL_WRITE |
        IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ)
    );

    rrs->mr = mr;
    rrs->rdma_key = mr->rkey;
    pr_info("done exporting memory!");
}

/******************************************
  coordinator
 ******************************************/

void send_msg_producer_add(struct connection *conn) {
    pr_debug("sending MSG_PRODUCER_ADD");

    conn->send_msg->type = MSG_PRODUCER_ADD;
    strcpy(conn->send_msg->data.ip, globals.producer);
    conn->send_msg->data.port = globals.port;
    conn->send_msg->data.nslabs = globals.cluster_size; /*RDMA_PRODUCER_NSLABS;*/
    conn->send_msg->data.addr = (void *)globals.rrs->base_addr;
    conn->send_msg->data.rdmakey = globals.rrs->rdma_key;
    send_message(conn);

    // TODO: wait for notificaiton from controller that
    // producer was successfully added?
}

void send_msg_producer_rem(struct connection *conn) {
    pr_debug("sending MSG_PRODUCER_REM");

    conn->send_msg->type = MSG_PRODUCER_REM;
    strcpy(conn->send_msg->data.ip, globals.producer);
    send_message(conn);
}

void on_recv_done(struct connection *conn) {
    pr_debug("received done! %p", conn->recv_msg->data.addr);
}

void on_completion_client(struct ibv_wc *wc, enum ibv_wc_opcode opcode, int msgtype) {
    struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

    if (wc->status != IBV_WC_SUCCESS) {
        pr_err("RDMA request failed with status %d: %s", wc->status, ibv_wc_status_str(wc->status));
    }
    ASSERT(wc->status == IBV_WC_SUCCESS);

    if (wc->opcode & IBV_WC_RECV) {

        ASSERT(opcode == IBV_WC_RECV);
        ASSERT(conn->recv_msg->type == msgtype);

        switch (conn->recv_msg->type) {
            case MSG_DONE:
                on_recv_done(conn);
                break;
            default:
                ASSERT(0);
        }
    }
    else {
        if (opcode == IBV_WC_SEND) {
            ASSERT(conn->send_msg->type == msgtype);
            pr_debug("send completed successfully %p msg %d.", conn, conn->send_msg->type);
        }
        else {
            ASSERT(opcode == IBV_WC_RDMA_READ || opcode == IBV_WC_RDMA_WRITE);

            pr_debug("RDMA READ/WRITE completed successfully");
        }
    }
}

void register_memory_client(struct connection *conn) {
    conn->send_msg = malloc(sizeof(struct message));
    conn->recv_msg = malloc(sizeof(struct message));

    //conn->rdma_local_region = malloc(RDMA_BUFFER_SIZE);

    ASSERT(conn->send_mr = ibv_reg_mr(s_ctx->pd, conn->send_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE));

    ASSERT(conn->recv_mr = ibv_reg_mr(s_ctx->pd, conn->recv_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE));

    /*
  ASSERT(conn->rdma_local_mr = ibv_reg_mr(s_ctx->pd,
        conn->rdma_local_region,
        RDMA_BUFFER_SIZE,
        IBV_ACCESS_LOCAL_WRITE));
*/
}

void build_context_client(struct ibv_context *verbs) {
    if (s_ctx) {
        ASSERT(s_ctx->ctx == verbs);

        return;
    }

    s_ctx = (struct context *)malloc(sizeof(struct context));

    s_ctx->ctx = verbs;

    ASSERT(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
    ASSERT(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
    ASSERT(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary*/
}

void build_qp_attr_client(struct ibv_qp_init_attr *qp_attr) {
    memset(qp_attr, 0, sizeof(*qp_attr));

    qp_attr->send_cq = s_ctx->cq;
    qp_attr->recv_cq = s_ctx->cq;
    qp_attr->qp_type = IBV_QPT_RC;

    qp_attr->cap.max_send_wr = 10;
    qp_attr->cap.max_recv_wr = 10;
    qp_attr->cap.max_send_sge = 1;
    qp_attr->cap.max_recv_sge = 1;
}

void build_connection_client(struct rdma_cm_id *id) {
    struct connection *conn;
    struct ibv_qp_init_attr qp_attr;

    build_context_client(id->verbs);
    build_qp_attr_client(&qp_attr);

    ASSERTZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));

    id->context = conn = (struct connection *)malloc(sizeof(struct connection));

    conn->id = id;
    conn->qp = id->qp;
    conn->peer = NULL;

    conn->connected = 0;

    register_memory_client(conn);
    post_receives(conn);
}

int on_addr_resolved(struct rdma_cm_id *id) {
    pr_debug("address resolved.");

    build_connection_client(id);
    ASSERTZ(rdma_resolve_route(id, TIMEOUT_IN_MS));

    return 0;
}

int on_route_resolved(struct rdma_cm_id *id) {
    struct rdma_conn_param cm_params;

    pr_debug("route resolved.\n");
    build_params(&cm_params);
    ASSERTZ(rdma_connect(id, &cm_params));

    return 0;
}

int on_connection_client(struct rdma_cm_id *id) {
    pr_debug("on connection");

    struct connection *conn = (struct connection *)id->context;
    conn->connected = 1;

    send_msg_producer_add(conn);
    return 1;
}

int on_disconnect_client(struct rdma_cm_id *id) {
    pr_debug("disconnected.");

    destroy_connection((struct connection *)id->context);
    return 1; /* exit event loop */
}

int on_event(struct rdma_cm_event *event) {
    int r = 0;

    pr_debug("on_event client");

    if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)
        r = on_addr_resolved(event->id);
    else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)
        r = on_route_resolved(event->id);
    else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
        r = on_connection_client(event->id);
    else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
        r = on_disconnect_client(event->id);
    else {
        pr_err("on_event: %d status: %d\n", event->event, event->status);
        ASSERT(0 && "Unknown event: is producer running?");
    }

    return r;
}

///****************************************

void coordinator_connect(struct coordinator_conn_t *rrc) {
    struct rdma_cm_event *event = NULL;

    // initiate connection
    while (rdma_get_cm_event(rrc->rchannel, &event) == 0) {
        struct rdma_cm_event event_copy;

        memcpy(&event_copy, event, sizeof(*event));
        rdma_ack_cm_event(event);

        if (on_event(&event_copy))
            break;
    }

    pr_info("client connected!");
}

void coordinator_create(struct coordinator_conn_t *rrc) {
    char portstr[10];
    struct addrinfo *addr;

    sprintf(portstr, "%d", rrc->port);
    pr_info("client connection to producer %s on port %s", rrc->ip, portstr);
    ASSERTZ(getaddrinfo(rrc->ip, portstr, NULL, &addr));

    ASSERT(rrc->rchannel = rdma_create_event_channel());
    ASSERTZ(rdma_create_id(rrc->rchannel, &(rrc->rid), NULL, RDMA_PS_TCP));
    ASSERTZ(rdma_resolve_addr(rrc->rid, NULL, addr->ai_addr, TIMEOUT_IN_MS));

    freeaddrinfo(addr);
}

void coordinator_destroy(struct coordinator_conn_t *rrc) {
    rdma_destroy_event_channel(rrc->rchannel);
}

void connect2coordinator(char *coord_ip, int coord_port) {
    pr_debug("Connecting to coordinator on %s:%d\n", coord_ip, coord_port);

    strcpy(_coord.ip, coord_ip);
    _coord.port = coord_port;

    coordinator_create(&_coord);
    coordinator_connect(&_coord);
}

/******************************************
  producer
 ******************************************/

static void producer_create(struct producer_t *rrs) {
    pr_debug("creating producer");
    struct sockaddr_in addr;
    uint16_t port = globals.port;

    rrs->rchannel = NULL;
    rrs->rid = NULL;
    rrs->base_addr = 0;
    rrs->rdma_key = 0;

    globals.rrs = rrs;

    clilst_init();

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // address 0 - listen across all RDMA devices
    addr.sin_port = htons(port);
    inet_aton(globals.producer, &addr.sin_addr);

    ASSERT(rrs->rchannel = rdma_create_event_channel());
    ASSERTZ(rdma_create_id(rrs->rchannel, &rrs->rid, NULL, RDMA_PS_TCP));
    ASSERTZ(rdma_bind_addr(rrs->rid, (struct sockaddr *)&addr));

    build_context(rrs->rid->verbs);
    producer_export_memory(rrs, globals.cluster_size/*RDMA_PRODUCER_NSLABS*/, RDMA_PRODUCER_SLAB_SIZE);

    ASSERTZ(rdma_listen(rrs->rid, 10)); /* backlog=10 is arbitrary */

    port = ntohs(rdma_get_src_port(rrs->rid));
    pr_info("producer %s", globals.producer);
    pr_info("listening on port %d.", port);
    pr_info("slab size %ld.", globals.cluster_size);

    connect2coordinator(globals.coord_ip, globals.coord_port);
}

static void producer_destroy(struct producer_t *rrs) {
    pr_debug("destroying producer");

    rdma_destroy_id(rrs->rid);
    rdma_destroy_event_channel(rrs->rchannel);

    clilst_destroy();
}

static void producer_run() {
    struct rdma_cm_event *event = NULL;

    struct producer_t *rrs = (struct producer_t *)malloc(sizeof(struct producer_t));

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    pr_info("Pinning producer to core %d", PIN_PRODUCER_CORE);
    CPU_SET(PIN_PRODUCER_CORE, &cpuset);
    ASSERTZ(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset));

    producer_create(rrs);

    while ((rdma_get_cm_event(rrs->rchannel, &event) == 0) && !aborted) {
        struct rdma_cm_event event_copy;

        pr_debug("producer loop");

        memcpy(&event_copy, event, sizeof(*event));
        rdma_ack_cm_event(event);

        process_event(&event_copy);
    }

    send_msg_producer_rem(_coord.rid->context);
    coordinator_destroy(&_coord);

    producer_destroy(rrs);
}

/*********************************************
 *********************************************/

void usage() {
    printf("Usage ./producer [-i producer-ip] [-p producer-port] [-c coordinator-ip] [-r coordinator-port]\n");
    printf("Default producer address is %s\n", RDMA_PRODUCER_IP);
    printf("Default port is %d\n", RDMA_PRODUCER_PORT);
    printf("Default coordinator ip address is %s\n", COORDINATOR_IP);
    printf("Default coordinator port is %d\n", COORDINATOR_PORT);
    printf("\n");
}

int main(int argc, char **argv) {
    int opt;
    register_signal_handler();

    strcpy(globals.producer, RDMA_PRODUCER_IP);
    globals.port = RDMA_PRODUCER_PORT;
    strcpy(globals.coord_ip, COORDINATOR_IP);
    globals.coord_port = COORDINATOR_PORT;
    globals.cluster_size = RDMA_PRODUCER_NSLABS;
    while ((opt = getopt(argc, argv, "hs:p:c:r:n:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case 'i':
            strcpy(globals.producer, optarg);
            break;
        case 'p':
            globals.port = atoi(optarg);
            break;
        case 'c':
            strcpy(globals.coord_ip, optarg);
            break;
        case 'r':
            globals.coord_port = atol(optarg);
            break;
        case 'n':
            globals.cluster_size = atoi(optarg);
            break;
        }
    }

    pr_info("producer started");
    producer_run();
    pr_info("producer stopped");
    return 0;
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
        case MSG_DONE:
            pr_info("received message DONE!");
            break;
        default:
            ASSERT(0);
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
    pr_info("Pinning completion poller to core %d", PIN_PRODUCER_POLLER_CORE);
    CPU_SET(PIN_PRODUCER_POLLER_CORE, &cpuset);
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

    struct client_info_t *cli = (struct client_info_t *)malloc(sizeof(struct client_info_t));
    cli->lstptr = NULL;
    clilst_add(cli);
    conn->peer = (void *)cli;

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
    pr_info("connected.\n");
    struct connection *conn = (struct connection *)id->context;

    conn->connected = 1;

    return 0;
}

int on_disconnect(struct rdma_cm_id *id) {
    pr_info("disconnected.\n");
    struct connection *conn = (struct connection *)id->context;

    conn->connected = 0;

    clilst_remove(conn->peer);

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
