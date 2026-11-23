// Task queues and asynchronous call blocks, as the game runtime provides them.
//
// Types and semantics are the documented ones of the public desktop headers
// XTaskQueue.h, XAsync.h and XAsyncProvider.h. Other services of this runtime
// use the same machinery for their own asynchronous calls.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace holy::xgr {

// --- public types (documented) ----------------------------------------------

enum class DispatchMode : uint32_t { Manual = 0, ThreadPool = 1, SerializedThreadPool = 2, Immediate = 3 };
enum class Port : uint32_t { Work = 0, Completion = 1 };

using TaskQueueCallback = void(CALLBACK*)(void* context, bool canceled);
using TerminatedCallback = void(CALLBACK*)(void* context);
using MonitorCallback = void(CALLBACK*)(void* context, void* queue, Port port);

struct RegistrationToken {
  uint64_t token;
};

struct AsyncBlock;
using AsyncCompletionRoutine = void(CALLBACK*)(AsyncBlock* block);
using AsyncWork = HRESULT(CALLBACK*)(AsyncBlock* block);

struct AsyncBlock {
  void* queue;
  void* context;
  AsyncCompletionRoutine callback;
  unsigned char internal[sizeof(void*) * 4];
};

enum class AsyncOp : uint32_t { Begin = 0, DoWork = 1, GetResult = 2, Cancel = 3, Cleanup = 4 };

struct AsyncProviderData {
  AsyncBlock* async;
  size_t buffer_size;
  void* buffer;
  void* context;
};

using AsyncProvider = HRESULT(CALLBACK*)(AsyncOp op, const AsyncProviderData* data);

// --- task queue API ------------------------------------------------------------

HRESULT task_queue_create(DispatchMode work, DispatchMode completion, void** queue);
HRESULT task_queue_create_composite(void* work_port, void* completion_port, void** queue);
HRESULT task_queue_get_port(void* queue, Port port, void** port_handle);
HRESULT task_queue_duplicate_handle(void* queue, void** duplicate);
bool task_queue_dispatch(void* queue, Port port, uint32_t timeout_ms);
void task_queue_close_handle(void* queue);
HRESULT task_queue_terminate(void* queue, bool wait, void* context, TerminatedCallback callback);
HRESULT task_queue_submit_callback(void* queue, Port port, void* context, TaskQueueCallback callback);
HRESULT task_queue_submit_delayed_callback(void* queue, Port port, uint32_t delay_ms, void* context,
                                           TaskQueueCallback callback);
HRESULT task_queue_register_waiter(void* queue, Port port, HANDLE wait_handle, void* context,
                                   TaskQueueCallback callback, RegistrationToken* token);
void task_queue_unregister_waiter(void* queue, RegistrationToken token);
HRESULT task_queue_register_monitor(void* queue, void* context, MonitorCallback callback, RegistrationToken* token);
void task_queue_unregister_monitor(void* queue, RegistrationToken token);
bool task_queue_get_current_process_task_queue(void** queue);
void task_queue_set_current_process_task_queue(void* queue);

// --- asynchronous calls ----------------------------------------------------------

HRESULT async_get_status(AsyncBlock* block, bool wait);
HRESULT async_get_result_size(AsyncBlock* block, size_t* size);
void async_cancel(AsyncBlock* block);
HRESULT async_run(AsyncBlock* block, AsyncWork work);
HRESULT async_begin(AsyncBlock* block, void* context, const void* identity, const char* identity_name,
                    AsyncProvider provider);
HRESULT async_schedule(AsyncBlock* block, uint32_t delay_ms);
void async_complete(AsyncBlock* block, HRESULT result, size_t required_size);
HRESULT async_get_result(AsyncBlock* block, const void* identity, size_t buffer_size, void* buffer,
                         size_t* buffer_used);

// --- time-sensitive threads -----------------------------------------------------

bool thread_is_time_sensitive();
HRESULT thread_set_time_sensitive(bool value);
void thread_assert_not_time_sensitive();

}  // namespace holy::xgr
