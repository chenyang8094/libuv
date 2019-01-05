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
#include "uv-common.h"
#include "heap-inl.h"

#include <assert.h>
#include <limits.h>

/* 比较两个定时器，用于在定时器堆的插入操作中比较两个定时器的大小，如果ha超时时间比hb短，返回1，否则返回0 */
static int timer_less_than(const struct heap_node* ha,
                           const struct heap_node* hb) {
  const uv_timer_t* a;
  const uv_timer_t* b;

  /* 根据堆节点还原uv_timer_t结构，典型contaner_of用法 */
  a = container_of(ha, uv_timer_t, heap_node);
  b = container_of(hb, uv_timer_t, heap_node);

  /* 根据timeout大小比较 */
  if (a->timeout < b->timeout)
    return 1;
  if (b->timeout < a->timeout)
    return 0;

  /* 
     如果timeout相同，就比较start_id
   */
  if (a->start_id < b->start_id)
    return 1;
  if (b->start_id < a->start_id)
    return 0;

  return 0;
}

/* 初始化一个定时器handle */
int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  /* 一个handle的基础初始化，将loop绑定到handle，设置handle类型、标志，并加入
  loop->handle_queue队列中 */
  uv__handle_init(loop, (uv_handle_t*)handle, UV_TIMER);
  handle->timer_cb = NULL;
  handle->repeat = 0;
  return 0;
}

/* 启动一个定时器 */
int uv_timer_start(uv_timer_t* handle,
                   uv_timer_cb cb,
                   uint64_t timeout,
                   uint64_t repeat) {
  uint64_t clamped_timeout;
  /* 定时器回调不能为NULL */
  if (cb == NULL)
    return UV_EINVAL;

  /* 如果定时器handle此时已经是激活状态，则需要先停止 */
  if (uv__is_active(handle))
    uv_timer_stop(handle);

  /* 计算超时绝对时间，为当前时间loop->time加上timeout */
  clamped_timeout = handle->loop->time + timeout;
  /* TODO: */
  if (clamped_timeout < timeout)
    clamped_timeout = (uint64_t) -1;

  /* 初始化定时器handle */
  handle->timer_cb = cb;
  handle->timeout = clamped_timeout;
  handle->repeat = repeat;
  /* 
     start_id作为定时器的二级索引，会在uv__timer_cmp()被使用，其值就是loop->timer_counter
  */
  handle->start_id = handle->loop->timer_counter++;
  /* 将定时器handle中的heap_node插入定时器堆（小根堆）timer_heap */
  heap_insert((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  /* 激活定时器handle，也就是修改状态UV_HANDLE_ACTIVE，甚至会改变loop引用计数 */
  uv__handle_start(handle);

  return 0;
}

/* 定制一个定时器 */
int uv_timer_stop(uv_timer_t* handle) {
  /* 如果这个定时器handle不是激活的就直接退出 */
  if (!uv__is_active(handle))
    return 0;

  /* 从定时器最小堆中删除这个定时器节点 */
  heap_remove((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  /* 将handle标记为非激活状态 */
  uv__handle_stop(handle);

  return 0;
}

/* 重新添加定时器 */
int uv_timer_again(uv_timer_t* handle) {
  /* 如果定时器回调为NULL或者repeat标志位0，则直接退出 */
  if (handle->timer_cb == NULL || handle->repeat == 0)
    return UV_EINVAL;
  
  /* 启动定时器 */
  uv_timer_start(handle, handle->timer_cb, handle->repeat, handle->repeat);

  return 0;
}

/* 设置是否是自动重复定时器标志 */
void uv_timer_set_repeat(uv_timer_t* handle, uint64_t repeat) {
  handle->repeat = repeat;
}

/* 获取是否是自动重复定时器标志 */
uint64_t uv_timer_get_repeat(const uv_timer_t* handle) {
  return handle->repeat;
}

/* 获取下一个超时时间（即能保证至少有一个定时器超时） */
int uv__next_timeout(const uv_loop_t* loop) {
  const struct heap_node* heap_node;
  const uv_timer_t* handle;
  uint64_t diff;

  /* 从小顶堆取出根节点 */
  heap_node = heap_min((const struct heap*) &loop->timer_heap);
  if (heap_node == NULL)
    return -1; /* block indefinitely */

  /* 根据堆节点还原uv_timer_t结构，典型contaner_of用法 */
  handle = container_of(heap_node, uv_timer_t, heap_node);
  /* 如果定时器超时时间小于当前时间，也就是已经超时，则直接返回0 */
  if (handle->timeout <= loop->time)
    return 0;
  
  /* 否则就返回超时时间和房钱时间的差值 */
  diff = handle->timeout - loop->time;
  /* 如果差值超过INT最大值，则修正为INT最大值 */
  if (diff > INT_MAX)
    diff = INT_MAX;

  return diff;
}

/* 运行定时器任务 */
void uv__run_timers(uv_loop_t* loop) {
  struct heap_node* heap_node;
  uv_timer_t* handle;
  
  /* 遍历所有定时器任务 */
  for (;;) {
    /* 从最小堆中取出根节点 */
    heap_node = heap_min((struct heap*) &loop->timer_heap);
    /* 堆为空，直接跳出循环 */
    if (heap_node == NULL)
      break;

    /* 根据堆节点还原uv_timer_t结构，典型contaner_of用法 */
    handle = container_of(heap_node, uv_timer_t, heap_node);
    /* 如果定时器超时时间timeout大于当前时间loop->time，则定时器都没有超时 */
    if (handle->timeout > loop->time)
      break;

    /* 否则，先停止定时器 */
    uv_timer_stop(handle);
    /* 该定时器是否需要自动重复添加 */
    uv_timer_again(handle);
    /* 执行定时器回调 */
    handle->timer_cb(handle);
  }
}

/* 关闭一个定时器 */
void uv__timer_close(uv_timer_t* handle) {
  /* 停止定时器 */
  uv_timer_stop(handle);
}
