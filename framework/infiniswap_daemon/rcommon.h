#ifndef __RCOMMON_H__ 
#define __RCOMMON_H__

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "utils.h"

#define MAX_PRODUCER 128
#define TIMEOUT_IN_MS   500  /* ms */

enum {
  M_WRITE,
  M_READ
};

enum {
  DISCONNECTED,
  CONNECTED
};


struct message {
  enum {
    MSG_PRODUCER_ADD, 
    MSG_PRODUCER_REM,
    MSG_SLAB_ADD, 
    MSG_SLAB_REM,
    MSG_DONE,
    MSG_SLAB_ADD_PARTIAL,
    MSG_DONE_SLAB_ADD,
    NUM_MSG_TYPE
  } type;

  struct {
    struct ibv_mr mr;
    void *addr;
    size_t size;
    char ip[200];
    int port;
    int id;
    unsigned int rdmakey;
    int nslabs;
    //void *memaddr;
  } data;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t cq_poller_thread;
};


struct connection {
  struct rdma_cm_id *id;
  struct ibv_qp *qp;

  void *peer;
  volatile int connected;

  struct ibv_mr *recv_mr;
  struct ibv_mr *send_mr;
  struct message *recv_msg;
  struct message *send_msg;

  struct ibv_mr *rdma_local_mr;
  char *rdma_local_region;

  unsigned long local_addr;
  unsigned long remote_addr;
  unsigned int lkey;
  unsigned int rkey;
};

void build_params(struct rdma_conn_param *params);
void destroy_connection(struct connection *conn);
void post_receives(struct connection *conn);
void send_message(struct connection *conn);
void do_rdma_op(struct connection *conn, size_t size, int mode);

#endif  // __RCOMMON_H__
