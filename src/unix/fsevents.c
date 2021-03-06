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

#if TARGET_OS_IPHONE

/* iOS (currently) doesn't provide the FSEvents-API (nor CoreServices) */

int uv__fsevents_init(uv_fs_event_t* handle) {
  return 0;
}


int uv__fsevents_close(uv_fs_event_t* handle) {
  return 0;
}


void uv__fsevents_loop_delete(uv_loop_t* loop) {
  return 0;
}

#else /* TARGET_OS_IPHONE */

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>

#include <CoreFoundation/CFRunLoop.h>
#include <CoreServices/CoreServices.h>

typedef struct uv__fsevents_event_s uv__fsevents_event_t;
typedef struct uv__cf_loop_signal_s uv__cf_loop_signal_t;
typedef void (*cf_loop_signal_cb)(void* arg);

struct uv__cf_loop_signal_s {
  cf_loop_signal_cb cb;
  QUEUE member;
  void* arg;
};

struct uv__fsevents_event_s {
  int events;
  QUEUE member;
  char path[1];
};

/* Forward declarations */
static void uv__cf_loop_cb(void* arg);
static void* uv__cf_loop_runner(void* arg);
static void uv__cf_loop_signal(uv_loop_t* loop,
                               cf_loop_signal_cb cb,
                               void* arg);

#define UV__FSEVENTS_WALK(handle, block)                                      \
    {                                                                         \
      QUEUE* curr;                                                            \
      QUEUE split_head;                                                       \
      uv__fsevents_event_t* event;                                            \
      uv_mutex_lock(&(handle)->cf_mutex);                                     \
      QUEUE_INIT(&split_head);                                                \
      if (!QUEUE_EMPTY(&(handle)->cf_events)) {                               \
        QUEUE* split_pos = QUEUE_HEAD(&(handle)->cf_events);                  \
        QUEUE_SPLIT(&(handle)->cf_events, split_pos, &split_head);            \
      }                                                                       \
      uv_mutex_unlock(&(handle)->cf_mutex);                                   \
      while (!QUEUE_EMPTY(&split_head)) {                                     \
        curr = QUEUE_HEAD(&split_head);                                       \
        /* Invoke callback */                                                 \
        event = QUEUE_DATA(curr, uv__fsevents_event_t, member);               \
        QUEUE_REMOVE(curr);                                                   \
        /* Invoke block code, but only if handle wasn't closed */             \
        if (((handle)->flags & (UV_CLOSING | UV_CLOSED)) == 0)                \
          block                                                               \
        /* Free allocated data */                                             \
        free(event);                                                          \
      }                                                                       \
    }


static void uv__fsevents_cb(uv_async_t* cb, int status) {
  uv_fs_event_t* handle;

  handle = cb->data;

  UV__FSEVENTS_WALK(handle, {
    if (handle->event_watcher.fd != -1)
      handle->cb(handle, event->path[0] ? event->path : NULL, event->events, 0);
  });

  if ((handle->flags & (UV_CLOSING | UV_CLOSED)) == 0 &&
      handle->event_watcher.fd == -1) {
    uv__fsevents_close(handle);
  }
}


static void uv__fsevents_event_cb(ConstFSEventStreamRef streamRef,
                                  void* info,
                                  size_t numEvents,
                                  void* eventPaths,
                                  const FSEventStreamEventFlags eventFlags[],
                                  const FSEventStreamEventId eventIds[]) {
  size_t i;
  int len;
  char** paths;
  char* path;
  char* pos;
  uv_fs_event_t* handle;
  uv__fsevents_event_t* event;
  QUEUE add_list;
  int kFSEventsModified;
  int kFSEventsRenamed;

  kFSEventsModified = kFSEventStreamEventFlagItemFinderInfoMod |
                      kFSEventStreamEventFlagItemModified |
                      kFSEventStreamEventFlagItemInodeMetaMod |
                      kFSEventStreamEventFlagItemChangeOwner |
                      kFSEventStreamEventFlagItemXattrMod;
  kFSEventsRenamed = kFSEventStreamEventFlagItemCreated |
                     kFSEventStreamEventFlagItemRemoved |
                     kFSEventStreamEventFlagItemRenamed;

  handle = info;
  paths = eventPaths;
  QUEUE_INIT(&add_list);

  for (i = 0; i < numEvents; i++) {
    /* Ignore system events */
    if (eventFlags[i] & (kFSEventStreamEventFlagUserDropped |
                         kFSEventStreamEventFlagKernelDropped |
                         kFSEventStreamEventFlagEventIdsWrapped |
                         kFSEventStreamEventFlagHistoryDone |
                         kFSEventStreamEventFlagMount |
                         kFSEventStreamEventFlagUnmount |
                         kFSEventStreamEventFlagRootChanged)) {
      continue;
    }

    /* TODO: Report errors */
    path = paths[i];
    len = strlen(path);

    /* Remove absolute path prefix */
    if (strstr(path, handle->realpath) == path) {
      path += handle->realpath_len;
      len -= handle->realpath_len;

      /* Skip back slash */
      if (*path != 0) {
        path++;
        len--;
      }
    }

#ifdef MAC_OS_X_VERSION_10_7
    /* Ignore events with path equal to directory itself */
    if (len == 0)
      continue;
#endif /* MAC_OS_X_VERSION_10_7 */

    /* Do not emit events from subdirectories (without option set) */
    pos = strchr(path, '/');
    if ((handle->cf_flags & UV_FS_EVENT_RECURSIVE) == 0 &&
        pos != NULL &&
        pos != path + 1)
      continue;

#ifndef MAC_OS_X_VERSION_10_7
    path = "";
    len = 0;
#endif /* MAC_OS_X_VERSION_10_7 */

    event = malloc(sizeof(*event) + len);
    if (event == NULL)
      break;

    memcpy(event->path, path, len + 1);

    if ((eventFlags[i] & kFSEventsModified) != 0 &&
        (eventFlags[i] & kFSEventsRenamed) == 0)
      event->events = UV_CHANGE;
    else
      event->events = UV_RENAME;

    QUEUE_INSERT_TAIL(&add_list, &event->member);
  }
  uv_mutex_lock(&handle->cf_mutex);
  QUEUE_ADD(&handle->cf_events, &add_list);
  uv_mutex_unlock(&handle->cf_mutex);

  uv_async_send(handle->cf_cb);
}


static void uv__fsevents_schedule(void* arg) {
  uv_fs_event_t* handle;

  handle = arg;
  FSEventStreamScheduleWithRunLoop(handle->cf_eventstream,
                                   handle->loop->cf_loop,
                                   kCFRunLoopDefaultMode);
  FSEventStreamStart(handle->cf_eventstream);
  uv_sem_post(&handle->cf_sem);
}


static int uv__fsevents_loop_init(uv_loop_t* loop) {
  CFRunLoopSourceContext ctx;
  pthread_attr_t attr_storage;
  pthread_attr_t* attr;
  int err;

  if (loop->cf_loop != NULL)
    return 0;

  err = uv_mutex_init(&loop->cf_mutex);
  if (err)
    return err;

  err = uv_sem_init(&loop->cf_sem, 0);
  if (err)
    goto fail_sem_init;

  QUEUE_INIT(&loop->cf_signals);
  memset(&ctx, 0, sizeof(ctx));
  ctx.info = loop;
  ctx.perform = uv__cf_loop_cb;
  loop->cf_cb = CFRunLoopSourceCreate(NULL, 0, &ctx);

  /* In the unlikely event that pthread_attr_init() fails, create the thread
   * with the default stack size. We'll use a little more address space but
   * that in itself is not a fatal error.
   */
  attr = &attr_storage;
  if (pthread_attr_init(attr))
    attr = NULL;

  if (attr != NULL)
    if (pthread_attr_setstacksize(attr, 3 * PTHREAD_STACK_MIN))
      abort();

  /* uv_thread_t is an alias for pthread_t. */
  err = -pthread_create(&loop->cf_thread, attr, uv__cf_loop_runner, loop);

  if (attr != NULL)
    pthread_attr_destroy(attr);

  if (err)
    goto fail_thread_create;

  /* Synchronize threads */
  uv_sem_wait(&loop->cf_sem);
  assert(loop->cf_loop != NULL);
  return 0;

fail_thread_create:
  uv_sem_destroy(&loop->cf_sem);

fail_sem_init:
  uv_mutex_destroy(&loop->cf_mutex);
  return err;
}


void uv__fsevents_loop_delete(uv_loop_t* loop) {
  uv__cf_loop_signal_t* s;
  QUEUE* q;

  if (loop->cf_loop == NULL)
    return;

  uv__cf_loop_signal(loop, NULL, NULL);
  uv_thread_join(&loop->cf_thread);
  uv_sem_destroy(&loop->cf_sem);
  uv_mutex_destroy(&loop->cf_mutex);

  /* Free any remaining data */
  while (!QUEUE_EMPTY(&loop->cf_signals)) {
    q = QUEUE_HEAD(&loop->cf_signals);
    s = QUEUE_DATA(q, uv__cf_loop_signal_t, member);
    QUEUE_REMOVE(q);
    free(s);
  }
}


static void* uv__cf_loop_runner(void* arg) {
  uv_loop_t* loop;

  loop = arg;
  loop->cf_loop = CFRunLoopGetCurrent();

  CFRunLoopAddSource(loop->cf_loop,
                     loop->cf_cb,
                     kCFRunLoopDefaultMode);

  uv_sem_post(&loop->cf_sem);

  CFRunLoopRun();
  CFRunLoopRemoveSource(loop->cf_loop,
                        loop->cf_cb,
                        kCFRunLoopDefaultMode);

  return NULL;
}


static void uv__cf_loop_cb(void* arg) {
  uv_loop_t* loop;
  QUEUE* item;
  QUEUE split_head;
  uv__cf_loop_signal_t* s;

  loop = arg;

  uv_mutex_lock(&loop->cf_mutex);
  QUEUE_INIT(&split_head);
  if (!QUEUE_EMPTY(&loop->cf_signals)) {
    QUEUE* split_pos = QUEUE_HEAD(&loop->cf_signals);
    QUEUE_SPLIT(&loop->cf_signals, split_pos, &split_head);
  }
  uv_mutex_unlock(&loop->cf_mutex);

  while (!QUEUE_EMPTY(&split_head)) {
    item = QUEUE_HEAD(&split_head);

    s = QUEUE_DATA(item, uv__cf_loop_signal_t, member);

    /* This was a termination signal */
    if (s->cb == NULL)
      CFRunLoopStop(loop->cf_loop);
    else
      s->cb(s->arg);

    QUEUE_REMOVE(item);
    free(s);
  }
}


void uv__cf_loop_signal(uv_loop_t* loop, cf_loop_signal_cb cb, void* arg) {
  uv__cf_loop_signal_t* item;

  item = malloc(sizeof(*item));
  /* XXX: Fail */
  if (item == NULL)
    abort();

  item->arg = arg;
  item->cb = cb;

  uv_mutex_lock(&loop->cf_mutex);
  QUEUE_INSERT_TAIL(&loop->cf_signals, &item->member);
  uv_mutex_unlock(&loop->cf_mutex);

  assert(loop->cf_loop != NULL);
  CFRunLoopSourceSignal(loop->cf_cb);
  CFRunLoopWakeUp(loop->cf_loop);
}


int uv__fsevents_init(uv_fs_event_t* handle) {
  FSEventStreamContext ctx;
  FSEventStreamRef ref;
  CFStringRef path;
  CFArrayRef paths;
  CFAbsoluteTime latency;
  FSEventStreamCreateFlags flags;
  int err;

  err = uv__fsevents_loop_init(handle->loop);
  if (err)
    return err;

  /* Initialize context */
  ctx.version = 0;
  ctx.info = handle;
  ctx.retain = NULL;
  ctx.release = NULL;
  ctx.copyDescription = NULL;

  /* Get absolute path to file */
  handle->realpath = realpath(handle->filename, NULL);
  if (handle->realpath != NULL)
    handle->realpath_len = strlen(handle->realpath);

  /* Initialize paths array */
  path = CFStringCreateWithCString(NULL,
                                   handle->filename,
                                   CFStringGetSystemEncoding());
  paths = CFArrayCreate(NULL, (const void**)&path, 1, NULL);

  latency = 0.15;

  /* Set appropriate flags */
  flags = kFSEventStreamCreateFlagFileEvents;

  ref = FSEventStreamCreate(NULL,
                            &uv__fsevents_event_cb,
                            &ctx,
                            paths,
                            kFSEventStreamEventIdSinceNow,
                            latency,
                            flags);
  handle->cf_eventstream = ref;

  /*
   * Events will occur in other thread.
   * Initialize callback for getting them back into event loop's thread
   */
  handle->cf_cb = malloc(sizeof(*handle->cf_cb));
  if (handle->cf_cb == NULL)
    return -ENOMEM;

  handle->cf_cb->data = handle;
  uv_async_init(handle->loop, handle->cf_cb, uv__fsevents_cb);
  handle->cf_cb->flags |= UV__HANDLE_INTERNAL;
  uv_unref((uv_handle_t*) handle->cf_cb);

  uv_mutex_init(&handle->cf_mutex);
  uv_sem_init(&handle->cf_sem, 0);
  QUEUE_INIT(&handle->cf_events);

  uv__cf_loop_signal(handle->loop, uv__fsevents_schedule, handle);

  return 0;
}


int uv__fsevents_close(uv_fs_event_t* handle) {
  if (handle->cf_eventstream == NULL)
    return -EINVAL;

  /* Ensure that event stream was scheduled */
  uv_sem_wait(&handle->cf_sem);

  /* Stop emitting events */
  FSEventStreamStop(handle->cf_eventstream);

  /* Release stream */
  FSEventStreamInvalidate(handle->cf_eventstream);
  FSEventStreamRelease(handle->cf_eventstream);
  handle->cf_eventstream = NULL;

  uv_close((uv_handle_t*) handle->cf_cb, (uv_close_cb) free);

  /* Free data in queue */
  UV__FSEVENTS_WALK(handle, {
    /* NOP */
  })

  uv_mutex_destroy(&handle->cf_mutex);
  uv_sem_destroy(&handle->cf_sem);
  free(handle->realpath);
  handle->realpath = NULL;
  handle->realpath_len = 0;

  return 0;
}

#endif /* TARGET_OS_IPHONE */
