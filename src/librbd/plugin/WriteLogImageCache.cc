// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ceph_ver.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/PluginRegistry.h"
#include "librbd/ImageCtx.h"
#include "librbd/cache/WriteLogImageDispatch.h"
#include "librbd/cache/pwl/InitRequest.h"
#include "librbd/plugin/WriteLogImageCache.h"

extern "C" {

const char *__ceph_plugin_version() {
  return CEPH_GIT_NICE_VER;
}

int __ceph_plugin_init(CephContext *cct, const std::string& type,
                       const std::string& name) {
  auto plugin_registry = cct->get_plugin_registry();
  return plugin_registry->add(
    type, name, new librbd::plugin::WriteLogImageCache<librbd::ImageCtx>(cct));
}

} // extern "C"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::plugin::WriteLogImageCache: " \
                           << this << " " << __func__ << ": "

namespace librbd {
namespace plugin {

template <typename I>
void WriteLogImageCache<I>::init(I* image_ctx, Api<I>& api, HookPoints<I>* hook_points,
                                 Context* on_finish) {
  m_image_ctx = image_ctx;
  bool rwl_enabled = m_image_ctx->config.template get_val<bool>(
    "rbd_rwl_enabled");
  if (!rwl_enabled || !m_image_ctx->data_ctx.is_valid()) {
    on_finish->complete(0);
    return;
  }

  auto cct = m_image_ctx->cct;
  ldout(cct, 5) << dendl;

  WriteLogImageCacheHook<I> cache_hook;
  *hook_points = cache_hook;

  on_finish->complete(0);
}

template <typename I>
void WriteLogImageCacheHook<I>::start(I* image_ctx, Context* on_finish) {
  cache::pwl::InitRequest<I> *req = cache::pwl::InitRequest<I>::create(
    image_ctx, on_finish);
  req->send();
}

template <typename I>
void WriteLogImageCacheHook<I>::shutdown(I* image_ctx, Context* on_finish) {
  image_ctx.io_image_dispatcher->shut_down_dispatch(
    io::IMAGE_DISPATCH_LAYER_WRITEBACK_CACHE, on_finish);
}

} // namespace plugin
} // namespace librbd

template class librbd::plugin::WriteLogImageCache<librbd::ImageCtx>;
