// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_UTILS_H
#define CEPH_LIBRBD_IO_UTILS_H

#include "include/int_types.h"
#include "include/buffer_fwd.h"
#include "include/rados/rados_types.hpp"
#include "common/zipkin_trace.h"
#include "librbd/io/Types.h"
#include <map>

class ObjectExtent;

namespace neorados { struct Op; }

namespace librbd {

struct ImageCtx;

namespace io {
class AioCompletion;
namespace util {

void apply_op_flags(uint32_t op_flags, uint32_t flags, neorados::Op* op);

bool assemble_write_same_extent(const LightweightObjectExtent &object_extent,
                                const ceph::bufferlist& data,
                                ceph::bufferlist *ws_data,
                                bool force_write);

template <typename ImageCtxT = librbd::ImageCtx>
void read_parent(ImageCtxT *image_ctx, uint64_t object_no, const Extents &extents,
                 librados::snap_t snap_id, const ZTracer::Trace &trace,
                 ceph::bufferlist* data, Context* on_finish);

template <typename ImageCtxT = librbd::ImageCtx>
int clip_request(ImageCtxT *image_ctx, Extents &image_extents);

void set_read_clip_length(Extents &image_extents,
                          AioCompletion *aio_comp);

template <typename ImageCtxT = librbd::ImageCtx>
bool finish_request_early_if_readonly(ImageCtxT *image_ctx, AioCompletion *aio_comp);

bool finish_request_early_if_nodata(Extents &image_extents,
                                     AioCompletion *aio_comp);

} // namespace util
} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_UTILS_H
