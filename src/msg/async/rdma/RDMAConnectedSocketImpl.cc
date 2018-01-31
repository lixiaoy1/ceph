// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSKY <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "RDMAStack.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << " RDMAConnectedSocketImpl "

RDMAConnectedSocketImpl::RDMAConnectedSocketImpl(CephContext *cct, Infiniband* ib, RDMADispatcher* s,
						 RDMAWorker *w)
  : cct(cct), error(0), infiniband(ib),
    dispatcher(s), worker(w), lock("RDMAConnectedSocketImpl::lock"),
    pending(false)
{
  int fd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
  assert(fd >= 0);

  if (cct->_conf->ms_async_rdma_cm)
    cmgr = new RDMAConnCM(cct, this, ib, s, w, info);
  else
    cmgr = new RDMAConnTCP(cct, this, ib, s, w, info); 
}

RDMAConnectedSocketImpl::~RDMAConnectedSocketImpl()
{
  ldout(cct, 20) << __func__ << " destruct." << dendl;
  cleanup();
  worker->remove_pending_conn(this);
  dispatcher->erase_qpn(my_msg.qpn);

  for (unsigned i=0; i < wc.size(); ++i) {
    dispatcher->post_chunk_to_pool(reinterpret_cast<Chunk*>(wc[i].wr_id));
  }
  for (unsigned i=0; i < buffers.size(); ++i) {
    dispatcher->post_chunk_to_pool(buffers[i]);
  }

  Mutex::Locker l(lock);
  if (notify_fd >= 0)
    ::close(notify_fd);
  error = ECONNRESET;
  cmgr->set_orphan();
  cmgr = nullptr;
}

void RDMAConnectedSocketImpl::pass_wc(std::vector<ibv_wc> &&v)
{
  Mutex::Locker l(lock);
  if (wc.empty())
    wc = std::move(v);
  else
    wc.insert(wc.end(), v.begin(), v.end());
  notify();
}

void RDMAConnectedSocketImpl::get_wc(std::vector<ibv_wc> &w)
{
  Mutex::Locker l(lock);
  if (wc.empty())
    return ;
  w.swap(wc);
}

int RDMAConnectedSocketImpl::try_connect(const entity_addr_t& peer_addr, const SocketOptions &opts) {
  return cmgr->try_connect(peer_addr, opts);
}

ssize_t RDMAConnectedSocketImpl::read(char* buf, size_t len)
{
  uint64_t i = 0;
  int r = ::read(notify_fd, &i, sizeof(i));
  ldout(cct, 20) << __func__ << " notify_fd : " << i << " in " << my_msg.qpn << " r = " << r << dendl;
  
  if (!active) {
    ldout(cct, 1) << __func__ << " when ib not active. len: " << len << dendl;
    return -EAGAIN;
  }
  
  if (0 == cmgr->connected) {
    ldout(cct, 1) << __func__ << " when ib not connected. len: " << len <<dendl;
    return -EAGAIN;
  }
  ssize_t read = 0;
  if (!buffers.empty())
    read = read_buffers(buf,len);

  std::vector<ibv_wc> cqe;
  get_wc(cqe);
  if (cqe.empty()) {
    if (!buffers.empty()) {
      notify();
    }
    if (read > 0) {
      return read;
    }
    if (error) {
      return -error;
    } else {
      return -EAGAIN;
    }
  }

  ldout(cct, 20) << __func__ << " poll queue got " << cqe.size() << " responses. QP: " << my_msg.qpn << dendl;
  for (size_t i = 0; i < cqe.size(); ++i) {
    ibv_wc* response = &cqe[i];
    assert(response->status == IBV_WC_SUCCESS);
    Chunk* chunk = reinterpret_cast<Chunk *>(response->wr_id);
    ldout(cct, 25) << __func__ << " chunk length: " << response->byte_len << " bytes." << chunk << dendl;
    chunk->prepare_read(response->byte_len);
    worker->perf_logger->inc(l_msgr_rdma_rx_bytes, response->byte_len);
    if (response->byte_len == 0) {
      dispatcher->perf_logger->inc(l_msgr_rdma_rx_fin);
      if (cmgr->connected) {
        error = ECONNRESET;
        ldout(cct, 20) << __func__ << " got remote close msg..." << dendl;
      }
      dispatcher->post_chunk_to_pool(chunk);
    } else {
      if (read == (ssize_t)len) {
        buffers.push_back(chunk);
        ldout(cct, 25) << __func__ << " buffers add a chunk: " << response->byte_len << dendl;
      } else if (read + response->byte_len > (ssize_t)len) {
        read += chunk->read(buf+read, (ssize_t)len-read);
        buffers.push_back(chunk);
        ldout(cct, 25) << __func__ << " buffers add a chunk: " << chunk->get_offset() << ":" << chunk->get_bound() << dendl;
      } else {
        read += chunk->read(buf+read, response->byte_len);
        dispatcher->post_chunk_to_pool(chunk);
      }
    }
  }

  worker->perf_logger->inc(l_msgr_rdma_rx_chunks, cqe.size());
  cmgr->post_read();

  if (!buffers.empty()) {
    notify();
  }

  if (read == 0 && error)
    return -error;
  return read == 0 ? -EAGAIN : read;
}

ssize_t RDMAConnectedSocketImpl::read_buffers(char* buf, size_t len)
{
  size_t read = 0, tmp = 0;
  auto c = buffers.begin();
  for (; c != buffers.end() ; ++c) {
    tmp = (*c)->read(buf+read, len-read);
    read += tmp;
    ldout(cct, 25) << __func__ << " this iter read: " << tmp << " bytes." << " offset: " << (*c)->get_offset() << " ,bound: " << (*c)->get_bound()  << ". Chunk:" << *c  << dendl;
    if ((*c)->over()) {
      dispatcher->post_chunk_to_pool(*c);
      ldout(cct, 25) << __func__ << " one chunk over." << dendl;
    }
    if (read == len) {
      break;
    }
  }

  if (c != buffers.end() && (*c)->over())
    ++c;
  buffers.erase(buffers.begin(), c);
  ldout(cct, 25) << __func__ << " got " << read  << " bytes, buffers size: " << buffers.size() << dendl;
  return read;
}

ssize_t RDMAConnectedSocketImpl::zero_copy_read(bufferptr &data)
{
  if (error)
    return -error;
  static const int MAX_COMPLETIONS = 16;
  ibv_wc wc[MAX_COMPLETIONS];
  ssize_t size = 0;

  ibv_wc*  response;
  Chunk* chunk;
  bool loaded = false;
  auto iter = buffers.begin();
  if (iter != buffers.end()) {
    chunk = *iter;
    // FIXME need to handle release
    // auto del = std::bind(&Chunk::post_srq, std::move(chunk), infiniband);
    buffers.erase(iter);
    loaded = true;
    size = chunk->bound;
  }

  std::vector<ibv_wc> cqe;
  get_wc(cqe);
  if (cqe.empty())
    return size == 0 ? -EAGAIN : size;

  ldout(cct, 20) << __func__ << " pool completion queue got " << cqe.size() << " responses."<< dendl;

  for (size_t i = 0; i < cqe.size(); ++i) {
    response = &wc[i];
    chunk = reinterpret_cast<Chunk*>(response->wr_id);
    chunk->prepare_read(response->byte_len);
    if (!loaded && i == 0) {
      // FIXME need to handle release
      // auto del = std::bind(&Chunk::post_srq, std::move(chunk), infiniband);
      size = chunk->bound;
      continue;
    }
    buffers.push_back(chunk);
    iter++;
  }

  if (size == 0)
    return -EAGAIN;
  return size;
}

ssize_t RDMAConnectedSocketImpl::send(bufferlist &bl, bool more)
{
  if (error) {
    if (!cmgr->active)
      return -EPIPE;
    return -error;
  }
  size_t bytes = bl.length();
  if (!bytes)
    return 0;
  {
    Mutex::Locker l(lock);
    pending_bl.claim_append(bl);
    if (!cmgr->connected) {
      ldout(cct, 20) << __func__ << " fake send to upper, QP: " << my_msg.qpn << dendl;
      return bytes;
    }
  }
  ldout(cct, 20) << __func__ << " QP: " << my_msg.qpn << dendl;
  ssize_t r = submit(more);
  if (r < 0 && r != -EAGAIN)
    return r;
  return bytes;
}

ssize_t RDMAConnectedSocketImpl::submit(bool more)
{
  if (error)
    return -error;
  Mutex::Locker l(lock);
  size_t bytes = pending_bl.length();
  ldout(cct, 20) << __func__ << " we need " << bytes << " bytes. iov size: "
                 << pending_bl.buffers().size() << dendl;
  if (!bytes)
    return 0;

  auto fill_tx_via_copy = [this](std::vector<Chunk*> &tx_buffers, unsigned bytes,
                                 std::list<bufferptr>::const_iterator &start,
                                 std::list<bufferptr>::const_iterator &end) -> unsigned {
    assert(start != end);
    auto chunk_idx = tx_buffers.size();
    int ret = worker->get_reged_mem(this, tx_buffers, bytes);
    if (ret == 0) {
      ldout(cct, 1) << __func__ << " no enough buffers in worker " << worker << dendl;
      worker->perf_logger->inc(l_msgr_rdma_tx_no_mem);
      return 0;
    }

    unsigned total_copied = 0;
    Chunk *current_chunk = tx_buffers[chunk_idx];
    while (start != end) {
      const uintptr_t addr = reinterpret_cast<const uintptr_t>(start->c_str());
      unsigned copied = 0;
      while (copied < start->length()) {
        uint32_t r = current_chunk->write((char*)addr+copied, start->length() - copied);
        copied += r;
        total_copied += r;
        bytes -= r;
        if (current_chunk->full()){
          if (++chunk_idx == tx_buffers.size())
            return total_copied;
          current_chunk = tx_buffers[chunk_idx];
        }
      }
      ++start;
    }
    assert(bytes == 0);
    return total_copied;
  };

  std::vector<Chunk*> tx_buffers;
  std::list<bufferptr>::const_iterator it = pending_bl.buffers().begin();
  std::list<bufferptr>::const_iterator copy_it = it;
  unsigned total = 0;
  unsigned need_reserve_bytes = 0;
  while (it != pending_bl.buffers().end()) {
    if (infiniband->is_tx_buffer(it->raw_c_str())) {
      if (need_reserve_bytes) {
        unsigned copied = fill_tx_via_copy(tx_buffers, need_reserve_bytes, copy_it, it);
        total += copied;
        if (copied < need_reserve_bytes)
          goto sending;
        need_reserve_bytes = 0;
      }
      assert(copy_it == it);
      tx_buffers.push_back(infiniband->get_tx_chunk_by_buffer(it->raw_c_str()));
      total += it->length();
      ++copy_it;
    } else {
      need_reserve_bytes += it->length();
    }
    ++it;
  }
  if (need_reserve_bytes)
    total += fill_tx_via_copy(tx_buffers, need_reserve_bytes, copy_it, it);

 sending:
  if (total == 0)
    return -EAGAIN;
  assert(total <= pending_bl.length());
  bufferlist swapped;
  if (total < pending_bl.length()) {
    worker->perf_logger->inc(l_msgr_rdma_tx_parital_mem);
    pending_bl.splice(total, pending_bl.length()-total, &swapped);
    pending_bl.swap(swapped);
  } else {
    pending_bl.clear();
  }

  ldout(cct, 20) << __func__ << " left bytes: " << pending_bl.length() << " in buffers "
                 << pending_bl.buffers().size() << " tx chunks " << tx_buffers.size() << dendl;

  int r = post_work_request(tx_buffers);
  if (r < 0)
    return r;

  ldout(cct, 20) << __func__ << " finished sending " << bytes << " bytes." << dendl;
  return pending_bl.length() ? -EAGAIN : 0;
}

int RDMAConnectedSocketImpl::post_work_request(std::vector<Chunk*> &tx_buffers)
{
  ldout(cct, 20) << __func__ << " QP: " << my_msg.qpn << " " << tx_buffers[0] << dendl;
  vector<Chunk*>::iterator current_buffer = tx_buffers.begin();
  ibv_sge isge[tx_buffers.size()];
  uint32_t current_sge = 0;
  ibv_send_wr iswr[tx_buffers.size()];
  uint32_t current_swr = 0;
  ibv_send_wr* pre_wr = NULL;
  uint32_t num = 0; 

  memset(iswr, 0, sizeof(iswr));
  memset(isge, 0, sizeof(isge));
 
  while (current_buffer != tx_buffers.end()) {
    isge[current_sge].addr = reinterpret_cast<uint64_t>((*current_buffer)->buffer);
    isge[current_sge].length = (*current_buffer)->get_offset();
    isge[current_sge].lkey = (*current_buffer)->mr->lkey;
    ldout(cct, 25) << __func__ << " sending buffer: " << *current_buffer << " length: " << isge[current_sge].length  << dendl;

    iswr[current_swr].wr_id = reinterpret_cast<uint64_t>(*current_buffer);
    iswr[current_swr].next = NULL;
    iswr[current_swr].sg_list = &isge[current_sge];
    iswr[current_swr].num_sge = 1;
    iswr[current_swr].opcode = IBV_WR_SEND;
    iswr[current_swr].send_flags = IBV_SEND_SIGNALED;
    /*if (isge[current_sge].length < infiniband->max_inline_data) {
      iswr[current_swr].send_flags = IBV_SEND_INLINE;
      ldout(cct, 20) << __func__ << " send_inline." << dendl;
      }*/

    num++;
    worker->perf_logger->inc(l_msgr_rdma_tx_bytes, isge[current_sge].length);
    if (pre_wr)
      pre_wr->next = &iswr[current_swr];
    pre_wr = &iswr[current_swr];
    ++current_sge;
    ++current_swr;
    ++current_buffer;
  }

  ibv_send_wr *bad_tx_work_request;
  if (ibv_post_send(qp->get_qp(), iswr, &bad_tx_work_request)) {
    ldout(cct, 1) << __func__ << " failed to send data"
                  << " (most probably should be peer not ready): "
                  << cpp_strerror(errno) << dendl;
    worker->perf_logger->inc(l_msgr_rdma_tx_failed);
    return -errno;
  }
  qp->add_tx_wr(num);
  worker->perf_logger->inc(l_msgr_rdma_tx_chunks, tx_buffers.size());
  ldout(cct, 20) << __func__ << " qp state is " << Infiniband::qp_state_string(qp->get_state()) << dendl;
  return 0;
}

void RDMAConnectedSocketImpl::fin() {
  ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = reinterpret_cast<uint64_t>(qp);
  wr.num_sge = 0;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  ibv_send_wr* bad_tx_work_request;
  if (ibv_post_send(qp->get_qp(), &wr, &bad_tx_work_request)) {
    ldout(cct, 1) << __func__ << " failed to send message="
                  << " ibv_post_send failed(most probably should be peer not ready): "
                  << cpp_strerror(errno) << dendl;
    worker->perf_logger->inc(l_msgr_rdma_tx_failed);
    return ;
  }
  qp->add_tx_wr(1);
}

void RDMAConnectedSocketImpl::cleanup() {
  if (con_handler && tcp_fd >= 0) {
    (static_cast<C_handle_connection*>(con_handler))->close();
    worker->center.submit_to(worker->center.get_id(), [this]() {
      worker->center.delete_file_event(tcp_fd, EVENT_READABLE);
    }, false);
    delete con_handler;
    con_handler = nullptr;
  }
}

void RDMAConnectedSocketImpl::notify()
{
  // note: notify_fd is an event fd (man eventfd)
  // write argument must be a 64bit integer
  uint64_t i = 1;

  assert(sizeof(i) == write(notify_fd, &i, sizeof(i)));
}

void RDMAConnectedSocketImpl::shutdown()
{
  if (!error)
    fin();
  error = ECONNRESET;
  active = false;
}

void RDMAConnectedSocketImpl::close()
{
  if (!error)
    fin();
  error = ECONNRESET;
  active = false;
}

void RDMAConnectedSocketImpl::fault()
{
  ldout(cct, 1) << __func__ << " tcp fd " << tcp_fd << dendl;
  /*if (qp) {
    qp->to_dead();
    qp = NULL;
    }*/
  error = ECONNRESET;
  connected = 1;
  notify();
}

void RDMAConnectedSocketImpl::set_accept_fd(int sd)
{
  tcp_fd = sd;
  is_server = true;
  worker->center.submit_to(worker->center.get_id(), [this]() {
			   worker->center.create_file_event(tcp_fd, EVENT_READABLE, con_handler);
			   }, true);
}
