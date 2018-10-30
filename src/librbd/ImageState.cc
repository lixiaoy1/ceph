// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/ImageState.h"
#include "include/rbd/librbd.hpp"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Cond.h"
#include "common/WorkQueue.h"
#include "librbd/ImageCtx.h"
#include "librbd/Utils.h"
#include "librbd/image/CloseRequest.h"
#include "librbd/image/OpenRequest.h"
#include "librbd/image/RefreshRequest.h"
#include "librbd/image/SetSnapRequest.h"
#include "librbd/cache/ImageCache.h"
#include "librbd/cache/ImageWriteback.h"
#include "librbd/cache/ReplicatedWriteLog.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::ImageState: " << this << " "

namespace librbd {

using util::create_async_context_callback;
using util::create_context_callback;

class ImageUpdateWatchers {
public:

  explicit ImageUpdateWatchers(CephContext *cct) : m_cct(cct),
    m_lock(util::unique_lock_name("librbd::ImageUpdateWatchers::m_lock", this)) {
  }

  ~ImageUpdateWatchers() {
    ceph_assert(m_watchers.empty());
    ceph_assert(m_in_flight.empty());
    ceph_assert(m_pending_unregister.empty());
    ceph_assert(m_on_shut_down_finish == nullptr);

    destroy_work_queue();
  }

  void flush(Context *on_finish) {
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << dendl;
    {
      Mutex::Locker locker(m_lock);
      if (!m_in_flight.empty()) {
	Context *ctx = new FunctionContext(
	  [this, on_finish](int r) {
	    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
	                     << ": completing flush" << dendl;
	    on_finish->complete(r);
	  });
	m_work_queue->queue(ctx, 0);
	return;
      }
    }
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
		     << ": completing flush" << dendl;
    on_finish->complete(0);
  }

  void shut_down(Context *on_finish) {
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << dendl;
    {
      Mutex::Locker locker(m_lock);
      ceph_assert(m_on_shut_down_finish == nullptr);
      m_watchers.clear();
      if (!m_in_flight.empty()) {
	m_on_shut_down_finish = on_finish;
	return;
      }
    }
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
		     << ": completing shut down" << dendl;
    on_finish->complete(0);
  }

  void register_watcher(UpdateWatchCtx *watcher, uint64_t *handle) {
    ldout(m_cct, 20) << __func__ << ": watcher=" << watcher << dendl;

    Mutex::Locker locker(m_lock);
    ceph_assert(m_on_shut_down_finish == nullptr);

    create_work_queue();

    *handle = m_next_handle++;
    m_watchers.insert(std::make_pair(*handle, watcher));
  }

  void unregister_watcher(uint64_t handle, Context *on_finish) {
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << ": handle="
		     << handle << dendl;
    int r = 0;
    {
      Mutex::Locker locker(m_lock);
      auto it = m_watchers.find(handle);
      if (it == m_watchers.end()) {
	r = -ENOENT;
      } else {
	if (m_in_flight.find(handle) != m_in_flight.end()) {
	  ceph_assert(m_pending_unregister.find(handle) == m_pending_unregister.end());
	  m_pending_unregister[handle] = on_finish;
	  on_finish = nullptr;
	}
	m_watchers.erase(it);
      }
    }

    if (on_finish) {
      ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
		       << ": completing unregister" << dendl;
      on_finish->complete(r);
    }
  }

  void notify() {
    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << dendl;

    Mutex::Locker locker(m_lock);
    for (auto it : m_watchers) {
      send_notify(it.first, it.second);
    }
  }

  void send_notify(uint64_t handle, UpdateWatchCtx *watcher) {
    ceph_assert(m_lock.is_locked());

    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << ": handle="
		     << handle << ", watcher=" << watcher << dendl;

    m_in_flight.insert(handle);

    Context *ctx = new FunctionContext(
      [this, handle, watcher](int r) {
	handle_notify(handle, watcher);
      });

    m_work_queue->queue(ctx, 0);
  }

  void handle_notify(uint64_t handle, UpdateWatchCtx *watcher) {

    ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__ << ": handle="
		     << handle << ", watcher=" << watcher << dendl;

    watcher->handle_notify();

    Context *on_unregister_finish = nullptr;
    Context *on_shut_down_finish = nullptr;

    {
      Mutex::Locker locker(m_lock);

      auto in_flight_it = m_in_flight.find(handle);
      ceph_assert(in_flight_it != m_in_flight.end());
      m_in_flight.erase(in_flight_it);

      // If there is no more in flight notifications for this watcher
      // and it is pending unregister, complete it now.
      if (m_in_flight.find(handle) == m_in_flight.end()) {
	auto it = m_pending_unregister.find(handle);
	if (it != m_pending_unregister.end()) {
	  on_unregister_finish = it->second;
	  m_pending_unregister.erase(it);
	}
      }

      if (m_in_flight.empty()) {
	ceph_assert(m_pending_unregister.empty());
	if (m_on_shut_down_finish != nullptr) {
	  std::swap(m_on_shut_down_finish, on_shut_down_finish);
	}
      }
    }

    if (on_unregister_finish != nullptr) {
      ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
		       << ": completing unregister" << dendl;
      on_unregister_finish->complete(0);
    }

    if (on_shut_down_finish != nullptr) {
      ldout(m_cct, 20) << "ImageUpdateWatchers::" << __func__
		       << ": completing shut down" << dendl;
      on_shut_down_finish->complete(0);
    }
  }

private:
  class ThreadPoolSingleton : public ThreadPool {
  public:
    explicit ThreadPoolSingleton(CephContext *cct)
      : ThreadPool(cct, "librbd::ImageUpdateWatchers::thread_pool", "tp_librbd",
		   1) {
      start();
    }
    ~ThreadPoolSingleton() override {
      stop();
    }
  };

  CephContext *m_cct;
  Mutex m_lock;
  ContextWQ *m_work_queue = nullptr;
  std::map<uint64_t, UpdateWatchCtx*> m_watchers;
  uint64_t m_next_handle = 0;
  std::multiset<uint64_t> m_in_flight;
  std::map<uint64_t, Context*> m_pending_unregister;
  Context *m_on_shut_down_finish = nullptr;

  void create_work_queue() {
    if (m_work_queue != nullptr) {
      return;
    }
    auto& thread_pool = m_cct->lookup_or_create_singleton_object<
      ThreadPoolSingleton>("librbd::ImageUpdateWatchers::thread_pool",
			   false, m_cct);
    m_work_queue = new ContextWQ("librbd::ImageUpdateWatchers::op_work_queue",
				 m_cct->_conf.get_val<uint64_t>("rbd_op_thread_timeout"),
				 &thread_pool);
  }

  void destroy_work_queue() {
    if (m_work_queue == nullptr) {
      return;
    }
    m_work_queue->drain();
    delete m_work_queue;
  }
};

template <typename I>
ImageState<I>::ImageState(I *image_ctx)
  : m_image_ctx(image_ctx), m_state(STATE_UNINITIALIZED),
    m_lock(util::unique_lock_name("librbd::ImageState::m_lock", this)),
    m_last_refresh(0), m_refresh_seq(0),
    m_update_watchers(new ImageUpdateWatchers(image_ctx->cct)) {
}

template <typename I>
ImageState<I>::~ImageState() {
  ceph_assert(m_state == STATE_UNINITIALIZED || m_state == STATE_CLOSED);
  delete m_update_watchers;
}

template <typename I>
int ImageState<I>::open(uint64_t flags) {
  C_SaferCond ctx;
  open(flags, &ctx);

  int r = ctx.wait();
  if (r < 0) {
    delete m_image_ctx;
  }
  return r;
}

template <typename I>
void ImageState<I>::open(uint64_t flags, Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_lock.Lock();
  ceph_assert(m_state == STATE_UNINITIALIZED);
  m_open_flags = flags;

  Action action(ACTION_TYPE_OPEN);
  action.refresh_seq = m_refresh_seq;

  execute_action_unlock(action, on_finish);
}

template <typename I>
int ImageState<I>::close() {
  C_SaferCond ctx;
  close(&ctx);

  int r = ctx.wait();
  delete m_image_ctx;
  return r;
}

template <typename I>
void ImageState<I>::close(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_lock.Lock();
  ceph_assert(!is_closed());

  Action action(ACTION_TYPE_CLOSE);
  action.refresh_seq = m_refresh_seq;
  execute_action_unlock(action, on_finish);
}

template <typename I>
void ImageState<I>::handle_update_notification() {
  Mutex::Locker locker(m_lock);
  ++m_refresh_seq;

  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << ": refresh_seq = " << m_refresh_seq << ", "
		 << "last_refresh = " << m_last_refresh << dendl;

  if (m_state == STATE_OPEN) {
    m_update_watchers->notify();
  }
}

template <typename I>
bool ImageState<I>::is_refresh_required() const {
  Mutex::Locker locker(m_lock);
  return (m_last_refresh != m_refresh_seq || find_pending_refresh() != nullptr);
}

template <typename I>
int ImageState<I>::refresh() {
  C_SaferCond refresh_ctx;
  refresh(&refresh_ctx);
  return refresh_ctx.wait();
}

template <typename I>
void ImageState<I>::refresh(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_lock.Lock();
  if (is_closed()) {
    m_lock.Unlock();
    on_finish->complete(-ESHUTDOWN);
    return;
  }

  Action action(ACTION_TYPE_REFRESH);
  action.refresh_seq = m_refresh_seq;
  execute_action_unlock(action, on_finish);
}

template <typename I>
int ImageState<I>::refresh_if_required() {
  C_SaferCond ctx;
  {
    m_lock.Lock();
    Action action(ACTION_TYPE_REFRESH);
    action.refresh_seq = m_refresh_seq;

    auto refresh_action = find_pending_refresh();
    if (refresh_action != nullptr) {
      // if a refresh is in-flight, delay until it is finished
      action = *refresh_action;
    } else if (m_last_refresh == m_refresh_seq) {
      m_lock.Unlock();
      return 0;
    } else if (is_closed()) {
      m_lock.Unlock();
      return -ESHUTDOWN;
    }

    execute_action_unlock(action, &ctx);
  }

  return ctx.wait();
}

template <typename I>
const typename ImageState<I>::Action *
ImageState<I>::find_pending_refresh() const {
  ceph_assert(m_lock.is_locked());

  auto it = std::find_if(m_actions_contexts.rbegin(),
                         m_actions_contexts.rend(),
                         [](const ActionContexts& action_contexts) {
      return (action_contexts.first == ACTION_TYPE_REFRESH);
    });
  if (it != m_actions_contexts.rend()) {
    return &it->first;
  }
  return nullptr;
}

template <typename I>
void ImageState<I>::snap_set(uint64_t snap_id, Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << ": snap_id=" << snap_id << dendl;

  Action action(ACTION_TYPE_SET_SNAP);
  action.snap_id = snap_id;

  m_lock.Lock();
  execute_action_unlock(action, on_finish);
}

template <typename I>
void ImageState<I>::prepare_lock(Context *on_ready) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << __func__ << dendl;

  m_lock.Lock();
  if (is_closed()) {
    m_lock.Unlock();
    on_ready->complete(-ESHUTDOWN);
    return;
  }

  Action action(ACTION_TYPE_LOCK);
  action.on_ready = on_ready;
  execute_action_unlock(action, nullptr);
}

template <typename I>
void ImageState<I>::handle_prepare_lock_complete() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << __func__ << dendl;

  m_lock.Lock();
  if (m_state != STATE_PREPARING_LOCK) {
    m_lock.Unlock();
    return;
  }

  complete_action_unlock(STATE_OPEN, 0);
}

template <typename I>
int ImageState<I>::register_update_watcher(UpdateWatchCtx *watcher,
					 uint64_t *handle) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_update_watchers->register_watcher(watcher, handle);

  ldout(cct, 20) << __func__ << ": handle=" << *handle << dendl;
  return 0;
}

template <typename I>
int ImageState<I>::unregister_update_watcher(uint64_t handle) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << ": handle=" << handle << dendl;

  C_SaferCond ctx;
  m_update_watchers->unregister_watcher(handle, &ctx);
  return ctx.wait();
}

template <typename I>
void ImageState<I>::flush_update_watchers(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_update_watchers->flush(on_finish);
}

template <typename I>
void ImageState<I>::shut_down_update_watchers(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  m_update_watchers->shut_down(on_finish);
}

template <typename I>
bool ImageState<I>::is_transition_state() const {
  switch (m_state) {
  case STATE_UNINITIALIZED:
  case STATE_OPEN:
  case STATE_CLOSED:
    return false;
  case STATE_OPENING:
  case STATE_CLOSING:
  case STATE_REFRESHING:
  case STATE_SETTING_SNAP:
  case STATE_PREPARING_LOCK:
    break;
  }
  return true;
}

template <typename I>
bool ImageState<I>::is_closed() const {
  ceph_assert(m_lock.is_locked());

  return ((m_state == STATE_CLOSED) ||
          (!m_actions_contexts.empty() &&
           m_actions_contexts.back().first.action_type == ACTION_TYPE_CLOSE));
}

template <typename I>
void ImageState<I>::append_context(const Action &action, Context *context) {
  ceph_assert(m_lock.is_locked());

  ActionContexts *action_contexts = nullptr;
  for (auto &action_ctxs : m_actions_contexts) {
    if (action == action_ctxs.first) {
      action_contexts = &action_ctxs;
      break;
    }
  }

  if (action_contexts == nullptr) {
    m_actions_contexts.push_back({action, {}});
    action_contexts = &m_actions_contexts.back();
  }

  if (context != nullptr) {
    action_contexts->second.push_back(context);
  }
}

template <typename I>
void ImageState<I>::execute_next_action_unlock() {
  ceph_assert(m_lock.is_locked());
  ceph_assert(!m_actions_contexts.empty());
  switch (m_actions_contexts.front().first.action_type) {
  case ACTION_TYPE_OPEN:
    send_open_unlock();
    return;
  case ACTION_TYPE_CLOSE:
    send_close_unlock();
    return;
  case ACTION_TYPE_REFRESH:
    send_refresh_unlock();
    return;
  case ACTION_TYPE_SET_SNAP:
    send_set_snap_unlock();
    return;
  case ACTION_TYPE_LOCK:
    send_prepare_lock_unlock();
    return;
  }
  ceph_abort();
}

template <typename I>
void ImageState<I>::execute_action_unlock(const Action &action,
                                          Context *on_finish) {
  ceph_assert(m_lock.is_locked());

  append_context(action, on_finish);
  if (!is_transition_state()) {
    execute_next_action_unlock();
  } else {
    m_lock.Unlock();
  }
}

template <typename I>
void ImageState<I>::complete_action_unlock(State next_state, int r) {
  ceph_assert(m_lock.is_locked());
  ceph_assert(!m_actions_contexts.empty());

  ActionContexts action_contexts(std::move(m_actions_contexts.front()));
  m_actions_contexts.pop_front();

  m_state = next_state;
  m_lock.Unlock();

  for (auto ctx : action_contexts.second) {
    ctx->complete(r);
  }

  if (next_state != STATE_UNINITIALIZED && next_state != STATE_CLOSED) {
    m_lock.Lock();
    if (!is_transition_state() && !m_actions_contexts.empty()) {
      execute_next_action_unlock();
    } else {
      m_lock.Unlock();
    }
  }
}

template <typename I>
void ImageState<I>::send_open_unlock() {
  ceph_assert(m_lock.is_locked());
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_state = STATE_OPENING;

  Context *ctx = create_async_context_callback(
    *m_image_ctx, create_context_callback<
      ImageState<I>, &ImageState<I>::handle_open>(this));
  image::OpenRequest<I> *req = image::OpenRequest<I>::create(
    m_image_ctx, m_open_flags, ctx);

  m_lock.Unlock();
  req->send();
}

template <typename I>
void ImageState<I>::handle_open(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to open image: " << cpp_strerror(r) << dendl;
  }

  m_lock.Lock();
  complete_action_unlock(r < 0 ? STATE_UNINITIALIZED : STATE_OPEN, r);
}

template <typename I>
void ImageState<I>::send_close_unlock() {
  ceph_assert(m_lock.is_locked());
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_state = STATE_CLOSING;

  Context *ctx = create_context_callback<
    ImageState<I>, &ImageState<I>::handle_close>(this);
  image::CloseRequest<I> *req = image::CloseRequest<I>::create(
    m_image_ctx, ctx);

  m_lock.Unlock();
  req->send();
}

template <typename I>
void ImageState<I>::handle_close(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "error occurred while closing image: " << cpp_strerror(r)
               << dendl;
  }

  m_lock.Lock();
  complete_action_unlock(STATE_CLOSED, r);
}

template <typename I>
void ImageState<I>::send_refresh_unlock() {
  ceph_assert(m_lock.is_locked());
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_state = STATE_REFRESHING;
  ceph_assert(!m_actions_contexts.empty());
  auto &action_context = m_actions_contexts.front().first;
  ceph_assert(action_context.action_type == ACTION_TYPE_REFRESH);

  Context *ctx = create_async_context_callback(
    *m_image_ctx, create_context_callback<
      ImageState<I>, &ImageState<I>::handle_refresh>(this));
  image::RefreshRequest<I> *req = image::RefreshRequest<I>::create(
    *m_image_ctx, false, false, ctx);

  m_lock.Unlock();
  req->send();
}

template <typename I>
void ImageState<I>::handle_refresh(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  m_lock.Lock();
  ceph_assert(!m_actions_contexts.empty());

  ActionContexts &action_contexts(m_actions_contexts.front());
  ceph_assert(action_contexts.first.action_type == ACTION_TYPE_REFRESH);
  ceph_assert(m_last_refresh <= action_contexts.first.refresh_seq);

  if (r == -ERESTART) {
    ldout(cct, 5) << "incomplete refresh: not updating sequence" << dendl;
    r = 0;
  } else {
    m_last_refresh = action_contexts.first.refresh_seq;
  }

  complete_action_unlock(STATE_OPEN, r);
}

template <typename I>
void ImageState<I>::send_set_snap_unlock() {
  ceph_assert(m_lock.is_locked());

  m_state = STATE_SETTING_SNAP;

  ceph_assert(!m_actions_contexts.empty());
  ActionContexts &action_contexts(m_actions_contexts.front());
  ceph_assert(action_contexts.first.action_type == ACTION_TYPE_SET_SNAP);

  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": "
                 << "snap_id=" << action_contexts.first.snap_id << dendl;

  Context *ctx = create_async_context_callback(
    *m_image_ctx, create_context_callback<
      ImageState<I>, &ImageState<I>::handle_set_snap>(this));
  image::SetSnapRequest<I> *req = image::SetSnapRequest<I>::create(
    *m_image_ctx, action_contexts.first.snap_id, ctx);

  m_lock.Unlock();
  req->send();
}

template <typename I>
void ImageState<I>::handle_set_snap(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << " r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to set snapshot: " << cpp_strerror(r) << dendl;
  }

  m_lock.Lock();
  complete_action_unlock(STATE_OPEN, r);
}

template <typename I>
void ImageState<I>::send_prepare_lock_unlock() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  ceph_assert(m_lock.is_locked());
  m_state = STATE_PREPARING_LOCK;

  ceph_assert(!m_actions_contexts.empty());
  ActionContexts &action_contexts(m_actions_contexts.front());
  ceph_assert(action_contexts.first.action_type == ACTION_TYPE_LOCK);

  Context *on_ready = action_contexts.first.on_ready;
  m_lock.Unlock();

  if (on_ready == nullptr) {
    complete_action_unlock(STATE_OPEN, 0);
    return;
  }

  // wake up the lock handler now that its safe to proceed
  on_ready->complete(0);
}

template <typename I>
void ImageState<I>::init_image_cache(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  // {
  //   /* This must (?) be called during exclusive lock acquire */
  //   Mutex::Locker locker(m_lock);
  //   ceph_assert(STATE_PREPARING_LOCK == m_state);
  // }

  bool image_cache_state_modified = false;
  bool cache_exists = m_image_ctx->image_cache_state.present;
  bool cache_desired = m_image_ctx->test_features(RBD_FEATURE_IMAGE_CACHE) && m_image_ctx->rwl_enabled;

  /* Avoid creating a cache in certain cases */
  cache_desired &= !m_image_ctx->read_only;
  cache_desired &= !m_image_ctx->test_features(RBD_FEATURE_MIGRATING);
  cache_desired &= !m_image_ctx->old_format;

  if (!(cache_exists || cache_desired)) {
    ldout(cct, 20) << __func__ << " Image cache not used" << dendl;
    on_finish->complete(0);
    return;
  }

#if defined(WITH_RWL)
  /* Find the layer with the RWL */
  cls::rbd::ReplicatedWriteLogSpec *rwl_spec = nullptr;
  for (auto spec_it = m_image_ctx->image_cache_state.layers.begin();
       spec_it != m_image_ctx->image_cache_state.layers.end();) {
    if (cls::rbd::get_image_cache_spec_type(*spec_it) == cls::rbd::IMAGE_CACHE_TYPE_RWL) {
      /* Only one layer of RWL is supported */
      ldout(cct, 20) << __func__ << " Found RWL in image cache state: "
		     << boost::get<cls::rbd::ReplicatedWriteLogSpec>(*spec_it) << dendl;
      if (rwl_spec == nullptr) {
	rwl_spec = &boost::get<cls::rbd::ReplicatedWriteLogSpec>(*spec_it);
      } else {
	ldout(cct, 20) << __func__ << " Removing extra RWL config spec" << dendl;
	spec_it = m_image_ctx->image_cache_state.layers.erase(spec_it);
	continue;
      }
    }
    ++spec_it;
  }

  /* Record RWL config for image if no RWL config is recorded,
     or there was no cache left behind by the last opener. */
  if (cache_desired && (!rwl_spec || !m_image_ctx->image_cache_state.present)) {
    auto rwl_conf = cls::rbd::ReplicatedWriteLogSpec("<this node>",
						     m_image_ctx->rwl_path,
						     m_image_ctx->rwl_size,
						     m_image_ctx->rwl_invalidate_on_flush);
    if (!rwl_spec) {
      ldout(cct, 20) << __func__ << " Initializing RWL config in state" << dendl;
      m_image_ctx->image_cache_state.layers.push_back({rwl_conf});
    } else {
      ldout(cct, 20) << __func__ << " Adjusting RWL config in state" << dendl;
      *rwl_spec = rwl_conf;
    }
    image_cache_state_modified = true;
  } else {
    /* Use the saved config if it's there */
    ldout(cct, 20) << __func__ << " Using RWL config from state" << dendl;
    ceph_assert(cls::rbd::get_image_cache_spec_type(*rwl_spec) == cls::rbd::IMAGE_CACHE_TYPE_RWL);
    m_image_ctx->rwl_path = rwl_spec->path;
    m_image_ctx->rwl_size = rwl_spec->size;
    m_image_ctx->rwl_invalidate_on_flush = rwl_spec->invalidate_on_flush;
    /* TODO: If these are different, shut down the existing cache and re-init with new params */
  }

  ceph_assert(!m_image_ctx->old_format);
  if (!cache_desired) {
    ldout(cct, 4) << "Existing image cache must be initialized" << dendl;
  } else {
    ldout(cct, 20) << __func__ << " Image configured to use image cache" << dendl;
  }

  m_image_ctx->m_rwl_spec = rwl_spec;
  m_image_ctx->image_cache_layers.clear();
  cache::ImageCache<I> *layer = nullptr;
  /* Don't create ImageWriteback unless there's at least one ImageCache enabled */
  layer = new cache::ImageWriteback<I>(*m_image_ctx);
  m_image_ctx->image_cache = layer;
  /* An ImageCache below RWL would be created here */
  ldout(cct, 4) << this << " " << __func__ << " RWL enabled" << dendl;
  layer = new cache::ReplicatedWriteLog<I>(*m_image_ctx, layer);
  m_image_ctx->image_cache = layer;
  m_image_ctx->image_cache_layers.push_front(layer);
  Context *ctx = on_finish;
  ctx = new FunctionContext(
    [this, ctx, image_cache_state_modified](int r) {
      if (r < 0) {
	ctx->complete(r);
      } else {
	ldout(m_image_ctx->cct, 20) << __func__ << " All image caches initialized" << dendl;
	update_image_cache_state(ctx, image_cache_state_modified);
      }
    });
  ldout(cct, 20) << __func__ << " Initializing image caches" << dendl;
  m_image_ctx->image_cache->init(ctx);
  return;
#else //defined(WITH_RWL)
  on_finish->complete(0);
#endif //defined(WITH_RWL)
}

template <typename I>
void ImageState<I>::shut_down_image_cache(Context *on_finish) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  if (m_image_ctx->image_cache == nullptr) {
    on_finish->complete(0);
    return;
  }

  Context *ctx = on_finish;
  ctx = new FunctionContext(
    [this, ctx](int r) {
      if (r < 0) {
	ctx->complete(r);
      } else {
	m_image_ctx->image_cache_layers.clear();
	delete m_image_ctx->image_cache;
	m_image_ctx->image_cache = nullptr;
	ctx->complete(0);
      }
    });
  ctx = new FunctionContext(
    [this, ctx](int r) {
      m_image_ctx->op_work_queue->queue(ctx, r);
    });
  ctx = new FunctionContext(
    [this, ctx](int r) {
      if (r < 0) {
	ctx->complete(r);
      } else {
	update_image_cache_state(ctx);
      }
    });
  ctx = new FunctionContext(
    [this, ctx](int r) {
      m_image_ctx->op_work_queue->queue(ctx, r);
    });
  m_image_ctx->image_cache->shut_down(ctx);
}

template <typename I>
void ImageState<I>::update_image_cache_state(Context *on_finish, bool always_write) {
  ldout(m_image_ctx->cct, 10) << this << " " << __func__ << dendl;
  bool changed = always_write;
  bool any_present = false;
  bool all_empty = true;
  bool all_clean = true;

  /* Collect states of all image caches and produce collective state */
  for (auto &layer : m_image_ctx->image_cache_layers) {
    bool present;
    bool empty;
    bool clean;
    layer->get_state(clean, empty, present);
    any_present |= present;
    all_empty &= empty;
    all_clean &= clean;
  }

  /* Detect collective state changes */
  changed |= any_present != m_image_ctx->image_cache_state.present;
  changed |= all_empty != m_image_ctx->image_cache_state.empty;
  changed |= all_clean != m_image_ctx->image_cache_state.clean;

  if (changed) {
    ldout(m_image_ctx->cct, 10) << "state changed" << dendl;
    m_image_ctx->image_cache_state.present = any_present;
    m_image_ctx->image_cache_state.empty = all_empty;
    m_image_ctx->image_cache_state.clean = all_clean;
    write_image_cache_state(on_finish);
  } else {
    on_finish->complete(0);
  }
}

template <typename I>
void ImageState<I>::write_image_cache_state(Context *on_finish) {
  ldout(m_image_ctx->cct, 10) << __func__ << " state=" << m_image_ctx->image_cache_state << dendl;
  librados::ObjectWriteOperation op;
  librbd::cls_client::set_image_cache_state(&op, m_image_ctx->image_cache_state);

  librados::AioCompletion *comp = librbd::util::create_rados_callback(on_finish);
  int r = m_image_ctx->md_ctx.aio_operate(m_image_ctx->header_oid, comp, &op);
  ceph_assert(r == 0);
  comp->release();
}

} // namespace librbd

template class librbd::ImageState<librbd::ImageCtx>;
