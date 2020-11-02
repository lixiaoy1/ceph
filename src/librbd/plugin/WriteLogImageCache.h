// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_PLUGIN_WRITELOG_IMAGE_CACHE_H
#define CEPH_LIBRBD_PLUGIN_WRITELOG_IMAGE_CACHE_H

#include "librbd/plugin/Types.h"
#include "include/Context.h"

namespace librbd {

struct ImageCtx;

namespace plugin {

template <typename ImageCtxT>
class WriteLogImageCache : public Interface<ImageCtxT> {
public:
  WriteLogImageCache(CephContext* cct) : Interface<ImageCtxT>(cct) {
  }

  void init(ImageCtxT* image_ctx, Api<ImageCtxT>& api,
            HookPoints<ImageCtxT>* hook_points,
            Context* on_finish) override;

private:
  ImageCtxT* m_image_ctx = nullptr;

};

template <typename ImageCtxT>
class WriteLogImageCacheHook : public HookPointes<ImageCtxT> {
public:
  WriteLogImageCacheHook() : HookPointes() {
  }

  void start(ImageCtxT* image_ctx, Context* on_finish) override;
  void shutdown(ImageCtxT* image_ctx, Context* on_finish) override;

private:
  ImageCtxT* m_image_ctx = nullptr;
};

} // namespace plugin
} // namespace librbd

extern template class librbd::plugin::WriteLogImageCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_PLUGIN_WRITELOG_IMAGE_CACHE_H
