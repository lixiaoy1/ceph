
RDMAConnMgr::RDMAConnMgr(CephContext *cct, RDMAConnectedSocketImpl *sock,
                         Infiniband* ib, RDMADispatcher* s, RDMAWorker *w,
                         int r)
  : refs(r), cct(cct), socket(sock), infiniband(ib), dispatcher(s), worker(w),
    is_server(false), active(false),
    connected(0)
{
}

void RDMAConnMgr::put()
{
  int r = refs.dec();

  ldout(cct, 1) << __func__ << " " << *this << ": ref-- = " << r << dendl;

  if (!r)
    delete this;
}

nt RDMAConnMgr::create_queue_pair()
{
  qp = infiniband->create_queue_pair(ibport, dispatcher->get_tx_cq(), dispatcher->get_rx_cq(), IBV_QPT_RC, this);

  socket->local_qpn = qp->get_local_qp_number();

  return !qp;
}

int RDMAConnMgr::activate()
{
  if (!is_server) {
    connected = 1; //indicate successfully
    ldout(cct, 20) << __func__ << " handle fake send, wake it up. " << *socket << dendl;
    socket->submit(false);
  }
  active = true;

  return 0;
}

void RDMAConnMgr::post_read()
{
  if (!is_server || connected)
    return;

  ldout(cct, 20) << __func__ << " we do not need last handshake, " << *socket << dendl;
  connected = 1; //if so, we don't need the last handshake
  cleanup();
  socket->submit(false);
}

void RDMAConnMgr::shutdown()
{
  if (!socket->error)
    socket->fin();
  socket->error = ECONNRESET;
  active = false;
}

void RDMAConnMgr::close()
{
  shutdown();
}


