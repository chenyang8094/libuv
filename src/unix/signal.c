/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SA_RESTART
# define SA_RESTART 0
#endif

/* 信号消息类型定义 */
typedef struct {
  /* 信号watcher */
  uv_signal_t* handle;
  /* 信号 */
  int signum;
} uv__signal_msg_t;

RB_HEAD(uv__signal_tree_s, uv_signal_s);


static int uv__signal_unlock(void);
static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum,
                            int oneshot);
static void uv__signal_event(uv_loop_t* loop, uv__io_t* w, unsigned int events);
static int uv__signal_compare(uv_signal_t* w1, uv_signal_t* w2);
static void uv__signal_stop(uv_signal_t* handle);
static void uv__signal_unregister_handler(int signum);


static uv_once_t uv__signal_global_init_guard = UV_ONCE_INIT;
/* 用于信号的红黑树  */
static struct uv__signal_tree_s uv__signal_tree =
    RB_INITIALIZER(uv__signal_tree);
/*  */
static int uv__signal_lock_pipefd[2] = { -1, -1 };
/*  */
RB_GENERATE_STATIC(uv__signal_tree_s,
                   uv_signal_s, tree_entry,
                   uv__signal_compare)

static void uv__signal_global_reinit(void);

static void uv__signal_global_init(void) {
  /* 如果uv__signal_lock_pipefd还没有被初始化 */
  if (uv__signal_lock_pipefd[0] == -1)
    /* pthread_atfork can register before and after handlers, one
     * for each child. This only registers one for the child. That
     * state is both persistent and cumulative, so if we keep doing
     * it the handler functions will be called multiple times. Thus
     * we only want to do it once.
     * 
     * 原型：int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                          void (*child)(void));
       细节参见： http://man7.org/linux/man-pages/man3/pthread_atfork.3.html
     */
    if (pthread_atfork(NULL, NULL, &uv__signal_global_reinit))
      abort();

  uv__signal_global_reinit();
}

/*
 将UV_DESTRUCTOR宏替换后相当于： __attribute__((destructor)) static void uv__signal_global_fini(void)
 destructor属性表示uv__signal_global_fini会在main退出或者exit被调用的时候被执行
 具体参见：https://gcc.gnu.org/onlinedocs/gcc-4.3.0/gcc/Function-Attributes.html
*/
UV_DESTRUCTOR(static void uv__signal_global_fini(void)) {
  /* We can only use signal-safe functions here.
   * That includes read/write and close, fortunately.
   * We do all of this directly here instead of resetting
   * uv__signal_global_init_guard because
   * uv__signal_global_once_init is only called from uv_loop_init
   * and this needs to function in existing loops.
   */
  if (uv__signal_lock_pipefd[0] != -1) {
    uv__close(uv__signal_lock_pipefd[0]);
    uv__signal_lock_pipefd[0] = -1;
  }

  if (uv__signal_lock_pipefd[1] != -1) {
    uv__close(uv__signal_lock_pipefd[1]);
    uv__signal_lock_pipefd[1] = -1;
  }
}

/* 信号全局重新初始化 */
static void uv__signal_global_reinit(void) {
  /* 创建pipe之前先全部关闭、重置 */
  uv__signal_global_fini();

  /* 初始化管道uv__signal_lock_pipefd，用于锁，都是阻塞式的 */
  if (uv__make_pipe(uv__signal_lock_pipefd, 0))
    abort();

  /* 初始化管道uv__signal_lock_pipefd，用于锁 */
  if (uv__signal_unlock())
    abort();
}

/* 全局一次初始化函数 */
void uv__signal_global_once_init(void) {
  /* 全局一次执行uv__signal_global_init */
  uv_once(&uv__signal_global_init_guard, uv__signal_global_init);
}

/* 上锁 */
static int uv__signal_lock(void) {
  int r;
  char data;

  do {
    /* 从管道读端uv__signal_lock_pipefd[0]读一个字节数据，如果是因为信号中断导致的读
    是被就不断重试。如果写端没有关闭且管道没有数据read阻塞 */
    r = read(uv__signal_lock_pipefd[0], &data, sizeof data);
  } while (r < 0 && errno == EINTR);

  return (r < 0) ? -1 : 0;
}

/* 解锁 */
static int uv__signal_unlock(void) {
  int r;
  char data = 42;

  do {
    /* 向管道写端uv__signal_lock_pipefd[1]写入一个字节数据，如果是因为被信号中断导致写失败
    就重复写。如果读端全部关闭，则写会导致收到SIGPIPE信号，如果读端没有关闭，则写满管道会导致写阻塞 */
    r = write(uv__signal_lock_pipefd[1], &data, sizeof data);
  } while (r < 0 && errno == EINTR);

  return (r < 0) ? -1 : 0;
}

/* 屏蔽所有信号并上锁 */
static void uv__signal_block_and_lock(sigset_t* saved_sigmask) {
  sigset_t new_mask;

  /* 将所有信号填充到new_mask中 */
  if (sigfillset(&new_mask))
    abort();

  /* 屏蔽所有的信号，将原来的信号掩码存放saved_sigmask */
  if (pthread_sigmask(SIG_SETMASK, &new_mask, saved_sigmask))
    abort();

  /* 上锁 */
  if (uv__signal_lock())
    abort();
}

/* 解锁，并解除对信号的屏蔽 */
static void uv__signal_unlock_and_unblock(sigset_t* saved_sigmask) {
  if (uv__signal_unlock())
    abort();

  if (pthread_sigmask(SIG_SETMASK, saved_sigmask, NULL))
    abort();
}

/* 找到处理signum信号的第一个信号handle */
static uv_signal_t* uv__signal_first_handle(int signum) {
  /* This function must be called with the signal lock held. */
  uv_signal_t lookup;
  uv_signal_t* handle;

  /* 初始化要查找的信号handle */
  lookup.signum = signum;
  lookup.flags = 0;
  lookup.loop = NULL;

  /* 从红黑树中找到signum对应的信号handle */
  handle = RB_NFIND(uv__signal_tree_s, &uv__signal_tree, &lookup);

  if (handle != NULL && handle->signum == signum)
    return handle;

  return NULL;
}

/* 信号处理函数 */
static void uv__signal_handler(int signum) {
  uv__signal_msg_t msg;
  uv_signal_t* handle;
  int saved_errno;

  /* 保存当前errno */
  saved_errno = errno;
  memset(&msg, 0, sizeof msg);
  
  /* 上锁 */
  if (uv__signal_lock()) {
    /* 加锁失败，恢复errno并返回 */
    errno = saved_errno;
    return;
  }

  /* 遍历信号红黑树，找到关注signum信号的所有handle  */
  for (handle = uv__signal_first_handle(signum);
       handle != NULL && handle->signum == signum;
       handle = RB_NEXT(uv__signal_tree_s, &uv__signal_tree, handle)) {
    int r;

    msg.signum = signum;
    msg.handle = handle;

    /* write() should be atomic for small data chunks, so the entire message
     * should be written at once. In theory the pipe could become full, in
     * which case the user is out of luck.
     */
    do {
      /* 向管道写端loop->signal_pipefd[1]发送信息，loop->signal_pipefd[0]读端已
      经被加入epoll中，会收到这个事件。如果因为信号中断导致写失败，则重试 */
      r = write(handle->loop->signal_pipefd[1], &msg, sizeof msg);
    } while (r == -1 && errno == EINTR);

    /* 要么返回值为msg的大小，要么返回为-1但是errno为EAGAIN或EWOULDBLOCK（这个管道是非阻塞的） */
    assert(r == sizeof msg ||
           (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));

    /*  如果成功发送信号,caught_signals计数 */
    if (r != -1)
      handle->caught_signals++;
  }
  
  /* 解锁 */
  uv__signal_unlock();
  /* 恢复errno */
  errno = saved_errno;
}

/* 注册信号处理函数 */
static int uv__signal_register_handler(int signum, int oneshot) {
  /* When this function is called, the signal lock must be held. */
  struct sigaction sa;

  /* XXX use a separate signal stack? */
  memset(&sa, 0, sizeof(sa));
  if (sigfillset(&sa.sa_mask))
    abort();
  /* 设置信号处理函数uv__signal_handler */
  sa.sa_handler = uv__signal_handler;
  sa.sa_flags = SA_RESTART;
  /* SA_RESETHAND：当调用信号处理函数时，将信号的处理函数重置为缺省值SIG_DFL  */
  if (oneshot)
    sa.sa_flags |= SA_RESETHAND;

  /* XXX save old action so we can restore it later on? */
  if (sigaction(signum, &sa, NULL))
    return UV__ERR(errno);

  return 0;
}


static void uv__signal_unregister_handler(int signum) {
  /* When this function is called, the signal lock must be held. */
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;

  /* sigaction can only fail with EINVAL or EFAULT; an attempt to deregister a
   * signal implies that it was successfully registered earlier, so EINVAL
   * should never happen.
   */
  if (sigaction(signum, &sa, NULL))
    abort();
}

/* 信号初始化（只初始化一次）  */
static int uv__signal_loop_once_init(uv_loop_t* loop) {
  int err;

  /* 如果已经被初始化了就返回 */
  if (loop->signal_pipefd[0] != -1)
    return 0;

  /* 创建信号管道，该管道为非阻塞方式  */
  err = uv__make_pipe(loop->signal_pipefd, UV__F_NONBLOCK);
  if (err)
    return err;

  /* 初始化signal_io_watcher，回调为uv__signal_event，fd为管道的一端loop->signal_pipefd[0]  */
  uv__io_init(&loop->signal_io_watcher,
              uv__signal_event,
              loop->signal_pipefd[0]);
  /* 向loop注册这个watcher，注册事件为POLLIN  */
  uv__io_start(loop, &loop->signal_io_watcher, POLLIN);

  return 0;
}


int uv__signal_loop_fork(uv_loop_t* loop) {
  uv__io_stop(loop, &loop->signal_io_watcher, POLLIN);
  uv__close(loop->signal_pipefd[0]);
  uv__close(loop->signal_pipefd[1]);
  loop->signal_pipefd[0] = -1;
  loop->signal_pipefd[1] = -1;
  return uv__signal_loop_once_init(loop);
}


void uv__signal_loop_cleanup(uv_loop_t* loop) {
  QUEUE* q;

  /* Stop all the signal watchers that are still attached to this loop. This
   * ensures that the (shared) signal tree doesn't contain any invalid entries
   * entries, and that signal handlers are removed when appropriate.
   * It's safe to use QUEUE_FOREACH here because the handles and the handle
   * queue are not modified by uv__signal_stop().
   */
  QUEUE_FOREACH(q, &loop->handle_queue) {
    uv_handle_t* handle = QUEUE_DATA(q, uv_handle_t, handle_queue);

    if (handle->type == UV_SIGNAL)
      uv__signal_stop((uv_signal_t*) handle);
  }

  if (loop->signal_pipefd[0] != -1) {
    uv__close(loop->signal_pipefd[0]);
    loop->signal_pipefd[0] = -1;
  }

  if (loop->signal_pipefd[1] != -1) {
    uv__close(loop->signal_pipefd[1]);
    loop->signal_pipefd[1] = -1;
  }
}

/* 信号初始化 */
int uv_signal_init(uv_loop_t* loop, uv_signal_t* handle) {
  int err;

  /* 信号初始化 */
  err = uv__signal_loop_once_init(loop);
  if (err)
    return err;

  /* 信号handle初始化 */
  uv__handle_init(loop, (uv_handle_t*) handle, UV_SIGNAL);
  handle->signum = 0;
  handle->caught_signals = 0;
  handle->dispatched_signals = 0;

  return 0;
}


void uv__signal_close(uv_signal_t* handle) {

  uv__signal_stop(handle);

  /* If there are any caught signals "trapped" in the signal pipe, we can't
   * call the close callback yet. Otherwise, add the handle to the finish_close
   * queue.
   */
  if (handle->caught_signals == handle->dispatched_signals) {
    uv__make_close_pending((uv_handle_t*) handle);
  }
}

/* 注册一个信号 */
int uv_signal_start(uv_signal_t* handle, uv_signal_cb signal_cb, int signum) {
  /* 启动信号 */
  return uv__signal_start(handle, signal_cb, signum, 0);
}


int uv_signal_start_oneshot(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum) {
  return uv__signal_start(handle, signal_cb, signum, 1);
}


static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum,
                            int oneshot) {
  sigset_t saved_sigmask;
  int err;
  uv_signal_t* first_handle;

  /* 确定这个信号handle不能被关闭 */
  assert(!uv__is_closing(handle));

  /* If the user supplies signum == 0, then return an error already. If the
   * signum is otherwise invalid then uv__signal_register will find out
   * eventually.
   */
  if (signum == 0)
    return UV_EINVAL;

  /* Short circuit: if the signal watcher is already watching {signum} don't
   * go through the process of deregistering and registering the handler.
   * Additionally, this avoids pending signals getting lost in the small time
   * time frame that handle->signum == 0.
   */
  if (signum == handle->signum) {
    handle->signal_cb = signal_cb;
    return 0;
  }

  /* If the signal handler was already active, stop it first. */
  if (handle->signum != 0) {
    uv__signal_stop(handle);
  }

  uv__signal_block_and_lock(&saved_sigmask);

  /* If at this point there are no active signal watchers for this signum (in
   * any of the loops), it's time to try and register a handler for it here.
   * Also in case there's only one-shot handlers and a regular handler comes in.
   */
  first_handle = uv__signal_first_handle(signum);
  if (first_handle == NULL ||
      (!oneshot && (first_handle->flags & UV_SIGNAL_ONE_SHOT))) {
    err = uv__signal_register_handler(signum, oneshot);
    if (err) {
      /* Registering the signal handler failed. Must be an invalid signal. */
      uv__signal_unlock_and_unblock(&saved_sigmask);
      return err;
    }
  }

  handle->signum = signum;
  if (oneshot)
    handle->flags |= UV_SIGNAL_ONE_SHOT;

  RB_INSERT(uv__signal_tree_s, &uv__signal_tree, handle);

  uv__signal_unlock_and_unblock(&saved_sigmask);

  /* 设置信号handle回调 */
  handle->signal_cb = signal_cb;
  /* 激活这个信号handle */
  uv__handle_start(handle);

  return 0;
}

/* loop->signal_io_watcher的回调 */
static void uv__signal_event(uv_loop_t* loop,
                             uv__io_t* w,
                             unsigned int events) {
  uv__signal_msg_t* msg;
  uv_signal_t* handle;
  char buf[sizeof(uv__signal_msg_t) * 32];
  size_t bytes, end, i;
  int r;

  bytes = 0;
  end = 0;

  do {
    r = read(loop->signal_pipefd[0], buf + bytes, sizeof(buf) - bytes);

    if (r == -1 && errno == EINTR)
      continue;

    if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* If there are bytes in the buffer already (which really is extremely
       * unlikely if possible at all) we can't exit the function here. We'll
       * spin until more bytes are read instead.
       */
      if (bytes > 0)
        continue;

      /* Otherwise, there was nothing there. */
      return;
    }

    /* Other errors really should never happen. */
    if (r == -1)
      abort();

    bytes += r;

    /* `end` is rounded down to a multiple of sizeof(uv__signal_msg_t). */
    end = (bytes / sizeof(uv__signal_msg_t)) * sizeof(uv__signal_msg_t);

    for (i = 0; i < end; i += sizeof(uv__signal_msg_t)) {
      msg = (uv__signal_msg_t*) (buf + i);
      handle = msg->handle;

      if (msg->signum == handle->signum) {
        assert(!(handle->flags & UV_HANDLE_CLOSING));
        handle->signal_cb(handle, handle->signum);
      }

      handle->dispatched_signals++;

      if (handle->flags & UV_SIGNAL_ONE_SHOT)
        uv__signal_stop(handle);

      /* If uv_close was called while there were caught signals that were not
       * yet dispatched, the uv__finish_close was deferred. Make close pending
       * now if this has happened.
       */
      if ((handle->flags & UV_HANDLE_CLOSING) &&
          (handle->caught_signals == handle->dispatched_signals)) {
        uv__make_close_pending((uv_handle_t*) handle);
      }
    }

    bytes -= end;

    /* If there are any "partial" messages left, move them to the start of the
     * the buffer, and spin. This should not happen.
     */
    if (bytes) {
      memmove(buf, buf + end, bytes);
      continue;
    }
  } while (end == sizeof buf);
}


static int uv__signal_compare(uv_signal_t* w1, uv_signal_t* w2) {
  int f1;
  int f2;
  /* Compare signums first so all watchers with the same signnum end up
   * adjacent.
   */
  if (w1->signum < w2->signum) return -1;
  if (w1->signum > w2->signum) return 1;

  /* Handlers without UV_SIGNAL_ONE_SHOT set will come first, so if the first
   * handler returned is a one-shot handler, the rest will be too.
   */
  f1 = w1->flags & UV_SIGNAL_ONE_SHOT;
  f2 = w2->flags & UV_SIGNAL_ONE_SHOT;
  if (f1 < f2) return -1;
  if (f1 > f2) return 1;

  /* Sort by loop pointer, so we can easily look up the first item after
   * { .signum = x, .loop = NULL }.
   */
  if (w1->loop < w2->loop) return -1;
  if (w1->loop > w2->loop) return 1;

  if (w1 < w2) return -1;
  if (w1 > w2) return 1;

  return 0;
}


int uv_signal_stop(uv_signal_t* handle) {
  assert(!uv__is_closing(handle));
  uv__signal_stop(handle);
  return 0;
}


static void uv__signal_stop(uv_signal_t* handle) {
  uv_signal_t* removed_handle;
  sigset_t saved_sigmask;
  uv_signal_t* first_handle;
  int rem_oneshot;
  int first_oneshot;
  int ret;

  /* If the watcher wasn't started, this is a no-op. */
  if (handle->signum == 0)
    return;

  uv__signal_block_and_lock(&saved_sigmask);

  removed_handle = RB_REMOVE(uv__signal_tree_s, &uv__signal_tree, handle);
  assert(removed_handle == handle);
  (void) removed_handle;

  /* Check if there are other active signal watchers observing this signal. If
   * not, unregister the signal handler.
   */
  first_handle = uv__signal_first_handle(handle->signum);
  if (first_handle == NULL) {
    uv__signal_unregister_handler(handle->signum);
  } else {
    rem_oneshot = handle->flags & UV_SIGNAL_ONE_SHOT;
    first_oneshot = first_handle->flags & UV_SIGNAL_ONE_SHOT;
    if (first_oneshot && !rem_oneshot) {
      ret = uv__signal_register_handler(handle->signum, 1);
      assert(ret == 0);
    }
  }

  uv__signal_unlock_and_unblock(&saved_sigmask);

  handle->signum = 0;
  uv__handle_stop(handle);
}
