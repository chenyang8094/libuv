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

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);
static int uv__async_eventfd(void);


/* 初始化一个异步uv_async_t handle */
int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  /* 启动异步事件监听 */
  err = uv__async_start(loop);
  if (err)
    return err;

  /* 初始化handle,绑定loop，设置类型等 */
  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  /* 设置该handle的回调 */
  handle->async_cb = async_cb;
  /* handle悬挂标志，为1表示有异步事件需要处理 */
  handle->pending = 0;

  /* 将该handle插入loop->async_handles队列，以便回调的时候可以找到 */
  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  /* 激活该handle */
  uv__handle_start(handle);

  return 0;
}

/* 发送异步通知 */
int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. 
    展开后  (*(volatile int*) &(handle->pending))
  */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;

  /* 原子比较并交换,将该handle的pending原子修改为1，表示有异步事件需要处理 */
  if (cmpxchgi(&handle->pending, 0, 1) == 0)
    uv__async_send(handle->loop);/* 向loop发送异步通知 */

  return 0;
}

/* 关闭一个异步handle */
void uv__async_close(uv_async_t* handle) {
  /* 将该handle从loop->async_handles队列移除 */
  QUEUE_REMOVE(&handle->queue);
  /* 失效该handle */
  uv__handle_stop(handle);
}

/* 异步事件回调 */
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;

  /* watcher必须是loop->async_io_watcher */
  assert(w == &loop->async_io_watcher);

  /* 将fd的内容读到buf，内容是什么没有关系 */
  for (;;) {
    /* 从异步事件描述符读取 */
    r = read(w->fd, buf, sizeof(buf));

    /* 如果把buf直接读满，则说明异步事件可能很多，需要再读一次 */
    if (r == sizeof(buf))
      continue;

    /* 否则跳出循环准备事件处理 */
    if (r != -1)
      break;

    /* fd为非阻塞模式 */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    /* 被信号打断，则重新读 */
    if (errno == EINTR)
      continue;

    abort();
  }

  /* 将loop->async_handles上的队列移动到queue */
  QUEUE_MOVE(&loop->async_handles, &queue);
  /* 遍历queue */
  while (!QUEUE_EMPTY(&queue)) {
    /* 取queue头结点 */
    q = QUEUE_HEAD(&queue);
    /* 还原uv_async_t结构 */
    h = QUEUE_DATA(q, uv_async_t, queue);

    /* 移除该节点 */
    QUEUE_REMOVE(q);
    /* 再次将该节点插入loop->async_handles */
    QUEUE_INSERT_TAIL(&loop->async_handles, q);

    /* 只有pending为1的handle才需要被处理，将uv_async_t->pending原子设置为0 */
    if (cmpxchgi(&h->pending, 1, 0) == 0)
      continue;

    /* uv_async_t是否设置回调函数 */
    if (h->async_cb == NULL)
      continue;

    /* 回调uv_async_t的回调函数 */
    h->async_cb(h);
  }
}

/* 向loop发送异步通知 */
static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  /* 获取异步通知文件描述符，注意这里有两种情况
  一种是用eventfd实现，一种是用pipe实现 */
  fd = loop->async_wfd;

#if defined(__linux__)
  /* 如果为-1则表示异步通知使用eventfd实现 */
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    /* loop->async_io_watcher.fd上就是eventfd */
    fd = loop->async_io_watcher.fd; 
  }
#endif

  do
    /* 向async_wfd写数据，loop会捕捉到这个事件 */
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  /* 如果返回值就是写的字节数，成功直接返回 */
  if (r == len)
    return;

  /* 如果是因为非阻塞导致的错误，则返回 */
  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  /* 其他错误，直接abort */
  abort();
}

/* 启动异步事件监听 */
static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

  /* 如果已经初始化过就直接返回  */
  if (loop->async_io_watcher.fd != -1)
    return 0;

  /* 创建eventfd */
  err = uv__async_eventfd();
  if (err >= 0) {
    pipefd[0] = err;
    pipefd[1] = -1;/* 注意这里-1的特殊之处 */
  }
  else if (err == UV_ENOSYS) {
    /* 如果系统不支持eventfd，那么就是用pipe来实现，这里以非阻塞的方式创建 */
    err = uv__make_pipe(pipefd, UV__F_NONBLOCK);
#if defined(__linux__)
    /* Save a file descriptor by opening one of the pipe descriptors as
     * read/write through the procfs.  That file descriptor can then
     * function as both ends of the pipe.
     */
    if (err == 0) {
      char buf[32];
      int fd;

      snprintf(buf, sizeof(buf), "/proc/self/fd/%d", pipefd[0]);
      fd = uv__open_cloexec(buf, O_RDWR);
      if (fd >= 0) {
        uv__close(pipefd[0]);
        uv__close(pipefd[1]);
        pipefd[0] = fd;
        pipefd[1] = fd;
      }
    }
#endif
  }

  if (err < 0)
    return err;

  /* 初始化loop->async_io_watcher，回调函数为uv__async_io，监听的描述符为pipefd[0] */
  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  /* 注册async_io_watcher到loop中，监听POLLIN事件 */
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  /* 异步事件的写端，注意，如果使用eventfd方式实现，这里为-1，send函数会做区分 */
  loop->async_wfd = pipefd[1];

  return 0;
}

/*  */
int uv__async_fork(uv_loop_t* loop) {
  /*  */
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  /*  */
  uv__async_stop(loop);

  return uv__async_start(loop);
}

/* 停止异步事件 */
void uv__async_stop(uv_loop_t* loop) {
  /* 如果loop->async_io_watcher.fd无效直接退出 */
  if (loop->async_io_watcher.fd == -1)
    return;

  /* loop->async_wfd不为-1则为pipe方式  */
  if (loop->async_wfd != -1) {
    /* pipe两端fd应该不一样，现在关闭一端 */
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }
  
  /* 取消loop->async_io_watcher对POLLIN事件的监听 */
  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  /* 关闭loop->async_io_watcher.fd */
  uv__close(loop->async_io_watcher.fd);
  /* 关闭eventfd或者pipe的另一端 */
  loop->async_io_watcher.fd = -1;
}

/* 创建eventfd */
static int uv__async_eventfd(void) {
#if defined(__linux__)
  /* 注意这里都是局部静态变量，只有第一次需要盲目重试 */
  static int no_eventfd2;
  static int no_eventfd;
  int fd;

  if (no_eventfd2)
    goto skip_eventfd2;

  /* 优先使用较新的api，支持设置flags，具体定义参见：https://linux.die.net/man/2/eventfd2 */
  fd = uv__eventfd2(0, UV__EFD_CLOEXEC | UV__EFD_NONBLOCK);
  if (fd != -1)
    return fd;

  /* 如果错误不是因为不支持uv__eventfd2，就返回错误 */
  if (errno != ENOSYS)
    return UV__ERR(errno);

  /* 否则就是系统不支持uv__eventfd2 */
  no_eventfd2 = 1;

skip_eventfd2:

  if (no_eventfd)
    goto skip_eventfd;

  /*
    如果没有uv__eventfd2，则使用uv__eventfd
    详细参见：https://www.systutorials.com/docs/linux/man/2-eventfd/
  */
  fd = uv__eventfd(0);
  if (fd != -1) {
    /* 需要单独设置CLOEXEC和NONBLOCK标志 */
    uv__cloexec(fd, 1);
    uv__nonblock(fd, 1);
    return fd;
  }

  /* 如果错误不是因为不支持uv__eventfd，就返回错误 */
  if (errno != ENOSYS)
    return UV__ERR(errno);

  /* 否则就是系统不支持uv__eventfd */
  no_eventfd = 1;

skip_eventfd:

#endif

  return UV_ENOSYS;
}
