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

#include "uv-common.h"

#if !defined(_WIN32)
# include "unix/internal.h"
#endif

#include <stdlib.h>

#define MAX_THREADPOOL_SIZE 128

static uv_once_t once = UV_ONCE_INIT;/* pthread_once_t */
static uv_cond_t cond;/* 队列为空时线程池的线程会在该条件变量上睡眠 */
static uv_mutex_t mutex;/* 线程池内部锁 */
static unsigned int idle_threads;/* 当前空闲线程的数目 */
static unsigned int slow_io_work_running;
static unsigned int nthreads;
static uv_thread_t* threads;
static uv_thread_t default_threads[4];/* 默认四个线程的线程池 */
static QUEUE exit_message;/* 线程池退出消息 */
static QUEUE wq; /* 线程池线程全部会检查这个queue，一旦发现有任务就执行，但是只能有一个线程抢占到 */
static QUEUE run_slow_work_message;
static QUEUE slow_io_pending_wq;/* 慢IO型的任务都会放到这个队列 */

/* 慢任务的数目不成超过线程池线程数的一般 */
static unsigned int slow_work_thread_threshold(void) {
  return (nthreads + 1) / 2;
}

/*  */
static void uv__cancelled(struct uv__work* w) {
  abort();
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 * 
 * 线程池（每个线程）的工作函数
 */
static void worker(void* arg) {
  struct uv__work* w;
  QUEUE* q;
  int is_slow_work;

  /* 参数是传进来的信号量 */
  uv_sem_post((uv_sem_t*) arg);
  arg = NULL;

  /* 因为是多线程访问，因此需要加锁同步，mutex为线程池内部锁 */
  uv_mutex_lock(&mutex);
  /*  */
  for (;;) {
    /* `mutex` should always be locked at this point. */

    /* 
      当任务队列wq为空或者（wq只剩run_slow_work_message一个节点且慢io任务的执行次数已经超过阈值），
      那么此时就循环等待。
    */
    while (QUEUE_EMPTY(&wq) ||
           (QUEUE_HEAD(&wq) == &run_slow_work_message &&
            QUEUE_NEXT(&run_slow_work_message) == &wq &&
            slow_io_work_running >= slow_work_thread_threshold())) {
      /* 空闲线程数加1 */
      idle_threads += 1;
      /* 等待条件变量 */
      uv_cond_wait(&cond, &mutex);
      /* 被唤醒之后，说明有任务被post到队列，因此空闲线程数需要减1 */
      idle_threads -= 1;
    }

    /* 取出队列的头部节点（第一个task） */
    q = QUEUE_HEAD(&wq);
    /* 如果这是一个退出消息 */
    if (q == &exit_message) {
      /* 给条件变量发信号 */
      uv_cond_signal(&cond);
      /* 解锁 */
      uv_mutex_unlock(&mutex);
      /* 直接退出循环（也就是退出线程） */
      break;
    }

    /* 从队列中移除这个task */
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is executing. */

    is_slow_work = 0;
    /* 如果这个Task是run_slow_work_message */
    if (q == &run_slow_work_message) {
      /* If we're at the slow I/O threshold, re-schedule until after all
         other work in the queue is done. */
      if (slow_io_work_running >= slow_work_thread_threshold()) {
        /* 如果已处理慢io任务的数目超过阈值，就先不处理，将其加到wq中，开始处理下一个任务 */
        QUEUE_INSERT_TAIL(&wq, q);
        continue;
      }

      /* 
         否则，慢IO没有超过阈值，可以执行慢IO任务，但是如果slow_io_pending_wq为空，则说明
         这个慢IO任务已经被取消了。则开始下一次循环
         */
      if (QUEUE_EMPTY(&slow_io_pending_wq))
        continue;

      /* 标记这是一个慢IO任务 */
      is_slow_work = 1;
      /* 慢IO任务执行次数计数 */
      slow_io_work_running++;

      /* 从slow_io_pending_wq中取出并删除这个任务 */
      q = QUEUE_HEAD(&slow_io_pending_wq);
      QUEUE_REMOVE(q);
      QUEUE_INIT(q);

      /* If there is more slow I/O work, schedule it to be run as well.
         如果slow_io_pending_wq不为空，说明还有满IO任务待处理
       */
      if (!QUEUE_EMPTY(&slow_io_pending_wq)) {
        /* 再向wq中拆入run_slow_work_message */
        QUEUE_INSERT_TAIL(&wq, &run_slow_work_message);
        /* 如果空闲线程大于0，就唤醒线程池 */
        if (idle_threads > 0)
          uv_cond_signal(&cond);
      }
    }

    /* wq访问结束，mutex可以解锁 */
    uv_mutex_unlock(&mutex);

    /* 还原uv__work */
    w = QUEUE_DATA(q, struct uv__work, wq);
    /* 执行这个task */
    w->work(w);

    /* 对loop->wq_mutex上互斥锁，因为接下来会并发修改loop->wq */
    uv_mutex_lock(&w->loop->wq_mutex);
    /*   */
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    /* 将该已经执行完的task加入loop->wq  */
    QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq);
    /* 向loop发送异步通知  */
    uv_async_send(&w->loop->wq_async);
    /*  解锁 */
    uv_mutex_unlock(&w->loop->wq_mutex);

    /* Lock `mutex` since that is expected at the start of the next
     * iteration. */
    uv_mutex_lock(&mutex);
    if (is_slow_work) {
      /* `slow_io_work_running` is protected by `mutex`. */
      slow_io_work_running--;
    }
  }
}

/* 将一个uv__work提交到工作队列
uv__work_kind为任务类型：
  UV__WORK_CPU,
  UV__WORK_FAST_IO,
  UV__WORK_SLOW_IO
 */
static void post(QUEUE* q, enum uv__work_kind kind) {
  /* 要操纵工作队列，必须加锁 */
  uv_mutex_lock(&mutex);
  /* 慢IO型任务 */
  if (kind == UV__WORK_SLOW_IO) {
    /* 将该类型的任务加入到一个单独的队列slow_io_pending_wq */
    QUEUE_INSERT_TAIL(&slow_io_pending_wq, q);
    /* run_slow_work_message不为空表示已经被加入了wq队列 */
    if (!QUEUE_EMPTY(&run_slow_work_message)) {
      /* Running slow I/O tasks is already scheduled => Nothing to do here.
         The worker that runs said other task will schedule this one as well. */
      uv_mutex_unlock(&mutex);
      return;
    }
    /* 由于这正的慢IO任务已经被加入到slow_io_pending_wq，因此普通的wq中就加入run_slow_work_message，这
    可以继续向下执行并唤醒线程池（也就是每执行一个慢IO任务，wq中就会加入一个run_slow_work_message） */
    q = &run_slow_work_message;
  }

  /* 插入普通工作队列 */
  QUEUE_INSERT_TAIL(&wq, q);
  /* 如果有空闲线程就给条件变量发信号 */
  if (idle_threads > 0)
    uv_cond_signal(&cond);
  
  /* 解锁 */
  uv_mutex_unlock(&mutex);
}


#ifndef _WIN32
/* 在mian退出或者执行exit后的清理函数 */
UV_DESTRUCTOR(static void cleanup(void)) {
  unsigned int i;

  if (nthreads == 0)
    return;

  /* 向工作队列提交一个退出消息 */
  post(&exit_message, UV__WORK_CPU);

  /* 等待线程池的线程全部退出 http://man7.org/linux/man-pages/man3/pthread_join.3.html */
  for (i = 0; i < nthreads; i++)
    if (uv_thread_join(threads + i))
      abort();

  /* 如果不是默认线程池，还要释放内存 */
  if (threads != default_threads)
    uv__free(threads);

  /* 销毁锁和条件变量 */
  uv_mutex_destroy(&mutex);
  uv_cond_destroy(&cond);

  threads = NULL;
  nthreads = 0;
}
#endif

/* 线程池初始化 */
static void init_threads(void) {
  unsigned int i;
  const char* val;
  uv_sem_t sem;

  /* 默认线程池大小 */
  nthreads = ARRAY_SIZE(default_threads);
  /* 环境变量设置的线程池大小 */
  val = getenv("UV_THREADPOOL_SIZE");
  if (val != NULL)
    nthreads = atoi(val);
  /* 至少要有一个线程 */
  if (nthreads == 0)
    nthreads = 1;
  /* 最大为128 */
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  /* 指向线程数组 */
  threads = default_threads;
  /* 如果用户设置的线程池比默认的大 */
  if (nthreads > ARRAY_SIZE(default_threads)) {
    /* 则重新动态分配线程池数组 */
    threads = uv__malloc(nthreads * sizeof(threads[0]));
    /* 分配失败就用默认的设置 */
    if (threads == NULL) {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }

  /* 条件变量初始化 */
  if (uv_cond_init(&cond))
    abort();

  /* 互斥锁初始化 */
  if (uv_mutex_init(&mutex))
    abort();

  /* 初始化工作队列 */
  QUEUE_INIT(&wq);
  /* 初始化慢IO型task工作队列 */
  QUEUE_INIT(&slow_io_pending_wq);
  QUEUE_INIT(&run_slow_work_message);

  /* 初始化信号量 */
  if (uv_sem_init(&sem, 0))
    abort();
   
  /* 创建线程，每个线程都传入信号量这个参数 */
  for (i = 0; i < nthreads; i++)
    if (uv_thread_create(threads + i, worker, &sem))
      abort();

  /* 等待所有线程都创建完成（确切的说是全部执行了worker函数） */
  for (i = 0; i < nthreads; i++)
    uv_sem_wait(&sem);

  /* 销毁信号量 */
  uv_sem_destroy(&sem);
}


#ifndef _WIN32
static void reset_once(void) {
  uv_once_t child_once = UV_ONCE_INIT;
  memcpy(&once, &child_once, sizeof(child_once));
}
#endif

/* 只初始化一次 */
static void init_once(void) {
#ifndef _WIN32
  /* Re-initialize the threadpool after fork.
   * Note that this discards the global mutex and condition as well
   * as the work queue.
   */
  if (pthread_atfork(NULL, NULL, &reset_once))
    abort();
#endif
  init_threads();
}

/* 向线程池提交一个uv__work */
void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     enum uv__work_kind kind,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  /* once这个变量如果还没初始化过，就会执行init_once，否则不会执行init_once */                        
  uv_once(&once, init_once);
  /* 设置uv__work */
  w->loop = loop;
  w->work = work;
  w->done = done;
  /* 提交到工作队列 */
  post(&w->wq, kind);
}

/* 取消一个uv__work */
static int uv__work_cancel(uv_loop_t* loop, uv_req_t* req, struct uv__work* w) {
  int cancelled;

  uv_mutex_lock(&mutex);
  uv_mutex_lock(&w->loop->wq_mutex);

  cancelled = !QUEUE_EMPTY(&w->wq) && w->work != NULL;
  if (cancelled)
    QUEUE_REMOVE(&w->wq);

  uv_mutex_unlock(&w->loop->wq_mutex);
  uv_mutex_unlock(&mutex);

  if (!cancelled)
    return UV_EBUSY;

  w->work = uv__cancelled;
  uv_mutex_lock(&loop->wq_mutex);
  QUEUE_INSERT_TAIL(&loop->wq, &w->wq);
  uv_async_send(&loop->wq_async);
  uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


/* 当一个task被执行完之后，异步事件最终会回调该函数，也就是说，每个task是在线程池中被
执行，但是回调却是在loop线程中 */
void uv__work_done(uv_async_t* handle) {
  struct uv__work* w;
  uv_loop_t* loop;
  QUEUE* q;
  QUEUE wq;
  int err;

  /* 还原loop */
  loop = container_of(handle, uv_loop_t, wq_async);
  /* loop->wq会被并发访问，先加锁 */
  uv_mutex_lock(&loop->wq_mutex);
  /* 已经执行完的会被取消的task都会被加入loop->wq，将loop->wq全部移动到wq */
  QUEUE_MOVE(&loop->wq, &wq);
  uv_mutex_unlock(&loop->wq_mutex);

  /* 遍历wq */
  while (!QUEUE_EMPTY(&wq)) {
    /* 取出闭关删除头节点 */
    q = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(q);

    /* 还原uv__work结构 */
    w = container_of(q, struct uv__work, wq);
    /* 如果该work被取消 */
    err = (w->work == uv__cancelled) ? UV_ECANCELED : 0;
    /* 否则就执行其done函数 */
    w->done(w, err);
  }
}

/*  */
static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

  req->work_cb(req);
}

/*  */
static void uv__queue_done(struct uv__work* w, int err) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

  req->after_work_cb(req, err);
}

/*  */
int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return UV_EINVAL;

  uv__req_init(loop, req, UV_WORK);
  req->loop = loop;
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;
  uv__work_submit(loop,
                  &req->work_req,
                  UV__WORK_CPU,
                  uv__queue_work,
                  uv__queue_done);
  return 0;
}

/*  */
int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    break;
  case UV_GETNAMEINFO:
    loop = ((uv_getnameinfo_t*) req)->loop;
    wreq = &((uv_getnameinfo_t*) req)->work_req;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    break;
  default:
    return UV_EINVAL;
  }

  return uv__work_cancel(loop, req, wreq);
}
