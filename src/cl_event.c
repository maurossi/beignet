/*
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Rong Yang <rong.r.yang@intel.com>
 */

#include "cl_event.h"
#include "cl_context.h"
#include "cl_utils.h"
#include "cl_alloc.h"
#include "cl_khr_icd.h"
#include "cl_kernel.h"
#include "cl_command_queue.h"

#include <assert.h>
#include <stdio.h>

void cl_event_update_last_events(cl_command_queue queue, int wait)
{
  cl_event last_event = get_last_event(queue);
  if(!last_event) return;
  cl_event next, now;
  now = last_event;
  while(now){
    next = now->last_next;//get next first in case set status maintain it
    cl_event_update_status(now,wait);//update event status
    now = next;
  }
}

void cl_event_insert_last_events(cl_command_queue queue,cl_event event)
{
  if(!event) return;
  cl_event last_event = get_last_event(queue);
  if(last_event){
    cl_event now = last_event;
    while(now->last_next)
      now = now->last_next;
    now->last_next = event;
    event->last_prev = now;
  }
  else set_last_event(queue,event);
}

static inline cl_bool
cl_event_is_gpu_command_type(cl_command_type type)
{
  switch(type) {
    case CL_COMMAND_COPY_BUFFER:
    case CL_COMMAND_FILL_BUFFER:
    case CL_COMMAND_COPY_IMAGE:
    case CL_COMMAND_COPY_IMAGE_TO_BUFFER:
    case CL_COMMAND_COPY_BUFFER_TO_IMAGE:
    case CL_COMMAND_COPY_BUFFER_RECT:
    case CL_COMMAND_TASK:
    case CL_COMMAND_NDRANGE_KERNEL:
      return CL_TRUE;
    default:
      return CL_FALSE;
  }
}

int cl_event_flush(cl_event event)
{
  int err = CL_SUCCESS;
  if(!event) {
    err = CL_INVALID_VALUE;
    return err;
  }

  assert(event->gpgpu_event != NULL);
  if (event->gpgpu) {
    err = cl_command_queue_flush_gpgpu(event->queue, event->gpgpu);
    cl_gpgpu_delete(event->gpgpu);
    event->gpgpu = NULL;
  }
  cl_gpgpu_event_flush(event->gpgpu_event);
  cl_event_insert_last_events(event->queue,event);
  return err;
}

cl_event cl_event_new(cl_context ctx, cl_command_queue queue, cl_command_type type, cl_bool emplict)
{
  cl_event event = NULL;
  GET_QUEUE_THREAD_GPGPU(queue);

  /* Allocate and inialize the structure itself */
  TRY_ALLOC_NO_ERR (event, CALLOC(struct _cl_event));
  CL_OBJECT_INIT_BASE(event, CL_OBJECT_EVENT_MAGIC);

  /* Append the event in the context event list */
  pthread_mutex_lock(&ctx->event_lock);
    event->next = ctx->events;
    if (ctx->events != NULL)
      ctx->events->prev = event;
    ctx->events = event;
  pthread_mutex_unlock(&ctx->event_lock);
  event->ctx   = ctx;
  cl_context_add_ref(ctx);

  /* Initialize all members and create GPGPU event object */
  event->queue = queue;
  event->type  = type;
  event->gpgpu_event = NULL;
  if(type == CL_COMMAND_USER) {
    event->status = CL_SUBMITTED;
  }
  else {
    event->status = CL_QUEUED;
    if(cl_event_is_gpu_command_type(event->type))
      event->gpgpu_event = cl_gpgpu_event_new(gpgpu);
  }
  cl_event_add_ref(event);       //dec when complete
  event->user_cb = NULL;
  event->enqueue_cb = NULL;
  event->waits_head = NULL;
  event->emplict = emplict;

exit:
  return event;
error:
  cl_event_delete(event);
  event = NULL;
  goto exit;
}

void cl_event_delete(cl_event event)
{
  if (UNLIKELY(event == NULL))
    return;

  cl_event_update_status(event, 0);

  if (CL_OBJECT_DEC_REF(event) > 1)
    return;

  /* Call all user's callback if haven't execute */
  cl_event_call_callback(event, CL_COMPLETE, CL_TRUE); // CL_COMPLETE status will force all callbacks that are not executed to run

  /* delete gpgpu event object */
  if(event->gpgpu_event)
    cl_gpgpu_event_delete(event->gpgpu_event);

  /* Remove it from the list */
  assert(event->ctx);
  pthread_mutex_lock(&event->ctx->event_lock);

  if (event->prev)
    event->prev->next = event->next;
  if (event->next)
    event->next->prev = event->prev;
  /* if this is the head, update head pointer ctx->events */
  if (event->ctx->events == event)
    event->ctx->events = event->next;

  pthread_mutex_unlock(&event->ctx->event_lock);
  cl_context_delete(event->ctx);

  if (event->gpgpu) {
    fprintf(stderr, "Warning: a event is deleted with a pending enqueued task.\n");
    cl_gpgpu_delete(event->gpgpu);
    event->gpgpu = NULL;
  }

  CL_OBJECT_DESTROY_BASE(event);
  cl_free(event);
}

void cl_event_add_ref(cl_event event)
{
  assert(event);
  CL_OBJECT_INC_REF(event);
}

cl_int cl_event_set_callback(cl_event event ,
                                  cl_int command_exec_callback_type,
                                  EVENT_NOTIFY pfn_notify,
                                  void* user_data)
{
  assert(event);
  assert(pfn_notify);

  cl_int err = CL_SUCCESS;
  user_callback *cb;
  TRY_ALLOC(cb, CALLOC(user_callback));

  cb->pfn_notify  = pfn_notify;
  cb->user_data   = user_data;
  cb->status      = command_exec_callback_type;
  cb->executed    = CL_FALSE;


  // It is possible that the event enqueued is already completed.
  // clEnqueueReadBuffer can be synchronous and when the callback
  // is registered after, it still needs to get executed.
  pthread_mutex_lock(&event->ctx->event_lock); // Thread safety required: operations on the event->status can be made from many different threads
  if(event->status <= command_exec_callback_type) {
    /* Call user callback */
    pthread_mutex_unlock(&event->ctx->event_lock); // pfn_notify can call clFunctions that use the event_lock and from here it's not required
    cb->pfn_notify(event, event->status, cb->user_data);
    cl_free(cb);
  } else {
    // Enqueue to callback list
    cb->next        = event->user_cb;
    event->user_cb  = cb;
    pthread_mutex_unlock(&event->ctx->event_lock);
  }

exit:
  return err;
error:
  err = CL_OUT_OF_HOST_MEMORY;
  cl_free(cb);
  goto exit;
};

cl_int cl_event_check_waitlist(cl_uint num_events_in_wait_list,
                                    const cl_event *event_wait_list,
                                    cl_event *event,cl_context ctx)
{
  cl_int err = CL_SUCCESS;
  cl_int i;
  /* check the event_wait_list and num_events_in_wait_list */
  if((event_wait_list == NULL) &&
     (num_events_in_wait_list > 0))
    goto error;

  if ((event_wait_list != NULL) &&
      (num_events_in_wait_list == 0)){
    goto error;
  }

  /* check the event and context */
  for(i=0; i<num_events_in_wait_list; i++) {
    CHECK_EVENT(event_wait_list[i]);
    if(event_wait_list[i]->status < CL_COMPLETE) {
      err = CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST;
      goto exit;
    }
    if(event && event == &event_wait_list[i])
      goto error;
    if(event_wait_list[i]->ctx != ctx) {
      err = CL_INVALID_CONTEXT;
      goto exit;
    }
  }

exit:
  return err;
error:
  err = CL_INVALID_EVENT_WAIT_LIST;  //reset error
  goto exit;
}

cl_int cl_event_wait_events(cl_uint num_events_in_wait_list, const cl_event *event_wait_list,
                            cl_command_queue queue)
{
  cl_int i;

  /* Check whether wait user events */
  for(i=0; i<num_events_in_wait_list; i++) {
    if(event_wait_list[i]->status <= CL_COMPLETE)
      continue;

    /* Need wait on user event, return and do enqueue defer */
    if((event_wait_list[i]->type == CL_COMMAND_USER) ||
       (event_wait_list[i]->enqueue_cb &&
       (event_wait_list[i]->enqueue_cb->wait_user_events != NULL))){
      return CL_ENQUEUE_EXECUTE_DEFER;
    }
  }

  if(queue && queue->barrier_events_num )
      return CL_ENQUEUE_EXECUTE_DEFER;

  /* Non user events or all user event finished, wait all enqueue events finish */
  for(i=0; i<num_events_in_wait_list; i++) {
    if(event_wait_list[i]->status <= CL_COMPLETE)
      continue;

    //enqueue callback haven't finish, in another thread, wait
    if(event_wait_list[i]->enqueue_cb != NULL)
      return CL_ENQUEUE_EXECUTE_DEFER;
    if(event_wait_list[i]->gpgpu_event)
      cl_gpgpu_event_update_status(event_wait_list[i]->gpgpu_event, 1);
    cl_event_set_status(event_wait_list[i], CL_COMPLETE);  //Execute user's callback
  }
  return CL_ENQUEUE_EXECUTE_IMM;
}

void cl_event_new_enqueue_callback(cl_event event,
                                            enqueue_data *data,
                                            cl_uint num_events_in_wait_list,
                                            const cl_event *event_wait_list)
{
  enqueue_callback *cb, *node;
  user_event *user_events, *u_ev;
  cl_command_queue queue = event ? event->queue : NULL;
  cl_int i;
  cl_int err = CL_SUCCESS;

  /* Allocate and initialize the structure itself */
  TRY_ALLOC_NO_ERR (cb, CALLOC(enqueue_callback));
  cb->num_events = 0;
  TRY_ALLOC_NO_ERR (cb->wait_list, CALLOC_ARRAY(cl_event, num_events_in_wait_list));
  for(i=0; i<num_events_in_wait_list; i++) {
    //user event will insert to cb->wait_user_events, need not in wait list, avoid ref twice
    if(event_wait_list[i]->type != CL_COMMAND_USER) {
      cb->wait_list[cb->num_events++] = event_wait_list[i];
      cl_event_add_ref(event_wait_list[i]);  //add defer enqueue's wait event reference
    }
  }
  cb->event = event;
  cb->next = NULL;
  cb->wait_user_events = NULL;

  if(queue && queue->barrier_events_num > 0) {
    for(i=0; i<queue->barrier_events_num; i++) {
      /* Insert the enqueue_callback to user event list */
      node = queue->wait_events[i]->waits_head;
      if(node == NULL)
        queue->wait_events[i]->waits_head = cb;
      else{
        while((node != cb) && node->next)
          node = node->next;
        if(node == cb)   //wait on dup user event
          continue;
        node->next = cb;
      }

      /* Insert the user event to enqueue_callback's wait_user_events */
      TRY(cl_event_insert_user_event, &cb->wait_user_events, queue->wait_events[i]);
      cl_event_add_ref(queue->wait_events[i]);
    }
  }

  /* Find out all user events that in event_wait_list wait */
  for(i=0; i<num_events_in_wait_list; i++) {
    if(event_wait_list[i]->status <= CL_COMPLETE)
      continue;

    if(event_wait_list[i]->type == CL_COMMAND_USER) {
      /* Insert the enqueue_callback to user event list */
      node = event_wait_list[i]->waits_head;
      if(node == NULL)
        event_wait_list[i]->waits_head = cb;
      else {
        while((node != cb) && node->next)
          node = node->next;
        if(node == cb)   //wait on dup user event
          continue;
        node->next = cb;
      }
      /* Insert the user event to enqueue_callback's wait_user_events */
      TRY(cl_event_insert_user_event, &cb->wait_user_events, event_wait_list[i]);
      cl_event_add_ref(event_wait_list[i]);
      if(queue)
        cl_command_queue_insert_event(queue, event_wait_list[i]);
      if(queue && data->type == EnqueueBarrier){
        cl_command_queue_insert_barrier_event(queue, event_wait_list[i]);
      }
    } else if(event_wait_list[i]->enqueue_cb != NULL) {
      user_events = event_wait_list[i]->enqueue_cb->wait_user_events;
      while(user_events != NULL) {
        /* Insert the enqueue_callback to user event's  waits_tail */
        node = user_events->event->waits_head;
        if(node == NULL)
          event_wait_list[i]->waits_head = cb;
        else{
          while((node != cb) && node->next)
            node = node->next;
          if(node == cb) {  //wait on dup user event
            user_events = user_events->next;
            continue;
          }
          node->next = cb;
        }

        /* Insert the user event to enqueue_callback's wait_user_events */
        TRY(cl_event_insert_user_event, &cb->wait_user_events, user_events->event);
        cl_event_add_ref(user_events->event);
        if(queue)
          cl_command_queue_insert_event(event->queue, user_events->event);
        if(queue && data->type == EnqueueBarrier){
          cl_command_queue_insert_barrier_event(event->queue, user_events->event);
        }
        user_events = user_events->next;
      }
    }
  }
  if(event != NULL && event->queue != NULL && event->gpgpu_event != NULL) {
    event->gpgpu = cl_thread_gpgpu_take(event->queue);
    data->ptr = (void *)event->gpgpu_event;
  }
  cb->data = *data;
  if(event)
    event->enqueue_cb = cb;

exit:
  return;
error:
  if(cb) {
    while(cb->wait_user_events) {
      u_ev = cb->wait_user_events;
      cb->wait_user_events = cb->wait_user_events->next;
      cl_event_delete(u_ev->event);
      cl_free(u_ev);
    }
    for(i=0; i<cb->num_events; i++) {
      if(cb->wait_list[i]) {
        cl_event_delete(cb->wait_list[i]);
      }
    }
    cl_free(cb);
  }
  goto exit;
}

void cl_event_call_callback(cl_event event, cl_int status, cl_bool free_cb) {
  user_callback *user_cb = NULL;
  user_callback *queue_cb = NULL; // For thread safety, we create a queue that holds user_callback's pfn_notify contents
  user_callback *temp_cb = NULL;
  user_cb = event->user_cb;
  pthread_mutex_lock(&event->ctx->event_lock);
  while(user_cb) {
    if(user_cb->status >= status
        && user_cb->executed == CL_FALSE) { // Added check to not execute a callback when it was already handled
      user_cb->executed = CL_TRUE;
      temp_cb = cl_malloc(sizeof(user_callback));
      if(!temp_cb) {
        break; // Out of memory
      }
      temp_cb->pfn_notify = user_cb->pfn_notify; // Minor struct copy to call ppfn_notify out of the pthread_mutex
      temp_cb->user_data = user_cb->user_data;
      if(free_cb) {
        cl_free(user_cb);
      }
      if(!queue_cb) {
        queue_cb = temp_cb;
        queue_cb->next = NULL;
      } else { // Enqueue First
        temp_cb->next = queue_cb;
        queue_cb = temp_cb;
      }
    }
    user_cb = user_cb->next;
  }
  pthread_mutex_unlock(&event->ctx->event_lock);

  // Calling the callbacks outside of the event_lock is required because the callback can call cl_api functions and get deadlocked
  while(queue_cb) { // For each callback queued, actually execute the callback
    queue_cb->pfn_notify(event, event->status, queue_cb->user_data);
    temp_cb = queue_cb;
    queue_cb = queue_cb->next;
    cl_free(temp_cb);
  }
}

void cl_event_set_status(cl_event event, cl_int status)
{
  cl_int ret, i;
  cl_event evt;

  pthread_mutex_lock(&event->ctx->event_lock);
  if(status >= event->status) {
    pthread_mutex_unlock(&event->ctx->event_lock);
    return;
  }
  if(event->status <= CL_COMPLETE) {
    event->status = status;    //have done enqueue before or doing in another thread
    pthread_mutex_unlock(&event->ctx->event_lock);
    return;
  }

  if(status <= CL_COMPLETE) {
    if(event->enqueue_cb) {
      if(status == CL_COMPLETE) {
        cl_enqueue_handle(event, &event->enqueue_cb->data);
        if(event->gpgpu_event)
          cl_gpgpu_event_update_status(event->gpgpu_event, 1);  //now set complet, need refine
      } else {
        if(event->gpgpu_event) {
          // Error then cancel the enqueued event.
          cl_gpgpu_delete(event->gpgpu);
          event->gpgpu = NULL;
        }
      }

      event->status = status;  //Change the event status after enqueue and befor unlock

      pthread_mutex_unlock(&event->ctx->event_lock);
      for(i=0; i<event->enqueue_cb->num_events; i++)
        cl_event_delete(event->enqueue_cb->wait_list[i]);
      pthread_mutex_lock(&event->ctx->event_lock);

      if(event->enqueue_cb->wait_list)
        cl_free(event->enqueue_cb->wait_list);
      cl_free(event->enqueue_cb);
      event->enqueue_cb = NULL;
    }
  }
  if(event->status >= status)  //maybe changed in other threads
    event->status = status;
  pthread_mutex_unlock(&event->ctx->event_lock);

  /* Call user callback */
  cl_event_call_callback(event, status, CL_FALSE);

  if(event->type == CL_COMMAND_USER) {
    /* Check all defer enqueue */
    enqueue_callback *cb, *enqueue_cb = event->waits_head;
    while(enqueue_cb) {
      /* Remove this user event in enqueue_cb, update the header if needed. */
      cl_event_remove_user_event(&enqueue_cb->wait_user_events, event);
      cl_event_delete(event);

      /* Still wait on other user events */
      if(enqueue_cb->wait_user_events != NULL) {
        enqueue_cb = enqueue_cb->next;
        continue;
      }

      //remove user event frome enqueue_cb's ctx
      cl_command_queue_remove_event(enqueue_cb->event->queue, event);
      cl_command_queue_remove_barrier_event(enqueue_cb->event->queue, event);

      /* All user events complete, now wait enqueue events */
      ret = cl_event_wait_events(enqueue_cb->num_events, enqueue_cb->wait_list,
          enqueue_cb->event->queue);
      assert(ret != CL_ENQUEUE_EXECUTE_DEFER);
      ret = ~ret;
      cb = enqueue_cb;
      enqueue_cb = enqueue_cb->next;

      /* Call the pending operation */
      evt = cb->event;
      /* TODO: if this event wait on several events, one event's
         status is error, the others is complete, what's the status
         of this event? Can't find the description in OpenCL spec.
         Simply update to latest finish wait event.*/
      cl_event_set_status(cb->event, status);
      if(evt->emplict == CL_FALSE) {
        cl_event_delete(evt);
      }
    }
    event->waits_head = NULL;
  }

  if(event->status <= CL_COMPLETE){
    /* Maintain the last_list when event completed*/
    if (event->last_prev)
      event->last_prev->last_next = event->last_next;
    if (event->last_next)
      event->last_next->last_prev = event->last_prev;
    if(event->queue && get_last_event(event->queue) == event)
      set_last_event(event->queue, event->last_next);
    event->last_prev = NULL;
    event->last_next = NULL;
    cl_event_delete(event);
  }
}

void cl_event_update_status(cl_event event, int wait)
{
  if(event->status <= CL_COMPLETE)
    return;
  if((event->gpgpu_event) &&
     (cl_gpgpu_event_update_status(event->gpgpu_event, wait) == command_complete))
    cl_event_set_status(event, CL_COMPLETE);
}

cl_int cl_event_marker_with_wait_list(cl_command_queue queue,
                cl_uint num_events_in_wait_list,
                const cl_event *event_wait_list,
                cl_event* event)
{
  enqueue_data data = { 0 };
  cl_event e;

  e = cl_event_new(queue->ctx, queue, CL_COMMAND_MARKER, CL_TRUE);
  if(e == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  if(event != NULL ){
    *event = e;
  }

//enqueues a marker command which waits for either a list of events to complete, or if the list is
//empty it waits for all commands previously enqueued in command_queue to complete before it  completes.
  if(num_events_in_wait_list > 0){
    if(cl_event_wait_events(num_events_in_wait_list, event_wait_list, queue) == CL_ENQUEUE_EXECUTE_DEFER) {
      data.type = EnqueueMarker;
      cl_event_new_enqueue_callback(event?*event:NULL, &data, num_events_in_wait_list, event_wait_list);
      return CL_SUCCESS;
    }
  } else if(queue->wait_events_num > 0) {
    data.type = EnqueueMarker;
    cl_event_new_enqueue_callback(event?*event:NULL, &data, queue->wait_events_num, queue->wait_events);
    return CL_SUCCESS;
  }

  cl_event_update_last_events(queue,1);

  cl_event_set_status(e, CL_COMPLETE);
  return CL_SUCCESS;
}

cl_int cl_event_barrier_with_wait_list(cl_command_queue queue,
                cl_uint num_events_in_wait_list,
                const cl_event *event_wait_list,
                cl_event* event)
{
  enqueue_data data = { 0 };
  cl_event e;

  e = cl_event_new(queue->ctx, queue, CL_COMMAND_BARRIER, CL_TRUE);
  if(e == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  if(event != NULL ){
    *event = e;
  }
//enqueues a barrier command which waits for either a list of events to complete, or if the list is
//empty it waits for all commands previously enqueued in command_queue to complete before it  completes.
  if(num_events_in_wait_list > 0){
    if(cl_event_wait_events(num_events_in_wait_list, event_wait_list, queue) == CL_ENQUEUE_EXECUTE_DEFER) {
      data.type = EnqueueBarrier;
      cl_event_new_enqueue_callback(e, &data, num_events_in_wait_list, event_wait_list);
      return CL_SUCCESS;
    }
  } else if(queue->wait_events_num > 0) {
    data.type = EnqueueBarrier;
    cl_event_new_enqueue_callback(e, &data, queue->wait_events_num, queue->wait_events);
    return CL_SUCCESS;
  }

  cl_event_update_last_events(queue,1);

  cl_event_set_status(e, CL_COMPLETE);
  return CL_SUCCESS;
}

cl_ulong cl_event_get_cpu_timestamp(cl_ulong *cpu_time)
{
  struct timespec ts;

 if(clock_gettime(CLOCK_MONOTONIC_RAW,&ts) != 0){
  printf("CPU Timmer error\n");
  return CL_FALSE;
  }
  *cpu_time = (1000000000.0) * (cl_ulong) ts.tv_sec + (cl_ulong) ts.tv_nsec;

  return CL_SUCCESS;
}

cl_int cl_event_get_queued_cpu_timestamp(cl_event event)
{
  cl_int ret_val;

  ret_val = cl_event_get_cpu_timestamp(&event->queued_timestamp);

  return ret_val;
}

cl_ulong cl_event_get_timestamp_delta(cl_ulong start_timestamp,cl_ulong end_timestamp)
{
  cl_ulong ret_val;

  if(end_timestamp > start_timestamp){
   ret_val = end_timestamp - start_timestamp;
   }
  else {
   /*if start time stamp is greater than end timstamp then set ret value to max*/
   ret_val = ((cl_ulong) 1 << 32);
  }

  return ret_val;
}

cl_ulong cl_event_get_start_timestamp(cl_event event)
{
  cl_ulong ret_val;

   ret_val = cl_event_get_timestamp_delta(event->timestamp[0],event->timestamp[2]);

  return ret_val;
}

cl_ulong cl_event_get_end_timestamp(cl_event event)
{
 cl_ulong ret_val;

  ret_val = cl_event_get_timestamp_delta(event->timestamp[0],event->timestamp[3]);

  return ret_val;
}

cl_int cl_event_get_timestamp(cl_event event, cl_profiling_info param_name)
{
  cl_ulong ret_val = 0;
  GET_QUEUE_THREAD_GPGPU(event->queue);

  if (!event->gpgpu_event) {
    cl_gpgpu_event_get_gpu_cur_timestamp(gpgpu, &ret_val);
    event->timestamp[param_name - CL_PROFILING_COMMAND_QUEUED] = ret_val;
    return CL_SUCCESS;
  }

  if(param_name == CL_PROFILING_COMMAND_SUBMIT ||
         param_name == CL_PROFILING_COMMAND_QUEUED) {
    cl_gpgpu_event_get_gpu_cur_timestamp(gpgpu, &ret_val);
    event->timestamp[param_name - CL_PROFILING_COMMAND_QUEUED] = ret_val;
    return CL_SUCCESS;
  } else if(param_name == CL_PROFILING_COMMAND_START) {
    cl_gpgpu_event_get_exec_timestamp(gpgpu, event->gpgpu_event, 0, &ret_val);
    event->timestamp[param_name - CL_PROFILING_COMMAND_QUEUED] = ret_val;
    return CL_SUCCESS;
  } else if (param_name == CL_PROFILING_COMMAND_END) {
    cl_gpgpu_event_get_exec_timestamp(gpgpu, event->gpgpu_event, 1, &ret_val);
    event->timestamp[param_name - CL_PROFILING_COMMAND_QUEUED] = ret_val;
    return CL_SUCCESS;
  }
  return CL_INVALID_VALUE;
}

cl_int cl_event_insert_user_event(user_event** p_u_ev, cl_event event)
{
  user_event * u_iter = *p_u_ev;
  user_event * u_ev;

  while(u_iter)
  {
    if(u_iter->event == event)
      return CL_SUCCESS;
    u_iter = u_iter->next;
  }

  TRY_ALLOC_NO_ERR (u_ev, CALLOC(user_event));
  u_ev->event = event;
  u_ev->next = *p_u_ev;
  *p_u_ev = u_ev;


  return CL_SUCCESS;
error:
  return CL_FALSE;
}

cl_int cl_event_remove_user_event(user_event** p_u_ev, cl_event event)
{
  user_event * u_iter = *p_u_ev;
  user_event * u_prev = *p_u_ev;

  while(u_iter){
    if(u_iter->event == event ){
      if(u_iter == *p_u_ev){
        *p_u_ev = u_iter->next;
      }else{
        u_prev->next = u_iter->next;
      }
      cl_free(u_iter);
      break;
    }
    u_prev = u_iter;
    u_iter = u_iter->next;
  }

  return CL_SUCCESS;
}
