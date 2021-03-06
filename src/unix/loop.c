/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "uv/tree.h"
#include "internal.h"
#include "heap-inl.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 事件循环loop结构初始化 */
int uv_loop_init(uv_loop_t* loop) {
  void* saved_data;
  int err;

  /* 暂存User data */
  saved_data = loop->data;
  /* 清空loop结构 */
  memset(loop, 0, sizeof(*loop));
  /* 恢复User data */
  loop->data = saved_data;

  /* 初始化定时器堆结构 */
  heap_init((struct heap*) &loop->timer_heap);
  /* 初始化线程池（threadpool）工作队列 */
  QUEUE_INIT(&loop->wq);
  /*   */
  QUEUE_INIT(&loop->idle_handles);
  /*   */
  QUEUE_INIT(&loop->async_handles);
  /*   */
  QUEUE_INIT(&loop->check_handles);
  /*   */
  QUEUE_INIT(&loop->prepare_handles);
  /*   */
  QUEUE_INIT(&loop->handle_queue);

  /*   */
  loop->active_handles = 0;
  /*   */
  loop->active_reqs.count = 0;
  /*   */
  loop->nfds = 0;
  /*   */
  loop->watchers = NULL;
  /*   */
  loop->nwatchers = 0;
  /*   */
  QUEUE_INIT(&loop->pending_queue);
  /*   */
  QUEUE_INIT(&loop->watcher_queue);

  /*   */
  loop->closing_handles = NULL;
  /*   */
  uv__update_time(loop);
  /*   */
  loop->async_io_watcher.fd = -1;
  /*   */
  loop->async_wfd = -1;
  /*   */
  loop->signal_pipefd[0] = -1;
  loop->signal_pipefd[1] = -1;
  /*   */
  loop->backend_fd = -1;
  /*   */
  loop->emfile_fd = -1;
  
  /*   */
  loop->timer_counter = 0;
  /*   */
  loop->stop_flag = 0;

  /* loop平台相关初始化，如用linux的epoll创建后端fd */
  err = uv__platform_loop_init(loop);
  if (err)
    return err;

  /* 信号全局一次初始化  */
  uv__signal_global_once_init();
  /* 信号handle初始化 */
  err = uv_signal_init(loop, &loop->child_watcher);
  if (err)
    goto fail_signal_init;

  /*   */
  uv__handle_unref(&loop->child_watcher);
  /*   */
  loop->child_watcher.flags |= UV_HANDLE_INTERNAL;
  /*   */
  QUEUE_INIT(&loop->process_handles);

  /*   */
  err = uv_rwlock_init(&loop->cloexec_lock);
  if (err)
    goto fail_rwlock_init;

  /*   */
  err = uv_mutex_init(&loop->wq_mutex);
  if (err)
    goto fail_mutex_init;

  /* 初始化一个异步事件，loop->wq_async句柄用于线程池的work queue的异步通知，异步事件的回调为uv__work_done */
  err = uv_async_init(loop, &loop->wq_async, uv__work_done);
  if (err)
    goto fail_async_init;

  /*   */
  uv__handle_unref(&loop->wq_async);
  loop->wq_async.flags |= UV_HANDLE_INTERNAL;

  return 0;

fail_async_init:
  uv_mutex_destroy(&loop->wq_mutex);

fail_mutex_init:
  uv_rwlock_destroy(&loop->cloexec_lock);

fail_rwlock_init:
  uv__signal_loop_cleanup(loop);

fail_signal_init:
  uv__platform_loop_delete(loop);

  return err;
}


int uv_loop_fork(uv_loop_t* loop) {
  int err;
  unsigned int i;
  uv__io_t* w;

  err = uv__io_fork(loop);
  if (err)
    return err;

  err = uv__async_fork(loop);
  if (err)
    return err;

  err = uv__signal_loop_fork(loop);
  if (err)
    return err;

  /* Rearm all the watchers that aren't re-queued by the above. */
  for (i = 0; i < loop->nwatchers; i++) {
    w = loop->watchers[i];
    if (w == NULL)
      continue;

    if (w->pevents != 0 && QUEUE_EMPTY(&w->watcher_queue)) {
      w->events = 0; /* Force re-registration in uv__io_poll. */
      QUEUE_INSERT_TAIL(&loop->watcher_queue, &w->watcher_queue);
    }
  }

  return 0;
}

/*   */
void uv__loop_close(uv_loop_t* loop) {
  /*   */
  uv__signal_loop_cleanup(loop);
  /*   */
  uv__platform_loop_delete(loop);
  /*   */
  uv__async_stop(loop);

  /*   */
  if (loop->emfile_fd != -1) {
    uv__close(loop->emfile_fd);
    loop->emfile_fd = -1;
  }

  /*   */
  if (loop->backend_fd != -1) {
    uv__close(loop->backend_fd);
    loop->backend_fd = -1;
  }

  /*   */
  uv_mutex_lock(&loop->wq_mutex);
  /*   */
  assert(QUEUE_EMPTY(&loop->wq) && "thread pool work queue not empty!");
  /*   */
  assert(!uv__has_active_reqs(loop));
  /*   */
  uv_mutex_unlock(&loop->wq_mutex);
  /*   */
  uv_mutex_destroy(&loop->wq_mutex);

  /*
   * Note that all thread pool stuff is finished at this point and
   * it is safe to just destroy rw lock
   */
  uv_rwlock_destroy(&loop->cloexec_lock);

#if 0
  assert(QUEUE_EMPTY(&loop->pending_queue));
  assert(QUEUE_EMPTY(&loop->watcher_queue));
  assert(loop->nfds == 0);
#endif

  uv__free(loop->watchers);
  loop->watchers = NULL;
  loop->nwatchers = 0;
}


int uv__loop_configure(uv_loop_t* loop, uv_loop_option option, va_list ap) {
  if (option != UV_LOOP_BLOCK_SIGNAL)
    return UV_ENOSYS;

  if (va_arg(ap, int) != SIGPROF)
    return UV_EINVAL;

  loop->flags |= UV_LOOP_BLOCK_SIGPROF;
  return 0;
}
