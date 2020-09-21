// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/dout.h"
#include "librbd/cache/pwl/PWLImageDispatch.h"
#include "librbd/cache/pwl/ShutdownRequest.h"
#include "librbd/cache/WriteLogCache.h"
#include "librbd/ImageCtx.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/Utils.h"

#define dout_subsys ceph_subsys_rbd_pwl
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::PWLImageDispatch: " << this << " " \
                           << __func__ << ": "

namespace librbd {
namespace cache {
namespace pwl{

namespace {

void start_in_flight_io(io::AioCompletion* aio_comp) {
  if (!aio_comp->async_op.started()) {
    aio_comp->start_op();
  }
}

} // anonymous namespace

template <typename I>
void RWLImageDispatch<I>::shut_down(Context* on_finish) {
  ceph_assert(m_image_cache != nullptr);

  Context* ctx = new LambdaContext(
      [this, on_finish](int r) {
        m_image_cache = nullptr;
	on_finish->complete(r);
      });

  cache::pwl::ShutdownRequest<I> *req = cache::pwl::ShutdownRequest<I>::create(
    *m_image_ctx, m_image_cache, ctx);
  req->send();
}

template <typename I>
bool RWLImageDispatch<I>::read(
    io::AioCompletion* aio_comp, io::Extents &&image_extents,
    io::ReadResult &&read_result, int op_flags,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;

  aio_comp->set_request_count(1);
  aio_comp->read_result = std::move(read_result);
  uint64_t length = io::util::extents_length(image_extents);
  aio_comp->read_result.set_clip_length(length);

  auto *req_comp = new io::ReadResult::C_ImageReadRequest(
    aio_comp, image_extents);

  m_image_cache->aio_read(std::move(image_extents),
                                  &req_comp->bl, op_flags,
                                  req_comp);
  return true;
}

template <typename I>
bool RWLImageDispatch<I>::write(
    io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&bl,
    int op_flags, const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;

  aio_comp->set_request_count(1);
  io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
  m_image_cache->aio_write(std::move(image_extents),
                           std::move(bl), op_flags, req_comp);
  return true;
}

template <typename I>
bool RWLImageDispatch<I>::discard(
    io::AioCompletion* aio_comp, io::Extents &&image_extents,
    uint32_t discard_granularity_bytes, const ZTracer::Trace &parent_trace,
    uint64_t tid, std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;
  
  aio_comp->set_request_count(image_extents.size());
  for (auto &extent : image_extents) {
    io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
    m_image_cache->aio_discard(extent.first, extent.second,
                               discard_granularity_bytes,
                               req_comp);
  }
  return true;
}

template <typename I>
bool RWLImageDispatch<I>::write_same(
    io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&bl,
    int op_flags, const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;

  aio_comp->set_request_count(image_extents.size());
  for (auto &extent : image_extents) {
    io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
    m_image_cache->aio_writesame(extent.first, extent.second,
                                 std::move(bl), op_flags,
                                 req_comp);
  }
  return true;
}

template <typename I>
bool RWLImageDispatch<I>::compare_and_write(
    io::AioCompletion* aio_comp, io::Extents &&image_extents, bufferlist &&cmp_bl,
    bufferlist &&bl, uint64_t *mismatch_offset, int op_flags,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "image_extents=" << image_extents << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;

  aio_comp->set_request_count(1);
  io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
  m_image_cache->aio_compare_and_write(
    std::move(image_extents), std::move(cmp_bl), std::move(bl),
    mismatch_offset, op_flags, req_comp);
  return true;
}

template <typename I>
bool RWLImageDispatch<I>::flush(
    io::AioCompletion* aio_comp, io::FlushSource flush_source,
    const ZTracer::Trace &parent_trace, uint64_t tid,
    std::atomic<uint32_t>* image_dispatch_flags,
    io::DispatchResult* dispatch_result, Context* on_dispatched) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "tid=" << tid << dendl;

  start_in_flight_io(aio_comp);

  *dispatch_result = io::DISPATCH_RESULT_COMPLETE;

  aio_comp->set_request_count(1);
  io::C_AioRequest *req_comp = new io::C_AioRequest(aio_comp);
  m_image_cache->aio_flush(flush_source, req_comp);

  return true;
}

template <typename I>
void RWLImageDispatch<I>::handle_finished(int r, uint64_t tid) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "tid=" << tid << dendl;
}

} // namespace pwl
} // namespace io
} // namespace librbd

template class librbd::cache::pwl::RWLImageDispatch<librbd::ImageCtx>;
