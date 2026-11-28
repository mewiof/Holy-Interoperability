// Task queues and asynchronous call blocks.
//
// Semantics follow the public desktop headers XTaskQueue.h, XAsync.h and
// XAsyncProvider.h and their documentation:
//
// Task queues
//   - A queue has a work port and a completion port, each with a dispatch
//     mode: Manual (callbacks wait for XTaskQueueDispatch), ThreadPool,
//     SerializedThreadPool (thread pool, one callback at a time, in order) or
//     Immediate (run on the submitting thread).
//   - Composite queues are built from ports of other queues and share them.
//   - Handles are reference counted; XTaskQueueDuplicateHandle adds a
//     reference, XTaskQueueCloseHandle drops one.
//   - Monitors are told when a callback is submitted to a port of the queue.
//   - Termination cancels pending callbacks (they run with canceled = true),
//     refuses new ones, then runs the terminated callback.
//   - The process task queue is created on first use with thread-pool work
//     and completion ports. XTaskQueueGetCurrentProcessTaskQueue returns a
//     duplicated handle the caller closes.
//
// Asynchronous calls
//   - XAsyncBegin starts a call with a provider; the provider sees Begin,
//     DoWork (scheduled through XAsyncSchedule), GetResult, Cancel and
//     Cleanup. DoWork returning anything other than E_PENDING completes the
//     call with that result.
//   - On completion the block's completion routine runs on the completion
//     port. A call without result data (or a failed one) is cleaned up at
//     completion, otherwise when the result is retrieved.
//   - Waiters (XAsyncGetStatus with wait) are released after the completion
//     routine has run, so a caller that waited may free the block.
//   - The block's internal bytes hold the call state, so status and result
//     size stay readable after the provider has been cleaned up.
#include "core/libs/xgameruntime/task_queue.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <vector>

#include "core/libs/support.h"

namespace holy::xgr {
namespace {

constexpr char kComponent[] = "xtaskqueue";
constexpr uint32_t kQueueSignature = 0x55515448;  // "HTQU"
constexpr uint32_t kPortSignature = 0x50515448;   // "HTQP"
constexpr uint32_t kAsyncSignature = 0x43594148;  // "HAYC"

// Documented error codes.
constexpr HRESULT kErrorNotSufficientBuffer = static_cast<HRESULT>(0x8007007A);
constexpr HRESULT kErrorIllegalMethodCall = static_cast<HRESULT>(0x8000000E);

class Lock {
 public:
  explicit Lock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
  ~Lock() { ReleaseSRWLockExclusive(&lock_); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  SRWLOCK& lock_;
};

struct Entry {
  void* context;
  TaskQueueCallback callback;
};

struct PortState;
struct QueueState;

struct Monitor {
  uint64_t token;
  QueueState* queue;  // the queue the monitor was registered through (not referenced)
  void* context;
  MonitorCallback callback;
};

struct DelayedJob {
  PortState* port;
  Entry entry;
  PTP_TIMER timer;
};

struct Waiter {
  uint64_t token;
  PortState* port;
  Entry entry;
  PTP_WAIT wait;
};

struct PortState {
  uint32_t signature = kPortSignature;
  DispatchMode mode = DispatchMode::ThreadPool;
  Port id = Port::Work;
  std::atomic<long> refs{1};
  SRWLOCK lock = SRWLOCK_INIT;
  CONDITION_VARIABLE available = CONDITION_VARIABLE_INIT;
  CONDITION_VARIABLE idle = CONDITION_VARIABLE_INIT;
  std::deque<Entry> ready;  // Manual and SerializedThreadPool
  std::vector<DelayedJob*> delayed;
  std::vector<Monitor> monitors;
  bool serial_running = false;
  bool terminated = false;
  long in_flight = 0;  // callbacks submitted to the thread pool or running
};

struct QueueState {
  uint32_t signature = kQueueSignature;
  std::atomic<long> refs{1};
  PortState* ports[2] = {};
  SRWLOCK lock = SRWLOCK_INIT;
  std::vector<Waiter*> waiters;
  bool terminated = false;
};

std::atomic<uint64_t> g_next_token{1};

void port_add_ref(PortState* port) {
  port->refs.fetch_add(1);
}

void port_release(PortState* port) {
  if (port->refs.fetch_sub(1) == 1) {
    port->signature = 0;
    delete port;
  }
}

QueueState* as_queue(void* handle) {
  auto* queue = static_cast<QueueState*>(handle);
  if (!queue || queue->signature != kQueueSignature) return nullptr;
  return queue;
}

PortState* as_port(void* handle) {
  auto* port = static_cast<PortState*>(handle);
  if (!port || port->signature != kPortSignature) return nullptr;
  return port;
}

void run_entry(const Entry& entry, bool canceled) {
  entry.callback(entry.context, canceled);
}

void finish_in_flight(PortState* port) {
  Lock lock(port->lock);
  if (--port->in_flight == 0) WakeAllConditionVariable(&port->idle);
}

struct PoolJob {
  PortState* port;
  Entry entry;
};

void CALLBACK pool_callback(PTP_CALLBACK_INSTANCE, void* parameter) {
  auto* job = static_cast<PoolJob*>(parameter);
  bool canceled;
  {
    Lock lock(job->port->lock);
    canceled = job->port->terminated;
  }
  run_entry(job->entry, canceled);
  finish_in_flight(job->port);
  port_release(job->port);
  delete job;
}

void CALLBACK serial_drain(PTP_CALLBACK_INSTANCE, void* parameter) {
  auto* port = static_cast<PortState*>(parameter);
  for (;;) {
    Entry entry;
    bool canceled;
    {
      Lock lock(port->lock);
      if (port->ready.empty()) {
        port->serial_running = false;
        if (--port->in_flight == 0) WakeAllConditionVariable(&port->idle);
        break;
      }
      entry = port->ready.front();
      port->ready.pop_front();
      canceled = port->terminated;
    }
    run_entry(entry, canceled);
  }
  port_release(port);
}

void notify_monitors(PortState* port) {
  std::vector<Monitor> monitors;
  {
    Lock lock(port->lock);
    monitors = port->monitors;
  }
  for (const Monitor& monitor : monitors) monitor.callback(monitor.context, monitor.queue, port->id);
}

// Hands a due callback to the port according to its dispatch mode.
HRESULT enqueue(PortState* port, const Entry& entry) {
  switch (port->mode) {
    case DispatchMode::Immediate: {
      bool canceled;
      {
        Lock lock(port->lock);
        canceled = port->terminated;
      }
      notify_monitors(port);
      run_entry(entry, canceled);
      return S_OK;
    }
    case DispatchMode::Manual: {
      {
        Lock lock(port->lock);
        if (port->terminated) return E_ABORT;
        port->ready.push_back(entry);
        WakeConditionVariable(&port->available);
      }
      notify_monitors(port);
      return S_OK;
    }
    case DispatchMode::SerializedThreadPool: {
      bool start = false;
      {
        Lock lock(port->lock);
        if (port->terminated) return E_ABORT;
        port->ready.push_back(entry);
        if (!port->serial_running) {
          port->serial_running = true;
          ++port->in_flight;
          start = true;
        }
      }
      notify_monitors(port);
      if (start) {
        port_add_ref(port);
        if (!TrySubmitThreadpoolCallback(&serial_drain, port, nullptr)) {
          LIB_LOG(holy::lib::kError, kComponent, "thread pool refused a callback (error %lu)", GetLastError());
          port_release(port);
          return E_OUTOFMEMORY;
        }
      }
      return S_OK;
    }
    case DispatchMode::ThreadPool:
    default: {
      {
        Lock lock(port->lock);
        if (port->terminated) return E_ABORT;
        ++port->in_flight;
      }
      notify_monitors(port);
      port_add_ref(port);
      auto* job = new PoolJob{port, entry};
      if (!TrySubmitThreadpoolCallback(&pool_callback, job, nullptr)) {
        LIB_LOG(holy::lib::kError, kComponent, "thread pool refused a callback (error %lu)", GetLastError());
        delete job;
        finish_in_flight(port);
        port_release(port);
        return E_OUTOFMEMORY;
      }
      return S_OK;
    }
  }
}

void CALLBACK delayed_due(PTP_CALLBACK_INSTANCE, void* parameter, PTP_TIMER) {
  auto* job = static_cast<DelayedJob*>(parameter);
  PortState* port = job->port;
  bool owned = false;
  {
    Lock lock(port->lock);
    for (auto it = port->delayed.begin(); it != port->delayed.end(); ++it) {
      if (*it == job) {
        port->delayed.erase(it);
        owned = true;
        break;
      }
    }
  }
  // When termination already took the job it also runs and frees it.
  if (!owned) return;
  if (FAILED(enqueue(port, job->entry))) run_entry(job->entry, true);
  CloseThreadpoolTimer(job->timer);
  port_release(port);
  delete job;
}

HRESULT submit(PortState* port, uint32_t delay_ms, const Entry& entry) {
  if (delay_ms == 0) return enqueue(port, entry);
  auto* job = new DelayedJob{port, entry, nullptr};
  job->timer = CreateThreadpoolTimer(&delayed_due, job, nullptr);
  if (!job->timer) {
    delete job;
    return E_OUTOFMEMORY;
  }
  {
    Lock lock(port->lock);
    if (port->terminated) {
      CloseThreadpoolTimer(job->timer);
      delete job;
      return E_ABORT;
    }
    port->delayed.push_back(job);
  }
  port_add_ref(port);
  // Relative due time in 100 ns units.
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(delay_ms) * 10000;
  FILETIME due_time;
  due_time.dwLowDateTime = due.LowPart;
  due_time.dwHighDateTime = static_cast<DWORD>(due.HighPart);
  SetThreadpoolTimer(job->timer, &due_time, 0, 0);
  return S_OK;
}

// Cancels everything still pending on a port and waits for callbacks already
// running elsewhere to finish.
void terminate_port(PortState* port) {
  std::deque<Entry> ready;
  std::vector<DelayedJob*> delayed;
  {
    Lock lock(port->lock);
    if (port->terminated) return;
    port->terminated = true;
    if (port->mode == DispatchMode::Manual) ready.swap(port->ready);
    delayed.swap(port->delayed);
    WakeAllConditionVariable(&port->available);
  }
  for (DelayedJob* job : delayed) {
    SetThreadpoolTimer(job->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(job->timer, TRUE);
    CloseThreadpoolTimer(job->timer);
    run_entry(job->entry, true);
    port_release(port);
    delete job;
  }
  for (const Entry& entry : ready) run_entry(entry, true);
  Lock lock(port->lock);
  while (port->in_flight > 0) SleepConditionVariableSRW(&port->idle, &port->lock, INFINITE, 0);
}

PortState* new_port(DispatchMode mode, Port id) {
  auto* port = new PortState();
  port->mode = mode;
  port->id = id;
  return port;
}

bool valid_mode(DispatchMode mode) {
  return static_cast<uint32_t>(mode) <= static_cast<uint32_t>(DispatchMode::Immediate);
}

bool valid_port(Port port) {
  return port == Port::Work || port == Port::Completion;
}

// --- process task queue ---------------------------------------------------------

SRWLOCK g_process_lock = SRWLOCK_INIT;
QueueState* g_process_queue = nullptr;
bool g_process_queue_set = false;  // an explicit Set (even to null) disables the default

void queue_add_ref(QueueState* queue) {
  queue->refs.fetch_add(1);
}

void queue_release(QueueState* queue) {
  if (queue->refs.fetch_sub(1) != 1) return;
  for (Waiter* waiter : queue->waiters) {
    SetThreadpoolWait(waiter->wait, nullptr, nullptr);
    WaitForThreadpoolWaitCallbacks(waiter->wait, TRUE);
    CloseThreadpoolWait(waiter->wait);
    port_release(waiter->port);
    delete waiter;
  }
  for (PortState* port : queue->ports) port_release(port);
  queue->signature = 0;
  delete queue;
}

QueueState* process_queue_reference() {
  Lock lock(g_process_lock);
  if (!g_process_queue && !g_process_queue_set) {
    auto* queue = new QueueState();
    queue->ports[0] = new_port(DispatchMode::ThreadPool, Port::Work);
    queue->ports[1] = new_port(DispatchMode::ThreadPool, Port::Completion);
    g_process_queue = queue;
  }
  if (g_process_queue) queue_add_ref(g_process_queue);
  return g_process_queue;
}

// A null queue handle means the process task queue.
QueueState* resolve_queue(void* handle) {
  if (!handle) return process_queue_reference();
  QueueState* queue = as_queue(handle);
  if (queue) queue_add_ref(queue);
  return queue;
}

void CALLBACK waiter_signalled(PTP_CALLBACK_INSTANCE, void* parameter, PTP_WAIT, TP_WAIT_RESULT) {
  auto* waiter = static_cast<Waiter*>(parameter);
  if (FAILED(enqueue(waiter->port, waiter->entry))) run_entry(waiter->entry, true);
}

// --- asynchronous calls --------------------------------------------------------------

// What a block's internal bytes hold (this runtime's own layout). The call
// state is named by pool slot and generation rather than by pointer: states
// live in a pool that is never freed, so a block that outlived its call (or
// was freed and reused by the title) can always be checked safely.
struct BlockInternal {
  uint32_t slot;
  uint32_t generation;
  std::atomic<HRESULT> status;
  uint32_t signature;
  const void* identity;
  size_t required;
};
static_assert(sizeof(BlockInternal) <= sizeof(AsyncBlock::internal));

struct AsyncState {
  uint32_t slot = 0;
  uint32_t generation = 0;
  long refs = 0;  // guarded by g_async_lock
  SRWLOCK lock = SRWLOCK_INIT;
  AsyncBlock* block = nullptr;
  AsyncProvider provider = nullptr;
  void* context = nullptr;
  const char* identity_name = nullptr;
  uint64_t begun_ms = 0;     // when it began (slow operations are logged)
  AsyncWork work = nullptr;  // XAsyncRun
  QueueState* queue = nullptr;
  HANDLE done = nullptr;  // manual reset, set once waiters may return
  bool completed = false;
  bool cleanup_started = false;
  bool cleaned = false;
  bool signaled = false;
  bool block_reference = false;  // the block's reference is still held
};

SRWLOCK g_async_lock = SRWLOCK_INIT;  // guards the pool and reference counts
std::deque<AsyncState>& pool() {
  static auto* storage = new std::deque<AsyncState>();
  return *storage;
}
std::vector<uint32_t>& free_slots() {
  static auto* storage = new std::vector<uint32_t>();
  return *storage;
}

BlockInternal* internal_of(AsyncBlock* block) {
  return reinterpret_cast<BlockInternal*>(block->internal);
}

// A fresh state with one reference (the block's).
AsyncState* allocate_state() {
  Lock lock(g_async_lock);
  AsyncState* state;
  if (!free_slots().empty()) {
    state = &pool()[free_slots().back()];
    free_slots().pop_back();
  } else {
    state = &pool().emplace_back();
    state->slot = static_cast<uint32_t>(pool().size() - 1);
  }
  const uint32_t slot = state->slot;
  const uint32_t generation = state->generation;
  *state = AsyncState{};
  state->slot = slot;
  state->generation = generation;
  state->refs = 1;
  state->block_reference = true;
  return state;
}

void async_release(AsyncState* state) {
  QueueState* queue = nullptr;
  HANDLE done = nullptr;
  {
    Lock lock(g_async_lock);
    if (--state->refs != 0) return;
    queue = state->queue;
    done = state->done;
    state->queue = nullptr;
    state->done = nullptr;
    ++state->generation;
    free_slots().push_back(state->slot);
  }
  if (done) CloseHandle(done);
  if (queue) queue_release(queue);
}

// Returns the block's live call state with a reference, or null.
AsyncState* acquire_state(AsyncBlock* block) {
  const BlockInternal* internal = internal_of(block);
  if (internal->signature != kAsyncSignature) return nullptr;
  Lock lock(g_async_lock);
  if (internal->slot >= pool().size()) return nullptr;
  AsyncState* state = &pool()[internal->slot];
  if (state->generation != internal->generation || state->refs == 0 || !state->block_reference) return nullptr;
  ++state->refs;
  return state;
}

// Drops the block's reference once the provider is cleaned up and waiters
// have been released. Never touches the block, which the title may already
// have freed in its completion routine.
void drop_block_reference_when_finished(AsyncState* state) {
  bool drop = false;
  {
    Lock lock(g_async_lock);
    if (state->block_reference && state->cleaned && state->signaled) {
      state->block_reference = false;
      drop = true;
    }
  }
  if (drop) async_release(state);
}

void signal(AsyncState* state) {
  SetEvent(state->done);
  {
    Lock lock(g_async_lock);
    state->signaled = true;
  }
  drop_block_reference_when_finished(state);
}

// Runs the provider's Cleanup once.
void cleanup(AsyncState* state) {
  {
    Lock lock(state->lock);
    if (state->cleanup_started) return;
    state->cleanup_started = true;
  }
  if (state->provider) {
    AsyncProviderData data{state->block, 0, nullptr, state->context};
    state->provider(AsyncOp::Cleanup, &data);
  }
  {
    Lock lock(g_async_lock);
    state->cleaned = true;
  }
  drop_block_reference_when_finished(state);
}

struct CompletionJob {
  AsyncState* state;
};

void CALLBACK completion_callback(void* context, bool) {
  auto* job = static_cast<CompletionJob*>(context);
  AsyncState* state = job->state;
  delete job;
  const AsyncCompletionRoutine routine = state->block->callback;
  if (routine) routine(state->block);
  signal(state);
  async_release(state);
}

void complete(AsyncState* state, HRESULT result, size_t required) {
  if (result == E_PENDING) result = E_UNEXPECTED;
  {
    Lock lock(state->lock);
    if (state->completed) return;
    state->completed = true;
  }
  // Operations that take long hold up titles that wait for them: the first
  // ones logged (by identity), and counted.
  if (state->begun_ms && GetTickCount64() - state->begun_ms >= 1000) {
    static const holy::lib::Counter slow("xgameruntime.async_operations_over_1s");
    slow.add(1);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 16) {
      LIB_LOG(holy::lib::kWarn, "async", "an asynchronous operation (%s) took %llu ms (0x%08lX)",
              state->identity_name ? state->identity_name : "unnamed",
              static_cast<unsigned long long>(GetTickCount64() - state->begun_ms), static_cast<unsigned long>(result));
    }
  }
  BlockInternal* internal = internal_of(state->block);
  internal->required = SUCCEEDED(result) ? required : 0;
  internal->status.store(result);
  if (FAILED(result) || required == 0) cleanup(state);

  if (!state->block->callback) {
    signal(state);
    return;
  }
  {
    Lock lock(g_async_lock);
    ++state->refs;
  }
  auto* job = new CompletionJob{state};
  if (FAILED(enqueue(state->queue->ports[1], Entry{job, &completion_callback}))) {
    // The queue is gone or terminated: the routine still has to learn the
    // outcome, so it runs here.
    completion_callback(job, true);
  }
}

void CALLBACK do_work_callback(void* context, bool canceled) {
  auto* state = static_cast<AsyncState*>(context);
  bool completed;
  {
    Lock lock(state->lock);
    completed = state->completed;
  }
  if (!completed) {
    if (canceled) {
      complete(state, E_ABORT, 0);
    } else {
      HRESULT result;
      if (state->work) {
        result = state->work(state->block);
      } else {
        AsyncProviderData data{state->block, 0, nullptr, state->context};
        result = state->provider(AsyncOp::DoWork, &data);
      }
      if (result != E_PENDING) complete(state, result, 0);
    }
  }
  async_release(state);
}

// Identity used for calls started by XAsyncRun.
const char kRunIdentity = 0;

HRESULT CALLBACK run_provider(AsyncOp op, const AsyncProviderData* data) {
  if (op == AsyncOp::Begin) return async_schedule(data->async, 0);
  return S_OK;
}

thread_local bool t_time_sensitive = false;

}  // namespace

// --- task queue API -------------------------------------------------------------------

HRESULT task_queue_create(DispatchMode work, DispatchMode completion, void** queue) {
  if (!queue) return E_POINTER;
  *queue = nullptr;
  if (!valid_mode(work) || !valid_mode(completion)) return E_INVALIDARG;
  auto* state = new QueueState();
  state->ports[0] = new_port(work, Port::Work);
  state->ports[1] = new_port(completion, Port::Completion);
  *queue = state;
  LIB_LOG(holy::lib::kDebug, kComponent, "queue %p created (work mode %u, completion mode %u)", *queue,
          static_cast<unsigned>(work), static_cast<unsigned>(completion));
  return S_OK;
}

HRESULT task_queue_create_composite(void* work_port, void* completion_port, void** queue) {
  if (!queue) return E_POINTER;
  *queue = nullptr;
  PortState* work = as_port(work_port);
  PortState* completion = as_port(completion_port);
  if (!work || !completion) return E_INVALIDARG;
  auto* state = new QueueState();
  port_add_ref(work);
  port_add_ref(completion);
  state->ports[0] = work;
  state->ports[1] = completion;
  *queue = state;
  return S_OK;
}

HRESULT task_queue_get_port(void* queue, Port port, void** port_handle) {
  if (!port_handle) return E_POINTER;
  *port_handle = nullptr;
  if (!valid_port(port)) return E_INVALIDARG;
  QueueState* state = resolve_queue(queue);
  if (!state) return E_INVALIDARG;
  // Port handles stay valid while the queue that handed them out is open.
  *port_handle = state->ports[static_cast<uint32_t>(port)];
  queue_release(state);
  return S_OK;
}

HRESULT task_queue_duplicate_handle(void* queue, void** duplicate) {
  if (!duplicate) return E_POINTER;
  *duplicate = nullptr;
  QueueState* state = as_queue(queue);
  if (!state) return E_INVALIDARG;
  queue_add_ref(state);
  *duplicate = state;
  return S_OK;
}

bool task_queue_dispatch(void* queue, Port port, uint32_t timeout_ms) {
  if (!valid_port(port)) return false;
  QueueState* state = resolve_queue(queue);
  if (!state) return false;
  PortState* target = state->ports[static_cast<uint32_t>(port)];
  bool dispatched = false;
  if (target->mode == DispatchMode::Manual) {
    Entry entry{};
    bool canceled = false;
    {
      Lock lock(target->lock);
      const ULONGLONG start = GetTickCount64();
      while (target->ready.empty() && !target->terminated) {
        DWORD wait = INFINITE;
        if (timeout_ms != INFINITE) {
          const ULONGLONG elapsed = GetTickCount64() - start;
          if (elapsed >= timeout_ms) break;
          wait = static_cast<DWORD>(timeout_ms - elapsed);
        }
        if (!SleepConditionVariableSRW(&target->available, &target->lock, wait, 0) &&
            GetLastError() == ERROR_TIMEOUT) {
          break;
        }
      }
      if (!target->ready.empty()) {
        entry = target->ready.front();
        target->ready.pop_front();
        canceled = target->terminated;
        dispatched = true;
        ++target->in_flight;
      }
    }
    if (dispatched) {
      run_entry(entry, canceled);
      finish_in_flight(target);
    }
  }
  queue_release(state);
  return dispatched;
}

void task_queue_close_handle(void* queue) {
  QueueState* state = as_queue(queue);
  if (!state) return;
  {
    Lock lock(g_process_lock);
    // The process queue's own reference is not the caller's to drop.
    if (state == g_process_queue && state->refs.load() <= 1) return;
  }
  queue_release(state);
}

HRESULT task_queue_terminate(void* queue, bool wait, void* context, TerminatedCallback callback) {
  QueueState* state = resolve_queue(queue);
  if (!state) return E_INVALIDARG;
  {
    Lock lock(state->lock);
    state->terminated = true;
  }
  struct Job {
    QueueState* queue;
    void* context;
    TerminatedCallback callback;
  };
  auto finish = [](Job* job) {
    terminate_port(job->queue->ports[0]);
    terminate_port(job->queue->ports[1]);
    if (job->callback) job->callback(job->context);
    queue_release(job->queue);
    delete job;
  };
  auto* job = new Job{state, context, callback};
  if (wait) {
    finish(job);
    return S_OK;
  }
  if (!TrySubmitThreadpoolCallback(
          [](PTP_CALLBACK_INSTANCE, void* parameter) {
            auto* pending = static_cast<Job*>(parameter);
            terminate_port(pending->queue->ports[0]);
            terminate_port(pending->queue->ports[1]);
            if (pending->callback) pending->callback(pending->context);
            queue_release(pending->queue);
            delete pending;
          },
          job, nullptr)) {
    finish(job);
  }
  return S_OK;
}

HRESULT task_queue_submit_callback(void* queue, Port port, void* context, TaskQueueCallback callback) {
  return task_queue_submit_delayed_callback(queue, port, 0, context, callback);
}

HRESULT task_queue_submit_delayed_callback(void* queue, Port port, uint32_t delay_ms, void* context,
                                           TaskQueueCallback callback) {
  if (!callback || !valid_port(port)) return E_INVALIDARG;
  QueueState* state = resolve_queue(queue);
  if (!state) return E_INVALIDARG;
  bool terminated;
  {
    Lock lock(state->lock);
    terminated = state->terminated;
  }
  const HRESULT result =
      terminated ? E_ABORT : submit(state->ports[static_cast<uint32_t>(port)], delay_ms, Entry{context, callback});
  queue_release(state);
  return result;
}

HRESULT task_queue_register_waiter(void* queue, Port port, HANDLE wait_handle, void* context,
                                   TaskQueueCallback callback, RegistrationToken* token) {
  if (!token || !callback || !wait_handle || !valid_port(port)) return E_INVALIDARG;
  QueueState* state = as_queue(queue);
  if (!state) return E_INVALIDARG;
  auto* waiter = new Waiter{g_next_token.fetch_add(1), state->ports[static_cast<uint32_t>(port)],
                            Entry{context, callback}, nullptr};
  waiter->wait = CreateThreadpoolWait(&waiter_signalled, waiter, nullptr);
  if (!waiter->wait) {
    delete waiter;
    return E_OUTOFMEMORY;
  }
  port_add_ref(waiter->port);
  {
    Lock lock(state->lock);
    state->waiters.push_back(waiter);
  }
  // Assumed: one notification per registration; the registration stays until
  // it is unregistered.
  SetThreadpoolWait(waiter->wait, wait_handle, nullptr);
  token->token = waiter->token;
  return S_OK;
}

void task_queue_unregister_waiter(void* queue, RegistrationToken token) {
  QueueState* state = as_queue(queue);
  if (!state) return;
  Waiter* found = nullptr;
  {
    Lock lock(state->lock);
    for (auto it = state->waiters.begin(); it != state->waiters.end(); ++it) {
      if ((*it)->token == token.token) {
        found = *it;
        state->waiters.erase(it);
        break;
      }
    }
  }
  if (!found) return;
  SetThreadpoolWait(found->wait, nullptr, nullptr);
  WaitForThreadpoolWaitCallbacks(found->wait, TRUE);
  CloseThreadpoolWait(found->wait);
  port_release(found->port);
  delete found;
}

HRESULT task_queue_register_monitor(void* queue, void* context, MonitorCallback callback, RegistrationToken* token) {
  if (!token || !callback) return E_INVALIDARG;
  QueueState* state = as_queue(queue);
  if (!state) return E_INVALIDARG;
  const uint64_t value = g_next_token.fetch_add(1);
  for (PortState* port : state->ports) {
    Lock lock(port->lock);
    port->monitors.push_back(Monitor{value, state, context, callback});
  }
  token->token = value;
  return S_OK;
}

void task_queue_unregister_monitor(void* queue, RegistrationToken token) {
  QueueState* state = as_queue(queue);
  if (!state) return;
  for (PortState* port : state->ports) {
    Lock lock(port->lock);
    for (auto it = port->monitors.begin(); it != port->monitors.end(); ++it) {
      if (it->token == token.token) {
        port->monitors.erase(it);
        break;
      }
    }
  }
}

bool task_queue_get_current_process_task_queue(void** queue) {
  if (!queue) return false;
  QueueState* state = process_queue_reference();
  *queue = state;
  return state != nullptr;
}

void task_queue_set_current_process_task_queue(void* queue) {
  QueueState* state = as_queue(queue);
  if (state) queue_add_ref(state);
  QueueState* previous;
  {
    Lock lock(g_process_lock);
    previous = g_process_queue;
    g_process_queue = state;
    g_process_queue_set = true;
  }
  if (previous) queue_release(previous);
}

// --- asynchronous calls --------------------------------------------------------------

namespace {

HRESULT begin(AsyncBlock* block, void* context, const void* identity, const char* identity_name,
              AsyncProvider provider, AsyncWork work) {
  if (!block || !provider) return E_INVALIDARG;
  QueueState* queue = resolve_queue(block->queue);
  if (!queue) return E_INVALIDARG;

  AsyncState* state = allocate_state();
  state->block = block;
  state->provider = provider;
  state->context = context;
  state->identity_name = identity_name;
  state->begun_ms = GetTickCount64();
  state->work = work;
  state->queue = queue;
  state->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!state->done) {
    async_release(state);
    return E_OUTOFMEMORY;
  }

  BlockInternal* internal = internal_of(block);
  std::memset(block->internal, 0, sizeof(block->internal));
  internal->status.store(E_PENDING);
  internal->identity = identity;
  internal->required = 0;
  internal->slot = state->slot;
  internal->generation = state->generation;
  internal->signature = kAsyncSignature;

  AsyncProviderData data{block, 0, nullptr, context};
  const HRESULT result = provider(AsyncOp::Begin, &data);
  if (FAILED(result)) {
    // The call never started: no completion routine, but the provider is
    // still cleaned up and the block reports the failure.
    {
      Lock lock(state->lock);
      state->completed = true;
    }
    internal->status.store(result);
    cleanup(state);
    signal(state);
    return result;
  }
  return S_OK;
}

}  // namespace

HRESULT async_begin(AsyncBlock* block, void* context, const void* identity, const char* identity_name,
                    AsyncProvider provider) {
  return begin(block, context, identity, identity_name, provider, nullptr);
}

HRESULT async_run(AsyncBlock* block, AsyncWork work) {
  if (!block || !work) return E_INVALIDARG;
  return begin(block, nullptr, &kRunIdentity, "XAsyncRun", &run_provider, work);
}

HRESULT async_schedule(AsyncBlock* block, uint32_t delay_ms) {
  if (!block) return E_INVALIDARG;
  AsyncState* state = acquire_state(block);
  if (!state) return E_INVALIDARG;
  const HRESULT result = submit(state->queue->ports[0], delay_ms, Entry{state, &do_work_callback});
  if (FAILED(result)) async_release(state);
  return result;
}

void async_complete(AsyncBlock* block, HRESULT result, size_t required_size) {
  if (!block) return;
  AsyncState* state = acquire_state(block);
  if (!state) return;
  complete(state, result, required_size);
  async_release(state);
}

HRESULT async_get_status(AsyncBlock* block, bool wait) {
  if (!block) return E_INVALIDARG;
  BlockInternal* internal = internal_of(block);
  if (internal->signature != kAsyncSignature) return E_INVALIDARG;
  if (wait) {
    if (AsyncState* state = acquire_state(block)) {
      WaitForSingleObject(state->done, INFINITE);
      async_release(state);
    }
  }
  return internal->status.load();
}

HRESULT async_get_result_size(AsyncBlock* block, size_t* size) {
  if (!block || !size) return E_INVALIDARG;
  *size = 0;
  BlockInternal* internal = internal_of(block);
  if (internal->signature != kAsyncSignature) return E_INVALIDARG;
  const HRESULT status = internal->status.load();
  if (FAILED(status) || status == E_PENDING) return status;
  *size = internal->required;
  return S_OK;
}

void async_cancel(AsyncBlock* block) {
  if (!block) return;
  AsyncState* state = acquire_state(block);
  if (!state) return;
  bool completed;
  {
    Lock lock(state->lock);
    completed = state->completed;
  }
  if (!completed) {
    if (state->provider) {
      AsyncProviderData data{block, 0, nullptr, state->context};
      state->provider(AsyncOp::Cancel, &data);
    }
    complete(state, E_ABORT, 0);
  }
  async_release(state);
}

HRESULT async_get_result(AsyncBlock* block, const void* identity, size_t buffer_size, void* buffer,
                         size_t* buffer_used) {
  if (buffer_used) *buffer_used = 0;
  if (!block) return E_INVALIDARG;
  BlockInternal* internal = internal_of(block);
  if (internal->signature != kAsyncSignature) return E_INVALIDARG;
  if (identity && internal->identity != identity) return E_INVALIDARG;
  const HRESULT status = internal->status.load();
  if (status == E_PENDING || FAILED(status)) return status;
  const size_t required = internal->required;
  if (required == 0) return status;
  if (buffer_size < required || !buffer) return kErrorNotSufficientBuffer;

  AsyncState* state = acquire_state(block);
  if (!state) return kErrorIllegalMethodCall;  // result already retrieved
  AsyncProviderData data{block, buffer_size, buffer, state->context};
  HRESULT result = S_OK;
  if (state->provider) result = state->provider(AsyncOp::GetResult, &data);
  if (SUCCEEDED(result) && buffer_used) *buffer_used = required;
  internal->required = 0;
  cleanup(state);
  async_release(state);
  return result;
}

// --- time-sensitive threads -----------------------------------------------------

bool thread_is_time_sensitive() {
  return t_time_sensitive;
}

HRESULT thread_set_time_sensitive(bool value) {
  t_time_sensitive = value;
  return S_OK;
}

void thread_assert_not_time_sensitive() {
  if (t_time_sensitive) {
    LIB_LOG(holy::lib::kWarn, "xthread", "a blocking runtime function was called on a time-sensitive thread");
  }
}

}  // namespace holy::xgr
