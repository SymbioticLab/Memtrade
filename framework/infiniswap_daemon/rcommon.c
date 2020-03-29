#include "rcommon.h"


void build_params(struct rdma_conn_param *params) {
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}


void destroy_connection(struct connection *conn) {
  rdma_destroy_qp(conn->id);

  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->recv_mr);
//  ibv_dereg_mr(conn->rdma_local_mr);

  free(conn->send_msg);
  free(conn->recv_msg);
//  free(conn->rdma_local_region);

  rdma_destroy_id(conn->id);

  free(conn);
}


void do_rdma_op(struct connection *conn, size_t size, int mode) {
    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    if (mode == M_WRITE) {
      pr_debug("writing message to remote memory...remote_addr=%lx size=%lu", conn->remote_addr, size);
    } else {
      pr_debug("reading message from remote memory...remote_addr=%lx size=%lu", conn->remote_addr, size);
      pr_debug("into local address %lx %u", conn->local_addr, conn->lkey);
    }

    memset(&sge, 0, sizeof(sge));
    sge.addr = conn->local_addr;
    sge.length = size;
    sge.lkey = conn->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)conn;
    wr.opcode = (mode == M_WRITE) ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = conn->remote_addr;
    wr.wr.rdma.rkey = conn->rkey;

    ASSERTZ(ibv_post_send(conn->qp, &wr, &bad_wr));

    pr_debug("done: do_rdma_op %d\n", mode);
}

void post_receives(struct connection *conn) {
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)conn->recv_msg;
  sge.length = sizeof(struct message);
  sge.lkey = conn->recv_mr->lkey;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  ASSERTZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}


void send_message(struct connection *conn) {
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)conn->send_msg;
  sge.length = sizeof(struct message);
  sge.lkey = conn->send_mr->lkey;

  while (!conn->connected);

  ASSERTZ(ibv_post_send(conn->qp, &wr, &bad_wr));
}

