#ifndef CEPH_MSG_RDMA_CONN_MGR
#define CEPH_MSG_RDMA_CONN_MGR

class RDMAConnMgr {
  friend class RDMAConnectedSocketImpl;

 private:
  atomic_t refs;

 protected:
  CephContext *cct;
  RDMAConnectedSocketImpl *socket;
  Infiniband* infiniband;
  RDMADispatcher* dispatcher;
  RDMAWorker* worker;

  Device *ibdev = nullptr;
  int ibport = -1;
  QueuePair *qp = nullptr;

  bool is_server;
  bool active;// qp is active ?
  int connected;

 public:
  RDMAConnMgr(CephContext *cct, RDMAConnectedSocketImpl *sock,
              Infiniband* ib, RDMADispatcher* s, RDMAWorker *w, int r);
  virtual ~RDMAConnMgr() { };

  virtual void put();

  virtual ostream &print(ostream &out) const = 0;

  virtual void cleanup() = 0;
  virtual int try_connect(const entity_addr_t&, const SocketOptions &opt) = 0;
  virtual int activate();

  int create_queue_pair();

  virtual ibv_qp *qp_create(ibv_pd *pd, ibv_qp_init_attr *qpia) = 0;
  virtual void qp_to_err() = 0;
  virtual void qp_destroy() = 0;

  void post_read();

  virtual void shutdown();
  void close();

  virtual void set_orphan() { socket = nullptr; put(); };
};

#endif

