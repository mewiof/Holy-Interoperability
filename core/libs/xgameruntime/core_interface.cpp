// The runtime's core interface: asynchronous calls, task queues and the
// time-sensitive thread flag.
//
// Observed: interface {073b7dcb-1fcf-4030-94be-e3c9eb623428}. The method in
// each slot was identified from the thunk that calls it (argument count,
// widths and return handling) matched against the documented declarations of
// XAsync.h, XAsyncProvider.h, XTaskQueue.h and XThread.h, which the slots
// follow in order. Only slots whose thunk has been seen are bound; the rest
// stay reporting stubs.
//
//   3 XAsyncGetStatus            12 XTaskQueueCreate                 22 XTaskQueueTerminate
//   4 XAsyncGetResultSize        13 XTaskQueueCreateComposite        25 XTaskQueueGetCurrentProcessTaskQueue
//   5 XAsyncCancel               14 XTaskQueueGetPort                29 XThreadAssertNotTimeSensitive
//   6 XAsyncRun                  15 XTaskQueueDuplicateHandle
//   7 XAsyncBegin                16 XTaskQueueDispatch
//   9 XAsyncSchedule             17 XTaskQueueCloseHandle
//  10 XAsyncComplete             19 XTaskQueueSubmitDelayedCallback
//  11 XAsyncGetResult
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"

namespace holy::xgr {
namespace {

HRESULT STDMETHODCALLTYPE get_status(Object*, AsyncBlock* block, bool wait) {
  return async_get_status(block, wait);
}
HRESULT STDMETHODCALLTYPE get_result_size(Object*, AsyncBlock* block, size_t* size) {
  return async_get_result_size(block, size);
}
void STDMETHODCALLTYPE cancel(Object*, AsyncBlock* block) {
  async_cancel(block);
}
HRESULT STDMETHODCALLTYPE run(Object*, AsyncBlock* block, AsyncWork work) {
  return async_run(block, work);
}
HRESULT STDMETHODCALLTYPE begin(Object*, AsyncBlock* block, void* context, const void* identity,
                                const char* identity_name, AsyncProvider provider) {
  return async_begin(block, context, identity, identity_name, provider);
}
HRESULT STDMETHODCALLTYPE schedule(Object*, AsyncBlock* block, uint32_t delay_ms) {
  return async_schedule(block, delay_ms);
}
void STDMETHODCALLTYPE complete(Object*, AsyncBlock* block, HRESULT result, size_t required_size) {
  async_complete(block, result, required_size);
}
HRESULT STDMETHODCALLTYPE get_result(Object*, AsyncBlock* block, const void* identity, size_t buffer_size,
                                     void* buffer, size_t* buffer_used) {
  return async_get_result(block, identity, buffer_size, buffer, buffer_used);
}

HRESULT STDMETHODCALLTYPE queue_create(Object*, DispatchMode work, DispatchMode completion, void** queue) {
  return task_queue_create(work, completion, queue);
}
HRESULT STDMETHODCALLTYPE queue_create_composite(Object*, void* work_port, void* completion_port, void** queue) {
  return task_queue_create_composite(work_port, completion_port, queue);
}
HRESULT STDMETHODCALLTYPE queue_get_port(Object*, void* queue, Port port, void** port_handle) {
  return task_queue_get_port(queue, port, port_handle);
}
HRESULT STDMETHODCALLTYPE queue_duplicate_handle(Object*, void* queue, void** duplicate) {
  return task_queue_duplicate_handle(queue, duplicate);
}
bool STDMETHODCALLTYPE queue_dispatch(Object*, void* queue, Port port, uint32_t timeout_ms) {
  return task_queue_dispatch(queue, port, timeout_ms);
}
void STDMETHODCALLTYPE queue_close_handle(Object*, void* queue) {
  task_queue_close_handle(queue);
}
HRESULT STDMETHODCALLTYPE queue_submit_delayed_callback(Object*, void* queue, Port port, uint32_t delay_ms,
                                                        void* context, TaskQueueCallback callback) {
  return task_queue_submit_delayed_callback(queue, port, delay_ms, context, callback);
}
HRESULT STDMETHODCALLTYPE queue_terminate(Object*, void* queue, bool wait, void* context,
                                          TerminatedCallback callback) {
  return task_queue_terminate(queue, wait, context, callback);
}
bool STDMETHODCALLTYPE queue_get_current_process_task_queue(Object*, void** queue) {
  return task_queue_get_current_process_task_queue(queue);
}
void STDMETHODCALLTYPE assert_not_time_sensitive(Object*) {
  thread_assert_not_time_sensitive();
}

}  // namespace

void register_core_interface() {
  // {073b7dcb-1fcf-4030-94be-e3c9eb623428}
  constexpr GUID kCore = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
  Interface& core = define_interface(kCore, "core");
  set_method(core, 3, "XAsyncGetStatus", &get_status);
  set_method(core, 4, "XAsyncGetResultSize", &get_result_size);
  set_method(core, 5, "XAsyncCancel", &cancel);
  set_method(core, 6, "XAsyncRun", &run);
  set_method(core, 7, "XAsyncBegin", &begin);
  set_method(core, 9, "XAsyncSchedule", &schedule);
  set_method(core, 10, "XAsyncComplete", &complete);
  set_method(core, 11, "XAsyncGetResult", &get_result);
  set_method(core, 12, "XTaskQueueCreate", &queue_create);
  set_method(core, 13, "XTaskQueueCreateComposite", &queue_create_composite);
  set_method(core, 14, "XTaskQueueGetPort", &queue_get_port);
  set_method(core, 15, "XTaskQueueDuplicateHandle", &queue_duplicate_handle);
  set_method(core, 16, "XTaskQueueDispatch", &queue_dispatch);
  set_method(core, 17, "XTaskQueueCloseHandle", &queue_close_handle);
  set_method(core, 19, "XTaskQueueSubmitDelayedCallback", &queue_submit_delayed_callback);
  set_method(core, 22, "XTaskQueueTerminate", &queue_terminate);
  set_method(core, 25, "XTaskQueueGetCurrentProcessTaskQueue", &queue_get_current_process_task_queue);
  set_method(core, 29, "XThreadAssertNotTimeSensitive", &assert_not_time_sensitive);
}

}  // namespace holy::xgr
