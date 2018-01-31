// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Mellanox <amirva@mellanox.com>
 *
 * Author: Amir Vadai <amirva@mellanox.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

#include "Infiniband.h"
#include "RDMAStack.h"
#include "RDMAConnCM.h"
#include "Device.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << " RDMAConnCM "

/* ref is initialized to 2:
 * - LAST_WQE_REACHED and qp is destroyed
 * - set_orphan() is called (socket is destroyed)
 */
RDMAConnCM::RDMAConnCM(CephContext *cct, RDMAConnectedSocketImpl *sock,
		       Infiniband *ib, RDMADispatcher* s, RDMAWorker *w,
		       void *_info)
  : RDMAConnMgr(cct, sock, ib, s, w, 2), con_handler(new C_handle_cm_event(this))
{
  int err;

  channel = rdma_create_event_channel();
  assert(channel);

  worker->center.submit_to(worker->center.get_id(), [this]() {
			   worker->center.create_file_event(channel->fd, EVENT_READABLE, con_handler);
			   }, false);

  if (_info) {
    RDMAConnCMInfo *info = (RDMAConnCMInfo *)_info;

    is_server = true;

    id = info->id;
    id->context = socket;

    err = rdma_migrate_id(id, channel);
    assert(!err);

    socket->remote_qpn = info->remote_qpn;

    err = alloc_resources();
    assert(!err);

    activate();

    ldout(cct, 1) << __func__ << " (server) " << *this << dendl;
    return;
  }

  err = rdma_create_id(channel, &id, NULL, RDMA_PS_TCP);
  assert(!err);

  ldout(cct, 1) << __func__ << " " << *this << dendl;
}

RDMAConnCM::~RDMAConnCM()
{
  ldout(cct, 1) << __func__ << " called" << dendl;

  rdma_destroy_id(id);
  rdma_destroy_event_channel(channel);
}

int RDMAConnCM::try_connect(const entity_addr_t &peer_addr,
			    const SocketOptions &opts)
{
  int err;
  char buf[NI_MAXHOST] = { 0 };
  char serv[NI_MAXSERV] = { 0 };
  struct rdma_addrinfo hints = { 0 };
  struct rdma_addrinfo *rai;
 
  hints.ai_port_space = RDMA_PS_TCP;

  getnameinfo(peer_addr.get_sockaddr(), peer_addr.get_sockaddr_len(),
	      buf, sizeof(buf),
	      serv, sizeof(serv),
	      NI_NUMERICHOST | NI_NUMERICSERV);

  ldout(cct, 1) << __func__ << " dest: " << buf << ":" << peer_addr.get_port() << dendl;

  ldout(cct, 1) << __func__ << " nonblock:" << opts.nonblock << ", nodelay:"
    << opts.nodelay << ", rbuf_size: " << opts.rcbuf_size << dendl;

  err = rdma_getaddrinfo(buf, serv, &hints, &rai);
  assert(!err);

#define TIMEOUT 2000
  err = rdma_resolve_addr(id, rai->ai_src_addr, rai->ai_dst_addr, TIMEOUT);
  assert(!err);

  rdma_freeaddrinfo(rai);

  return 0;
}

void RDMAConnCM::handle_cm_event()
{
  struct rdma_cm_event *event;
  int err;

  ldout(cct, 1) << __func__ << dendl;

  err = rdma_get_cm_event(id->channel, &event);
  assert(!err);

  ldout(cct, 1) << __func__ << " " << *this << " event: " << rdma_event_str(event->event) << dendl;

  switch (event->event) {
  case RDMA_CM_EVENT_ADDR_RESOLVED:
    err = alloc_resources();
    assert(!err);

    err = rdma_resolve_route(id, TIMEOUT);
    assert(!err);
    break;

  case RDMA_CM_EVENT_ROUTE_RESOLVED:
    err = rdma_connect(id, NULL);
    assert(!err);
    break;

  case RDMA_CM_EVENT_ESTABLISHED:
    cm_established(event->param.conn.qp_num);
    break;

  case RDMA_CM_EVENT_DISCONNECTED:
    if (!socket)
      break;

    if (socket->error) {
      socket->error = ECONNRESET;
      close();
    } else {
      rdma_disconnect(id);
      connected = 0;
      socket->abort_connection();
      return;
    }

    rdma_disconnect(id);
    break;

  case RDMA_CM_EVENT_TIMEWAIT_EXIT:
    cleanup();
    rdma_ack_cm_event(event);
    put();

    return;

  case RDMA_CM_EVENT_REJECTED:
    socket->fault();
    break;

  case RDMA_CM_EVENT_ADDR_ERROR:
  case RDMA_CM_EVENT_ROUTE_ERROR:
  case RDMA_CM_EVENT_CONNECT_ERROR:
  case RDMA_CM_EVENT_UNREACHABLE:
    ldout(cct, 1) << __func__  << " " << *this << " event: " << rdma_event_str(event->event) <<
      ", error: " << event->status << dendl;
    dispatcher->perf_logger->inc(l_msgr_rdma_handshake_errors);
    socket->fault();
    break;

  case RDMA_CM_EVENT_DEVICE_REMOVAL:
    assert(0);

  default:
    assert(0);
    break;
  }

  rdma_ack_cm_event(event);
}

int RDMAConnCM::alloc_resources()
{
  ibdev = infiniband->get_device(id->verbs);
  ibport = id->port_num;

  ldout(cct, 1) << __func__ << " Device: " << *ibdev << " port: " << ibport << dendl;

  ibdev->init(ibport);

  create_queue_pair();
  socket->register_qp(qp);

  return 0;
}

void RDMAConnCM::cm_established(uint32_t qpn)
{
  if (!is_server) {
    // client finished connecting to server
    socket->remote_qpn = qpn;
    ldout(cct, 20) << __func__ << " connection established: " << *this << dendl;
    if (!connected) {
      int r = activate();
      assert(!r);
    }
    socket->notify();

    return;
  }

  // server finished getting connected from client
  connected = 1;
  cleanup();
  socket->submit(false);
  socket->notify();
}

void RDMAConnCM::shutdown()
{
  ldout(cct, 1) << __func__ << dendl;
  if (socket->error)
    rdma_disconnect(id);

  RDMAConnMgr::shutdown();
}

void RDMAConnCM::cleanup()
{
  if (con_handler) {
    worker->center.submit_to(worker->center.get_id(), [this]() {
			     worker->center.delete_file_event(channel->fd, EVENT_READABLE);
			     }, false);
    delete con_handler;
    con_handler = nullptr;
  }
};

struct ibv_qp *RDMAConnCM::qp_create(ibv_pd *pd, ibv_qp_init_attr *qpia)
{
  int err;

  err = rdma_create_qp(id, pd, qpia);
  if (err) {
    lderr(cct) << __func__ << " failed to create queue pair. err: " <<
      err << " errno: " << cpp_strerror(errno) << dendl;
    return nullptr;
  }

  return id->qp;
}

void RDMAConnCM::qp_to_err()
{
  rdma_disconnect(id);
}

void RDMAConnCM::qp_destroy()
{
    rdma_destroy_qp(id);
}

ostream &RDMAConnCM::print(ostream &o) const
{
  return o << "CM: {" <<
    " id: " << id <<
    " }";
}

RDMAServerConnCM::RDMAServerConnCM(CephContext *cct, Infiniband *ib, RDMADispatcher *s, RDMAWorker *w, entity_addr_t& a)
  : RDMAServerSocketImpl(cct, ib, s, w, a), channel(nullptr), listen_id(nullptr)
{
  int err;

  channel = rdma_create_event_channel();
  assert(channel);

  err = rdma_create_id(channel, &listen_id, this, RDMA_PS_TCP);
  assert(!err);
}

RDMAServerConnCM::~RDMAServerConnCM()
{
  rdma_destroy_id(listen_id);
  rdma_destroy_event_channel(channel);
}

int RDMAServerConnCM::listen(entity_addr_t &sa, const SocketOptions &opt)
{
  int err;

  err = rdma_bind_addr(listen_id, (struct sockaddr *)sa.get_sockaddr());
  if (err) {
    err = -errno;
    ldout(cct, 10) << __func__ << " unable to bind to " << sa.get_sockaddr()
                   << " on port " << sa.get_port() << ": " << cpp_strerror(errno) << dendl;
    goto err;
  }

  err = rdma_listen(listen_id, 128);
  if (err) {
    err = -errno;
    lderr(cct) << __func__ << " unable to listen on " << sa << ": " << cpp_strerror(errno) << dendl;
    goto err;
  }

  ldout(cct, 1) << __func__ << " bind to " << sa.get_sockaddr() << " on port " << sa.get_port()  << dendl;

  return 0;

err:
  return err;
}

int RDMAServerConnCM::accept(ConnectedSocket *sock, const SocketOptions &opt, entity_addr_t *out, Worker *w)
{
  struct rdma_cm_event *event;
  RDMAConnectedSocketImpl *socket;
  int ret;

  ldout(cct, 15) << __func__ << dendl;

  struct pollfd pfd = {
    .fd = channel->fd,
    .events = POLLIN,
  };

  // rmda_get_cm_event() is blocking even if fd is nonblocking - need to
  // poll() before calling it.
  ret = ::poll(&pfd, 1, 0);
  assert(ret >= 0);
  if (!ret)
    return -EAGAIN;

  ret = rdma_get_cm_event(channel, &event);
  if (ret) {
    lderr(cct) << __func__ << " error getting cm event: " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }

  ldout(cct, 1) << __func__ << " CM event: " << rdma_event_str(event->event) << dendl;

  assert(event->event == RDMA_CM_EVENT_CONNECT_REQUEST);

  struct rdma_cm_id *new_id = event->id;
  struct rdma_conn_param *peer = &event->param.conn;
  struct rdma_conn_param conn_param;

  rdma_ack_cm_event(event);

  RDMAConnCMInfo conn_info = {
    .id = new_id,
    .remote_qpn = peer->qp_num,
  };
  socket = new RDMAConnectedSocketImpl(cct, infiniband, dispatcher, dynamic_cast<RDMAWorker*>(w), &conn_info);
  assert(socket);

  memset(&conn_param, 0, sizeof(conn_param));
  conn_param.qp_num = socket->local_qpn;
  conn_param.srq = 1;

  ret = rdma_accept(new_id, &conn_param);
  assert(!ret);

  ldout(cct, 20) << __func__ << " accepted a new QP" << dendl;

  std::unique_ptr<RDMAConnectedSocketImpl> csi(socket);
  *sock = ConnectedSocket(std::move(csi));
  if (out) {
    struct sockaddr *addr = &new_id->route.addr.dst_addr;
    out->set_sockaddr(addr);
  }

  return 0;
}

void RDMAServerConnCM::abort_accept()
{
  rdma_destroy_id(listen_id);
  rdma_destroy_event_channel(channel);
}

int RDMAServerConnCM::fd() const
{
  return channel->fd;
}
