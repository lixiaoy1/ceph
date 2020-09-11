// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/cache/ImageCache.h"
#include "librbd/cache/pwl/DiscardRequest.h"
#include "librbd/ImageCtx.h"
#include "common/dout.h"
#include "common/errno.h"
#include "librbd/asio/ContextWQ.h"
#include "librbd/cache/Types.h"


#define dout_subsys ceph_subsys_rbd_pwl
#undef dout_prefix
#define dout_prefix *_dout << "librbd::cache::ImageCache:: " \
                           << this << " " << __func__ << ": "


namespace librbd {
namespace cache {

template <typename I>
void ImageCache<I>::discard_cache(I &image_ctx, Context *ctx) {
  cache::pwl::DiscardRequest<I> *req = cache::pwl::DiscardRequest<I>::create(
    image_ctx, ctx);
  req->send();
}

} // namespace cache
} // namespace librbd

template class librbd::cache::ImageCache<librbd::ImageCtx>;
