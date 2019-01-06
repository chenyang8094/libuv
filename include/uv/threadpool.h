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

/*
 * This file is private to libuv. It provides common functionality to both
 * Windows and Unix backends.
 */

#ifndef UV_THREADPOOL_H_
#define UV_THREADPOOL_H_

/* uv__work就代表一个线程池的task
 */
struct uv__work {
  /* 该task实际要执行的任务 */
  void (*work)(struct uv__work *w);
  /* 任务执行完之后会回调该函数进行状态确认 */
  void (*done)(struct uv__work *w, int status);
  /* 该task绑定的loop */
  struct uv_loop_s* loop;
  /* 用于与其他task构建链表 */
  void* wq[2];
};

#endif /* UV_THREADPOOL_H_ */
