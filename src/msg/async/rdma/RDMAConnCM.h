#ifndef CEPH_MSG_RDMA_CONNECTED_SOCKET_CM_H
#define CEPH_MSG_RDMA_CONNECTED_SOCKET_CM_H

struct RDMAConnCMInfo {
  struct rdma_cm_id *id;
  uint32_t remote_qpn;
};

class RDMAConnCM : public RDMAConnMgr {
  class C_handle_cm_event : public EventCallback {
    RDMAConnCM *cmgr;
  public:
    C_handle_cm_event(RDMAConnCM *cmgr): cmgr(cmgr) {}
    void do_request(int fd) {
      cmgr->handle_cm_event();
    }
  };

public:
  RDMAConnCM(CephContext *cct, RDMAConnectedSocketImpl *sock,
             Infiniband *ib, RDMADispatcher* s, RDMAWorker *w,
             void *info);
  virtual ~RDMAConnCM();

  virtual int try_connect(const entity_addr_t&, const SocketOptions &opt) override;
  virtual void shutdown() override;

  virtual void cleanup() override;

  virtual ostream &print(ostream &o) const override;

  virtual ibv_qp *qp_create(ibv_pd *pd, ibv_qp_init_attr *qpia) override;
  //virtual void qp_to_err() override;
  virtual void qp_destroy() override;

private:
  void handle_cm_event();
  int alloc_resources();
  int create_channel();
  void cm_established(uint32_t qpn);

  struct rdma_cm_id *id;

  rdma_event_channel *channel;

  EventCallbackRef con_handler;
};


class RDMAServerConnCM : public RDMAServerSocketImpl {
  struct rdma_event_channel *channel;
  struct rdma_cm_id *listen_id;

 public:
  RDMAServerConnCM(CephContext *cct, Infiniband *ib, RDMADispatcher *s, RDMAWorker *w, entity_addr_t& a);
  ~RDMAServerConnCM();

  int listen(entity_addr_t &sa, const SocketOptions &opt);
  virtual int accept(ConnectedSocket *s, const SocketOptions &opts, entity_addr_t *out, Worker *w) override;
  virtual void abort_accept() override;
  virtual int fd() const override;
  void handle_cm_event();
};

#endif
